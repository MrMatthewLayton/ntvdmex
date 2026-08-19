/* vdd_opl_synth.c -- OPL2 (YM3812) FM synthesis for vdd_opl.c.  Pure C, no
 * <windows.h>, no libm: all transcendentals come from the generated tables.
 *
 * Written from the documented YM3812 behaviour rather than ported from an
 * existing core, so it is ours and MIT-clean. The structure follows the real
 * chip because that is what makes it cheap AND correct:
 *
 *   - Everything is LOG domain. Amplitudes multiply, so in the log domain they
 *     add: an operator's output is exp2(-(logsin[phase] + env + TL + KSL)). One
 *     lookup and some adds -- no multiplies and no floats in the sample loop.
 *   - Phase is a 20-bit accumulator; the top 10 bits index a quarter-wave sine
 *     table that we mirror and negate to get the full cycle.
 *   - FM is phase modulation: the modulator's output is added to the carrier's
 *     phase index before lookup. Feedback is the same trick applied to operator 1
 *     using the average of its own last two outputs, which is what stops it
 *     oscillating into noise.
 *
 * Rendered at the chip's native 49716 Hz (3.579545 MHz / 72) so the phase maths
 * is exact; the mixer resamples to the host rate. Deliberately NOT modelled yet:
 * tremolo/vibrato depth (0xBD) and rhythm mode -- both are additive on top of
 * this and neither affects pitch or note timing.
 *
 * CALIBRATION NOTE: envelope RATES are anchored empirically (see OPL_EG_ANCHOR).
 * Pitch is exact and the envelope shape is right, but absolute attack/decay times
 * are within roughly a factor of two of real silicon at the extremes of the rate
 * range. That is audible as a slightly different "feel", not as wrong notes, and
 * is the first thing to refine against a reference recording.
 */
#include "vdd_opl.h"
#include "opl_tables.h"

/* MULT register -> frequency multiplier, doubled so that entry 0 (x0.5) is an
   integer. The duplicated 20/20 and 24/24 and 30/30 entries are the chip's, not
   a typo: MULT 11, 13 and 14 alias their neighbours. */
static const uint8_t opl_mult2[16] =
    { 1, 2, 4, 6, 8, 10, 12, 14, 16, 18, 20, 20, 24, 24, 30, 30 };

/* Key-scale-level ROM, in units of 0.375 dB, indexed by the top 4 bits of F-num. */
static const uint8_t opl_kslrom[16] =
    { 0, 32, 40, 45, 48, 51, 53, 55, 56, 58, 59, 60, 61, 62, 63, 64 };
/* KSL field -> right shift. 0 means "no key scaling", expressed as a shift big
   enough to annihilate the value. */
static const uint8_t opl_kslshift[4] = { 8, 1, 2, 0 };

/* One log unit = 1/256 octave of amplitude. Envelope steps are 0.1875 dB and
   total-level steps 0.75 dB, so they scale into log units by 8 and 32. */
#define OPL_ENV_TO_LOG   8
#define OPL_TL_TO_LOG   32
#define OPL_KSL_TO_LOG  16      /* 0.375 dB per ROM unit                         */
#define OPL_ENV_MAX    511      /* fully attenuated                              */

/* Envelope rate anchor: at this 6-bit rate the envelope moves one level unit per
   sample, and every 4 rate units doubles the speed. See the calibration note. */
#define OPL_EG_ANCHOR   52

/* env is carried in 8.8 fixed point so the slowest rates still make progress. */
#define OPL_ENV_SHIFT    8

/* exp2(-x/256) * 4096 for an arbitrary x, via the table plus a shift. */
static int32_t opl_exp2neg(int32_t logv)
{
    int sh;
    if (logv < 0) logv = 0;
    sh = logv >> 8;
    if (sh > 13) return 0;                      /* below one LSB: silent          */
    return (int32_t)opl_exp[logv & 0xFF] >> sh;
}

/* Full-cycle sine in the log domain: returns the attenuation for this phase and
   sets *neg for the negative half. The table holds one quarter, mirrored twice. */
static int32_t opl_logsin_full(uint32_t phase_idx, int *neg)
{
    uint32_t q = (phase_idx >> 8) & 3, i = phase_idx & 0xFF;
    *neg = (q & 2) ? 1 : 0;
    return (int32_t)opl_logsin[(q & 1) ? (255 - i) : i];
}

/* The OPL2's four waveforms are cheap edits of the sine: 1 clips the negative
   half to zero, 2 rectifies it, 3 keeps only the rising quarters. */
static int32_t opl_wave(uint8_t wave, uint32_t phase_idx, int *neg, int *mute)
{
    *mute = 0;
    switch (wave) {
    case 1:                                     /* half-wave rectified            */
        if (phase_idx & 0x200) { *mute = 1; *neg = 0; return 0; }
        return opl_logsin_full(phase_idx, neg);
    case 2:                                     /* absolute value                 */
        { int32_t v = opl_logsin_full(phase_idx, neg); *neg = 0; return v; }
    case 3:                                     /* pulse-sine (rising quarters)   */
        if (phase_idx & 0x100) { *mute = 1; *neg = 0; return 0; }
        { int32_t v = opl_logsin_full(phase_idx, neg); *neg = 0; return v; }
    default:
        return opl_logsin_full(phase_idx, neg);
    }
}

/* --- envelope ------------------------------------------------------------- */
/* Effective 6-bit rate: the 4-bit register value, scaled up by where the note
   sits on the keyboard. High notes decay faster on a real OPL, and KSR selects
   how strongly that applies. */
static int opl_eff_rate(const opl_state *st, int chi, uint8_t r4, uint8_t ksr)
{
    int ksr_val = (st->ch[chi].block << 1) | ((st->ch[chi].fnum >> 9) & 1);
    int rof = ksr ? ksr_val : (ksr_val >> 2);
    int r;
    if (!r4) return 0;                          /* rate 0 never moves             */
    r = r4 * 4 + rof;
    return r > 63 ? 63 : r;
}

/* Envelope step per sample in 8.8 units, doubling every 4 rate units. */
static int32_t opl_eg_step(int rate)
{
    int e = rate - OPL_EG_ANCHOR;               /* e == 0 -> one level per sample */
    int32_t base = 1 << OPL_ENV_SHIFT;
    if (!rate) return 0;
    if (e >= 0) {
        int sh = e / 4;
        if (sh > 12) sh = 12;
        return base << sh;                      /* (fractional quarter ignored)   */
    }
    { int sh = (-e + 3) / 4;
      if (sh > 20) return 0;                    /* slower than we can represent   */
      return base >> sh; }
}

static void opl_env_tick(opl_state *st, int chi, int opi)
{
    opl_op *o = &st->op[opi];
    int32_t step;
    switch (o->eg_state) {
    case OPL_EG_ATTACK:
        step = opl_eg_step(opl_eff_rate(st, chi, o->ar, o->ksr));
        if (!step || o->ar == 15) { o->env = 0; o->eg_state = OPL_EG_DECAY; break; }
        /* Attack is exponential: the closer to full volume, the slower it moves.
           Scaling the step by the remaining attenuation gives that curve without
           a second table. */
        o->env -= (int32_t)(((int64_t)step * (o->env + (1 << OPL_ENV_SHIFT)))
                            >> (OPL_ENV_SHIFT + 3));
        if (o->env <= 0) { o->env = 0; o->eg_state = OPL_EG_DECAY; }
        break;
    case OPL_EG_DECAY:
        step = opl_eg_step(opl_eff_rate(st, chi, o->dr, o->ksr));
        o->env += step;
        /* SL is 4 bits of 3 dB each; 15 means "all the way down". */
        { int32_t sl = (o->sl == 15) ? (OPL_ENV_MAX << OPL_ENV_SHIFT)
                                     : ((int32_t)o->sl * 16) << OPL_ENV_SHIFT;
          if (o->env >= sl) { o->env = sl; o->eg_state = OPL_EG_SUSTAIN; } }
        break;
    case OPL_EG_SUSTAIN:
        /* EGT selects sustaining (hold) versus percussive (keep decaying). */
        if (!o->egt) {
            o->env += opl_eg_step(opl_eff_rate(st, chi, o->rr, o->ksr));
            if (o->env >= (OPL_ENV_MAX << OPL_ENV_SHIFT)) {
                o->env = OPL_ENV_MAX << OPL_ENV_SHIFT; o->eg_state = OPL_EG_OFF;
            }
        }
        break;
    case OPL_EG_RELEASE:
        o->env += opl_eg_step(opl_eff_rate(st, chi, o->rr, o->ksr));
        if (o->env >= (OPL_ENV_MAX << OPL_ENV_SHIFT)) {
            o->env = OPL_ENV_MAX << OPL_ENV_SHIFT; o->eg_state = OPL_EG_OFF;
        }
        break;
    default:
        o->env = OPL_ENV_MAX << OPL_ENV_SHIFT;
        break;
    }
    if (o->env < 0) o->env = 0;
    if (o->env > (OPL_ENV_MAX << OPL_ENV_SHIFT)) o->env = OPL_ENV_MAX << OPL_ENV_SHIFT;
}

/* --- operator ------------------------------------------------------------- */
/* Phase increment per native sample: (F-num << block) * multiplier / 2. With
   multiplier 1 this gives f = fnum * 49716 / 2^(20-block), the chip's formula. */
static uint32_t opl_phase_inc(const opl_state *st, int chi, const opl_op *o)
{
    uint32_t base = (uint32_t)st->ch[chi].fnum << st->ch[chi].block;
    return (base * opl_mult2[o->mult]) >> 1;
}

/* Static attenuation from total level and key scaling, in log units. */
static int32_t opl_static_att(const opl_state *st, int chi, const opl_op *o)
{
    int32_t att = (int32_t)o->tl * OPL_TL_TO_LOG;
    int32_t k = (int32_t)opl_kslrom[(st->ch[chi].fnum >> 6) & 0x0F]
              - 8 * (7 - (int32_t)st->ch[chi].block);
    if (k < 0) k = 0;
    att += (k >> opl_kslshift[o->ksl]) * OPL_KSL_TO_LOG;
    return att;
}

/* One operator sample. `mod` is a phase offset in sine-table steps (FM input). */
static int32_t opl_op_sample(opl_state *st, int chi, int opi, int32_t mod)
{
    opl_op *o = &st->op[opi];
    uint32_t idx;
    int32_t logv, att, amp;
    int neg = 0, mute = 0;

    o->phase += opl_phase_inc(st, chi, o);
    if (o->eg_state == OPL_EG_OFF) return 0;

    idx  = ((o->phase >> 10) + (uint32_t)mod) & 0x3FF;
    logv = opl_wave(o->wave, idx, &neg, &mute);
    if (mute) return 0;

    att = logv + (o->env >> OPL_ENV_SHIFT) * OPL_ENV_TO_LOG + opl_static_att(st, chi, o);
    amp = opl_exp2neg(att);
    return neg ? -amp : amp;
}

/* --- public: render ------------------------------------------------------- */
void vdd_opl_render(opl_state *st, int16_t *out, uint32_t frames)
{
    uint32_t n;
    for (n = 0; n < frames; ++n) {
        int32_t acc = 0;
        int c;
        for (c = 0; c < OPL_NUM_CH; ++c) {
            int mi = vdd_opl_op_index(c, 0), ci = vdd_opl_op_index(c, 1);
            opl_op *m = &st->op[mi], *cr = &st->op[ci];
            int32_t mo, co, fbmod = 0;

            if (m->eg_state == OPL_EG_OFF && cr->eg_state == OPL_EG_OFF) continue;

            /* Feedback uses the mean of the operator's last two outputs, which is
               what keeps a self-modulating operator stable instead of screaming. */
            if (st->ch[c].fb)
                fbmod = (m->out1 + m->out2) >> (9 - st->ch[c].fb);

            mo = opl_op_sample(st, c, mi, fbmod >> 1);
            m->out2 = m->out1; m->out1 = mo;
            opl_env_tick(st, c, mi);

            if (st->ch[c].cnt) {                /* additive: both operators heard  */
                co = opl_op_sample(st, c, ci, 0);
                acc += mo + co;
            } else {                            /* FM: modulator bends the carrier */
                co = opl_op_sample(st, c, ci, mo >> 1);
                acc += co;
            }
            opl_env_tick(st, c, ci);
        }
        if (acc >  32767) acc =  32767;
        if (acc < -32768) acc = -32768;
        out[n] = (int16_t)acc;
    }
}
