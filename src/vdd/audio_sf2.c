/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * The SoundFont synth, hosted: see audio_sf2.h for why and how. The synth itself is
 * sf2_synth.c; this file maps the font, starts it, queues messages and times Render.
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#include "audio_sf2.h"
#include <windows.h>
#include "audio_format.h"
#include "sf2_synth.h"

#define AUDIO_SF2_QUEUE_BYTES           32768   /* Messages between two Renders, or while the font loads */
#define AUDIO_SF2_QUEUE_SHORT           0
#define AUDIO_SF2_QUEUE_LONG            1
#define AUDIO_SF2_QUEUE_HEADER          3       /* kind byte + 16-bit length */
#define AUDIO_SF2_QUEUE_LONGEST         0xFFFF
#define AUDIO_SF2_MIDI_CHANNELS         16
#define AUDIO_SF2_STATUS_CONTROL        0xB0
#define AUDIO_SF2_DATA1_SHIFT           8
#define AUDIO_SF2_CC_SUSTAIN            64
#define AUDIO_SF2_CC_ALL_SOUND_OFF      120
#define AUDIO_SF2_CC_RESET_CONTROLLERS  121
#define AUDIO_SF2_CC_ALL_NOTES_OFF      123
#define AUDIO_SF2_STEP_CREATE_FILE      "CreateFile"
#define AUDIO_SF2_STEP_FILE_SIZE        "GetFileSize (a font of 4 GB or more)"
#define AUDIO_SF2_STEP_CREATE_MAPPING   "CreateFileMapping"
#define AUDIO_SF2_STEP_MAP_VIEW         "MapViewOfFile"
#define AUDIO_SF2_STEP_PARSE            "parse"
#define AUDIO_SF2_STEP_NO_PRESETS       "parse (the font has no presets)"
#define AUDIO_SF2_STEP_CREATE_THREAD    "CreateThread"

static AUDIO_SF2_STATS g_AudioSf2Stats;
static CRITICAL_SECTION g_AudioSf2Lock;         /* the queue */
static volatile LONG g_AudioSf2State;
static volatile LONG g_AudioSf2IsStopped;
static DWORD g_AudioSf2Hz;
static DWORD g_AudioSf2WakeMs;
static CHAR g_AudioSf2Path[MAX_PATH];
static HANDLE g_AudioSf2File = INVALID_HANDLE_VALUE;
static HANDLE g_AudioSf2Mapping;
static PCBYTE g_AudioSf2View;
static SF2_FONT g_AudioSf2Font;
static SF2_SYNTH g_AudioSf2Synth;               /* touched only by the start thread, then the audio thread */
static BYTE g_AudioSf2Queue[AUDIO_SF2_QUEUE_BYTES];
static DWORD g_AudioSf2QueueLength;
static BYTE g_AudioSf2Drain[AUDIO_SF2_QUEUE_BYTES];     /* the audio thread's copy of the queue */

/* Caller holds the lock. A message that does not fit is dropped and counted, never cut. */
static VOID AudioSf2Queue(BYTE kind, const BYTE *payload, DWORD length)
{
    if (length > AUDIO_SF2_QUEUE_LONGEST
        || g_AudioSf2QueueLength + AUDIO_SF2_QUEUE_HEADER + length > AUDIO_SF2_QUEUE_BYTES)
    {
        g_AudioSf2Stats.Dropped++;
        return;
    }

    g_AudioSf2Queue[g_AudioSf2QueueLength] = kind;
    g_AudioSf2Queue[g_AudioSf2QueueLength + 1] = (BYTE)length;
    g_AudioSf2Queue[g_AudioSf2QueueLength + 2] = (BYTE)(length >> 8);
    CopyMemory(g_AudioSf2Queue + g_AudioSf2QueueLength + AUDIO_SF2_QUEUE_HEADER, payload, length);
    g_AudioSf2QueueLength += AUDIO_SF2_QUEUE_HEADER + length;
}

/* Audio thread: hand a drained queue to the synth, in arrival order. */
static VOID AudioSf2Play(const BYTE *queue, DWORD queueLength)
{
    DWORD offset = 0;

    while (offset < queueLength)
    {
        BYTE kind = queue[offset];
        DWORD length = queue[offset + 1] | (queue[offset + 2] << 8);
        const BYTE *payload = queue + offset + AUDIO_SF2_QUEUE_HEADER;

        if (kind == AUDIO_SF2_QUEUE_SHORT)
        {
            DWORD message;

            CopyMemory(&message, payload, sizeof(message));
            Sf2SynthShortMessage(&g_AudioSf2Synth, message);
        }
        else
            Sf2SynthSysEx(&g_AudioSf2Synth, payload, length);

        offset += AUDIO_SF2_QUEUE_HEADER + length;
    }
}

/* start (background): */

static BOOL AudioSf2Fail(PCSTR step, PCSTR detail, DWORD error)
{
    g_AudioSf2Stats.FailedStep = step;
    g_AudioSf2Stats.FailedDetail = detail;
    g_AudioSf2Stats.FailedError = error;
    return FALSE;
}

/* Map the whole file read-only and parse it in place. TRUE = the synth is ready to play. */
static BOOL AudioSf2Open(VOID)
{
    DWORD sizeHigh = 0;
    DWORD sizeLow;
    SF2_PARSE_RESULT result;

    g_AudioSf2File = CreateFileA(
        g_AudioSf2Path,
        GENERIC_READ,
        FILE_SHARE_READ,
        NULL,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_RANDOM_ACCESS,
        NULL);

    if (g_AudioSf2File == INVALID_HANDLE_VALUE)
        return AudioSf2Fail(AUDIO_SF2_STEP_CREATE_FILE, NULL, GetLastError());

    sizeLow = GetFileSize(g_AudioSf2File, &sizeHigh);

    if (sizeLow == INVALID_FILE_SIZE && GetLastError() != NO_ERROR)
        return AudioSf2Fail(AUDIO_SF2_STEP_FILE_SIZE, NULL, GetLastError());

    if (sizeHigh)
        return AudioSf2Fail(AUDIO_SF2_STEP_FILE_SIZE, NULL, ERROR_FILE_TOO_LARGE);

    g_AudioSf2Stats.FileBytes = sizeLow;
    g_AudioSf2Mapping = CreateFileMappingA(g_AudioSf2File, NULL, PAGE_READONLY, 0, 0, NULL);

    if (!g_AudioSf2Mapping)
        return AudioSf2Fail(AUDIO_SF2_STEP_CREATE_MAPPING, NULL, GetLastError());

    /* [CAUTION]: ONE CONTIGUOUS VIEW OF THE WHOLE FILE. A 135 MB font needs 135 MB of free,
     * unbroken address space in a 32-bit process that also holds the guest's memory; if it is
     * not there, this step fails, says so, and MIDI falls back to midiOut.
     */
    g_AudioSf2View = (PCBYTE)MapViewOfFile(g_AudioSf2Mapping, FILE_MAP_READ, 0, 0, 0);

    if (!g_AudioSf2View)
        return AudioSf2Fail(AUDIO_SF2_STEP_MAP_VIEW, NULL, GetLastError());

    result = Sf2FontParse(&g_AudioSf2Font, g_AudioSf2View, sizeLow);

    if (result != SF2_PARSE_OK)
        return AudioSf2Fail(AUDIO_SF2_STEP_PARSE, Sf2FontErrorText(result), (DWORD)result);

    g_AudioSf2Stats.Presets = Sf2FontPresetCount(&g_AudioSf2Font);
    g_AudioSf2Stats.Instruments = Sf2FontInstrumentCount(&g_AudioSf2Font);
    g_AudioSf2Stats.Samples = Sf2FontSampleCount(&g_AudioSf2Font);

    if (!g_AudioSf2Stats.Presets)
        return AudioSf2Fail(AUDIO_SF2_STEP_NO_PRESETS, NULL, 0);

    Sf2SynthInitialize(&g_AudioSf2Synth, &g_AudioSf2Font, g_AudioSf2Hz);
    return TRUE;
}

static VOID AudioSf2Close(VOID)
{
    if (g_AudioSf2View)
        UnmapViewOfFile(g_AudioSf2View);

    if (g_AudioSf2Mapping)
        CloseHandle(g_AudioSf2Mapping);

    if (g_AudioSf2File != INVALID_HANDLE_VALUE)
        CloseHandle(g_AudioSf2File);

    g_AudioSf2View = NULL;
    g_AudioSf2Mapping = NULL;
    g_AudioSf2File = INVALID_HANDLE_VALUE;
}

/* Read one byte of every page of the sample data, so the pages are resident before a note
 * needs them: a page fault inside Render is a disk read inside the mixer's deadline.
 */
static VOID AudioSf2Warm(VOID)
{
    SYSTEM_INFO system;
    PCBYTE samples = (PCBYTE)g_AudioSf2Font.Samples;
    DWORD sampleBytes = g_AudioSf2Font.SampleCount * sizeof(INT16);
    volatile BYTE sink = 0;
    DWORD offset;

    GetSystemInfo(&system);

    for (offset = 0; offset < sampleBytes; offset += system.dwPageSize)
        sink ^= samples[offset];

    (VOID)sink;
}

static DWORD WINAPI AudioSf2StartThread(LPVOID parameter)
{
    BOOL isOpen;
    DWORD readyMs;

    (VOID)parameter;
    isOpen = AudioSf2Open();
    readyMs = GetTickCount();
    g_AudioSf2Stats.StartMs = readyMs - g_AudioSf2WakeMs;

    if (!isOpen)
    {
        AudioSf2Close();
        EnterCriticalSection(&g_AudioSf2Lock);
        g_AudioSf2Stats.Dropped += g_AudioSf2Stats.Messages + g_AudioSf2Stats.SysEx;    /* nowhere to send them */
        g_AudioSf2QueueLength = 0;
        InterlockedExchange(&g_AudioSf2State, AUDIO_SF2_FAILED);
        g_AudioSf2Stats.State = AUDIO_SF2_FAILED;
        LeaveCriticalSection(&g_AudioSf2Lock);
        return 0;
    }

    /* Ready before warm: the queue starts playing now, and a note that beats the warm-up to a
     * page faults it in as it would have anyway.
     */
    InterlockedExchange(&g_AudioSf2State, AUDIO_SF2_READY);
    g_AudioSf2Stats.State = AUDIO_SF2_READY;
    AudioSf2Warm();
    g_AudioSf2Stats.WarmMs = GetTickCount() - readyMs;
    return 0;
}

/* the API: */

VOID AudioSf2Prepare(DWORD sampleHz, PCSTR path)
{
    InitializeCriticalSection(&g_AudioSf2Lock);
    g_AudioSf2Hz = sampleHz;
    lstrcpynA(g_AudioSf2Path, path, sizeof(g_AudioSf2Path));
}

VOID AudioSf2Wake(VOID)
{
    HANDLE thread;
    LONG previous;

    if (!g_AudioSf2Hz)
        return;

    previous = InterlockedCompareExchange(&g_AudioSf2State, AUDIO_SF2_STARTING, AUDIO_SF2_IDLE);

    if (previous != AUDIO_SF2_IDLE)
        return;

    g_AudioSf2Stats.State = AUDIO_SF2_STARTING;
    g_AudioSf2WakeMs = GetTickCount();
    thread = CreateThread(NULL, 0, AudioSf2StartThread, NULL, 0, NULL);

    if (!thread)
    {
        AudioSf2Fail(AUDIO_SF2_STEP_CREATE_THREAD, NULL, GetLastError());
        InterlockedExchange(&g_AudioSf2State, AUDIO_SF2_FAILED);
        g_AudioSf2Stats.State = AUDIO_SF2_FAILED;
        return;
    }

    SetThreadPriority(thread, THREAD_PRIORITY_BELOW_NORMAL);   /* never ahead of the guest */
    CloseHandle(thread);
}

BOOL AudioSf2Short(DWORD message)
{
    if (g_AudioSf2State == AUDIO_SF2_IDLE)
        AudioSf2Wake();

    if (g_AudioSf2State == AUDIO_SF2_FAILED)
        return FALSE;

    EnterCriticalSection(&g_AudioSf2Lock);

    if (g_AudioSf2State != AUDIO_SF2_FAILED)
    {
        AudioSf2Queue(AUDIO_SF2_QUEUE_SHORT, (const BYTE *)&message, sizeof(message));
        g_AudioSf2Stats.Messages++;
    }

    LeaveCriticalSection(&g_AudioSf2Lock);
    return g_AudioSf2State != AUDIO_SF2_FAILED;
}

BOOL AudioSf2Long(const BYTE *message, DWORD length)
{
    if (g_AudioSf2State == AUDIO_SF2_IDLE)
        AudioSf2Wake();

    if (g_AudioSf2State == AUDIO_SF2_FAILED)
        return FALSE;

    EnterCriticalSection(&g_AudioSf2Lock);

    if (g_AudioSf2State != AUDIO_SF2_FAILED)
    {
        AudioSf2Queue(AUDIO_SF2_QUEUE_LONG, message, length);
        g_AudioSf2Stats.SysEx++;
    }

    LeaveCriticalSection(&g_AudioSf2Lock);
    return g_AudioSf2State != AUDIO_SF2_FAILED;
}

VOID AudioSf2Silence(VOID)
{
    DWORD channel;

    if (g_AudioSf2State != AUDIO_SF2_READY && g_AudioSf2State != AUDIO_SF2_STARTING)
        return;

    for (channel = 0; channel < AUDIO_SF2_MIDI_CHANNELS; ++channel)
    {
        DWORD control = AUDIO_SF2_STATUS_CONTROL | channel;

        AudioSf2Short(control | (AUDIO_SF2_CC_SUSTAIN << AUDIO_SF2_DATA1_SHIFT));
        AudioSf2Short(control | (AUDIO_SF2_CC_ALL_SOUND_OFF << AUDIO_SF2_DATA1_SHIFT));
        AudioSf2Short(control | (AUDIO_SF2_CC_ALL_NOTES_OFF << AUDIO_SF2_DATA1_SHIFT));
        AudioSf2Short(control | (AUDIO_SF2_CC_RESET_CONTROLLERS << AUDIO_SF2_DATA1_SHIFT));
    }
}

BOOL AudioSf2Render(INT16 *output, DWORD frames)
{
    PCSF2_SYNTH_STATS synthStats = &g_AudioSf2Synth.Stats;
    LARGE_INTEGER before;
    LARGE_INTEGER after;
    LONGLONG ticks;
    DWORD drainLength;
    BOOL hasSound;

    if (g_AudioSf2State != AUDIO_SF2_READY || g_AudioSf2IsStopped)
        return FALSE;

    /* The queue is copied out under the lock and played outside it: the exec thread can keep
     * queueing while the synth renders.
     */
    EnterCriticalSection(&g_AudioSf2Lock);
    drainLength = g_AudioSf2QueueLength;
    CopyMemory(g_AudioSf2Drain, g_AudioSf2Queue, drainLength);
    g_AudioSf2QueueLength = 0;
    LeaveCriticalSection(&g_AudioSf2Lock);

    QueryPerformanceCounter(&before);
    AudioSf2Play(g_AudioSf2Drain, drainLength);
    hasSound = Sf2SynthRender(&g_AudioSf2Synth, output, frames);
    QueryPerformanceCounter(&after);

    ticks = after.QuadPart - before.QuadPart;
    g_AudioSf2Stats.RenderCalls++;
    g_AudioSf2Stats.RenderTicks += ticks;

    if (ticks > g_AudioSf2Stats.RenderTicksWorst)
        g_AudioSf2Stats.RenderTicksWorst = ticks;

    g_AudioSf2Stats.NotesOn = synthStats->NotesOn;
    g_AudioSf2Stats.NotesSilent = synthStats->NotesSilent;
    g_AudioSf2Stats.VoicesPeak = synthStats->VoicesPeak;
    g_AudioSf2Stats.VoicesStolen = synthStats->VoicesStolen;
    return hasSound;
}

VOID AudioSf2Stop(VOID)
{
    InterlockedExchange(&g_AudioSf2IsStopped, TRUE);
}

PCAUDIO_SF2_STATS AudioSf2Stats(VOID)
{
    return &g_AudioSf2Stats;
}

PCSTR AudioSf2Path(VOID)
{
    return g_AudioSf2Path;
}
