/* audio_wave.c -- see audio_wave.h.  waveOut/midiOut bound at runtime. */
#include "audio_wave.h"
#include "audio_rec.h"    /* the ONE includer -- see its header */
#include "midi_route.h"   /* #136: which midiOut device the MIDI setting means */
#include "vdd_mpu.h"      /* #136: MPU_SYSEX_MAX, the longest SysEx we pass on */
#define COBJMACROS
#include <dsound.h>       /* #234: interfaces only -- dsound.dll is bound at runtime */

/* Bits of mmsystem we need, declared here rather than pulling in <mmsystem.h>:
   this file is compiled freestanding and only ever calls through the pointers
   below, so the ABI is all that matters. */
#define WAVE_FORMAT_PCM   1
#define WAVE_MAPPER       ((UINT)-1)
#ifndef CALLBACK_EVENT
#define CALLBACK_EVENT    0x00050000
#endif
#define WHDR_DONE         0x00000001
#define WHDR_PREPARED     0x00000002

typedef struct { WORD wFormatTag, nChannels; DWORD nSamplesPerSec, nAvgBytesPerSec;
                 WORD nBlockAlign, wBitsPerSample, cbSize; } AW_WAVEFORMATEX;
typedef struct { LPSTR lpData; DWORD dwBufferLength, dwBytesRecorded;
                 DWORD_PTR dwUser; DWORD dwFlags, dwLoops;
                 void *lpNext; DWORD_PTR reserved; } AW_WAVEHDR;

typedef UINT (WINAPI *PFN_waveOutOpen)(void **, UINT, const AW_WAVEFORMATEX *,
                                       DWORD_PTR, DWORD_PTR, DWORD);
typedef UINT (WINAPI *PFN_waveOutHdr)(void *, AW_WAVEHDR *, UINT);
typedef UINT (WINAPI *PFN_waveOutOne)(void *);
typedef UINT (WINAPI *PFN_waveOutVol)(void *, DWORD *);
typedef UINT (WINAPI *PFN_midiOutOpen)(void **, UINT, DWORD_PTR, DWORD_PTR, DWORD);
typedef UINT (WINAPI *PFN_midiOutShort)(void *, DWORD);
typedef UINT (WINAPI *PFN_midiOutClose)(void *);
/* #136: device names (MIDIOUTCAPSA) and SysEx (MIDIHDR + the long-message calls). */
typedef struct { WORD wMid, wPid; UINT vDriverVersion; CHAR szPname[32];
                 WORD wTechnology, wVoices, wNotes, wChannelMask; DWORD dwSupport; } AW_MIDIOUTCAPSA;
typedef struct { LPSTR lpData; DWORD dwBufferLength, dwBytesRecorded; DWORD_PTR dwUser;
                 DWORD dwFlags; void *lpNext; DWORD_PTR reserved; DWORD dwOffset;
                 DWORD_PTR dwReserved[8]; } AW_MIDIHDR;
#define AW_MHDR_DONE      0x00000001
#define AW_MHDR_PREPARED  0x00000002
typedef UINT (WINAPI *PFN_midiOutGetNumDevs)(void);
typedef UINT (WINAPI *PFN_midiOutGetDevCapsA)(UINT_PTR, AW_MIDIOUTCAPSA *, UINT);
typedef UINT (WINAPI *PFN_midiOutHdr)(void *, AW_MIDIHDR *, UINT);

static PFN_waveOutOpen   p_waveOutOpen;
static PFN_waveOutHdr    p_waveOutPrepare, p_waveOutUnprepare, p_waveOutWrite;
static PFN_waveOutOne    p_waveOutReset, p_waveOutClose;
static PFN_waveOutVol    p_waveOutGetVolume;
static PFN_midiOutOpen   p_midiOutOpen;
static PFN_midiOutShort  p_midiOutShortMsg;
static PFN_midiOutClose  p_midiOutClose;
static PFN_midiOutClose  p_midiOutReset;     /* same shape: UINT (HMIDIOUT) */
static PFN_midiOutGetNumDevs  p_midiOutGetNumDevs;     /* #136 */
static PFN_midiOutGetDevCapsA p_midiOutGetDevCapsA;
static PFN_midiOutHdr    p_midiOutPrepare, p_midiOutUnprepare, p_midiOutLongMsg;

/* #136: SysEx slots. midiOutLongMsg is ASYNCHRONOUS -- the driver owns the buffer until
   it sets MHDR_DONE -- so each message is copied into a slot that stays put until then. */
#define AW_SYSEX_SLOTS 8
static AW_MIDIHDR s_sxhdr[AW_SYSEX_SLOTS];
static char       s_sxbuf[AW_SYSEX_SLOTS][MPU_SYSEX_MAX];
static unsigned   s_sxnext;

static AW_WAVEHDR *hdr_of(audio_wave *aw, int i)
{ return (AW_WAVEHDR *)aw->hdr[i]; }

static DWORD WINAPI aw_thread(LPVOID pv)
{
    audio_wave *aw = (audio_wave *)pv;
    uint32_t i;                          /* matches aw->nbufs; was int (sign-compare) */

    /* THE EXEC THREAD RUNS FLAT OUT. It is a guest burning 100% of a core, and at normal
       priority this thread loses the race often enough that buffers run dry -- which is
       heard as a periodic tick or pulse in otherwise correct music, exactly as reported.
       Audio refill is a hard real-time deadline (every ~11.6 ms per buffer) doing almost no
       work, so it belongs above the guest. TIME_CRITICAL is what waveOut feeders normally
       use; fall back gracefully if the system refuses it. */
    if (!SetThreadPriority(GetCurrentThread(), 15 /* THREAD_PRIORITY_TIME_CRITICAL */))
        SetThreadPriority(GetCurrentThread(), 2 /* THREAD_PRIORITY_HIGHEST */);

    /* Prime every buffer, then top them up as the driver returns them. */
    for (i = 0; i < aw->nbufs && !aw->silent; ++i) {
        AW_WAVEHDR *h = hdr_of(aw, i);
        h->lpData = (LPSTR)aw->buf[i];
        h->dwBufferLength = aw->nframes * AW_CHANNELS * sizeof(int16_t);
        h->dwFlags = 0; h->dwLoops = 0; h->dwUser = 0;
        p_waveOutPrepare(aw->hwo, h, sizeof(AW_WAVEHDR));
        aw->fill(aw->ctx, aw->buf[i], aw->nframes);
        audio_rec_feed(aw->buf[i], aw->nframes * AW_CHANNELS);
        p_waveOutWrite(aw->hwo, h, sizeof(AW_WAVEHDR));
    }

    while (aw->running) {
        if (aw->silent) {
            /* No device: still pump the mixer at real-time pace, because that is
               what advances SB playback and raises its IRQ. */
            aw->fill(aw->ctx, aw->buf[0], aw->nframes);
            audio_rec_feed(aw->buf[0], aw->nframes * AW_CHANNELS);
            Sleep((aw->nframes * 1000) / (aw->hz ? aw->hz : 44100));
            continue;
        }
        /* How much of the queue has the driver already given back? Sampled BEFORE we
           refill anything, so it is the true low-water mark of this pass. See the note
           on `starved` in audio_wave.h: waveOutWrite succeeding tells us nothing. */
        { uint32_t back = 0;
          for (i = 0; i < aw->nbufs; ++i)
              if (hdr_of(aw, i)->dwFlags & WHDR_DONE) ++back;
          if (back > aw->drain_max) aw->drain_max = back;
          if (back >= aw->nbufs) ++aw->starved;
          aw->drain_hist[back <= AW_BUFFERS ? back : AW_BUFFERS]++; }

        for (i = 0; i < aw->nbufs; ++i) {
            AW_WAVEHDR *h = hdr_of(aw, i);
            if (!(h->dwFlags & WHDR_DONE)) continue;
            h->dwFlags &= ~WHDR_DONE;
            aw->fill(aw->ctx, aw->buf[i], aw->nframes);
        audio_rec_feed(aw->buf[i], aw->nframes * AW_CHANNELS);
            if (p_waveOutWrite(aw->hwo, h, sizeof(AW_WAVEHDR)) != 0) aw->underruns++;
        }
        /* Wake early and often: a full buffer is ~11.6 ms, so a 20 ms timeout could miss a
           whole buffer's deadline if the completion event is late. */
        WaitForSingleObject(aw->event, 5);
    }

    if (!aw->silent) {
        p_waveOutReset(aw->hwo);
        for (i = 0; i < aw->nbufs; ++i)
            p_waveOutUnprepare(aw->hwo, hdr_of(aw, i), sizeof(AW_WAVEHDR));
    }
    return 0;
}

/* ── #234: THE DIRECTSOUND OUTPUT. One looping secondary buffer holding the same
     lead as the waveOut queue (nbufs x nframes), filled ahead of the play cursor a
     chunk at a time from the same mixer callback, and fed to the same recorder. The
     cursor tells us how far ahead we are, so starvation is MEASURED rather than
     inferred: `starved` counts passes where the play cursor had caught up with us.
     GLOBALFOCUS so it keeps playing when the window is not in front (the pause in
     #219 is what silences a background window, not the output). */
typedef HRESULT (WINAPI *PFN_DirectSoundCreate)(LPCGUID, LPDIRECTSOUND *, LPUNKNOWN);

static int aw_ds_open(audio_wave *aw, const AW_WAVEFORMATEX *fmt)
{
    HMODULE m = LoadLibraryA("dsound.dll");
    PFN_DirectSoundCreate create;
    LPDIRECTSOUND ds = NULL; LPDIRECTSOUNDBUFFER b = NULL;
    DSBUFFERDESC d; WAVEFORMATEX wf;
    void *p1, *p2; DWORD n1, n2;
    if (!m) return 0;
    create = (PFN_DirectSoundCreate)GetProcAddress(m, "DirectSoundCreate");
    if (!create || FAILED(create(NULL, &ds, NULL))) return 0;
    if (FAILED(IDirectSound_SetCooperativeLevel(ds, GetDesktopWindow(), DSSCL_NORMAL))) {
        IDirectSound_Release(ds); return 0;
    }
    ZeroMemory(&wf, sizeof wf);
    wf.wFormatTag = WAVE_FORMAT_PCM; wf.nChannels = fmt->nChannels;
    wf.nSamplesPerSec = fmt->nSamplesPerSec; wf.wBitsPerSample = fmt->wBitsPerSample;
    wf.nBlockAlign = fmt->nBlockAlign; wf.nAvgBytesPerSec = fmt->nAvgBytesPerSec;
    ZeroMemory(&d, sizeof d); d.dwSize = sizeof d;
    d.dwFlags = DSBCAPS_GLOBALFOCUS | DSBCAPS_GETCURRENTPOSITION2;
    aw->ds_bytes = aw->nbufs * aw->nframes * AW_CHANNELS * (uint32_t)sizeof(int16_t);
    d.dwBufferBytes = aw->ds_bytes; d.lpwfxFormat = &wf;
    if (FAILED(IDirectSound_CreateSoundBuffer(ds, &d, &b, NULL))) {
        IDirectSound_Release(ds); return 0;
    }
    /* Start from silence; the thread fills ahead of the cursor from here. */
    if (SUCCEEDED(IDirectSoundBuffer_Lock(b, 0, aw->ds_bytes, &p1, &n1, &p2, &n2, 0))) {
        ZeroMemory(p1, n1); if (p2) ZeroMemory(p2, n2);
        IDirectSoundBuffer_Unlock(b, p1, n1, p2, n2);
    }
    aw->ds = ds; aw->dsb = b; aw->ds_wpos = 0;
    if (FAILED(IDirectSoundBuffer_Play(b, 0, 0, DSBPLAY_LOOPING))) {
        IDirectSoundBuffer_Release(b); IDirectSound_Release(ds);
        aw->ds = aw->dsb = NULL; return 0;
    }
    return 1;
}

static DWORD WINAPI aw_ds_thread(LPVOID pv)
{
    audio_wave *aw = (audio_wave *)pv;
    LPDIRECTSOUNDBUFFER b = (LPDIRECTSOUNDBUFFER)aw->dsb;
    uint32_t chunk = aw->nframes * AW_CHANNELS * (uint32_t)sizeof(int16_t);
    if (!SetThreadPriority(GetCurrentThread(), 15 /* THREAD_PRIORITY_TIME_CRITICAL */))
        SetThreadPriority(GetCurrentThread(), 2 /* THREAD_PRIORITY_HIGHEST */);
    while (aw->running) {
        DWORD play = 0, wr = 0;
        uint32_t ahead, space;
        if (FAILED(IDirectSoundBuffer_GetCurrentPosition(b, &play, &wr))) { Sleep(5); continue; }
        ahead = (aw->ds_wpos + aw->ds_bytes - play) % aw->ds_bytes;   /* queued, not played */
        space = aw->ds_bytes - ahead;
        if (ahead < chunk) ++aw->starved;               /* the cursor caught up with us */
        {   uint32_t queued_bufs = ahead / chunk, back = aw->nbufs - (queued_bufs < aw->nbufs ? queued_bufs : aw->nbufs);
            if (back > aw->drain_max) aw->drain_max = back;
            aw->drain_hist[back <= AW_BUFFERS ? back : AW_BUFFERS]++; }
        while (space >= chunk) {
            void *p1, *p2; DWORD n1, n2;
            aw->fill(aw->ctx, aw->buf[0], aw->nframes);
            audio_rec_feed(aw->buf[0], aw->nframes * AW_CHANNELS);
            if (SUCCEEDED(IDirectSoundBuffer_Lock(b, aw->ds_wpos, chunk, &p1, &n1, &p2, &n2, 0))) {
                CopyMemory(p1, aw->buf[0], n1);
                if (p2) CopyMemory(p2, (BYTE *)aw->buf[0] + n1, n2);
                IDirectSoundBuffer_Unlock(b, p1, n1, p2, n2);
            } else aw->underruns++;
            aw->ds_wpos = (aw->ds_wpos + chunk) % aw->ds_bytes;
            space -= chunk;
        }
        Sleep(2);
    }
    IDirectSoundBuffer_Stop(b);
    return 0;
}

static int aw_bind(audio_wave *aw)
{
    aw->mod = LoadLibraryA("winmm.dll");
    if (!aw->mod) return 0;
    p_waveOutOpen       = (PFN_waveOutOpen) GetProcAddress(aw->mod, "waveOutOpen");
    p_waveOutPrepare    = (PFN_waveOutHdr)  GetProcAddress(aw->mod, "waveOutPrepareHeader");
    p_waveOutUnprepare  = (PFN_waveOutHdr)  GetProcAddress(aw->mod, "waveOutUnprepareHeader");
    p_waveOutWrite      = (PFN_waveOutHdr)  GetProcAddress(aw->mod, "waveOutWrite");
    p_waveOutGetVolume  = (PFN_waveOutVol)  GetProcAddress(aw->mod, "waveOutGetVolume");
    p_waveOutReset      = (PFN_waveOutOne)  GetProcAddress(aw->mod, "waveOutReset");
    p_waveOutClose      = (PFN_waveOutOne)  GetProcAddress(aw->mod, "waveOutClose");
    p_midiOutOpen       = (PFN_midiOutOpen) GetProcAddress(aw->mod, "midiOutOpen");
    p_midiOutShortMsg   = (PFN_midiOutShort)GetProcAddress(aw->mod, "midiOutShortMsg");
    p_midiOutClose      = (PFN_midiOutClose)GetProcAddress(aw->mod, "midiOutClose");
    p_midiOutReset      = (PFN_midiOutClose)GetProcAddress(aw->mod, "midiOutReset");
    p_midiOutGetNumDevs = (PFN_midiOutGetNumDevs) GetProcAddress(aw->mod, "midiOutGetNumDevs");
    p_midiOutGetDevCapsA= (PFN_midiOutGetDevCapsA)GetProcAddress(aw->mod, "midiOutGetDevCapsA");
    p_midiOutPrepare    = (PFN_midiOutHdr)  GetProcAddress(aw->mod, "midiOutPrepareHeader");
    p_midiOutUnprepare  = (PFN_midiOutHdr)  GetProcAddress(aw->mod, "midiOutUnprepareHeader");
    p_midiOutLongMsg    = (PFN_midiOutHdr)  GetProcAddress(aw->mod, "midiOutLongMsg");
    return p_waveOutOpen && p_waveOutPrepare && p_waveOutWrite &&
           p_waveOutReset && p_waveOutClose;
}

/* ── #136: OPEN THE MIDI DEVICE THE SETTING NAMES. ─────────────────────────────────
     Host GM (the default) is the old call, unchanged: device 0, no enumeration. Any
     other choice enumerates the devices and opens the one midi_route_pick finds by name;
     none found -> device 0 anyway, with midi_ext = 0 so it is treated as the GM synth
     it is (no SysEx), and the host's log line says the choice was not met. */
static void aw_midi_open(audio_wave *aw)
{
    UINT dev = 0;
    if (!p_midiOutOpen) return;
    if (aw->midi_choice != MIDI_ROUTE_GM && p_midiOutGetNumDevs && p_midiOutGetDevCapsA) {
        static char names[16][32];
        const char *np[16];
        UINT n = p_midiOutGetNumDevs(), i;
        int pick, k;
        aw->midi_ndevs = n;
        if (n > 16) n = 16;
        for (i = 0; i < n; ++i) {
            AW_MIDIOUTCAPSA caps;
            names[i][0] = 0;
            if (p_midiOutGetDevCapsA(i, &caps, sizeof caps) == 0) {
                for (k = 0; k < 31 && caps.szPname[k]; ++k) names[i][k] = caps.szPname[k];
                names[i][k] = 0;
            }
            np[i] = names[i];
        }
        pick = midi_route_pick(aw->midi_choice, np, (int)n);
        if (pick >= 0) { dev = (UINT)pick; aw->midi_ext = 1; }
        if (dev < n) {
            for (k = 0; k < 31 && names[dev][k]; ++k) aw->midi_name[k] = names[dev][k];
            aw->midi_name[k] = 0;
        }
    }
    if (p_midiOutOpen(&aw->hmidi, dev, 0, 0, 0) == 0) aw->midi_dev = (int)dev;
    else { aw->hmidi = 0; aw->midi_ext = 0; }
}

int audio_wave_start(audio_wave *aw, uint32_t hz, aw_fill_fn fill, void *ctx)
{
    AW_WAVEFORMATEX fmt;
    unsigned i; unsigned char *p = (unsigned char *)aw;
    /* ► PRESERVE THE LEAD ACROSS THE ZEROING. The caller sets aw->nbufs from awbufs.txt
         BEFORE calling us, and this function wipes the whole struct -- so read it back
         out first, exactly as VddSbReset() preserves its bus pointers. Getting this
         wrong would silently pin the experiment at one value while appearing to vary it,
         which is the failure mode this counter exists to avoid. */
    uint32_t want_bufs = aw->nbufs, want_frames = aw->nframes;
    int want_ds = aw->want_ds;                  /* #234: preserved like the lead */
    int force_silent = aw->force_silent;        /* #132: ditto -- the rig caught it wiped */
    int midi_choice = aw->midi_choice;          /* #136: ditto */
    for (i = 0; i < sizeof(*aw); ++i) p[i] = 0;
    if (!want_bufs)   want_bufs   = AW_DEF_BUFFERS;     /* 0 = "leave it alone"  */
    if (want_bufs < 2) want_bufs = 2;
    if (want_bufs > AW_BUFFERS) want_bufs = AW_BUFFERS;
    if (!want_frames) want_frames = AW_DEF_FRAMES;
    if (want_frames < AW_MIN_FRAMES) want_frames = AW_MIN_FRAMES;
    if (want_frames > AW_FRAMES)     want_frames = AW_FRAMES;
    aw->nbufs   = want_bufs;
    aw->nframes = want_frames;
    aw->want_ds = want_ds;
    aw->force_silent = force_silent;
    aw->midi_choice = midi_choice;
    aw->midi_dev = -1;

    aw->hz = hz ? hz : 44100;
    aw->fill = fill; aw->ctx = ctx;
    aw->silent = 1;                             /* until a device opens           */

    if (!aw->force_silent && aw_bind(aw)) {
        fmt.wFormatTag = WAVE_FORMAT_PCM;
        fmt.nChannels = AW_CHANNELS;          /* #189 */
        fmt.nSamplesPerSec = aw->hz;
        fmt.wBitsPerSample = 16;
        fmt.nBlockAlign = (WORD)(fmt.nChannels * fmt.wBitsPerSample / 8);
        fmt.nAvgBytesPerSec = fmt.nSamplesPerSec * fmt.nBlockAlign;
        fmt.cbSize = 0;
        aw->event = CreateEventA(NULL, FALSE, FALSE, NULL);
        if (aw->want_ds && aw_ds_open(aw, &fmt)) {     /* #234: DirectSound first...   */
            aw->using_ds = 1; aw->silent = 0;
        } else if (p_waveOutOpen(&aw->hwo, WAVE_MAPPER, &fmt,  /* ...WinMM otherwise */
                          (DWORD_PTR)aw->event, 0, CALLBACK_EVENT) == 0)
            aw->silent = 0;
        /* ⚠ ASK THE DRIVER WHAT ITS VOLUME IS. Every other counter here can read
             perfect while the machine is silent, because Windows' own WAVE slider
             attenuates after us. Read, never written -- see audio_wave.h. */
        if (!aw->silent && !aw->using_ds && p_waveOutGetVolume) {
            DWORD v = 0;
            if (p_waveOutGetVolume(aw->hwo, &v) == 0) {
                aw->dev_volume = (uint32_t)v; aw->dev_volume_ok = 1;
            }
        }
        /* MIDI is optional and independent: XP's GS Wavetable synth is device 0. */
        aw_midi_open(aw);                   /* #136: or the device the setting names */
    }

    aw->running = 1;
    aw->thread = CreateThread(NULL, 0, aw->using_ds ? aw_ds_thread : aw_thread, aw, 0, NULL);
    if (!aw->thread) { aw->running = 0; return 1; }
    return aw->silent ? 1 : 0;
}

void audio_wave_stop(audio_wave *aw)
{
    if (!aw->running) return;
    InterlockedExchange(&aw->running, 0);
    if (aw->event) SetEvent(aw->event);
    if (aw->thread) { WaitForSingleObject(aw->thread, 500); CloseHandle(aw->thread); }
    if (!aw->silent && aw->hwo && p_waveOutClose) p_waveOutClose(aw->hwo);
    if (aw->dsb) { IDirectSoundBuffer_Release((LPDIRECTSOUNDBUFFER)aw->dsb); aw->dsb = NULL; }
    if (aw->ds)  { IDirectSound_Release((LPDIRECTSOUND)aw->ds); aw->ds = NULL; }
    audio_wave_midi_silence(aw);          /* #214: a held note must not outlive us */
    /* #136: midiOutReset (in the silence above) hands every queued SysEx buffer back,
       and a prepared header must be unprepared before its device closes. */
    {   unsigned i;
        for (i = 0; i < AW_SYSEX_SLOTS; ++i)
            if ((s_sxhdr[i].dwFlags & AW_MHDR_PREPARED) && aw->hmidi && p_midiOutUnprepare)
                p_midiOutUnprepare(aw->hmidi, &s_sxhdr[i], sizeof s_sxhdr[i]);
    }
    if (aw->hmidi && p_midiOutClose) p_midiOutClose(aw->hmidi);
    if (aw->event) CloseHandle(aw->event);
    if (aw->mod) FreeLibrary(aw->mod);
    aw->thread = 0; aw->event = 0; aw->hwo = 0; aw->hmidi = 0; aw->mod = 0;
}

void audio_wave_midi(audio_wave *aw, uint32_t msg)
{
    if (aw->hmidi && p_midiOutShortMsg) p_midiOutShortMsg(aw->hmidi, msg);
}

void audio_wave_midi_long(audio_wave *aw, const uint8_t *msg, uint32_t len)
{
    unsigned t, i;
    AW_MIDIHDR *h;
    if (!aw->hmidi || !p_midiOutLongMsg || !p_midiOutPrepare || !p_midiOutUnprepare) return;
    if (!len || len > MPU_SYSEX_MAX) { aw->sysex_dropped++; return; }
    for (t = 0; t < AW_SYSEX_SLOTS; ++t) {
        i = (s_sxnext + t) % AW_SYSEX_SLOTS;
        h = &s_sxhdr[i];
        if (h->dwFlags & AW_MHDR_PREPARED) {
            if (!(h->dwFlags & AW_MHDR_DONE)) continue;           /* the driver still has it */
            p_midiOutUnprepare(aw->hmidi, h, sizeof *h);
        }
        {   uint32_t k;
            for (k = 0; k < len; ++k) s_sxbuf[i][k] = (char)msg[k]; }
        ZeroMemory(h, sizeof *h);
        h->lpData = s_sxbuf[i]; h->dwBufferLength = len; h->dwBytesRecorded = len;
        if (p_midiOutPrepare(aw->hmidi, h, sizeof *h) != 0) { aw->sysex_dropped++; return; }
        if (p_midiOutLongMsg(aw->hmidi, h, sizeof *h) != 0) {
            p_midiOutUnprepare(aw->hmidi, h, sizeof *h);
            aw->sysex_dropped++; return;
        }
        s_sxnext = (i + 1) % AW_SYSEX_SLOTS;
        aw->sysex_sent++;
        return;
    }
    aw->sysex_dropped++;                                         /* every slot in flight */
}

/* #214: EVERY NOTE OFF, ON EVERY CHANNEL. The emulated MPU-401 can be reset, but the
   notes it already sent live in XP's synth, which knows nothing about a program ending:
   Close Program on Doom left them sounding. Per channel: sustain pedal up (a held pedal
   outlives a note-off), All Sound Off (120), All Notes Off (123), Reset All Controllers
   (121, so pitch bend and modulation do not carry into the next program); then
   midiOutReset, which is winmm's own "turn off all notes". Program changes are left
   alone -- every program sets its own. */
void audio_wave_midi_silence(audio_wave *aw)
{
    uint32_t ch;
    if (!aw->hmidi || !p_midiOutShortMsg) return;
    for (ch = 0; ch < 16; ++ch) {
        p_midiOutShortMsg(aw->hmidi, 0xB0u | ch | (64u  << 8));             /* sustain 0 */
        p_midiOutShortMsg(aw->hmidi, 0xB0u | ch | (120u << 8));             /* sound off */
        p_midiOutShortMsg(aw->hmidi, 0xB0u | ch | (123u << 8));             /* notes off */
        p_midiOutShortMsg(aw->hmidi, 0xB0u | ch | (121u << 8));             /* reset CCs */
    }
    if (p_midiOutReset) p_midiOutReset(aw->hmidi);
}

/* Recording what we play -- see audio_rec.h. Exported so the host can drive it. */
int      aw_rec_start(const char *path, uint32_t hz) { return audio_rec_start(path, hz); }
uint32_t aw_rec_stop(void)    { return audio_rec_stop(); }
int      aw_rec_active(void)  { return audio_rec_active(); }
uint32_t aw_rec_dropped(void) { return g_arec.dropped; }
