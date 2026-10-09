/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * Record what the host plays, to a .WAV file.  (s81)
 *
 * WHY. Two jobs, one mechanism. Tools > Capture > Record Audio is a product feature the
 * user asked for; and "Doom's quit sound is glitchy" is a claim about what reaches the
 * speaker that no counter in this project can see -- every audio statistic measures the
 * guest's ring or our sample stream in aggregate, never the waveform itself. A recording
 * of the exact samples handed to waveOut makes it something to look at.
 *
 * SHAPE. The mixer's output is mono 16-bit at the device rate (audio_wave.c opens it that
 * way), and it is fed from the audio thread, which runs at TIME_CRITICAL on an ~11.6 ms
 * deadline. File I/O does not belong there: a disk-cache flush would become an audible
 * underrun only while recording, the one time someone is listening closely. So the audio
 * thread only copies into a lock-free single-producer/single-consumer ring, and a writer
 * thread at normal priority drains it to disk. A full ring DROPS samples and counts them,
 * rather than blocking the audio thread; `Dropped` is reported so a recording with holes
 * says so.
 *
 * One recorder per process -- so this header is included by ONE translation unit only,
 * audio_wave.c (the feeder), which exports AudioWaveRecord* for everyone else. Including it a
 * second time would silently create a second, unfed recorder.
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#ifndef NTVDMEX_AUDIO_REC_H
#define NTVDMEX_AUDIO_REC_H

#include <windows.h>
#include <stdint.h>
#include "audio_format.h"   /* defines only: AUDIO_STEREO_CHANNELS */

/* The canonical 44-byte PCM .WAV header (AudioRecorderHeader). */
#define AUDIO_RECORDER_HEADER_BYTES         44
#define AUDIO_RECORDER_TAG_BYTES            16          /* "RIFF....WAVEfmt " */
#define AUDIO_RECORDER_RIFF_SIZE_OFFSET     4
#define AUDIO_RECORDER_RIFF_SIZE_BASE       36          /* The header less "RIFF" and its size */
#define AUDIO_RECORDER_FMT_SIZE_OFFSET      16
#define AUDIO_RECORDER_FMT_CHUNK_BYTES      16
#define AUDIO_RECORDER_FORMAT_OFFSET        20
#define AUDIO_RECORDER_FORMAT_PCM           1
#define AUDIO_RECORDER_CHANNELS_OFFSET      22
#define AUDIO_RECORDER_RATE_OFFSET          24
#define AUDIO_RECORDER_BYTE_RATE_OFFSET     28
#define AUDIO_RECORDER_BLOCK_ALIGN_OFFSET   32
#define AUDIO_RECORDER_FRAME_BYTES          4           /* 16-bit L+R */
#define AUDIO_RECORDER_BITS_OFFSET          34
#define AUDIO_RECORDER_BITS_PER_SAMPLE      16
#define AUDIO_RECORDER_DATA_SIZE_OFFSET     40
#define AUDIO_RECORDER_DEFAULT_HZ           44100
#define AUDIO_RECORDER_DRAIN_MS             50
#define AUDIO_RECORDER_STOP_TIMEOUT_MS      2000
#define AUDIO_RECORDER_RING                 (1u << 18)  /* 262144 samples: ~3 s of stereo at 44.1 kHz */

typedef struct _AUDIO_RECORDER
{
    HANDLE          File, Thread;
    volatile LONG   IsActive;                /* 1 = the audio thread may feed */
    volatile LONG   IsStopping;              /* 1 = writer: drain, finish, exit */
    volatile LONG   Head, Tail;              /* ring indices (mod AUDIO_RECORDER_RING) */
    UINT32        SampleHz;
    UINT32        DataBytes;                 /* written to disk so far */
    UINT32        Dropped;                   /* samples lost to a full ring */
    INT16         Ring[AUDIO_RECORDER_RING];
} AUDIO_RECORDER, *PAUDIO_RECORDER;
typedef const AUDIO_RECORDER *PCAUDIO_RECORDER;

static AUDIO_RECORDER g_AudioRecorder;

static VOID AudioRecorderPut32(BYTE *bytes, UINT32 value)
{
    bytes[0] = (BYTE)value;
    bytes[1] = (BYTE)(value >> 8);
    bytes[2] = (BYTE)(value >> 16);
    bytes[3] = (BYTE)(value >> 24);
}

static VOID AudioRecorderPut16(BYTE *bytes, UINT32 value)
{
    bytes[0] = (BYTE)value;
    bytes[1] = (BYTE)(value >> 8);
}

/* The canonical 44-byte PCM header; sizes are patched when the recording stops. */
static VOID AudioRecorderHeader(
    BYTE header[AUDIO_RECORDER_HEADER_BYTES],
    UINT32 sampleHz,
    UINT32 dataBytes)
{
    static const CHAR tag[] = "RIFF....WAVEfmt ";
    INT byteIndex;

    for (byteIndex = 0; byteIndex < AUDIO_RECORDER_TAG_BYTES; ++byteIndex)
        header[byteIndex] = (BYTE)tag[byteIndex];
    AudioRecorderPut32(header + AUDIO_RECORDER_RIFF_SIZE_OFFSET, AUDIO_RECORDER_RIFF_SIZE_BASE + dataBytes);
    AudioRecorderPut32(header + AUDIO_RECORDER_FMT_SIZE_OFFSET, AUDIO_RECORDER_FMT_CHUNK_BYTES);     /* fmt chunk size */
    AudioRecorderPut16(header + AUDIO_RECORDER_FORMAT_OFFSET, AUDIO_RECORDER_FORMAT_PCM);      /* PCM */
    AudioRecorderPut16(header + AUDIO_RECORDER_CHANNELS_OFFSET, AUDIO_STEREO_CHANNELS);      /* stereo (#189) */
    AudioRecorderPut32(header + AUDIO_RECORDER_RATE_OFFSET, sampleHz);
    AudioRecorderPut32(header + AUDIO_RECORDER_BYTE_RATE_OFFSET, sampleHz * AUDIO_RECORDER_FRAME_BYTES);              /* bytes per second */
    AudioRecorderPut16(header + AUDIO_RECORDER_BLOCK_ALIGN_OFFSET, AUDIO_RECORDER_FRAME_BYTES);      /* block align: L+R */
    AudioRecorderPut16(header + AUDIO_RECORDER_BITS_OFFSET, AUDIO_RECORDER_BITS_PER_SAMPLE);     /* bits per sample */
    header[36] = 'd';
    header[37] = 'a';
    header[38] = 't';
    header[39] = 'a';
    AudioRecorderPut32(header + AUDIO_RECORDER_DATA_SIZE_OFFSET, dataBytes);
}

/* Drain whatever is in the ring to the file. Writer thread only. */
static VOID AudioRecorderDrain(PAUDIO_RECORDER recorder)
{
    for (;;)
    {
        LONG tail = recorder->Tail, head = recorder->Head;
        DWORD count, written = 0;
        if (tail == head)
            return;
        count = (DWORD)((head > tail) ? (head - tail) : ((LONG)AUDIO_RECORDER_RING - tail));   /* contiguous run */
        WriteFile(recorder->File, &recorder->Ring[tail], count * sizeof(INT16), &written, NULL);
        recorder->DataBytes += written;
        InterlockedExchange(&recorder->Tail, (LONG)((tail + (LONG)count) & (AUDIO_RECORDER_RING - 1)));
    }
}

static DWORD WINAPI AudioRecorderWriter(LPVOID parameter)
{
    PAUDIO_RECORDER recorder = (PAUDIO_RECORDER)parameter;

    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    while (!recorder->IsStopping)
    {
        AudioRecorderDrain(recorder);
        Sleep(AUDIO_RECORDER_DRAIN_MS);
    }
    AudioRecorderDrain(recorder);
    return 0;
}

static INT AudioRecorderIsActive(VOID)
{
    return g_AudioRecorder.IsActive != 0;
}

/* Start recording to `path`. 0 = started; -1 = already recording or cannot create. */
static INT AudioRecorderStart(PCSTR path, UINT32 sampleHz)
{
    PAUDIO_RECORDER recorder = &g_AudioRecorder;
    BYTE header[AUDIO_RECORDER_HEADER_BYTES];
    DWORD written = 0;

    if (recorder->IsActive || recorder->File)
        return -1;
    recorder->File = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS,
                          FILE_ATTRIBUTE_NORMAL, NULL);
    if (recorder->File == INVALID_HANDLE_VALUE)
    {
        recorder->File = 0;
        return -1;
    }
    recorder->SampleHz = sampleHz ? sampleHz : AUDIO_RECORDER_DEFAULT_HZ;
    recorder->Head = recorder->Tail = 0;
    recorder->DataBytes = 0;
    recorder->Dropped = 0;
    recorder->IsStopping = 0;
    AudioRecorderHeader(header, recorder->SampleHz, 0);
    WriteFile(recorder->File, header, sizeof header, &written, NULL);
    recorder->Thread = CreateThread(NULL, 0, AudioRecorderWriter, recorder, 0, NULL);
    if (!recorder->Thread)
    {
        CloseHandle(recorder->File);
        recorder->File = 0;
        return -1;
    }
    InterlockedExchange(&recorder->IsActive, 1);
    return 0;
}

/* Stop, flush, patch the sizes, close. Safe to call when not recording. Returns the
 * number of samples written (0 if nothing was recording).
 */
static UINT32 AudioRecorderStop(VOID)
{
    PAUDIO_RECORDER recorder = &g_AudioRecorder;
    BYTE header[AUDIO_RECORDER_HEADER_BYTES];
    DWORD written = 0;

    if (!recorder->File)
        return 0;
    InterlockedExchange(&recorder->IsActive, 0);        /* the audio thread stops feeding */
    InterlockedExchange(&recorder->IsStopping, 1);
    WaitForSingleObject(recorder->Thread, AUDIO_RECORDER_STOP_TIMEOUT_MS);
    CloseHandle(recorder->Thread);
    recorder->Thread = 0;
    AudioRecorderDrain(recorder);              /* anything fed after the last pass */
    AudioRecorderHeader(header, recorder->SampleHz, recorder->DataBytes);
    SetFilePointer(recorder->File, 0, NULL, FILE_BEGIN);
    WriteFile(recorder->File, header, sizeof header, &written, NULL);
    CloseHandle(recorder->File);
    recorder->File = 0;
    return recorder->DataBytes / AUDIO_RECORDER_FRAME_BYTES;            /* frames (L/R pairs) */
}

/* Audio thread: copy `n` samples in, or drop them if the ring is full. Never blocks. */
static VOID AudioRecorderFeed(const INT16 *samples, UINT32 count)
{
    PAUDIO_RECORDER recorder = &g_AudioRecorder;
    UINT32 sampleIndex;
    LONG head, tail, room;

    if (!recorder->IsActive)
        return;
    head = recorder->Head;
    tail = recorder->Tail;
    room = (LONG)(AUDIO_RECORDER_RING - 1) - ((head - tail) & (LONG)(AUDIO_RECORDER_RING - 1));
    if ((LONG)count > room)
    {
        recorder->Dropped += count;
        return;
    }
    for (sampleIndex = 0; sampleIndex < count; ++sampleIndex)
        recorder->Ring[(head + (LONG)sampleIndex) & (AUDIO_RECORDER_RING - 1)] = samples[sampleIndex];
    InterlockedExchange(&recorder->Head, (LONG)((head + (LONG)count) & (AUDIO_RECORDER_RING - 1)));
}

#endif
