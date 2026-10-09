/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * Host audio + MIDI output for the sound stack (XP waveOut/midiOut).
 *
 * The only part of the sound epic that touches Windows. winmm is bound at runtime
 * with LoadLibrary/GetProcAddress, the same way present_ddraw.c binds DirectDraw,
 * so the host keeps its short import list and a machine without audio simply gets
 * no sound instead of failing to start.
 *
 * IMPORTANT: the pump thread runs even when the sound card cannot be opened. The
 * mixer is not just the audible path -- it is what walks the Sound Blaster's DMA
 * buffer and raises the block-completion IRQ a game waits on. If a failure to
 * open waveOut stopped the pump, every SB game would hang on a silent machine.
 * So on failure we keep calling the fill callback at real-time pace and discard
 * the samples.
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#ifndef NTVDMEX_AUDIO_WAVE_H
#define NTVDMEX_AUDIO_WAVE_H

#include <windows.h>
#include <stdint.h>
#include "audio_format.h"   /* defines only: AUDIO_STEREO_CHANNELS */

#define AUDIO_WAVE_BUFFERS          24      /* CAP on buffers in flight (storage is sized to it) */
#define AUDIO_WAVE_FRAMES           512     /* CAP on frames per buffer */

/* [CAUTION]: THE CAP AND THE DEFAULT MUST BE SEPARATE CONSTANTS. They used to be one: the
 * clamp read `if (want_bufs < 2) want_bufs = AUDIO_WAVE_BUFFERS;`, i.e. "0 means use them
 * all", which was 6 and therefore also the default. Raising the cap to 24 without
 * splitting these would have silently moved the DEFAULT lead from 70 ms to 280 ms
 * and every later run would have been measuring a different machine.
 */
#define AUDIO_WAVE_DEFAULT_BUFFERS  6       /* Default lead = 6 x 512 / 44100 = ~70 ms */
#define AUDIO_WAVE_DEFAULT_FRAMES   512     /* Default step = 11.6 ms per mixer burst */
#define AUDIO_WAVE_HEADER_BYTES     32      /* WAVEHDR is 32 bytes on win32 */
#define AUDIO_MIDI_NAME_LENGTH      32      /* MIDIOUTCAPSA.szPname: MAXPNAMELEN */
#define AUDIO_WAVE_MIN_FRAMES       64      /* Below this the per-buffer callback overhead wins */

/* -- [CAUTION] THE BUFFER SIZE IS THE DMA POSITION'S GRANULARITY, WHICH IS A SEPARATE
 * SUSPECT FROM THE LEAD. --------------------------------------------------------
 * The guest's DMA read pointer ONLY advances while the mixer runs, and the mixer
 * runs one whole waveOut buffer at a time. At 512 frames that is 241 source bytes
 * -- 0.94 of a 256-byte block -- consumed in ONE burst every 11.6 ms, after which
 * the position is frozen. DMX polls it every ~7.1 ms and steers its refills by it,
 * so it sees `0, 0, +241` where real hardware would creep ~148 bytes per poll: a
 * staircase where the hardware gives a ramp. Its writes then land in the wrong
 * block, which is what the ring shows -- 20% of blocks silent, 30% of the rest a
 * verbatim repeat of the previous lap.
 * - SMALLER BUFFERS, MORE OF THEM, SAME TOTAL LEAD. FrameCount x BufferCount is what matters
 *   for the lead; FrameCount alone is the granularity. Making both runtime lets ONE
 *   binary produce baseline and treatment with no rebuild, so the comparison cannot
 *   be confounded by anything else that changed -- and lets the difference be A/B'd
 *   by ear. 128 frames x 24 buffers is the same 70 ms lead at a quarter of the step.
 */
/* - THE BUFFER COUNT IS THE AUDIO LEAD, AND THE LEAD IS A SUSPECT. Every queued
 * buffer is 11.6 ms that our DMA read pointer runs AHEAD of what is audible, and
 * the guest has to refill a block before we reach it. Doom's longest protected-mode
 * stretch with no host turn was measured at 62.8 ms against a 70 ms lead -- close
 * enough that the lead cannot be assumed innocent. So it is a RUNTIME field, set
 * from awbufs.txt before the pump starts, and the SB's replay counter is scored
 * against it: change one number, read one number back. Clamped to [2, AUDIO_WAVE_BUFFERS];
 * 0 means "use them all".
 */

/* Fill `frames` mono 16-bit samples. Called on the audio thread; the host wraps
 * it in the same lock the exec thread uses for the device bus.
 */
typedef VOID (*PAUDIO_WAVE_FILL_ROUTINE)(PVOID context, INT16 *output, UINT32 frames);

typedef struct _AUDIO_WAVE
{
    HMODULE   Module;
    HANDLE    Thread, Event;
    volatile LONG IsRunning;
    PVOID WaveOut;                      /* HWAVEOUT, opaque here */
    PVOID MidiOut;                      /* HMIDIOUT, opaque here */
    UINT32  SampleHz;
    PAUDIO_WAVE_FILL_ROUTINE Fill; PVOID Context;
    INT       IsSilent;                 /* 1 = no device; pump but discard */
    INT      IsForcedSilent;  /* s90 #132: safe mode -- never open a device; the pump still runs */
    UINT32  Underruns;
    /* [CAUTION]: `Underruns` COUNTS waveOutWrite FAILURES, WHICH IS NOT STARVATION (Importance =
     * 1): It has read 0 in every run ever made, including runs the user describes as audibly
     * broken, because waveOutWrite does not fail when the QUEUE runs dry -- it succeeds, having
     * been called too late. When the last queued buffer finishes before we hand the driver the next
     * one, the DEVICE plays silence, and that is a gap at the speaker that nothing here can see.
     * Every audio counter in this project measures either the guest's ring or our sample stream;
     * neither is what reaches the ear.
     * - COUNT THE QUEUE DEPTH INSTEAD, AND COUNT IT AT ITS WORST. On each pass, how
     *   many buffers has the driver already handed back? That many are NOT queued. If
     *   it equals BufferCount the queue was completely empty and the device definitely ran
     *   dry. The histogram gives the MARGIN rather than a pass/fail: `drain` sitting at
     *   BufferCount-1 means we are one buffer from silence the whole time, which a "starved=0"
     *   would have reported as healthy.
     */
    UINT32  Starved;                    /* passes with EVERY buffer handed back */
    UINT32  DrainMax;                   /* worst simultaneous handed-back count */
    UINT32  DrainHistogram[AUDIO_WAVE_BUFFERS + 1];
    /* [CAUTION]: THE DEVICE HAS ITS OWN VOLUME, AND IT IS NOT OURS (Importance = 1):
     * Everything this struct measures can be perfect -- buffers written,
     * handed back, never starved -- while the machine is silent, because the
     * WAVE slider in Windows' own volume control attenuates AFTER us. That is
     * indistinguishable from a broken mixer in every counter we had, so ASK
     * THE DRIVER and print the answer. 0xFFFF is full scale per channel.
     *
     * [CAUTION]: We only READ it. Turning a user's volume up because our test wants to be
     * heard is not a fix, it is a surprise.
     */
    UINT32  DeviceVolume;               /* waveOutGetVolume: right<<16 | left */
    INT       IsDeviceVolumeKnown;      /* 0 = the driver would not tell us */
    UINT32  BufferCount;                /* buffers actually queued: the LEAD */
    UINT32  FrameCount;                 /* frames per buffer: the GRANULARITY */
    /* #234 (docs/EMULATION.md): WinMM or DirectSound. Set by the caller BEFORE
     * AudioWaveStart (preserved across its zeroing, like BufferCount); `IsUsingDirectSound` says
     * which one actually opened -- DirectSound falls back to WinMM if it will not.
     */
    INT       WantsDirectSound;
    INT       IsUsingDirectSound;
    PVOID DirectSound, DirectSoundBuffer;                 /* IDirectSound, IDirectSoundBuffer */
    UINT32  DirectSoundBytes, DirectSoundWritePosition;        /* ring size, next byte we write */
    /* #136: Settings > Audio > MIDI (MIDI_ROUTE_*, midi_route.h). Set by the caller
     * BEFORE AudioWaveStart and preserved across its zeroing, like WantsDirectSound. 0 = Host
     * GM = device 0 without enumerating, which is every build so far. The rest is what
     * happened, for the log: the device opened (-1 = none), whether it is an EXTERNAL
     * synth that was found by name (=> it gets SysEx), how many devices there were,
     * and the opened device's name.
     */
    INT       MidiChoice;
    INT       MidiDevice;
    INT       IsMidiExternal;
    UINT32  MidiDeviceCount;
    char      MidiName[AUDIO_MIDI_NAME_LENGTH];         /* `char`, not CHAR: CHAR here moved code in AudioWaveStart (s93) */
    UINT32  SysExSent, SysExDropped;

    /* WAVEHDR + sample storage, allocated inline to avoid a heap dependency */
    BYTE Headers[AUDIO_WAVE_BUFFERS][AUDIO_WAVE_HEADER_BYTES];  /* WAVEHDR is 32 bytes on win32 */
    INT16   Buffers[AUDIO_WAVE_BUFFERS][AUDIO_WAVE_FRAMES * AUDIO_STEREO_CHANNELS];   /* interleaved L/R */
} AUDIO_WAVE, *PAUDIO_WAVE;
typedef const AUDIO_WAVE *PCAUDIO_WAVE;

/* Start the audio pump. Returns 0 on success, 1 if it fell back to silent pumping
 * (still a success as far as the guest is concerned).
 */
INT  AudioWaveStart(_Inout_ PAUDIO_WAVE wave, _In_ UINT32 sampleHz, _In_ PAUDIO_WAVE_FILL_ROUTINE fill, _In_opt_ PVOID context);
VOID AudioWaveStop(_Inout_ PAUDIO_WAVE wave);

/* Send one packed MIDI short message (status | d1<<8 | d2<<16) to the host synth.
 * Safe to call when MIDI never opened -- it is simply dropped.
 */
VOID AudioWaveMidi(_In_ PAUDIO_WAVE wave, _In_ UINT32 message);
/* #136: one complete SysEx message (F0 .. F7) to the host synth, through midiOutLongMsg.
 * Only ever wired for an external synth (wave->IsMidiExternal). Never blocks: the buffer is
 * copied into one of a few slots, and a message that finds every slot still queued in
 * the driver is dropped and counted (SysExDropped) rather than waited for, because
 * this runs on the exec thread inside a port trap.
 */
VOID AudioWaveMidiLong(_Inout_ PAUDIO_WAVE wave, _In_reads_(length) const BYTE *message, _In_ UINT32 length);
/* #214: silence the host synth -- sustain up, all sound/notes off, controllers reset on
 * all 16 channels, then midiOutReset. Called whenever a program is torn down.
 */
VOID AudioWaveMidiSilence(_In_ PAUDIO_WAVE wave);

/* Record exactly what reaches waveOut to a mono 16-bit .WAV (s81; see audio_rec.h).
 * start: 0 = recording, -1 = already recording / cannot create. stop: samples written.
 */
INT      AudioWaveRecordStart(_In_ PCSTR path, _In_ UINT32 sampleHz);
UINT32 AudioWaveRecordStop(VOID);
INT      AudioWaveIsRecording(VOID);
UINT32 AudioWaveRecordDropped(VOID);

#endif /* NTVDMEX_AUDIO_WAVE_H */
