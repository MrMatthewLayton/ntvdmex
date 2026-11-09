/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * The SoundFont synth, hosted: Settings > Audio > MIDI = SoundFont with a SoundFontPath plays
 * the guest's MIDI through our own SF2 synth (sf2_synth.h), rendering into OUR mixer.
 *
 * WHY OUR OWN SYNTH (Importance = 2):
 * Until now the path row was stored and never read: MIDI = SoundFont only looked for a SoundFont
 * MIDI driver installed in Windows (midi_route.h), and those drivers keep their own list of
 * fonts and take none from us. The user picked .sf2 files and nothing changed. With a file
 * chosen, the font is now ours to play; with no file, the driver search stands, as before.
 * Like the built-in GS synth (audio_dmsynth.h), the music is then one more source in the
 * mixer: recorded, attenuated and muted with everything else.
 *
 * HOW IT IS DRIVEN (the shape of audio_dmsynth.h):
 * - Started LAZILY, on a background thread, when the guest first puts an MPU-401 into UART
 *   mode or first sends MIDI. The file is MAPPED, never read into the heap -- a SoundFont can
 *   be 135 MB on a 32-bit machine -- and parsed in place; then the thread touches every page
 *   of the sample data once, so a first note does not wait on the disk inside the mixer.
 * - Every message is queued, and the audio thread hands the queue to the synth at the start
 *   of each Render. So messages that arrive while the font loads are kept, the exec thread
 *   never waits on the synth, and the synth only ever runs on one thread.
 * - If it cannot start, every call says so (returns FALSE), the log names the step, and the
 *   host falls back to what MIDI = SoundFont did before: the midiOut device midi_route.h
 *   found (a SoundFont driver by name, else device 0).
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#ifndef NTVDMEX_AUDIO_SF2_H
#define NTVDMEX_AUDIO_SF2_H

#include "../ntvdmex_types.h"

enum
{
    AUDIO_SF2_IDLE = 0, AUDIO_SF2_STARTING, AUDIO_SF2_READY, AUDIO_SF2_FAILED
};

/* What the synth did, for the STAGE2 report. */
typedef struct _AUDIO_SF2_STATS
{
    LONG   State;               /* AUDIO_SF2_* */
    PCSTR  FailedStep;          /* the step that failed, or NULL */
    PCSTR  FailedDetail;        /* the parse error's text, or NULL */
    DWORD  FailedError;         /* GetLastError() of a failed Win32 step */
    DWORD  StartMs;             /* wake -> ready (map + parse) */
    DWORD  WarmMs;              /* ready -> every sample page touched */
    DWORD  FileBytes;
    DWORD  Presets;
    DWORD  Instruments;
    DWORD  Samples;
    DWORD  Messages;            /* short messages queued */
    DWORD  SysEx;               /* SysEx messages queued */
    DWORD  Dropped;             /* lost: queue full, or the synth failed with them queued */
    DWORD  NotesOn;
    DWORD  NotesSilent;         /* no preset or zone for them */
    DWORD  VoicesPeak;
    DWORD  VoicesStolen;
    DWORD  RenderCalls;
    LONGLONG RenderTicks;       /* QueryPerformanceCounter ticks spent in Render */
    LONGLONG RenderTicksWorst;
} AUDIO_SF2_STATS, *PAUDIO_SF2_STATS;

typedef const AUDIO_SF2_STATS *PCAUDIO_SF2_STATS;

/* Before anything else: the mixer's rate and the font's path. No file is opened yet. */
VOID AudioSf2Prepare(_In_ DWORD sampleHz, _In_ PCSTR path);

/* The guest is about to play MIDI: load the font on a background thread, once. */
VOID AudioSf2Wake(VOID);

/* One short message (status | data1 << 8 | data2 << 16). FALSE = the synth failed: use midiOut. */
BOOL AudioSf2Short(_In_ DWORD message);

/* One whole SysEx message, F0 .. F7. FALSE = the synth failed. */
BOOL AudioSf2Long(_In_reads_(length) const BYTE *message, _In_ DWORD length);

/* Every note off, every controller reset, on all 16 channels (as AudioWaveMidiSilence). */
VOID AudioSf2Silence(VOID);

/* Render `frames` interleaved stereo frames into `output` (overwritten). FALSE = not ready, or
 * no note sounding: nothing written, and nothing to mix.
 */
BOOL AudioSf2Render(_Out_writes_(frames * 2) INT16 *output, _In_ DWORD frames);

/* At exit, after the mixer has stopped calling Render. */
VOID AudioSf2Stop(VOID);

PCAUDIO_SF2_STATS AudioSf2Stats(VOID);

/* The path Prepare was given (for the log). */
PCSTR AudioSf2Path(VOID);

#endif /* NTVDMEX_AUDIO_SF2_H */
