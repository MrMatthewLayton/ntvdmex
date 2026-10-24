/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * The SoundFont 2 synth: see sf2_synth.h. Section numbers are those of the SoundFont Technical
 * Specification 2.04.
 *
 * No libm (the host links no C runtime): exp2 and log2 are series below, evaluated at the
 * control rate only.
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#include "sf2_synth.h"
#include "../ntvdmex_bits.h"

/* Units, section 9.3. */
#define SF2_CENTS_PER_OCTAVE                1200.0
#define SF2_CENTS_PER_SEMITONE              100.0
#define SF2_CENTIBELS_PER_DECADE            200.0                   /* A centibel is 10^(1/200) in amplitude */
#define SF2_ABSOLUTE_CENTS_HZ               8.176                   /* Section 8.1.2, gens 8/22/24: 0 absolute cents */
#define SF2_LOG2_OF_10                      3.32192809488736234787
#define SF2_LN_2                            0.69314718055994530942
#define SF2_TWO_PI                          6.28318530717958647692
#define SF2_PER_MILLE                       1000.0                  /* 0.1% units: pan, sustainModEnv */

/* Envelopes, section 8.1.2 gens 25-40 and section 9.1.7. */
#define SF2_ENVELOPE_FULL_CENTIBELS         1000.0 /* Gen 37: "conventionally 1000 indicates full attenuation" */
#define SF2_SILENT_CENTIBELS                960.0                   /* Section 9.1.7: at 96 dB the gain jumps to zero */
#define SF2_MAXIMUM_ATTENUATION             1440.0                  /* Section 8.1.3, gens 37 / 48 */
#define SF2_TIMECENTS_INSTANT               (-12000.0)              /* Section 8.1.3: the default, "<1 msec" */
#define SF2_KEY_SCALING_CENTRE              60                      /* Gens 31/32/39/40: key 60 is unchanged */
#define SF2_HOLD_TIMECENTS_MAXIMUM          5000.0                  /* Section 8.1.3, gens 27/35 */
#define SF2_DECAY_TIMECENTS_MAXIMUM         8000.0                  /* Section 8.1.3, gens 28/36 */

/* Our choice: section 8.1.2 (gen 57) says only that the other note is "rapidly terminated". */
#define SF2_EXCLUSIVE_RELEASE_SECONDS       0.005

/* The filter, section 9.1.3 and gens 8-9. */
#define SF2_FILTER_OPEN_CENTS               13500.0                 /* Gen 8: past ~20 kHz with Q 0, no filter */
#define SF2_FILTER_LOWEST_CENTS             1500.0                  /* Section 8.1.3, gen 8's minimum */
#define SF2_FILTER_HIGHEST_FRACTION         0.45                    /* Of the output rate: below Nyquist */
#define SF2_FILTER_BUTTERWORTH_Q            0.70710678118654752440  /* A 2-pole with no resonant peak */
#define SF2_FILTER_RETUNE_CENTS             1.0                     /* Recompute the coefficients past this change */
#define SF2_FILTER_DENORMAL_FLOOR           1.0e-15f

/* Modulator sources, section 8.2. */
#define SF2_SOURCE_INDEX_MASK               0x007F
#define SF2_SOURCE_MIDI_CONTROLLER          0x0080                  /* The C bit, section 8.2.1 */
#define SF2_SOURCE_NEGATIVE                 0x0100                  /* The D bit, section 8.2.2 */
#define SF2_SOURCE_BIPOLAR                  0x0200                  /* The P bit, section 8.2.3 */
#define SF2_SOURCE_TYPE_SHIFT               10                      /* Section 8.2.4 */
#define SF2_SOURCE_TYPE_LINEAR              0
#define SF2_SOURCE_TYPE_CONCAVE             1
#define SF2_SOURCE_TYPE_CONVEX              2
#define SF2_SOURCE_TYPE_SWITCH              3
#define SF2_SOURCE_NONE                     0                       /* General palette, section 8.2.1 */
#define SF2_SOURCE_VELOCITY                 2
#define SF2_SOURCE_KEY                      3
#define SF2_SOURCE_POLY_PRESSURE            10
#define SF2_SOURCE_CHANNEL_PRESSURE         13
#define SF2_SOURCE_PITCH_WHEEL              14
#define SF2_SOURCE_PITCH_WHEEL_SENSITIVITY  16
#define SF2_DESTINATION_LINK                0x8000                  /* Section 7.4: a link to another modulator */
#define SF2_TRANSFORM_LINEAR                0                       /* Section 8.3 */
#define SF2_TRANSFORM_ABSOLUTE              2
#define SF2_DEFAULT_MODULATORS              10                      /* Section 8.4.1 - 8.4.10 */
#define SF2_CC_LSB_FIRST                    32                      /* Section 8.2.1: 32-63 are the LSBs of 0-31 */
#define SF2_CC_LSB_LAST                     63

/* Gen 54, sampleModes: the two low bits. */
#define SF2_LOOP_MODE_MASK                  3
#define SF2_LOOP_NONE                       0
#define SF2_LOOP_CONTINUOUS                 1
#define SF2_LOOP_UNUSED                     2                       /* "should be interpreted as indicating no loop" */
#define SF2_LOOP_UNTIL_RELEASE              3

/* The four zones a voice's modulators come from, in section 9.5.1's order of application. */
#define SF2_ZONE_PRESET_GLOBAL              0
#define SF2_ZONE_PRESET_LOCAL               1
#define SF2_ZONE_INSTRUMENT_GLOBAL          2
#define SF2_ZONE_INSTRUMENT_LOCAL           3
#define SF2_ZONE_LEVELS                     4

/* The concave curve, section 8.2.4. The specification gives its shape only as "variations of"
 * a logarithm. We take the curve that, scaled by the 960 cB of the default velocity and CC7 /
 * CC11 modulators (sections 8.4.1 / 8.4.5 / 8.4.7), attenuates by the square of the remaining
 * travel: -(40/96) x log10(1 - x), reaching 1 (96 dB) where that square is 96 dB down.
 */
#define SF2_CONCAVE_SCALE                   (40.0 / 96.0)
#define SF2_CONCAVE_END                     0.99999

/* MIDI, section 9.2 and the MIDI 1.0 specification it defers to. */
#define SF2_STATUS_MASK                     0xF0
#define SF2_CHANNEL_MASK                    0x0F
#define SF2_DATA_MASK                       0x7F
#define SF2_DATA_SHIFT                      7                       /* A 14-bit value: MSB << 7 | LSB */
#define SF2_DATA1_SHIFT                     8
#define SF2_DATA2_SHIFT                     16
#define SF2_STATUS_NOTE_OFF                 0x80
#define SF2_STATUS_NOTE_ON                  0x90
#define SF2_STATUS_POLY_PRESSURE            0xA0
#define SF2_STATUS_CONTROL                  0xB0
#define SF2_STATUS_PROGRAM                  0xC0
#define SF2_STATUS_CHANNEL_PRESSURE         0xD0
#define SF2_STATUS_PITCH_BEND               0xE0
#define SF2_CC_BANK_SELECT                  0
#define SF2_CC_MODULATION                   1
#define SF2_CC_DATA_ENTRY                   6
#define SF2_CC_VOLUME                       7
#define SF2_CC_PAN                          10
#define SF2_CC_EXPRESSION                   11
#define SF2_CC_DATA_ENTRY_LSB               38
#define SF2_CC_SUSTAIN                      64
#define SF2_CC_SOFT                         67                      /* The last pedal Reset All Controllers clears */
#define SF2_CC_NRPN_LSB                     98
#define SF2_CC_NRPN_MSB                     99
#define SF2_CC_RPN_LSB                      100
#define SF2_CC_RPN_MSB                      101
#define SF2_CC_ALL_SOUND_OFF                120
#define SF2_CC_RESET_CONTROLLERS            121
#define SF2_CC_ALL_NOTES_OFF                123                     /* 124-127 (omni / mono / poly) imply it too */
#define SF2_PEDAL_ON                        64                      /* Section 9.2: active at 64 or more */
#define SF2_RPN_BEND_RANGE                  0x0000
#define SF2_RPN_FINE_TUNING                 0x0001
#define SF2_RPN_COARSE_TUNING               0x0002
#define SF2_RPN_NULL                        0x3FFF
#define SF2_PITCH_BEND_CENTRE               8192
#define SF2_DATA14_MAXIMUM                  16383                   /* A 14-bit value: the bend, master volume */
#define SF2_PITCH_BEND_RANGE                16384.0
#define SF2_CONTROLLER_MAXIMUM              127
#define SF2_CONTROLLER_RANGE                128.0                   /* Section 9.5.3: 127 maps to 127/128 */
#define SF2_COARSE_TUNING_CENTRE            64
#define SF2_DEFAULT_VOLUME                  100                     /* GM power-on values */
#define SF2_DEFAULT_PAN                     64
#define SF2_DEFAULT_EXPRESSION              127
#define SF2_DEFAULT_BEND_RANGE_CENTS        200
#define SF2_SYSEX_START                     0xF0

/* SysEx: GM System On (MIDI Manufacturers Association), Roland GS DT1, Yamaha XG System On. */
#define SF2_SYSEX_UNIVERSAL_NON_REALTIME    0x7E
#define SF2_SYSEX_UNIVERSAL_REALTIME        0x7F
#define SF2_SYSEX_GENERAL_MIDI              0x09
#define SF2_SYSEX_DEVICE_CONTROL            0x04
#define SF2_SYSEX_MASTER_VOLUME             0x01
#define SF2_SYSEX_GM_LENGTH                 6
#define SF2_SYSEX_MASTER_VOLUME_LENGTH      8
#define SF2_SYSEX_ROLAND                    0x41
#define SF2_SYSEX_ROLAND_GS                 0x42
#define SF2_SYSEX_ROLAND_DT1                0x12
#define SF2_SYSEX_GS_LENGTH                 11
#define SF2_SYSEX_GS_PATCH                  0x40
#define SF2_SYSEX_GS_SYSTEM                 0x00
#define SF2_SYSEX_GS_RESET                  0x7F
#define SF2_SYSEX_GS_PART_BLOCK             0x10                    /* 40 1x ..: part x */
#define SF2_SYSEX_GS_BLOCK_MASK             0xF0
#define SF2_SYSEX_GS_PART_MASK              0x0F
#define SF2_SYSEX_GS_RHYTHM_PART            0x15                    /* 40 1x 15: "use for rhythm part" */
#define SF2_SYSEX_GS_PART_TEN               0                       /* X = 0 is part 10, the drum part */
#define SF2_SYSEX_GS_LAST_LOW_PART          9                       /* X = 1..9 are parts 1..9 */
#define SF2_SYSEX_YAMAHA                    0x43
#define SF2_SYSEX_YAMAHA_DEVICE_MASK        0xF0
#define SF2_SYSEX_YAMAHA_PARAMETER          0x10
#define SF2_SYSEX_YAMAHA_XG                 0x4C
#define SF2_SYSEX_XG_SYSTEM_ON              0x7E
#define SF2_SYSEX_XG_LENGTH                 9

/* The oscillator and output. */
#define SF2_FRACTION_ONE                    4294967296.0            /* 0.32 fixed point */
#define SF2_FRACTION_TO_FLOAT               (1.0f / 2147483648.0f)  /* Of the fraction >> 1 */
#define SF2_MAXIMUM_RATE                    256.0                   /* Sample points per output frame */
#define SF2_MINIMUM_SAMPLE_RATE             400 /* Section 7.10: zero is illegal; use the nearest practical */
#define SF2_DEFAULT_ROOT_KEY                60                      /* Section 7.10: for 255 and the illegal 128-254 */
#define SF2_OUTPUT_GAIN                     1.0f                    /* Mix scale before saturation */
#define SF2_PAN_CENTRE                      0.5                     /* Gen 17: each side's share at pan 0 */
#define SF2_STEAL_HELD_PENALTY              4.0f /* Above any amplitude: steal released voices first */

#define SF2_DOUBLE_EXPONENT_SHIFT           52
#define SF2_DOUBLE_EXPONENT_MASK            0x7FFull
#define SF2_DOUBLE_EXPONENT_BIAS            1023
#define SF2_DOUBLE_EXPONENT_MINIMUM         (-1022)
#define SF2_DOUBLE_EXPONENT_MAXIMUM         1023
#define SF2_DOUBLE_MANTISSA_MASK            0x000FFFFFFFFFFFFFull
#define SF2_SQRT_2                          1.41421356237309504880
#define SF2_SINE_TERMS                      10

#define SF2_EXP_SERIES_TERMS                (sizeof(g_Sf2ExpSeries) / sizeof(g_Sf2ExpSeries[0]))

/* Generator kinds, section 8.1.1. */
enum
{
    SF2_KIND_UNUSED = 0, SF2_KIND_VALUE, SF2_KIND_SAMPLE, SF2_KIND_RANGE, SF2_KIND_SUBSTITUTION,
    SF2_KIND_INDEX
};

typedef struct _SF2_GENERATOR_INFO
{
    SHORT Default;                  /* Section 8.1.3 */
    SHORT Minimum;
    SHORT Maximum;
    BYTE  Kind;
} SF2_GENERATOR_INFO;

typedef union _SF2_DOUBLE_BITS
{
    double Value;
    UINT64 Bits;
} SF2_DOUBLE_BITS;

/* Section 8.1.3: default, minimum, maximum and kind of every generator. A range is byLo | byHi
 * << 8 (section 4.5's rangesType); 0x7F00 is 0..127. Rows with no limits use the field's range.
 */
static const SF2_GENERATOR_INFO g_Sf2Generators[SF2_GENERATOR_COUNT] =
{
    {      0, -32768,  32767, SF2_KIND_SAMPLE },        /* 0 startAddrsOffset */
    {      0, -32768,  32767, SF2_KIND_SAMPLE },        /* 1 endAddrsOffset */
    {      0, -32768,  32767, SF2_KIND_SAMPLE },        /* 2 startloopAddrsOffset */
    {      0, -32768,  32767, SF2_KIND_SAMPLE },        /* 3 endloopAddrsOffset */
    {      0, -32768,  32767, SF2_KIND_SAMPLE },        /* 4 startAddrsCoarseOffset */
    {      0, -12000,  12000, SF2_KIND_VALUE },         /* 5 modLfoToPitch */
    {      0, -12000,  12000, SF2_KIND_VALUE },         /* 6 vibLfoToPitch */
    {      0, -12000,  12000, SF2_KIND_VALUE },         /* 7 modEnvToPitch */
    {  13500,   1500,  13500, SF2_KIND_VALUE },         /* 8 initialFilterFc */
    {      0,      0,    960, SF2_KIND_VALUE },         /* 9 initialFilterQ */
    {      0, -12000,  12000, SF2_KIND_VALUE },         /* 10 modLfoToFilterFc */
    {      0, -12000,  12000, SF2_KIND_VALUE },         /* 11 modEnvToFilterFc */
    {      0, -32768,  32767, SF2_KIND_SAMPLE },        /* 12 endAddrsCoarseOffset */
    {      0,   -960,    960, SF2_KIND_VALUE },         /* 13 modLfoToVolume */
    {      0, -32768,  32767, SF2_KIND_UNUSED },        /* 14 unused1 */
    {      0,      0,   1000, SF2_KIND_VALUE },         /* 15 chorusEffectsSend */
    {      0,      0,   1000, SF2_KIND_VALUE },         /* 16 reverbEffectsSend */
    {      0,   -500,    500, SF2_KIND_VALUE },         /* 17 pan */
    {      0, -32768,  32767, SF2_KIND_UNUSED },        /* 18 unused2 */
    {      0, -32768,  32767, SF2_KIND_UNUSED },        /* 19 unused3 */
    {      0, -32768,  32767, SF2_KIND_UNUSED },        /* 20 unused4 */
    { -12000, -12000,   5000, SF2_KIND_VALUE },         /* 21 delayModLFO */
    {      0, -16000,   4500, SF2_KIND_VALUE },         /* 22 freqModLFO */
    { -12000, -12000,   5000, SF2_KIND_VALUE },         /* 23 delayVibLFO */
    {      0, -16000,   4500, SF2_KIND_VALUE },         /* 24 freqVibLFO */
    { -12000, -12000,   5000, SF2_KIND_VALUE },         /* 25 delayModEnv */
    { -12000, -12000,   8000, SF2_KIND_VALUE },         /* 26 attackModEnv */
    { -12000, -12000,   5000, SF2_KIND_VALUE },         /* 27 holdModEnv */
    { -12000, -12000,   8000, SF2_KIND_VALUE },         /* 28 decayModEnv */
    {      0,      0,   1000, SF2_KIND_VALUE },         /* 29 sustainModEnv */
    { -12000, -12000,   8000, SF2_KIND_VALUE },         /* 30 releaseModEnv */
    {      0,  -1200,   1200, SF2_KIND_VALUE },         /* 31 keynumToModEnvHold */
    {      0,  -1200,   1200, SF2_KIND_VALUE },         /* 32 keynumToModEnvDecay */
    { -12000, -12000,   5000, SF2_KIND_VALUE },         /* 33 delayVolEnv */
    { -12000, -12000,   8000, SF2_KIND_VALUE },         /* 34 attackVolEnv */
    { -12000, -12000,   5000, SF2_KIND_VALUE },         /* 35 holdVolEnv */
    { -12000, -12000,   8000, SF2_KIND_VALUE },         /* 36 decayVolEnv */
    {      0,      0,   1440, SF2_KIND_VALUE },         /* 37 sustainVolEnv */
    { -12000, -12000,   8000, SF2_KIND_VALUE },         /* 38 releaseVolEnv */
    {      0,  -1200,   1200, SF2_KIND_VALUE },         /* 39 keynumToVolEnvHold */
    {      0,  -1200,   1200, SF2_KIND_VALUE },         /* 40 keynumToVolEnvDecay */
    {      0, -32768,  32767, SF2_KIND_INDEX },         /* 41 instrument */
    {      0, -32768,  32767, SF2_KIND_UNUSED },        /* 42 reserved1 */
    { 0x7F00, -32768,  32767, SF2_KIND_RANGE },         /* 43 keyRange */
    { 0x7F00, -32768,  32767, SF2_KIND_RANGE },         /* 44 velRange */
    {      0, -32768,  32767, SF2_KIND_SAMPLE },        /* 45 startloopAddrsCoarseOffset */
    {     -1, -32768,  32767, SF2_KIND_SUBSTITUTION },  /* 46 keynum */
    {     -1, -32768,  32767, SF2_KIND_SUBSTITUTION },  /* 47 velocity */
    {      0,      0,   1440, SF2_KIND_VALUE },         /* 48 initialAttenuation */
    {      0, -32768,  32767, SF2_KIND_UNUSED },        /* 49 reserved2 */
    {      0, -32768,  32767, SF2_KIND_SAMPLE },        /* 50 endloopAddrsCoarseOffset */
    {      0,   -120,    120, SF2_KIND_VALUE },         /* 51 coarseTune */
    {      0,    -99,     99, SF2_KIND_VALUE },         /* 52 fineTune */
    {      0, -32768,  32767, SF2_KIND_INDEX },         /* 53 sampleID */
    {      0, -32768,  32767, SF2_KIND_SAMPLE },        /* 54 sampleModes */
    {      0, -32768,  32767, SF2_KIND_UNUSED },        /* 55 reserved3 */
    {    100,      0,   1200, SF2_KIND_VALUE },         /* 56 scaleTuning */
    {      0, -32768,  32767, SF2_KIND_SAMPLE },        /* 57 exclusiveClass */
    {     -1, -32768,  32767, SF2_KIND_SAMPLE },        /* 58 overridingRootKey */
    {      0, -32768,  32767, SF2_KIND_UNUSED },        /* 59 unused5 */
    {      0, -32768,  32767, SF2_KIND_UNUSED }         /* 60 endOper */
};

/* Section 8.4: the default modulators of the instrument level. Two of its entries disagree
 * with their own text, and the text is followed:
 * - 8.4.5 prints the source as 0x0582, whose index bits say CC2, beside "index = 7" and the
 *   title "Continuous Controller 7": the source is 0x0587.
 * - 8.4.6 names its destination "Initial Attenuation" in the table and the pan in its text;
 *   the pan is meant (its amount is "tenths of a percent panned-right").
 */
static const SF2_MODULATOR g_Sf2DefaultModulators[SF2_DEFAULT_MODULATORS] =
{
    { 0x0502, SF2_GEN_INITIAL_ATTENUATION,   960,    0x0000, SF2_TRANSFORM_LINEAR },   /* 8.4.1 */
    { 0x0102, SF2_GEN_FILTER_FC,             -2400,  0x0000, SF2_TRANSFORM_LINEAR },   /* 8.4.2 */
    { 0x000D, SF2_GEN_VIB_LFO_TO_PITCH,      50,     0x0000, SF2_TRANSFORM_LINEAR },   /* 8.4.3 */
    { 0x0081, SF2_GEN_VIB_LFO_TO_PITCH,      50,     0x0000, SF2_TRANSFORM_LINEAR },   /* 8.4.4 */
    { 0x0587, SF2_GEN_INITIAL_ATTENUATION,   960,    0x0000, SF2_TRANSFORM_LINEAR },   /* 8.4.5 */
    { 0x028A, SF2_GEN_PAN,                   1000,   0x0000, SF2_TRANSFORM_LINEAR },   /* 8.4.6 */
    { 0x058B, SF2_GEN_INITIAL_ATTENUATION,   960,    0x0000, SF2_TRANSFORM_LINEAR },   /* 8.4.7 */
    { 0x00DB, SF2_GEN_REVERB_SEND,           200,    0x0000, SF2_TRANSFORM_LINEAR },   /* 8.4.8 */
    { 0x00DD, SF2_GEN_CHORUS_SEND,           200,    0x0000, SF2_TRANSFORM_LINEAR },   /* 8.4.9 */
    { 0x020E, SF2_DESTINATION_INITIAL_PITCH, 12700,  0x0010, SF2_TRANSFORM_LINEAR }    /* 8.4.10 */
};

/* 1/k! for the exp series below. */
static const double g_Sf2ExpSeries[] =
{
    1.0, 1.0, 1.0 / 2.0, 1.0 / 6.0, 1.0 / 24.0, 1.0 / 120.0, 1.0 / 720.0, 1.0 / 5040.0,
    1.0 / 40320.0, 1.0 / 362880.0, 1.0 / 3628800.0
};

/* the arithmetic (control rate only): */

/* 2^exponent: the integer part goes into the exponent bits, the fraction is e^(f ln 2) by
 * its series (f ln 2 < 0.7, so 11 terms are within 1e-9).
 */
static double Sf2Exp2(double exponent)
{
    SF2_DOUBLE_BITS scale;
    double power;
    double sum;
    INT whole;
    INT term;

    if (exponent < SF2_DOUBLE_EXPONENT_MINIMUM)
        return 0;

    if (exponent > SF2_DOUBLE_EXPONENT_MAXIMUM)
        exponent = SF2_DOUBLE_EXPONENT_MAXIMUM;

    whole = (INT)exponent;

    if ((double)whole > exponent)
        --whole;

    power = (exponent - whole) * SF2_LN_2;
    sum = g_Sf2ExpSeries[SF2_EXP_SERIES_TERMS - 1];

    for (term = (INT)SF2_EXP_SERIES_TERMS - 2; term >= 0; --term)
        sum = sum * power + g_Sf2ExpSeries[term];

    scale.Bits = (UINT64)(whole + SF2_DOUBLE_EXPONENT_BIAS) << SF2_DOUBLE_EXPONENT_SHIFT;
    return sum * scale.Value;
}

/* log2(value), value > 0: the exponent bits, then ln of the mantissa (folded into
 * [0.71, 1.41]) as 2 atanh((m - 1) / (m + 1)).
 */
static double Sf2Log2(double value)
{
    SF2_DOUBLE_BITS bits;
    double mantissa;
    double ratio;
    double square;
    double series;
    INT exponent;

    if (value <= 0)
        return SF2_DOUBLE_EXPONENT_MINIMUM;

    bits.Value = value;
    exponent = (INT)((bits.Bits >> SF2_DOUBLE_EXPONENT_SHIFT) & SF2_DOUBLE_EXPONENT_MASK)
        - SF2_DOUBLE_EXPONENT_BIAS;
    bits.Bits = (bits.Bits & SF2_DOUBLE_MANTISSA_MASK)
        | ((UINT64)SF2_DOUBLE_EXPONENT_BIAS << SF2_DOUBLE_EXPONENT_SHIFT);
    mantissa = bits.Value;

    if (mantissa > SF2_SQRT_2)
    {
        mantissa /= 2;
        ++exponent;
    }

    /* atanh(r) = r + r^3/3 + r^5/5 + ..., |r| < 0.18 */
    ratio = (mantissa - 1) / (mantissa + 1);
    square = ratio * ratio;
    series = 1 + square * (1.0 / 3 + square * (1.0 / 5 + square * (1.0 / 7 + square / 9)));
    return exponent + 2 * ratio * series / SF2_LN_2;
}

static double Sf2CentibelsToGain(double centibels)
{
    return Sf2Exp2(-centibels * SF2_LOG2_OF_10 / SF2_CENTIBELS_PER_DECADE);
}

static double Sf2GainToCentibels(double gain)
{
    return -Sf2Log2(gain) * SF2_CENTIBELS_PER_DECADE / SF2_LOG2_OF_10;
}

/* Section 9.3: absolute timecents, 0 = one second. The default and minimum, -12000, is
 * "<1 msec" (section 8.1.3), and -32768 "conventionally" none at all (section 8.1.2): both 0.
 */
static double Sf2TimecentsToSeconds(double timecents)
{
    if (timecents <= SF2_TIMECENTS_INSTANT)
        return 0;

    return Sf2Exp2(timecents / SF2_CENTS_PER_OCTAVE);
}

static double Sf2AbsoluteCentsToHz(double cents)
{
    return SF2_ABSOLUTE_CENTS_HZ * Sf2Exp2(cents / SF2_CENTS_PER_OCTAVE);
}

static double Sf2Clamp(double value, double minimum, double maximum)
{
    if (value < minimum)
        return minimum;

    if (value > maximum)
        return maximum;

    return value;
}

/* The concave and convex source types, section 8.2.4, on 0..1. */
static double Sf2Concave(double travel)
{
    double output;

    if (travel <= 0)
        return 0;

    if (travel >= SF2_CONCAVE_END)
        return 1;

    output = -SF2_CONCAVE_SCALE * Sf2Log2(1 - travel) / SF2_LOG2_OF_10;
    return output > 1 ? 1 : output;
}

static double Sf2Convex(double travel)
{
    return 1 - Sf2Concave(1 - travel);
}

/* sin and cos of 0 .. ~2.9 by their series (filter design, when the cutoff moves). */
static double Sf2Sine(double angle)
{
    double term = angle;
    double sum = angle;
    INT power;

    for (power = 1; power < SF2_SINE_TERMS; ++power)
    {
        term *= -angle * angle / ((2 * power) * (2 * power + 1));
        sum += term;
    }

    return sum;
}

static double Sf2Cosine(double angle)
{
    double term = 1;
    double sum = 1;
    INT power;

    for (power = 1; power < SF2_SINE_TERMS; ++power)
    {
        term *= -angle * angle / ((2 * power - 1) * (2 * power));
        sum += term;
    }

    return sum;
}

static DWORD Sf2SecondsToBlocks(PCSF2_SYNTH synth, double seconds)
{
    return (DWORD)(seconds / synth->BlockSeconds);
}

/* A change of `fullChange` over `seconds`, per control block; instant = all of it at once. */
static FLOAT Sf2StepPerBlock(PCSF2_SYNTH synth, double seconds, double fullChange)
{
    if (seconds <= synth->BlockSeconds)
        return (FLOAT)fullChange;

    return (FLOAT)(fullChange * synth->BlockSeconds / seconds);
}

/* generators and modulators: */

double Sf2VoiceParameter(PCSF2_VOICE voice, INT generator)
{
    const SF2_GENERATOR_INFO *info = &g_Sf2Generators[generator];
    double value = voice->Generators[generator] + voice->Modulation[generator];

    return Sf2Clamp(value, info->Minimum, info->Maximum);
}

static BOOL Sf2IsInRange(SHORT range, BYTE value)
{
    BYTE low = (BYTE)(range & BYTE_MASK);
    BYTE high = (BYTE)((range >> BYTE_SHIFT) & BYTE_MASK);

    return value >= low && value <= high;
}

/* Gens 43 / 44: does the played key and velocity fall in the zone's ranges? */
static BOOL Sf2IsZoneHit(const SHORT *values, BYTE key, BYTE velocity)
{
    return Sf2IsInRange(values[SF2_GEN_KEY_RANGE], key)
        && Sf2IsInRange(values[SF2_GEN_VELOCITY_RANGE], velocity);
}

/* One zone's generators onto `values` (section 9.4: a local zone replaces its global zone's
 * and the defaults). Sections 7.5 / 7.9: keyRange counts only first, velRange only first or
 * after keyRange, a repeated generator replaces the earlier one, unknown and unused ones are
 * ignored (section 10.3). Section 8.5: sample and substitution generators are ignored at the
 * preset level.
 */
static VOID Sf2ApplyZoneGenerators(
    PCSF2_FONT font,
    BOOL isPreset,
    const SF2_ZONE *zone,
    SHORT *values)
{
    PCSF2_TABLE generators = isPreset ? &font->PresetGenerators : &font->InstrumentGenerators;
    WORD previousOperator = SF2_GENERATOR_COUNT;
    DWORD generatorEnd = zone->GeneratorEnd;
    DWORD generatorIndex;

    for (generatorIndex = zone->GeneratorFirst; generatorIndex < generatorEnd; ++generatorIndex)
    {
        SF2_GENERATOR generator;
        DWORD position = generatorIndex - zone->GeneratorFirst;
        BYTE kind;

        Sf2FontReadGenerator(generators, generatorIndex, &generator);

        if (generator.Operator >= SF2_GENERATOR_COUNT)
        {
            previousOperator = generator.Operator;
            continue;
        }

        kind = g_Sf2Generators[generator.Operator].Kind;

        if (kind == SF2_KIND_UNUSED
            || kind == SF2_KIND_INDEX
            || (generator.Operator == SF2_GEN_KEY_RANGE && position != 0)
            || (generator.Operator == SF2_GEN_VELOCITY_RANGE
                && position != 0
                && !(position == 1 && previousOperator == SF2_GEN_KEY_RANGE))
            || (isPreset && (kind == SF2_KIND_SAMPLE || kind == SF2_KIND_SUBSTITUTION)))
        {
            previousOperator = generator.Operator;
            continue;
        }

        values[generator.Operator] = generator.Amount;
        previousOperator = generator.Operator;
    }
}

static BOOL Sf2IsSourceValid(WORD source)
{
    WORD index = source & SF2_SOURCE_INDEX_MASK;

    if ((source >> SF2_SOURCE_TYPE_SHIFT) > SF2_SOURCE_TYPE_SWITCH)
        return FALSE;

    /* Section 8.2.1: controllers 0, 6, 32-63, 98-101 and 120-127 are illegal sources. */
    if (source & SF2_SOURCE_MIDI_CONTROLLER)
        return index != SF2_CC_BANK_SELECT
            && index != SF2_CC_DATA_ENTRY
            && !(index >= SF2_CC_LSB_FIRST && index <= SF2_CC_LSB_LAST)
            && !(index >= SF2_CC_NRPN_LSB && index <= SF2_CC_RPN_MSB)
            && index < SF2_CC_ALL_SOUND_OFF;

    /* The general palette. Link (127) is not supported: section 9.5.4's chains are ignored. */
    return index == SF2_SOURCE_NONE
        || index == SF2_SOURCE_VELOCITY
        || index == SF2_SOURCE_KEY
        || index == SF2_SOURCE_POLY_PRESSURE
        || index == SF2_SOURCE_CHANNEL_PRESSURE
        || index == SF2_SOURCE_PITCH_WHEEL
        || index == SF2_SOURCE_PITCH_WHEEL_SENSITIVITY;
}

/* Sections 7.4 / 7.8 / 10.3: a modulator with an unknown source, destination or transform is
 * ignored; so is one whose destination is a link. Section 8.1.1: destinations are value
 * generators only.
 */
static BOOL Sf2IsModulatorValid(PCSF2_MODULATOR modulator)
{
    return Sf2IsSourceValid(modulator->Source)
        && Sf2IsSourceValid(modulator->AmountSource)
        && !(modulator->Destination & SF2_DESTINATION_LINK)
        && modulator->Destination < SF2_GENERATOR_COUNT
        && g_Sf2Generators[modulator->Destination].Kind == SF2_KIND_VALUE
        && (modulator->Transform == SF2_TRANSFORM_LINEAR
            || modulator->Transform == SF2_TRANSFORM_ABSOLUTE);
}

/* Section 9.5.1: identical = same source, destination, amount source and transform. */
static BOOL Sf2AreModulatorsIdentical(PCSF2_MODULATOR left, PCSF2_MODULATOR right)
{
    return left->Source == right->Source
        && left->Destination == right->Destination
        && left->AmountSource == right->AmountSource
        && left->Transform == right->Transform;
}

/* Put `modulator` in the list, replacing an identical one (section 9.5.1: within a zone the
 * later of two identical modulators wins, and an instrument zone's replaces a default or its
 * global zone's), or adding its amount to it (the preset level).
 */
static VOID Sf2PutModulator(
    PSF2_MODULATOR list,
    PDWORD count,
    PCSF2_MODULATOR modulator,
    BOOL isAdditive)
{
    DWORD listIndex;

    for (listIndex = 0; listIndex < *count; ++listIndex)
    {
        PSF2_MODULATOR entry = &list[listIndex];

        if (Sf2AreModulatorsIdentical(entry, modulator))
        {
            if (isAdditive)
                entry->Amount = (SHORT)Sf2Clamp(
                    entry->Amount + modulator->Amount,
                    INT16_MIN_VALUE,
                    INT16_MAX_VALUE);
            else
                entry->Amount = modulator->Amount;

            return;
        }
    }

    if (*count < SF2_SYNTH_VOICE_MODULATORS)
        list[(*count)++] = *modulator;
}

static VOID Sf2PutZoneModulators(
    PCSF2_FONT font,
    BOOL isPreset,
    const SF2_ZONE *zone,
    PSF2_MODULATOR list,
    PDWORD count)
{
    PCSF2_TABLE modulators = isPreset ? &font->PresetModulators : &font->InstrumentModulators;
    DWORD modulatorEnd = zone->ModulatorEnd;
    DWORD modulatorIndex;

    for (modulatorIndex = zone->ModulatorFirst; modulatorIndex < modulatorEnd; ++modulatorIndex)
    {
        SF2_MODULATOR modulator;

        Sf2FontReadModulator(modulators, modulatorIndex, &modulator);

        if (Sf2IsModulatorValid(&modulator))
            Sf2PutModulator(list, count, &modulator, FALSE);
    }
}

/* A source mapped to 0..1 or -1..1 (sections 8.2.2 - 8.2.4, 9.5.3). */
static double Sf2SourceValue(PCSF2_SYNTH synth, PCSF2_VOICE voice, WORD source)
{
    PCSF2_CHANNEL channel = &synth->Channels[voice->Channel];
    WORD index = source & SF2_SOURCE_INDEX_MASK;
    double raw;
    double maximum = SF2_CONTROLLER_MAXIMUM;
    double range = SF2_CONTROLLER_RANGE;
    double travel;
    BOOL isBipolar = (source & SF2_SOURCE_BIPOLAR) != 0;

    if (source & SF2_SOURCE_MIDI_CONTROLLER)
        raw = channel->Controllers[index];
    else
    {
        switch (index)
        {
        case SF2_SOURCE_NONE:
            return 1;       /* Section 8.2.1: "treated as if its value were set to 1" */

        case SF2_SOURCE_VELOCITY:
            raw = voice->EffectiveVelocity;
            break;

        case SF2_SOURCE_KEY:
            raw = voice->EffectiveKey;
            break;

        case SF2_SOURCE_POLY_PRESSURE:
            raw = channel->PolyPressure[voice->Key];
            break;

        case SF2_SOURCE_CHANNEL_PRESSURE:
            raw = channel->ChannelPressure;
            break;

        case SF2_SOURCE_PITCH_WHEEL:
            raw = channel->PitchBend;
            maximum = SF2_DATA14_MAXIMUM;
            range = SF2_PITCH_BEND_RANGE;
            break;

        case SF2_SOURCE_PITCH_WHEEL_SENSITIVITY:
            /* Semitones over 127, not 128: section 8.4.10's 12700-cent amount is 127
             * semitones, so only /127 makes a 2-semitone range bend 200 cents (table 2's
             * /128 would leave it 198.4).
             */
            raw = channel->BendRangeCents / SF2_CENTS_PER_SEMITONE;
            range = SF2_CONTROLLER_MAXIMUM;
            break;

        default:
            return 0;
        }
    }

    travel = (source & SF2_SOURCE_NEGATIVE) ? (maximum - raw) / range : raw / range;

    switch (source >> SF2_SOURCE_TYPE_SHIFT)
    {
    case SF2_SOURCE_TYPE_CONCAVE:
        if (!isBipolar)
            return Sf2Concave(travel);

        return travel >= SF2_PAN_CENTRE ? Sf2Concave(2 * travel - 1) : -Sf2Concave(1 - 2 * travel);

    case SF2_SOURCE_TYPE_CONVEX:
        if (!isBipolar)
            return Sf2Convex(travel);

        return travel >= SF2_PAN_CENTRE ? Sf2Convex(2 * travel - 1) : -Sf2Convex(1 - 2 * travel);

    case SF2_SOURCE_TYPE_SWITCH:
        if (travel >= SF2_PAN_CENTRE)
            return 1;

        return isBipolar ? -1 : 0;

    default:
        return isBipolar ? 2 * travel - 1 : travel;
    }
}

/* Section 9.5.1: destination += Transform(amount x primary x amount source), every summing node
 * at once. Called at note-on and whenever the channel's controllers change.
 */
static VOID Sf2VoiceModulate(PCSF2_SYNTH synth, PSF2_VOICE voice)
{
    DWORD destination;
    DWORD modulatorIndex;

    for (destination = 0; destination < SF2_DESTINATION_COUNT; ++destination)
        voice->Modulation[destination] = 0;

    for (modulatorIndex = 0; modulatorIndex < voice->ModulatorCount; ++modulatorIndex)
    {
        PCSF2_MODULATOR modulator = &voice->Modulators[modulatorIndex];
        double value = modulator->Amount
            * Sf2SourceValue(synth, voice, modulator->Source)
            * Sf2SourceValue(synth, voice, modulator->AmountSource);

        if (modulator->Transform == SF2_TRANSFORM_ABSOLUTE && value < 0)
            value = -value;

        voice->Modulation[modulator->Destination] += (FLOAT)value;
    }

    voice->ModulationSerial = synth->Channels[voice->Channel].Serial;
}

/* envelopes and LFOs: */

static VOID Sf2EnvelopeStart(
    PCSF2_SYNTH synth,
    PSF2_ENVELOPE envelope,
    double delaySeconds,
    double attackSeconds,
    double holdSeconds,
    double decaySeconds,
    double sustainLevel,
    double releaseSeconds,
    double fullChange)
{
    envelope->Stage = SF2_STAGE_DELAY;
    envelope->BlocksLeft = Sf2SecondsToBlocks(synth, delaySeconds);
    envelope->Progress = 0;
    envelope->Level = 0;
    envelope->AttackStep = Sf2StepPerBlock(synth, attackSeconds, 1);
    envelope->HoldBlocks = Sf2SecondsToBlocks(synth, holdSeconds);
    envelope->DecayStep = Sf2StepPerBlock(synth, decaySeconds, fullChange);
    envelope->SustainLevel = (FLOAT)sustainLevel;
    envelope->ReleaseStep = Sf2StepPerBlock(synth, releaseSeconds, fullChange);
}

/* Section 9.1.7, one control block on. The volume envelope's level falls in centibels (gens
 * 36 / 38: "a constant dB change for each time unit"), the modulation envelope's linearly
 * (gens 28 / 30).
 */
static VOID Sf2EnvelopeAdvance(PSF2_ENVELOPE envelope, BOOL isVolume)
{
    if (envelope->Stage == SF2_STAGE_DELAY)
    {
        if (envelope->BlocksLeft)
        {
            --envelope->BlocksLeft;
            return;
        }

        envelope->Stage = SF2_STAGE_ATTACK;
    }

    if (envelope->Stage == SF2_STAGE_ATTACK)
    {
        envelope->Progress += envelope->AttackStep;

        if (envelope->Progress >= 1)
        {
            envelope->Progress = 1;
            envelope->Stage = SF2_STAGE_HOLD;
            envelope->BlocksLeft = envelope->HoldBlocks;
            envelope->Level = isVolume ? 0.0f : 1.0f;
        }

        return;
    }

    if (envelope->Stage == SF2_STAGE_HOLD)
    {
        if (envelope->BlocksLeft)
        {
            --envelope->BlocksLeft;
            return;
        }

        envelope->Stage = SF2_STAGE_DECAY;
    }

    if (envelope->Stage == SF2_STAGE_DECAY)
    {
        if (isVolume)
        {
            envelope->Level += envelope->DecayStep;

            if (envelope->Level >= envelope->SustainLevel)
            {
                envelope->Level = envelope->SustainLevel;
                envelope->Stage = SF2_STAGE_SUSTAIN;
            }
        }
        else
        {
            envelope->Level -= envelope->DecayStep;

            if (envelope->Level <= envelope->SustainLevel)
            {
                envelope->Level = envelope->SustainLevel;
                envelope->Stage = SF2_STAGE_SUSTAIN;
            }
        }

        return;
    }

    if (envelope->Stage == SF2_STAGE_RELEASE)
    {
        if (isVolume)
        {
            envelope->Level += envelope->ReleaseStep;

            if (envelope->Level >= SF2_SILENT_CENTIBELS)
                envelope->Stage = SF2_STAGE_DONE;
        }
        else
        {
            envelope->Level -= envelope->ReleaseStep;

            if (envelope->Level <= 0)
            {
                envelope->Level = 0;
                envelope->Stage = SF2_STAGE_DONE;
            }
        }
    }
}

/* Gens 26 / 34: the attack is "convex" -- linear in amplitude for the volume envelope; for
 * the modulation envelope, the convex source curve of section 8.2.4.
 */
static double Sf2EnvelopeModulationValue(PCSF2_ENVELOPE envelope)
{
    switch (envelope->Stage)
    {
    case SF2_STAGE_DELAY:
    case SF2_STAGE_DONE:
        return 0;

    case SF2_STAGE_ATTACK:
        return Sf2Convex(envelope->Progress);

    case SF2_STAGE_HOLD:
        return 1;

    default:
        return envelope->Level;
    }
}

double Sf2EnvelopeVolumeGain(PCSF2_ENVELOPE envelope)
{
    switch (envelope->Stage)
    {
    case SF2_STAGE_DELAY:
    case SF2_STAGE_DONE:
        return 0;

    case SF2_STAGE_ATTACK:
        return envelope->Progress;

    default:
        return envelope->Level >= SF2_SILENT_CENTIBELS ? 0 : Sf2CentibelsToGain(envelope->Level);
    }
}

/* Section 9.1.7: key-off enters release from the current value, whatever the phase. */
static VOID Sf2EnvelopeRelease(PSF2_ENVELOPE envelope, BOOL isVolume)
{
    if (envelope->Stage == SF2_STAGE_DONE || envelope->Stage == SF2_STAGE_RELEASE)
        return;

    if (isVolume)
    {
        if (envelope->Stage == SF2_STAGE_DELAY)
            envelope->Level = (FLOAT)SF2_SILENT_CENTIBELS;
        else if (envelope->Stage == SF2_STAGE_ATTACK)
            envelope->Level = envelope->Progress > 0
                ? (FLOAT)Sf2GainToCentibels(envelope->Progress)
                : (FLOAT)SF2_SILENT_CENTIBELS;
    }
    else
        envelope->Level = (FLOAT)Sf2EnvelopeModulationValue(envelope);

    envelope->Stage = SF2_STAGE_RELEASE;

    if (isVolume && envelope->Level >= SF2_SILENT_CENTIBELS)
        envelope->Stage = SF2_STAGE_DONE;
}

static VOID Sf2LfoStart(
    PCSF2_SYNTH synth,
    PSF2_LFO lfo,
    double delayTimecents,
    double frequencyCents)
{
    lfo->DelayBlocks = Sf2SecondsToBlocks(synth, Sf2TimecentsToSeconds(delayTimecents));
    lfo->Phase = 0;
    lfo->PhaseStep = (FLOAT)(Sf2AbsoluteCentsToHz(frequencyCents) * synth->BlockSeconds);
    lfo->Value = 0;
}

/* Section 9.1.6: zero through the delay, then from zero up to +1, down to -1, up again. */
static VOID Sf2LfoAdvance(PSF2_LFO lfo)
{
    FLOAT phase;

    if (lfo->DelayBlocks)
    {
        --lfo->DelayBlocks;
        return;
    }

    lfo->Phase += lfo->PhaseStep;

    while (lfo->Phase >= 1)
        lfo->Phase -= 1;

    phase = lfo->Phase;

    if (phase < 0.25f)
        lfo->Value = 4 * phase;
    else if (phase < 0.75f)
        lfo->Value = 2 - 4 * phase;
    else
        lfo->Value = 4 * phase - 4;
}

/* voices: */

static VOID Sf2VoiceFree(PSF2_SYNTH synth, PSF2_VOICE voice)
{
    if (voice->State == SF2_VOICE_FREE)
        return;

    voice->State = SF2_VOICE_FREE;
    --synth->ActiveVoices;
}

static VOID Sf2VoiceRelease(PSF2_VOICE voice)
{
    if (voice->State != SF2_VOICE_ON)
        return;

    voice->State = SF2_VOICE_RELEASED;
    voice->IsSustained = FALSE;
    Sf2EnvelopeRelease(&voice->VolumeEnvelope, TRUE);
    Sf2EnvelopeRelease(&voice->ModulationEnvelope, FALSE);
}

/* Section 9.1.3: the filter is "idealized", a resonant 2-pole low-pass. This is the bilinear
 * transform of H(s) = 1 / (s^2 + s/Q + 1) at the cutoff. Q: gen 9 is the peak's height above
 * the DC gain in cB; 0 is "not resonant", taken as the Butterworth Q with no peak.
 */
static VOID Sf2VoiceDesignFilter(
    PCSF2_SYNTH synth,
    PSF2_VOICE voice,
    double cutoffCents,
    double resonance)
{
    double cutoffHz = Sf2AbsoluteCentsToHz(cutoffCents);
    double angle;
    double sine;
    double cosine;
    double quality = SF2_FILTER_BUTTERWORTH_Q;
    double alpha;
    double normaliser;

    if (cutoffHz > SF2_FILTER_HIGHEST_FRACTION * synth->OutputHz)
        cutoffHz = SF2_FILTER_HIGHEST_FRACTION * synth->OutputHz;

    if (resonance > 0)
    {
        quality = Sf2Exp2(resonance * SF2_LOG2_OF_10 / SF2_CENTIBELS_PER_DECADE);

        if (quality < SF2_FILTER_BUTTERWORTH_Q)
            quality = SF2_FILTER_BUTTERWORTH_Q;
    }

    angle = SF2_TWO_PI * cutoffHz / synth->OutputHz;
    sine = Sf2Sine(angle);
    cosine = Sf2Cosine(angle);
    alpha = sine / (2 * quality);
    normaliser = 1 + alpha;
    voice->FilterB0 = (FLOAT)((1 - cosine) / 2 / normaliser);
    voice->FilterB1 = (FLOAT)((1 - cosine) / normaliser);
    voice->FilterB2 = voice->FilterB0;
    voice->FilterA1 = (FLOAT)(-2 * cosine / normaliser);
    voice->FilterA2 = (FLOAT)((1 - alpha) / normaliser);
    voice->FilterCutoffCents = (FLOAT)cutoffCents;
    voice->FilterQ = (FLOAT)resonance;
}

/* Everything the next control block plays with: pitch, gain (ramped over `rampFrames`), filter.
 * Section 9.1: pitch, cutoff and volume each = generator + modulators + LFOs + envelopes.
 */
static VOID Sf2VoiceTargets(PSF2_SYNTH synth, PSF2_VOICE voice, DWORD rampFrames)
{
    PCSF2_CHANNEL channel = &synth->Channels[voice->Channel];
    double modulationLfo;
    double vibratoLfo;
    double modulationEnvelope;
    double cents;
    double rate;
    double attenuation;
    double amplitude;
    double pan;
    double cutoff;
    double resonance;
    FLOAT targetLeft;
    FLOAT targetRight;

    if (voice->ModulationSerial != channel->Serial)
        Sf2VoiceModulate(synth, voice);

    modulationLfo = voice->ModulationLfo.Value;
    vibratoLfo = voice->VibratoLfo.Value;
    modulationEnvelope = Sf2EnvelopeModulationValue(&voice->ModulationEnvelope);

    /* Section 9.1.1: the pitch, in cents. */
    cents = voice->KeyPitchCents
        + Sf2VoiceParameter(voice, SF2_GEN_COARSE_TUNE) * SF2_CENTS_PER_SEMITONE
        + Sf2VoiceParameter(voice, SF2_GEN_FINE_TUNE)
        + voice->Modulation[SF2_DESTINATION_INITIAL_PITCH]
        + channel->CoarseTuneSemitones * SF2_CENTS_PER_SEMITONE
        + channel->FineTuneCents
        + modulationLfo * Sf2VoiceParameter(voice, SF2_GEN_MOD_LFO_TO_PITCH)
        + vibratoLfo * Sf2VoiceParameter(voice, SF2_GEN_VIB_LFO_TO_PITCH)
        + modulationEnvelope * Sf2VoiceParameter(voice, SF2_GEN_MOD_ENV_TO_PITCH);
    rate = Sf2Clamp(voice->RateRatio * Sf2Exp2(cents / SF2_CENTS_PER_OCTAVE), 0, SF2_MAXIMUM_RATE);
    voice->StepWhole = (DWORD)rate;
    voice->StepFraction = (DWORD)((rate - voice->StepWhole) * SF2_FRACTION_ONE);

    /* Section 9.1.3: the cutoff and its resonance. */
    cutoff = Sf2Clamp(
        Sf2VoiceParameter(voice, SF2_GEN_FILTER_FC)
            + modulationLfo * Sf2VoiceParameter(voice, SF2_GEN_MOD_LFO_TO_FILTER_FC)
            + modulationEnvelope * Sf2VoiceParameter(voice, SF2_GEN_MOD_ENV_TO_FILTER_FC),
        SF2_FILTER_LOWEST_CENTS,
        SF2_FILTER_OPEN_CENTS);
    resonance = Sf2VoiceParameter(voice, SF2_GEN_FILTER_Q);

    if (cutoff >= SF2_FILTER_OPEN_CENTS && resonance <= 0)
        voice->IsFiltered = FALSE;
    else
    {
        if (!voice->IsFiltered)
        {
            voice->FilterX1 = 0;
            voice->FilterX2 = 0;
            voice->FilterY1 = 0;
            voice->FilterY2 = 0;
            Sf2VoiceDesignFilter(synth, voice, cutoff, resonance);
        }
        else if (cutoff - voice->FilterCutoffCents >= SF2_FILTER_RETUNE_CENTS
            || voice->FilterCutoffCents - cutoff >= SF2_FILTER_RETUNE_CENTS
            || resonance != voice->FilterQ)
            Sf2VoiceDesignFilter(synth, voice, cutoff, resonance);

        voice->IsFiltered = TRUE;
    }

    /* Section 9.1.4: the gain, in centibels. Gen 13: a positive LFO excursion raises the
     * volume. Gen 9: the resonance lowers the DC gain by half its height.
     */
    attenuation = Sf2VoiceParameter(voice, SF2_GEN_INITIAL_ATTENUATION)
        - modulationLfo * Sf2VoiceParameter(voice, SF2_GEN_MOD_LFO_TO_VOLUME);

    if (voice->IsFiltered)
        attenuation += resonance / 2;

    attenuation = Sf2Clamp(attenuation, -SF2_MAXIMUM_ATTENUATION, SF2_MAXIMUM_ATTENUATION);
    amplitude = Sf2EnvelopeVolumeGain(&voice->VolumeEnvelope)
        * Sf2CentibelsToGain(attenuation)
        * synth->MasterGain;

    /* Gen 17: linear shares, its own example being -250 = 75% left, 25% right. */
    pan = Sf2VoiceParameter(voice, SF2_GEN_PAN) / SF2_PER_MILLE;
    targetLeft = (FLOAT)(amplitude * (SF2_PAN_CENTRE - pan)) * SF2_OUTPUT_GAIN;
    targetRight = (FLOAT)(amplitude * (SF2_PAN_CENTRE + pan)) * SF2_OUTPUT_GAIN;
    voice->GainStepLeft = (targetLeft - voice->GainLeft) / (FLOAT)rampFrames;
    voice->GainStepRight = (targetRight - voice->GainRight) / (FLOAT)rampFrames;
    voice->Amplitude = (FLOAT)amplitude;
}

/* A free voice, or the quietest sounding one -- released voices first -- with every other
 * voice of its note (a stereo pair stays a pair). Never one of the note being started.
 */
static PSF2_VOICE Sf2VoiceAllocate(PSF2_SYNTH synth, DWORD noteId)
{
    PSF2_VOICE victim = NULL;
    FLOAT victimScore = 0;
    DWORD victimNote;
    DWORD voiceIndex;

    for (voiceIndex = 0; voiceIndex < SF2_SYNTH_VOICES; ++voiceIndex)
        if (synth->Voices[voiceIndex].State == SF2_VOICE_FREE)
            return &synth->Voices[voiceIndex];

    for (voiceIndex = 0; voiceIndex < SF2_SYNTH_VOICES; ++voiceIndex)
    {
        PSF2_VOICE voice = &synth->Voices[voiceIndex];
        FLOAT penalty = voice->State == SF2_VOICE_ON ? SF2_STEAL_HELD_PENALTY : 0.0f;
        FLOAT score = voice->Amplitude + penalty;

        if (voice->NoteId == noteId)
            continue;

        if (!victim || score < victimScore)
        {
            victim = voice;
            victimScore = score;
        }
    }

    if (!victim)
        return NULL;

    victimNote = victim->NoteId;

    for (voiceIndex = 0; voiceIndex < SF2_SYNTH_VOICES; ++voiceIndex)
    {
        PSF2_VOICE voice = &synth->Voices[voiceIndex];

        if (voice->State != SF2_VOICE_FREE && voice->NoteId == victimNote)
        {
            Sf2VoiceFree(synth, voice);
            synth->Stats.VoicesStolen++;
        }
    }

    return victim;
}

/* Section 7.10 / 8.1.2: where in the sample data a voice plays, with the address offsets.
 * FALSE = nothing to play. A loop outside the played range is dropped, not trusted.
 */
static BOOL Sf2VoiceSetAddresses(PCSF2_FONT font, PSF2_VOICE voice, const SF2_SAMPLE_HEADER *header)
{
    const SHORT *generators = voice->Generators;
    INT64 lastPoint = (INT64)font->SampleCount - 1;
    INT64 start = (INT64)header->Start
        + generators[SF2_GEN_START_OFFSET]
        + (INT64)generators[SF2_GEN_START_COARSE_OFFSET] * SF2_COARSE_OFFSET_POINTS;
    INT64 end = (INT64)header->End
        + generators[SF2_GEN_END_OFFSET]
        + (INT64)generators[SF2_GEN_END_COARSE_OFFSET] * SF2_COARSE_OFFSET_POINTS;
    INT64 loopStart = (INT64)header->StartLoop
        + generators[SF2_GEN_START_LOOP_OFFSET]
        + (INT64)generators[SF2_GEN_START_LOOP_COARSE_OFFSET] * SF2_COARSE_OFFSET_POINTS;
    INT64 loopEnd = (INT64)header->EndLoop
        + generators[SF2_GEN_END_LOOP_OFFSET]
        + (INT64)generators[SF2_GEN_END_LOOP_COARSE_OFFSET] * SF2_COARSE_OFFSET_POINTS;

    /* The point after the last one played is read by the interpolator, so `end` stops one
     * short of the data (section 6.1's 46 zero points normally sit there).
     */
    start = (INT64)Sf2Clamp((double)start, 0, (double)lastPoint);
    end = (INT64)Sf2Clamp((double)end, 0, (double)lastPoint);

    if (start >= end)
        return FALSE;

    voice->Start = (DWORD)start;
    voice->End = (DWORD)end;
    voice->Position = voice->Start;
    voice->Fraction = 0;

    if (loopStart < start || loopEnd > end || loopEnd <= loopStart)
    {
        voice->LoopMode = SF2_LOOP_NONE;
        voice->LoopStart = voice->Start;
        voice->LoopEnd = voice->End;
    }
    else
    {
        voice->LoopStart = (DWORD)loopStart;
        voice->LoopEnd = (DWORD)loopEnd;
    }

    return TRUE;
}

/* Section 8.1.2, gen 57: a new note in an exclusive class cuts every other sounding note of
 * that class. "The scope ... is the entire preset": a channel plays one preset, so the channel.
 */
static VOID Sf2CutExclusiveClass(PSF2_SYNTH synth, BYTE channel, INT exclusiveClass, DWORD noteId)
{
    DWORD voiceIndex;
    FLOAT fastStep = Sf2StepPerBlock(
        synth,
        SF2_EXCLUSIVE_RELEASE_SECONDS,
        SF2_ENVELOPE_FULL_CENTIBELS);

    for (voiceIndex = 0; voiceIndex < SF2_SYNTH_VOICES; ++voiceIndex)
    {
        PSF2_VOICE voice = &synth->Voices[voiceIndex];

        if (voice->State == SF2_VOICE_FREE
            || voice->Channel != channel
            || voice->ExclusiveClass != exclusiveClass
            || voice->NoteId == noteId)
            continue;

        Sf2VoiceRelease(voice);
        voice->VolumeEnvelope.ReleaseStep = fastStep;
    }
}

/* Gens 46 / 47: a key or velocity the zone substitutes for the played one, after the ranges. */
static BYTE Sf2Substitute(SHORT substitute, BYTE played)
{
    return substitute >= 0 && substitute < SF2_MIDI_VALUES ? (BYTE)substitute : played;
}

/* Section 9.5.1: the defaults, then the instrument's global and local zones replacing; then
 * the preset's global and local zones (local replacing global) added on.
 */
static VOID Sf2VoiceGatherModulators(
    PCSF2_FONT font,
    PSF2_VOICE voice,
    const SF2_ZONE *const zones[SF2_ZONE_LEVELS])
{
    SF2_MODULATOR presetList[SF2_SYNTH_VOICE_MODULATORS];
    DWORD presetCount = 0;
    PSF2_MODULATOR list = voice->Modulators;
    PDWORD count = &voice->ModulatorCount;
    DWORD modulatorIndex;

    *count = 0;

    for (modulatorIndex = 0; modulatorIndex < SF2_DEFAULT_MODULATORS; ++modulatorIndex)
        list[(*count)++] = g_Sf2DefaultModulators[modulatorIndex];

    if (zones[SF2_ZONE_INSTRUMENT_GLOBAL])
        Sf2PutZoneModulators(font, FALSE, zones[SF2_ZONE_INSTRUMENT_GLOBAL], list, count);

    Sf2PutZoneModulators(font, FALSE, zones[SF2_ZONE_INSTRUMENT_LOCAL], list, count);

    if (zones[SF2_ZONE_PRESET_GLOBAL])
        Sf2PutZoneModulators(font, TRUE, zones[SF2_ZONE_PRESET_GLOBAL], presetList, &presetCount);

    Sf2PutZoneModulators(font, TRUE, zones[SF2_ZONE_PRESET_LOCAL], presetList, &presetCount);

    for (modulatorIndex = 0; modulatorIndex < presetCount; ++modulatorIndex)
        Sf2PutModulator(list, count, &presetList[modulatorIndex], TRUE);
}

/* A hold or decay time with its key scaling (gens 31/32/39/40: key 60 unchanged, higher keys
 * shorter for a positive amount), in seconds.
 */
static double Sf2KeyScaledSeconds(
    PCSF2_VOICE voice,
    INT timeGenerator,
    INT scalingGenerator,
    double maximumTimecents)
{
    INT keysAboveCentre = voice->EffectiveKey - SF2_KEY_SCALING_CENTRE;
    double timecents = Sf2VoiceParameter(voice, timeGenerator)
        - Sf2VoiceParameter(voice, scalingGenerator) * keysAboveCentre;

    return Sf2TimecentsToSeconds(Sf2Clamp(timecents, SF2_TIMECENTS_INSTANT, maximumTimecents));
}

/* The envelopes (gens 25-40) and LFOs (gens 21-24), fixed at note-on. */
static VOID Sf2VoiceStartModulation(PCSF2_SYNTH synth, PSF2_VOICE voice)
{
    double sustainDrop = Sf2VoiceParameter(voice, SF2_GEN_MOD_ENV_SUSTAIN) / SF2_PER_MILLE;

    Sf2EnvelopeStart(
        synth,
        &voice->VolumeEnvelope,
        Sf2TimecentsToSeconds(Sf2VoiceParameter(voice, SF2_GEN_VOL_ENV_DELAY)),
        Sf2TimecentsToSeconds(Sf2VoiceParameter(voice, SF2_GEN_VOL_ENV_ATTACK)),
        Sf2KeyScaledSeconds(
            voice,
            SF2_GEN_VOL_ENV_HOLD,
            SF2_GEN_KEY_TO_VOL_ENV_HOLD,
            SF2_HOLD_TIMECENTS_MAXIMUM),
        Sf2KeyScaledSeconds(
            voice,
            SF2_GEN_VOL_ENV_DECAY,
            SF2_GEN_KEY_TO_VOL_ENV_DECAY,
            SF2_DECAY_TIMECENTS_MAXIMUM),
        Sf2VoiceParameter(voice, SF2_GEN_VOL_ENV_SUSTAIN),
        Sf2TimecentsToSeconds(Sf2VoiceParameter(voice, SF2_GEN_VOL_ENV_RELEASE)),
        SF2_ENVELOPE_FULL_CENTIBELS);
    Sf2EnvelopeStart(
        synth,
        &voice->ModulationEnvelope,
        Sf2TimecentsToSeconds(Sf2VoiceParameter(voice, SF2_GEN_MOD_ENV_DELAY)),
        Sf2TimecentsToSeconds(Sf2VoiceParameter(voice, SF2_GEN_MOD_ENV_ATTACK)),
        Sf2KeyScaledSeconds(
            voice,
            SF2_GEN_MOD_ENV_HOLD,
            SF2_GEN_KEY_TO_MOD_ENV_HOLD,
            SF2_HOLD_TIMECENTS_MAXIMUM),
        Sf2KeyScaledSeconds(
            voice,
            SF2_GEN_MOD_ENV_DECAY,
            SF2_GEN_KEY_TO_MOD_ENV_DECAY,
            SF2_DECAY_TIMECENTS_MAXIMUM),
        1 - sustainDrop,
        Sf2TimecentsToSeconds(Sf2VoiceParameter(voice, SF2_GEN_MOD_ENV_RELEASE)),
        1);
    Sf2LfoStart(
        synth,
        &voice->ModulationLfo,
        Sf2VoiceParameter(voice, SF2_GEN_MOD_LFO_DELAY),
        Sf2VoiceParameter(voice, SF2_GEN_MOD_LFO_FREQUENCY));
    Sf2LfoStart(
        synth,
        &voice->VibratoLfo,
        Sf2VoiceParameter(voice, SF2_GEN_VIB_LFO_DELAY),
        Sf2VoiceParameter(voice, SF2_GEN_VIB_LFO_FREQUENCY));
}

/* One voice for one instrument zone (section 9.4): instrument generators absolute, preset
 * generators added for value generators only, modulators by section 9.5.1's hierarchy.
 */
static BOOL Sf2VoiceStart(
    PSF2_SYNTH synth,
    BYTE channelIndex,
    BYTE key,
    BYTE velocity,
    DWORD noteId,
    const SHORT *instrumentValues,
    const SHORT *presetValues,
    const SF2_ZONE *const zones[SF2_ZONE_LEVELS],
    WORD sampleIndex)
{
    PCSF2_FONT font = synth->Font;
    SF2_SAMPLE_HEADER header;
    PSF2_VOICE voice;
    DWORD generatorIndex;
    DWORD rampFrames;
    INT rootKey;
    DWORD sampleRate;

    Sf2FontReadSampleHeader(font, sampleIndex, &header);

    /* Sections 5.4 / 5.5: a ROM sample, with no ROM, does not sound. */
    if (header.SampleType & SF2_SAMPLE_ROM)
        return FALSE;

    voice = Sf2VoiceAllocate(synth, noteId);

    if (!voice)
        return FALSE;

    for (generatorIndex = 0; generatorIndex < SF2_GENERATOR_COUNT; ++generatorIndex)
    {
        if (g_Sf2Generators[generatorIndex].Kind == SF2_KIND_VALUE)
            voice->Generators[generatorIndex] = (SHORT)Sf2Clamp(
                instrumentValues[generatorIndex] + presetValues[generatorIndex],
                INT16_MIN_VALUE,
                INT16_MAX_VALUE);
        else
            voice->Generators[generatorIndex] = instrumentValues[generatorIndex];
    }

    voice->Channel = channelIndex;
    voice->Key = key;
    voice->Velocity = velocity;
    voice->NoteId = noteId;
    voice->SampleIndex = sampleIndex;
    voice->SampleType = header.SampleType;
    voice->SampleLink = header.SampleLink;
    voice->IsSustained = FALSE;
    voice->PitchLeader = -1;
    voice->LoopMode = (BYTE)(voice->Generators[SF2_GEN_SAMPLE_MODES] & SF2_LOOP_MODE_MASK);
    voice->ExclusiveClass = voice->Generators[SF2_GEN_EXCLUSIVE_CLASS];

    if (voice->LoopMode == SF2_LOOP_UNUSED)
        voice->LoopMode = SF2_LOOP_NONE;

    if (!Sf2VoiceSetAddresses(font, voice, &header))
        return FALSE;

    voice->EffectiveKey = Sf2Substitute(voice->Generators[SF2_GEN_KEYNUM], key);
    voice->EffectiveVelocity = Sf2Substitute(voice->Generators[SF2_GEN_VELOCITY], velocity);
    Sf2VoiceGatherModulators(font, voice, zones);
    Sf2VoiceModulate(synth, voice);

    /* Section 7.10 / gen 58: the root key; section 9.1.1 / gen 56: the key's distance from it. */
    rootKey = voice->Generators[SF2_GEN_OVERRIDING_ROOT_KEY];

    if (rootKey < 0 || rootKey >= SF2_MIDI_VALUES)
        rootKey = header.OriginalPitch < SF2_MIDI_VALUES
            ? header.OriginalPitch
            : SF2_DEFAULT_ROOT_KEY;

    voice->KeyPitchCents = Sf2VoiceParameter(voice, SF2_GEN_SCALE_TUNING)
            * (voice->EffectiveKey - rootKey)
        + header.PitchCorrection;
    sampleRate = header.SampleRate < SF2_MINIMUM_SAMPLE_RATE
        ? SF2_MINIMUM_SAMPLE_RATE
        : header.SampleRate;
    voice->RateRatio = (double)sampleRate / synth->OutputHz;
    Sf2VoiceStartModulation(synth, voice);

    if (voice->ExclusiveClass)
        Sf2CutExclusiveClass(synth, channelIndex, voice->ExclusiveClass, noteId);

    voice->GainLeft = 0;
    voice->GainRight = 0;
    voice->IsFiltered = FALSE;
    voice->State = SF2_VOICE_ON;
    rampFrames = synth->FramesUntilControl ? synth->FramesUntilControl : SF2_SYNTH_BLOCK_FRAMES;
    Sf2VoiceTargets(synth, voice, rampFrames);

    synth->ActiveVoices++;
    synth->Stats.VoicesStarted++;

    if (synth->ActiveVoices > synth->Stats.VoicesPeak)
        synth->Stats.VoicesPeak = synth->ActiveVoices;

    return TRUE;
}

/* Section 7.10: a stereo pair plays "entirely synchronously, with their pitch controlled by
 * the right sample's generators". A left voice whose right partner started with it follows
 * that partner's playback rate.
 */
static VOID Sf2LinkStereoPairs(PSF2_SYNTH synth, DWORD noteId)
{
    DWORD leftIndex;
    DWORD rightIndex;

    for (leftIndex = 0; leftIndex < SF2_SYNTH_VOICES; ++leftIndex)
    {
        PSF2_VOICE left = &synth->Voices[leftIndex];

        if (left->State == SF2_VOICE_FREE
            || left->NoteId != noteId
            || !(left->SampleType & SF2_SAMPLE_LEFT))
            continue;

        for (rightIndex = 0; rightIndex < SF2_SYNTH_VOICES; ++rightIndex)
        {
            PSF2_VOICE right = &synth->Voices[rightIndex];

            if (right->State != SF2_VOICE_FREE
                && right->NoteId == noteId
                && (right->SampleType & SF2_SAMPLE_RIGHT)
                && right->SampleIndex == left->SampleLink)
            {
                left->PitchLeader = (INT)rightIndex;
                left->StepWhole = right->StepWhole;
                left->StepFraction = right->StepFraction;
                break;
            }
        }
    }
}

/* One instrument's zones for one preset zone (section 9.4). The number of voices started. */
static DWORD Sf2NoteOnInstrument(
    PSF2_SYNTH synth,
    BYTE channelIndex,
    BYTE key,
    BYTE velocity,
    DWORD noteId,
    WORD instrumentIndex,
    const SHORT *presetValues,
    const SF2_ZONE *presetGlobalZone,
    const SF2_ZONE *presetZone)
{
    PCSF2_FONT font = synth->Font;
    SHORT globalValues[SF2_GENERATOR_COUNT];
    SF2_ZONE globalZone;
    BOOL hasGlobal = FALSE;
    DWORD firstBag;
    DWORD endBag;
    DWORD bagIndex;
    DWORD generatorIndex;
    DWORD started = 0;

    for (generatorIndex = 0; generatorIndex < SF2_GENERATOR_COUNT; ++generatorIndex)
        globalValues[generatorIndex] = g_Sf2Generators[generatorIndex].Default;

    Sf2FontInstrumentBags(font, instrumentIndex, &firstBag, &endBag);

    for (bagIndex = firstBag; bagIndex < endBag; ++bagIndex)
    {
        SHORT values[SF2_GENERATOR_COUNT];
        const SF2_ZONE *zones[SF2_ZONE_LEVELS];
        SF2_ZONE zone;

        Sf2FontReadZone(font, FALSE, bagIndex, &zone);

        /* Section 7.7: only the first zone may be global; any other without a sampleID is
         * ignored.
         */
        if (!zone.HasTerminal)
        {
            if (bagIndex == firstBag)
            {
                globalZone = zone;
                hasGlobal = TRUE;
                Sf2ApplyZoneGenerators(font, FALSE, &zone, globalValues);
            }

            continue;
        }

        for (generatorIndex = 0; generatorIndex < SF2_GENERATOR_COUNT; ++generatorIndex)
            values[generatorIndex] = globalValues[generatorIndex];

        Sf2ApplyZoneGenerators(font, FALSE, &zone, values);

        /* Section 9.4: the preset zone's ranges filter by intersection (checked by the
         * caller); the instrument zone's here.
         */
        if (!Sf2IsZoneHit(values, key, velocity))
            continue;

        zones[SF2_ZONE_PRESET_GLOBAL] = presetGlobalZone;
        zones[SF2_ZONE_PRESET_LOCAL] = presetZone;
        zones[SF2_ZONE_INSTRUMENT_GLOBAL] = hasGlobal ? &globalZone : NULL;
        zones[SF2_ZONE_INSTRUMENT_LOCAL] = &zone;

        if (Sf2VoiceStart(
                synth,
                channelIndex,
                key,
                velocity,
                noteId,
                values,
                presetValues,
                zones,
                zone.TerminalIndex))
            ++started;
    }

    return started;
}

static VOID Sf2NoteOff(PSF2_SYNTH synth, BYTE channelIndex, BYTE key)
{
    BOOL isSustainDown = synth->Channels[channelIndex].Controllers[SF2_CC_SUSTAIN] >= SF2_PEDAL_ON;
    DWORD voiceIndex;

    for (voiceIndex = 0; voiceIndex < SF2_SYNTH_VOICES; ++voiceIndex)
    {
        PSF2_VOICE voice = &synth->Voices[voiceIndex];

        if (voice->State != SF2_VOICE_ON || voice->Channel != channelIndex || voice->Key != key)
            continue;

        /* Section 9.2, CC64: the key-off is stored and executed when sustain ends. */
        if (isSustainDown)
            voice->IsSustained = TRUE;
        else
            Sf2VoiceRelease(voice);
    }
}

static VOID Sf2NoteOn(PSF2_SYNTH synth, BYTE channelIndex, BYTE key, BYTE velocity)
{
    PCSF2_CHANNEL channel = &synth->Channels[channelIndex];
    PCSF2_FONT font = synth->Font;
    SF2_PRESET_HEADER preset;
    SHORT globalValues[SF2_GENERATOR_COUNT];
    SF2_ZONE globalZone;
    BOOL hasGlobal = FALSE;
    DWORD bagIndex;
    DWORD generatorIndex;
    DWORD noteId;
    DWORD started = 0;
    DWORD voiceIndex;

    synth->Stats.NotesOn++;

    /* A key struck again ends its previous note: the same choice a struck string makes. */
    for (voiceIndex = 0; voiceIndex < SF2_SYNTH_VOICES; ++voiceIndex)
    {
        PSF2_VOICE voice = &synth->Voices[voiceIndex];

        if (voice->State == SF2_VOICE_ON && voice->Channel == channelIndex && voice->Key == key)
            Sf2VoiceRelease(voice);
    }

    if (channel->PresetIndex == SF2_NONE)
    {
        synth->Stats.NotesSilent++;
        return;
    }

    noteId = ++synth->NextNoteId;

    /* Preset level: no defaults (section 8.5: preset generators are only additive), the ranges
     * open until a zone narrows them.
     */
    for (generatorIndex = 0; generatorIndex < SF2_GENERATOR_COUNT; ++generatorIndex)
        globalValues[generatorIndex] = 0;

    globalValues[SF2_GEN_KEY_RANGE] = g_Sf2Generators[SF2_GEN_KEY_RANGE].Default;
    globalValues[SF2_GEN_VELOCITY_RANGE] = g_Sf2Generators[SF2_GEN_VELOCITY_RANGE].Default;
    Sf2FontReadPreset(font, channel->PresetIndex, &preset);

    for (bagIndex = preset.FirstBag; bagIndex < preset.EndBag; ++bagIndex)
    {
        SHORT values[SF2_GENERATOR_COUNT];
        SF2_ZONE zone;

        Sf2FontReadZone(font, TRUE, bagIndex, &zone);

        /* Section 7.3: only the first zone may be global; any other without an Instrument is
         * ignored.
         */
        if (!zone.HasTerminal)
        {
            if (bagIndex == preset.FirstBag)
            {
                globalZone = zone;
                hasGlobal = TRUE;
                Sf2ApplyZoneGenerators(font, TRUE, &zone, globalValues);
            }

            continue;
        }

        for (generatorIndex = 0; generatorIndex < SF2_GENERATOR_COUNT; ++generatorIndex)
            values[generatorIndex] = globalValues[generatorIndex];

        Sf2ApplyZoneGenerators(font, TRUE, &zone, values);

        if (!Sf2IsZoneHit(values, key, velocity))
            continue;

        started += Sf2NoteOnInstrument(
            synth,
            channelIndex,
            key,
            velocity,
            noteId,
            zone.TerminalIndex,
            values,
            hasGlobal ? &globalZone : NULL,
            &zone);
    }

    if (!started)
        synth->Stats.NotesSilent++;
    else
        Sf2LinkStereoPairs(synth, noteId);
}

/* channels: */

/* The preset for the channel's bank and program. Section 7.2: percussion is bank 128. If the
 * bank lacks the program, our fallbacks: drums to bank 128 program 0 (the standard kit); a
 * melodic program to bank 0 (GS variation banks fall back to their capital tone), then to
 * bank 0 program 0, so a GM score on a small font plays rather than falls silent.
 */
static VOID Sf2ChannelSelectPreset(PSF2_SYNTH synth, PSF2_CHANNEL channel)
{
    WORD bank = channel->IsPercussion ? SF2_PERCUSSION_BANK : channel->BankSelect;
    DWORD presetIndex = Sf2FontFindPreset(synth->Font, bank, channel->Program);

    if (presetIndex == SF2_NONE && channel->IsPercussion)
        presetIndex = Sf2FontFindPreset(synth->Font, SF2_PERCUSSION_BANK, 0);

    if (presetIndex == SF2_NONE && !channel->IsPercussion)
        presetIndex = Sf2FontFindPreset(synth->Font, 0, channel->Program);

    if (presetIndex == SF2_NONE && !channel->IsPercussion)
        presetIndex = Sf2FontFindPreset(synth->Font, 0, 0);

    channel->Bank = (BYTE)(bank == SF2_PERCUSSION_BANK ? 0 : bank);
    channel->PresetIndex = presetIndex;
}

static VOID Sf2ChannelReleaseSustained(PSF2_SYNTH synth, BYTE channelIndex)
{
    DWORD voiceIndex;

    for (voiceIndex = 0; voiceIndex < SF2_SYNTH_VOICES; ++voiceIndex)
    {
        PSF2_VOICE voice = &synth->Voices[voiceIndex];

        if (voice->State == SF2_VOICE_ON && voice->Channel == channelIndex && voice->IsSustained)
            Sf2VoiceRelease(voice);
    }
}

/* Reset All Controllers, as the MIDI specification defines it (section 9.2 defers to it):
 * modulation, expression, the pedals, pressure, the bend and the RPN selection. Volume, pan,
 * bank and program are kept.
 */
static VOID Sf2ChannelResetControllers(PSF2_SYNTH synth, BYTE channelIndex)
{
    PSF2_CHANNEL channel = &synth->Channels[channelIndex];
    DWORD controller;

    channel->Controllers[SF2_CC_MODULATION] = 0;
    channel->Controllers[SF2_CC_EXPRESSION] = SF2_DEFAULT_EXPRESSION;

    for (controller = SF2_CC_SUSTAIN; controller <= SF2_CC_SOFT; ++controller)
        channel->Controllers[controller] = 0;

    for (controller = 0; controller < SF2_MIDI_VALUES; ++controller)
        channel->PolyPressure[controller] = 0;

    channel->ChannelPressure = 0;
    channel->PitchBend = SF2_PITCH_BEND_CENTRE;
    channel->RegisteredParameter = SF2_RPN_NULL;
    channel->IsNonRegistered = FALSE;
    channel->Serial++;
    Sf2ChannelReleaseSustained(synth, channelIndex);
}

static VOID Sf2ChannelPowerOn(PSF2_SYNTH synth, BYTE channelIndex)
{
    PSF2_CHANNEL channel = &synth->Channels[channelIndex];
    DWORD serial = channel->Serial;
    DWORD controller;

    for (controller = 0; controller < SF2_SYNTH_CONTROLLERS; ++controller)
        channel->Controllers[controller] = 0;

    channel->Controllers[SF2_CC_VOLUME] = SF2_DEFAULT_VOLUME;
    channel->Controllers[SF2_CC_PAN] = SF2_DEFAULT_PAN;
    channel->Program = 0;
    channel->BankSelect = 0;
    channel->IsPercussion = channelIndex == SF2_SYNTH_PERCUSSION_CHANNEL;
    channel->BendRangeCents = SF2_DEFAULT_BEND_RANGE_CENTS;
    channel->FineTuneCents = 0;
    channel->CoarseTuneSemitones = 0;
    channel->Serial = serial;
    Sf2ChannelResetControllers(synth, channelIndex);
    Sf2ChannelSelectPreset(synth, channel);
}

/* Data entry (CC6 / CC38) for the selected RPN; section 9.2. NRPNs are taken but not acted on
 * (section 9.6's SoundFont NRPNs are not implemented).
 */
static VOID Sf2ChannelDataEntry(PSF2_CHANNEL channel)
{
    BYTE msb = channel->Controllers[SF2_CC_DATA_ENTRY];
    BYTE lsb = channel->Controllers[SF2_CC_DATA_ENTRY_LSB];

    if (channel->IsNonRegistered)
        return;

    switch (channel->RegisteredParameter)
    {
    case SF2_RPN_BEND_RANGE:
        channel->BendRangeCents = (SHORT)(msb * SF2_CENTS_PER_SEMITONE + lsb);
        break;

    case SF2_RPN_FINE_TUNING:
        channel->FineTuneCents = (SHORT)((((msb << SF2_DATA_SHIFT) | lsb) - SF2_PITCH_BEND_CENTRE)
            * SF2_CENTS_PER_SEMITONE
            / SF2_PITCH_BEND_CENTRE);
        break;

    case SF2_RPN_COARSE_TUNING:
        channel->CoarseTuneSemitones = (SHORT)(msb - SF2_COARSE_TUNING_CENTRE);
        break;

    default:
        break;
    }
}

static VOID Sf2ChannelControl(PSF2_SYNTH synth, BYTE channelIndex, BYTE controller, BYTE value)
{
    PSF2_CHANNEL channel = &synth->Channels[channelIndex];
    DWORD voiceIndex;

    channel->Controllers[controller] = value;
    channel->Serial++;

    switch (controller)
    {
    case SF2_CC_BANK_SELECT:
        channel->BankSelect = value;
        break;

    case SF2_CC_DATA_ENTRY:
    case SF2_CC_DATA_ENTRY_LSB:
        Sf2ChannelDataEntry(channel);
        break;

    case SF2_CC_NRPN_LSB:
    case SF2_CC_NRPN_MSB:
        channel->IsNonRegistered = TRUE;
        break;

    case SF2_CC_RPN_LSB:
    case SF2_CC_RPN_MSB:
        channel->IsNonRegistered = FALSE;
        channel->RegisteredParameter = (WORD)(
            (channel->Controllers[SF2_CC_RPN_MSB] << SF2_DATA_SHIFT)
            | channel->Controllers[SF2_CC_RPN_LSB]);
        break;

    case SF2_CC_SUSTAIN:
        if (value < SF2_PEDAL_ON)
            Sf2ChannelReleaseSustained(synth, channelIndex);

        break;

    case SF2_CC_ALL_SOUND_OFF:
        /* Section 9.2: bypass the release phase, whatever the pedals say. */
        for (voiceIndex = 0; voiceIndex < SF2_SYNTH_VOICES; ++voiceIndex)
            if (synth->Voices[voiceIndex].Channel == channelIndex)
                Sf2VoiceFree(synth, &synth->Voices[voiceIndex]);

        break;

    case SF2_CC_RESET_CONTROLLERS:
        Sf2ChannelResetControllers(synth, channelIndex);
        break;

    default:
        /* Section 9.2: All Notes Off enters release "pending their status in SUSTAIN"; the
         * omni / mono / poly messages after it imply it (MIDI 1.0).
         */
        if (controller >= SF2_CC_ALL_NOTES_OFF)
        {
            for (voiceIndex = 0; voiceIndex < SF2_SYNTH_VOICES; ++voiceIndex)
            {
                PSF2_VOICE voice = &synth->Voices[voiceIndex];

                if (voice->State == SF2_VOICE_ON && voice->Channel == channelIndex)
                    Sf2NoteOff(synth, channelIndex, voice->Key);
            }
        }

        break;
    }
}

/* the API: */

VOID Sf2SynthReset(PSF2_SYNTH synth)
{
    DWORD voiceIndex;
    BYTE channelIndex;

    for (voiceIndex = 0; voiceIndex < SF2_SYNTH_VOICES; ++voiceIndex)
        Sf2VoiceFree(synth, &synth->Voices[voiceIndex]);

    for (channelIndex = 0; channelIndex < SF2_SYNTH_CHANNELS; ++channelIndex)
        Sf2ChannelPowerOn(synth, channelIndex);

    synth->MasterGain = 1;
}

VOID Sf2SynthInitialize(PSF2_SYNTH synth, PCSF2_FONT font, DWORD outputHz)
{
    SIZE_T byteIndex;

    for (byteIndex = 0; byteIndex < sizeof(*synth); ++byteIndex)
        ((PBYTE)synth)[byteIndex] = 0;

    synth->Font = font;
    synth->OutputHz = outputHz;
    synth->BlockSeconds = (double)SF2_SYNTH_BLOCK_FRAMES / outputHz;
    Sf2SynthReset(synth);
}

VOID Sf2SynthShortMessage(PSF2_SYNTH synth, DWORD message)
{
    BYTE status = (BYTE)(message & SF2_STATUS_MASK);
    BYTE channelIndex = (BYTE)(message & SF2_CHANNEL_MASK);
    BYTE data1 = (BYTE)((message >> SF2_DATA1_SHIFT) & SF2_DATA_MASK);
    BYTE data2 = (BYTE)((message >> SF2_DATA2_SHIFT) & SF2_DATA_MASK);
    PSF2_CHANNEL channel = &synth->Channels[channelIndex];

    switch (status)
    {
    case SF2_STATUS_NOTE_OFF:
        Sf2NoteOff(synth, channelIndex, data1);
        break;

    case SF2_STATUS_NOTE_ON:
        if (data2)
            Sf2NoteOn(synth, channelIndex, data1, data2);
        else
            Sf2NoteOff(synth, channelIndex, data1);

        break;

    case SF2_STATUS_POLY_PRESSURE:
        channel->PolyPressure[data1] = data2;
        channel->Serial++;
        break;

    case SF2_STATUS_CONTROL:
        Sf2ChannelControl(synth, channelIndex, data1, data2);
        break;

    case SF2_STATUS_PROGRAM:
        channel->Program = data1;
        Sf2ChannelSelectPreset(synth, channel);
        break;

    case SF2_STATUS_CHANNEL_PRESSURE:
        channel->ChannelPressure = data1;
        channel->Serial++;
        break;

    case SF2_STATUS_PITCH_BEND:
        channel->PitchBend = (WORD)(data1 | (data2 << SF2_DATA_SHIFT));
        channel->Serial++;
        break;

    default:
        break;      /* system messages: nothing for a synth to do */
    }
}

VOID Sf2SynthSysEx(PSF2_SYNTH synth, PCBYTE message, DWORD length)
{
    if (length < SF2_SYSEX_GM_LENGTH || message[0] != SF2_SYSEX_START)
        return;

    /* F0 7E <device> 09 0n F7: GM System On (n = 1), Off (2), GM2 On (3). Each means
     * "start from the power-on state".
     */
    if (message[1] == SF2_SYSEX_UNIVERSAL_NON_REALTIME && message[3] == SF2_SYSEX_GENERAL_MIDI)
    {
        Sf2SynthReset(synth);
        synth->Stats.Resets++;
        return;
    }

    /* F0 7F <device> 04 01 <lsb> <msb> F7: master volume. */
    if (length >= SF2_SYSEX_MASTER_VOLUME_LENGTH
        && message[1] == SF2_SYSEX_UNIVERSAL_REALTIME
        && message[3] == SF2_SYSEX_DEVICE_CONTROL
        && message[4] == SF2_SYSEX_MASTER_VOLUME)
    {
        DWORD volume = ((message[6] & SF2_DATA_MASK) << SF2_DATA_SHIFT)
            | (message[5] & SF2_DATA_MASK);

        synth->MasterGain = (FLOAT)volume / (FLOAT)SF2_DATA14_MAXIMUM;
        return;
    }

    /* F0 41 <device> 42 12 <address x3> <data> <sum> F7: Roland GS, data set. */
    if (length >= SF2_SYSEX_GS_LENGTH
        && message[1] == SF2_SYSEX_ROLAND
        && message[3] == SF2_SYSEX_ROLAND_GS
        && message[4] == SF2_SYSEX_ROLAND_DT1
        && message[5] == SF2_SYSEX_GS_PATCH)
    {
        if (message[6] == SF2_SYSEX_GS_SYSTEM && message[7] == SF2_SYSEX_GS_RESET)
        {
            Sf2SynthReset(synth);
            synth->Stats.Resets++;
        }
        else if ((message[6] & SF2_SYSEX_GS_BLOCK_MASK) == SF2_SYSEX_GS_PART_BLOCK
            && message[7] == SF2_SYSEX_GS_RHYTHM_PART)
        {
            BYTE part = message[6] & SF2_SYSEX_GS_PART_MASK;
            BYTE channelIndex = part == SF2_SYSEX_GS_PART_TEN
                ? SF2_SYNTH_PERCUSSION_CHANNEL
                : (BYTE)(part <= SF2_SYSEX_GS_LAST_LOW_PART ? part - 1 : part);
            PSF2_CHANNEL channel = &synth->Channels[channelIndex];

            channel->IsPercussion = message[8] != 0;
            Sf2ChannelSelectPreset(synth, channel);
        }

        return;
    }

    /* F0 43 1n 4C 00 00 7E 00 F7: Yamaha XG System On. */
    if (length >= SF2_SYSEX_XG_LENGTH
        && message[1] == SF2_SYSEX_YAMAHA
        && (message[2] & SF2_SYSEX_YAMAHA_DEVICE_MASK) == SF2_SYSEX_YAMAHA_PARAMETER
        && message[3] == SF2_SYSEX_YAMAHA_XG
        && message[4] == 0
        && message[5] == 0
        && message[6] == SF2_SYSEX_XG_SYSTEM_ON)
    {
        Sf2SynthReset(synth);
        synth->Stats.Resets++;
    }
}

double Sf2VoicePlaybackRate(PCSF2_VOICE voice)
{
    return voice->StepWhole + voice->StepFraction / SF2_FRACTION_ONE;
}

/* One control block on for every voice: envelopes and LFOs advance, finished voices go, the
 * rest get their next targets. A stereo pair's left voice then takes its right partner's rate.
 */
static VOID Sf2SynthControl(PSF2_SYNTH synth)
{
    DWORD voiceIndex;

    for (voiceIndex = 0; voiceIndex < SF2_SYNTH_VOICES; ++voiceIndex)
    {
        PSF2_VOICE voice = &synth->Voices[voiceIndex];
        PCSF2_ENVELOPE volume = &voice->VolumeEnvelope;

        if (voice->State == SF2_VOICE_FREE)
            continue;

        Sf2EnvelopeAdvance(&voice->VolumeEnvelope, TRUE);
        Sf2EnvelopeAdvance(&voice->ModulationEnvelope, FALSE);
        Sf2LfoAdvance(&voice->ModulationLfo);
        Sf2LfoAdvance(&voice->VibratoLfo);

        /* Section 9.1.7: at 96 dB down the gain is zero, so a voice that has decayed there
         * is finished even before its release.
         */
        if (volume->Stage == SF2_STAGE_DONE
            || ((volume->Stage == SF2_STAGE_DECAY || volume->Stage == SF2_STAGE_SUSTAIN)
                && volume->Level >= SF2_SILENT_CENTIBELS))
        {
            Sf2VoiceFree(synth, voice);
            continue;
        }

        Sf2VoiceTargets(synth, voice, SF2_SYNTH_BLOCK_FRAMES);
    }

    for (voiceIndex = 0; voiceIndex < SF2_SYNTH_VOICES; ++voiceIndex)
    {
        PSF2_VOICE voice = &synth->Voices[voiceIndex];
        PCSF2_VOICE leader;

        if (voice->State == SF2_VOICE_FREE || voice->PitchLeader < 0)
            continue;

        leader = &synth->Voices[voice->PitchLeader];

        if (leader->State != SF2_VOICE_FREE && leader->NoteId == voice->NoteId)
        {
            voice->StepWhole = leader->StepWhole;
            voice->StepFraction = leader->StepFraction;
        }
    }
}

/* THE PER-SAMPLE LOOP (Importance = 2):
 * Linear interpolation between a point and the next, the optional biquad, and the ramped gain
 * into the float mix. Section 6.3: in a loop the point after the last looped one is the loop's
 * first, so the next point wraps with the position. FALSE = the sample ran out (no loop).
 */
static BOOL Sf2VoiceRun(PCSF2_SYNTH synth, PSF2_VOICE voice, FLOAT *mix, DWORD frames)
{
    const INT16 *data = synth->Font->Samples;
    BOOL isLooping = voice->LoopMode == SF2_LOOP_CONTINUOUS
        || (voice->LoopMode == SF2_LOOP_UNTIL_RELEASE && voice->State == SF2_VOICE_ON);
    DWORD boundary = isLooping ? voice->LoopEnd : voice->End;
    DWORD wrapPoint = isLooping ? voice->LoopEnd : SF2_NONE;
    DWORD loopStart = voice->LoopStart;
    DWORD loopLength = voice->LoopEnd - voice->LoopStart;
    DWORD position = voice->Position;
    DWORD fraction = voice->Fraction;
    DWORD stepWhole = voice->StepWhole;
    DWORD stepFraction = voice->StepFraction;
    FLOAT gainLeft = voice->GainLeft;
    FLOAT gainRight = voice->GainRight;
    FLOAT stepLeft = voice->GainStepLeft;
    FLOAT stepRight = voice->GainStepRight;
    BOOL isFiltered = voice->IsFiltered;
    FLOAT b0 = voice->FilterB0;
    FLOAT b1 = voice->FilterB1;
    FLOAT b2 = voice->FilterB2;
    FLOAT a1 = voice->FilterA1;
    FLOAT a2 = voice->FilterA2;
    FLOAT x1 = voice->FilterX1;
    FLOAT x2 = voice->FilterX2;
    FLOAT y1 = voice->FilterY1;
    FLOAT y2 = voice->FilterY2;
    BOOL isRunning = TRUE;
    DWORD frame;

    for (frame = 0; frame < frames; ++frame)
    {
        DWORD next = position + 1;
        DWORD advanced;
        FLOAT current;
        FLOAT sample;

        if (next == wrapPoint)
            next = loopStart;

        current = data[position];
        sample = current
            + (data[next] - current) * (FLOAT)(INT32)(fraction >> 1) * SF2_FRACTION_TO_FLOAT;

        if (isFiltered)
        {
            FLOAT output = b0 * sample + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;

            x2 = x1;
            x1 = sample;
            y2 = y1;
            y1 = output;
            sample = output;
        }

        mix[frame * SF2_SYNTH_STEREO] += sample * gainLeft;
        mix[frame * SF2_SYNTH_STEREO + 1] += sample * gainRight;
        gainLeft += stepLeft;
        gainRight += stepRight;
        advanced = fraction + stepFraction;
        position += stepWhole + (advanced < fraction);
        fraction = advanced;

        if (position >= boundary)
        {
            if (!isLooping)
            {
                isRunning = FALSE;
                break;
            }

            position = loopStart + (position - loopStart) % loopLength;
        }
    }

    /* A filter left ringing into silence decays into denormals, which are slow on x87. */
    if (y1 < SF2_FILTER_DENORMAL_FLOOR && y1 > -SF2_FILTER_DENORMAL_FLOOR)
        y1 = 0;

    if (y2 < SF2_FILTER_DENORMAL_FLOOR && y2 > -SF2_FILTER_DENORMAL_FLOOR)
        y2 = 0;

    voice->Position = position;
    voice->Fraction = fraction;
    voice->GainLeft = gainLeft;
    voice->GainRight = gainRight;
    voice->FilterX1 = x1;
    voice->FilterX2 = x2;
    voice->FilterY1 = y1;
    voice->FilterY2 = y2;
    return isRunning;
}

BOOL Sf2SynthRender(PSF2_SYNTH synth, PINT16 output, DWORD frames)
{
    DWORD done = 0;

    if (!synth->ActiveVoices)
        return FALSE;

    while (done < frames)
    {
        DWORD chunk;
        DWORD sampleIndex;
        DWORD voiceIndex;

        if (!synth->FramesUntilControl)
        {
            Sf2SynthControl(synth);
            synth->FramesUntilControl = SF2_SYNTH_BLOCK_FRAMES;
        }

        chunk = frames - done;

        if (chunk > synth->FramesUntilControl)
            chunk = synth->FramesUntilControl;

        for (sampleIndex = 0; sampleIndex < chunk * SF2_SYNTH_STEREO; ++sampleIndex)
            synth->Mix[sampleIndex] = 0;

        for (voiceIndex = 0; voiceIndex < SF2_SYNTH_VOICES; ++voiceIndex)
        {
            PSF2_VOICE voice = &synth->Voices[voiceIndex];

            if (voice->State != SF2_VOICE_FREE && !Sf2VoiceRun(synth, voice, synth->Mix, chunk))
                Sf2VoiceFree(synth, voice);
        }

        /* Saturate: a loud chord clips at full scale, never wraps to the other rail. */
        for (sampleIndex = 0; sampleIndex < chunk * SF2_SYNTH_STEREO; ++sampleIndex)
        {
            FLOAT value = synth->Mix[sampleIndex];

            if (value > INT16_MAX_VALUE)
                value = INT16_MAX_VALUE;
            else if (value < INT16_MIN_VALUE)
                value = INT16_MIN_VALUE;

            output[done * SF2_SYNTH_STEREO + sampleIndex] = (INT16)value;
        }

        done += chunk;
        synth->FramesUntilControl -= chunk;
    }

    return TRUE;
}
