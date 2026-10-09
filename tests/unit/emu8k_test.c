/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * Off-VM battery for the AWE32's EMU8000 (src/vdd/vdd_emu8k.c, #233).
 *
 * Every expectation is from the AWE32/EMU8000 Programmer's Guide rev 1.00 (section n, p.n), written
 * before the model was run, and every access goes through the PORTS exactly as a DOS program
 * makes it -- the pointer at E+802h, then the data port:
 *   T1  detection as the period drivers do it (HWCF1/HWCF2 read-back) and the section 4 procedure
 *   T2  the wall clock WC advancing at the sample rate
 *   T3  register read-back, per channel, word and doubleword, and E+402h's two meanings
 *   T4  sound memory: DMA-stream upload through SMALW/SMLD and SMARW/SMRD, read-back through
 *       SMALR/SMLD with the stale prefetch word, the ROM region, a stream with no channel
 *   T5  a looping channel started by section 6's recipe: pitch from IP, octave up/down, pan
 *   T6  the volume envelope: attack shape, IFATN attenuation, release shape and end
 *   T7  the filter: low cutoff attenuates, Q resonates, Q0/FFh is transparent
 *   T8  output gating: HWCF3's audio enable, a DMA channel is silent
 *   T9  the mixer hook: VddAudioSetEmu8k puts the chip into the host's stereo mix
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#include <stdio.h>
#include <string.h>
#include "vdd_emu8k.h"
#include "vdd_audio.h"

/* ---- the chip's ports and register map (section 2, p.6-7) ---------------------------------- */

#define EMU8K_TEST_BASE                     0x620           /* E: SB 220h + 400h */
#define EMU8K_TEST_DATA0                    (EMU8K_TEST_BASE + 0x000)
#define EMU8K_TEST_DATA0_HIGH               (EMU8K_TEST_BASE + 0x002)
#define EMU8K_TEST_DATA1                    (EMU8K_TEST_BASE + 0x400)
#define EMU8K_TEST_DATA1_HIGH_DATA2         (EMU8K_TEST_BASE + 0x402)
#define EMU8K_TEST_DATA3                    (EMU8K_TEST_BASE + 0x800)
#define EMU8K_TEST_POINTER                  (EMU8K_TEST_BASE + 0x802)
#define EMU8K_TEST_POINTER_REGISTER_SHIFT   5
#define EMU8K_TEST_POINTER_LOW_BYTE         0xFF

/* The bus's access widths and directions (VddBusIo). */
#define EMU8K_TEST_WORD                     2
#define EMU8K_TEST_DOUBLEWORD               4
#define EMU8K_TEST_OUT                      0
#define EMU8K_TEST_IN                       1

/* Register numbers behind each data port. */
#define EMU8K_TEST_CPF                      0               /* Data0 */
#define EMU8K_TEST_PTRX                     1
#define EMU8K_TEST_CVCF                     2
#define EMU8K_TEST_VTFT                     3
#define EMU8K_TEST_PSST                     6
#define EMU8K_TEST_CSL                      7
#define EMU8K_TEST_CCCA                     0               /* Data1 */
#define EMU8K_TEST_GLOBALS                  1               /* Data1/Data2 r1: the registers named by channel */
#define EMU8K_TEST_INIT_LOW                 2               /* Data1 r2 = INIT1, Data2 r2 = INIT2 */
#define EMU8K_TEST_INIT_HIGH                3               /* Data1 r3 = INIT3, Data2 r3 = INIT4 */
#define EMU8K_TEST_ENVVOL                   4
#define EMU8K_TEST_DCYSUSV                  5
#define EMU8K_TEST_ENVVAL                   6
#define EMU8K_TEST_DCYSUS                   7
#define EMU8K_TEST_ATKHLDV                  4               /* Data2 */
#define EMU8K_TEST_LFO1VAL                  5
#define EMU8K_TEST_ATKHLD                   6
#define EMU8K_TEST_LFO2VAL                  7
#define EMU8K_TEST_IP                       0               /* Data3 */
#define EMU8K_TEST_IFATN                    1
#define EMU8K_TEST_PEFE                     2
#define EMU8K_TEST_FMMOD                    3
#define EMU8K_TEST_TREMFRQ                  4
#define EMU8K_TEST_FM2FRQ2                  5

/* The global registers at r1, by channel (p.10-13). */
#define EMU8K_TEST_HWCF4                    9
#define EMU8K_TEST_HWCF5                    10
#define EMU8K_TEST_HWCF6                    13
#define EMU8K_TEST_SMALR                    20
#define EMU8K_TEST_SMARR                    21
#define EMU8K_TEST_SMALW                    22
#define EMU8K_TEST_SMARW                    23
#define EMU8K_TEST_SMLD                     26              /* Data1: SMLD; Data2: SMRD */
#define EMU8K_TEST_WC                       27
#define EMU8K_TEST_HWCF1                    29
#define EMU8K_TEST_HWCF2                    30
#define EMU8K_TEST_HWCF3                    31

/* Register values from the guide. */
#define EMU8K_TEST_HWCF1_INIT               0x0059          /* Section 4 */
#define EMU8K_TEST_HWCF1_PROBE_MASK         0x007E          /* What the period drivers mask the read with */
#define EMU8K_TEST_HWCF1_PROBE_VALUE        0x0058
#define EMU8K_TEST_HWCF2_INIT               0x0020
#define EMU8K_TEST_HWCF2_PROBE_BITS         0x0003
#define EMU8K_TEST_HWCF3_AUDIO_OFF          0x0000
#define EMU8K_TEST_HWCF3_AUDIO_ON           0x0004
#define EMU8K_TEST_HWCF4_INIT               0
#define EMU8K_TEST_HWCF5_INIT               0x00000083u
#define EMU8K_TEST_HWCF6_INIT               0x00008000u
#define EMU8K_TEST_ENGINE_OFF               0x0080          /* DCYSUSV bit 7 */
#define EMU8K_TEST_RELEASE                  0x8000          /* DCYSUSV bit 15 */
#define EMU8K_TEST_NO_DELAY                 0x8000          /* ENVVOL/ENVVAL/LFOnVAL: 8000h = no delay */
#define EMU8K_TEST_FLAT_ENVELOPE            0x7F7F          /* Fastest rate, no hold / full sustain */
#define EMU8K_TEST_LFO_FREQUENCY            0x0010
#define EMU8K_TEST_UNITY_CP                 0x40000000u     /* CP 4000h: one word per sample (p.7) */
#define EMU8K_TEST_FULL_TARGET              0x0000FFFFu     /* VTFT/CVCF: volume 0, cutoff FFFFh */
#define EMU8K_TEST_PITCH_UNITY              0xE000          /* IP E000h = unity */
#define EMU8K_TEST_PITCH_OCTAVE_UP          0xF000
#define EMU8K_TEST_PITCH_OCTAVE_DOWN        0xD000
#define EMU8K_TEST_CP_UNITY                 0x4000
#define EMU8K_TEST_OPEN_FILTER              0xFF00          /* IFATN: cutoff FFh, no attenuation */
#define EMU8K_TEST_RELEASE_5C               0x805C          /* Section 7's release example */
#define EMU8K_TEST_ATTACK_40                0x7F40
#define EMU8K_TEST_ATTENUATION_12DB         0xFF20          /* IFATN 20h: 32 x 0.375 dB */
#define EMU8K_TEST_CUTOFF_LOW               0x0000
#define EMU8K_TEST_CUTOFF_ON_TONE           0x6800
#define EMU8K_TEST_Q_MAX                    0xF
#define EMU8K_TEST_Q_SHIFT_IN_HIGH          12              /* CCCA bits 31-28, in its MS word */
#define EMU8K_TEST_CCCA_HIGH_KEEP           0x0FFF
#define EMU8K_TEST_Q_SHIFT                  28
#define EMU8K_TEST_PAN_SHIFT                24
#define EMU8K_TEST_PAN_LEFT                 0xFF
#define EMU8K_TEST_PAN_RIGHT                0x00
#define EMU8K_TEST_PAN_CENTRE               0x80
#define EMU8K_TEST_INTERPOLATOR_OFFSET      1               /* The registers hold the address minus one */
#define EMU8K_TEST_SMA_FLAG                 0x80000000u     /* EMPTY / FULL */
#define EMU8K_TEST_ADDRESS_MASK             0xFFFFFF

/* The DMA stream modes for CCCA (bits 26-24: DMA, write, right). */
#define EMU8K_TEST_STREAM_LEFT_READ         0x04000000u
#define EMU8K_TEST_STREAM_RIGHT_READ        0x05000000u
#define EMU8K_TEST_STREAM_LEFT_WRITE        0x06000000u
#define EMU8K_TEST_STREAM_RIGHT_WRITE       0x07000000u
#define EMU8K_TEST_CCCA_DMA                 0x04000000u

/* The channels each part uses. */
#define EMU8K_TEST_TONE_CHANNEL             0
#define EMU8K_TEST_SECOND_CHANNEL           1
#define EMU8K_TEST_LAST_CHANNEL             31
#define EMU8K_TEST_LEFT_WRITE_CHANNEL       30
#define EMU8K_TEST_RIGHT_WRITE_CHANNEL      29
#define EMU8K_TEST_LEFT_READ_CHANNEL        28
#define EMU8K_TEST_RIGHT_READ_CHANNEL       27

/* Sound memory. */
#define EMU8K_TEST_DRAM_START               0x200000u
#define EMU8K_TEST_TONE_ADDRESS             0x201000u       /* Where the test tone lives in DRAM */
#define EMU8K_TEST_LOOP_WORDS               64
#define EMU8K_TEST_TONE_WORDS               192             /* Three periods of the loop */
#define EMU8K_TEST_TONE_QUARTER             16
#define EMU8K_TEST_TONE_THREE_QUARTERS      48
#define EMU8K_TEST_TONE_STEP                1024
#define EMU8K_TEST_TONE_HALF_CYCLE          32768
#define EMU8K_TEST_TONE_FULL_CYCLE          65536
#define EMU8K_TEST_TONE_PEAK                16383

/* Timing. */
#define EMU8K_TEST_RATE                     44100           /* One second of samples */
#define EMU8K_TEST_TENTH_SECOND             4410
#define EMU8K_TEST_ENGINE_TICK              64              /* A render that crosses an engine tick */
#define EMU8K_TEST_INIT_WAIT_SAMPLES        1024            /* Section 4 step: "wait 1024 sample periods" */
#define EMU8K_TEST_INIT_WAIT_RENDERS        16              /* ...in renders of 64 */
#define EMU8K_TEST_SPIN_LIMIT               1000
#define EMU8K_TEST_MICROSECONDS_TO_SAMPLES_NUMERATOR 441u
#define EMU8K_TEST_MICROSECONDS_TO_SAMPLES_DENOMINATOR 10000u
#define EMU8K_TEST_TWENTY_MS                882             /* Samples */
#define EMU8K_TEST_FULL_VOLUME              0xFFFF

static INT g_Checks;
static INT g_Failures;
static BYTE   g_GuestMemory[0x10000];
static WORD   g_Dram[EMU8K_DRAM_WORDS];
static VDD_BUS g_Bus;
static EMU8K_STATE g_Emu8k;
static INT16  g_Samples[EMU8K_STEREO_SIDES * EMU8K_TEST_RATE];
static UINT64 g_FakeMicroseconds;

static VOID Emu8kTestCheck(BOOL passed, PCSTR description)
{
    ++g_Checks;

    if (passed)
    {
        printf("  PASS  %s\n", description);
        return;
    }

    printf("  FAIL  %s\n", description);
    ++g_Failures;
}

/* ---- port access, exactly as a DOS program makes it ----------------------------------- */

static VOID Emu8kTestOutWord(WORD port, WORD value)
{
    UINT32 busValue = value;

    VddBusIo(&g_Bus, port, EMU8K_TEST_WORD, EMU8K_TEST_OUT, &busValue);
}

static WORD Emu8kTestInWord(WORD port)
{
    UINT32 busValue = 0;

    VddBusIo(&g_Bus, port, EMU8K_TEST_WORD, EMU8K_TEST_IN, &busValue);
    return (WORD)busValue;
}

static VOID Emu8kTestOutDword(WORD port, DWORD value)
{
    UINT32 busValue = value;

    VddBusIo(&g_Bus, port, EMU8K_TEST_DOUBLEWORD, EMU8K_TEST_OUT, &busValue);
}

static DWORD Emu8kTestInDword(WORD port)
{
    UINT32 busValue = 0;

    VddBusIo(&g_Bus, port, EMU8K_TEST_DOUBLEWORD, EMU8K_TEST_IN, &busValue);
    return busValue;
}

static VOID Emu8kTestSelect(INT registerNumber, INT channel)
{
    Emu8kTestOutWord(EMU8K_TEST_POINTER, (WORD)((registerNumber << EMU8K_TEST_POINTER_REGISTER_SHIFT) | channel));
}

/* a doubleword is the LS word to the port, then the MS word two higher (section 2) */
static VOID Emu8kTestData0Write(INT registerNumber, INT channel, DWORD value)
{
    Emu8kTestSelect(registerNumber, channel);
    Emu8kTestOutWord(EMU8K_TEST_DATA0, (WORD)value);
    Emu8kTestOutWord(EMU8K_TEST_DATA0_HIGH, (WORD)(value >> WORD_SHIFT));
}

static DWORD Emu8kTestData0Read(INT registerNumber, INT channel)
{
    DWORD lowWord;

    Emu8kTestSelect(registerNumber, channel);
    lowWord = Emu8kTestInWord(EMU8K_TEST_DATA0);
    return lowWord | ((DWORD)Emu8kTestInWord(EMU8K_TEST_DATA0_HIGH) << WORD_SHIFT);
}

static VOID Emu8kTestData1WriteDword(INT registerNumber, INT channel, DWORD value)
{
    Emu8kTestSelect(registerNumber, channel);
    Emu8kTestOutWord(EMU8K_TEST_DATA1, (WORD)value);
    Emu8kTestOutWord(EMU8K_TEST_DATA1_HIGH_DATA2, (WORD)(value >> WORD_SHIFT));
}

static DWORD Emu8kTestData1ReadDword(INT registerNumber, INT channel)
{
    DWORD lowWord;

    Emu8kTestSelect(registerNumber, channel);
    lowWord = Emu8kTestInWord(EMU8K_TEST_DATA1);
    return lowWord | ((DWORD)Emu8kTestInWord(EMU8K_TEST_DATA1_HIGH_DATA2) << WORD_SHIFT);
}

static VOID Emu8kTestData1Write(INT registerNumber, INT channel, WORD value)
{
    Emu8kTestSelect(registerNumber, channel);
    Emu8kTestOutWord(EMU8K_TEST_DATA1, value);
}

static WORD Emu8kTestData1Read(INT registerNumber, INT channel)
{
    Emu8kTestSelect(registerNumber, channel);
    return Emu8kTestInWord(EMU8K_TEST_DATA1);
}

static VOID Emu8kTestData2Write(INT registerNumber, INT channel, WORD value)
{
    Emu8kTestSelect(registerNumber, channel);
    Emu8kTestOutWord(EMU8K_TEST_DATA1_HIGH_DATA2, value);
}

static WORD Emu8kTestData2Read(INT registerNumber, INT channel)
{
    Emu8kTestSelect(registerNumber, channel);
    return Emu8kTestInWord(EMU8K_TEST_DATA1_HIGH_DATA2);
}

static VOID Emu8kTestData3Write(INT registerNumber, INT channel, WORD value)
{
    Emu8kTestSelect(registerNumber, channel);
    Emu8kTestOutWord(EMU8K_TEST_DATA3, value);
}

static WORD Emu8kTestData3Read(INT registerNumber, INT channel)
{
    Emu8kTestSelect(registerNumber, channel);
    return Emu8kTestInWord(EMU8K_TEST_DATA3);
}

static WORD Emu8kTestWallClock(VOID)
{
    return Emu8kTestData2Read(EMU8K_TEST_GLOBALS, EMU8K_TEST_WC);
}

static VOID Emu8kTestSetStreamAddress(INT stream, DWORD address)
{
    Emu8kTestData1WriteDword(EMU8K_TEST_GLOBALS, stream, address);
}

static WORD Emu8kTestCurrentVolume(INT channel)
{
    return (WORD)(Emu8kTestData0Read(EMU8K_TEST_CVCF, channel) >> WORD_SHIFT);
}

/* ---- rendering --------------------------------------------------------------------- */

static VOID Emu8kTestRender(DWORD frameCount)
{
    while (frameCount)
    {
        DWORD chunk = frameCount > EMU8K_TEST_RATE ? EMU8K_TEST_RATE : frameCount;
        VddEmu8kRenderStereo(&g_Emu8k, g_Samples, chunk);
        frameCount -= chunk;
    }
}

/* rising zero crossings of (L+R) over `frameCount` frames = the frequency in Hz when
 * frameCount = 44100
 */
static INT Emu8kTestCrossings(DWORD frameCount)
{
    DWORD frame;
    INT crossingCount = 0;
    INT previous = 0;

    VddEmu8kRenderStereo(&g_Emu8k, g_Samples, frameCount);

    for (frame = 0; frame < frameCount; ++frame)
    {
        INT sum = g_Samples[EMU8K_STEREO_SIDES * frame] + g_Samples[EMU8K_STEREO_SIDES * frame + 1];

        if (previous < 0 && sum >= 0)
            crossingCount++;

        previous = sum;
    }

    return crossingCount;
}

static INT Emu8kTestMagnitude(INT16 sample)
{
    return sample < 0 ? -sample : sample;
}

static VOID Emu8kTestPeaks(DWORD frameCount, PINT peakLeft, PINT peakRight)
{
    DWORD frame;
    INT left = 0;
    INT right = 0;

    VddEmu8kRenderStereo(&g_Emu8k, g_Samples, frameCount);

    for (frame = 0; frame < frameCount; ++frame)
    {
        INT magnitudeLeft = Emu8kTestMagnitude(g_Samples[EMU8K_STEREO_SIDES * frame]);
        INT magnitudeRight = Emu8kTestMagnitude(g_Samples[EMU8K_STEREO_SIDES * frame + 1]);

        if (magnitudeLeft > left)
            left = magnitudeLeft;

        if (magnitudeRight > right)
            right = magnitudeRight;
    }

    *peakLeft = left;
    *peakRight = right;
}

static UINT64 Emu8kTestFakeClock(PVOID context)
{
    (VOID)context;
    return g_FakeMicroseconds;
}

/* ---- the guide's recipes ----------------------------------------------------------- */

/* section 5: allocate `channel` to a DMA stream -- the guide's seven steps in its order. */
static VOID Emu8kTestAllocateStream(INT channel, DWORD mode)
{
    Emu8kTestData1Write(EMU8K_TEST_DCYSUSV, channel, EMU8K_TEST_ENGINE_OFF);
    Emu8kTestData0Write(EMU8K_TEST_VTFT, channel, 0);
    Emu8kTestData0Write(EMU8K_TEST_CVCF, channel, 0);
    Emu8kTestData0Write(EMU8K_TEST_PTRX, channel, EMU8K_TEST_UNITY_CP);
    Emu8kTestData0Write(EMU8K_TEST_CPF, channel, EMU8K_TEST_UNITY_CP);
    Emu8kTestData0Write(EMU8K_TEST_PSST, channel, 0);
    Emu8kTestData0Write(EMU8K_TEST_CSL, channel, 0);
    Emu8kTestData1WriteDword(EMU8K_TEST_CCCA, channel, mode);
}

/* section 6: start a sound on `channel`. Addresses are the ACTUAL audio locations; the registers
 * get them minus one (the interpolator offset).
 */
static VOID Emu8kTestNoteOn(
    INT channel,
    DWORD start,
    DWORD loopStart,
    DWORD loopEnd,
    BYTE pan,
    WORD pitch,
    WORD filterAttenuation,
    WORD attackHold,
    WORD decaySustain,
    BYTE resonance)
{
    /* silent and idle */
    Emu8kTestData1Write(EMU8K_TEST_DCYSUSV, channel, EMU8K_TEST_ENGINE_OFF);
    Emu8kTestData0Write(EMU8K_TEST_VTFT, channel, 0);
    Emu8kTestData0Write(EMU8K_TEST_CVCF, channel, 0);
    Emu8kTestData0Write(EMU8K_TEST_PTRX, channel, 0);
    Emu8kTestData0Write(EMU8K_TEST_CPF, channel, 0);
    Emu8kTestData1Write(EMU8K_TEST_ENVVOL, channel, EMU8K_TEST_NO_DELAY);
    Emu8kTestData1Write(EMU8K_TEST_ENVVAL, channel, EMU8K_TEST_NO_DELAY);
    Emu8kTestData1Write(EMU8K_TEST_DCYSUS, channel, EMU8K_TEST_FLAT_ENVELOPE);
    Emu8kTestData2Write(EMU8K_TEST_ATKHLDV, channel, attackHold);
    Emu8kTestData2Write(EMU8K_TEST_LFO1VAL, channel, EMU8K_TEST_NO_DELAY);
    Emu8kTestData2Write(EMU8K_TEST_ATKHLD, channel, EMU8K_TEST_FLAT_ENVELOPE);
    Emu8kTestData2Write(EMU8K_TEST_LFO2VAL, channel, EMU8K_TEST_NO_DELAY);
    Emu8kTestData3Write(EMU8K_TEST_IP, channel, pitch);
    Emu8kTestData3Write(EMU8K_TEST_IFATN, channel, filterAttenuation);
    Emu8kTestData3Write(EMU8K_TEST_PEFE, channel, 0);
    Emu8kTestData3Write(EMU8K_TEST_FMMOD, channel, 0);
    Emu8kTestData3Write(EMU8K_TEST_TREMFRQ, channel, EMU8K_TEST_LFO_FREQUENCY);
    Emu8kTestData3Write(EMU8K_TEST_FM2FRQ2, channel, EMU8K_TEST_LFO_FREQUENCY);
    Emu8kTestData0Write(EMU8K_TEST_PSST, channel,
                        ((DWORD)pan << EMU8K_TEST_PAN_SHIFT) | (loopStart - EMU8K_TEST_INTERPOLATOR_OFFSET));
    Emu8kTestData0Write(EMU8K_TEST_CSL, channel, loopEnd - EMU8K_TEST_INTERPOLATOR_OFFSET);
    Emu8kTestData1WriteDword(EMU8K_TEST_CCCA, channel,
                             ((DWORD)resonance << EMU8K_TEST_Q_SHIFT) | (start - EMU8K_TEST_INTERPOLATOR_OFFSET));
    Emu8kTestData0Write(EMU8K_TEST_VTFT, channel, EMU8K_TEST_FULL_TARGET);
    Emu8kTestData0Write(EMU8K_TEST_CVCF, channel, EMU8K_TEST_FULL_TARGET);
    Emu8kTestData1Write(EMU8K_TEST_DCYSUSV, channel, decaySustain);
    Emu8kTestData0Write(EMU8K_TEST_PTRX, channel, EMU8K_TEST_UNITY_CP);
    Emu8kTestData0Write(EMU8K_TEST_CPF, channel, EMU8K_TEST_UNITY_CP);
}

/* The test tone's loop: a 64-word triangle at TONE_ADDRESS + 64. */
static VOID Emu8kTestPlayTone(INT channel, BYTE pan, WORD filterAttenuation, WORD attackHold)
{
    Emu8kTestNoteOn(channel, EMU8K_TEST_TONE_ADDRESS,
                    EMU8K_TEST_TONE_ADDRESS + EMU8K_TEST_LOOP_WORDS,
                    EMU8K_TEST_TONE_ADDRESS + 2 * EMU8K_TEST_LOOP_WORDS, pan,
                    EMU8K_TEST_PITCH_UNITY, filterAttenuation, attackHold,
                    EMU8K_TEST_FLAT_ENVELOPE, 0);
}

/* section 7: an abrupt end. */
static VOID Emu8kTestNoteKill(INT channel)
{
    Emu8kTestData1Write(EMU8K_TEST_DCYSUSV, channel, EMU8K_TEST_ENGINE_OFF);
    Emu8kTestData0Write(EMU8K_TEST_VTFT, channel, EMU8K_TEST_FULL_TARGET);
    Emu8kTestData0Write(EMU8K_TEST_CVCF, channel, EMU8K_TEST_FULL_TARGET);
}

INT main(VOID)
{
    DWORD wordIndex;
    INT channel;

    printf("== AWE32 EMU8000 battery ==\n");
    memset(&g_Emu8k, 0, sizeof g_Emu8k);
    VddBusInitialize(&g_Bus, g_GuestMemory);
    g_Emu8k.Dram = g_Dram;
    g_Emu8k.DramWords = EMU8K_DRAM_WORDS;
    g_Emu8k.BasePort = EMU8K_TEST_BASE;
    { NTVDD_DEVICE device = VddEmu8kDevice(&g_Emu8k);
      Emu8kTestCheck(VddBusAdd(&g_Bus, &device) == 0, "add: emu8k at 620h/A20h/E20h (three port groups)"); }

    /* ---- T1: detection -- as the period drivers do it: write HWCF1/2/3, read 1 and 2 back ---- */
    Emu8kTestData1Write(EMU8K_TEST_GLOBALS, EMU8K_TEST_HWCF1, EMU8K_TEST_HWCF1_INIT);
    Emu8kTestData1Write(EMU8K_TEST_GLOBALS, EMU8K_TEST_HWCF2, EMU8K_TEST_HWCF2_INIT);
    Emu8kTestData1Write(EMU8K_TEST_GLOBALS, EMU8K_TEST_HWCF3, EMU8K_TEST_HWCF3_AUDIO_OFF);
    Emu8kTestCheck((Emu8kTestData1Read(EMU8K_TEST_GLOBALS, EMU8K_TEST_HWCF1) & EMU8K_TEST_HWCF1_PROBE_MASK)
                   == EMU8K_TEST_HWCF1_PROBE_VALUE, "detect: HWCF1 & 7Eh reads 58h after 59h  <-- THE TEST");
    Emu8kTestCheck((Emu8kTestData1Read(EMU8K_TEST_GLOBALS, EMU8K_TEST_HWCF2) & EMU8K_TEST_HWCF2_PROBE_BITS)
                   == EMU8K_TEST_HWCF2_PROBE_BITS, "detect: HWCF2 & 03h reads 03h after 20h");
    Emu8kTestSelect(EMU8K_TEST_DCYSUSV, 17);
    Emu8kTestCheck((Emu8kTestInWord(EMU8K_TEST_POINTER) & EMU8K_TEST_POINTER_LOW_BYTE)
                   == ((EMU8K_TEST_DCYSUSV << EMU8K_TEST_POINTER_REGISTER_SHIFT) | 17),
                   "pointer: reg/channel read back in the low byte");
    /* section 4, the whole initialisation procedure, as AWEUTIL /S would run it */
    for (channel = 0; channel < EMU8K_VOICES; ++channel)
        Emu8kTestData1Write(EMU8K_TEST_DCYSUSV, channel, EMU8K_TEST_ENGINE_OFF);

    for (channel = 0; channel < EMU8K_VOICES; ++channel)
    {
        Emu8kTestData1Write(EMU8K_TEST_ENVVOL, channel, 0);
        Emu8kTestData1Write(EMU8K_TEST_ENVVAL, channel, 0);
        Emu8kTestData1Write(EMU8K_TEST_DCYSUS, channel, 0);
        Emu8kTestData2Write(EMU8K_TEST_ATKHLDV, channel, 0);
        Emu8kTestData2Write(EMU8K_TEST_LFO1VAL, channel, 0);
        Emu8kTestData2Write(EMU8K_TEST_ATKHLD, channel, 0);
        Emu8kTestData2Write(EMU8K_TEST_LFO2VAL, channel, 0);
        Emu8kTestData3Write(EMU8K_TEST_IP, channel, 0);
        Emu8kTestData3Write(EMU8K_TEST_IFATN, channel, 0);
        Emu8kTestData3Write(EMU8K_TEST_PEFE, channel, 0);
        Emu8kTestData3Write(EMU8K_TEST_FMMOD, channel, 0);
        Emu8kTestData3Write(EMU8K_TEST_TREMFRQ, channel, 0);
        Emu8kTestData3Write(EMU8K_TEST_FM2FRQ2, channel, 0);
        Emu8kTestData0Write(EMU8K_TEST_PTRX, channel, 0);
        Emu8kTestData0Write(EMU8K_TEST_VTFT, channel, 0);
        Emu8kTestData0Write(EMU8K_TEST_PSST, channel, 0);
        Emu8kTestData0Write(EMU8K_TEST_CSL, channel, 0);
        Emu8kTestData1WriteDword(EMU8K_TEST_CCCA, channel, 0);
    }

    for (channel = 0; channel < EMU8K_VOICES; ++channel)
    {
        Emu8kTestData0Write(EMU8K_TEST_CPF, channel, 0);
        Emu8kTestData0Write(EMU8K_TEST_CVCF, channel, 0);
    }

    Emu8kTestSetStreamAddress(EMU8K_TEST_SMALR, 0);
    Emu8kTestSetStreamAddress(EMU8K_TEST_SMARR, 0);
    Emu8kTestSetStreamAddress(EMU8K_TEST_SMALW, 0);
    Emu8kTestSetStreamAddress(EMU8K_TEST_SMARW, 0);
    /* a distinct marker per INIT array and channel, so each array is seen to be its own */
    for (channel = 0; channel < EMU8K_VOICES; ++channel)
    {
        Emu8kTestData1Write(EMU8K_TEST_INIT_LOW, channel, (WORD)(0x1000 + channel));
        Emu8kTestData2Write(EMU8K_TEST_INIT_LOW, channel, (WORD)(0x2000 + channel));
        Emu8kTestData1Write(EMU8K_TEST_INIT_HIGH, channel, (WORD)(0x3000 + channel));
        Emu8kTestData2Write(EMU8K_TEST_INIT_HIGH, channel, (WORD)(0x4000 + channel));
    }

    { WORD startClock = Emu8kTestWallClock();
    INT spins = 0;

      while ((WORD)(Emu8kTestWallClock() - startClock) < EMU8K_TEST_INIT_WAIT_SAMPLES
             && spins < EMU8K_TEST_SPIN_LIMIT)
      {
          Emu8kTestRender(EMU8K_TEST_ENGINE_TICK);
          spins++;
      }

      Emu8kTestCheck(spins == EMU8K_TEST_INIT_WAIT_RENDERS,
                     "§4: a guest waiting 1024 samples on WC gets out, in 1024 samples"); }
    Emu8kTestData1WriteDword(EMU8K_TEST_GLOBALS, EMU8K_TEST_HWCF4, EMU8K_TEST_HWCF4_INIT);
    Emu8kTestData1WriteDword(EMU8K_TEST_GLOBALS, EMU8K_TEST_HWCF5, EMU8K_TEST_HWCF5_INIT);
    Emu8kTestData1WriteDword(EMU8K_TEST_GLOBALS, EMU8K_TEST_HWCF6, EMU8K_TEST_HWCF6_INIT);
    Emu8kTestData1Write(EMU8K_TEST_GLOBALS, EMU8K_TEST_HWCF3, EMU8K_TEST_HWCF3_AUDIO_ON);
    Emu8kTestCheck(Emu8kTestData1Read(EMU8K_TEST_INIT_LOW, 7) == 0x1007
                   && Emu8kTestData2Read(EMU8K_TEST_INIT_LOW, 7) == 0x2007
                   && Emu8kTestData1Read(EMU8K_TEST_INIT_HIGH, 31) == 0x301F
                   && Emu8kTestData2Read(EMU8K_TEST_INIT_HIGH, 0) == 0x4000,
                   "INIT1-4: each of the four arrays holds its own 32 words");
    Emu8kTestCheck(Emu8kTestData1ReadDword(EMU8K_TEST_GLOBALS, EMU8K_TEST_HWCF5) == EMU8K_TEST_HWCF5_INIT
                   && Emu8kTestData1ReadDword(EMU8K_TEST_GLOBALS, EMU8K_TEST_HWCF6) == EMU8K_TEST_HWCF6_INIT,
                   "HWCF5/HWCF6: doublewords read back");

    /* ---- T2: the wall clock ---- */
    { WORD startClock = Emu8kTestWallClock(), endClock;
      Emu8kTestRender(1000);
      endClock = Emu8kTestWallClock();
      Emu8kTestCheck((WORD)(endClock - startClock) == 1000, "WC: 1000 rendered samples advance it by exactly 1000");
      g_Emu8k.Clock = Emu8kTestFakeClock;
      g_FakeMicroseconds = 0;
      startClock = Emu8kTestWallClock();
      g_FakeMicroseconds = 1000000u;
      endClock = Emu8kTestWallClock();
      Emu8kTestCheck((WORD)(endClock - startClock) == EMU8K_TEST_RATE, "WC on a host clock: one second = 44100 counts");
      g_FakeMicroseconds = 1486000u;
      endClock = Emu8kTestWallClock();
      Emu8kTestCheck((WORD)(endClock - startClock) < 100 || (WORD)(endClock - startClock) > 65436,
                     "WC: wraps every 1.486 s (p.13)");
      g_Emu8k.Clock = NULL; }

    /* ---- T3: register read-back ---- */
    Emu8kTestData3Write(EMU8K_TEST_IP, 0, 0xE123);
    Emu8kTestData3Write(EMU8K_TEST_IP, EMU8K_TEST_LAST_CHANNEL, 0xD456);
    Emu8kTestData3Write(EMU8K_TEST_IFATN, 0, 0xAB12);
    Emu8kTestData3Write(EMU8K_TEST_PEFE, 0, 0x1234);
    Emu8kTestData3Write(EMU8K_TEST_FMMOD, 0, 0x5678);
    Emu8kTestData3Write(EMU8K_TEST_TREMFRQ, 0, 0x9ABC);
    Emu8kTestData3Write(EMU8K_TEST_FM2FRQ2, 0, 0xDEF0);
    Emu8kTestData1Write(EMU8K_TEST_ENVVOL, 0, 0x7123);
    Emu8kTestData1Write(EMU8K_TEST_ENVVAL, 0, 0x7456);
    Emu8kTestData2Write(EMU8K_TEST_LFO1VAL, 0, 0x7789);
    Emu8kTestData2Write(EMU8K_TEST_LFO2VAL, 0, 0x7ABC);
    Emu8kTestCheck(Emu8kTestData3Read(EMU8K_TEST_IP, 0) == 0xE123
                   && Emu8kTestData3Read(EMU8K_TEST_IP, EMU8K_TEST_LAST_CHANNEL) == 0xD456,
                   "IP: channel 0 and channel 31 are separate registers");
    Emu8kTestCheck(Emu8kTestData3Read(EMU8K_TEST_IFATN, 0) == 0xAB12 && Emu8kTestData3Read(EMU8K_TEST_PEFE, 0) == 0x1234
                   && Emu8kTestData3Read(EMU8K_TEST_FMMOD, 0) == 0x5678 && Emu8kTestData3Read(EMU8K_TEST_TREMFRQ, 0) == 0x9ABC
                   && Emu8kTestData3Read(EMU8K_TEST_FM2FRQ2, 0) == 0xDEF0,
                   "Data3: IFATN PEFE FMMOD TREMFRQ FM2FRQ2 read back");
    Emu8kTestCheck(Emu8kTestData1Read(EMU8K_TEST_ENVVOL, 0) == 0x7123 && Emu8kTestData1Read(EMU8K_TEST_ENVVAL, 0) == 0x7456
                   && Emu8kTestData2Read(EMU8K_TEST_LFO1VAL, 0) == 0x7789 && Emu8kTestData2Read(EMU8K_TEST_LFO2VAL, 0) == 0x7ABC,
                   "ENVVOL ENVVAL LFO1VAL LFO2VAL read back");
    Emu8kTestData2Write(EMU8K_TEST_ATKHLDV, 0, 0x12FF);
    Emu8kTestData2Write(EMU8K_TEST_ATKHLD, 0, 0x34FF);
    Emu8kTestData1Write(EMU8K_TEST_DCYSUS, 0, 0x56FF);
    Emu8kTestCheck(Emu8kTestData2Read(EMU8K_TEST_ATKHLDV, 0) == 0x127F && Emu8kTestData2Read(EMU8K_TEST_ATKHLD, 0) == 0x347F
                   && Emu8kTestData1Read(EMU8K_TEST_DCYSUS, 0) == 0x567F,
                   "ATKHLDV/ATKHLD/DCYSUS: bit 7 reads as zero (p.15-16)");
    Emu8kTestData0Write(EMU8K_TEST_PSST, 5, 0x80123456u);
    Emu8kTestData0Write(EMU8K_TEST_CSL, 5, 0x40234567u);
    Emu8kTestCheck(Emu8kTestData0Read(EMU8K_TEST_PSST, 5) == 0x80123456u && Emu8kTestData0Read(EMU8K_TEST_CSL, 5) == 0x40234567u,
                   "PSST/CSL: doublewords through two word transfers");
    Emu8kTestSelect(EMU8K_TEST_PSST, 6);
    Emu8kTestOutDword(EMU8K_TEST_DATA0, 0xC0345678u);
    Emu8kTestCheck(Emu8kTestData0Read(EMU8K_TEST_PSST, 6) == 0xC0345678u
                   && (Emu8kTestSelect(EMU8K_TEST_PSST, 6), Emu8kTestInDword(EMU8K_TEST_DATA0)) == 0xC0345678u,
                   "PSST: one 32-bit OUT/IN = the two word transfers");
    Emu8kTestData1WriteDword(EMU8K_TEST_CCCA, 7, 0xF0ABCDEFu);
    Emu8kTestCheck(Emu8kTestData1ReadDword(EMU8K_TEST_CCCA, 7) == 0xF0ABCDEFu, "CCCA: Q, control bits and address read back");
    Emu8kTestData1WriteDword(EMU8K_TEST_CCCA, 7, 0);
    /* E+402h: CCCA's MS word when r0 is selected, the Data2 word register otherwise */
    Emu8kTestSelect(EMU8K_TEST_CCCA, 8);
    Emu8kTestOutWord(EMU8K_TEST_DATA1_HIGH_DATA2, 0x1234);
    Emu8kTestCheck((Emu8kTestData1ReadDword(EMU8K_TEST_CCCA, 8) >> WORD_SHIFT) == 0x1234
                   && Emu8kTestData2Read(EMU8K_TEST_ATKHLDV, 8) == 0,
                   "E+402h with r0 selected is CCCA's MS word, not ATKHLDV");
    Emu8kTestData1WriteDword(EMU8K_TEST_CCCA, 8, 0);

    /* ---- T4: sound memory (section 5) ---- */
    VddEmu8kReset(&g_Emu8k);
    Emu8kTestAllocateStream(EMU8K_TEST_LEFT_WRITE_CHANNEL, EMU8K_TEST_STREAM_LEFT_WRITE);
    Emu8kTestCheck(!(Emu8kTestData1ReadDword(EMU8K_TEST_GLOBALS, EMU8K_TEST_SMALW) & EMU8K_TEST_SMA_FLAG),
                   "SMALW: FULL clear before the address is set");
    Emu8kTestSetStreamAddress(EMU8K_TEST_SMALW, EMU8K_TEST_DRAM_START);

    for (wordIndex = 0; wordIndex < 256; ++wordIndex)
        Emu8kTestData1Write(EMU8K_TEST_GLOBALS, EMU8K_TEST_SMLD, (WORD)(0xA500 + wordIndex));

    Emu8kTestCheck(Emu8kTestData1ReadDword(EMU8K_TEST_GLOBALS, EMU8K_TEST_SMALW) == 0x200100u,
                   "SMALW: advanced one word per SMLD write, FULL clear");
    Emu8kTestCheck(g_Dram[0] == 0xA500 && g_Dram[255] == 0xA5FF, "DRAM: 256 words landed at 200000h");
    Emu8kTestAllocateStream(EMU8K_TEST_RIGHT_WRITE_CHANNEL, EMU8K_TEST_STREAM_RIGHT_WRITE);
    Emu8kTestSetStreamAddress(EMU8K_TEST_SMARW, 0x200200u);

    for (wordIndex = 0; wordIndex < 16; ++wordIndex)
        Emu8kTestData2Write(EMU8K_TEST_GLOBALS, EMU8K_TEST_SMLD, (WORD)(0x5A00 + wordIndex));

    Emu8kTestCheck(g_Dram[0x200] == 0x5A00 && g_Dram[0x20F] == 0x5A0F
                   && Emu8kTestData1ReadDword(EMU8K_TEST_GLOBALS, EMU8K_TEST_SMARW) == 0x200210u,
                   "SMRD/SMARW: the right stream writes too");
    Emu8kTestAllocateStream(EMU8K_TEST_LEFT_READ_CHANNEL, EMU8K_TEST_STREAM_LEFT_READ);
    Emu8kTestSetStreamAddress(EMU8K_TEST_SMALR, EMU8K_TEST_DRAM_START);
    (VOID)Emu8kTestData1Read(EMU8K_TEST_GLOBALS, EMU8K_TEST_SMLD);       /* the stale word (section 5) */
    { BOOL allMatch = TRUE;

      for (wordIndex = 0; wordIndex < 256; ++wordIndex)
          if (Emu8kTestData1Read(EMU8K_TEST_GLOBALS, EMU8K_TEST_SMLD) != (WORD)(0xA500 + wordIndex))
              allMatch = FALSE;

      Emu8kTestCheck(allMatch, "SMALR/SMLD: after one stale read, 256 words read back in order  <-- UPLOAD/READBACK"); }
    Emu8kTestCheck(!(Emu8kTestData1ReadDword(EMU8K_TEST_GLOBALS, EMU8K_TEST_SMALR) & EMU8K_TEST_SMA_FLAG),
                   "SMALR: EMPTY clear while a channel serves the stream");
    { WORD staleWord;
      Emu8kTestSetStreamAddress(EMU8K_TEST_SMALR, 0x200010u);
      (VOID)Emu8kTestData1Read(EMU8K_TEST_GLOBALS, EMU8K_TEST_SMLD);
      Emu8kTestCheck(Emu8kTestData1Read(EMU8K_TEST_GLOBALS, EMU8K_TEST_SMLD) == 0xA510,
                     "SMLD: reading from 200010h");                          /* prefetched 200011h */
      Emu8kTestSetStreamAddress(EMU8K_TEST_SMALR, 0x200005u);
      staleWord = Emu8kTestData1Read(EMU8K_TEST_GLOBALS, EMU8K_TEST_SMLD);
      Emu8kTestCheck(staleWord == 0xA511 && Emu8kTestData1Read(EMU8K_TEST_GLOBALS, EMU8K_TEST_SMLD) == 0xA505,
                     "SMLD read is a PREFETCH: the first word after a new SMALR is the OLD stream's next word"); }
    Emu8kTestAllocateStream(EMU8K_TEST_RIGHT_READ_CHANNEL, EMU8K_TEST_STREAM_RIGHT_READ);
    Emu8kTestSetStreamAddress(EMU8K_TEST_SMARR, 0x200200u);
    (VOID)Emu8kTestData2Read(EMU8K_TEST_GLOBALS, EMU8K_TEST_SMLD);
    Emu8kTestCheck(Emu8kTestData2Read(EMU8K_TEST_GLOBALS, EMU8K_TEST_SMLD) == 0x5A00
                   && Emu8kTestData2Read(EMU8K_TEST_GLOBALS, EMU8K_TEST_SMLD) == 0x5A01,
                   "SMARR/SMRD: the right stream reads too");
    Emu8kTestSetStreamAddress(EMU8K_TEST_SMALR, 0x000100u);
    (VOID)Emu8kTestData1Read(EMU8K_TEST_GLOBALS, EMU8K_TEST_SMLD);
    Emu8kTestCheck(Emu8kTestData1Read(EMU8K_TEST_GLOBALS, EMU8K_TEST_SMLD) == 0,
                   "ROM region (below 200000h): reads zero -- no GM ROM image fitted");
    Emu8kTestSetStreamAddress(EMU8K_TEST_SMALW, 0x000100u);
    Emu8kTestData1Write(EMU8K_TEST_GLOBALS, EMU8K_TEST_SMLD, 0x7777);
    Emu8kTestCheck(g_Emu8k.SoundMemoryRomWrites == 1, "ROM region: a write goes nowhere");
    Emu8kTestSetStreamAddress(EMU8K_TEST_SMALW, 0x23FFFFu);
    Emu8kTestData1Write(EMU8K_TEST_GLOBALS, EMU8K_TEST_SMLD, 0x4242);
    Emu8kTestSetStreamAddress(EMU8K_TEST_SMALW, 0x240000u);
    Emu8kTestData1Write(EMU8K_TEST_GLOBALS, EMU8K_TEST_SMLD, 0x4343);
    Emu8kTestSetStreamAddress(EMU8K_TEST_SMALR, 0x23FFFFu);
    (VOID)Emu8kTestData1Read(EMU8K_TEST_GLOBALS, EMU8K_TEST_SMLD);
    Emu8kTestCheck(Emu8kTestData1Read(EMU8K_TEST_GLOBALS, EMU8K_TEST_SMLD) == 0x4242
                   && Emu8kTestData1Read(EMU8K_TEST_GLOBALS, EMU8K_TEST_SMLD) == 0,
                   "DRAM: the last of 512 KB is there, the word past it reads zero");

    for (channel = EMU8K_TEST_RIGHT_READ_CHANNEL; channel <= EMU8K_TEST_LEFT_WRITE_CHANNEL; ++channel)
        Emu8kTestData1WriteDword(EMU8K_TEST_CCCA, channel, 0);      /* deallocate every stream */

    Emu8kTestSetStreamAddress(EMU8K_TEST_SMALW, 0x200400u);
    Emu8kTestData1Write(EMU8K_TEST_GLOBALS, EMU8K_TEST_SMLD, 0x1234);
    Emu8kTestCheck((Emu8kTestData1ReadDword(EMU8K_TEST_GLOBALS, EMU8K_TEST_SMALW) & EMU8K_TEST_SMA_FLAG)
                   && g_Dram[0x400] != 0x1234, "no channel on the stream: the word waits, FULL set");
    Emu8kTestAllocateStream(EMU8K_TEST_LEFT_WRITE_CHANNEL, EMU8K_TEST_STREAM_LEFT_WRITE);
    Emu8kTestCheck(g_Dram[0x400] == 0x1234
                   && !(Emu8kTestData1ReadDword(EMU8K_TEST_GLOBALS, EMU8K_TEST_SMALW) & EMU8K_TEST_SMA_FLAG),
                   "allocating a channel completes it, FULL clears");
    Emu8kTestData1WriteDword(EMU8K_TEST_CCCA, EMU8K_TEST_LEFT_WRITE_CHANNEL, 0);

    /* ---- T5: a looping channel: pitch and pan ---- */
    /* Three periods of a 64-word triangle, +/-16384, so the loop seam is continuous. */
    Emu8kTestSetStreamAddress(EMU8K_TEST_SMALW, EMU8K_TEST_TONE_ADDRESS);
    Emu8kTestAllocateStream(EMU8K_TEST_LEFT_WRITE_CHANNEL, EMU8K_TEST_STREAM_LEFT_WRITE);

    for (wordIndex = 0; wordIndex < EMU8K_TEST_TONE_WORDS; ++wordIndex)
    {
        DWORD phase = wordIndex & (EMU8K_TEST_LOOP_WORDS - 1);
        INT32 level = phase < EMU8K_TEST_TONE_QUARTER ? (INT32)phase * EMU8K_TEST_TONE_STEP
                    : phase < EMU8K_TEST_TONE_THREE_QUARTERS ? EMU8K_TEST_TONE_HALF_CYCLE - (INT32)phase * EMU8K_TEST_TONE_STEP
                    : (INT32)phase * EMU8K_TEST_TONE_STEP - EMU8K_TEST_TONE_FULL_CYCLE;
        Emu8kTestData1Write(EMU8K_TEST_GLOBALS, EMU8K_TEST_SMLD,
                            (WORD)(INT16)(level > EMU8K_TEST_TONE_PEAK ? EMU8K_TEST_TONE_PEAK
                                          : level < -EMU8K_TEST_TONE_PEAK ? -EMU8K_TEST_TONE_PEAK : level));
    }

    Emu8kTestData1WriteDword(EMU8K_TEST_CCCA, EMU8K_TEST_LEFT_WRITE_CHANNEL, 0);
    Emu8kTestCheck(g_Dram[EMU8K_TEST_TONE_ADDRESS - EMU8K_DRAM_BASE + EMU8K_TEST_TONE_QUARTER] == EMU8K_TEST_TONE_PEAK
                   && g_Dram[EMU8K_TEST_TONE_ADDRESS - EMU8K_DRAM_BASE + EMU8K_TEST_LOOP_WORDS] == 0, "tone uploaded");
    Emu8kTestPlayTone(EMU8K_TEST_TONE_CHANNEL, EMU8K_TEST_PAN_CENTRE, EMU8K_TEST_OPEN_FILTER, EMU8K_TEST_FLAT_ENVELOPE);
    Emu8kTestRender(EMU8K_TEST_TENTH_SECOND);
    Emu8kTestCheck((Emu8kTestData0Read(EMU8K_TEST_CPF, EMU8K_TEST_TONE_CHANNEL) >> WORD_SHIFT) == EMU8K_TEST_CP_UNITY
                   && (Emu8kTestData0Read(EMU8K_TEST_PTRX, EMU8K_TEST_TONE_CHANNEL) >> WORD_SHIFT) == EMU8K_TEST_CP_UNITY,
                   "IP E000h: pitch target and current pitch = 4000h (unity)");
    { DWORD currentAddress = Emu8kTestData1ReadDword(EMU8K_TEST_CCCA, EMU8K_TEST_TONE_CHANNEL) & EMU8K_TEST_ADDRESS_MASK;
      Emu8kTestCheck(currentAddress >= EMU8K_TEST_TONE_ADDRESS + EMU8K_TEST_LOOP_WORDS - EMU8K_TEST_INTERPOLATOR_OFFSET
                     && currentAddress < EMU8K_TEST_TONE_ADDRESS + 2 * EMU8K_TEST_LOOP_WORDS - EMU8K_TEST_INTERPOLATOR_OFFSET,
                     "CCCA: the current address stays inside the loop"); }
    { INT frequency = Emu8kTestCrossings(EMU8K_TEST_RATE);
    CHAR message[96];
      sprintf(message, "unity pitch: a 64-word loop plays at 44100/64 = 689 Hz (got %d)", frequency);
      Emu8kTestCheck(frequency >= 687 && frequency <= 691, message); }
    Emu8kTestData3Write(EMU8K_TEST_IP, EMU8K_TEST_TONE_CHANNEL, EMU8K_TEST_PITCH_OCTAVE_UP);
    { INT frequency;
    CHAR message[96];
      Emu8kTestRender(EMU8K_TEST_ENGINE_TICK);
      frequency = Emu8kTestCrossings(EMU8K_TEST_RATE);
      sprintf(message, "IP F000h (+1 octave): 1378 Hz (got %d)", frequency);
      Emu8kTestCheck(frequency >= 1375 && frequency <= 1381, message); }
    Emu8kTestData3Write(EMU8K_TEST_IP, EMU8K_TEST_TONE_CHANNEL, EMU8K_TEST_PITCH_OCTAVE_DOWN);
    { INT frequency;
    CHAR message[96];
      Emu8kTestRender(EMU8K_TEST_ENGINE_TICK);
      frequency = Emu8kTestCrossings(EMU8K_TEST_RATE);
      sprintf(message, "IP D000h (-1 octave): 345 Hz (got %d)", frequency);
      Emu8kTestCheck(frequency >= 343 && frequency <= 346, message); }
    Emu8kTestData3Write(EMU8K_TEST_IP, EMU8K_TEST_TONE_CHANNEL, EMU8K_TEST_PITCH_UNITY);
    { INT leftOfLeft, rightOfLeft, leftOfRight, rightOfRight, leftOfCentre, rightOfCentre;
      Emu8kTestPlayTone(EMU8K_TEST_TONE_CHANNEL, EMU8K_TEST_PAN_LEFT, EMU8K_TEST_OPEN_FILTER, EMU8K_TEST_FLAT_ENVELOPE);
      Emu8kTestRender(EMU8K_TEST_TENTH_SECOND);
      Emu8kTestPeaks(EMU8K_TEST_TENTH_SECOND, &leftOfLeft, &rightOfLeft);
      Emu8kTestPlayTone(EMU8K_TEST_TONE_CHANNEL, EMU8K_TEST_PAN_RIGHT, EMU8K_TEST_OPEN_FILTER, EMU8K_TEST_FLAT_ENVELOPE);
      Emu8kTestRender(EMU8K_TEST_TENTH_SECOND);
      Emu8kTestPeaks(EMU8K_TEST_TENTH_SECOND, &leftOfRight, &rightOfRight);
      Emu8kTestPlayTone(EMU8K_TEST_TONE_CHANNEL, EMU8K_TEST_PAN_CENTRE, EMU8K_TEST_OPEN_FILTER, EMU8K_TEST_FLAT_ENVELOPE);
      Emu8kTestRender(EMU8K_TEST_TENTH_SECOND);
      Emu8kTestPeaks(EMU8K_TEST_TENTH_SECOND, &leftOfCentre, &rightOfCentre);
      printf("        pan FFh: L=%d R=%d   pan 00h: L=%d R=%d   pan 80h: L=%d R=%d\n",
             leftOfLeft, rightOfLeft, leftOfRight, rightOfRight, leftOfCentre, rightOfCentre);
      Emu8kTestCheck(leftOfLeft > 15000 && rightOfLeft == 0, "pan FFh: extreme LEFT (p.9) -- the right channel is silent");
      Emu8kTestCheck(rightOfRight > 15000 && leftOfRight == 0, "pan 00h: extreme RIGHT -- the left channel is silent");
      Emu8kTestCheck(leftOfCentre > leftOfLeft * 45 / 100 && leftOfCentre < leftOfLeft * 55 / 100
                     && rightOfCentre > leftOfLeft * 45 / 100 && rightOfCentre < leftOfLeft * 55 / 100,
                     "pan 80h: the middle of a linear crossfade -- half on each side"); }

    /* ---- T6: the volume envelope ---- */
    { DWORD attackSlowest = VddEmu8kAttackMicroseconds(1), attackFastest = VddEmu8kAttackMicroseconds(0x7F);
      DWORD decaySlowest = VddEmu8kDecayMicrosecondsPerDb(1);
      DWORD decayFastest = VddEmu8kDecayMicrosecondsPerDb(0x7F);
      printf("        attack 01h=%u us 7Fh=%u us   decay 01h=%u us/dB 7Fh=%u us/dB\n",
             attackSlowest, attackFastest, decaySlowest, decayFastest);
      Emu8kTestCheck(attackSlowest > 11760000 && attackSlowest < 12000000 && attackFastest > 5900 && attackFastest < 6100,
                     "attack: 01h = 11.88 s, 7Fh = 6 ms (p.16)");
      Emu8kTestCheck(decaySlowest > 465000 && decaySlowest < 475000 && decayFastest > 235 && decayFastest < 245,
                     "decay: 01h = 470 ms/dB, 7Fh = 240 us/dB (p.15)"); }
    { DWORD attackSamples = (DWORD)(((UINT64)VddEmu8kAttackMicroseconds(0x40) * EMU8K_TEST_MICROSECONDS_TO_SAMPLES_NUMERATOR)
                                    / EMU8K_TEST_MICROSECONDS_TO_SAMPLES_DENOMINATOR);
      WORD atQuarter;
      WORD atHalf;
      WORD atEnd;
      Emu8kTestPlayTone(EMU8K_TEST_TONE_CHANNEL, EMU8K_TEST_PAN_CENTRE, EMU8K_TEST_OPEN_FILTER, EMU8K_TEST_ATTACK_40);
      Emu8kTestRender(attackSamples / 4);
      atQuarter = Emu8kTestCurrentVolume(EMU8K_TEST_TONE_CHANNEL);
      Emu8kTestRender(attackSamples / 4);
      atHalf = Emu8kTestCurrentVolume(EMU8K_TEST_TONE_CHANNEL);
      Emu8kTestRender(attackSamples / 2 + EMU8K_TEST_ENGINE_TICK);
      atEnd = Emu8kTestCurrentVolume(EMU8K_TEST_TONE_CHANNEL);
      printf("        attack 40h (%u samples): CV at T/4=%u T/2=%u T=%u\n", attackSamples, atQuarter, atHalf, atEnd);
      Emu8kTestCheck(atQuarter > EMU8K_TEST_FULL_VOLUME * 20 / 100 && atQuarter < EMU8K_TEST_FULL_VOLUME * 30 / 100
                     && atHalf > EMU8K_TEST_FULL_VOLUME * 45 / 100 && atHalf < EMU8K_TEST_FULL_VOLUME * 55 / 100,
                     "attack: LINEAR in amplitude -- a quarter at T/4, a half at T/2");
      Emu8kTestCheck(atEnd >= 0xFFF0, "attack: full volume at T"); }
    { WORD atStart, after20Ms, after40Ms;
    CHAR message[128];
      Emu8kTestData1Write(EMU8K_TEST_DCYSUSV, EMU8K_TEST_TONE_CHANNEL, EMU8K_TEST_RELEASE_5C);
      Emu8kTestRender(EMU8K_TEST_ENGINE_TICK / 2);
      atStart = Emu8kTestCurrentVolume(EMU8K_TEST_TONE_CHANNEL);
      Emu8kTestRender(EMU8K_TEST_TWENTY_MS);
      after20Ms = Emu8kTestCurrentVolume(EMU8K_TEST_TONE_CHANNEL);
      Emu8kTestRender(EMU8K_TEST_TWENTY_MS);
      after40Ms = Emu8kTestCurrentVolume(EMU8K_TEST_TONE_CHANNEL);
      sprintf(message, "release 5Ch: dB-linear -- the 2nd 20 ms falls by the same ratio as the 1st (%u %u %u)",
              atStart, after20Ms, after40Ms);
      Emu8kTestCheck(after20Ms < atStart && after40Ms < after20Ms && atStart > 60000
                     && (double)after40Ms / after20Ms > 0.85 * (double)after20Ms / atStart
                     && (double)after40Ms / after20Ms < 1.15 * (double)after20Ms / atStart, message);
      Emu8kTestCheck((double)after20Ms / atStart > 0.25 && (double)after20Ms / atStart < 0.37,
                     "release 5Ch: ~10 dB per 20 ms (1.97 ms/dB)");
      Emu8kTestRender(EMU8K_TEST_RATE / 4);
      Emu8kTestCheck(Emu8kTestCurrentVolume(EMU8K_TEST_TONE_CHANNEL) == 0
                     && (Emu8kTestData0Read(EMU8K_TEST_VTFT, EMU8K_TEST_TONE_CHANNEL) >> WORD_SHIFT) == 0,
                     "release: silent (CV = VT = 0) within 250 ms"); }
    Emu8kTestPlayTone(EMU8K_TEST_TONE_CHANNEL, EMU8K_TEST_PAN_CENTRE, EMU8K_TEST_ATTENUATION_12DB, EMU8K_TEST_FLAT_ENVELOPE);
    Emu8kTestRender(EMU8K_TEST_TENTH_SECOND);
    { WORD currentVolume = Emu8kTestCurrentVolume(EMU8K_TEST_TONE_CHANNEL);
    CHAR message[96];
      sprintf(message, "IFATN 20h: 32 x 0.375 = 12 dB -> CV = FFFFh x 0.251 (got %u)", currentVolume);
      Emu8kTestCheck(currentVolume > 16100 && currentVolume < 16800, message); }
    Emu8kTestNoteKill(EMU8K_TEST_TONE_CHANNEL);
    Emu8kTestCheck(Emu8kTestCurrentVolume(EMU8K_TEST_TONE_CHANNEL) == 0
                   && (Emu8kTestData0Read(EMU8K_TEST_VTFT, EMU8K_TEST_TONE_CHANNEL) >> WORD_SHIFT) == 0,
                   "§7 abrupt end: engine off, VT and CV zero at once");

    /* ---- T7: the filter ---- */
    { INT peakOpen, peakLowCutoff, peakOnTone, peakResonant, unusedRight;
      Emu8kTestPlayTone(EMU8K_TEST_TONE_CHANNEL, EMU8K_TEST_PAN_CENTRE, EMU8K_TEST_OPEN_FILTER, EMU8K_TEST_FLAT_ENVELOPE);
      Emu8kTestRender(EMU8K_TEST_TENTH_SECOND);
      Emu8kTestPeaks(EMU8K_TEST_TENTH_SECOND, &peakOpen, &unusedRight);
      Emu8kTestData3Write(EMU8K_TEST_IFATN, EMU8K_TEST_TONE_CHANNEL, EMU8K_TEST_CUTOFF_LOW);
      Emu8kTestRender(EMU8K_TEST_TENTH_SECOND);
      Emu8kTestPeaks(EMU8K_TEST_TENTH_SECOND, &peakLowCutoff, &unusedRight);
      Emu8kTestData3Write(EMU8K_TEST_IFATN, EMU8K_TEST_TONE_CHANNEL, EMU8K_TEST_CUTOFF_ON_TONE);
      Emu8kTestRender(EMU8K_TEST_TENTH_SECOND);
      Emu8kTestPeaks(EMU8K_TEST_TENTH_SECOND, &peakOnTone, &unusedRight);
      { DWORD ccca = Emu8kTestData1ReadDword(EMU8K_TEST_CCCA, EMU8K_TEST_TONE_CHANNEL);
        Emu8kTestSelect(EMU8K_TEST_CCCA, EMU8K_TEST_TONE_CHANNEL);
        Emu8kTestOutWord(EMU8K_TEST_DATA1_HIGH_DATA2,
                         (WORD)((EMU8K_TEST_Q_MAX << EMU8K_TEST_Q_SHIFT_IN_HIGH)
                                | ((ccca >> WORD_SHIFT) & EMU8K_TEST_CCCA_HIGH_KEEP))); }
      Emu8kTestRender(2 * EMU8K_TEST_TENTH_SECOND);
      Emu8kTestPeaks(EMU8K_TEST_TENTH_SECOND, &peakResonant, &unusedRight);
      printf("        peak: open %d   cutoff 00h %d   cutoff 68h Q0 %d   Q15 %d\n",
             peakOpen, peakLowCutoff, peakOnTone, peakResonant);
      /* the negative peak: -16383 x FFFFh >> 16 = -16383 (the shift floors), x pan 80h's
       * 128/256 = -8192. Bit-exact, or something filtered it.
       */
      Emu8kTestCheck(peakOpen == 8192, "Q 0 + cutoff FFh: the filter does not alter the signal (p.17) -- bit-exact peak");
      Emu8kTestCheck(peakLowCutoff < peakOpen * 15 / 100, "cutoff 00h (125 Hz): a 689 Hz tone is cut to under 15%");
      Emu8kTestCheck(peakResonant > peakOnTone * 3, "Q 15 at a cutoff on the tone: resonance lifts it more than 3x");
      Emu8kTestCheck(Emu8kTestData0Read(EMU8K_TEST_CVCF, EMU8K_TEST_TONE_CHANNEL) != 0
                     && (Emu8kTestData0Read(EMU8K_TEST_CVCF, EMU8K_TEST_TONE_CHANNEL) & WORD_MASK)
                        == EMU8K_TEST_CUTOFF_ON_TONE,
                     "CVCF: the current cutoff follows IFATN's byte (6800h)"); }
    Emu8kTestNoteKill(EMU8K_TEST_TONE_CHANNEL);

    /* ---- T8: output gating ---- */
    { INT peakLeft, peakRight;
      Emu8kTestPlayTone(EMU8K_TEST_TONE_CHANNEL, EMU8K_TEST_PAN_CENTRE, EMU8K_TEST_OPEN_FILTER, EMU8K_TEST_FLAT_ENVELOPE);
      Emu8kTestRender(EMU8K_TEST_TENTH_SECOND);
      Emu8kTestData1Write(EMU8K_TEST_GLOBALS, EMU8K_TEST_HWCF3, EMU8K_TEST_HWCF3_AUDIO_OFF);
      Emu8kTestPeaks(2000, &peakLeft, &peakRight);
      Emu8kTestCheck(peakLeft == 0 && peakRight == 0, "HWCF3 = 0: audio output disabled (§4)");
      Emu8kTestData1Write(EMU8K_TEST_GLOBALS, EMU8K_TEST_HWCF3, EMU8K_TEST_HWCF3_AUDIO_ON);
      Emu8kTestPeaks(2000, &peakLeft, &peakRight);
      Emu8kTestCheck(peakLeft > 0, "HWCF3 = 4: audio back");
      Emu8kTestNoteKill(EMU8K_TEST_TONE_CHANNEL);
      Emu8kTestPlayTone(EMU8K_TEST_SECOND_CHANNEL, EMU8K_TEST_PAN_CENTRE, EMU8K_TEST_OPEN_FILTER, EMU8K_TEST_FLAT_ENVELOPE);
      Emu8kTestRender(EMU8K_TEST_TENTH_SECOND);
      { DWORD ccca = Emu8kTestData1ReadDword(EMU8K_TEST_CCCA, EMU8K_TEST_SECOND_CHANNEL);
        Emu8kTestData1WriteDword(EMU8K_TEST_CCCA, EMU8K_TEST_SECOND_CHANNEL, ccca | EMU8K_TEST_CCCA_DMA); }
      Emu8kTestPeaks(2000, &peakLeft, &peakRight);
      Emu8kTestCheck(peakLeft == 0 && peakRight == 0, "a channel in DMA mode makes no sound");
      Emu8kTestData1WriteDword(EMU8K_TEST_CCCA, EMU8K_TEST_SECOND_CHANNEL, 0);
      Emu8kTestNoteKill(EMU8K_TEST_SECOND_CHANNEL); }

    /* ---- T9: the mixer hook ---- */
    { static AUDIO_STATE audio;
    static INT16 mixed[EMU8K_STEREO_SIDES * 1024];
      BOOL hasLeft = FALSE;
      BOOL hasRight = FALSE;
      DWORD frame;
      VddAudioInitialize(&audio, NULL, NULL, EMU8K_TEST_RATE);
      VddAudioSetEmu8k(&audio, &g_Emu8k);
      Emu8kTestPlayTone(EMU8K_TEST_TONE_CHANNEL, EMU8K_TEST_PAN_LEFT, EMU8K_TEST_OPEN_FILTER, EMU8K_TEST_FLAT_ENVELOPE);
      VddAudioMixStereo(&audio, mixed, 1024);

      for (frame = 0; frame < 1024; ++frame)
      {
          if (mixed[EMU8K_STEREO_SIDES * frame])
              hasLeft = TRUE;

          if (mixed[EMU8K_STEREO_SIDES * frame + 1])
              hasRight = TRUE;
      }

      Emu8kTestCheck(hasLeft && !hasRight,
                     "mixer: vdd_audio_set_emu8k -- a hard-left voice reaches the host's LEFT channel only");
      VddAudioSetEmu8k(&audio, NULL);
      Emu8kTestNoteKill(EMU8K_TEST_TONE_CHANNEL); }

    printf("\n%d checks, %d failed\n", g_Checks, g_Failures);
    return g_Failures ? 1 : 0;
}
