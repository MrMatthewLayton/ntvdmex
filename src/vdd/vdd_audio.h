/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * The audio mixer: OPL + Sound Blaster -> one output stream.
 *
 * This is the piece that makes the sound devices actually do anything. Each
 * device renders at its OWN rate -- the OPL at the chip's native 49716 Hz so its
 * phase arithmetic stays exact, the SB at whatever rate the game programmed
 * (commonly 11025 or 22050) -- and neither matches the host's output rate. The
 * mixer resamples both onto a common clock and sums them.
 *
 * It is also the TRANSPORT, not just a nicety: VddSbRender() is what walks the
 * DMA buffer and raises the block-completion IRQ a game waits on. Until something
 * pulls samples through here, a game programs a transfer and hangs forever. So
 * the mixer must keep being called even when nothing is audible.
 *
 * Resampling is linear interpolation on a 16.16 fixed-point position. That is
 * good enough for 8-bit DOS audio and FM, and cheap; the alternative (a windowed
 * sinc) would be inaudible improvement on material this band-limited.
 *
 * Pure C, no <windows.h>: the host sink lives in audio_wave.c, so the whole mixer
 * is exercised off-VM by tests/unit/audio_test.c with no sound card involved.
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#ifndef NTVDMEX_VDD_AUDIO_H
#define NTVDMEX_VDD_AUDIO_H

#include "vdd_opl.h"
#include "vdd_sb.h"
#include "vdd_gus.h"
#include "vdd_emu8k.h"
#include "vdd_speaker.h"

#define AUDIO_OUTPUT_HZ     44100u  /* Host output rate */
#define AUDIO_CHUNK         512u    /* output frames the mixer works in */

/* Worst-case source frames for one chunk: the OPL's 49716 Hz is the fastest
 * source, plus a couple of samples of interpolation headroom.
 */
#define AUDIO_SOURCE_MAX    (AUDIO_CHUNK * 2u + 4u)

/* A linear-interpolating resampler from `SourceHz` to the output rate. */
typedef struct _AUDIO_RESAMPLER
{
    UINT32 SourceHz;
    UINT32 Step;            /* (SourceHz << 16) / OutputHz */
    UINT32 Fraction;            /* 16.16 position between Previous and Current */
    INT32  Previous, Current; /* the two source samples being interpolated */
    INT32  PreviousRight, CurrentRight;   /* #189: ...and the right channel's, for a stereo source */
    INT      IsPrimed;
} AUDIO_RESAMPLER, *PAUDIO_RESAMPLER;
typedef const AUDIO_RESAMPLER *PCAUDIO_RESAMPLER;

/* THE PC SPEAKER IS A THIRD SOURCE, AND IT USED TO BE SILENT:
 * vdd_speaker.c models port 0x61 and reports the tone, and nothing ever turned
 * that into a sample -- so every beep a guest made went nowhere while the score
 * line said "SB16 PCM, OPL2/3 FM, MPU-401, speaker". It is a square wave gated
 * by two bits, which is exactly what the hardware does, so it is a dozen lines
 * here rather than a device of its own. Amplitude is deliberately WELL below
 * full scale: a real speaker is a 1-inch cone, not a line output, and a
 * full-scale square would sit on the clip rail over everything else.
 */
#define AUDIO_SPEAKER_LEVEL     6000    /* Peak sample for an active speaker tone */
#define AUDIO_SPEAKER_HZ_MIN    20u     /* Below this it is a click train, not a tone */
#define AUDIO_SPEAKER_HZ_MAX    20000u  /* Above it, nothing at 44.1 kHz is audible */

typedef struct _AUDIO_STATE
{
    POPL_STATE Opl;
    PSB_STATE  Sb;
    PGUS_STATE Gus;           /* Gravis UltraSound; NULL = not fitted (s80) */
    PEMU8K_STATE Emu8k;      /* AWE32 EMU8000 wavetable; NULL = not fitted (#233) */
    PCSPEAKER_STATE Speaker;  /* PC speaker; NULL = not fitted */
    UINT32   OutputHz;
    AUDIO_RESAMPLER OplResampler, SbResampler, GusResampler, Emu8kResampler;
    /* Speaker phase as a 16-bit fraction of one cycle, clocked at OutputHz. The
     * top bit IS the half-cycle, so the sample is one test and no branch on the
     * frequency; it persists across calls so a held tone does not restart (and
     * click) at every chunk boundary.
     */
    WORD   SpeakerPhase;
    INT32    SpeakerLevel;    /* peak amplitude; 0 = speaker switched off */
    /* Master attenuator, applied AFTER the sum -- the position a volume control
     * occupies on a real machine, so a game's own mixer settings still work
     * underneath it. 0..100; `IsMuted` is separate so muting does not lose it.
     */
    UINT32   Master;
    INT        IsMuted;
    INT16    Scratch[AUDIO_STEREO_CHANNELS * AUDIO_SOURCE_MAX];  /* #189: room for interleaved L/R */
    UINT32   FramesMixed;     /* diagnostics: total output frames produced */
    /* -- AND WHAT THE SPEAKER PATH ACTUALLY DID, BECAUSE "I HEARD NOTHING" HAS
     * FOUR CAUSES AND NO LOG DISTINGUISHED THEM. Counted where the decision is
     * made, so each one separates a different failure:
     * SpeakerGated  -- frames where port 0x61 said SOUNDING
     * SpeakerFrames -- frames actually emitted (gated AND a usable frequency)
     * SpeakerHz     -- the last frequency asked for, in Hz
     * gated=0 means the guest's port writes never reached this struct;
     * gated>0 with frames=0 means the frequency was refused; both non-zero
     * means we produced samples and the fault is downstream of the mixer.
     */
    UINT32   SpeakerGated, SpeakerFrames, SpeakerHz;
} AUDIO_STATE, *PAUDIO_STATE;
typedef const AUDIO_STATE *PCAUDIO_STATE;

/* Set up the mixer for its two sources. Safe to call again after a device's rate
 * changes; the resamplers re-derive their step from the device on each mix.
 * Leaves the speaker unfitted, the master volume at 100 and unmuted.
 */
VOID VddAudioInitialize(_Out_ PAUDIO_STATE state, _In_opt_ POPL_STATE opl, _In_opt_ PSB_STATE soundBlaster, _In_ UINT32 outputHz);

/* Fit (or unfit) the PC speaker. `isEnabled` 0 leaves the VDD on the bus -- port
 * 0x61 must keep answering, guests time delay loops off its refresh bit -- and
 * only stops it being audible.
 */
VOID VddAudioSetSpeaker(_Inout_ PAUDIO_STATE state, _In_opt_ PCSPEAKER_STATE speaker, _In_ INT isEnabled);
/* Fit (or remove, NULL) the Gravis UltraSound as a mixer source. */
VOID VddAudioSetGus(_Inout_ PAUDIO_STATE state, _In_opt_ PGUS_STATE gus);
/* Fit (or remove, NULL) the AWE32's EMU8000 as a mixer source (#233). */
VOID VddAudioSetEmu8k(_Inout_ PAUDIO_STATE state, _In_opt_ PEMU8K_STATE emu8k);

/* Master volume, 0..100, clamped; `isMuted` outputs silence without losing it. */
VOID VddAudioSetMaster(_Inout_ PAUDIO_STATE state, _In_ UINT32 percent, _In_ INT isMuted);

/* Produce `frames` mono 16-bit samples at OutputHz, pulling from both devices.
 * Always produces exactly `frames` samples (silence when nothing is playing), so
 * a host audio thread can call it unconditionally.
 */
VOID VddAudioMix(_Inout_ PAUDIO_STATE state, _Out_writes_(frames) INT16 *output, _In_ UINT32 frames);
/* #189: the same mix in stereo -- `frames` interleaved L/R pairs (2*frames samples).
 * This is what the host plays; VddAudioMix is this folded to mono.
 */
VOID VddAudioMixStereo(_Inout_ PAUDIO_STATE state, _Out_writes_(AUDIO_STEREO_CHANNELS * frames) INT16 *output, _In_ UINT32 frames);

#endif /* NTVDMEX_VDD_AUDIO_H */
