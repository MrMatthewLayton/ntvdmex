/*
 * vdd_audio.h -- the audio mixer: OPL + Sound Blaster -> one output stream.
 *
 * This is the piece that makes the sound devices actually do anything. Each
 * device renders at its OWN rate -- the OPL at the chip's native 49716 Hz so its
 * phase arithmetic stays exact, the SB at whatever rate the game programmed
 * (commonly 11025 or 22050) -- and neither matches the host's output rate. The
 * mixer resamples both onto a common clock and sums them.
 *
 * It is also the TRANSPORT, not just a nicety: vdd_sb_render() is what walks the
 * DMA buffer and raises the block-completion IRQ a game waits on. Until something
 * pulls samples through here, a game programs a transfer and hangs forever. So
 * the mixer must keep being called even when nothing is audible.
 *
 * Resampling is linear interpolation on a 16.16 fixed-point position. That is
 * good enough for 8-bit DOS audio and FM, and cheap; the alternative (a windowed
 * sinc) would be inaudible improvement on material this band-limited.
 *
 * Pure C, no <windows.h>: the host sink lives in audio_wave.c, so the whole mixer
 * is exercised off-VM by tools/dostest/audio_test.c with no sound card involved.
 */
#ifndef NTVDMEX_VDD_AUDIO_H
#define NTVDMEX_VDD_AUDIO_H

#include "vdd_opl.h"
#include "vdd_sb.h"
#include "vdd_speaker.h"

#define AUDIO_OUT_HZ    44100u    /* host output rate                            */
#define AUDIO_CHUNK      512u     /* output frames the mixer works in            */
/* Worst-case source frames for one chunk: the OPL's 49716 Hz is the fastest
   source, plus a couple of samples of interpolation headroom. */
#define AUDIO_SRC_MAX  (AUDIO_CHUNK * 2u + 4u)

/* A linear-interpolating resampler from `src_hz` to the output rate. */
typedef struct audio_resampler {
    uint32_t src_hz;
    uint32_t step;            /* (src_hz << 16) / out_hz                         */
    uint32_t frac;            /* 16.16 position between prev and cur             */
    int32_t  prev, cur;       /* the two source samples being interpolated       */
    int      primed;
} audio_resampler;

/* ── THE PC SPEAKER IS A THIRD SOURCE, AND IT USED TO BE SILENT. ─────────────────
     vdd_speaker.c models port 0x61 and reports the tone, and nothing ever turned
     that into a sample -- so every beep a guest made went nowhere while the score
     line said "SB16 PCM, OPL2/3 FM, MPU-401, speaker". It is a square wave gated
     by two bits, which is exactly what the hardware does, so it is a dozen lines
     here rather than a device of its own. Amplitude is deliberately WELL below
     full scale: a real speaker is a 1-inch cone, not a line output, and a
     full-scale square would sit on the clip rail over everything else. */
#define AUDIO_SPK_LEVEL 6000      /* peak sample for an active speaker tone      */
#define AUDIO_SPK_HZ_MIN   20u    /* below this it is a click train, not a tone  */
#define AUDIO_SPK_HZ_MAX 20000u   /* above it, nothing at 44.1 kHz is audible    */

typedef struct audio_state {
    opl_state *opl;
    sb_state  *sb;
    const speaker_state *spk; /* PC speaker; NULL = not fitted                   */
    uint32_t   out_hz;
    audio_resampler r_opl, r_sb;
    /* Speaker phase as a 16-bit fraction of one cycle, clocked at out_hz. The
       top bit IS the half-cycle, so the sample is one test and no branch on the
       frequency; it persists across calls so a held tone does not restart (and
       click) at every chunk boundary. */
    uint16_t   spk_phase;
    int32_t    spk_level;     /* peak amplitude; 0 = speaker switched off        */
    /* Master attenuator, applied AFTER the sum -- the position a volume control
       occupies on a real machine, so a game's own mixer settings still work
       underneath it. 0..100; `muted` is separate so muting does not lose it. */
    uint32_t   master;
    int        muted;
    int16_t    scratch[AUDIO_SRC_MAX];
    uint32_t   frames_mixed;  /* diagnostics: total output frames produced       */
} audio_state;

/* Set up the mixer for its two sources. Safe to call again after a device's rate
   changes; the resamplers re-derive their step from the device on each mix.
   Leaves the speaker unfitted, the master volume at 100 and unmuted. */
void vdd_audio_init(audio_state *st, opl_state *opl, sb_state *sb, uint32_t out_hz);

/* Fit (or unfit) the PC speaker. `enable` 0 leaves the VDD on the bus -- port
   0x61 must keep answering, guests time delay loops off its refresh bit -- and
   only stops it being audible. */
void vdd_audio_set_speaker(audio_state *st, const speaker_state *spk, int enable);

/* Master volume, 0..100, clamped; `muted` outputs silence without losing it. */
void vdd_audio_set_master(audio_state *st, uint32_t percent, int muted);

/* Produce `frames` mono 16-bit samples at out_hz, pulling from both devices.
   Always produces exactly `frames` samples (silence when nothing is playing), so
   a host audio thread can call it unconditionally. */
void vdd_audio_mix(audio_state *st, int16_t *out, uint32_t frames);

#endif /* NTVDMEX_VDD_AUDIO_H */
