/* opl_synth_test.c -- off-VM battery for the OPL2/OPL3 FM core (vdd_opl_synth.c).
 *
 * T7-T12 are the OPL3 (#232). T7 is the one GOLDEN test in this file, and it is
 * the exception that proves the rule below: it does not claim the OPL2 output is
 * RIGHT, only that it is UNCHANGED -- that adding a second chip moved nothing in
 * the first. The rest are properties again: NEW gates array 1, C0 bits 4/5 route
 * left/right, a 4-op voice is heard only through its algorithm's carriers, and
 * waveforms 4-7 exist only with NEW set.
 *
 * These are PROPERTY tests, not golden-sample comparisons. The core is written
 * from the documented YM3812 behaviour rather than ported, so bit-exactness with
 * real silicon is not the goal and asserting it would only encode my own errors.
 * What must be true for music to be usable is testable directly:
 *
 *   - pitch is CORRECT (measured by counting zero crossings, then compared with
 *     the chip's published formula f = fnum * 49716 / 2^(20-block))
 *   - a key-on produces sound and a key-off eventually produces silence
 *   - louder settings really are louder (total level attenuates monotonically)
 *   - FM actually modulates: a modulator at non-zero level changes the carrier's
 *     waveform rather than being ignored
 *   - nothing ever exceeds int16, whatever the register soup
 */
#include <stdio.h>
#include <string.h>
#include "vdd_opl.h"

VOID VddOplRender(POPL_STATE state, INT16 *output, UINT32 frames);

static INT g_Total = 0, g_Failures = 0;
#define CHECK(condition,message) do{ g_Total++; if(condition){printf("  PASS  %s\n",(message));} \
    else{printf("  FAIL  %s\n",(message)); g_Failures++;} }while(0)

#define OPL_NATIVE_HZ 49716
static INT16 g_Samples[OPL_NATIVE_HZ];              /* one second                     */

static OPL_STATE g_Opl;

/* Operator index -> register offset. The banks skip 0x06/0x07 and 0x0E/0x0F. */
static BYTE OplSynthTestOperatorRegister(INT operatorIndex) { return (BYTE)(operatorIndex + 2 * (operatorIndex / 6)); }

/* Give an operator an instant attack that holds at full volume, so a test hears a
   steady tone rather than a transient, at the requested total level. */
static VOID OplSynthTestSetOperator(INT operatorIndex, BYTE totalLevel)
{
    VddOplWriteRegister(&g_Opl, (BYTE)(0x20 + OplSynthTestOperatorRegister(operatorIndex)), 0x21);   /* EGT=1 MULT=1  */
    VddOplWriteRegister(&g_Opl, (BYTE)(0x40 + OplSynthTestOperatorRegister(operatorIndex)), totalLevel);
    VddOplWriteRegister(&g_Opl, (BYTE)(0x60 + OplSynthTestOperatorRegister(operatorIndex)), 0xF0);   /* AR=15 DR=0    */
    VddOplWriteRegister(&g_Opl, (BYTE)(0x80 + OplSynthTestOperatorRegister(operatorIndex)), 0x0F);   /* SL=0  RR=15   */
}

/* Program a sustained tone on channel `c` and key it on. The MODULATOR is muted
   by default (TL max): without that it defaults to full volume and every test
   measures a two-operator blend instead of the thing it means to measure. */
static VOID OplSynthTestNoteOn(INT channel, WORD fnum, BYTE block, BYTE totalLevel)
{
    OplSynthTestSetOperator(VddOplOperatorIndex(channel, 0), 0x3F);                          /* silent mod   */
    OplSynthTestSetOperator(VddOplOperatorIndex(channel, 1), totalLevel);                            /* carrier      */
    VddOplWriteRegister(&g_Opl, (BYTE)(0xC0 + channel), 0x01);            /* additive     */
    VddOplWriteRegister(&g_Opl, (BYTE)(0xA0 + channel), (BYTE)(fnum & 0xFF));
    VddOplWriteRegister(&g_Opl, (BYTE)(0xB0 + channel),
                      (BYTE)(0x20 | (block << 2) | ((fnum >> 8) & 3)));
}

/* Count positive-going zero crossings -> cycles -> Hz. */
static double OplSynthTestMeasureHz(const INT16 *samples, INT count)
{
    INT index, crossings = 0, wasPositive = 0;
    for (index = 0; index < count; ++index) {
        INT isPositive = samples[index] > 0;
        if (isPositive && !wasPositive) crossings++;
        wasPositive = isPositive;
    }
    return (double)crossings * OPL_NATIVE_HZ / (double)count;
}
static long OplSynthTestRms(const INT16 *samples, INT count)
{
    long long accumulator = 0; INT index;
    for (index = 0; index < count; ++index) accumulator += (long long)samples[index] * samples[index];
    return (long)(accumulator / (count ? count : 1));
}

/* ── OPL3 helpers (#232) ──────────────────────────────────────────────────── */
static INT16 g_StereoSamples[2 * 8192];                /* interleaved L/R                */

/* Operator 0-35 -> 9-bit register offset: array 1 is 0x100 + the same layout.  */
static WORD OplSynthTestOperatorRegister9(INT operatorIndex)
{
    INT array = operatorIndex / 18, slot = operatorIndex % 18;
    return (WORD)(array * 0x100 + slot + 2 * (slot / 6));
}
static VOID OplSynthTestWrite9(WORD registerIndex, BYTE byteValue) { VddOplWriteRegister(&g_Opl, registerIndex, byteValue); }
/* A steady full-level operator, or (live=0) one that never attacks: AR=0 holds
   it at full attenuation, so it outputs exactly 0 -- and so modulates nothing. */
static VOID OplSynthTestOperator9(INT operatorIndex, INT isLive, BYTE wave)
{
    OplSynthTestWrite9((WORD)(0x20 + OplSynthTestOperatorRegister9(operatorIndex)), 0x21);
    OplSynthTestWrite9((WORD)(0x40 + OplSynthTestOperatorRegister9(operatorIndex)), 0x00);
    OplSynthTestWrite9((WORD)(0x60 + OplSynthTestOperatorRegister9(operatorIndex)), isLive ? 0xF0 : 0x00);
    OplSynthTestWrite9((WORD)(0x80 + OplSynthTestOperatorRegister9(operatorIndex)), 0x0F);
    OplSynthTestWrite9((WORD)(0xE0 + OplSynthTestOperatorRegister9(operatorIndex)), wave);
}
/* Program channel c (0-17) and key it on: `c0` is the whole C0 byte. */
static VOID OplSynthTestChannel9(INT channel, BYTE c0Byte, WORD fnum, BYTE block)
{
    WORD registerOffset = (WORD)((channel / 9) * 0x100 + channel % 9);
    OplSynthTestWrite9((WORD)(0xC0 + registerOffset), c0Byte);
    OplSynthTestWrite9((WORD)(0xA0 + registerOffset), (BYTE)(fnum & 0xFF));
    OplSynthTestWrite9((WORD)(0xB0 + registerOffset), (BYTE)(0x20 | (block << 2) | ((fnum >> 8) & 3)));
}
static VOID OplSynthTestOpl3Fresh(INT isNew)
{
    memset(&g_Opl, 0, sizeof g_Opl); g_Opl.IsOpl3 = 1; VddOplReset(&g_Opl);
    if (isNew) OplSynthTestWrite9(0x105, 0x01);
}
static long OplSynthTestRmsSide(const INT16 *samples, INT count, INT side)
{
    long long accumulator = 0; INT index;
    for (index = 0; index < count; ++index) accumulator += (long long)samples[2 * index + side] * samples[2 * index + side];
    return (long)(accumulator / (count ? count : 1));
}
static UINT32 OplSynthTestFnv16(UINT32 hash, const INT16 *samples, INT count)
{
    INT index;
    for (index = 0; index < count; ++index) {
        hash ^= (BYTE)samples[index];                    hash *= 16777619u;
        hash ^= (BYTE)((WORD)samples[index] >> 8);   hash *= 16777619u;
    }
    return hash;
}

/* ── THE OPL2 GOLDEN. A fixed register sequence touching every OPL2 feature the
     synth models -- 9 voices, feedback, both connections, the four waveforms
     (with WSE), KSL, AM/VIB at full depth, key-offs, rhythm bass drum and
     tom-tom -- rendered and hashed. The checksum was taken from the build BEFORE
     the OPL3 existed (42a9029); an OPL2 must still produce it sample for sample,
     and so must an OPL3 with NEW clear. `which` selects the render path:
     0 mono, 1 the left of the stereo render, 2 its right.

   ⚠ #139 CHANGED THE FULL HASH, DELIBERATELY, AND ONLY FROM THE RHYTHM WRITE ON.
     The sequence enters rhythm mode with channel 7 still keyed and then keys the
     hi-hat, cymbal and snare -- three voices that were silent before #139 -- and a
     drum bit is now OR'd with its channel's key bit (as the reference does), which
     also changes what is still sounding after rhythm mode is left. So the golden is
     now TWO numbers: OPL2_GOLDEN_MELODIC hashes the 16800 samples BEFORE the first
     0xBD rhythm write and is the pre-OPL3 value's own prefix, unchanged by #139
     (a sample-by-sample dump of the old and new builds differs first at sample
     16800); OPL2_GOLDEN is the whole run, re-taken at #139. Against the reference,
     the three segments after that point moved from 0.975/0.966/0.980 best-lag
     correlation to 0.988/0.983/0.998, the melodic one stayed at 0.9995. */
#define OPL2_GOLDEN_MELODIC 0xD827E8E0u     /* samples 0-16799: must NEVER change    */
#define OPL2_GOLDEN         0x48947CCEu     /* the whole run, as of #139 (it was
                                               0xA60B79B9 from 42a9029 until then)  */
static UINT32 g_Hash, g_HashMelodic;
static VOID OplSynthTestEat(INT which, INT count)
{
    INT index;
    if (which == 0) { VddOplRender(&g_Opl, g_Samples, (UINT32)count); g_Hash = OplSynthTestFnv16(g_Hash, g_Samples, count); return; }
    VddOplRenderStereo(&g_Opl, g_StereoSamples, (UINT32)count);
    for (index = 0; index < count; ++index) g_Samples[index] = g_StereoSamples[2 * index + (which - 1)];
    g_Hash = OplSynthTestFnv16(g_Hash, g_Samples, count);
}
static VOID OplSynthTestWrite(UINT registerIndex, UINT value) { VddOplWriteRegister(&g_Opl, (BYTE)registerIndex, (BYTE)value); }
static UINT32 OplSynthTestGoldenRun(INT isOpl3, INT which)
{
    INT channel, round;
    memset(&g_Opl, 0, sizeof g_Opl); g_Opl.IsOpl3 = (BYTE)isOpl3; VddOplReset(&g_Opl);
    g_Hash = 2166136261u;
    OplSynthTestWrite(0x01, 0x20);                                 /* WSE                       */
    OplSynthTestWrite(0xBD, 0xC0);                                 /* deep AM + VIB             */
    for (channel = 0; channel < 9; ++channel) {
        INT modulator = VddOplOperatorIndex(channel, 0), carrier = VddOplOperatorIndex(channel, 1);
        UINT modulatorOffset = (UINT)(modulator + 2 * (modulator / 6)), carrierOffset = (UINT)(carrier + 2 * (carrier / 6));
        OplSynthTestWrite(0x20 + modulatorOffset, 0x21 | ((channel & 1) << 7) | ((channel & 2) << 5) | (channel & 4 ? 0x10 : 0) | (channel % 5));
        OplSynthTestWrite(0x20 + carrierOffset, 0x21 | ((channel & 2) << 6));
        OplSynthTestWrite(0x40 + modulatorOffset, (UINT)(0x10 + channel * 3) | ((channel % 4) << 6));
        OplSynthTestWrite(0x40 + carrierOffset, (UINT)(channel * 2) | (((channel + 1) % 4) << 6));
        OplSynthTestWrite(0x60 + modulatorOffset, 0xF0 - (UINT)channel * 0x11 + 3);
        OplSynthTestWrite(0x60 + carrierOffset, 0xD2 + (UINT)channel);
        OplSynthTestWrite(0x80 + modulatorOffset, 0x35 + (UINT)channel * 0x10);
        OplSynthTestWrite(0x80 + carrierOffset, 0x24 + (UINT)channel);
        OplSynthTestWrite(0xE0 + modulatorOffset, (UINT)channel & 3);
        OplSynthTestWrite(0xE0 + carrierOffset, (UINT)(channel + 1) & 3);
        OplSynthTestWrite(0xC0 + channel, (UINT)((channel * 3) & 0x0E) | (UINT)(channel & 1));
        OplSynthTestWrite(0xA0 + channel, 0x40 + (UINT)channel * 23);
        OplSynthTestWrite(0xB0 + channel, 0x20 | (UINT)((2 + channel % 5) << 2) | (UINT)(channel & 3));
        OplSynthTestEat(which, 700);
    }
    OplSynthTestEat(which, 6000);
    for (channel = 0; channel < 9; channel += 2) { OplSynthTestWrite(0xB0 + channel, g_Opl.Registers[0xB0 + channel] & ~0x20); OplSynthTestEat(which, 900); }
    g_HashMelodic = g_Hash;                         /* everything before rhythm  */
    OplSynthTestWrite(0xBD, 0xE0 | 0x10 | 0x04);                   /* rhythm: bass drum + tom   */
    OplSynthTestEat(which, 4000);
    OplSynthTestWrite(0xBD, 0xE0 | 0x0B);
    OplSynthTestEat(which, 3000);
    OplSynthTestWrite(0xBD, 0x00);
    for (round = 0; round < 8; ++round) OplSynthTestEat(which, 4000);
    return g_Hash;
}

/* ── RHYTHM MODE: SNARE, HI-HAT, CYMBAL (#139) ──────────────────────────────────
     The rules in vdd_opl_synth.c were measured against a reference core
     (tools/oplref/oplprobe rphase / noise / restart / keyor); these checks need no
     reference. They restate each rule HERE, independently -- the noise as a
     one-bit-at-a-time LFSR rather than the synth's nine-at-once, the phases from
     the published F-number formula -- and hold the synth's output to it sample by
     sample, through what can be read off a sample without inverting anything:
       cymbal  phase B<<9 | 0x080       -> sign is B, magnitude sin(45 deg)
       snare   phase S<<9 | (S^n)<<8    -> loud iff S^n, and then its sign is S
       hi-hat  phase B<<9 | 0x0D0/0x034 -> sign is B, loud (0x0D0) iff B^n
     Each stream is checked from the SECOND sample after its key-on: the first is
     still at the envelope's starting attenuation (silent) and carries no phase. */
static const BYTE g_MultiplierTimesTwo[16] = { 1, 2, 4, 6, 8, 10, 12, 14, 16, 18, 20, 20, 24, 24, 30, 30 };
static UINT32 OplSynthTestIncrement(WORD fnum, BYTE block, BYTE mult) { return ((UINT32)(fnum << block) * g_MultiplierTimesTwo[mult]) >> 1; }
static UINT32 OplSynthTestPhaseBit(UINT32 phase13, UINT32 phase17)
{
    return (((phase13 >> 2) ^ (phase13 >> 7)) | ((phase13 >> 3) ^ (phase17 >> 5)) | ((phase17 >> 3) ^ (phase17 >> 5))) & 1;
}
/* The noise, one step at a time: u[i] = u[i-9] ^ u[i-23], seeded with the synth's
   power-on window (bit k = u[k]); sample m reads u[72(m+1)] (hi-hat) and
   u[72(m+1)+6] (snare). */
#define T_NSAMP 6000
static BYTE g_Noise[72 * (T_NSAMP + 2) + 32];
static VOID OplSynthTestNoiseInitialize(VOID)
{
    INT index, count = (INT)sizeof g_Noise;
    for (index = 0; index < 23; ++index) g_Noise[index] = (BYTE)((OPL_NOISE_SEED >> index) & 1);
    for (index = 23; index < count; ++index) g_Noise[index] = g_Noise[index - 9] ^ g_Noise[index - 23];
}
static VOID OplSynthTestRhythmSetup(WORD fnum7, BYTE block7, WORD fnum8, BYTE block8, BYTE multiplier13, BYTE multiplier17)
{
    INT index;
    memset(&g_Opl, 0, sizeof g_Opl); VddOplReset(&g_Opl);
    VddOplWriteRegister(&g_Opl, 0x01, 0x20);
    for (index = 0; index < 6; ++index) {
        BYTE operatorOffset = (BYTE)(0x10 + index), mult = (BYTE)(index == 1 ? multiplier13 : index == 5 ? multiplier17 : 1);
        VddOplWriteRegister(&g_Opl, (BYTE)(0x20 + operatorOffset), (BYTE)(0x20 | mult));   /* EGT     */
        VddOplWriteRegister(&g_Opl, (BYTE)(0x40 + operatorOffset), 0x00);
        VddOplWriteRegister(&g_Opl, (BYTE)(0x60 + operatorOffset), 0xF0);                     /* AR=15   */
        VddOplWriteRegister(&g_Opl, (BYTE)(0x80 + operatorOffset), 0x0F);
        VddOplWriteRegister(&g_Opl, (BYTE)(0xE0 + operatorOffset), 0x00);
    }
    VddOplWriteRegister(&g_Opl, 0xA6, 0x00); VddOplWriteRegister(&g_Opl, 0xB6, 0x10);
    VddOplWriteRegister(&g_Opl, 0xA7, (BYTE)fnum7); VddOplWriteRegister(&g_Opl, 0xB7, (BYTE)((block7 << 2) | (fnum7 >> 8)));
    VddOplWriteRegister(&g_Opl, 0xA8, (BYTE)fnum8); VddOplWriteRegister(&g_Opl, 0xB8, (BYTE)((block8 << 2) | (fnum8 >> 8)));
}
/* Mean-removed autocorrelation at `lag`, normalised: the fraction of a signal that
   repeats with that period. No libm needed. */
static double OplSynthTestPeriodicity(const INT16 *samples, INT count, INT lag)
{
    double mean = 0, numerator = 0, denominator = 0; INT index;
    for (index = 0; index < count; ++index) mean += samples[index];
    mean /= count;
    for (index = 0; index + lag < count; ++index) { numerator += (samples[index] - mean) * (samples[index + lag] - mean); denominator += (samples[index] - mean) * (samples[index] - mean); }
    return denominator > 0 ? numerator / denominator : 0;
}

static VOID OplSynthTestRhythm(VOID)
{
    static INT16 firstRender[T_NSAMP], secondRender[T_NSAMP];
    /* The register setups: the default test pitch, and two with unrelated pitches
       and MULTs so the five accumulator bits vary independently. */
    static const struct { WORD Fnum7; BYTE Block7; WORD Fnum8; BYTE Block8; BYTE Multiplier13, Multiplier17; } setups[3] = {
        { 0x200, 4, 0x200, 4, 1, 1 }, { 0x1A3, 5, 0x2F1, 4, 3, 5 }, { 0x0B7, 6, 0x3E5, 2, 1, 7 } };
    INT setup, sample, wrongCount;
    CHAR description[160];
    OplSynthTestNoiseInitialize();

    for (setup = 0; setup < 3; ++setup) {
        UINT32 increment13 = OplSynthTestIncrement(setups[setup].Fnum7, setups[setup].Block7, setups[setup].Multiplier13), increment17 = OplSynthTestIncrement(setups[setup].Fnum8, setups[setup].Block8, setups[setup].Multiplier17);
        /* CYMBAL */
        OplSynthTestRhythmSetup(setups[setup].Fnum7, setups[setup].Block7, setups[setup].Fnum8, setups[setup].Block8, setups[setup].Multiplier13, setups[setup].Multiplier17);
        VddOplWriteRegister(&g_Opl, 0xBD, 0x20 | 0x02);
        VddOplRender(&g_Opl, firstRender, T_NSAMP);
        for (wrongCount = 0, sample = 1; sample < T_NSAMP; ++sample) {
            UINT32 phase13 = (((UINT32)(sample + 1) * increment13) >> 10) & 1023, phase17 = (((UINT32)(sample + 1) * increment17) >> 10) & 1023;
            INT expectedSign = OplSynthTestPhaseBit(phase13, phase17) ? -1 : 1;
            if (!((firstRender[sample] > 5000 && expectedSign > 0) || (firstRender[sample] < -5000 && expectedSign < 0))) wrongCount++;
        }
        snprintf(description, sizeof description, "cymbal, setup %d: sign stream is the phase bit B, every sample (%d wrong)", setup, wrongCount);
        CHECK(wrongCount == 0, description);
        /* SNARE */
        OplSynthTestRhythmSetup(setups[setup].Fnum7, setups[setup].Block7, setups[setup].Fnum8, setups[setup].Block8, setups[setup].Multiplier13, setups[setup].Multiplier17);
        VddOplWriteRegister(&g_Opl, 0xBD, 0x20 | 0x08);
        VddOplRender(&g_Opl, firstRender, T_NSAMP);
        for (wrongCount = 0, sample = 1; sample < T_NSAMP; ++sample) {
            UINT32 phase13 = (((UINT32)(sample + 1) * increment13) >> 10) & 1023, snareBit = (phase13 >> 8) & 1;
            UINT32 isLoud = snareBit ^ g_Noise[72 * (sample + 1) + 6];
            if (isLoud ? !((snareBit && firstRender[sample] < -7000) || (!snareBit && firstRender[sample] > 7000)) : (firstRender[sample] > 200 || firstRender[sample] < -200)) wrongCount++;
        }
        snprintf(description, sizeof description, "snare, setup %d: loud iff S^noise, sign = op13 bit 8, every sample (%d wrong)", setup, wrongCount);
        CHECK(wrongCount == 0, description);
        /* HI-HAT */
        OplSynthTestRhythmSetup(setups[setup].Fnum7, setups[setup].Block7, setups[setup].Fnum8, setups[setup].Block8, setups[setup].Multiplier13, setups[setup].Multiplier17);
        VddOplWriteRegister(&g_Opl, 0xBD, 0x20 | 0x01);
        VddOplRender(&g_Opl, firstRender, T_NSAMP);
        for (wrongCount = 0, sample = 1; sample < T_NSAMP; ++sample) {
            UINT32 phase13 = (((UINT32)(sample + 1) * increment13) >> 10) & 1023, phase17Previous = (((UINT32)sample * increment17) >> 10) & 1023;
            UINT32 phaseBit = OplSynthTestPhaseBit(phase13, phase17Previous), isLoud = phaseBit ^ g_Noise[72 * (sample + 1)];
            INT magnitude = firstRender[sample] < 0 ? -firstRender[sample] : firstRender[sample];
            if ((phaseBit ? firstRender[sample] >= 0 : firstRender[sample] <= 0) || (isLoud ? magnitude < 6000 : (magnitude < 1500 || magnitude > 3500))) wrongCount++;
        }
        snprintf(description, sizeof description, "hi-hat, setup %d: sign = B (op17 one sample behind), loud iff B^noise (%d wrong)", setup, wrongCount);
        CHECK(wrongCount == 0, description);
    }

    /* The noise alone: op13/op17 frozen (F-num 0) makes B = 0, so the hi-hat's
       loud/quiet stream IS the noise -- and it must be the LFSR's, from power-on. */
    OplSynthTestRhythmSetup(0, 0, 0, 0, 1, 1);
    VddOplWriteRegister(&g_Opl, 0xBD, 0x20 | 0x01);
    VddOplRender(&g_Opl, firstRender, T_NSAMP);
    {   INT onesCount = 0;
        for (wrongCount = 0, sample = 1; sample < T_NSAMP; ++sample) {
            INT isLoud = firstRender[sample] > 6000;
            onesCount += isLoud;
            if (isLoud != g_Noise[72 * (sample + 1)]) wrongCount++;
        }
        snprintf(description, sizeof description, "noise: hi-hat with frozen accumulators follows the 23-bit LFSR from reset (%d wrong, %d/%d ones)",
                 wrongCount, onesCount, T_NSAMP);
        CHECK(wrongCount == 0 && onesCount > T_NSAMP * 4 / 10 && onesCount < T_NSAMP * 6 / 10, description); }

    /* A key-on into a RUNNING chip restarts op13 one step further on than reset
       does (oplprobe restart): the hi-hat keyed at sample 1000, op17 free-running. */
    {   UINT32 increment13 = OplSynthTestIncrement(0x1A3, 5, 3), increment17 = OplSynthTestIncrement(0x2F1, 4, 5);
        const INT keyOnSample = 1000;
        OplSynthTestRhythmSetup(0x1A3, 5, 0x2F1, 4, 3, 5);
        VddOplWriteRegister(&g_Opl, 0xBD, 0x20);
        VddOplRender(&g_Opl, firstRender, keyOnSample);
        VddOplWriteRegister(&g_Opl, 0xBD, 0x21);
        VddOplRender(&g_Opl, firstRender + keyOnSample, T_NSAMP - keyOnSample);
        for (wrongCount = 0, sample = keyOnSample + 1; sample < T_NSAMP; ++sample) {
            UINT32 phase13 = (((UINT32)(sample - keyOnSample + 2) * increment13) >> 10) & 1023, phase17Previous = (((UINT32)sample * increment17) >> 10) & 1023;
            UINT32 phaseBit = OplSynthTestPhaseBit(phase13, phase17Previous);
            if (phaseBit ? firstRender[sample] >= 0 : firstRender[sample] <= 0) wrongCount++;
        }
        snprintf(description, sizeof description, "restart: hi-hat keyed into a running chip -- op13 restarts one step on (%d wrong)", wrongCount);
        CHECK(wrongCount == 0, description); }

    /* Character, without a reference: how much of each voice repeats with the
       128-sample period of the default test pitch. Measured on the reference as
       tonality 0.003 / 0.509 / 0.749 (hi-hat / snare / cymbal). */
    {   double hiHatFraction, snareFraction, cymbalFraction;
        OplSynthTestRhythmSetup(0x200, 4, 0x200, 4, 1, 1); VddOplWriteRegister(&g_Opl, 0xBD, 0x21);
        VddOplRender(&g_Opl, firstRender, T_NSAMP); hiHatFraction = OplSynthTestPeriodicity(firstRender, T_NSAMP, 128);
        OplSynthTestRhythmSetup(0x200, 4, 0x200, 4, 1, 1); VddOplWriteRegister(&g_Opl, 0xBD, 0x28);
        VddOplRender(&g_Opl, firstRender, T_NSAMP); snareFraction = OplSynthTestPeriodicity(firstRender, T_NSAMP, 128);
        OplSynthTestRhythmSetup(0x200, 4, 0x200, 4, 1, 1); VddOplWriteRegister(&g_Opl, 0xBD, 0x22);
        VddOplRender(&g_Opl, firstRender, T_NSAMP); cymbalFraction = OplSynthTestPeriodicity(firstRender, T_NSAMP, 128);
        printf("        periodic fraction at the note's period: hi-hat %.3f snare %.3f cymbal %.3f\n", hiHatFraction, snareFraction, cymbalFraction);
        CHECK(hiHatFraction < 0.1, "character: the hi-hat is noise -- no dominant tone");
        CHECK(snareFraction > 0.35 && snareFraction < 0.65, "character: the snare is half tone, half noise");
        CHECK(cymbalFraction > 0.95, "character: the cymbal is fully periodic -- no noise in it"); }

    /* Determinism: the whole kit, twice from reset, bit-identical. */
    {   UINT32 firstHash, secondHash;
        OplSynthTestRhythmSetup(0x1A3, 5, 0x2F1, 4, 3, 5); VddOplWriteRegister(&g_Opl, 0xBD, 0x3F);
        VddOplRender(&g_Opl, firstRender, T_NSAMP); firstHash = OplSynthTestFnv16(2166136261u, firstRender, T_NSAMP);
        OplSynthTestRhythmSetup(0x1A3, 5, 0x2F1, 4, 3, 5); VddOplWriteRegister(&g_Opl, 0xBD, 0x3F);
        VddOplRender(&g_Opl, secondRender, T_NSAMP); secondHash = OplSynthTestFnv16(2166136261u, secondRender, T_NSAMP);
        CHECK(firstHash == secondHash && OplSynthTestRms(firstRender, T_NSAMP) > 1000000, "determinism: all five drums, twice from reset, bit-identical"); }

    /* A drum bit and its channel's key bit are OR'd (oplprobe keyor): with channel
       8's key already holding op14, the tom-tom bit changes nothing at all. */
    {   INT index;
        for (index = 0; index < 2; ++index) {
            OplSynthTestRhythmSetup(0x200, 4, 0x200, 4, 1, 1);
            VddOplWriteRegister(&g_Opl, 0x72, 0xF4);                          /* op14 AR15 DR4  */
            VddOplWriteRegister(&g_Opl, 0x92, 0xF4);                          /* SL15 RR4       */
            VddOplWriteRegister(&g_Opl, 0xB8, 0x20 | (4 << 2) | 2);           /* ch8 key on     */
            VddOplWriteRegister(&g_Opl, 0xBD, 0x20);
            VddOplRender(&g_Opl, index ? secondRender : firstRender, 3000);
            if (index) VddOplWriteRegister(&g_Opl, 0xBD, 0x24);                   /* + tom-tom bit  */
            VddOplRender(&g_Opl, (index ? secondRender : firstRender) + 3000, T_NSAMP - 3000);
        }
        CHECK(OplSynthTestFnv16(2166136261u, firstRender, T_NSAMP) == OplSynthTestFnv16(2166136261u, secondRender, T_NSAMP) && OplSynthTestRms(firstRender + 3000, 2000) > 0,
              "keying: a drum bit on an operator its channel key already holds restarts nothing");
        CHECK(g_Opl.ProfileRhythmHits[2] == 1, "keying: ... and the tom-tom hit is still counted"); }

    /* OPL3 routing: hi-hat and snare follow channel 7's C0, the cymbal channel 8's. */
    {   long leftLevel, rightLevel;
        OplSynthTestRhythmSetup(0x200, 4, 0x200, 4, 1, 1);         /* an OPL2 reset ...          */
        g_Opl.IsOpl3 = 1; OplSynthTestWrite9(0x105, 0x01);                /* ... made an OPL3, NEW set  */
        OplSynthTestWrite9(0xC6, 0x20); OplSynthTestWrite9(0xC7, 0x10); OplSynthTestWrite9(0xC8, 0x20);                  /* ch7 left only  */
        OplSynthTestWrite9(0xBD, 0x20 | 0x09);                                            /* hi-hat + snare */
        VddOplRenderStereo(&g_Opl, g_StereoSamples, 4096);
        leftLevel = OplSynthTestRmsSide(g_StereoSamples, 4096, 0); rightLevel = OplSynthTestRmsSide(g_StereoSamples, 4096, 1);
        CHECK(leftLevel > 1000000 && rightLevel == 0, "OPL3 routing: hi-hat and snare go where channel 7's C0 sends them");
        OplSynthTestWrite9(0xBD, 0x20); OplSynthTestWrite9(0xBD, 0x20 | 0x02);                            /* cymbal         */
        VddOplRenderStereo(&g_Opl, g_StereoSamples, 8192);
        leftLevel = OplSynthTestRmsSide(g_StereoSamples + 2 * 4096, 4096, 0); rightLevel = OplSynthTestRmsSide(g_StereoSamples + 2 * 4096, 4096, 1);
        CHECK(rightLevel > 1000000 && leftLevel < rightLevel / 1000, "OPL3 routing: the cymbal goes where channel 8's C0 sends it"); }
}

INT main(VOID)
{
    double frequency, expected;
    long loudLevel, quietLevel, offLevel;

    printf("== sound epic: OPL2 FM synthesis battery ==\n");

    /* T1: pitch accuracy against the chip's published formula ---------------- */
    /* fnum=0x200, block=4 -> 512 * 49716 / 2^16 = 388.4 Hz                     */
    memset(&g_Opl, 0, sizeof g_Opl); VddOplReset(&g_Opl);
    OplSynthTestNoteOn(0, 0x200, 4, 0);
    VddOplRender(&g_Opl, g_Samples, OPL_NATIVE_HZ / 4);
    frequency   = OplSynthTestMeasureHz(g_Samples, OPL_NATIVE_HZ / 4);
    expected = 512.0 * OPL_NATIVE_HZ / 65536.0;
    printf("        measured %.1f Hz, expected %.1f Hz\n", frequency, expected);
    CHECK(frequency > expected * 0.98 && frequency < expected * 1.02, "pitch: fnum=0x200 block=4 within 2%");

    /* an octave up must double the frequency                                   */
    memset(&g_Opl, 0, sizeof g_Opl); VddOplReset(&g_Opl);
    OplSynthTestNoteOn(0, 0x200, 5, 0);
    VddOplRender(&g_Opl, g_Samples, OPL_NATIVE_HZ / 4);
    frequency = OplSynthTestMeasureHz(g_Samples, OPL_NATIVE_HZ / 4);
    printf("        measured %.1f Hz, expected %.1f Hz\n", frequency, expected * 2.0);
    CHECK(frequency > expected * 2.0 * 0.98 && frequency < expected * 2.0 * 1.02, "pitch: block+1 is one octave up");

    /* a different fnum scales linearly                                         */
    memset(&g_Opl, 0, sizeof g_Opl); VddOplReset(&g_Opl);
    OplSynthTestNoteOn(0, 0x100, 4, 0);
    VddOplRender(&g_Opl, g_Samples, OPL_NATIVE_HZ / 4);
    frequency = OplSynthTestMeasureHz(g_Samples, OPL_NATIVE_HZ / 4);
    CHECK(frequency > expected * 0.5 * 0.97 && frequency < expected * 0.5 * 1.03, "pitch: half the F-number is half the pitch");

    /* T2: key-on makes sound, key-off eventually silences -------------------- */
    memset(&g_Opl, 0, sizeof g_Opl); VddOplReset(&g_Opl);
    OplSynthTestNoteOn(0, 0x200, 4, 0);
    VddOplRender(&g_Opl, g_Samples, 4096);
    loudLevel = OplSynthTestRms(g_Samples, 4096);
    CHECK(loudLevel > 10000, "key-on: channel produces signal");

    VddOplWriteRegister(&g_Opl, 0xB0, 0x10);                    /* key-off           */
    VddOplRender(&g_Opl, g_Samples, OPL_NATIVE_HZ / 2);           /* let release finish */
    VddOplRender(&g_Opl, g_Samples, 4096);
    offLevel = OplSynthTestRms(g_Samples, 4096);
    CHECK(offLevel < loudLevel / 100, "key-off: decays to silence");

    /* T3: total level attenuates ------------------------------------------- */
    memset(&g_Opl, 0, sizeof g_Opl); VddOplReset(&g_Opl);
    OplSynthTestNoteOn(0, 0x200, 4, 0x20);                             /* -24 dB            */
    VddOplRender(&g_Opl, g_Samples, 4096);
    quietLevel = OplSynthTestRms(g_Samples, 4096);
    CHECK(quietLevel < loudLevel / 4 && quietLevel > 0, "total level: higher TL is quieter but audible");

    /* T4: FM actually modulates -------------------------------------------- */
    {
        long plainLevel, fmLevel;
        memset(&g_Opl, 0, sizeof g_Opl); VddOplReset(&g_Opl);
        OplSynthTestNoteOn(0, 0x200, 4, 0);
        VddOplWriteRegister(&g_Opl, 0xC0, 0x00);                /* FM, modulator muted */
        VddOplRender(&g_Opl, g_Samples, 8192);
        plainLevel = OplSynthTestRms(g_Samples, 8192);

        memset(&g_Opl, 0, sizeof g_Opl); VddOplReset(&g_Opl);
        OplSynthTestNoteOn(0, 0x200, 4, 0);
        VddOplWriteRegister(&g_Opl, 0xC0, 0x00);
        OplSynthTestSetOperator(VddOplOperatorIndex(0, 0), 0x00);               /* modulator at full   */
        VddOplWriteRegister(&g_Opl, 0xB0, 0x00);                /* re-key so it starts */
        VddOplWriteRegister(&g_Opl, 0xB0, (BYTE)(0x20 | (4 << 2) | 0x02));
        VddOplRender(&g_Opl, g_Samples, 8192);
        fmLevel = OplSynthTestRms(g_Samples, 8192);
        printf("        plain rms=%ld, modulated rms=%ld\n", plainLevel, fmLevel);
        CHECK(fmLevel != plainLevel, "FM: a live modulator changes the carrier output");
    }

    /* T5: output never leaves int16, even with everything blaring ----------- */
    {
        INT channel, index, clipped = 0;
        memset(&g_Opl, 0, sizeof g_Opl); VddOplReset(&g_Opl);
        for (channel = 0; channel < OPL_CHANNELS; ++channel) OplSynthTestNoteOn(channel, (WORD)(0x180 + channel * 16), 5, 0);
        for (channel = 0; channel < OPL_CHANNELS; ++channel) VddOplWriteRegister(&g_Opl, (BYTE)(0xC0 + channel), 0x0F);
        VddOplRender(&g_Opl, g_Samples, OPL_NATIVE_HZ / 8);
        for (index = 0; index < OPL_NATIVE_HZ / 8; ++index)
            if (g_Samples[index] == 32767 || g_Samples[index] == -32768) clipped++;
        CHECK(OplSynthTestRms(g_Samples, OPL_NATIVE_HZ / 8) > 0, "9 channels at once: still produces signal");
        printf("        %d of %d samples at the clip rail\n", clipped, OPL_NATIVE_HZ / 8);
        CHECK(clipped < OPL_NATIVE_HZ / 80, "9 channels at once: not permanently clipped");
    }

    /* T6: an untouched chip is silent --------------------------------------- */
    memset(&g_Opl, 0, sizeof g_Opl); VddOplReset(&g_Opl);
    VddOplRender(&g_Opl, g_Samples, 4096);
    CHECK(OplSynthTestRms(g_Samples, 4096) == 0, "reset: silent until a note is keyed on");

    /* ══ OPL3 (YMF262), GH #232 ═══════════════════════════════════════════════ */

    /* T7: the OPL2 golden -- nothing the OPL3 added leaks into OPL2 output ---- */
    { UINT32 hash;
      hash = OplSynthTestGoldenRun(0, 0); printf("        OPL2 mono  fnv=0x%08X (golden 0x%08X), melodic prefix 0x%08X (golden 0x%08X)\n",
                                   hash, OPL2_GOLDEN, g_HashMelodic, OPL2_GOLDEN_MELODIC);
      CHECK(g_HashMelodic == OPL2_GOLDEN_MELODIC,
            "golden: everything before rhythm mode bit-identical to the pre-OPL3 build");
      CHECK(hash == OPL2_GOLDEN, "golden: OPL2 mono render bit-identical to the #139 build");
      CHECK(OplSynthTestGoldenRun(0, 1) == OPL2_GOLDEN && OplSynthTestGoldenRun(0, 2) == OPL2_GOLDEN,
            "golden: OPL2 stereo render -- left AND right are that same signal");
      CHECK(OplSynthTestGoldenRun(1, 0) == OPL2_GOLDEN && OplSynthTestGoldenRun(1, 1) == OPL2_GOLDEN &&
            OplSynthTestGoldenRun(1, 2) == OPL2_GOLDEN,
            "golden: an OPL3 with NEW clear is the OPL2, sample for sample"); }

    /* T8: NEW gates array 1 ------------------------------------------------- */
    {   long offLevel, onLevel;
        OplSynthTestOpl3Fresh(0);
        OplSynthTestOperator9(VddOplOperatorIndex(9, 0), 0, 0); OplSynthTestOperator9(VddOplOperatorIndex(9, 1), 1, 0);
        OplSynthTestChannel9(9, 0x31, 0x200, 4);                     /* array 1 ch 0: L+R, additive */
        VddOplRenderStereo(&g_Opl, g_StereoSamples, 4096);
        offLevel = OplSynthTestRmsSide(g_StereoSamples, 4096, 0) + OplSynthTestRmsSide(g_StereoSamples, 4096, 1);
        CHECK(g_Opl.Channels[9].IsKeyOn == 1 && offLevel == 0,
              "NEW clear: an array-1 voice latches (keyed) but is SILENT");
        OplSynthTestWrite9(0x105, 0x01);
        VddOplRenderStereo(&g_Opl, g_StereoSamples, 4096);
        onLevel = OplSynthTestRmsSide(g_StereoSamples, 4096, 0);
        CHECK(onLevel > 10000 && OplSynthTestRmsSide(g_StereoSamples, 4096, 1) == onLevel,
              "NEW set: the latched array-1 voice sounds (both sides, C0=0x31)");
        /* an OPL2 has no array 1 at all: the same writes produce nothing       */
        memset(&g_Opl, 0, sizeof g_Opl); VddOplReset(&g_Opl);
        OplSynthTestOperator9(VddOplOperatorIndex(9, 0), 0, 0); OplSynthTestOperator9(VddOplOperatorIndex(9, 1), 1, 0);
        OplSynthTestChannel9(9, 0x31, 0x200, 4);
        OplSynthTestWrite9(0x105, 0x01);
        VddOplRenderStereo(&g_Opl, g_StereoSamples, 4096);
        CHECK(OplSynthTestRmsSide(g_StereoSamples, 4096, 0) == 0 && !VddOplIsNewMode(&g_Opl),
              "OPL2: array-1 writes (and NEW) do nothing");
    }

    /* T9: stereo routing, C0 bits 4 (A = left) and 5 (B = right) ------------ */
    {   static const struct { BYTE C0; INT Left, Right; PCSTR Message; } routes[] = {
            { 0x11, 1, 0, "stereo: C0 bit 4 (CHA) -> LEFT only" },
            { 0x21, 0, 1, "stereo: C0 bit 5 (CHB) -> RIGHT only" },
            { 0x31, 1, 1, "stereo: bits 4+5 -> both sides, equal" },
            { 0xC1, 0, 0, "stereo: CHC/CHD only (bits 6-7) -> silent: not wired on an SB16" },
            { 0x01, 0, 0, "stereo: NEW set, no routing bits -> silent" } };
        INT item;
        for (item = 0; item < 5; ++item) {
            long leftLevel, rightLevel;
            OplSynthTestOpl3Fresh(1);
            OplSynthTestOperator9(VddOplOperatorIndex(2, 0), 0, 0); OplSynthTestOperator9(VddOplOperatorIndex(2, 1), 1, 0);
            OplSynthTestChannel9(2, routes[item].C0, 0x200, 4);
            VddOplRenderStereo(&g_Opl, g_StereoSamples, 4096);
            leftLevel = OplSynthTestRmsSide(g_StereoSamples, 4096, 0); rightLevel = OplSynthTestRmsSide(g_StereoSamples, 4096, 1);
            CHECK((routes[item].Left ? leftLevel > 10000 : leftLevel == 0) && (routes[item].Right ? rightLevel > 10000 : rightLevel == 0) &&
                  (!(routes[item].Left && routes[item].Right) || leftLevel == rightLevel), routes[item].Message);
        }
        /* NEW clear: routing bits are not looked at -- mono to both, as an OPL2 */
        OplSynthTestOpl3Fresh(0);
        OplSynthTestOperator9(VddOplOperatorIndex(2, 0), 0, 0); OplSynthTestOperator9(VddOplOperatorIndex(2, 1), 1, 0);
        OplSynthTestChannel9(2, 0x11, 0x200, 4);
        VddOplRenderStereo(&g_Opl, g_StereoSamples, 4096);
        CHECK(OplSynthTestRmsSide(g_StereoSamples, 4096, 0) > 10000 &&
              OplSynthTestRmsSide(g_StereoSamples, 4096, 1) == OplSynthTestRmsSide(g_StereoSamples, 4096, 0),
              "stereo: NEW clear ignores C0 bits 4-5 -- mono to both sides");
        /* mono render of a left-only voice: the (L+R)/2 fold                     */
        OplSynthTestOpl3Fresh(1);
        OplSynthTestOperator9(VddOplOperatorIndex(2, 0), 0, 0); OplSynthTestOperator9(VddOplOperatorIndex(2, 1), 1, 0);
        OplSynthTestChannel9(2, 0x11, 0x200, 4);
        VddOplRender(&g_Opl, g_Samples, 4096);
        { long level = OplSynthTestRms(g_Samples, 4096);
          CHECK(level > 10000 / 4 && level < 30000000, "mono render with NEW set: folds (L+R)/2"); }
    }

    /* T10: 4-operator voices -- the carrier set per algorithm --------------- *
     * Pair 0+3. One operator is made live at a time (the others never attack,
     * so they output 0 and modulate nothing): the voice is audible exactly when
     * that operator is one the algorithm SUMS, and silent when it only
     * modulates -- a modulator into a silent operator is silence.             */
    {   /* audible[alg][op]: alg = CNT(ch0)<<1 | CNT(ch3)                         */
        static const INT audible[4][4] = {
            { 0, 0, 0, 1 },         /* 0,0  1->2->3->4                            */
            { 0, 1, 0, 1 },         /* 0,1  (1->2) + (3->4)                       */
            { 1, 0, 0, 1 },         /* 1,0  1 + (2->3->4)                         */
            { 1, 0, 1, 1 } };       /* 1,1  1 + (2->3) + 4                        */
        static PCSTR algorithmNames[4] = { "FM-FM", "FM-AM", "AM-FM", "AM-AM" };
        INT algorithm, item, operators[4];
        operators[0] = VddOplOperatorIndex(0, 0); operators[1] = VddOplOperatorIndex(0, 1);
        operators[2] = VddOplOperatorIndex(3, 0); operators[3] = VddOplOperatorIndex(3, 1);
        for (algorithm = 0; algorithm < 4; ++algorithm) {
            INT isOk = 1;
            CHAR description[96];
            for (item = 0; item < 4; ++item) {
                INT operatorIndex; long leftLevel;
                OplSynthTestOpl3Fresh(1);
                OplSynthTestWrite9(0x104, 0x01);
                for (operatorIndex = 0; operatorIndex < 4; ++operatorIndex) OplSynthTestOperator9(operators[operatorIndex], operatorIndex == item, 0);
                OplSynthTestWrite9(0xC3, (BYTE)(0x30 | (algorithm & 1)));   /* ch3: CNT2 (routing ignored) */
                OplSynthTestChannel9(0, (BYTE)(0x30 | (algorithm >> 1)), 0x200, 4);
                VddOplRenderStereo(&g_Opl, g_StereoSamples, 4096);
                leftLevel = OplSynthTestRmsSide(g_StereoSamples, 4096, 0);
                if (audible[algorithm][item] ? !(leftLevel > 10000) : (leftLevel != 0)) {
                    isOk = 0;
                    printf("        %s: operator %d live -> rms %ld (want %s)\n",
                           algorithmNames[algorithm], item + 1, leftLevel, audible[algorithm][item] ? "sound" : "silence");
                }
            }
            snprintf(description, sizeof description, "4-op %s: heard only through its carrier set", algorithmNames[algorithm]);
            CHECK(isOk, description);
        }
        /* FM really chains: 1 -> 2 -> 3 -> 4 with all live differs from 4 alone */
        {   UINT32 stereoHash;
            OplSynthTestOpl3Fresh(1); OplSynthTestWrite9(0x104, 0x01);
            OplSynthTestOperator9(operators[0], 0, 0); OplSynthTestOperator9(operators[1], 0, 0); OplSynthTestOperator9(operators[2], 0, 0); OplSynthTestOperator9(operators[3], 1, 0);
            OplSynthTestWrite9(0xC3, 0x30); OplSynthTestChannel9(0, 0x30, 0x200, 4);
            VddOplRenderStereo(&g_Opl, g_StereoSamples, 8192);
            stereoHash = OplSynthTestFnv16(2166136261u, g_StereoSamples, 2 * 8192);
            OplSynthTestOpl3Fresh(1); OplSynthTestWrite9(0x104, 0x01);
            OplSynthTestOperator9(operators[0], 1, 0); OplSynthTestOperator9(operators[1], 1, 0); OplSynthTestOperator9(operators[2], 1, 0); OplSynthTestOperator9(operators[3], 1, 0);
            OplSynthTestWrite9(0xC3, 0x30); OplSynthTestChannel9(0, 0x30, 0x200, 4);
            VddOplRenderStereo(&g_Opl, g_StereoSamples, 8192);
            CHECK(OplSynthTestRmsSide(g_StereoSamples, 8192, 0) > 0 && OplSynthTestFnv16(2166136261u, g_StereoSamples, 2 * 8192) != stereoHash,
                  "4-op FM-FM: live modulators 1-3 change operator 4's output");
        }
        /* The pairing needs NEW: same registers, NEW clear -> ch0 is a 2-op voice
           and only its own carrier (operator 2) is heard.                       */
        {   long leftWithNewClear;
            OplSynthTestOpl3Fresh(0); OplSynthTestWrite9(0x104, 0x01);
            OplSynthTestOperator9(operators[0], 0, 0); OplSynthTestOperator9(operators[1], 0, 0); OplSynthTestOperator9(operators[2], 0, 0); OplSynthTestOperator9(operators[3], 1, 0);
            OplSynthTestWrite9(0xC3, 0x30); OplSynthTestChannel9(0, 0x30, 0x200, 4);
            VddOplRenderStereo(&g_Opl, g_StereoSamples, 4096);
            leftWithNewClear = OplSynthTestRmsSide(g_StereoSamples, 4096, 0);
            CHECK(leftWithNewClear == 0, "4-op needs NEW: with NEW clear, ch3's carrier is not keyed by ch0");
        }
        /* the second channel's own key-on does nothing while paired             */
        {   OplSynthTestOpl3Fresh(1); OplSynthTestWrite9(0x104, 0x01);
            OplSynthTestOperator9(operators[2], 0, 0); OplSynthTestOperator9(operators[3], 1, 0);
            OplSynthTestChannel9(3, 0x31, 0x200, 4);
            VddOplRenderStereo(&g_Opl, g_StereoSamples, 4096);
            CHECK(OplSynthTestRmsSide(g_StereoSamples, 4096, 0) == 0, "4-op: keying the SECOND channel sounds nothing");
        }
    }

    /* T11: waveforms 4-7 exist only with NEW set --------------------------- */
    {   UINT32 sineHash, waveHash, waveHashNew, sineHashNew;
        INT waveform, zeroCount, index, flatCount;
        /* reference: a plain sine carrier, NEW clear, then NEW set              */
        OplSynthTestOpl3Fresh(0);
        OplSynthTestOperator9(VddOplOperatorIndex(1, 0), 0, 0); OplSynthTestOperator9(VddOplOperatorIndex(1, 1), 1, 0);
        OplSynthTestChannel9(1, 0x31, 0x200, 4);
        VddOplRenderStereo(&g_Opl, g_StereoSamples, 4096); sineHash = OplSynthTestFnv16(2166136261u, g_StereoSamples, 8192);
        OplSynthTestOpl3Fresh(1);
        OplSynthTestOperator9(VddOplOperatorIndex(1, 0), 0, 0); OplSynthTestOperator9(VddOplOperatorIndex(1, 1), 1, 0);
        OplSynthTestChannel9(1, 0x31, 0x200, 4);
        VddOplRenderStereo(&g_Opl, g_StereoSamples, 4096); sineHashNew = OplSynthTestFnv16(2166136261u, g_StereoSamples, 8192);
        for (waveform = 4; waveform < 8; ++waveform) {
            CHAR description[96];
            OplSynthTestOpl3Fresh(0);
            OplSynthTestOperator9(VddOplOperatorIndex(1, 0), 0, 0); OplSynthTestOperator9(VddOplOperatorIndex(1, 1), 1, (BYTE)waveform);
            OplSynthTestChannel9(1, 0x31, 0x200, 4);
            VddOplRenderStereo(&g_Opl, g_StereoSamples, 4096); waveHash = OplSynthTestFnv16(2166136261u, g_StereoSamples, 8192);
            OplSynthTestOpl3Fresh(1);
            OplSynthTestOperator9(VddOplOperatorIndex(1, 0), 0, 0); OplSynthTestOperator9(VddOplOperatorIndex(1, 1), 1, (BYTE)waveform);
            OplSynthTestChannel9(1, 0x31, 0x200, 4);
            VddOplRenderStereo(&g_Opl, g_StereoSamples, 4096); waveHashNew = OplSynthTestFnv16(2166136261u, g_StereoSamples, 8192);
            snprintf(description, sizeof description, "waveform %d: NEW clear plays wave %d (2 bits), NEW set plays %d",
                     waveform, waveform & 3, waveform);
            /* w & 3 == 0 for 4, so NEW clear == sine; 5-7 fold onto 1-3 (checked below) */
            CHECK((waveform != 4 || waveHash == sineHash) && waveHashNew != sineHashNew && waveHashNew != waveHash, description);
        }
        /* waveform 5 with NEW clear is waveform 1                                */
        OplSynthTestOpl3Fresh(0);
        OplSynthTestOperator9(VddOplOperatorIndex(1, 0), 0, 0); OplSynthTestOperator9(VddOplOperatorIndex(1, 1), 1, 5);
        OplSynthTestChannel9(1, 0x31, 0x200, 4);
        VddOplRenderStereo(&g_Opl, g_StereoSamples, 4096); waveHash = OplSynthTestFnv16(2166136261u, g_StereoSamples, 8192);
        OplSynthTestOpl3Fresh(0);
        OplSynthTestOperator9(VddOplOperatorIndex(1, 0), 0, 0); OplSynthTestOperator9(VddOplOperatorIndex(1, 1), 1, 1);
        OplSynthTestChannel9(1, 0x31, 0x200, 4);
        VddOplRenderStereo(&g_Opl, g_StereoSamples, 4096);
        CHECK(waveHash == OplSynthTestFnv16(2166136261u, g_StereoSamples, 8192), "waveform 5 with NEW clear == waveform 1");
        /* shapes: 4 is silent for half of each cycle; 6 is a square, flat-topped */
        OplSynthTestOpl3Fresh(1);
        OplSynthTestOperator9(VddOplOperatorIndex(1, 0), 0, 0); OplSynthTestOperator9(VddOplOperatorIndex(1, 1), 1, 4);
        OplSynthTestChannel9(1, 0x31, 0x200, 4);                     /* 128 samples per cycle      */
        VddOplRenderStereo(&g_Opl, g_StereoSamples, 4096);
        for (zeroCount = 0, index = 0; index < 4096; ++index) if (g_StereoSamples[2 * index] == 0) zeroCount++;
        printf("        wave 4: %d of 4096 samples silent\n", zeroCount);
        CHECK(zeroCount > 1900 && zeroCount < 2300, "waveform 4: silent through the second half-cycle");
        OplSynthTestOpl3Fresh(1);
        OplSynthTestOperator9(VddOplOperatorIndex(1, 0), 0, 0); OplSynthTestOperator9(VddOplOperatorIndex(1, 1), 1, 6);
        OplSynthTestChannel9(1, 0x31, 0x200, 4);
        VddOplRenderStereo(&g_Opl, g_StereoSamples, 4096);
        /* from sample 8: sample 0 precedes the AR=15 attack's first tick        */
        for (flatCount = 0, index = 8; index < 4096; ++index)
            if (g_StereoSamples[2 * index] == g_StereoSamples[16] || g_StereoSamples[2 * index] == -g_StereoSamples[16]) flatCount++;
        printf("        wave 6: level %d, %d of 4088 samples at +-level\n", g_StereoSamples[16], flatCount);
        CHECK(g_StereoSamples[16] > 3000 && flatCount == 4088, "waveform 6: a square (one magnitude, both signs)");
        /* OPL2: WSE (0x01 bit 5) gates waveform select; the OPL3 has no WSE     */
        memset(&g_Opl, 0, sizeof g_Opl); VddOplReset(&g_Opl);
        OplSynthTestOperator9(VddOplOperatorIndex(1, 0), 0, 0); OplSynthTestOperator9(VddOplOperatorIndex(1, 1), 1, 2);
        OplSynthTestChannel9(1, 0x01, 0x200, 4);
        VddOplRender(&g_Opl, g_Samples, 4096);
        { INT neg = 0; for (index = 0; index < 4096; ++index) if (g_Samples[index] < 0) neg++;
          CHECK(neg > 1500, "OPL2, WSE clear: 0xE0=2 still plays a sine (negative half present)"); }
        memset(&g_Opl, 0, sizeof g_Opl); VddOplReset(&g_Opl);
        OplSynthTestWrite9(0x01, 0x20);
        OplSynthTestOperator9(VddOplOperatorIndex(1, 0), 0, 0); OplSynthTestOperator9(VddOplOperatorIndex(1, 1), 1, 2);
        OplSynthTestChannel9(1, 0x01, 0x200, 4);
        VddOplRender(&g_Opl, g_Samples, 4096);
        { INT neg = 0; for (index = 0; index < 4096; ++index) if (g_Samples[index] < 0) neg++;
          CHECK(neg == 0, "OPL2, WSE set: waveform 2 (|sine|) -- no negative samples"); }
        OplSynthTestOpl3Fresh(0);
        OplSynthTestOperator9(VddOplOperatorIndex(1, 0), 0, 0); OplSynthTestOperator9(VddOplOperatorIndex(1, 1), 1, 2);
        OplSynthTestChannel9(1, 0x01, 0x200, 4);
        VddOplRender(&g_Opl, g_Samples, 4096);
        { INT neg = 0; for (index = 0; index < 4096; ++index) if (g_Samples[index] < 0) neg++;
          CHECK(neg == 0, "OPL3, NEW clear, no WSE: waveform 2 plays (no WSE on a YMF262)"); }
    }

    /* T12: 18 voices, full blast, stereo -- nothing leaves int16 ------------- */
    {   INT channel, index, clipped = 0;
        OplSynthTestOpl3Fresh(1);
        for (channel = 0; channel < OPL3_CHANNELS; ++channel) {
            OplSynthTestOperator9(VddOplOperatorIndex(channel, 0), 1, (BYTE)(channel & 7));
            OplSynthTestOperator9(VddOplOperatorIndex(channel, 1), 1, (BYTE)((channel + 3) & 7));
            OplSynthTestChannel9(channel, (BYTE)(0x3F), (WORD)(0x180 + channel * 16), 5);
        }
        VddOplRenderStereo(&g_Opl, g_StereoSamples, 8192);
        for (index = 0; index < 2 * 8192; ++index) if (g_StereoSamples[index] == 32767 || g_StereoSamples[index] == -32768) clipped++;
        CHECK(OplSynthTestRmsSide(g_StereoSamples, 8192, 0) > 0 && OplSynthTestRmsSide(g_StereoSamples, 8192, 1) > 0,
              "18 channels at once: both sides carry signal");
        printf("        %d of %d stereo samples at the clip rail\n", clipped, 2 * 8192);
    }

    /* ══ RHYTHM MODE: SNARE, HI-HAT, CYMBAL (#139) ══════════════════════════════ */
    OplSynthTestRhythm();

    printf("-- %d checks, %d failures --\n", g_Total, g_Failures);
    return g_Failures ? 1 : 0;
}
