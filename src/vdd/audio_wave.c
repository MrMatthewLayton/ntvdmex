/* audio_wave.c -- see audio_wave.h.  waveOut/midiOut bound at runtime. */
#include "audio_wave.h"
#include "audio_rec.h"    /* the ONE includer -- see its header */
#include "midi_route.h"   /* #136: which midiOut device the MIDI setting means */
#include "vdd_mpu.h"      /* #136: MPU_SYSEX_MAX, the longest SysEx we pass on */
#define COBJMACROS
#include <dsound.h>       /* #234: interfaces only -- dsound.dll is bound at runtime */

/* The modules and exports bound by name (no import library: optional at run time). */
#define AUDIO_MODULE_DSOUND "dsound.dll"
#define AUDIO_MODULE_WINMM  "winmm.dll"
#define AUDIO_EXPORT_DIRECT_SOUND_CREATE "DirectSoundCreate"
#define AUDIO_EXPORT_WAVE_OUT_OPEN               "waveOutOpen"
#define AUDIO_EXPORT_WAVE_OUT_PREPARE_HEADER     "waveOutPrepareHeader"
#define AUDIO_EXPORT_WAVE_OUT_UNPREPARE_HEADER   "waveOutUnprepareHeader"
#define AUDIO_EXPORT_WAVE_OUT_WRITE              "waveOutWrite"
#define AUDIO_EXPORT_WAVE_OUT_GET_VOLUME         "waveOutGetVolume"
#define AUDIO_EXPORT_WAVE_OUT_RESET              "waveOutReset"
#define AUDIO_EXPORT_WAVE_OUT_CLOSE              "waveOutClose"
#define AUDIO_EXPORT_MIDI_OUT_OPEN               "midiOutOpen"
#define AUDIO_EXPORT_MIDI_OUT_SHORT_MSG          "midiOutShortMsg"
#define AUDIO_EXPORT_MIDI_OUT_CLOSE              "midiOutClose"
#define AUDIO_EXPORT_MIDI_OUT_RESET              "midiOutReset"
#define AUDIO_EXPORT_MIDI_OUT_GET_NUM_DEVS       "midiOutGetNumDevs"
#define AUDIO_EXPORT_MIDI_OUT_GET_DEV_CAPS_A     "midiOutGetDevCapsA"
#define AUDIO_EXPORT_MIDI_OUT_PREPARE_HEADER     "midiOutPrepareHeader"
#define AUDIO_EXPORT_MIDI_OUT_UNPREPARE_HEADER   "midiOutUnprepareHeader"
#define AUDIO_EXPORT_MIDI_OUT_LONG_MSG           "midiOutLongMsg"

#define AUDIO_WAVE_DEFAULT_HZ       44100
#define AUDIO_WAVE_MIN_BUFFERS      2
#define AUDIO_WAVE_BITS_PER_SAMPLE  16
#define AUDIO_WAVE_WAKE_MS          5       /* a buffer is ~11.6 ms: wake well inside it */
#define AUDIO_WAVE_RETRY_MS         5
#define AUDIO_WAVE_DIRECT_SOUND_POLL_MS 2
#define AUDIO_WAVE_STOP_TIMEOUT_MS  500
#define AUDIO_WAVE_MIDI_DEVICES_MAX 16      /* devices enumerated for a MIDI choice     */
#define AUDIO_MIDI_NAME_CHARS       31      /* AUDIO_MIDI_NAME_LENGTH less the NUL      */
#define AUDIO_MIDI_HEADER_RESERVED  8       /* MIDIHDR.dwReserved                       */
#define AUDIO_MIDI_CHANNELS         16
#define AUDIO_MIDI_CONTROL_CHANGE   0xB0u
#define AUDIO_MIDI_DATA1_SHIFT      8       /* a short message: status | d1 << 8 | d2 << 16 */
#define AUDIO_MIDI_CC_SUSTAIN       64u
#define AUDIO_MIDI_CC_ALL_SOUND_OFF 120u
#define AUDIO_MIDI_CC_RESET_CONTROLLERS 121u
#define AUDIO_MIDI_CC_ALL_NOTES_OFF 123u

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

typedef struct _AUDIO_WAVE_FORMAT { WORD wFormatTag, nChannels; DWORD nSamplesPerSec, nAvgBytesPerSec;
                 WORD nBlockAlign, wBitsPerSample, cbSize; } AUDIO_WAVE_FORMAT, *PAUDIO_WAVE_FORMAT;
typedef const AUDIO_WAVE_FORMAT *PCAUDIO_WAVE_FORMAT;
typedef struct _AUDIO_WAVE_HEADER { LPSTR lpData; DWORD dwBufferLength, dwBytesRecorded;
                 DWORD_PTR dwUser; DWORD dwFlags, dwLoops;
                 PVOID lpNext; DWORD_PTR reserved; } AUDIO_WAVE_HEADER, *PAUDIO_WAVE_HEADER;
typedef const AUDIO_WAVE_HEADER *PCAUDIO_WAVE_HEADER;

typedef UINT (WINAPI *PFN_WAVE_OUT_OPEN)(PVOID *, UINT, const AUDIO_WAVE_FORMAT *,
                                       DWORD_PTR, DWORD_PTR, DWORD);
typedef UINT (WINAPI *PFN_WAVE_OUT_HEADER)(PVOID, PAUDIO_WAVE_HEADER, UINT);
typedef UINT (WINAPI *PFN_WAVE_OUT_HANDLE)(PVOID);
typedef UINT (WINAPI *PFN_WAVE_OUT_VOLUME)(PVOID, DWORD *);
typedef UINT (WINAPI *PFN_MIDI_OUT_OPEN)(PVOID *, UINT, DWORD_PTR, DWORD_PTR, DWORD);
typedef UINT (WINAPI *PFN_MIDI_OUT_SHORT)(PVOID, DWORD);
typedef UINT (WINAPI *PFN_MIDI_OUT_HANDLE)(PVOID);
/* #136: device names (MIDIOUTCAPSA) and SysEx (MIDIHDR + the long-message calls). */
typedef struct _AUDIO_MIDI_OUT_CAPS { WORD wMid, wPid; UINT vDriverVersion; CHAR szPname[AUDIO_MIDI_NAME_LENGTH];
                 WORD wTechnology, wVoices, wNotes, wChannelMask; DWORD dwSupport; } AUDIO_MIDI_OUT_CAPS, *PAUDIO_MIDI_OUT_CAPS;
typedef const AUDIO_MIDI_OUT_CAPS *PCAUDIO_MIDI_OUT_CAPS;
typedef struct _AUDIO_MIDI_HEADER { LPSTR lpData; DWORD dwBufferLength, dwBytesRecorded; DWORD_PTR dwUser;
                 DWORD dwFlags; PVOID lpNext; DWORD_PTR reserved; DWORD dwOffset;
                 DWORD_PTR dwReserved[AUDIO_MIDI_HEADER_RESERVED]; } AUDIO_MIDI_HEADER, *PAUDIO_MIDI_HEADER;
typedef const AUDIO_MIDI_HEADER *PCAUDIO_MIDI_HEADER;
#define AUDIO_WAVE_MHDR_DONE      0x00000001
#define AUDIO_WAVE_MHDR_PREPARED  0x00000002
typedef UINT (WINAPI *PFN_MIDI_OUT_GET_NUM_DEVS)(VOID);
typedef UINT (WINAPI *PFN_MIDI_OUT_GET_DEV_CAPS)(UINT_PTR, PAUDIO_MIDI_OUT_CAPS, UINT);
typedef UINT (WINAPI *PFN_MIDI_OUT_HEADER)(PVOID, PAUDIO_MIDI_HEADER, UINT);

static PFN_WAVE_OUT_OPEN   g_WaveOutOpen;
static PFN_WAVE_OUT_HEADER    g_WaveOutPrepareHeader, g_WaveOutUnprepareHeader, g_WaveOutWrite;
static PFN_WAVE_OUT_HANDLE    g_WaveOutReset, g_WaveOutClose;
static PFN_WAVE_OUT_VOLUME    g_WaveOutGetVolume;
static PFN_MIDI_OUT_OPEN   g_MidiOutOpen;
static PFN_MIDI_OUT_SHORT  g_MidiOutShortMsg;
static PFN_MIDI_OUT_HANDLE  g_MidiOutClose;
static PFN_MIDI_OUT_HANDLE  g_MidiOutReset;  /* same shape: UINT (HMIDIOUT) */
static PFN_MIDI_OUT_GET_NUM_DEVS  g_MidiOutGetNumDevs; /* #136 */
static PFN_MIDI_OUT_GET_DEV_CAPS g_MidiOutGetDevCapsA;
static PFN_MIDI_OUT_HEADER    g_MidiOutPrepareHeader, g_MidiOutUnprepareHeader, g_MidiOutLongMsg;

/* #136: SysEx slots. midiOutLongMsg is ASYNCHRONOUS -- the driver owns the buffer until
   it sets MHDR_DONE -- so each message is copied into a slot that stays put until then. */
#define AUDIO_WAVE_SYSEX_SLOTS 8
static AUDIO_MIDI_HEADER g_AudioWaveSysExHeaders[AUDIO_WAVE_SYSEX_SLOTS];
static CHAR       g_AudioWaveSysExBuffers[AUDIO_WAVE_SYSEX_SLOTS][MPU_SYSEX_MAX];
static UINT   g_AudioWaveSysExNext;

static PAUDIO_WAVE_HEADER AudioWaveHeader(PAUDIO_WAVE wave, INT bufferIndex)
{ return (PAUDIO_WAVE_HEADER)wave->Headers[bufferIndex]; }

static DWORD WINAPI AudioWaveThread(LPVOID parameter)
{
    PAUDIO_WAVE wave = (PAUDIO_WAVE)parameter;
    UINT32 bufferIndex;                          /* matches wave->BufferCount; was int (sign-compare) */

    /* THE EXEC THREAD RUNS FLAT OUT. It is a guest burning 100% of a core, and at normal
       priority this thread loses the race often enough that buffers run dry -- which is
       heard as a periodic tick or pulse in otherwise correct music, exactly as reported.
       Audio refill is a hard real-time deadline (every ~11.6 ms per buffer) doing almost no
       work, so it belongs above the guest. TIME_CRITICAL is what waveOut feeders normally
       use; fall back gracefully if the system refuses it. */
    if (!SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL))
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);

    /* Prime every buffer, then top them up as the driver returns them. */
    for (bufferIndex = 0; bufferIndex < wave->BufferCount && !wave->IsSilent; ++bufferIndex) {
        PAUDIO_WAVE_HEADER header = AudioWaveHeader(wave, bufferIndex);
        header->lpData = (LPSTR)wave->Buffers[bufferIndex];
        header->dwBufferLength = wave->FrameCount * AUDIO_STEREO_CHANNELS * sizeof(INT16);
        header->dwFlags = 0; header->dwLoops = 0; header->dwUser = 0;
        g_WaveOutPrepareHeader(wave->WaveOut, header, sizeof(AUDIO_WAVE_HEADER));
        wave->Fill(wave->Context, wave->Buffers[bufferIndex], wave->FrameCount);
        AudioRecorderFeed(wave->Buffers[bufferIndex], wave->FrameCount * AUDIO_STEREO_CHANNELS);
        g_WaveOutWrite(wave->WaveOut, header, sizeof(AUDIO_WAVE_HEADER));
    }

    while (wave->IsRunning) {
        if (wave->IsSilent) {
            /* No device: still pump the mixer at real-time pace, because that is
               what advances SB playback and raises its IRQ. */
            wave->Fill(wave->Context, wave->Buffers[0], wave->FrameCount);
            AudioRecorderFeed(wave->Buffers[0], wave->FrameCount * AUDIO_STEREO_CHANNELS);
            Sleep((wave->FrameCount * MILLISECONDS_PER_SECOND) / (wave->SampleHz ? wave->SampleHz : AUDIO_WAVE_DEFAULT_HZ));
            continue;
        }
        /* How much of the queue has the driver already given back? Sampled BEFORE we
           refill anything, so it is the true low-water mark of this pass. See the note
           on `Starved` in audio_wave.h: waveOutWrite succeeding tells us nothing. */
        { UINT32 handedBack = 0;
          for (bufferIndex = 0; bufferIndex < wave->BufferCount; ++bufferIndex)
              if (AudioWaveHeader(wave, bufferIndex)->dwFlags & WHDR_DONE) ++handedBack;
          if (handedBack > wave->DrainMax) wave->DrainMax = handedBack;
          if (handedBack >= wave->BufferCount) ++wave->Starved;
          wave->DrainHistogram[handedBack <= AUDIO_WAVE_BUFFERS ? handedBack : AUDIO_WAVE_BUFFERS]++; }

        for (bufferIndex = 0; bufferIndex < wave->BufferCount; ++bufferIndex) {
            PAUDIO_WAVE_HEADER header = AudioWaveHeader(wave, bufferIndex);
            if (!(header->dwFlags & WHDR_DONE)) continue;
            header->dwFlags &= ~WHDR_DONE;
            wave->Fill(wave->Context, wave->Buffers[bufferIndex], wave->FrameCount);
        AudioRecorderFeed(wave->Buffers[bufferIndex], wave->FrameCount * AUDIO_STEREO_CHANNELS);
            if (g_WaveOutWrite(wave->WaveOut, header, sizeof(AUDIO_WAVE_HEADER)) != 0) wave->Underruns++;
        }
        /* Wake early and often: a full buffer is ~11.6 ms, so a 20 ms timeout could miss a
           whole buffer's deadline if the completion event is late. */
        WaitForSingleObject(wave->Event, AUDIO_WAVE_WAKE_MS);
    }

    if (!wave->IsSilent) {
        g_WaveOutReset(wave->WaveOut);
        for (bufferIndex = 0; bufferIndex < wave->BufferCount; ++bufferIndex)
            g_WaveOutUnprepareHeader(wave->WaveOut, AudioWaveHeader(wave, bufferIndex), sizeof(AUDIO_WAVE_HEADER));
    }
    return 0;
}

/* ── #234: THE DIRECTSOUND OUTPUT. One looping secondary buffer holding the same
     lead as the waveOut queue (BufferCount x FrameCount), filled ahead of the play cursor a
     chunk at a time from the same mixer callback, and fed to the same recorder. The
     cursor tells us how far ahead we are, so starvation is MEASURED rather than
     inferred: `Starved` counts passes where the play cursor had caught up with us.
     GLOBALFOCUS so it keeps playing when the window is not in front (the pause in
     #219 is what silences a background window, not the output). */
typedef HRESULT (WINAPI *PFN_DIRECT_SOUND_CREATE)(LPCGUID, LPDIRECTSOUND *, LPUNKNOWN);

static INT AudioWaveDirectSoundOpen(PAUDIO_WAVE wave, PCAUDIO_WAVE_FORMAT format)
{
    HMODULE module = LoadLibraryA(AUDIO_MODULE_DSOUND);
    PFN_DIRECT_SOUND_CREATE directSoundCreate;
    LPDIRECTSOUND directSound = NULL; LPDIRECTSOUNDBUFFER buffer = NULL;
    DSBUFFERDESC description; WAVEFORMATEX waveFormat;
    PVOID part1, part2; DWORD length1, length2;
    if (!module) return 0;
    directSoundCreate = (PFN_DIRECT_SOUND_CREATE)GetProcAddress(module, AUDIO_EXPORT_DIRECT_SOUND_CREATE);
    if (!directSoundCreate || FAILED(directSoundCreate(NULL, &directSound, NULL))) return 0;
    if (FAILED(IDirectSound_SetCooperativeLevel(directSound, GetDesktopWindow(), DSSCL_NORMAL))) {
        IDirectSound_Release(directSound); return 0;
    }
    ZeroMemory(&waveFormat, sizeof waveFormat);
    waveFormat.wFormatTag = WAVE_FORMAT_PCM; waveFormat.nChannels = format->nChannels;
    waveFormat.nSamplesPerSec = format->nSamplesPerSec; waveFormat.wBitsPerSample = format->wBitsPerSample;
    waveFormat.nBlockAlign = format->nBlockAlign; waveFormat.nAvgBytesPerSec = format->nAvgBytesPerSec;
    ZeroMemory(&description, sizeof description); description.dwSize = sizeof description;
    description.dwFlags = DSBCAPS_GLOBALFOCUS | DSBCAPS_GETCURRENTPOSITION2;
    wave->DirectSoundBytes = wave->BufferCount * wave->FrameCount * AUDIO_STEREO_CHANNELS * (UINT32)sizeof(INT16);
    description.dwBufferBytes = wave->DirectSoundBytes; description.lpwfxFormat = &waveFormat;
    if (FAILED(IDirectSound_CreateSoundBuffer(directSound, &description, &buffer, NULL))) {
        IDirectSound_Release(directSound); return 0;
    }
    /* Start from silence; the thread fills ahead of the cursor from here. */
    if (SUCCEEDED(IDirectSoundBuffer_Lock(buffer, 0, wave->DirectSoundBytes, &part1, &length1, &part2, &length2, 0))) {
        ZeroMemory(part1, length1); if (part2) ZeroMemory(part2, length2);
        IDirectSoundBuffer_Unlock(buffer, part1, length1, part2, length2);
    }
    wave->DirectSound = directSound; wave->DirectSoundBuffer = buffer; wave->DirectSoundWritePosition = 0;
    if (FAILED(IDirectSoundBuffer_Play(buffer, 0, 0, DSBPLAY_LOOPING))) {
        IDirectSoundBuffer_Release(buffer); IDirectSound_Release(directSound);
        wave->DirectSound = wave->DirectSoundBuffer = NULL; return 0;
    }
    return 1;
}

static DWORD WINAPI AudioWaveDirectSoundThread(LPVOID parameter)
{
    PAUDIO_WAVE wave = (PAUDIO_WAVE)parameter;
    LPDIRECTSOUNDBUFFER buffer = (LPDIRECTSOUNDBUFFER)wave->DirectSoundBuffer;
    UINT32 chunk = wave->FrameCount * AUDIO_STEREO_CHANNELS * (UINT32)sizeof(INT16);
    if (!SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL))
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    while (wave->IsRunning) {
        DWORD playCursor = 0, writeCursor = 0;
        UINT32 ahead, space;
        if (FAILED(IDirectSoundBuffer_GetCurrentPosition(buffer, &playCursor, &writeCursor))) { Sleep(AUDIO_WAVE_RETRY_MS); continue; }
        ahead = (wave->DirectSoundWritePosition + wave->DirectSoundBytes - playCursor) % wave->DirectSoundBytes;   /* queued, not played */
        space = wave->DirectSoundBytes - ahead;
        if (ahead < chunk) ++wave->Starved;             /* the cursor caught up with us */
        {   UINT32 queuedBuffers = ahead / chunk, handedBack = wave->BufferCount - (queuedBuffers < wave->BufferCount ? queuedBuffers : wave->BufferCount);
            if (handedBack > wave->DrainMax) wave->DrainMax = handedBack;
            wave->DrainHistogram[handedBack <= AUDIO_WAVE_BUFFERS ? handedBack : AUDIO_WAVE_BUFFERS]++; }
        while (space >= chunk) {
            PVOID part1, part2; DWORD length1, length2;
            wave->Fill(wave->Context, wave->Buffers[0], wave->FrameCount);
            AudioRecorderFeed(wave->Buffers[0], wave->FrameCount * AUDIO_STEREO_CHANNELS);
            if (SUCCEEDED(IDirectSoundBuffer_Lock(buffer, wave->DirectSoundWritePosition, chunk, &part1, &length1, &part2, &length2, 0))) {
                CopyMemory(part1, wave->Buffers[0], length1);
                if (part2) CopyMemory(part2, (BYTE *)wave->Buffers[0] + length1, length2);
                IDirectSoundBuffer_Unlock(buffer, part1, length1, part2, length2);
            } else wave->Underruns++;
            wave->DirectSoundWritePosition = (wave->DirectSoundWritePosition + chunk) % wave->DirectSoundBytes;
            space -= chunk;
        }
        Sleep(AUDIO_WAVE_DIRECT_SOUND_POLL_MS);
    }
    IDirectSoundBuffer_Stop(buffer);
    return 0;
}

static INT AudioWaveBind(PAUDIO_WAVE wave)
{
    wave->Module = LoadLibraryA(AUDIO_MODULE_WINMM);
    if (!wave->Module) return 0;
    g_WaveOutOpen       = (PFN_WAVE_OUT_OPEN) GetProcAddress(wave->Module, AUDIO_EXPORT_WAVE_OUT_OPEN);
    g_WaveOutPrepareHeader    = (PFN_WAVE_OUT_HEADER)  GetProcAddress(wave->Module, AUDIO_EXPORT_WAVE_OUT_PREPARE_HEADER);
    g_WaveOutUnprepareHeader  = (PFN_WAVE_OUT_HEADER)  GetProcAddress(wave->Module, AUDIO_EXPORT_WAVE_OUT_UNPREPARE_HEADER);
    g_WaveOutWrite      = (PFN_WAVE_OUT_HEADER)  GetProcAddress(wave->Module, AUDIO_EXPORT_WAVE_OUT_WRITE);
    g_WaveOutGetVolume  = (PFN_WAVE_OUT_VOLUME)  GetProcAddress(wave->Module, AUDIO_EXPORT_WAVE_OUT_GET_VOLUME);
    g_WaveOutReset      = (PFN_WAVE_OUT_HANDLE)  GetProcAddress(wave->Module, AUDIO_EXPORT_WAVE_OUT_RESET);
    g_WaveOutClose      = (PFN_WAVE_OUT_HANDLE)  GetProcAddress(wave->Module, AUDIO_EXPORT_WAVE_OUT_CLOSE);
    g_MidiOutOpen       = (PFN_MIDI_OUT_OPEN) GetProcAddress(wave->Module, AUDIO_EXPORT_MIDI_OUT_OPEN);
    g_MidiOutShortMsg   = (PFN_MIDI_OUT_SHORT)GetProcAddress(wave->Module, AUDIO_EXPORT_MIDI_OUT_SHORT_MSG);
    g_MidiOutClose      = (PFN_MIDI_OUT_HANDLE)GetProcAddress(wave->Module, AUDIO_EXPORT_MIDI_OUT_CLOSE);
    g_MidiOutReset      = (PFN_MIDI_OUT_HANDLE)GetProcAddress(wave->Module, AUDIO_EXPORT_MIDI_OUT_RESET);
    g_MidiOutGetNumDevs = (PFN_MIDI_OUT_GET_NUM_DEVS) GetProcAddress(wave->Module, AUDIO_EXPORT_MIDI_OUT_GET_NUM_DEVS);
    g_MidiOutGetDevCapsA= (PFN_MIDI_OUT_GET_DEV_CAPS)GetProcAddress(wave->Module, AUDIO_EXPORT_MIDI_OUT_GET_DEV_CAPS_A);
    g_MidiOutPrepareHeader    = (PFN_MIDI_OUT_HEADER)  GetProcAddress(wave->Module, AUDIO_EXPORT_MIDI_OUT_PREPARE_HEADER);
    g_MidiOutUnprepareHeader  = (PFN_MIDI_OUT_HEADER)  GetProcAddress(wave->Module, AUDIO_EXPORT_MIDI_OUT_UNPREPARE_HEADER);
    g_MidiOutLongMsg    = (PFN_MIDI_OUT_HEADER)  GetProcAddress(wave->Module, AUDIO_EXPORT_MIDI_OUT_LONG_MSG);
    return g_WaveOutOpen && g_WaveOutPrepareHeader && g_WaveOutWrite &&
           g_WaveOutReset && g_WaveOutClose;
}

/* ── #136: OPEN THE MIDI DEVICE THE SETTING NAMES. ─────────────────────────────────
     Host GM (the default) is the old call, unchanged: device 0, no enumeration. Any
     other choice enumerates the devices and opens the one MidiRoutePick finds by name;
     none found -> device 0 anyway, with IsMidiExternal = 0 so it is treated as the GM synth
     it is (no SysEx), and the host's log line says the choice was not met. */
static VOID AudioWaveMidiOpen(PAUDIO_WAVE wave)
{
    UINT device = 0;
    if (!g_MidiOutOpen) return;
    if (wave->MidiChoice != MIDI_ROUTE_GM && g_MidiOutGetNumDevs && g_MidiOutGetDevCapsA) {
        static CHAR names[AUDIO_WAVE_MIDI_DEVICES_MAX][AUDIO_MIDI_NAME_LENGTH];
        PCSTR namePointers[AUDIO_WAVE_MIDI_DEVICES_MAX];
        UINT deviceCount = g_MidiOutGetNumDevs(), deviceIndex;
        INT pick, characterIndex;
        wave->MidiDeviceCount = deviceCount;
        if (deviceCount > AUDIO_WAVE_MIDI_DEVICES_MAX) deviceCount = AUDIO_WAVE_MIDI_DEVICES_MAX;
        for (deviceIndex = 0; deviceIndex < deviceCount; ++deviceIndex) {
            AUDIO_MIDI_OUT_CAPS capabilities;
            names[deviceIndex][0] = 0;
            if (g_MidiOutGetDevCapsA(deviceIndex, &capabilities, sizeof capabilities) == 0) {
                for (characterIndex = 0; characterIndex < AUDIO_MIDI_NAME_CHARS && capabilities.szPname[characterIndex]; ++characterIndex) names[deviceIndex][characterIndex] = capabilities.szPname[characterIndex];
                names[deviceIndex][characterIndex] = 0;
            }
            namePointers[deviceIndex] = names[deviceIndex];
        }
        pick = MidiRoutePick(wave->MidiChoice, namePointers, (INT)deviceCount);
        if (pick >= 0) { device = (UINT)pick; wave->IsMidiExternal = 1; }
        if (device < deviceCount) {
            for (characterIndex = 0; characterIndex < AUDIO_MIDI_NAME_CHARS && names[device][characterIndex]; ++characterIndex) wave->MidiName[characterIndex] = names[device][characterIndex];
            wave->MidiName[characterIndex] = 0;
        }
    }
    if (g_MidiOutOpen(&wave->MidiOut, device, 0, 0, 0) == 0) wave->MidiDevice = (INT)device;
    else { wave->MidiOut = 0; wave->IsMidiExternal = 0; }
}

INT AudioWaveStart(PAUDIO_WAVE wave, UINT32 sampleHz, PAUDIO_WAVE_FILL_ROUTINE fill, PVOID context)
{
    AUDIO_WAVE_FORMAT format;
    UINT byteIndex; BYTE *bytes = (BYTE *)wave;
    /* ► PRESERVE THE LEAD ACROSS THE ZEROING. The caller sets wave->BufferCount from awbufs.txt
         BEFORE calling us, and this function wipes the whole struct -- so read it back
         out first, exactly as VddSbReset() preserves its bus pointers. Getting this
         wrong would silently pin the experiment at one value while appearing to vary it,
         which is the failure mode this counter exists to avoid. */
    UINT32 wantBuffers = wave->BufferCount, wantFrames = wave->FrameCount;
    INT wantsDirectSound = wave->WantsDirectSound;                  /* #234: preserved like the lead */
    INT isForcedSilent = wave->IsForcedSilent;  /* #132: ditto -- the rig caught it wiped */
    INT midiChoice = wave->MidiChoice;          /* #136: ditto */
    for (byteIndex = 0; byteIndex < sizeof(*wave); ++byteIndex) bytes[byteIndex] = 0;
    if (!wantBuffers)   wantBuffers   = AUDIO_WAVE_DEFAULT_BUFFERS;     /* 0 = "leave it alone"  */
    if (wantBuffers < AUDIO_WAVE_MIN_BUFFERS) wantBuffers = AUDIO_WAVE_MIN_BUFFERS;
    if (wantBuffers > AUDIO_WAVE_BUFFERS) wantBuffers = AUDIO_WAVE_BUFFERS;
    if (!wantFrames) wantFrames = AUDIO_WAVE_DEFAULT_FRAMES;
    if (wantFrames < AUDIO_WAVE_MIN_FRAMES) wantFrames = AUDIO_WAVE_MIN_FRAMES;
    if (wantFrames > AUDIO_WAVE_FRAMES)     wantFrames = AUDIO_WAVE_FRAMES;
    wave->BufferCount   = wantBuffers;
    wave->FrameCount = wantFrames;
    wave->WantsDirectSound = wantsDirectSound;
    wave->IsForcedSilent = isForcedSilent;
    wave->MidiChoice = midiChoice;
    wave->MidiDevice = -1;

    wave->SampleHz = sampleHz ? sampleHz : AUDIO_WAVE_DEFAULT_HZ;
    wave->Fill = fill; wave->Context = context;
    wave->IsSilent = 1;                         /* until a device opens           */

    if (!wave->IsForcedSilent && AudioWaveBind(wave)) {
        format.wFormatTag = WAVE_FORMAT_PCM;
        format.nChannels = AUDIO_STEREO_CHANNELS;          /* #189 */
        format.nSamplesPerSec = wave->SampleHz;
        format.wBitsPerSample = AUDIO_WAVE_BITS_PER_SAMPLE;
        format.nBlockAlign = (WORD)(format.nChannels * format.wBitsPerSample / BITS_PER_BYTE);
        format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;
        format.cbSize = 0;
        wave->Event = CreateEventA(NULL, FALSE, FALSE, NULL);
        if (wave->WantsDirectSound && AudioWaveDirectSoundOpen(wave, &format)) {     /* #234: DirectSound first...   */
            wave->IsUsingDirectSound = 1; wave->IsSilent = 0;
        } else if (g_WaveOutOpen(&wave->WaveOut, WAVE_MAPPER, &format,  /* ...WinMM otherwise */
                          (DWORD_PTR)wave->Event, 0, CALLBACK_EVENT) == 0)
            wave->IsSilent = 0;
        /* ⚠ ASK THE DRIVER WHAT ITS VOLUME IS. Every other counter here can read
             perfect while the machine is silent, because Windows' own WAVE slider
             attenuates after us. Read, never written -- see audio_wave.h. */
        if (!wave->IsSilent && !wave->IsUsingDirectSound && g_WaveOutGetVolume) {
            DWORD volume = 0;
            if (g_WaveOutGetVolume(wave->WaveOut, &volume) == 0) {
                wave->DeviceVolume = (UINT32)volume; wave->IsDeviceVolumeKnown = 1;
            }
        }
        /* MIDI is optional and independent: XP's GS Wavetable synth is device 0. */
        AudioWaveMidiOpen(wave);            /* #136: or the device the setting names */
    }

    wave->IsRunning = 1;
    wave->Thread = CreateThread(NULL, 0, wave->IsUsingDirectSound ? AudioWaveDirectSoundThread : AudioWaveThread, wave, 0, NULL);
    if (!wave->Thread) { wave->IsRunning = 0; return 1; }
    return wave->IsSilent ? 1 : 0;
}

VOID AudioWaveStop(PAUDIO_WAVE wave)
{
    if (!wave->IsRunning) return;
    InterlockedExchange(&wave->IsRunning, 0);
    if (wave->Event) SetEvent(wave->Event);
    if (wave->Thread) { WaitForSingleObject(wave->Thread, AUDIO_WAVE_STOP_TIMEOUT_MS); CloseHandle(wave->Thread); }
    if (!wave->IsSilent && wave->WaveOut && g_WaveOutClose) g_WaveOutClose(wave->WaveOut);
    if (wave->DirectSoundBuffer) { IDirectSoundBuffer_Release((LPDIRECTSOUNDBUFFER)wave->DirectSoundBuffer); wave->DirectSoundBuffer = NULL; }
    if (wave->DirectSound)  { IDirectSound_Release((LPDIRECTSOUND)wave->DirectSound); wave->DirectSound = NULL; }
    AudioWaveMidiSilence(wave);           /* #214: a held note must not outlive us */
    /* #136: midiOutReset (in the silence above) hands every queued SysEx buffer back,
       and a prepared header must be unprepared before its device closes. */
    {   UINT slot;
        for (slot = 0; slot < AUDIO_WAVE_SYSEX_SLOTS; ++slot)
            if ((g_AudioWaveSysExHeaders[slot].dwFlags & AUDIO_WAVE_MHDR_PREPARED) && wave->MidiOut && g_MidiOutUnprepareHeader)
                g_MidiOutUnprepareHeader(wave->MidiOut, &g_AudioWaveSysExHeaders[slot], sizeof g_AudioWaveSysExHeaders[slot]);
    }
    if (wave->MidiOut && g_MidiOutClose) g_MidiOutClose(wave->MidiOut);
    if (wave->Event) CloseHandle(wave->Event);
    if (wave->Module) FreeLibrary(wave->Module);
    wave->Thread = 0; wave->Event = 0; wave->WaveOut = 0; wave->MidiOut = 0; wave->Module = 0;
}

VOID AudioWaveMidi(PAUDIO_WAVE wave, UINT32 message)
{
    if (wave->MidiOut && g_MidiOutShortMsg) g_MidiOutShortMsg(wave->MidiOut, message);
}

VOID AudioWaveMidiLong(PAUDIO_WAVE wave, const BYTE *message, UINT32 length)
{
    UINT attempt, slot;
    PAUDIO_MIDI_HEADER header;
    if (!wave->MidiOut || !g_MidiOutLongMsg || !g_MidiOutPrepareHeader || !g_MidiOutUnprepareHeader) return;
    if (!length || length > MPU_SYSEX_MAX) { wave->SysExDropped++; return; }
    for (attempt = 0; attempt < AUDIO_WAVE_SYSEX_SLOTS; ++attempt) {
        slot = (g_AudioWaveSysExNext + attempt) % AUDIO_WAVE_SYSEX_SLOTS;
        header = &g_AudioWaveSysExHeaders[slot];
        if (header->dwFlags & AUDIO_WAVE_MHDR_PREPARED) {
            if (!(header->dwFlags & AUDIO_WAVE_MHDR_DONE)) continue;           /* the driver still has it */
            g_MidiOutUnprepareHeader(wave->MidiOut, header, sizeof *header);
        }
        {   UINT32 byteIndex;
            for (byteIndex = 0; byteIndex < length; ++byteIndex) g_AudioWaveSysExBuffers[slot][byteIndex] = (CHAR)message[byteIndex]; }
        ZeroMemory(header, sizeof *header);
        header->lpData = g_AudioWaveSysExBuffers[slot]; header->dwBufferLength = length; header->dwBytesRecorded = length;
        if (g_MidiOutPrepareHeader(wave->MidiOut, header, sizeof *header) != 0) { wave->SysExDropped++; return; }
        if (g_MidiOutLongMsg(wave->MidiOut, header, sizeof *header) != 0) {
            g_MidiOutUnprepareHeader(wave->MidiOut, header, sizeof *header);
            wave->SysExDropped++; return;
        }
        g_AudioWaveSysExNext = (slot + 1) % AUDIO_WAVE_SYSEX_SLOTS;
        wave->SysExSent++;
        return;
    }
    wave->SysExDropped++;                                        /* every slot in flight */
}

/* #214: EVERY NOTE OFF, ON EVERY CHANNEL. The emulated MPU-401 can be reset, but the
   notes it already sent live in XP's synth, which knows nothing about a program ending:
   Close Program on Doom left them sounding. Per channel: sustain pedal up (a held pedal
   outlives a note-off), All Sound Off (120), All Notes Off (123), Reset All Controllers
   (121, so pitch bend and modulation do not carry into the next program); then
   midiOutReset, which is winmm's own "turn off all notes". Program changes are left
   alone -- every program sets its own. */
VOID AudioWaveMidiSilence(PAUDIO_WAVE wave)
{
    UINT32 channel;
    if (!wave->MidiOut || !g_MidiOutShortMsg) return;
    for (channel = 0; channel < AUDIO_MIDI_CHANNELS; ++channel) {
        g_MidiOutShortMsg(wave->MidiOut, AUDIO_MIDI_CONTROL_CHANGE | channel | (AUDIO_MIDI_CC_SUSTAIN  << AUDIO_MIDI_DATA1_SHIFT));    /* sustain 0 */
        g_MidiOutShortMsg(wave->MidiOut, AUDIO_MIDI_CONTROL_CHANGE | channel | (AUDIO_MIDI_CC_ALL_SOUND_OFF << AUDIO_MIDI_DATA1_SHIFT));    /* sound off */
        g_MidiOutShortMsg(wave->MidiOut, AUDIO_MIDI_CONTROL_CHANGE | channel | (AUDIO_MIDI_CC_ALL_NOTES_OFF << AUDIO_MIDI_DATA1_SHIFT));    /* notes off */
        g_MidiOutShortMsg(wave->MidiOut, AUDIO_MIDI_CONTROL_CHANGE | channel | (AUDIO_MIDI_CC_RESET_CONTROLLERS << AUDIO_MIDI_DATA1_SHIFT));    /* reset CCs */
    }
    if (g_MidiOutReset) g_MidiOutReset(wave->MidiOut);
}

/* Recording what we play -- see audio_rec.h. Exported so the host can drive it. */
INT      AudioWaveRecordStart(PCSTR path, UINT32 sampleHz) { return AudioRecorderStart(path, sampleHz); }
UINT32 AudioWaveRecordStop(VOID)    { return AudioRecorderStop(); }
INT      AudioWaveIsRecording(VOID)  { return AudioRecorderIsActive(); }
UINT32 AudioWaveRecordDropped(VOID) { return g_AudioRecorder.Dropped; }
