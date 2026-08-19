/* audio_wave.c -- see audio_wave.h.  waveOut/midiOut bound at runtime. */
#include "audio_wave.h"

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
typedef UINT (WINAPI *PFN_midiOutOpen)(void **, UINT, DWORD_PTR, DWORD_PTR, DWORD);
typedef UINT (WINAPI *PFN_midiOutShort)(void *, DWORD);
typedef UINT (WINAPI *PFN_midiOutClose)(void *);

static PFN_waveOutOpen   p_waveOutOpen;
static PFN_waveOutHdr    p_waveOutPrepare, p_waveOutUnprepare, p_waveOutWrite;
static PFN_waveOutOne    p_waveOutReset, p_waveOutClose;
static PFN_midiOutOpen   p_midiOutOpen;
static PFN_midiOutShort  p_midiOutShortMsg;
static PFN_midiOutClose  p_midiOutClose;

static AW_WAVEHDR *hdr_of(audio_wave *aw, int i)
{ return (AW_WAVEHDR *)aw->hdr[i]; }

static DWORD WINAPI aw_thread(LPVOID pv)
{
    audio_wave *aw = (audio_wave *)pv;
    int i;

    /* Prime every buffer, then top them up as the driver returns them. */
    for (i = 0; i < AW_BUFFERS && !aw->silent; ++i) {
        AW_WAVEHDR *h = hdr_of(aw, i);
        h->lpData = (LPSTR)aw->buf[i];
        h->dwBufferLength = AW_FRAMES * sizeof(int16_t);
        h->dwFlags = 0; h->dwLoops = 0; h->dwUser = 0;
        p_waveOutPrepare(aw->hwo, h, sizeof(AW_WAVEHDR));
        aw->fill(aw->ctx, aw->buf[i], AW_FRAMES);
        p_waveOutWrite(aw->hwo, h, sizeof(AW_WAVEHDR));
    }

    while (aw->running) {
        if (aw->silent) {
            /* No device: still pump the mixer at real-time pace, because that is
               what advances SB playback and raises its IRQ. */
            aw->fill(aw->ctx, aw->buf[0], AW_FRAMES);
            Sleep((AW_FRAMES * 1000) / (aw->hz ? aw->hz : 44100));
            continue;
        }
        for (i = 0; i < AW_BUFFERS; ++i) {
            AW_WAVEHDR *h = hdr_of(aw, i);
            if (!(h->dwFlags & WHDR_DONE)) continue;
            h->dwFlags &= ~WHDR_DONE;
            aw->fill(aw->ctx, aw->buf[i], AW_FRAMES);
            if (p_waveOutWrite(aw->hwo, h, sizeof(AW_WAVEHDR)) != 0) aw->underruns++;
        }
        WaitForSingleObject(aw->event, 20);
    }

    if (!aw->silent) {
        p_waveOutReset(aw->hwo);
        for (i = 0; i < AW_BUFFERS; ++i)
            p_waveOutUnprepare(aw->hwo, hdr_of(aw, i), sizeof(AW_WAVEHDR));
    }
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
    p_waveOutReset      = (PFN_waveOutOne)  GetProcAddress(aw->mod, "waveOutReset");
    p_waveOutClose      = (PFN_waveOutOne)  GetProcAddress(aw->mod, "waveOutClose");
    p_midiOutOpen       = (PFN_midiOutOpen) GetProcAddress(aw->mod, "midiOutOpen");
    p_midiOutShortMsg   = (PFN_midiOutShort)GetProcAddress(aw->mod, "midiOutShortMsg");
    p_midiOutClose      = (PFN_midiOutClose)GetProcAddress(aw->mod, "midiOutClose");
    return p_waveOutOpen && p_waveOutPrepare && p_waveOutWrite &&
           p_waveOutReset && p_waveOutClose;
}

int audio_wave_start(audio_wave *aw, uint32_t hz, aw_fill_fn fill, void *ctx)
{
    AW_WAVEFORMATEX fmt;
    unsigned i; unsigned char *p = (unsigned char *)aw;
    for (i = 0; i < sizeof(*aw); ++i) p[i] = 0;

    aw->hz = hz ? hz : 44100;
    aw->fill = fill; aw->ctx = ctx;
    aw->silent = 1;                             /* until a device opens           */

    if (aw_bind(aw)) {
        fmt.wFormatTag = WAVE_FORMAT_PCM;
        fmt.nChannels = 1;
        fmt.nSamplesPerSec = aw->hz;
        fmt.wBitsPerSample = 16;
        fmt.nBlockAlign = (WORD)(fmt.nChannels * fmt.wBitsPerSample / 8);
        fmt.nAvgBytesPerSec = fmt.nSamplesPerSec * fmt.nBlockAlign;
        fmt.cbSize = 0;
        aw->event = CreateEventA(NULL, FALSE, FALSE, NULL);
        if (p_waveOutOpen(&aw->hwo, WAVE_MAPPER, &fmt,
                          (DWORD_PTR)aw->event, 0, CALLBACK_EVENT) == 0)
            aw->silent = 0;
        /* MIDI is optional and independent: XP's GS Wavetable synth is device 0. */
        if (p_midiOutOpen) p_midiOutOpen(&aw->hmidi, 0, 0, 0, 0);
    }

    aw->running = 1;
    aw->thread = CreateThread(NULL, 0, aw_thread, aw, 0, NULL);
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
    if (aw->hmidi && p_midiOutClose) p_midiOutClose(aw->hmidi);
    if (aw->event) CloseHandle(aw->event);
    if (aw->mod) FreeLibrary(aw->mod);
    aw->thread = 0; aw->event = 0; aw->hwo = 0; aw->hmidi = 0; aw->mod = 0;
}

void audio_wave_midi(audio_wave *aw, uint32_t msg)
{
    if (aw->hmidi && p_midiOutShortMsg) p_midiOutShortMsg(aw->hmidi, msg);
}
