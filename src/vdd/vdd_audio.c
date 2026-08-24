/* vdd_audio.c -- see vdd_audio.h.  Resample the OPL and the Sound Blaster onto a
 * common output clock and sum them.  Pure C, no <windows.h>. */
#include "vdd_audio.h"

/* Source gain from the SB16 mixer registers, as a 0..256 fraction. A real card
   does not sum FM and sampled audio at unity -- the mixer chip attenuates both,
   which is exactly what stops a busy score from sitting on the clip rail. We
   honour the same registers (master 0x22, voice 0x04, FM 0x26), so a game's own
   volume control works, and their power-up value (0xCC) gives sane headroom. */
static int32_t mix_gain(const sb_state *sb, uint8_t reg)
{
    uint32_t master, src;
    if (!sb) return 256;
    master = (sb->mix[0x22] >> 4) & 0x0F;
    src    = (sb->mix[reg]   >> 4) & 0x0F;
    if (!sb->mix[0x22]) master = 12;            /* never programmed: power-up 0xCC */
    if (!sb->mix[reg])  src    = 12;
    return (int32_t)((master * src * 256u) / 225u);   /* (m/15)*(s/15) in 0..256   */
}

static void rs_setup(audio_resampler *r, uint32_t src_hz, uint32_t out_hz)
{
    if (!src_hz) src_hz = out_hz;
    if (r->src_hz != src_hz) {                  /* rate changed mid-stream        */
        r->src_hz = src_hz;
        r->step   = (uint32_t)(((uint64_t)src_hz << 16) / (out_hz ? out_hz : 1));
        if (!r->step) r->step = 1;
    }
}

/* ── PULL EXACTLY WHAT WILL BE CONSUMED, AND NOT ONE SAMPLE MORE. ───────────────────
     This used to ask for two extra samples every chunk, "the pair being interpolated
     between". For a synthesised source that is merely wasteful; for the SOUND BLASTER
     it is data loss. vdd_sb_render() is the TRANSPORT -- every sample it is asked for
     is pulled out of the guest's DMA ring and thrown away if the resampler does not
     use it. Two per chunk, 86 chunks a second at Doom's 11025 Hz stereo, is 172
     dropped PCM samples a second: the read pointer walks away from the guest's write
     pointer at 1.6%, and what you hear is a soft click at chunk rate over everything.
     The count is exact and provable: prev/cur persist across calls, so the only
     samples consumed are the ones a phase wrap loads, and there are exactly
     floor((frac + step*frames) / 0x10000) wraps. Priming loads the first pair, once. */
static uint32_t rs_need(const audio_resampler *r, uint32_t frames)
{
    uint64_t span = (uint64_t)r->frac + (uint64_t)r->step * frames;
    uint32_t n = (uint32_t)(span >> 16) + (r->primed ? 0u : 2u);
    return n > AUDIO_SRC_MAX ? AUDIO_SRC_MAX : n;
}

/* Advance one output frame, consuming source samples from `src` as needed.
   `*idx` walks the source buffer; it never runs past `n` because rs_need()
   sized the buffer for exactly this walk. */
static int32_t rs_step(audio_resampler *r, const int16_t *src, uint32_t n, uint32_t *idx)
{
    int32_t out;
    if (!r->primed) {                           /* load the first pair            */
        r->prev = (*idx < n) ? src[(*idx)++] : 0;
        r->cur  = (*idx < n) ? src[(*idx)++] : r->prev;
        r->primed = 1;
        r->frac = 0;
    }
    out = r->prev + (((r->cur - r->prev) * (int32_t)(r->frac >> 8)) >> 8);
    r->frac += r->step;
    while (r->frac >= 0x10000u) {
        r->frac -= 0x10000u;
        r->prev = r->cur;
        r->cur  = (*idx < n) ? src[(*idx)++] : r->cur;
    }
    return out;
}

void vdd_audio_init(audio_state *st, opl_state *opl, sb_state *sb, uint32_t out_hz)
{
    unsigned i; uint8_t *p = (uint8_t *)st;
    for (i = 0; i < sizeof(*st); ++i) p[i] = 0;
    st->opl = opl;
    st->sb  = sb;
    st->out_hz = out_hz ? out_hz : AUDIO_OUT_HZ;
}

void vdd_audio_mix(audio_state *st, int16_t *out, uint32_t frames)
{
    uint32_t done = 0;

    while (done < frames) {
        uint32_t n = frames - done;
        uint32_t i, need, idx;
        if (n > AUDIO_CHUNK) n = AUDIO_CHUNK;

        for (i = 0; i < n; ++i) out[done + i] = 0;

        /* --- FM ----------------------------------------------------------- */
        if (st->opl) {
            int32_t g = mix_gain(st->sb, 0x26);
            rs_setup(&st->r_opl, OPL_NATIVE_HZ, st->out_hz);
            need = rs_need(&st->r_opl, n);
            vdd_opl_render(st->opl, st->scratch, need);
            idx = 0;
            for (i = 0; i < n; ++i) {
                int32_t v = out[done + i]
                          + ((rs_step(&st->r_opl, st->scratch, need, &idx) * g) >> 8);
                out[done + i] = (int16_t)(v > 32767 ? 32767 : (v < -32768 ? -32768 : v));
            }
        }

        /* --- sampled audio ------------------------------------------------ */
        /* Called even while idle: this is what walks the DMA buffer and raises
           the block-completion IRQ the game is waiting for. */
        if (st->sb) {
            int32_t g = mix_gain(st->sb, 0x04);
            rs_setup(&st->r_sb, st->sb->rate_hz, st->out_hz);
            need = rs_need(&st->r_sb, n);
            vdd_sb_render(st->sb, st->scratch, need);
            idx = 0;
            for (i = 0; i < n; ++i) {
                int32_t v = out[done + i]
                          + ((rs_step(&st->r_sb, st->scratch, need, &idx) * g) >> 8);
                out[done + i] = (int16_t)(v > 32767 ? 32767 : (v < -32768 ? -32768 : v));
            }
        }

        st->frames_mixed += n;
        done += n;
    }
}
