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

/* (The mono walk, rs_step, went with #232: the OPL was its last caller, and every
   source is now stereo -- rs_step_st below, whose left channel is that walk.) */

void vdd_audio_init(audio_state *st, opl_state *opl, sb_state *sb, uint32_t out_hz)
{
    unsigned i; uint8_t *p = (uint8_t *)st;
    for (i = 0; i < sizeof(*st); ++i) p[i] = 0;
    st->opl = opl;
    st->sb  = sb;
    st->out_hz = out_hz ? out_hz : AUDIO_OUT_HZ;
    st->master = 100;                 /* the struct is zeroed above: 0 would be silence */
}

void vdd_audio_set_gus(audio_state *st, gus_state *gus) { st->gus = gus; }

void vdd_audio_set_speaker(audio_state *st, const speaker_state *spk, int enable)
{
    st->spk = spk;
    st->spk_level = enable ? AUDIO_SPK_LEVEL : 0;
}

void vdd_audio_set_master(audio_state *st, uint32_t percent, int muted)
{
    st->master = percent > 100 ? 100 : percent;
    st->muted  = muted ? 1 : 0;
}

/* ── #189: STEREO. A source that has two channels now keeps them: the SB's stereo
     transfers (they were averaged) and the GUS's per-voice pan (voices were summed).
     The OPL (#232: an OPL3 with NEW set routes each channel left/right; otherwise one
     output, in the middle) and the PC speaker (middle). Output
     is interleaved L/R, 2*frames samples. For a mono source L = R and both equal what
     the mono mixer produced, which is why vdd_audio_mix below can simply fold this. */
static int16_t mix_clip(int32_t v) { return (int16_t)(v > 32767 ? 32767 : (v < -32768 ? -32768 : v)); }

/* One output frame from an INTERLEAVED stereo source, consuming source frames as
   needed; the right channel's pair is carried beside the left (prev/cur for L,
   prev_r/cur_r). `*idx` walks the source buffer in frames and never runs past `n`,
   because rs_need() sized the buffer for exactly this walk. */
static void rs_step_st(audio_resampler *r, const int16_t *src, uint32_t n, uint32_t *idx,
                       int32_t *ol, int32_t *or_)
{
    if (!r->primed) {
        if (*idx < n) { r->prev = src[2 * *idx]; r->prev_r = src[2 * *idx + 1]; ++*idx; }
        else          { r->prev = r->prev_r = 0; }
        if (*idx < n) { r->cur = src[2 * *idx]; r->cur_r = src[2 * *idx + 1]; ++*idx; }
        else          { r->cur = r->prev; r->cur_r = r->prev_r; }
        r->primed = 1;
        r->frac = 0;
    }
    *ol  = r->prev   + (((r->cur   - r->prev)   * (int32_t)(r->frac >> 8)) >> 8);
    *or_ = r->prev_r + (((r->cur_r - r->prev_r) * (int32_t)(r->frac >> 8)) >> 8);
    r->frac += r->step;
    while (r->frac >= 0x10000u) {
        r->frac -= 0x10000u;
        r->prev = r->cur; r->prev_r = r->cur_r;
        if (*idx < n) { r->cur = src[2 * *idx]; r->cur_r = src[2 * *idx + 1]; ++*idx; }
    }
}

void vdd_audio_mix_st(audio_state *st, int16_t *out, uint32_t frames)
{
    uint32_t done = 0;

    while (done < frames) {
        uint32_t n = frames - done;
        uint32_t i, need, idx;
        int16_t *o;
        if (n > AUDIO_CHUNK) n = AUDIO_CHUNK;
        o = out + 2 * done;

        for (i = 0; i < 2 * n; ++i) o[i] = 0;

        /* --- FM: the OPL's own L/R (#232) ---------------------------------- */
        /* An OPL2, or an OPL3 before NEW, renders the same signal to both sides,
           and on L == R this walk is the old mono one exactly -- so an OPL2 mixes
           to the same samples it did as a mono source (audio_test's golden). */
        if (st->opl) {
            int32_t g = mix_gain(st->sb, 0x26);
            rs_setup(&st->r_opl, OPL_NATIVE_HZ, st->out_hz);
            need = rs_need(&st->r_opl, n);
            vdd_opl_render_st(st->opl, st->scratch, need);
            idx = 0;
            for (i = 0; i < n; ++i) {
                int32_t l, r;
                rs_step_st(&st->r_opl, st->scratch, need, &idx, &l, &r);
                o[2*i]   = mix_clip(o[2*i]   + ((l * g) >> 8));
                o[2*i+1] = mix_clip(o[2*i+1] + ((r * g) >> 8));
            }
        }

        /* --- sampled audio: the SB's own L/R ------------------------------ */
        /* Called even while idle: this is what walks the DMA buffer and raises
           the block-completion IRQ the game is waiting for. */
        if (st->sb) {
            int32_t g = mix_gain(st->sb, 0x04);
            rs_setup(&st->r_sb, vdd_sb_frame_hz(st->sb), st->out_hz);
            need = rs_need(&st->r_sb, n);
            vdd_sb_render_st(st->sb, st->scratch, need);
            idx = 0;
            for (i = 0; i < n; ++i) {
                int32_t l, r;
                rs_step_st(&st->r_sb, st->scratch, need, &idx, &l, &r);
                o[2*i]   = mix_clip(o[2*i]   + ((l * g) >> 8));
                o[2*i+1] = mix_clip(o[2*i+1] + ((r * g) >> 8));
            }
        }

        /* --- the GUS, panned per voice (see vdd_gus.c) -------------------- */
        if (st->gus) {
            rs_setup(&st->r_gus, vdd_gus_rate_hz(st->gus), st->out_hz);
            need = rs_need(&st->r_gus, n);
            vdd_gus_render_st(st->gus, st->scratch, need);
            idx = 0;
            for (i = 0; i < n; ++i) {
                int32_t l, r;
                rs_step_st(&st->r_gus, st->scratch, need, &idx, &l, &r);
                o[2*i]   = mix_clip(o[2*i]   + l);
                o[2*i+1] = mix_clip(o[2*i+1] + r);
            }
        }

        /* --- PC speaker (mono: both channels) ------------------------------ */
        /* Gated by port 0x61 bits 0+1 -- both, which is why a program that only
           sets the data bit to click the cone makes no tone here either. */
        if (st->spk && st->spk_level && vdd_speaker_active(st->spk)) {
            uint32_t hz = vdd_speaker_hz(st->spk);
            st->spk_gated += n;
            st->spk_hz = hz;
            if (hz >= AUDIO_SPK_HZ_MIN && hz <= AUDIO_SPK_HZ_MAX) {
                uint32_t step = (uint32_t)(((uint64_t)hz << 16) / st->out_hz);
                st->spk_frames += n;
                for (i = 0; i < n; ++i) {
                    int32_t v = (st->spk_phase & 0x8000u) ? st->spk_level : -st->spk_level;
                    o[2*i]   = mix_clip(o[2*i]   + v);
                    o[2*i+1] = mix_clip(o[2*i+1] + v);
                    st->spk_phase = (uint16_t)(st->spk_phase + step);
                }
            }
        }

        /* --- master attenuator -------------------------------------------- */
        if (st->muted) {
            for (i = 0; i < 2 * n; ++i) o[i] = 0;
        } else if (st->master < 100) {
            int32_t g = (int32_t)((st->master * 256u) / 100u);   /* 0..256 */
            for (i = 0; i < 2 * n; ++i) o[i] = (int16_t)((o[i] * g) >> 8);
        }

        st->frames_mixed += n;
        done += n;
    }
}

/* The mono mixer, as it always was to its callers: the stereo mix folded. For every
   mono source L = R, so (L + R) / 2 is exactly the old sample -- audio_test's checks
   are unchanged. (The SB's stereo was always averaged here; a GUS voice panned hard
   to one side now folds to half its old level -- nothing but the tests calls this.) */
void vdd_audio_mix(audio_state *st, int16_t *out, uint32_t frames)
{
    int16_t tmp[2 * AUDIO_CHUNK];
    uint32_t done = 0, i;
    while (done < frames) {
        uint32_t n = frames - done;
        if (n > AUDIO_CHUNK) n = AUDIO_CHUNK;
        vdd_audio_mix_st(st, tmp, n);
        for (i = 0; i < n; ++i) out[done + i] = (int16_t)(((int32_t)tmp[2*i] + tmp[2*i+1]) / 2);
        done += n;
    }
}
