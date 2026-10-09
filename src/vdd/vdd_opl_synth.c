/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * OPL2 (YM3812) and OPL3 (YMF262) FM synthesis for vdd_opl.c.
 *
 * Pure C, no <windows.h>, no libm: all transcendentals come from the generated
 * tables.
 *
 * The OPL3 is the OPL2 twice over plus three things, each gated on NEW (0x105
 * bit 0) and each below: waveforms 4-7 (OplWaveform), 4-operator voices
 * (OplVoiceFourOperator), and per-channel stereo routing (OplRoute). With NEW clear the
 * output is the OPL2's, sample for sample -- held by a golden checksum in
 * tests/unit/opl_synth_test.c. Written, like the rest, from the Yamaha
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
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#include "vdd_opl.h"
#include "opl_tables.h"

/* The log domain and the phase. */
#define OPL_LOG_OCTAVE_SHIFT                8       /* 256 log units: a factor of two */
#define OPL_LOG_FRACTION_MASK               0xFF
#define OPL_EXP_SILENT_SHIFT                13      /* Past this, below one LSB */
#define OPL_PHASE_FRACTION_BITS             10      /* The accumulator is 10.10 fixed point */
#define OPL_PHASE_MASK                      0x3FF   /* A 10-bit phase index: one cycle */
#define OPL_PHASE_HALF_MASK                 0x1FF
#define OPL_PHASE_SECOND_HALF               0x200
#define OPL_PHASE_ODD_QUARTER               0x100
#define OPL_PHASE_HALF_SHIFT                9
#define OPL_QUARTER_SHIFT                   8       /* The sine table holds one quarter */
#define OPL_QUARTER_MASK                    0xFF
#define OPL_QUARTER_LAST                    255
#define OPL_QUADRANT_MASK                   3
#define OPL_QUADRANT_MIRRORED               1
#define OPL_QUADRANT_NEGATIVE               2

/* Waveforms (0 is the sine). 1-3 are the OPL2's; 4-7 the OPL3's, with NEW set. */
#define OPL_WAVEFORM_HALF_SINE              1
#define OPL_WAVEFORM_ABSOLUTE_SINE          2
#define OPL_WAVEFORM_PULSE_SINE             3
#define OPL_WAVEFORM_DOUBLE_SINE            4
#define OPL_WAVEFORM_DOUBLE_ABSOLUTE_SINE   5
#define OPL_WAVEFORM_SQUARE                 6
#define OPL_WAVEFORM_DERIVED_SQUARE         7
#define OPL_WAVEFORM_OPL2_MASK              3
#define OPL3_NEW_BIT                        1       /* OPL3_REGISTER_NEW bit 0 */

/* Rates, envelope, key scaling. */
#define OPL_RATE_STEPS                      4       /* Effective rate = 4 x register + offset */
#define OPL_RATE_MAX                        63
#define OPL_RATE_LOW_MASK                   3
#define OPL_RATE_OCTAVE_SHIFT               2
#define OPL_FNUMBER_TOP_BIT_SHIFT           9
#define OPL_KEY_SCALE_RATE_OFF_SHIFT        2       /* KSR clear: the offset quartered */
#define OPL_ENVELOPE_MANTISSA_BASE          4       /* (4 + rate_lo): see OPL_ENVELOPE_DIVIDER_SHIFT */
#define OPL_ATTACK_RATE_INSTANT             15
#define OPL_SUSTAIN_LEVEL_BOTTOM            15      /* "all the way down" */
#define OPL_SUSTAIN_LEVEL_UNITS             16      /* Envelope units per SL step (3 dB) */
#define OPL_MULTIPLIERS                     16
#define OPL_KSL_ROM_ENTRIES                 16
#define OPL_KSL_SHIFTS                      4
#define OPL_KSL_FNUMBER_SHIFT               6       /* The top 4 bits of the F-number */
#define OPL_KSL_FNUMBER_MASK                0x0F
#define OPL_KSL_UNITS_PER_OCTAVE            8
#define OPL_KSL_OCTAVE_ORIGIN               8

/* The LFOs. */
#define OPL_TREMOLO_SHALLOW_SHIFT           2       /* DAM=0: a quarter as deep */
#define OPL_VIBRATO_STEPS                   8
#define OPL_VIBRATO_STEP_MASK               7
#define OPL_VIBRATO_FULL                    2       /* g_OplVibratoPattern's full-depth entry */
#define OPL_VIBRATO_DEEP_SHIFT              7       /* DVB=1: F-number >> 7 */
#define OPL_VIBRATO_SHALLOW_SHIFT           8

/* Voices and output. */
#define OPL_FEEDBACK_BASE_SHIFT             9       /* Feedback FB: the mean >> (9 - FB) */
#define OPL_RHYTHM_GAIN                     2       /* The drums are summed at double amplitude */
#define OPL_NOISE_NEAREST_TAP               9
#define OPL_NOISE_TAP_SHIFT                 14      /* 23 - 9 */
#define OPL_NOISE_NEW_BITS_MASK             0x1FF
#define OPL_HIHAT_PHASE_SET                 0x0D0u  /* B ^ noise set */
#define OPL_HIHAT_PHASE_CLEAR               0x034u
#define OPL_SNARE_PHASE_BIT                 8       /* S = bit 8 of op13's index */
#define OPL_SNARE_NOISE_SHIFT               8
#define OPL_CYMBAL_PHASE_LOW                0x080u
#define OPL_SAMPLE_MAX                      32767
#define OPL_SAMPLE_MIN                      (-32768)
#define OPL_MONO_FOLD_DIVISOR               2       /* The average of left and right */

/* MULT register -> frequency multiplier, doubled so that entry 0 (x0.5) is an
 * integer. The duplicated 20/20 and 24/24 and 30/30 entries are the chip's, not
 * a typo: MULT 11, 13 and 14 alias their neighbours.
 */
static const BYTE g_OplMultiplier2[OPL_MULTIPLIERS] =
    { 1, 2, 4, 6, 8, 10, 12, 14, 16, 18, 20, 20, 24, 24, 30, 30 };

/* Key-scale-level ROM, indexed by the top 4 bits of F-num. MEASURED to be in units
 * of 0.75 dB, not the 0.375 this once assumed: at KSL=3 the reference attenuates
 * 6.02 dB per octave and the block term moves 8 ROM units per octave, so one unit
 * is 0.7526 dB. See OPL_KEY_SCALE_TO_LOG and `oplprobe ksl`.
 */
static const BYTE g_OplKeyScaleRom[OPL_KSL_ROM_ENTRIES] =
    { 0, 32, 40, 45, 48, 51, 53, 55, 56, 58, 59, 60, 61, 62, 63, 64 };
/* KSL field -> right shift. 0 means "no key scaling", expressed as a shift big
 * enough to annihilate the value.
 */
static const BYTE g_OplKeyScaleShift[OPL_KSL_SHIFTS] = { 8, 1, 2, 0 };

/* One log unit = 1/256 octave of amplitude. Envelope steps are 0.1875 dB and
 * total-level steps 0.75 dB, so they scale into log units by 8 and 32.
 */
#define OPL_ENVELOPE_TO_LOG             8
#define OPL_TOTAL_LEVEL_TO_LOG          32
#define OPL_KEY_SCALE_TO_LOG            32  /* 0.75 dB per ROM unit -- measured, see above */

/* ENVELOPE SPEED. Measured: `oplprobe egrate` times the reference's decay at every
 * effective rate and reports samples per envelope unit. The law it gives is
 *   units per sample = (4 + rate_lo) / 2^(15 - rate_hi)
 * where rate_hi/rate_lo are the top four and bottom two bits of the 6-bit rate.
 * Note the MANTISSA: inside a group of four rates the speed goes 4:5:6:7 -- linear,
 * not geometric -- and only the group boundary is a doubling. Measured across 30
 * rates and all four sub-steps, the implied divisor came out 32983/33007/33008/
 * 33006, i.e. 2^15 to within the measurement's own bias.
 * - The previous model shifted by whole octaves and rounded the sub-step away,
 *   which made mid-range decays and releases run 1.5x too slow.
 */
#define OPL_ENVELOPE_DIVIDER_SHIFT      15

/* ATTACK. Not linear: the attenuation loses a FRACTION OF ITSELF each sample, so
 * the note rushes up and then eases in. `oplprobe attack` fits that fraction
 * against the decay speed at the same rate and gets 0.1435 -- constant to +-1.5%
 * over 19 rates and all four sub-steps, which is what says the shape is right and
 * not merely the endpoint. 147/1024 is that number.
 */
#define OPL_ENVELOPE_ATTACK_NUMERATOR   147
#define OPL_ENVELOPE_ATTACK_SHIFT       10

/* exp2(-x/256) * 4096 for an arbitrary x, via the table plus a shift. */
static INT32 OplExp2Negative(INT32 logValue)
{
    INT shift;
    if (logValue < 0) logValue = 0;
    shift = logValue >> OPL_LOG_OCTAVE_SHIFT;
    if (shift > OPL_EXP_SILENT_SHIFT) return 0;                   /* below one LSB: silent */
    return (INT32)g_OplExp[logValue & OPL_LOG_FRACTION_MASK] >> shift;
}

/* Full-cycle sine in the log domain: returns the attenuation for this phase and
 * sets *isNegative for the negative half. The table holds one quarter, mirrored twice.
 */
static INT32 OplLogSinFull(UINT32 phaseIndex, INT *isNegative)
{
    UINT32 quadrant = (phaseIndex >> OPL_QUARTER_SHIFT) & OPL_QUADRANT_MASK, index = phaseIndex & OPL_QUARTER_MASK;
    *isNegative = (quadrant & OPL_QUADRANT_NEGATIVE) ? 1 : 0;
    return (INT32)g_OplLogSin[(quadrant & OPL_QUADRANT_MIRRORED) ? (OPL_QUARTER_LAST - index) : index];
}

/* The OPL2's four waveforms are cheap edits of the sine: 1 clips the negative
 * half to zero, 2 rectifies it, 3 keeps only the rising quarters.
 * The OPL3 adds four more (YMF262 datasheet, waveform figure), all of which live
 * in the FIRST half-cycle or are antisymmetric about its end:
 * 4  a full sine at twice the rate in the first half, silence in the second
 * 5  the same, rectified ("camel")
 * 6  a square: full level, positive then negative
 * 7  the "derived square": an exponential fall from full level across the first
 *    half, and its point-mirror (negative, rising back to full) across the second.
 * In the log domain 7 is a straight line -- attenuation growing linearly with
 * phase. [CAUTION] The datasheet draws the curve but gives no slope; this uses one log
 * unit per 1/8 phase step, i.e. a factor of two every 32 of the 512 steps, so the
 * curve reaches silence by the end of its half-cycle. That slope is owed an
 * `oplprobe wave` measurement against the oracle, as every constant here was.
 */
#define OPL_WAVEFORM7_SLOPE_SHIFT   3

static INT32 OplWaveform(BYTE waveform, UINT32 phaseIndex, INT *isNegative, INT *isMuted)
{
    *isMuted = 0;
    switch (waveform)
    {
    case OPL_WAVEFORM_DOUBLE_SINE:                                     /* double-rate sine, 1st half */
        if (phaseIndex & OPL_PHASE_SECOND_HALF)
        {
            *isMuted = 1;
            *isNegative = 0;
            return 0;
        }
        return OplLogSinFull((phaseIndex << 1) & OPL_PHASE_MASK, isNegative);
    case OPL_WAVEFORM_DOUBLE_ABSOLUTE_SINE:                                     /* double-rate |sine|, 1st half */
        if (phaseIndex & OPL_PHASE_SECOND_HALF)
        {
            *isMuted = 1;
            *isNegative = 0;
            return 0;
        }
        {
            INT32 value = OplLogSinFull((phaseIndex << 1) & OPL_PHASE_MASK, isNegative);
            *isNegative = 0;
            return value;
        }
    case OPL_WAVEFORM_SQUARE:                                     /* square */
        *isNegative = (phaseIndex & OPL_PHASE_SECOND_HALF) ? 1 : 0;
        return 0;
    case OPL_WAVEFORM_DERIVED_SQUARE:                                     /* derived square (log sawtooth) */
        *isNegative = (phaseIndex & OPL_PHASE_SECOND_HALF) ? 1 : 0;
        return (INT32)(*isNegative ? ((phaseIndex & OPL_PHASE_HALF_MASK) ^ OPL_PHASE_HALF_MASK) : (phaseIndex & OPL_PHASE_HALF_MASK))
               << OPL_WAVEFORM7_SLOPE_SHIFT;
    case OPL_WAVEFORM_HALF_SINE:                                     /* half-wave rectified */
        if (phaseIndex & OPL_PHASE_SECOND_HALF)
        {
            *isMuted = 1;
            *isNegative = 0;
            return 0;
        }
        return OplLogSinFull(phaseIndex, isNegative);
    case OPL_WAVEFORM_ABSOLUTE_SINE:                                     /* absolute value */
        {
            INT32 value = OplLogSinFull(phaseIndex, isNegative);
            *isNegative = 0;
            return value;
        }
    case OPL_WAVEFORM_PULSE_SINE:                                     /* pulse-sine (rising quarters) */
        if (phaseIndex & OPL_PHASE_ODD_QUARTER)
        {
            *isMuted = 1;
            *isNegative = 0;
            return 0;
        }
        {
            INT32 value = OplLogSinFull(phaseIndex, isNegative);
            *isNegative = 0;
            return value;
        }
    default:
        return OplLogSinFull(phaseIndex, isNegative);
    }
}

/* -- WHICH WAVEFORM ACTUALLY PLAYS. Three chips' worth of rules over one 3-bit
 * field, decided HERE (at render time) because the gating registers can be
 * written after the waveform is:
 * OPL2         2 bits, and only while WSE (0x01 bit 5) is set -- with WSE
 *              clear the YM3812 plays a sine whatever 0xE0 says (YM3812
 *              application manual, register 01). Programs that forget WSE
 *              exist, and on the real card they get sines.
 * OPL3, NEW=0  2 bits. The YMF262 has no WSE: its register 0x01 is the LSI
 *              test register only, and the OPL2 waveforms are always live.
 * OPL3, NEW=1  all 3 bits -- waveforms 4-7.
 */
static BYTE OplEffectiveWaveform(PCOPL_STATE state, PCOPL_OPERATOR operatorState)
{
    if (state->IsOpl3) return (BYTE)(operatorState->Waveform & ((state->Registers[OPL3_REGISTER_NEW] & OPL3_NEW_BIT) ? OPL_WAVEFORM_MASK : OPL_WAVEFORM_OPL2_MASK));
    return (state->Registers[OPL_REGISTER_TEST] & OPL_TEST_WSE) ? (BYTE)(operatorState->Waveform & OPL_WAVEFORM_OPL2_MASK) : 0;
}

/* --- envelope ------------------------------------------------------------- */
/* Effective 6-bit rate: the 4-bit register value, scaled up by where the note
 * sits on the keyboard. High notes decay faster on a real OPL, and KSR selects
 * how strongly that applies.
 */
static INT OplEffectiveRate(PCOPL_STATE state, INT channel, BYTE rate4, BYTE keyScaleRate)
{
    INT keyScaleValue = (state->Channels[channel].Block << 1) | ((state->Channels[channel].FNumber >> OPL_FNUMBER_TOP_BIT_SHIFT) & 1);
    INT rateOffset = keyScaleRate ? keyScaleValue : (keyScaleValue >> OPL_KEY_SCALE_RATE_OFF_SHIFT);
    INT rate;
    if (!rate4) return 0;                       /* rate 0 never moves */
    rate = rate4 * OPL_RATE_STEPS + rateOffset;
    return rate > OPL_RATE_MAX ? OPL_RATE_MAX : rate;
}

/* Envelope step per sample, in env fixed point. See OPL_ENVELOPE_DIVIDER_SHIFT. */
static INT32 OplEnvelopeStep(INT rate)
{
    if (!rate) return 0;                        /* rate 0 never moves */
    return (INT32)(OPL_ENVELOPE_MANTISSA_BASE + (rate & OPL_RATE_LOW_MASK)) << (OPL_ENVELOPE_SHIFT - OPL_ENVELOPE_DIVIDER_SHIFT + (rate >> OPL_RATE_OCTAVE_SHIFT));
}

static VOID OplEnvelopeTick(POPL_STATE state, INT channel, INT operatorIndex)
{
    POPL_OPERATOR operatorState = &state->Operators[operatorIndex];
    INT32 step;
    switch (operatorState->EnvelopeState)
    {
    case OPL_ENVELOPE_ATTACK:
        step = OplEnvelopeStep(OplEffectiveRate(state, channel, operatorState->AttackRate, operatorState->KeyScaleRate));
        /* AR=0 is not "instant", it is NEVER: the operator stays fully attenuated
         * and the note is silent. Confirmed against the reference, which produces
         * a peak of 1 against our 4096 before this was fixed -- reading it the
         * other way turns silent voices into loud ones.
         */
        if (!step) break;
        if (operatorState->AttackRate == OPL_ATTACK_RATE_INSTANT)
        {
            operatorState->Envelope = 0;
            operatorState->EnvelopeState = OPL_ENVELOPE_DECAY;
            break;
        }
        /* Attack is exponential: the closer to full volume, the slower it moves.
         * Scaling the step by the remaining attenuation gives that curve without
         * a second table. The +1 unit keeps it moving once the product would
         * otherwise round to nothing, so a slow attack still finishes.
         */
        { INT64 decrement = (((INT64)step * (operatorState->Envelope + (1 << OPL_ENVELOPE_SHIFT))) >> OPL_ENVELOPE_SHIFT);
          decrement = (decrement * OPL_ENVELOPE_ATTACK_NUMERATOR) >> OPL_ENVELOPE_ATTACK_SHIFT;
          if (decrement < 1) decrement = 1;
          operatorState->Envelope -= (INT32)decrement; }
        if (operatorState->Envelope <= 0)
        {
            operatorState->Envelope = 0;
            operatorState->EnvelopeState = OPL_ENVELOPE_DECAY;
        }
        break;
    case OPL_ENVELOPE_DECAY:
        step = OplEnvelopeStep(OplEffectiveRate(state, channel, operatorState->DecayRate, operatorState->KeyScaleRate));
        operatorState->Envelope += step;
        /* SL is 4 bits of 3 dB each; 15 means "all the way down". */
        { INT32 sustainLevel = (operatorState->SustainLevel == OPL_SUSTAIN_LEVEL_BOTTOM) ? (OPL_ENVELOPE_MAX << OPL_ENVELOPE_SHIFT)
                                     : ((INT32)operatorState->SustainLevel * OPL_SUSTAIN_LEVEL_UNITS) << OPL_ENVELOPE_SHIFT;
          if (operatorState->Envelope >= sustainLevel)
          {
              operatorState->Envelope = sustainLevel;
              operatorState->EnvelopeState = OPL_ENVELOPE_SUSTAIN;
          }
          }
        break;
    case OPL_ENVELOPE_SUSTAIN:
        /* EGT selects sustaining (hold) versus percussive (keep decaying). */
        if (!operatorState->EnvelopeType)
        {
            operatorState->Envelope += OplEnvelopeStep(OplEffectiveRate(state, channel, operatorState->ReleaseRate, operatorState->KeyScaleRate));
            if (operatorState->Envelope >= (OPL_ENVELOPE_MAX << OPL_ENVELOPE_SHIFT))
            {
                operatorState->Envelope = OPL_ENVELOPE_MAX << OPL_ENVELOPE_SHIFT; operatorState->EnvelopeState = OPL_ENVELOPE_OFF;
            }
        }
        break;
    case OPL_ENVELOPE_RELEASE:
        operatorState->Envelope += OplEnvelopeStep(OplEffectiveRate(state, channel, operatorState->ReleaseRate, operatorState->KeyScaleRate));
        if (operatorState->Envelope >= (OPL_ENVELOPE_MAX << OPL_ENVELOPE_SHIFT))
        {
            operatorState->Envelope = OPL_ENVELOPE_MAX << OPL_ENVELOPE_SHIFT; operatorState->EnvelopeState = OPL_ENVELOPE_OFF;
        }
        break;
    default:
        operatorState->Envelope = OPL_ENVELOPE_MAX << OPL_ENVELOPE_SHIFT;
        break;
    }
    if (operatorState->Envelope < 0) operatorState->Envelope = 0;
    if (operatorState->Envelope > (OPL_ENVELOPE_MAX << OPL_ENVELOPE_SHIFT)) operatorState->Envelope = OPL_ENVELOPE_MAX << OPL_ENVELOPE_SHIFT;
}

/* --- the two low-frequency oscillators ------------------------------------ */
/* TREMOLO. MEASURED (`oplprobe lfo`): a 52-step triangle climbing to 26 envelope
 * units and back, one step every 256 samples -- 3.73 Hz, and 4.89 dB deep at DAM=1
 * against 4.87 measured. It is a STAIRCASE, not a sine: the reference's amplitude
 * moves in exact 0.188 dB steps, one envelope unit at a time. DAM=0 is the same
 * counter shifted down two places, which is why its steps last four times as long
 * and it measures a quarter as deep.
 */
#define OPL_TREMOLO_STEP_SHIFT  8   /* 256 samples per tremolo step */
#define OPL_TREMOLO_STEPS       52
#define OPL_TREMOLO_PEAK        26

static INT32 OplTremoloUnits(PCOPL_STATE state)
{
    UINT32 step = (state->LfoCount >> OPL_TREMOLO_STEP_SHIFT) % OPL_TREMOLO_STEPS;
    INT32  units = (step < OPL_TREMOLO_PEAK) ? (INT32)step : (INT32)(OPL_TREMOLO_STEPS - step);
    return (state->Registers[OPL_REGISTER_RHYTHM] & OPL_BD_TREMOLO_DEPTH) ? units : (units >> OPL_TREMOLO_SHALLOW_SHIFT);
}

/* VIBRATO. MEASURED: eight steps of 1024 samples -- 49716/8192 = 6.069 Hz, which
 * is what the reference reads to four figures -- following the pattern
 * 0, half, full, half, 0, -half, -full, -half.
 * The depth is a SHIFT of the F-number, not a fixed number of cents: fnum >> 7,
 * halved again when DVB is clear. That predicts 25.23 cents peak-to-peak at
 * F-num 0x3C0 against 25.17 measured, and 27.05 at 0x200 against 27.07 -- and it
 * means the effect QUANTISES, so an F-num below 128 gets no vibrato at all. A
 * constant-cents implementation matches at one pitch and drifts at every other.
 */
#define OPL_VIBRATO_STEP_SHIFT  10  /* 1024 samples per vibrato step */
static const INT8 g_OplVibratoPattern[OPL_VIBRATO_STEPS] = { 0, 1, 2, 1, 0, -1, -2, -1 };

static INT32 OplVibratoOffset(PCOPL_STATE state, INT channel)
{
    INT32 fullDepth = (INT32)state->Channels[channel].FNumber >> ((state->Registers[OPL_REGISTER_RHYTHM] & OPL_BD_VIBRATO_DEPTH) ? OPL_VIBRATO_DEEP_SHIFT : OPL_VIBRATO_SHALLOW_SHIFT);
    INT     patternStep    = g_OplVibratoPattern[(state->LfoCount >> OPL_VIBRATO_STEP_SHIFT) & OPL_VIBRATO_STEP_MASK];
    INT32 offset    = (patternStep == OPL_VIBRATO_FULL || patternStep == -OPL_VIBRATO_FULL) ? fullDepth : (patternStep ? (fullDepth >> 1) : 0);
    return (patternStep < 0) ? -offset : offset;
}

/* --- operator ------------------------------------------------------------- */
/* Phase increment per native sample: (F-num << block) * multiplier / 2. With
 * multiplier 1 this gives f = fnum * 49716 / 2^(20-block), the chip's formula.
 * Vibrato rides on the F-number itself, so it scales with block and multiplier
 * exactly as the pitch does -- which is why the effect is a constant interval
 * rather than a constant number of hertz.
 */
static UINT32 OplPhaseIncrement(PCOPL_STATE state, INT channel, PCOPL_OPERATOR operatorState)
{
    INT32 fNumber = (INT32)state->Channels[channel].FNumber;
    UINT32 baseIncrement;
    if (operatorState->Vibrato) fNumber += OplVibratoOffset(state, channel);
    if (fNumber < 0) fNumber = 0;
    baseIncrement = (UINT32)fNumber << state->Channels[channel].Block;
    return (baseIncrement * g_OplMultiplier2[operatorState->Multiplier]) >> 1;
}

/* Static attenuation from total level and key scaling, in log units. */
static INT32 OplStaticAttenuation(PCOPL_STATE state, INT channel, PCOPL_OPERATOR operatorState)
{
    INT32 attenuation = (INT32)operatorState->TotalLevel * OPL_TOTAL_LEVEL_TO_LOG;
    /* The octave origin is 8, not 7. Measured: at KSL=3 with F-num's top nibble at
     * 15 the reference is still at full volume in block 0 and down 6 dB in block 1,
     * which places the zero one octave lower than this had it. Being one octave out
     * under-attenuates every high note -- at block 7 by 6 dB, and by 18 dB once the
     * KSL=3 shift is applied -- so bass and treble sit at the wrong relative
     * levels across the whole keyboard.
     */
    INT32 keyScale = (INT32)g_OplKeyScaleRom[(state->Channels[channel].FNumber >> OPL_KSL_FNUMBER_SHIFT) & OPL_KSL_FNUMBER_MASK]
              - OPL_KSL_UNITS_PER_OCTAVE * (OPL_KSL_OCTAVE_ORIGIN - (INT32)state->Channels[channel].Block);
    if (keyScale < 0) keyScale = 0;
    attenuation += (keyScale >> g_OplKeyScaleShift[operatorState->KeyScaleLevel]) * OPL_KEY_SCALE_TO_LOG;
    return attenuation;
}

/* MODULATION DEPTH. An operator's output runs to +-4096 at full volume and that
 * value is added STRAIGHT into the 1024-step phase index -- so a modulator at
 * TL=0 swings the carrier through four whole cycles. MEASURED: `oplprobe mod`
 * fits the modulation index from the sideband amplitudes and reads 4.000 cycles
 * for the reference against our 2.000, a ratio of exactly 0.501 held across the
 * whole TL sweep. We were halving it on the way in.
 * - This is why the timbre was wrong while the tune was right. Halving the index
 *   does not change pitch, tempo or loudness -- phase modulation preserves total
 *   power -- it changes only WHICH harmonics are present and how strongly, which
 *   is precisely "the right tune, but the instruments sound flat".
 * - Feedback measured the same way: our FB=n matched the reference's FB=n-1 step
 *   for step, the same factor of two, in the same place.
 */

/* An operator's output at phase index `phaseIndex` (0-1023): waveform, envelope, level,
 * tremolo. Split out of OplOperatorSample for the rhythm voices, whose phase is not
 * their own accumulator's.
 */
static INT32 OplOperatorOutput(PCOPL_STATE state, INT channel, PCOPL_OPERATOR operatorState, UINT32 phaseIndex)
{
    INT32 logValue, attenuation, amplitude;
    INT isNegative = 0, isMuted = 0;

    logValue = OplWaveform(OplEffectiveWaveform(state, operatorState), phaseIndex & OPL_PHASE_MASK, &isNegative, &isMuted);
    if (isMuted) return 0;

    attenuation = logValue + (operatorState->Envelope >> OPL_ENVELOPE_SHIFT) * OPL_ENVELOPE_TO_LOG + OplStaticAttenuation(state, channel, operatorState);
    if (operatorState->AmplitudeModulation) attenuation += OplTremoloUnits(state) * OPL_ENVELOPE_TO_LOG;
    amplitude = OplExp2Negative(attenuation);
    return isNegative ? -amplitude : amplitude;
}

/* One operator sample. `modulation` is a phase offset in sine-table steps (FM input). */
static INT32 OplOperatorSample(POPL_STATE state, INT channel, INT operatorIndex, INT32 modulation)
{
    POPL_OPERATOR operatorState = &state->Operators[operatorIndex];
    operatorState->Phase += OplPhaseIncrement(state, channel, operatorState);
    if (operatorState->EnvelopeState == OPL_ENVELOPE_OFF) return 0;
    return OplOperatorOutput(state, channel, operatorState, (operatorState->Phase >> OPL_PHASE_FRACTION_BITS) + (UINT32)modulation);
}

/* --- rhythm mode ---------------------------------------------------------- *
 * With 0xBD bit 5 set, channels 6-8 stop being melodic voices and become five
 * percussion ones. Everything below was mapped from the OUTSIDE (`oplprobe
 * rhythm`) rather than written down from memory: each operator was silenced in
 * turn to find whose ENVELOPE drives which drum, and each operator's MULT was
 * doubled in turn to find whose PHASE it runs on. Those are not the same answer,
 * and assuming they were would have got the snare wrong.
 *
 * voice       envelope   phase        character (measured)
 * bass drum   op12+op15  own          tonality 1.000 -- ordinary 2-op FM
 * tom-tom     op14       own          tonality 1.000 -- a plain sine
 * snare       op16       OP13's       tonality 0.509 -- half tone, half noise
 * hi-hat      op13       op13 + op17  tonality 0.003 -- essentially pure noise
 * cymbal      op17       op13 + op17  tonality 0.749
 *
 * Also measured, and not something to guess at: the percussion voices are summed
 * at DOUBLE amplitude. A tom-tom peaks at 8170 where an ordinary operator at the
 * same settings peaks at 4085.
 *
 * -- SNARE, HI-HAT AND CYMBAL (#139). These three read a PHASE THEY DO NOT OWN: a
 *  10-bit index built from bits of op13's and op17's accumulators and from a
 *  noise bit, which then goes through the operator's waveform, envelope and level
 *  exactly as an ordinary phase would. Everything here was READ OUT of the
 *  reference's output, never taken from its source (`oplprobe rphase`, `noise`):
 *  run the drum eight times, once per waveform, and invert the eight output
 *  samples against a table of the same eight waveforms measured on the tom-tom
 *  (an ordinary operator) -- which gives the 10-bit phase index the drum used at
 *  every sample. What came out:
 *
 *    voice    phase index (measured, 0 mispredictions in 11936 samples, on a
 *             register setup the fit never saw)
 *    hi-hat   B<<9 | (B ^ noise ? 0x0D0 : 0x034)
 *    snare    S<<9 | (S ^ noise) << 8          S = bit 8 of op13's index
 *    cymbal   B<<9 | 0x080
 *
 *  where B, "the phase bit", is a function of five accumulator bits and nothing
 *  else. A search over every subset of up to five of the twenty bits of the two
 *  indices found exactly ONE subset that determines it -- op13 bits 2, 3, 7 and
 *  op17 bits 3, 5 -- and its truth table is zero in exactly four of 32 cells:
 *    B = (p13.2 ^ p13.7) | (p13.3 ^ p17.5) | (p17.3 ^ p17.5)
 *  (p13.3 ^ p17.3 in the middle term is the same function.) [CAUTION] Not a guess to be
 *  "corrected" from memory: the obvious-looking alternative with p13.3 OR'd in on
 *  its own is REFUTED by the table -- cell 11010 reads 0.
 *
 *  TIMING, also measured (the subset search fits the sample offsets too): the
 *  hi-hat sees op13's index for THIS sample but op17's from the PREVIOUS one; the
 *  cymbal and snare see both current. The reference also delivers its carrier
 *  slots one sample later than ours -- snare and cymbal best-align at lag +4
 *  like the bass drum, the hi-hat at +3 like the tom-tom. That is the pipeline,
 *  not the waveform: see the lag note in `oplprobe rhythm`.
 *
 *  op13 and op17 run their phase every sample in rhythm mode whether or not
 *  their own drum is keyed (the hi-hat alone still reads op17's bits). A drum's
 *  key-on restarts its own operator's accumulator -- and WHERE it restarts
 *  matters here as it does nowhere else, because the other accumulator is not
 *  restarted with it. MEASURED (`oplprobe restart`, key-on at samples 1, 2, 37,
 *  3001): a key-on restarts the accumulator ONE STEP FURTHER ON than the
 *  running one's own start from reset would put it -- so its first sample reads
 *  two steps, not one (RhythmRestart, set in vdd_opl.c). A key-on before the
 *  first sample after reset is the exception and reads like reset itself, which
 *  is why the default experiment never saw it. Without this the hi-hat and
 *  cymbal keyed into a running chip score 0.71 / 0.70; with it 0.9999, the rest
 *  being two samples of key-on latency the tom-tom shares. (Ordinary operators
 *  restart the same way, but there it is a one-sample shift of a lone waveform
 *  against its envelope -- inaudible, invisible at best lag, and the OPL2
 *  golden holds it -- so only these two accumulators model it.)
 *
 * -- THE NOISE (`oplprobe noise`). With op13/op17 frozen (F-num 0) B is 0, so the
 *  hi-hat and snare phases above carry nothing but the noise bit. Both streams
 *  have linear complexity 23 (Berlekamp-Massey) with the SAME recurrence,
 *      s[n] = s[n-1] ^ s[n-8] ^ s[n-9] ^ s[n-23]
 *  which is precisely the trinomial LFSR  u[i] = u[i-9] ^ u[i-23]  (period
 *  2^23-1) sampled once every 9*2^k of its own steps -- and 72 = 9*8 is the
 *  chip's master clocks per output sample. The decimation is not a free choice:
 *  solving for the one state that yields the hi-hat's stream, the snare's stream
 *  turns out to be the SAME register read 66 steps (0.917 samples) earlier under
 *  72 steps per sample (or 33 / 132 under 36 / 144, the same thing scaled),
 *  while under 9 or 18 it is on no shared register at all. 72 it is, and the
 *  state it solves to is ONE SET BIT (OPL_NOISE_SEED) at exactly the point our
 *  renderer starts from after reset. So the noise is sample-exact from
 *  power-on, not merely statistically alike: the hi-hat's and snare's decoded
 *  noise bits match the model at every one of 3936 samples.
 *  In our frame the snare's output sits one sample after the hi-hat's (the lag
 *  note above), so "66 steps before the hi-hat's NEXT sample" is bit 6 of the
 *  same 23-bit window whose bit 0 the hi-hat reads.
 *
 * Summed at double amplitude like the other drums. Hi-hat and snare follow channel
 * 7's C0 routing and the cymbal channel 8's, like the tom-tom -- measured, OPL3
 * with NEW set, one channel routed left at a time (`oplprobe drums`). The hit
 * counters (ProfileRhythmHits) stay: they say how much percussion a game uses.
 */

/* The noise register: 72 steps of u[i] = u[i-9] ^ u[i-23] per sample. `window` holds
 * the last 23 outputs, bit 0 the oldest; the nearest tap is 9 back, so nine new
 * bits can be made at once and eight of those make one sample's 72.
 */
#define OPL_NOISE_STEPS_PER_SAMPLE  72
static UINT32 OplNoiseStep(UINT32 window)
{
    INT group;
    if (!window) window = OPL_NOISE_SEED;         /* a never-reset struct: zero is a dead state */
    for (group = 0; group < OPL_NOISE_STEPS_PER_SAMPLE / OPL_NOISE_NEAREST_TAP; ++group)
        window = (window >> OPL_NOISE_NEAREST_TAP) | ((((window >> OPL_NOISE_TAP_SHIFT) ^ window) & OPL_NOISE_NEW_BITS_MASK) << OPL_NOISE_TAP_SHIFT);
    return window;
}

#define OPL_NOISE_BIT_HIHAT     0   /* Which bit of the window each drum reads -- the */
#define OPL_NOISE_BIT_SNARE     6   /* snare 6 steps later (see `oplprobe noise`) */

/* The phase bit B from op13's and op17's 10-bit indices. */
static UINT32 OplRhythmPhaseBit(UINT32 phase13, UINT32 phase17)
{
    return (((phase13 >> 2) ^ (phase13 >> 7)) | ((phase13 >> 3) ^ (phase17 >> 5)) | ((phase17 >> 3) ^ (phase17 >> 5))) & 1;
}

/* Returns the drums per CHANNEL -- bass drum on channel 6, hi-hat and snare on 7,
 * tom-tom and cymbal on 8 -- because on an OPL3 with NEW set each channel's C0
 * bits route it left or right, and a drum goes where its channel register sends it.
 */
static VOID OplRhythmSample(POPL_STATE state, INT32 *channel6Output, INT32 *channel7Output, INT32 *channel8Output)
{
    INT32 modulatorOutput, carrierOutput, feedbackModulation = 0;
    POPL_OPERATOR modulator = &state->Operators[OPL_OPERATOR_BASS_DRUM_MODULATOR], carrier = &state->Operators[OPL_OPERATOR_BASS_DRUM_CARRIER];
    POPL_OPERATOR hihat = &state->Operators[OPL_OPERATOR_HIHAT], snare = &state->Operators[OPL_OPERATOR_SNARE], cymbal = &state->Operators[OPL_OPERATOR_CYMBAL];
    UINT32 phase13, phase17, phase17Previous, phaseBit;
    *channel6Output = *channel7Output = *channel8Output = 0;

    /* BASS DRUM -- channel 6, an ordinary two-operator voice. */
    if (modulator->EnvelopeState != OPL_ENVELOPE_OFF || carrier->EnvelopeState != OPL_ENVELOPE_OFF)
    {
        if (state->Channels[OPL_RHYTHM_CHANNEL_BASS_DRUM].Feedback) feedbackModulation = (modulator->Output1 + modulator->Output2) >> (OPL_FEEDBACK_BASE_SHIFT - state->Channels[OPL_RHYTHM_CHANNEL_BASS_DRUM].Feedback);
        modulatorOutput = OplOperatorSample(state, OPL_RHYTHM_CHANNEL_BASS_DRUM, OPL_OPERATOR_BASS_DRUM_MODULATOR, feedbackModulation);
        modulator->Output2 = modulator->Output1; modulator->Output1 = modulatorOutput;
        OplEnvelopeTick(state, OPL_RHYTHM_CHANNEL_BASS_DRUM, OPL_OPERATOR_BASS_DRUM_MODULATOR);
        if (state->Channels[OPL_RHYTHM_CHANNEL_BASS_DRUM].Connection)
        {
            carrierOutput = OplOperatorSample(state, OPL_RHYTHM_CHANNEL_BASS_DRUM, OPL_OPERATOR_BASS_DRUM_CARRIER, 0);
            *channel6Output = (modulatorOutput + carrierOutput) * OPL_RHYTHM_GAIN;
        }
        else
        {
            carrierOutput = OplOperatorSample(state, OPL_RHYTHM_CHANNEL_BASS_DRUM, OPL_OPERATOR_BASS_DRUM_CARRIER, modulatorOutput);
            *channel6Output = carrierOutput * OPL_RHYTHM_GAIN;
        }
        OplEnvelopeTick(state, OPL_RHYTHM_CHANNEL_BASS_DRUM, OPL_OPERATOR_BASS_DRUM_CARRIER);
    }

    /* TOM-TOM -- op14 alone, on channel 8's pitch. */
    if (state->Operators[OPL_OPERATOR_TOM_TOM].EnvelopeState != OPL_ENVELOPE_OFF)
    {
        *channel8Output = OplOperatorSample(state, OPL_RHYTHM_CHANNEL_TOM_CYMBAL, OPL_OPERATOR_TOM_TOM, 0) * OPL_RHYTHM_GAIN;
        OplEnvelopeTick(state, OPL_RHYTHM_CHANNEL_TOM_CYMBAL, OPL_OPERATOR_TOM_TOM);
    }

    /* The two accumulators the other three read. They run every sample, keyed or
     * not; the hi-hat takes op17's index from BEFORE this sample's step.
     */
    phase17Previous = (cymbal->Phase >> OPL_PHASE_FRACTION_BITS) & OPL_PHASE_MASK;
    hihat->Phase += OplPhaseIncrement(state, OPL_RHYTHM_CHANNEL_HIHAT_SNARE, hihat) << (state->RhythmRestart & 1);
    cymbal->Phase += OplPhaseIncrement(state, OPL_RHYTHM_CHANNEL_TOM_CYMBAL, cymbal) << ((state->RhythmRestart >> 1) & 1);
    state->RhythmRestart = 0;
    phase13 = (hihat->Phase >> OPL_PHASE_FRACTION_BITS) & OPL_PHASE_MASK;
    phase17 = (cymbal->Phase >> OPL_PHASE_FRACTION_BITS) & OPL_PHASE_MASK;

    /* HI-HAT -- op13's envelope and level, channel 7. */
    if (hihat->EnvelopeState != OPL_ENVELOPE_OFF)
    {
        phaseBit = OplRhythmPhaseBit(phase13, phase17Previous);
        *channel7Output += OplOperatorOutput(state, OPL_RHYTHM_CHANNEL_HIHAT_SNARE, hihat, (phaseBit << OPL_PHASE_HALF_SHIFT) |
                          ((phaseBit ^ (state->Noise >> OPL_NOISE_BIT_HIHAT)) & 1 ? OPL_HIHAT_PHASE_SET : OPL_HIHAT_PHASE_CLEAR)) * OPL_RHYTHM_GAIN;
        OplEnvelopeTick(state, OPL_RHYTHM_CHANNEL_HIHAT_SNARE, OPL_OPERATOR_HIHAT);
    }
    /* SNARE -- op16's envelope and level, on op13's bit 8, channel 7. */
    if (snare->EnvelopeState != OPL_ENVELOPE_OFF)
    {
        phaseBit = (phase13 >> OPL_SNARE_PHASE_BIT) & 1;
        *channel7Output += OplOperatorOutput(state, OPL_RHYTHM_CHANNEL_HIHAT_SNARE, snare, (phaseBit << OPL_PHASE_HALF_SHIFT) |
                          (((phaseBit ^ (state->Noise >> OPL_NOISE_BIT_SNARE)) & 1) << OPL_SNARE_NOISE_SHIFT)) * OPL_RHYTHM_GAIN;
        OplEnvelopeTick(state, OPL_RHYTHM_CHANNEL_HIHAT_SNARE, OPL_OPERATOR_SNARE);
    }
    /* CYMBAL -- op17's envelope and level, channel 8. */
    if (cymbal->EnvelopeState != OPL_ENVELOPE_OFF)
    {
        *channel8Output += OplOperatorOutput(state, OPL_RHYTHM_CHANNEL_TOM_CYMBAL, cymbal, (OplRhythmPhaseBit(phase13, phase17) << OPL_PHASE_HALF_SHIFT) | OPL_CYMBAL_PHASE_LOW) * OPL_RHYTHM_GAIN;
        OplEnvelopeTick(state, OPL_RHYTHM_CHANNEL_TOM_CYMBAL, OPL_OPERATOR_CYMBAL);
    }
}

/* --- one voice ------------------------------------------------------------ */
/* An ordinary two-operator channel: its contribution to the output, or 0 when
 * both operators are off -- in which case neither phase nor envelope moves. An
 * idle voice costs nothing, and the OPL2 golden depends on that staying so.
 */
static INT32 OplVoiceTwoOperator(POPL_STATE state, INT channel)
{
    INT modulatorIndex = VddOplOperatorIndex(channel, OPL_MODULATOR), carrierIndex = VddOplOperatorIndex(channel, OPL_CARRIER);
    POPL_OPERATOR modulator = &state->Operators[modulatorIndex], carrier = &state->Operators[carrierIndex];
    INT32 modulatorOutput, carrierOutput, feedbackModulation = 0, output;

    if (modulator->EnvelopeState == OPL_ENVELOPE_OFF && carrier->EnvelopeState == OPL_ENVELOPE_OFF) return 0;

    /* Feedback uses the mean of the operator's last two outputs, which is
     * what keeps a self-modulating operator stable instead of screaming.
     */
    if (state->Channels[channel].Feedback)
        feedbackModulation = (modulator->Output1 + modulator->Output2) >> (OPL_FEEDBACK_BASE_SHIFT - state->Channels[channel].Feedback);

    modulatorOutput = OplOperatorSample(state, channel, modulatorIndex, feedbackModulation);
    modulator->Output2 = modulator->Output1; modulator->Output1 = modulatorOutput;
    OplEnvelopeTick(state, channel, modulatorIndex);

    if (state->Channels[channel].Connection)                 /* additive: both operators heard */
    {
        carrierOutput = OplOperatorSample(state, channel, carrierIndex, 0);
        output = modulatorOutput + carrierOutput;
    }
    else                                      /* FM: modulator bends the carrier */
    {
        carrierOutput = OplOperatorSample(state, channel, carrierIndex, modulatorOutput);
        output = carrierOutput;
    }
    OplEnvelopeTick(state, channel, carrierIndex);
    return output;
}

/* -- A 4-OPERATOR VOICE (OPL3, NEW set, `channel` -- c below -- leads the pair c / c+3).
 * Operators 1-2 are channel c's, 3-4 channel c+3's. The two CNT bits -- c's
 * first, c+3's second -- choose the algorithm (YMF262 datasheet, 4-operator
 * connection figure); "->" is phase modulation, "+" is summed to the output:
 * CNT 0,0   1 -> 2 -> 3 -> 4                 one FM chain; 4 is heard
 * CNT 1,0   1  +  (2 -> 3 -> 4)              1 and 4 heard
 * CNT 0,1   (1 -> 2)  +  (3 -> 4)            2 and 4 heard
 * CNT 1,1   1  +  (2 -> 3)  +  4             1, 3 and 4 heard
 * Everything the voice has ONE of comes from channel c: the pitch (all four
 * operators run on c's F-number and block, key scaling included), the key-on,
 * the output routing, and the feedback -- which only ever applies to operator 1.
 * Channel c+3's own F-number, block, key, feedback and routing are ignored
 * while it is paired.
 */
static INT32 OplVoiceFourOperator(POPL_STATE state, INT channel)
{
    INT operator1 = VddOplOperatorIndex(channel, OPL_MODULATOR),     operator2 = VddOplOperatorIndex(channel, OPL_CARRIER);
    INT operator3 = VddOplOperatorIndex(channel + OPL_FOUR_OPERATOR_PARTNER, OPL_MODULATOR), operator4 = VddOplOperatorIndex(channel + OPL_FOUR_OPERATOR_PARTNER, OPL_CARRIER);
    POPL_OPERATOR firstOperator = &state->Operators[operator1];
    INT connection1 = state->Channels[channel].Connection, connection2 = state->Channels[channel + OPL_FOUR_OPERATOR_PARTNER].Connection;
    INT32 sample1, sample2, sample3, sample4, feedbackModulation = 0;

    if (firstOperator->EnvelopeState == OPL_ENVELOPE_OFF && state->Operators[operator2].EnvelopeState == OPL_ENVELOPE_OFF &&
        state->Operators[operator3].EnvelopeState == OPL_ENVELOPE_OFF && state->Operators[operator4].EnvelopeState == OPL_ENVELOPE_OFF)
        return 0;

    if (state->Channels[channel].Feedback) feedbackModulation = (firstOperator->Output1 + firstOperator->Output2) >> (OPL_FEEDBACK_BASE_SHIFT - state->Channels[channel].Feedback);
    sample1 = OplOperatorSample(state, channel, operator1, feedbackModulation);
    firstOperator->Output2 = firstOperator->Output1; firstOperator->Output1 = sample1;
    OplEnvelopeTick(state, channel, operator1);

    sample2 = OplOperatorSample(state, channel, operator2, connection1 ? 0 : sample1);           /* 1 -> 2 unless CNT1 */
    OplEnvelopeTick(state, channel, operator2);
    sample3 = OplOperatorSample(state, channel, operator3, (connection2 && !connection1) ? 0 : sample2); /* 2 -> 3 except 0,1 */
    OplEnvelopeTick(state, channel, operator3);
    sample4 = OplOperatorSample(state, channel, operator4, (connection1 && connection2) ? 0 : sample3);  /* 3 -> 4 except 1,1 */
    OplEnvelopeTick(state, channel, operator4);

    if (!connection1 && !connection2) return sample4;              /* FM-FM */
    if ( connection1 && !connection2) return sample1 + sample4;         /* AM-FM */
    if (!connection1 &&  connection2) return sample2 + sample4;         /* FM-AM */
    return sample1 + sample3 + sample4;         /* AM-AM */
}

/* Add `channel`'s output to the two sides. Without NEW the chip has one output
 * and it goes to both, as it always did. With NEW, C0 bits 4/5 (outputs A/B) are
 * the SB16's left/right; C/D (bits 6/7) reach no DAC on that card and are dropped
 * -- a voice routed only there is silent, as on the real card, and so is a voice
 * with no routing bits at all (a driver that sets NEW must set them).
 */
static VOID OplRoute(PCOPL_STATE state, INT isNewMode, INT channel, INT32 value, INT32 *left, INT32 *right)
{
    BYTE routing;
    if (!isNewMode)
    {
        *left += value;
        *right += value;
        return;
    }
    routing = state->Registers[(channel / OPL_CHANNELS) * OPL_ARRAY1_BASE + OPL_REGISTER_FEEDBACK + channel % OPL_CHANNELS];
    if (routing & OPL_C0_OUTPUT_A) *left += value;
    if (routing & OPL_C0_OUTPUT_B) *right += value;
}

/* One native sample, both sides, unclipped. The ORDER is the OPL2's exactly --
 * rhythm, then the LFO tick, then channels 0-8 -- because opl_synth_test's golden
 * checksum holds OPL2 output bit-identical to the build before OPL3 existed.
 */
static VOID OplSampleStereo(POPL_STATE state, INT32 *leftOutput, INT32 *rightOutput)
{
    INT isNewMode = VddOplIsNewMode(state);
    INT isRhythm = (state->Registers[OPL_REGISTER_RHYTHM] & OPL_BD_RHYTHM) ? 1 : 0;
    INT channelCount = isNewMode ? OPL3_CHANNELS : OPL_CHANNELS, channel;
    INT32 left = 0, right = 0;

    /* The noise generator runs from power-on whatever the mode, like the LFOs:
     * the drums read it where it has got to, never from a restart.
     */
    state->Noise = OplNoiseStep(state->Noise);
    if (isRhythm)
    {
        INT32 channel6Output, channel7Output, channel8Output;
        OplRhythmSample(state, &channel6Output, &channel7Output, &channel8Output);
        OplRoute(state, isNewMode, OPL_RHYTHM_CHANNEL_BASS_DRUM, channel6Output, &left, &right);
        OplRoute(state, isNewMode, OPL_RHYTHM_CHANNEL_HIHAT_SNARE, channel7Output, &left, &right);
        OplRoute(state, isNewMode, OPL_RHYTHM_CHANNEL_TOM_CYMBAL, channel8Output, &left, &right);
    }
    /* Outside the channel loop, and before the early-outs below: the LFOs run
     * whether or not anything is sounding. Advancing them only while a note
     * plays would restart the sweep at every silence.
     */
    state->LfoCount++;
    for (channel = 0; channel < channelCount; ++channel)
    {
        INT role;
        if (isRhythm && channel >= OPL_RHYTHM_CHANNEL_BASS_DRUM && channel <= OPL_RHYTHM_CHANNEL_TOM_CYMBAL) continue;   /* 6-8 are percussion in rhythm */
        role = isNewMode ? OplFourOperatorRole(state, channel) : 0;
        if (role == OPL_FOUR_OPERATOR_SECOND) continue;                    /* rendered with its leader */
        OplRoute(state, isNewMode, channel, role ? OplVoiceFourOperator(state, channel) : OplVoiceTwoOperator(state, channel), &left, &right);
    }
    *leftOutput = left; *rightOutput = right;
}

static INT16 OplClip(INT32 value)
{
    return (INT16)(value > OPL_SAMPLE_MAX ? OPL_SAMPLE_MAX : (value < OPL_SAMPLE_MIN ? OPL_SAMPLE_MIN : value));
}

/* --- public: render ------------------------------------------------------- */
VOID VddOplRender(POPL_STATE state, INT16 *output, UINT32 frames)
{
    UINT32 frame;
    for (frame = 0; frame < frames; ++frame)
    {
        INT32 left, right;
        OplSampleStereo(state, &left, &right);
        /* Without NEW, l == r IS the chip's one output: returned as it always was.
         * With NEW, the fold the mixer's own mono path uses.
         */
        output[frame] = VddOplIsNewMode(state) ? (INT16)(((INT32)OplClip(left) + OplClip(right)) / OPL_MONO_FOLD_DIVISOR)
                                      : OplClip(left);
    }
}

VOID VddOplRenderStereo(POPL_STATE state, INT16 *output, UINT32 frames)
{
    UINT32 frame;
    for (frame = 0; frame < frames; ++frame)
    {
        INT32 left, right;
        OplSampleStereo(state, &left, &right);
        output[OPL_STEREO_CHANNELS * frame]     = OplClip(left);
        output[OPL_STEREO_CHANNELS * frame + 1] = OplClip(right);
    }
}
