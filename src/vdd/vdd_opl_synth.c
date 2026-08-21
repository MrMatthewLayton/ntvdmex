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
 * CALIBRATION. Every scaling constant below is MEASURED, not guessed: driven into
 * both this core and a reference one from an identical register stream, one
 * variable at a time, and read back out of the spectrum. `tools/oplref/oplprobe.c`
 * is that rig and each constant names the experiment that produced it, so any of
 * them can be re-derived in seconds rather than argued about. What the measurement
 * is allowed to give us is a PHYSICAL quantity -- a dB slope, a modulation index in
 * radians, an envelope speed in units per sample -- which is what the datasheet
 * describes and what the silicon does; the reference core's own source is not read.
 */
#include "vdd_opl.h"
#include "opl_tables.h"

/* MULT register -> frequency multiplier, doubled so that entry 0 (x0.5) is an
   integer. The duplicated 20/20 and 24/24 and 30/30 entries are the chip's, not
   a typo: MULT 11, 13 and 14 alias their neighbours. */
static const uint8_t opl_mult2[16] =
    { 1, 2, 4, 6, 8, 10, 12, 14, 16, 18, 20, 20, 24, 24, 30, 30 };

/* Key-scale-level ROM, indexed by the top 4 bits of F-num. MEASURED to be in units
   of 0.75 dB, not the 0.375 this once assumed: at KSL=3 the reference attenuates
   6.02 dB per octave and the block term moves 8 ROM units per octave, so one unit
   is 0.7526 dB. See OPL_KSL_TO_LOG and `oplprobe ksl`. */
static const uint8_t opl_kslrom[16] =
    { 0, 32, 40, 45, 48, 51, 53, 55, 56, 58, 59, 60, 61, 62, 63, 64 };
/* KSL field -> right shift. 0 means "no key scaling", expressed as a shift big
   enough to annihilate the value. */
static const uint8_t opl_kslshift[4] = { 8, 1, 2, 0 };

/* One log unit = 1/256 octave of amplitude. Envelope steps are 0.1875 dB and
   total-level steps 0.75 dB, so they scale into log units by 8 and 32. */
#define OPL_ENV_TO_LOG   8
#define OPL_TL_TO_LOG   32
#define OPL_KSL_TO_LOG  32      /* 0.75 dB per ROM unit -- measured, see above    */

/* ENVELOPE SPEED. Measured: `oplprobe egrate` times the reference's decay at every
   effective rate and reports samples per envelope unit. The law it gives is
       units per sample = (4 + rate_lo) / 2^(15 - rate_hi)
   where rate_hi/rate_lo are the top four and bottom two bits of the 6-bit rate.
   Note the MANTISSA: inside a group of four rates the speed goes 4:5:6:7 -- linear,
   not geometric -- and only the group boundary is a doubling. Measured across 30
   rates and all four sub-steps, the implied divisor came out 32983/33007/33008/
   33006, i.e. 2^15 to within the measurement's own bias.
   ► The previous model shifted by whole octaves and rounded the sub-step away,
     which made mid-range decays and releases run 1.5x too slow. */
#define OPL_EG_DIV_SHIFT 15

/* ATTACK. Not linear: the attenuation loses a FRACTION OF ITSELF each sample, so
   the note rushes up and then eases in. `oplprobe attack` fits that fraction
   against the decay speed at the same rate and gets 0.1435 -- constant to +-1.5%
   over 19 rates and all four sub-steps, which is what says the shape is right and
   not merely the endpoint. 147/1024 is that number. */
#define OPL_EG_ATTACK_NUM   147
#define OPL_EG_ATTACK_SHIFT  10


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

/* Envelope step per sample, in env fixed point. See OPL_EG_DIV_SHIFT. */
static int32_t opl_eg_step(int rate)
{
    if (!rate) return 0;                        /* rate 0 never moves             */
    return (int32_t)(4 + (rate & 3)) << (OPL_ENV_SHIFT - OPL_EG_DIV_SHIFT + (rate >> 2));
}

static void opl_env_tick(opl_state *st, int chi, int opi)
{
    opl_op *o = &st->op[opi];
    int32_t step;
    switch (o->eg_state) {
    case OPL_EG_ATTACK:
        step = opl_eg_step(opl_eff_rate(st, chi, o->ar, o->ksr));
        /* AR=0 is not "instant", it is NEVER: the operator stays fully attenuated
           and the note is silent. Confirmed against the reference, which produces
           a peak of 1 against our 4096 before this was fixed -- reading it the
           other way turns silent voices into loud ones. */
        if (!step) break;
        if (o->ar == 15) { o->env = 0; o->eg_state = OPL_EG_DECAY; break; }
        /* Attack is exponential: the closer to full volume, the slower it moves.
           Scaling the step by the remaining attenuation gives that curve without
           a second table. The +1 unit keeps it moving once the product would
           otherwise round to nothing, so a slow attack still finishes. */
        { int64_t d = (((int64_t)step * (o->env + (1 << OPL_ENV_SHIFT))) >> OPL_ENV_SHIFT);
          d = (d * OPL_EG_ATTACK_NUM) >> OPL_EG_ATTACK_SHIFT;
          if (d < 1) d = 1;
          o->env -= (int32_t)d; }
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

/* --- the two low-frequency oscillators ------------------------------------ */
/* TREMOLO. MEASURED (`oplprobe lfo`): a 52-step triangle climbing to 26 envelope
   units and back, one step every 256 samples -- 3.73 Hz, and 4.89 dB deep at DAM=1
   against 4.87 measured. It is a STAIRCASE, not a sine: the reference's amplitude
   moves in exact 0.188 dB steps, one envelope unit at a time. DAM=0 is the same
   counter shifted down two places, which is why its steps last four times as long
   and it measures a quarter as deep. */
#define OPL_AM_SHIFT   8        /* 256 samples per tremolo step                   */
#define OPL_AM_STEPS  52
#define OPL_AM_PEAK   26

static int32_t opl_trem_units(const opl_state *st)
{
    uint32_t s = (st->lfo_count >> OPL_AM_SHIFT) % OPL_AM_STEPS;
    int32_t  t = (s < OPL_AM_PEAK) ? (int32_t)s : (int32_t)(OPL_AM_STEPS - s);
    return (st->reg[0xBD] & OPL_BD_DAM) ? t : (t >> 2);
}

/* VIBRATO. MEASURED: eight steps of 1024 samples -- 49716/8192 = 6.069 Hz, which
   is what the reference reads to four figures -- following the pattern
   0, half, full, half, 0, -half, -full, -half.
   The depth is a SHIFT of the F-number, not a fixed number of cents: fnum >> 7,
   halved again when DVB is clear. That predicts 25.23 cents peak-to-peak at
   F-num 0x3C0 against 25.17 measured, and 27.05 at 0x200 against 27.07 -- and it
   means the effect QUANTISES, so an F-num below 128 gets no vibrato at all. A
   constant-cents implementation matches at one pitch and drifts at every other. */
#define OPL_VIB_SHIFT 10        /* 1024 samples per vibrato step                  */
static const signed char opl_vib_pat[8] = { 0, 1, 2, 1, 0, -1, -2, -1 };

static int32_t opl_vib_offset(const opl_state *st, int chi)
{
    int32_t full = (int32_t)st->ch[chi].fnum >> ((st->reg[0xBD] & OPL_BD_DVB) ? 7 : 8);
    int     t    = opl_vib_pat[(st->lfo_count >> OPL_VIB_SHIFT) & 7];
    int32_t v    = (t == 2 || t == -2) ? full : (t ? (full >> 1) : 0);
    return (t < 0) ? -v : v;
}

/* --- operator ------------------------------------------------------------- */
/* Phase increment per native sample: (F-num << block) * multiplier / 2. With
   multiplier 1 this gives f = fnum * 49716 / 2^(20-block), the chip's formula.
   Vibrato rides on the F-number itself, so it scales with block and multiplier
   exactly as the pitch does -- which is why the effect is a constant interval
   rather than a constant number of hertz. */
static uint32_t opl_phase_inc(const opl_state *st, int chi, const opl_op *o)
{
    int32_t fnum = (int32_t)st->ch[chi].fnum;
    uint32_t base;
    if (o->vib) fnum += opl_vib_offset(st, chi);
    if (fnum < 0) fnum = 0;
    base = (uint32_t)fnum << st->ch[chi].block;
    return (base * opl_mult2[o->mult]) >> 1;
}

/* Static attenuation from total level and key scaling, in log units. */
static int32_t opl_static_att(const opl_state *st, int chi, const opl_op *o)
{
    int32_t att = (int32_t)o->tl * OPL_TL_TO_LOG;
    /* The octave origin is 8, not 7. Measured: at KSL=3 with F-num's top nibble at
       15 the reference is still at full volume in block 0 and down 6 dB in block 1,
       which places the zero one octave lower than this had it. Being one octave out
       under-attenuates every high note -- at block 7 by 6 dB, and by 18 dB once the
       KSL=3 shift is applied -- so bass and treble sit at the wrong relative
       levels across the whole keyboard. */
    int32_t k = (int32_t)opl_kslrom[(st->ch[chi].fnum >> 6) & 0x0F]
              - 8 * (8 - (int32_t)st->ch[chi].block);
    if (k < 0) k = 0;
    att += (k >> opl_kslshift[o->ksl]) * OPL_KSL_TO_LOG;
    return att;
}

/* MODULATION DEPTH. An operator's output runs to +-4096 at full volume and that
   value is added STRAIGHT into the 1024-step phase index -- so a modulator at
   TL=0 swings the carrier through four whole cycles. MEASURED: `oplprobe mod`
   fits the modulation index from the sideband amplitudes and reads 4.000 cycles
   for the reference against our 2.000, a ratio of exactly 0.501 held across the
   whole TL sweep. We were halving it on the way in.
   ► This is why the timbre was wrong while the tune was right. Halving the index
     does not change pitch, tempo or loudness -- phase modulation preserves total
     power -- it changes only WHICH harmonics are present and how strongly, which
     is precisely "the right tune, but the instruments sound flat".
   ► Feedback measured the same way: our FB=n matched the reference's FB=n-1 step
     for step, the same factor of two, in the same place.                          */

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
    if (o->am) att += opl_trem_units(st) * OPL_ENV_TO_LOG;
    amp = opl_exp2neg(att);
    return neg ? -amp : amp;
}

/* --- rhythm mode ---------------------------------------------------------- *
 * With 0xBD bit 5 set, channels 6-8 stop being melodic voices and become five
 * percussion ones. Everything below was mapped from the OUTSIDE (`oplprobe
 * rhythm`) rather than written down from memory: each operator was silenced in
 * turn to find whose ENVELOPE drives which drum, and each operator's MULT was
 * doubled in turn to find whose PHASE it runs on. Those are not the same answer,
 * and assuming they were would have got the snare wrong.
 *
 *   voice       envelope   phase        character (measured)
 *   bass drum   op12+op15  own          tonality 1.000 -- ordinary 2-op FM
 *   tom-tom     op14       own          tonality 1.000 -- a plain sine
 *   snare       op16       OP13's       tonality 0.509 -- half tone, half noise
 *   hi-hat      op13       op13 + op17  tonality 0.003 -- essentially pure noise
 *   cymbal      op17       op13 + op17  tonality 0.749
 *
 * Also measured, and not something to guess at: the percussion voices are summed
 * at DOUBLE amplitude. A tom-tom peaks at 8170 where an ordinary operator at the
 * same settings peaks at 4085.
 *
 * ► THE LAST THREE ARE NOT IMPLEMENTED, DELIBERATELY. Snare, hi-hat and cymbal
 *   need the chip's special phase generator -- a boolean function of bits taken
 *   from two different phase accumulators, plus a noise source. That is precisely
 *   the kind of detail this project's cardinal rule exists for: writing it from
 *   half-memory would produce something plausible that is wrong, and the harness
 *   would score it as an improvement because ANY sound beats silence. They are
 *   counted instead (prof_rhythm_hits), so a run says out loud what it could not
 *   play. The measurements needed to derive them are recorded above and in
 *   return-ntvdm.md.                                                              */
static int32_t opl_rhythm_sample(opl_state *st)
{
    int32_t acc = 0, mo, co, fbmod = 0;
    opl_op *m = &st->op[12], *cr = &st->op[15];

    /* BASS DRUM -- channel 6, an ordinary two-operator voice. */
    if (m->eg_state != OPL_EG_OFF || cr->eg_state != OPL_EG_OFF) {
        if (st->ch[6].fb) fbmod = (m->out1 + m->out2) >> (9 - st->ch[6].fb);
        mo = opl_op_sample(st, 6, 12, fbmod);
        m->out2 = m->out1; m->out1 = mo;
        opl_env_tick(st, 6, 12);
        if (st->ch[6].cnt) { co = opl_op_sample(st, 6, 15, 0); acc += (mo + co) * 2; }
        else               { co = opl_op_sample(st, 6, 15, mo); acc += co * 2; }
        opl_env_tick(st, 6, 15);
    }

    /* TOM-TOM -- op14 alone, on channel 8's pitch. */
    if (st->op[14].eg_state != OPL_EG_OFF) {
        acc += opl_op_sample(st, 8, 14, 0) * 2;
        opl_env_tick(st, 8, 14);
    }

    /* Snare (op16), hi-hat (op13) and cymbal (op17) belong here. Their envelopes
       still run so that a key-on is not left latched forever, but they produce no
       sound: see the note above. */
    if (st->op[13].eg_state != OPL_EG_OFF) opl_env_tick(st, 7, 13);
    if (st->op[16].eg_state != OPL_EG_OFF) opl_env_tick(st, 7, 16);
    if (st->op[17].eg_state != OPL_EG_OFF) opl_env_tick(st, 8, 17);
    return acc;
}

/* --- public: render ------------------------------------------------------- */
void vdd_opl_render(opl_state *st, int16_t *out, uint32_t frames)
{
    uint32_t n;
    int rhythm = (st->reg[0xBD] & OPL_BD_RHY) ? 1 : 0;
    int nmelodic = rhythm ? 6 : OPL_NUM_CH;     /* 6-8 are percussion in rhythm    */
    for (n = 0; n < frames; ++n) {
        int32_t acc = rhythm ? opl_rhythm_sample(st) : 0;
        int c;
        /* Outside the channel loop, and before the early-out below: the LFOs run
           whether or not anything is sounding. Advancing them only while a note
           plays would restart the sweep at every silence. */
        st->lfo_count++;
        for (c = 0; c < nmelodic; ++c) {
            int mi = vdd_opl_op_index(c, 0), ci = vdd_opl_op_index(c, 1);
            opl_op *m = &st->op[mi], *cr = &st->op[ci];
            int32_t mo, co, fbmod = 0;

            if (m->eg_state == OPL_EG_OFF && cr->eg_state == OPL_EG_OFF) continue;

            /* Feedback uses the mean of the operator's last two outputs, which is
               what keeps a self-modulating operator stable instead of screaming. */
            if (st->ch[c].fb)
                fbmod = (m->out1 + m->out2) >> (9 - st->ch[c].fb);

            mo = opl_op_sample(st, c, mi, fbmod);
            m->out2 = m->out1; m->out1 = mo;
            opl_env_tick(st, c, mi);

            if (st->ch[c].cnt) {                /* additive: both operators heard  */
                co = opl_op_sample(st, c, ci, 0);
                acc += mo + co;
            } else {                            /* FM: modulator bends the carrier */
                co = opl_op_sample(st, c, ci, mo);
                acc += co;
            }
            opl_env_tick(st, c, ci);
        }
        if (acc >  32767) acc =  32767;
        if (acc < -32768) acc = -32768;
        out[n] = (int16_t)acc;
    }
}
