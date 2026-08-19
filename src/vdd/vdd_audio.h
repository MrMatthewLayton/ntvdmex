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

typedef struct audio_state {
    opl_state *opl;
    sb_state  *sb;
    uint32_t   out_hz;
    audio_resampler r_opl, r_sb;
    int16_t    scratch[AUDIO_SRC_MAX];
    uint32_t   frames_mixed;  /* diagnostics: total output frames produced       */
} audio_state;

/* Set up the mixer for its two sources. Safe to call again after a device's rate
   changes; the resamplers re-derive their step from the device on each mix. */
void vdd_audio_init(audio_state *st, opl_state *opl, sb_state *sb, uint32_t out_hz);

/* Produce `frames` mono 16-bit samples at out_hz, pulling from both devices.
   Always produces exactly `frames` samples (silence when nothing is playing), so
   a host audio thread can call it unconditionally. */
void vdd_audio_mix(audio_state *st, int16_t *out, uint32_t frames);

#endif /* NTVDMEX_VDD_AUDIO_H */
