/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * The built-in General MIDI synth: Windows' own DirectMusic software synth, rendering into
 * OUR mixer. (GH #155)
 *
 * WHY IT IS HOSTED, NOT SENT TO midiOut (Importance = 2):
 * Through midiOut, MIDI music goes to the GS Wavetable synth inside Windows' kernel mixer:
 * past our master volume, and out of reach of the recorder, which taps OUR mix. Hosting
 * the same synth through a synth sink of our own makes the music one more source in the
 * mixer: recorded, attenuated and muted with everything else, and clocked by our audio.
 * - XP ships the synth (CLSID_DirectMusicSynth) and the GS sound set it plays
 *   (GUID_DefaultGMCollection), so it sounds as the midiOut default did.
 * - Measured on the rig (s96 spike, runs/s96/dmspike): 235 instruments download in
 *   277 ms; a busy GM score renders at 0.3-0.5% of one CPU, 200 us worst per 10 ms.
 *
 * HOW IT IS DRIVEN:
 * - Started LAZILY, on a background thread, when the guest first puts an MPU-401 into
 *   UART mode (or first writes the GUS's MIDI UART): a program that never plays MIDI never
 *   pays the download. Messages that arrive while it starts are queued and sent once it
 *   is ready.
 * - The exec thread only PACKS messages, stamped with the time of the next sample to be
 *   rendered; the audio thread hands them to the synth and renders. So the exec thread
 *   never waits on DirectMusic.
 * - If it cannot start, every call says so (returns FALSE) and the host falls back to
 *   midiOut, as before #155.
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#ifndef NTVDMEX_AUDIO_DMSYNTH_H
#define NTVDMEX_AUDIO_DMSYNTH_H

#include "../ntvdmex_types.h"

enum
{
    AUDIO_DMSYNTH_IDLE = 0, AUDIO_DMSYNTH_STARTING, AUDIO_DMSYNTH_READY, AUDIO_DMSYNTH_FAILED
};

/* What the synth did, for the STAGE2 report. */
typedef struct _AUDIO_DMSYNTH_STATS
{
    LONG   State;               /* AUDIO_DMSYNTH_* */
    PCSTR  FailedStep;          /* the step that failed, or NULL */
    LONG   FailedResult;        /* its HRESULT */
    DWORD  StartMs;             /* wake -> ready */
    DWORD  Instruments;         /* GS instruments downloaded */
    DWORD  Voices;              /* voices the port granted */
    DWORD  Messages;            /* short messages packed */
    DWORD  SysEx;               /* SysEx messages packed */
    DWORD  Queued;              /* arrived while starting, sent when ready */
    DWORD  Dropped;             /* lost: queue full, or the synth failed with them queued */
    DWORD  RenderCalls;
    LONGLONG RenderTicks;       /* QueryPerformanceCounter ticks spent in Render */
    LONGLONG RenderTicksWorst;
} AUDIO_DMSYNTH_STATS, *PAUDIO_DMSYNTH_STATS;
typedef const AUDIO_DMSYNTH_STATS *PCAUDIO_DMSYNTH_STATS;

/* Before anything else: the rate the mixer runs at, which the synth renders at. No COM, no thread. */
VOID AudioDmSynthPrepare(_In_ DWORD sampleHz);

/* The guest is about to play MIDI: start the synth on a background thread, once. */
VOID AudioDmSynthWake(VOID);

/* One short message (status | data1 << 8 | data2 << 16). FALSE = the synth failed: use midiOut. */
BOOL AudioDmSynthShort(_In_ DWORD message);

/* One whole SysEx message, F0 .. F7. FALSE = the synth failed: use midiOut. */
BOOL AudioDmSynthLong(_In_reads_(length) const BYTE *message, _In_ DWORD length);

/* Every note off, every controller reset, on all 16 channels (as AudioWaveMidiSilence). */
VOID AudioDmSynthSilence(VOID);

/* Render `frames` interleaved stereo frames into `output` (overwritten). FALSE = not ready:
 * nothing written, and nothing to mix.
 */
BOOL AudioDmSynthRender(_Out_writes_(frames * 2) INT16 *output, _In_ DWORD frames);

/* At exit: stop the port. */
VOID AudioDmSynthStop(VOID);

PCAUDIO_DMSYNTH_STATS AudioDmSynthStats(VOID);

#endif /* NTVDMEX_AUDIO_DMSYNTH_H */
