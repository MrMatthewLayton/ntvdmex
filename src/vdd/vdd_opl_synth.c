/* vdd_opl_synth.c -- OPL2 (YM3812) and OPL3 (YMF262) FM synthesis for vdd_opl.c.
 * Pure C, no <windows.h>, no libm: all transcendentals come from the generated
 * tables.
 *
 * The OPL3 is the OPL2 twice over plus three things, each gated on NEW (0x105
 * bit 0) and each below: waveforms 4-7 (opl_wave), 4-operator voices
 * (opl_voice4), and per-channel stereo routing (opl_route). With NEW clear the
 * output is the OPL2's, sample for sample -- held by a golden checksum in
 * tools/dostest/opl_synth_test.c. Written, like the rest, from the Yamaha
 * datasheet; the reference core is an oracle for measurements only.
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
 * is exact; the mixer resamples to the host rate. Tremolo/vibrato and all five
 * rhythm-mode drums are modelled below, each from measurement.
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
   half to zero, 2 rectifies it, 3 keeps only the rising quarters.
   The OPL3 adds four more (YMF262 datasheet, waveform figure), all of which live
   in the FIRST half-cycle or are antisymmetric about its end:
     4  a full sine at twice the rate in the first half, silence in the second
     5  the same, rectified ("camel")
     6  a square: full level, positive then negative
     7  the "derived square": an exponential fall from full level across the first
        half, and its point-mirror (negative, rising back to full) across the second.
   In the log domain 7 is a straight line -- attenuation growing linearly with
   phase. ⚠ The datasheet draws the curve but gives no slope; this uses one log
   unit per 1/8 phase step, i.e. a factor of two every 32 of the 512 steps, so the
   curve reaches silence by the end of its half-cycle. That slope is owed an
   `oplprobe wave` measurement against the oracle, as every constant here was. */
#define OPL_W7_SLOPE_SHIFT 3

static int32_t opl_wave(uint8_t wave, uint32_t phase_idx, int *neg, int *mute)
{
    *mute = 0;
    switch (wave) {
    case 4:                                     /* double-rate sine, 1st half     */
        if (phase_idx & 0x200) { *mute = 1; *neg = 0; return 0; }
        return opl_logsin_full((phase_idx << 1) & 0x3FF, neg);
    case 5:                                     /* double-rate |sine|, 1st half   */
        if (phase_idx & 0x200) { *mute = 1; *neg = 0; return 0; }
        { int32_t v = opl_logsin_full((phase_idx << 1) & 0x3FF, neg); *neg = 0; return v; }
    case 6:                                     /* square                         */
        *neg = (phase_idx & 0x200) ? 1 : 0;
        return 0;
    case 7:                                     /* derived square (log sawtooth)  */
        *neg = (phase_idx & 0x200) ? 1 : 0;
        return (int32_t)(*neg ? ((phase_idx & 0x1FF) ^ 0x1FF) : (phase_idx & 0x1FF))
               << OPL_W7_SLOPE_SHIFT;
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

/* ── WHICH WAVEFORM ACTUALLY PLAYS. Three chips' worth of rules over one 3-bit
     field, decided HERE (at render time) because the gating registers can be
     written after the waveform is:
       OPL2         2 bits, and only while WSE (0x01 bit 5) is set -- with WSE
                    clear the YM3812 plays a sine whatever 0xE0 says (YM3812
                    application manual, register 01). Programs that forget WSE
                    exist, and on the real card they get sines.
       OPL3, NEW=0  2 bits. The YMF262 has no WSE: its register 0x01 is the LSI
                    test register only, and the OPL2 waveforms are always live.
       OPL3, NEW=1  all 3 bits -- waveforms 4-7. */
static uint8_t opl_eff_wave(const opl_state *st, const opl_op *o)
{
    if (st->opl3) return (uint8_t)(o->wave & ((st->reg[OPL3_REG_NEW] & 1) ? 7 : 3));
    return (st->reg[0x01] & 0x20) ? (uint8_t)(o->wave & 3) : 0;
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

/* An operator's output at phase index `idx` (0-1023): waveform, envelope, level,
   tremolo. Split out of opl_op_sample for the rhythm voices, whose phase is not
   their own accumulator's. */
static int32_t opl_op_out(const opl_state *st, int chi, const opl_op *o, uint32_t idx)
{
    int32_t logv, att, amp;
    int neg = 0, mute = 0;

    logv = opl_wave(opl_eff_wave(st, o), idx & 0x3FF, &neg, &mute);
    if (mute) return 0;

    att = logv + (o->env >> OPL_ENV_SHIFT) * OPL_ENV_TO_LOG + opl_static_att(st, chi, o);
    if (o->am) att += opl_trem_units(st) * OPL_ENV_TO_LOG;
    amp = opl_exp2neg(att);
    return neg ? -amp : amp;
}

/* One operator sample. `mod` is a phase offset in sine-table steps (FM input). */
static int32_t opl_op_sample(opl_state *st, int chi, int opi, int32_t mod)
{
    opl_op *o = &st->op[opi];
    o->phase += opl_phase_inc(st, chi, o);
    if (o->eg_state == OPL_EG_OFF) return 0;
    return opl_op_out(st, chi, o, (o->phase >> 10) + (uint32_t)mod);
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
 * ── SNARE, HI-HAT AND CYMBAL (#139). These three read a PHASE THEY DO NOT OWN: a
 *    10-bit index built from bits of op13's and op17's accumulators and from a
 *    noise bit, which then goes through the operator's waveform, envelope and level
 *    exactly as an ordinary phase would. Everything here was READ OUT of the
 *    reference's output, never taken from its source (`oplprobe rphase`, `noise`):
 *    run the drum eight times, once per waveform, and invert the eight output
 *    samples against a table of the same eight waveforms measured on the tom-tom
 *    (an ordinary operator) -- which gives the 10-bit phase index the drum used at
 *    every sample. What came out:
 *
 *      voice    phase index (measured, 0 mispredictions in 11936 samples, on a
 *               register setup the fit never saw)
 *      hi-hat   B<<9 | (B ^ noise ? 0x0D0 : 0x034)
 *      snare    S<<9 | (S ^ noise) << 8          S = bit 8 of op13's index
 *      cymbal   B<<9 | 0x080
 *
 *    where B, "the phase bit", is a function of five accumulator bits and nothing
 *    else. A search over every subset of up to five of the twenty bits of the two
 *    indices found exactly ONE subset that determines it -- op13 bits 2, 3, 7 and
 *    op17 bits 3, 5 -- and its truth table is zero in exactly four of 32 cells:
 *      B = (p13.2 ^ p13.7) | (p13.3 ^ p17.5) | (p17.3 ^ p17.5)
 *    (p13.3 ^ p17.3 in the middle term is the same function.) ⚠ Not a guess to be
 *    "corrected" from memory: the obvious-looking alternative with p13.3 OR'd in on
 *    its own is REFUTED by the table -- cell 11010 reads 0.
 *
 *    TIMING, also measured (the subset search fits the sample offsets too): the
 *    hi-hat sees op13's index for THIS sample but op17's from the PREVIOUS one; the
 *    cymbal and snare see both current. The reference also delivers its carrier
 *    slots one sample later than ours -- snare and cymbal best-align at lag +4
 *    like the bass drum, the hi-hat at +3 like the tom-tom. That is the pipeline,
 *    not the waveform: see the lag note in `oplprobe rhythm`.
 *
 *    op13 and op17 run their phase every sample in rhythm mode whether or not
 *    their own drum is keyed (the hi-hat alone still reads op17's bits). A drum's
 *    key-on restarts its own operator's accumulator -- and WHERE it restarts
 *    matters here as it does nowhere else, because the other accumulator is not
 *    restarted with it. MEASURED (`oplprobe restart`, key-on at samples 1, 2, 37,
 *    3001): a key-on restarts the accumulator ONE STEP FURTHER ON than the
 *    running one's own start from reset would put it -- so its first sample reads
 *    two steps, not one (rhy_restart, set in vdd_opl.c). A key-on before the
 *    first sample after reset is the exception and reads like reset itself, which
 *    is why the default experiment never saw it. Without this the hi-hat and
 *    cymbal keyed into a running chip score 0.71 / 0.70; with it 0.9999, the rest
 *    being two samples of key-on latency the tom-tom shares. (Ordinary operators
 *    restart the same way, but there it is a one-sample shift of a lone waveform
 *    against its envelope -- inaudible, invisible at best lag, and the OPL2
 *    golden holds it -- so only these two accumulators model it.)
 *
 * ── THE NOISE (`oplprobe noise`). With op13/op17 frozen (F-num 0) B is 0, so the
 *    hi-hat and snare phases above carry nothing but the noise bit. Both streams
 *    have linear complexity 23 (Berlekamp-Massey) with the SAME recurrence,
 *        s[n] = s[n-1] ^ s[n-8] ^ s[n-9] ^ s[n-23]
 *    which is precisely the trinomial LFSR  u[i] = u[i-9] ^ u[i-23]  (period
 *    2^23-1) sampled once every 9*2^k of its own steps -- and 72 = 9*8 is the
 *    chip's master clocks per output sample. The decimation is not a free choice:
 *    solving for the one state that yields the hi-hat's stream, the snare's stream
 *    turns out to be the SAME register read 66 steps (0.917 samples) earlier under
 *    72 steps per sample (or 33 / 132 under 36 / 144, the same thing scaled),
 *    while under 9 or 18 it is on no shared register at all. 72 it is, and the
 *    state it solves to is ONE SET BIT (OPL_NOISE_SEED) at exactly the point our
 *    renderer starts from after reset. So the noise is sample-exact from
 *    power-on, not merely statistically alike: the hi-hat's and snare's decoded
 *    noise bits match the model at every one of 3936 samples.
 *    In our frame the snare's output sits one sample after the hi-hat's (the lag
 *    note above), so "66 steps before the hi-hat's NEXT sample" is bit 6 of the
 *    same 23-bit window whose bit 0 the hi-hat reads.
 *
 * Summed at double amplitude like the other drums. Hi-hat and snare follow channel
 * 7's C0 routing and the cymbal channel 8's, like the tom-tom -- measured, OPL3
 * with NEW set, one channel routed left at a time (`oplprobe drums`). The hit
 * counters (prof_rhythm_hits) stay: they say how much percussion a game uses. */

/* The noise register: 72 steps of u[i] = u[i-9] ^ u[i-23] per sample. `w` holds
   the last 23 outputs, bit 0 the oldest; the nearest tap is 9 back, so nine new
   bits can be made at once and eight of those make one sample's 72. */
#define OPL_NOISE_STEPS_PER_SAMPLE 72
static uint32_t opl_noise_step(uint32_t w)
{
    int k;
    if (!w) w = OPL_NOISE_SEED;         /* a never-reset struct: zero is a dead state */
    for (k = 0; k < OPL_NOISE_STEPS_PER_SAMPLE / 9; ++k)
        w = (w >> 9) | ((((w >> 14) ^ w) & 0x1FF) << 14);
    return w;
}
#define OPL_NOISE_BIT_HH 0      /* which bit of the window each drum reads -- the */
#define OPL_NOISE_BIT_SD 6      /* snare 6 steps later (see `oplprobe noise`)     */

/* The phase bit B from op13's and op17's 10-bit indices. */
static uint32_t opl_rhythm_bit(uint32_t p13, uint32_t p17)
{
    return (((p13 >> 2) ^ (p13 >> 7)) | ((p13 >> 3) ^ (p17 >> 5)) | ((p17 >> 3) ^ (p17 >> 5))) & 1;
}

/* Returns the drums per CHANNEL -- bass drum on channel 6, hi-hat and snare on 7,
   tom-tom and cymbal on 8 -- because on an OPL3 with NEW set each channel's C0
   bits route it left or right, and a drum goes where its channel register sends it. */
static void opl_rhythm_sample(opl_state *st, int32_t *v6, int32_t *v7, int32_t *v8)
{
    int32_t mo, co, fbmod = 0;
    opl_op *m = &st->op[12], *cr = &st->op[15];
    opl_op *hh = &st->op[13], *sd = &st->op[16], *cy = &st->op[17];
    uint32_t p13, p17, p17_prev, b;
    *v6 = *v7 = *v8 = 0;

    /* BASS DRUM -- channel 6, an ordinary two-operator voice. */
    if (m->eg_state != OPL_EG_OFF || cr->eg_state != OPL_EG_OFF) {
        if (st->ch[6].fb) fbmod = (m->out1 + m->out2) >> (9 - st->ch[6].fb);
        mo = opl_op_sample(st, 6, 12, fbmod);
        m->out2 = m->out1; m->out1 = mo;
        opl_env_tick(st, 6, 12);
        if (st->ch[6].cnt) { co = opl_op_sample(st, 6, 15, 0); *v6 = (mo + co) * 2; }
        else               { co = opl_op_sample(st, 6, 15, mo); *v6 = co * 2; }
        opl_env_tick(st, 6, 15);
    }

    /* TOM-TOM -- op14 alone, on channel 8's pitch. */
    if (st->op[14].eg_state != OPL_EG_OFF) {
        *v8 = opl_op_sample(st, 8, 14, 0) * 2;
        opl_env_tick(st, 8, 14);
    }

    /* The two accumulators the other three read. They run every sample, keyed or
       not; the hi-hat takes op17's index from BEFORE this sample's step. */
    p17_prev = (cy->phase >> 10) & 0x3FF;
    hh->phase += opl_phase_inc(st, 7, hh) << (st->rhy_restart & 1);
    cy->phase += opl_phase_inc(st, 8, cy) << ((st->rhy_restart >> 1) & 1);
    st->rhy_restart = 0;
    p13 = (hh->phase >> 10) & 0x3FF;
    p17 = (cy->phase >> 10) & 0x3FF;

    /* HI-HAT -- op13's envelope and level, channel 7. */
    if (hh->eg_state != OPL_EG_OFF) {
        b = opl_rhythm_bit(p13, p17_prev);
        *v7 += opl_op_out(st, 7, hh, (b << 9) |
                          ((b ^ (st->noise >> OPL_NOISE_BIT_HH)) & 1 ? 0x0D0u : 0x034u)) * 2;
        opl_env_tick(st, 7, 13);
    }
    /* SNARE -- op16's envelope and level, on op13's bit 8, channel 7. */
    if (sd->eg_state != OPL_EG_OFF) {
        b = (p13 >> 8) & 1;
        *v7 += opl_op_out(st, 7, sd, (b << 9) |
                          (((b ^ (st->noise >> OPL_NOISE_BIT_SD)) & 1) << 8)) * 2;
        opl_env_tick(st, 7, 16);
    }
    /* CYMBAL -- op17's envelope and level, channel 8. */
    if (cy->eg_state != OPL_EG_OFF) {
        *v8 += opl_op_out(st, 8, cy, (opl_rhythm_bit(p13, p17) << 9) | 0x080u) * 2;
        opl_env_tick(st, 8, 17);
    }
}

/* --- one voice ------------------------------------------------------------ */
/* An ordinary two-operator channel: its contribution to the output, or 0 when
   both operators are off -- in which case neither phase nor envelope moves. An
   idle voice costs nothing, and the OPL2 golden depends on that staying so. */
static int32_t opl_voice2(opl_state *st, int c)
{
    int mi = vdd_opl_op_index(c, 0), ci = vdd_opl_op_index(c, 1);
    opl_op *m = &st->op[mi], *cr = &st->op[ci];
    int32_t mo, co, fbmod = 0, v;

    if (m->eg_state == OPL_EG_OFF && cr->eg_state == OPL_EG_OFF) return 0;

    /* Feedback uses the mean of the operator's last two outputs, which is
       what keeps a self-modulating operator stable instead of screaming. */
    if (st->ch[c].fb)
        fbmod = (m->out1 + m->out2) >> (9 - st->ch[c].fb);

    mo = opl_op_sample(st, c, mi, fbmod);
    m->out2 = m->out1; m->out1 = mo;
    opl_env_tick(st, c, mi);

    if (st->ch[c].cnt) {                        /* additive: both operators heard  */
        co = opl_op_sample(st, c, ci, 0);
        v = mo + co;
    } else {                                    /* FM: modulator bends the carrier */
        co = opl_op_sample(st, c, ci, mo);
        v = co;
    }
    opl_env_tick(st, c, ci);
    return v;
}

/* ── A 4-OPERATOR VOICE (OPL3, NEW set, channel `c` leads the pair c / c+3).
     Operators 1-2 are channel c's, 3-4 channel c+3's. The two CNT bits -- c's
     first, c+3's second -- choose the algorithm (YMF262 datasheet, 4-operator
     connection figure); "->" is phase modulation, "+" is summed to the output:
       CNT 0,0   1 -> 2 -> 3 -> 4                 one FM chain; 4 is heard
       CNT 1,0   1  +  (2 -> 3 -> 4)              1 and 4 heard
       CNT 0,1   (1 -> 2)  +  (3 -> 4)            2 and 4 heard
       CNT 1,1   1  +  (2 -> 3)  +  4             1, 3 and 4 heard
     Everything the voice has ONE of comes from channel c: the pitch (all four
     operators run on c's F-number and block, key scaling included), the key-on,
     the output routing, and the feedback -- which only ever applies to operator 1.
     Channel c+3's own F-number, block, key, feedback and routing are ignored
     while it is paired. */
static int32_t opl_voice4(opl_state *st, int c)
{
    int o1 = vdd_opl_op_index(c, 0),     o2 = vdd_opl_op_index(c, 1);
    int o3 = vdd_opl_op_index(c + 3, 0), o4 = vdd_opl_op_index(c + 3, 1);
    opl_op *p1 = &st->op[o1];
    int cnt1 = st->ch[c].cnt, cnt2 = st->ch[c + 3].cnt;
    int32_t s1, s2, s3, s4, fbmod = 0;

    if (p1->eg_state == OPL_EG_OFF && st->op[o2].eg_state == OPL_EG_OFF &&
        st->op[o3].eg_state == OPL_EG_OFF && st->op[o4].eg_state == OPL_EG_OFF)
        return 0;

    if (st->ch[c].fb) fbmod = (p1->out1 + p1->out2) >> (9 - st->ch[c].fb);
    s1 = opl_op_sample(st, c, o1, fbmod);
    p1->out2 = p1->out1; p1->out1 = s1;
    opl_env_tick(st, c, o1);

    s2 = opl_op_sample(st, c, o2, cnt1 ? 0 : s1);           /* 1 -> 2 unless CNT1   */
    opl_env_tick(st, c, o2);
    s3 = opl_op_sample(st, c, o3, (cnt2 && !cnt1) ? 0 : s2); /* 2 -> 3 except 0,1   */
    opl_env_tick(st, c, o3);
    s4 = opl_op_sample(st, c, o4, (cnt1 && cnt2) ? 0 : s3);  /* 3 -> 4 except 1,1   */
    opl_env_tick(st, c, o4);

    if (!cnt1 && !cnt2) return s4;              /* FM-FM                           */
    if ( cnt1 && !cnt2) return s1 + s4;         /* AM-FM                           */
    if (!cnt1 &&  cnt2) return s2 + s4;         /* FM-AM                           */
    return s1 + s3 + s4;                        /* AM-AM                           */
}

/* Add channel `c`'s output to the two sides. Without NEW the chip has one output
   and it goes to both, as it always did. With NEW, C0 bits 4/5 (outputs A/B) are
   the SB16's left/right; C/D (bits 6/7) reach no DAC on that card and are dropped
   -- a voice routed only there is silent, as on the real card, and so is a voice
   with no routing bits at all (a driver that sets NEW must set them). */
static void opl_route(const opl_state *st, int newm, int c, int32_t v, int32_t *l, int32_t *r)
{
    uint8_t c0;
    if (!newm) { *l += v; *r += v; return; }
    c0 = st->reg[(c / OPL_NUM_CH) * 0x100 + 0xC0 + c % OPL_NUM_CH];
    if (c0 & OPL_C0_CHA) *l += v;
    if (c0 & OPL_C0_CHB) *r += v;
}

/* One native sample, both sides, unclipped. The ORDER is the OPL2's exactly --
   rhythm, then the LFO tick, then channels 0-8 -- because opl_synth_test's golden
   checksum holds OPL2 output bit-identical to the build before OPL3 existed. */
static void opl_sample_lr(opl_state *st, int32_t *pl, int32_t *pr)
{
    int newm = vdd_opl_new_mode(st);
    int rhythm = (st->reg[0xBD] & OPL_BD_RHY) ? 1 : 0;
    int nch = newm ? OPL3_NUM_CH : OPL_NUM_CH, c;
    int32_t l = 0, r = 0;

    /* The noise generator runs from power-on whatever the mode, like the LFOs:
       the drums read it where it has got to, never from a restart. */
    st->noise = opl_noise_step(st->noise);
    if (rhythm) {
        int32_t v6, v7, v8;
        opl_rhythm_sample(st, &v6, &v7, &v8);
        opl_route(st, newm, 6, v6, &l, &r);
        opl_route(st, newm, 7, v7, &l, &r);
        opl_route(st, newm, 8, v8, &l, &r);
    }
    /* Outside the channel loop, and before the early-outs below: the LFOs run
       whether or not anything is sounding. Advancing them only while a note
       plays would restart the sweep at every silence. */
    st->lfo_count++;
    for (c = 0; c < nch; ++c) {
        int role;
        if (rhythm && c >= 6 && c <= 8) continue;   /* 6-8 are percussion in rhythm */
        role = newm ? opl_4op_role(st, c) : 0;
        if (role == 2) continue;                    /* rendered with its leader     */
        opl_route(st, newm, c, role ? opl_voice4(st, c) : opl_voice2(st, c), &l, &r);
    }
    *pl = l; *pr = r;
}

static int16_t opl_clip(int32_t v)
{
    return (int16_t)(v > 32767 ? 32767 : (v < -32768 ? -32768 : v));
}

/* --- public: render ------------------------------------------------------- */
void vdd_opl_render(opl_state *st, int16_t *out, uint32_t frames)
{
    uint32_t n;
    for (n = 0; n < frames; ++n) {
        int32_t l, r;
        opl_sample_lr(st, &l, &r);
        /* Without NEW, l == r IS the chip's one output: returned as it always was.
           With NEW, the fold the mixer's own mono path uses. */
        out[n] = vdd_opl_new_mode(st) ? (int16_t)(((int32_t)opl_clip(l) + opl_clip(r)) / 2)
                                      : opl_clip(l);
    }
}

void vdd_opl_render_st(opl_state *st, int16_t *out, uint32_t frames)
{
    uint32_t n;
    for (n = 0; n < frames; ++n) {
        int32_t l, r;
        opl_sample_lr(st, &l, &r);
        out[2 * n]     = opl_clip(l);
        out[2 * n + 1] = opl_clip(r);
    }
}
