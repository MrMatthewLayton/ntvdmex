/* vdd_emu8k.c -- the EMU8000 (Sound Blaster AWE32 wavetable). See vdd_emu8k.h and
 * docs/inventory/emu8k.md.
 *
 * Built from the AWE32/EMU8000 Programmer's Guide rev 1.00 (§n, p.n below), in our own words:
 * a device is in because it is in the period hardware contract, not because a guest asked.
 * Where the guide gives only the ENDS of a scale (attack 11.88 s .. 6 ms, decay 470 ms/dB ..
 * 240 us/dB) the curve between them is OURS -- logarithmic interpolation -- and says so.
 *
 * No C library (the host links -nostdlib): every exponential below comes from one 257-entry
 * table of 2^(i/256), built once by repeated multiplication.
 */
#include "vdd_emu8k.h"

/* ---- fixed-point formats --------------------------------------------------------------- */

#define EMU8K_Q16_SHIFT          16          /* Q16: 16 fraction bits                          */
#define EMU8K_Q16_ONE            65536       /* 1.0 in Q16                                     */
#define EMU8K_Q16_ONE_UNSIGNED   65536u
#define EMU8K_Q16_ONE_DOUBLE     65536.0
#define EMU8K_Q16_FRACTION_MASK  0xFFFFu
#define EMU8K_Q28_SHIFT          28          /* the filter's coefficients are Q28              */
#define EMU8K_Q28_ONE_DOUBLE     268435456.0
#define EMU8K_HIGH_BYTE_SHIFT    8
#define EMU8K_HIGH_WORD_SHIFT    16
#define EMU8K_LOW_WORD_MASK_KEEP_HIGH 0x0000FFFFu
#define EMU8K_WORD_MAX           0xFFFF

/* ---- fixed-point exponentials --------------------------------------------------------- */

#define EMU8K_EXP2_STEPS         256         /* table entries per octave                       */
#define EMU8K_EXP2_TABLE_START   1.0         /* 2^0                                            */
#define EMU8K_EXP2_TABLE_TOP     131072u     /* 2^1 in Q16: the entry after the last step      */
#define EMU8K_EXP2_STEP_RATIO    1.0027112750502025  /* 2^(1/256)                           */
#define EMU8K_EXP2_ROUND_HALF    0.5
#define EMU8K_EXP2_MAX_OCTAVE    15          /* 2^15 x Q16 is the largest DWORD result         */
#define EMU8K_EXP2_SATURATED     0xFFFFFFFFu
#define EMU8K_EXP2_MIN_OCTAVE    (-32)       /* below: the result underflows to zero           */
#define EMU8K_LOG2_OF_ZERO_OCTAVES 32        /* log2(0) answered as -32 octaves                */
#define EMU8K_RATE_CODES         128         /* a 7-bit rate code: 00h..7Fh                    */

static DWORD g_Emu8kExp2Table[EMU8K_EXP2_STEPS + 1];   /* 2^(i/256), Q16: 65536 .. 131072      */
static DWORD g_Emu8kAttackStep[EMU8K_RATE_CODES];     /* attack: Q16 of full level per tick  */
static INT32 g_Emu8kDecayStep[EMU8K_RATE_CODES];      /* decay/release: Q16 dB per tick      */
static INT   g_Emu8kTablesBuilt;

/* 2^(exponent / 65536), Q16. Good to about 1 part in 10^5 -- far below anything audible. */
static DWORD Emu8kExp2(INT32 exponent)
{
    INT32  octave = exponent >> EMU8K_Q16_SHIFT;           /* arithmetic: floor           */
    DWORD  fraction = (DWORD)exponent & EMU8K_Q16_FRACTION_MASK,
           tableIndex = fraction >> EMU8K_HIGH_BYTE_SHIFT, mantissa;
    mantissa = g_Emu8kExp2Table[tableIndex]
             + (DWORD)(((UINT64)(g_Emu8kExp2Table[tableIndex + 1] - g_Emu8kExp2Table[tableIndex])
                        * (fraction & BYTE_MASK)) >> EMU8K_HIGH_BYTE_SHIFT);
    if (octave >= EMU8K_EXP2_MAX_OCTAVE) return EMU8K_EXP2_SATURATED;
    if (octave >= 0)  return mantissa << octave;
    if (octave <= EMU8K_EXP2_MIN_OCTAVE) return 0;
    return mantissa >> -octave;
}

/* log2(value / 65536), Q16, for value > 0: the inverse of the above, by search of the same
   table. */
static INT32 Emu8kLog2(DWORD value)
{
    INT32 octave = 0; DWORD lowIndex = 0, highIndex = EMU8K_EXP2_STEPS, tableIndex;
    if (!value) return -(EMU8K_LOG2_OF_ZERO_OCTAVES << EMU8K_Q16_SHIFT);
    while (value >= EMU8K_EXP2_TABLE_TOP) { value >>= 1; ++octave; }
    while (value <  EMU8K_Q16_ONE_UNSIGNED)  { value <<= 1; --octave; }
    while (highIndex - lowIndex > 1) {
        tableIndex = (lowIndex + highIndex) >> 1;
        if (g_Emu8kExp2Table[tableIndex] <= value) lowIndex = tableIndex; else highIndex = tableIndex;
    }
    tableIndex = lowIndex;
    return octave * EMU8K_Q16_ONE + (INT32)(tableIndex << EMU8K_HIGH_BYTE_SHIFT)
         + (INT32)(((UINT64)(value - g_Emu8kExp2Table[tableIndex]) << EMU8K_HIGH_BYTE_SHIFT)
                   / (g_Emu8kExp2Table[tableIndex + 1] - g_Emu8kExp2Table[tableIndex]));
}

/* Q16 dB of attenuation -> Q16 linear gain. 20·log10(2) = 6.0206 dB per octave, so
   octaves = dB × 0.166096 = dB × 10885 / 65536. */
#define EMU8K_OCTAVES_PER_DB_Q16 10885       /* 0.166096 in Q16                                */
#define EMU8K_SILENCE_DB         100u        /* 100 dB down: treated as silence                */

static DWORD Emu8kDbToGain(INT32 attenuationQ16)
{
    if (attenuationQ16 <= 0) return EMU8K_Q16_ONE_UNSIGNED;
    if (attenuationQ16 >= (INT32)(EMU8K_SILENCE_DB << EMU8K_Q16_SHIFT)) return 0;
    return Emu8kExp2(-(INT32)(((INT64)attenuationQ16 * EMU8K_OCTAVES_PER_DB_Q16) >> EMU8K_Q16_SHIFT));
}

#define EMU8K_TICK_US_X10 7256u                            /* 32 / 44100 s = 725.6 us      */
#define EMU8K_TICK_US_SCALE 10u                            /* ...the X10 above             */
/* The slewing current volume is held as CV << 14: CV is 16 bits, so << 16 would reach the
   sign bit of an INT32 at full volume. */
#define EMU8K_CURRENT_VOLUME_SHIFT 14

/* The attack and decay scales (p.15-16): rate code 01h is the slowest, 7Fh the fastest, 00h
   never. Between the ends the scale is logarithmic (ours -- the guide gives only the ends):
   126 steps across log2(fastest / slowest), in Q16. */
#define EMU8K_RATE_CODE_MASK         0x7F
#define EMU8K_RATE_CODE_SLOWEST      1
#define EMU8K_RATE_CODE_SPAN         126       /* 01h..7Fh                                   */
#define EMU8K_ATTACK_SLOWEST_US      11880000u /* 01h: 11.88 s                               */
#define EMU8K_ATTACK_LOG2_SPAN_Q16   717709ll  /* -log2(6 ms / 11.88 s) = 10.9513, Q16       */
#define EMU8K_DECAY_SLOWEST_US_PER_DB 470000u  /* 01h: 470 ms/dB                             */
#define EMU8K_DECAY_LOG2_SPAN_Q16    716669ll  /* -log2(240 us / 470 ms) = 10.9354, Q16      */
#define EMU8K_MINIMUM_STEP           1         /* a step that rounds to 0 would never move   */

DWORD VddEmu8kAttackMicroseconds(BYTE rateCode)
{
    /* p.16: 0x01 = 11.88 s, 0x7F = 6 ms, 0 = never. log2(6 ms / 11.88 s) = -10.9513. */
    rateCode &= EMU8K_RATE_CODE_MASK;
    if (!rateCode) return 0;
    return (DWORD)(((UINT64)EMU8K_ATTACK_SLOWEST_US
                    * Emu8kExp2(-(INT32)((EMU8K_ATTACK_LOG2_SPAN_Q16 * (rateCode - EMU8K_RATE_CODE_SLOWEST))
                                         / EMU8K_RATE_CODE_SPAN))) >> EMU8K_Q16_SHIFT);
}

DWORD VddEmu8kDecayMicrosecondsPerDb(BYTE rateCode)
{
    /* p.15: 0x01 = 470 ms/dB, 0x7F = 240 us/dB, 0 = no decay. log2(240 us / 470 ms)
       = -10.9354. The guide's own example -- 0x5C "for a release rate of 100 msec" (§7) --
       lands at 1.97 ms/dB here: about 50 dB of fall in 100 ms. */
    rateCode &= EMU8K_RATE_CODE_MASK;
    if (!rateCode) return 0;
    return (DWORD)(((UINT64)EMU8K_DECAY_SLOWEST_US_PER_DB
                    * Emu8kExp2(-(INT32)((EMU8K_DECAY_LOG2_SPAN_Q16 * (rateCode - EMU8K_RATE_CODE_SLOWEST))
                                         / EMU8K_RATE_CODE_SPAN))) >> EMU8K_Q16_SHIFT);
}

static VOID Emu8kBuildTables(VOID)
{
    /* 2^(1/256) = 1.0027112750502025. Plain double arithmetic -- no libm needed. */
    double power = EMU8K_EXP2_TABLE_START;
    UINT   entry;
    if (g_Emu8kTablesBuilt) return;
    for (entry = 0; entry <= EMU8K_EXP2_STEPS; ++entry) { g_Emu8kExp2Table[entry] = (DWORD)(power * EMU8K_Q16_ONE_DOUBLE + EMU8K_EXP2_ROUND_HALF); power *= EMU8K_EXP2_STEP_RATIO; }
    g_Emu8kExp2Table[EMU8K_EXP2_STEPS] = EMU8K_EXP2_TABLE_TOP;
    g_Emu8kAttackStep[0] = 0; g_Emu8kDecayStep[0] = 0;
    for (entry = EMU8K_RATE_CODE_SLOWEST; entry < EMU8K_RATE_CODES; ++entry) {
        DWORD attackMicroseconds = VddEmu8kAttackMicroseconds((BYTE)entry);
        DWORD decayMicrosecondsPerDb = VddEmu8kDecayMicrosecondsPerDb((BYTE)entry);
        g_Emu8kAttackStep[entry] = (DWORD)(((UINT64)EMU8K_Q16_ONE_UNSIGNED * EMU8K_TICK_US_X10) / ((UINT64)attackMicroseconds * EMU8K_TICK_US_SCALE));
        if (!g_Emu8kAttackStep[entry]) g_Emu8kAttackStep[entry] = EMU8K_MINIMUM_STEP;
        g_Emu8kDecayStep[entry]  = (INT32)(((UINT64)EMU8K_Q16_ONE_UNSIGNED * EMU8K_TICK_US_X10) / ((UINT64)decayMicrosecondsPerDb * EMU8K_TICK_US_SCALE));
        if (!g_Emu8kDecayStep[entry]) g_Emu8kDecayStep[entry] = EMU8K_MINIMUM_STEP;
    }
    g_Emu8kTablesBuilt = 1;
}

/* ---- sound memory (§5) ------------------------------------------------------------- */

/* A word of sound memory. The ROM space reads the ROM image if the host fitted one and
   zero otherwise (we have no GM ROM -- see the inventory); DRAM above what is fitted
   reads zero. Nothing aliases. */
static INT16 Emu8kReadMemory(PCEMU8K_STATE state, DWORD address)
{
    address &= EMU8K_ADDR_MASK;
    if (address < EMU8K_DRAM_BASE) return (state->Rom && address < state->RomWords) ? (INT16)state->Rom[address] : 0;
    address -= EMU8K_DRAM_BASE;
    return (state->Dram && address < state->DramWords) ? (INT16)state->Dram[address] : 0;
}
static VOID Emu8kWriteMemory(PEMU8K_STATE state, DWORD address, WORD value)
{
    address &= EMU8K_ADDR_MASK;
    if (address < EMU8K_DRAM_BASE) { state->SoundMemoryRomWrites++; return; }   /* ROM: the write goes nowhere */
    address -= EMU8K_DRAM_BASE;
    if (state->Dram && address < state->DramWords) state->Dram[address] = value;
}

/* Is any channel allocated to DMA stream `stream` (0 LR, 1 RR, 2 LW, 3 RW)? CCCA bit 26 =
   DMA, bit 25 = write, bit 24 = right (p.9-10) -- so bits 25-24 ARE the stream number. */
#define EMU8K_CCCA_DMA            0x04000000u
#define EMU8K_CCCA_STREAM_SHIFT   24
#define EMU8K_CCCA_STREAM_MASK    3
#define EMU8K_FIRST_WRITE_STREAM  2          /* streams 2, 3 write; 0, 1 read              */
#define EMU8K_ADDRESS_STEP        1          /* a stream advances one word per transfer    */

static BOOL Emu8kIsStreamAllocated(PCEMU8K_STATE state, UINT stream)
{
    UINT voiceIndex;
    for (voiceIndex = 0; voiceIndex < EMU8K_VOICES; ++voiceIndex)
        if ((state->Voices[voiceIndex].Ccca & EMU8K_CCCA_DMA)
            && ((state->Voices[voiceIndex].Ccca >> EMU8K_CCCA_STREAM_SHIFT) & EMU8K_CCCA_STREAM_MASK) == stream) return TRUE;
    return FALSE;
}

/* The transfers are instantaneous here, so FULL and EMPTY are set only while a stream has
   NO channel allocated to carry it (§5: the transfer then waits "until ... aborted because
   no channels are currently programmed"). Allocating one completes the waiting transfer. */
static VOID Emu8kServiceStreams(PEMU8K_STATE state)
{
    UINT side;
    for (side = 0; side < EMU8K_STEREO_SIDES; ++side) {
        if (state->SoundMemoryFull[side] && Emu8kIsStreamAllocated(state, EMU8K_FIRST_WRITE_STREAM + side)) {
            Emu8kWriteMemory(state, state->SoundMemoryAddress[EMU8K_FIRST_WRITE_STREAM + side], state->SoundMemoryWriteLatch[side]);
            state->SoundMemoryAddress[EMU8K_FIRST_WRITE_STREAM + side] = (state->SoundMemoryAddress[EMU8K_FIRST_WRITE_STREAM + side] + EMU8K_ADDRESS_STEP) & EMU8K_ADDR_MASK;
            state->SoundMemoryFull[side] = FALSE; state->SoundMemoryWordsWritten++;
        }
        if (state->SoundMemoryEmpty[side] && Emu8kIsStreamAllocated(state, side)) {
            state->SoundMemoryReadLatch[side] = (WORD)Emu8kReadMemory(state, state->SoundMemoryAddress[side]);
            state->SoundMemoryAddress[side] = (state->SoundMemoryAddress[side] + EMU8K_ADDRESS_STEP) & EMU8K_ADDR_MASK;
            state->SoundMemoryEmpty[side] = FALSE; state->SoundMemoryWordsRead++;
        }
    }
}

/* SMLD/SMRD write (p.12): the word goes to SMAxW, which then increments. */
static VOID Emu8kSoundMemoryWrite(PEMU8K_STATE state, UINT side, WORD value)
{
    state->SoundMemoryWriteLatch[side] = value;
    if (Emu8kIsStreamAllocated(state, EMU8K_FIRST_WRITE_STREAM + side)) {
        Emu8kWriteMemory(state, state->SoundMemoryAddress[EMU8K_FIRST_WRITE_STREAM + side], value);
        state->SoundMemoryAddress[EMU8K_FIRST_WRITE_STREAM + side] = (state->SoundMemoryAddress[EMU8K_FIRST_WRITE_STREAM + side] + EMU8K_ADDRESS_STEP) & EMU8K_ADDR_MASK;
        state->SoundMemoryFull[side] = FALSE; state->SoundMemoryWordsWritten++;
    } else { state->SoundMemoryFull[side] = TRUE; state->SoundMemoryHeld++; }
}

/* SMLD/SMRD read (p.12): hand over what the read register holds, then PREFETCH the word at
   SMAxR into it. So the first read after setting SMAxR returns STALE data -- the guide
   tells a driver to read and discard one word first (§5), and this is why. */
static WORD Emu8kSoundMemoryRead(PEMU8K_STATE state, UINT side)
{
    WORD heldWord = state->SoundMemoryReadLatch[side];
    if (Emu8kIsStreamAllocated(state, side)) {
        state->SoundMemoryReadLatch[side] = (WORD)Emu8kReadMemory(state, state->SoundMemoryAddress[side]);
        state->SoundMemoryAddress[side] = (state->SoundMemoryAddress[side] + EMU8K_ADDRESS_STEP) & EMU8K_ADDR_MASK;
        state->SoundMemoryEmpty[side] = FALSE; state->SoundMemoryWordsRead++;
    } else state->SoundMemoryEmpty[side] = TRUE;
    return heldWord;
}

/* WC counts at 44.1 kHz: microseconds x 441 / 10000 = samples. */
#define EMU8K_WALL_CLOCK_NUMERATOR   441u
#define EMU8K_WALL_CLOCK_DENOMINATOR 10000u

static WORD Emu8kGetWallClock(PCEMU8K_STATE state)
{
    /* p.13: "continuously incrementing at the sample rate ... no mechanism to reset". */
    if (state->Clock) return (WORD)((state->Clock(state->ClockContext) * EMU8K_WALL_CLOCK_NUMERATOR) / EMU8K_WALL_CLOCK_DENOMINATOR);
    return (WORD)state->WallClock;
}

/* ---- the envelope engine (p.14-18, the diagram on p.19) ---------------------------------- */

#define EMU8K_DB_PER_OCTAVE_Q16      394566    /* 6.0206 dB in Q16                           */
#define EMU8K_DELAY_NONE             0x8000    /* p.14: 8000h = no delay; below, 725 us units */
#define EMU8K_HOLD_NONE_CODE         0x7Fu     /* p.16: 7Fh = no hold                        */
#define EMU8K_HOLD_CODE_MASK         0x7F
#define EMU8K_HOLD_STEP_US_X10       920000u   /* 92 ms per hold step, x10                   */
#define EMU8K_SUSTAIN_TOP_CODE       0x7Fu     /* p.14: 7Fh = 0 dB below peak                */
#define EMU8K_SUSTAIN_CODE_MASK      0x7F
#define EMU8K_SUSTAIN_STEP_DB_Q16    49152u    /* 0.75 dB per sustain step, Q16              */
#define EMU8K_RELEASE_FLOOR_DB       96u       /* a release that falls 96 dB is done         */

static VOID Emu8kEnvelopeStart(PEMU8K_ENVELOPE envelope) { envelope->Phase = EMU8K_ENV_DELAY; envelope->TickCount = 0; envelope->Amplitude = 0; envelope->Attenuation = 0; }

/* Release from wherever the envelope is. Mid-attack, the linear level becomes dB. */
static VOID Emu8kEnvelopeRelease(PEMU8K_ENVELOPE envelope)
{
    if (envelope->Phase == EMU8K_ENV_OFF || envelope->Phase == EMU8K_ENV_DONE) return;
    if (envelope->Phase == EMU8K_ENV_DELAY) { envelope->Phase = EMU8K_ENV_DONE; return; }
    if (envelope->Phase == EMU8K_ENV_ATTACK) {
        if (envelope->Amplitude <= 0) { envelope->Phase = EMU8K_ENV_DONE; return; }
        /* dB = -20·log10(amp) = -6.0206·log2(amp) */
        envelope->Attenuation = (INT32)(-((INT64)Emu8kLog2((DWORD)envelope->Amplitude) * EMU8K_DB_PER_OCTAVE_Q16) >> EMU8K_Q16_SHIFT);
        if (envelope->Attenuation < 0) envelope->Attenuation = 0;
    }
    envelope->Phase = EMU8K_ENV_RELEASE;
}

/* One engine tick. `delayRegister` = ENVVOL/ENVVAL, `attackHold` = ATKHLDV/ATKHLD,
   `decaySustain` = DCYSUSV/DCYSUS. Returns the envelope's level, Q16 linear (0..65536). */
static DWORD Emu8kEnvelopeTick(PEMU8K_ENVELOPE envelope, WORD delayRegister, WORD attackHold, WORD decaySustain)
{
    switch (envelope->Phase) {
    case EMU8K_ENV_DELAY: {
        /* p.14: 8000h = no delay; below that, 725 us units -- one engine tick each. */
        DWORD delayTicks = delayRegister >= EMU8K_DELAY_NONE ? 0 : (DWORD)EMU8K_DELAY_NONE - delayRegister;
        if (envelope->TickCount < delayTicks) { envelope->TickCount++; return 0; }
        envelope->Phase = EMU8K_ENV_ATTACK; envelope->TickCount = 0; envelope->Amplitude = 0;
    }   /* fall through */
    case EMU8K_ENV_ATTACK:
        /* p.16: bits 6-0, 0 = never attack. The rise is linear in amplitude over the time. */
        envelope->Amplitude += (INT32)g_Emu8kAttackStep[attackHold & EMU8K_RATE_CODE_MASK];
        if (envelope->Amplitude < EMU8K_Q16_ONE) return (DWORD)envelope->Amplitude;
        envelope->Amplitude = EMU8K_Q16_ONE; envelope->Attenuation = 0; envelope->Phase = EMU8K_ENV_HOLD; envelope->TickCount = 0;
        /* fall through */
    case EMU8K_ENV_HOLD: {
        /* p.16: bits 14-8 in 92 ms steps, 7Fh = no hold, 00h = 11.68 s. */
        DWORD holdTicks = ((EMU8K_HOLD_NONE_CODE - ((attackHold >> EMU8K_HIGH_BYTE_SHIFT) & EMU8K_HOLD_CODE_MASK)) * EMU8K_HOLD_STEP_US_X10) / EMU8K_TICK_US_X10;
        if (envelope->TickCount < holdTicks) { envelope->TickCount++; return EMU8K_Q16_ONE_UNSIGNED; }
        envelope->Phase = EMU8K_ENV_DECAY;
    }   /* fall through */
    case EMU8K_ENV_DECAY: {
        /* p.14-15: sustain bits 14-8 in 0.75 dB steps below peak (7Fh = 0 dB, 0 = silence);
           decay bits 6-0 as a time per dB, 0 = no decay. dB-linear: the fall is exponential. */
        DWORD sustainCode = (decaySustain >> EMU8K_HIGH_BYTE_SHIFT) & EMU8K_SUSTAIN_CODE_MASK;
        INT32 sustainLevel = sustainCode ? (INT32)((EMU8K_SUSTAIN_TOP_CODE - sustainCode) * EMU8K_SUSTAIN_STEP_DB_Q16) : (INT32)(EMU8K_SILENCE_DB << EMU8K_Q16_SHIFT);  /* 0.75 dB = 49152 */
        if (envelope->Attenuation < sustainLevel) { envelope->Attenuation += g_Emu8kDecayStep[decaySustain & EMU8K_RATE_CODE_MASK]; if (envelope->Attenuation > sustainLevel) envelope->Attenuation = sustainLevel; }
        return Emu8kDbToGain(envelope->Attenuation); }
    case EMU8K_ENV_RELEASE:
        envelope->Attenuation += g_Emu8kDecayStep[decaySustain & EMU8K_RATE_CODE_MASK];
        if (envelope->Attenuation >= (INT32)(EMU8K_RELEASE_FLOOR_DB << EMU8K_Q16_SHIFT)) { envelope->Phase = EMU8K_ENV_DONE; return 0; }
        return Emu8kDbToGain(envelope->Attenuation);
    default:
        return 0;
    }
}

/* A triangle LFO (p.18: FRQ in 0.042 Hz steps, FFh = 10.72 Hz). Starts at zero going up.
   Returns -65536..65536. */
#define EMU8K_LFO_PHASE_STEP        131008u   /* 2^32 x (10.72/255 Hz) x 725.6 us, per FRQ  */
#define EMU8K_LFO_QUARTER_CYCLE     16384u    /* of the 16-bit phase                        */
#define EMU8K_LFO_THREE_QUARTERS    49152u
#define EMU8K_LFO_SLOPE             4u        /* 16384 phase units rise 65536               */
#define EMU8K_LFO_HALF_CYCLE_LEVEL  131072
#define EMU8K_LFO_FULL_CYCLE_LEVEL  262144

static INT32 Emu8kLfoTick(PDWORD phase, PDWORD delay, BYTE frequency)
{
    DWORD phaseHigh;
    if (*delay) { (*delay)--; return 0; }
    phaseHigh = *phase >> EMU8K_Q16_SHIFT;
    /* phase step per tick = 2^32 × (frq × 10.72/255 Hz) × 725.6 us = frq × 131008 */
    *phase += (DWORD)frequency * EMU8K_LFO_PHASE_STEP;
    if (phaseHigh < EMU8K_LFO_QUARTER_CYCLE) return (INT32)(phaseHigh * EMU8K_LFO_SLOPE);
    if (phaseHigh < EMU8K_LFO_THREE_QUARTERS) return EMU8K_LFO_HALF_CYCLE_LEVEL - (INT32)(phaseHigh * EMU8K_LFO_SLOPE);
    return (INT32)(phaseHigh * EMU8K_LFO_SLOPE) - EMU8K_LFO_FULL_CYCLE_LEVEL;
}
static DWORD Emu8kLfoDelayTicks(WORD delayRegister) { return delayRegister >= EMU8K_DELAY_NONE ? 0 : (DWORD)EMU8K_DELAY_NONE - delayRegister; }

/* ---- the low-pass filter (CCCA Q, IFATN cutoff: p.9, p.17) --------------------------- */

/* sin/cos by series, in double: only ever called when a channel's cutoff or Q CHANGES. The
   divisors are the series' successive n(n+1): 2·3, 4·5, ... and 1·2, 3·4, ... */
#define EMU8K_SERIES_2X3    6
#define EMU8K_SERIES_4X5    20
#define EMU8K_SERIES_6X7    42
#define EMU8K_SERIES_8X9    72
#define EMU8K_SERIES_10X11  110
#define EMU8K_SERIES_1X2    2
#define EMU8K_SERIES_3X4    12
#define EMU8K_SERIES_5X6    30
#define EMU8K_SERIES_7X8    56
#define EMU8K_SERIES_9X10   90
#define EMU8K_SERIES_11X12  132

static double Emu8kSin(double angle) { double angleSquared = angle * angle;
    return angle * (1 - angleSquared / EMU8K_SERIES_2X3 * (1 - angleSquared / EMU8K_SERIES_4X5 * (1 - angleSquared / EMU8K_SERIES_6X7 * (1 - angleSquared / EMU8K_SERIES_8X9 * (1 - angleSquared / EMU8K_SERIES_10X11))))); }
static double Emu8kCos(double angle) { double angleSquared = angle * angle;
    return 1 - angleSquared / EMU8K_SERIES_1X2 * (1 - angleSquared / EMU8K_SERIES_3X4 * (1 - angleSquared / EMU8K_SERIES_5X6 * (1 - angleSquared / EMU8K_SERIES_7X8 * (1 - angleSquared / EMU8K_SERIES_9X10 * (1 - angleSquared / EMU8K_SERIES_11X12))))); }

/* The cutoff scale. p.17 puts IFATN's cutoff byte at "quarter semitones, 00h = 125 Hz,
   FFh = 8 kHz" -- but 255 quarter-semitones is 5.3 octaves and 125 Hz -> 8 kHz is 6. The
   END POINTS are what a program sets, so they win: six octaves across 0000h..FF00h of the
   16-bit cutoff, i.e. one octave = FF00h / 6 = 2A80h units. */
#define EMU8K_CUTOFF_OCTAVE 0x2A80
#define EMU8K_CUTOFF_BASE_HZ          125.0     /* cutoff 0000h                               */
#define EMU8K_CUTOFF_MAX_FRACTION     0.45      /* of the sample rate: below Nyquist          */
#define EMU8K_RADIANS_PER_CYCLE       (2.0 * 3.14159265358979)
#define EMU8K_FILTER_FLAT_Q           0         /* p.17: Q 0 and cutoff FFh = unaltered       */
#define EMU8K_FILTER_OPEN_CUTOFF      0xFF00
#define EMU8K_BUTTERWORTH_Q           0.70710678
#define EMU8K_RESONANCE_STEP_DB_Q16   117965u   /* per Q step, Q16                            */
#define EMU8K_FILTER_HEADROOM_MAX     262143    /* resonance headroom: 18 bits                */
#define EMU8K_FILTER_HEADROOM_MIN     (-262144)

static VOID Emu8kFilterSetup(PEMU8K_VOICE voice, WORD cutoff, BYTE resonance)
{
    double cutoffHz, angularFrequency, sine, cosine, alpha, quality, normaliser;
    if (voice->FilterIsValid && voice->FilterCutoff == cutoff && voice->FilterQ == resonance) return;
    voice->FilterCutoff = cutoff; voice->FilterQ = resonance; voice->FilterIsValid = TRUE;
    /* p.17: Q 0 and cutoff FFh = "the filter does not alter the signal". Exactly that. */
    voice->FilterIsBypassed = (BYTE)(resonance == EMU8K_FILTER_FLAT_Q && cutoff >= EMU8K_FILTER_OPEN_CUTOFF);
    if (voice->FilterIsBypassed) { voice->X1 = voice->X2 = voice->Y1 = voice->Y2 = 0; return; }
    cutoffHz = EMU8K_CUTOFF_BASE_HZ * (double)Emu8kExp2((INT32)(((DWORD)cutoff << EMU8K_Q16_SHIFT) / EMU8K_CUTOFF_OCTAVE)) / EMU8K_Q16_ONE_DOUBLE;
    if (cutoffHz > EMU8K_CUTOFF_MAX_FRACTION * EMU8K_RATE_HZ) cutoffHz = EMU8K_CUTOFF_MAX_FRACTION * EMU8K_RATE_HZ;
    angularFrequency = EMU8K_RADIANS_PER_CYCLE * cutoffHz / EMU8K_RATE_HZ;
    sine = Emu8kSin(angularFrequency); cosine = Emu8kCos(angularFrequency);
    /* p.9: Q 0 = no resonance, 15 = "about 24 dB". A resonant 2-pole's peak is ≈ its Q, so
       Q 0 is the flat Butterworth 0.707 and each step adds 1.6 dB above it (+3 dB for the
       0.707 itself): 15 -> 0.707 × 10^(27/20) ≈ 15.8 ≈ 24 dB. */
    quality = EMU8K_BUTTERWORTH_Q * (double)Emu8kExp2((INT32)(((UINT64)resonance * EMU8K_RESONANCE_STEP_DB_Q16 * EMU8K_OCTAVES_PER_DB_Q16) >> EMU8K_Q16_SHIFT)) / EMU8K_Q16_ONE_DOUBLE;
    alpha = sine / (2.0 * quality);
    normaliser = 1.0 + alpha;
    voice->B0 = (INT32)(((1.0 - cosine) / 2.0) / normaliser * EMU8K_Q28_ONE_DOUBLE);
    voice->B1 = (INT32)((1.0 - cosine) / normaliser * EMU8K_Q28_ONE_DOUBLE);
    voice->B2 = voice->B0;
    voice->A1 = (INT32)((-2.0 * cosine) / normaliser * EMU8K_Q28_ONE_DOUBLE);
    voice->A2 = (INT32)((1.0 - alpha) / normaliser * EMU8K_Q28_ONE_DOUBLE);
}

static INT32 Emu8kFilterRun(PEMU8K_VOICE voice, INT32 input)
{
    INT64 accumulator;
    INT32 output;
    if (voice->FilterIsBypassed) return input;
    accumulator = (INT64)voice->B0 * input + (INT64)voice->B1 * voice->X1 + (INT64)voice->B2 * voice->X2
                - (INT64)voice->A1 * voice->Y1 - (INT64)voice->A2 * voice->Y2;
    output = (INT32)(accumulator >> EMU8K_Q28_SHIFT);
    if (output > EMU8K_FILTER_HEADROOM_MAX) output = EMU8K_FILTER_HEADROOM_MAX; else if (output < EMU8K_FILTER_HEADROOM_MIN) output = EMU8K_FILTER_HEADROOM_MIN;   /* resonance headroom */
    voice->X2 = voice->X1; voice->X1 = input; voice->Y2 = voice->Y1; voice->Y1 = output;
    return output;
}

/* The per-channel engine tick: the envelope generator (unless DCYSUSV bit 7 has turned it
   off) recomputes the TARGETS, then the sound generator takes them up. */
#define EMU8K_DCYSUSV_ENGINE_OFF      0x80      /* p.14: bit 7                                */
#define EMU8K_PITCH_UNITY             0xE000    /* p.16: IP E000h = unity                     */
#define EMU8K_PITCH_TO_Q16_OCTAVES    16        /* 1000h per octave -> 65536 per octave       */
#define EMU8K_MODULATION_FULL_SCALE   127       /* a signed depth byte's full scale           */
#define EMU8K_CP_UNITY                0x4000u   /* p.7: CP 4000h = one word per sample        */
#define EMU8K_PITCH_MAX_OCTAVES       2         /* CP saturates two octaves up                */
#define EMU8K_CUTOFF_ENVELOPE_OCTAVES 6         /* ENV1 x PEFE lo: up to ±6 octaves           */
#define EMU8K_CUTOFF_LFO_OCTAVES      3         /* LFO1 x FMMOD lo: up to ±3 octaves          */
#define EMU8K_ATTENUATION_STEP_DB_Q16 24576     /* IFATN lo: 0.375 dB steps                   */
#define EMU8K_TREMOLO_DB              12        /* LFO1 x TREMFRQ hi: up to ±12 dB            */
#define EMU8K_TICK_ROUNDING           (EMU8K_TICK - 1u)  /* rounds a step away from zero    */
#define EMU8K_PAN_SHIFT               24        /* PSST bits 31-24                            */
#define EMU8K_PAN_FULL                255u      /* FFh = extreme left                         */
#define EMU8K_GAIN_UNITY              256u      /* Q8                                         */

static VOID Emu8kVoiceTick(PEMU8K_VOICE voice)
{
    INT32 currentVolume, volumeTarget;
    DWORD pitchTarget, cutoffTarget;
    if (!(voice->Dcysusv & EMU8K_DCYSUSV_ENGINE_OFF) && !(voice->Ccca & EMU8K_CCCA_DMA)) {
        DWORD volumeLevel = Emu8kEnvelopeTick(&voice->VolumeEnvelope, voice->Envvol, voice->Atkhldv, voice->Dcysusv);
        INT32 modulation  = (INT32)Emu8kEnvelopeTick(&voice->ModulationEnvelope, voice->Envval, voice->Atkhld, voice->Dcysus);
        INT32 lfo1        = Emu8kLfoTick(&voice->Lfo1Phase, &voice->Lfo1Delay, (BYTE)voice->Tremfrq);
        INT32 lfo2        = Emu8kLfoTick(&voice->Lfo2Phase, &voice->Lfo2Delay, (BYTE)voice->Fm2frq2);
        INT32 octaves, cutoff, attenuation;
        DWORD gain;
        /* pitch: IP E000h = unity, 1000h per octave (p.16) -> Q16 octaves; then ENV1 ×
           PEFE hi (±1 oct), LFO1 × FMMOD hi (±1 oct), LFO2 × FM2FRQ2 hi (±1 oct) */
        octaves  = ((INT32)voice->Ip - EMU8K_PITCH_UNITY) * EMU8K_PITCH_TO_Q16_OCTAVES;
        octaves += (INT32)(((INT64)modulation * (INT8)(voice->Pefe >> EMU8K_HIGH_BYTE_SHIFT)) / EMU8K_MODULATION_FULL_SCALE);
        octaves += (INT32)(((INT64)lfo1  * (INT8)(voice->Fmmod >> EMU8K_HIGH_BYTE_SHIFT)) / EMU8K_MODULATION_FULL_SCALE);
        octaves += (INT32)(((INT64)lfo2  * (INT8)(voice->Fm2frq2 >> EMU8K_HIGH_BYTE_SHIFT)) / EMU8K_MODULATION_FULL_SCALE);
        pitchTarget = (DWORD)(((UINT64)Emu8kExp2(octaves) * EMU8K_CP_UNITY) >> EMU8K_Q16_SHIFT); /* 4000h = unity (p.7) */
        if (octaves >= (EMU8K_PITCH_MAX_OCTAVES << EMU8K_Q16_SHIFT) || pitchTarget > EMU8K_WORD_MAX) pitchTarget = EMU8K_WORD_MAX;
        /* cutoff: IFATN hi, ENV1 × PEFE lo (±6 oct), LFO1 × FMMOD lo (±3 oct) */
        cutoff  = (INT32)(voice->Ifatn & HIGH_BYTE_MASK);
        cutoff += (INT32)(((INT64)modulation * (INT8)voice->Pefe  * EMU8K_CUTOFF_ENVELOPE_OCTAVES * EMU8K_CUTOFF_OCTAVE / EMU8K_MODULATION_FULL_SCALE) >> EMU8K_Q16_SHIFT);
        cutoff += (INT32)(((INT64)lfo1  * (INT8)voice->Fmmod * EMU8K_CUTOFF_LFO_OCTAVES * EMU8K_CUTOFF_OCTAVE / EMU8K_MODULATION_FULL_SCALE) >> EMU8K_Q16_SHIFT);
        cutoffTarget = cutoff < 0 ? 0 : cutoff > EMU8K_WORD_MAX ? EMU8K_WORD_MAX : (DWORD)cutoff;
        /* volume: ENV2 × IFATN lo (0.375 dB steps) × LFO1 tremolo (TREMFRQ hi, ±12 dB) */
        attenuation  = (INT32)(voice->Ifatn & BYTE_MASK) * EMU8K_ATTENUATION_STEP_DB_Q16;  /* 0.375 dB = 24576 Q16    */
        attenuation -= (INT32)(((INT64)lfo1 * (INT8)(voice->Tremfrq >> EMU8K_HIGH_BYTE_SHIFT) * EMU8K_TREMOLO_DB) / EMU8K_MODULATION_FULL_SCALE);
        gain = (DWORD)(((UINT64)volumeLevel * Emu8kDbToGain(attenuation)) >> EMU8K_Q16_SHIFT);
        if (attenuation < 0) gain = (DWORD)(((UINT64)volumeLevel * Emu8kExp2((INT32)(((INT64)-attenuation * EMU8K_OCTAVES_PER_DB_Q16) >> EMU8K_Q16_SHIFT))) >> EMU8K_Q16_SHIFT);
        volumeTarget = gain >= EMU8K_Q16_ONE_UNSIGNED ? EMU8K_WORD_MAX : (INT32)gain;
        voice->Ptrx = (pitchTarget << EMU8K_HIGH_WORD_SHIFT) | (voice->Ptrx & WORD_MASK_U);
        voice->Vtft = ((DWORD)volumeTarget << EMU8K_HIGH_WORD_SHIFT) | cutoffTarget;
    }
    /* The sound generator: current pitch and cutoff take their targets at once; current
       volume slews to its target across the tick, so an envelope step is not a click. */
    voice->Cpf  = (voice->Ptrx & HIGH_WORD_MASK_U) | (voice->Cpf & WORD_MASK_U);
    voice->Cvcf = (voice->Cvcf & HIGH_WORD_MASK_U) | (voice->Vtft & WORD_MASK_U);
    volumeTarget = (INT32)(voice->Vtft >> EMU8K_HIGH_WORD_SHIFT);
    currentVolume = voice->CurrentVolume;
    /* rounded AWAY from zero, so the slew ARRIVES: a truncated step settles short of the
       target (FFFEh for FFFFh) and never moves again. The render clamps the overshoot. */
    { INT32 volumeDelta = (volumeTarget << EMU8K_CURRENT_VOLUME_SHIFT) - currentVolume;
      voice->CurrentVolumeStep = (volumeDelta + (volumeDelta > 0 ? (INT32)EMU8K_TICK_ROUNDING : volumeDelta < 0 ? -((INT32)EMU8K_TICK_ROUNDING) : 0))
                 / (INT32)EMU8K_TICK; }
    /* p.9: PSST bits 31-24, 0 = extreme RIGHT, FFh = extreme LEFT. The diagram (p.19) sends
       PAN to one side and its LOGICAL NOT to the other: a linear crossfade. */
    { DWORD pan = voice->Psst >> EMU8K_PAN_SHIFT;
      voice->GainLeft = (INT32)((pan * EMU8K_GAIN_UNITY) / EMU8K_PAN_FULL);
      voice->GainRight = (INT32)(((EMU8K_PAN_FULL - pan) * EMU8K_GAIN_UNITY) / EMU8K_PAN_FULL); }
}

/* ---- the register file (§3) ------------------------------------------------------------- */

/* The Pointer register (§2): bits 7-5 select the register, bits 4-0 the channel. */
#define EMU8K_POINTER_REGISTER_SHIFT  5
#define EMU8K_POINTER_REGISTER_MASK   7
#define EMU8K_POINTER_CHANNEL_MASK    0x1F

/* The four data ports, as the register file numbers them. */
#define EMU8K_DATA0  0
#define EMU8K_DATA1  1
#define EMU8K_DATA2  2
#define EMU8K_DATA3  3

/* The registers behind each data port (p.6-7). */
#define EMU8K_DATA0_CPF      0
#define EMU8K_DATA0_PTRX     1
#define EMU8K_DATA0_CVCF     2
#define EMU8K_DATA0_VTFT     3
#define EMU8K_DATA0_R4       4
#define EMU8K_DATA0_R5       5
#define EMU8K_DATA0_PSST     6
#define EMU8K_DATA0_CSL      7
#define EMU8K_DATA1_CCCA     0
#define EMU8K_DATA1_R1       1         /* HWCF4-6, the sound-memory addresses, SMLD, HWCF1-3 */
#define EMU8K_DATA1_INIT1    2
#define EMU8K_DATA1_INIT3    3
#define EMU8K_DATA1_ENVVOL   4
#define EMU8K_DATA1_DCYSUSV  5
#define EMU8K_DATA1_ENVVAL   6
#define EMU8K_DATA1_DCYSUS   7
#define EMU8K_DATA2_R1       1         /* SMRD, WC, and words the map does not name         */
#define EMU8K_DATA2_INIT2    2
#define EMU8K_DATA2_INIT4    3
#define EMU8K_DATA2_ATKHLDV  4
#define EMU8K_DATA2_LFO1VAL  5
#define EMU8K_DATA2_ATKHLD   6
#define EMU8K_DATA2_LFO2VAL  7
#define EMU8K_DATA3_IP       0
#define EMU8K_DATA3_IFATN    1
#define EMU8K_DATA3_PEFE     2
#define EMU8K_DATA3_FMMOD    3
#define EMU8K_DATA3_TREMFRQ  4
#define EMU8K_DATA3_FM2FRQ2  5
#define EMU8K_DATA3_R6       6
#define EMU8K_DATA3_R7       7

/* The global registers at Data1/Data2 r1, by channel number (p.10-13). */
#define EMU8K_CHANNEL_HWCF4  9
#define EMU8K_CHANNEL_HWCF5  10
#define EMU8K_CHANNEL_HWCF6  13
#define EMU8K_CHANNEL_SMALR  20        /* the four sound-memory addresses: 20-23             */
#define EMU8K_CHANNEL_SMARR  21
#define EMU8K_CHANNEL_SMALW  22
#define EMU8K_CHANNEL_SMARW  23
#define EMU8K_CHANNEL_SMLD   26        /* Data1 r1: SMLD; Data2 r1: SMRD                     */
#define EMU8K_CHANNEL_WC     27        /* Data2 r1: the sample counter, read-only            */
#define EMU8K_CHANNEL_HWCF1  29
#define EMU8K_CHANNEL_HWCF2  30
#define EMU8K_CHANNEL_HWCF3  31

/* Which half of a doubleword register a word access is, and which stereo side. */
#define EMU8K_LOW_HALF       0
#define EMU8K_HIGH_HALF      1
#define EMU8K_LEFT           0
#define EMU8K_RIGHT          1

/* The four INIT effects programs, as EffectsInit's first index. */
#define EMU8K_INIT1          0
#define EMU8K_INIT2          1
#define EMU8K_INIT3          2
#define EMU8K_INIT4          3

#define EMU8K_DCYSUSV_RELEASE     0x8000   /* p.14: bit 15 = these are RELEASE values         */
#define EMU8K_ALWAYS_ZERO_BIT     0x80u    /* bit 7 of ATKHLD(V) and DCYSUS reads zero        */
#define EMU8K_SMA_FLAG            0x80000000u  /* SMAxR bit 31 = EMPTY, SMAxW bit 31 = FULL   */
#define EMU8K_HWCF1_READ_MASK     0x7E     /* the VLSI read error's shape, as drivers probe   */
#define EMU8K_HWCF2_READ_SET      0x0003

/* Data1's MS word and Data2 share E+402h. It is Data1's MS word exactly when the register
   selected is a Data1 DOUBLEWORD (p.6-7): CCCA (r0, every channel) and, in r1, HWCF4/5/6
   (ch 9, 10, 13) and the four sound-memory addresses (ch 20-23). Everything else there is
   the Data2 word register. */
static BOOL Emu8kIsData1DoubleWord(BYTE registerNumber, BYTE channel)
{
    if (registerNumber == EMU8K_DATA1_CCCA) return TRUE;
    if (registerNumber != EMU8K_DATA1_R1) return FALSE;
    return channel == EMU8K_CHANNEL_HWCF4 || channel == EMU8K_CHANNEL_HWCF5 || channel == EMU8K_CHANNEL_HWCF6 || (channel >= EMU8K_CHANNEL_SMALR && channel <= EMU8K_CHANNEL_SMARW);
}

static DWORD Emu8kSetHalf(DWORD registerValue, INT isHighHalf, WORD value)
{ return isHighHalf ? (registerValue & EMU8K_LOW_WORD_MASK_KEEP_HIGH) | ((DWORD)value << EMU8K_HIGH_WORD_SHIFT) : (registerValue & HIGH_WORD_MASK_U) | value; }
static WORD Emu8kGetHalf(DWORD registerValue, INT isHighHalf) { return (WORD)(isHighHalf ? registerValue >> EMU8K_HIGH_WORD_SHIFT : registerValue); }

/* DCYSUSV (p.14-15): bit 7 turns the engine off; bit 15 = these are RELEASE values. Turning
   the engine ON with a decay (bit 15 clear) is what starts a note -- §6 writes it last but
   one, after the envelope parameters and the silent CVCF/VTFT. */
static VOID Emu8kWriteDcysusv(PEMU8K_VOICE voice, WORD value, PEMU8K_STATE state)
{
    INT wasOff = (voice->Dcysusv & EMU8K_DCYSUSV_ENGINE_OFF) != 0;
    voice->Dcysusv = value;
    if (value & EMU8K_DCYSUSV_ENGINE_OFF) return;           /* engine off: nothing more moves   */
    if (value & EMU8K_DCYSUSV_RELEASE) { Emu8kEnvelopeRelease(&voice->VolumeEnvelope); state->Releases++; return; }
    if (wasOff || voice->VolumeEnvelope.Phase == EMU8K_ENV_OFF || voice->VolumeEnvelope.Phase == EMU8K_ENV_DONE) {
        Emu8kEnvelopeStart(&voice->VolumeEnvelope); Emu8kEnvelopeStart(&voice->ModulationEnvelope);
        voice->Lfo1Phase = voice->Lfo2Phase = 0;
        voice->Lfo1Delay = Emu8kLfoDelayTicks(voice->Lfo1val);
        voice->Lfo2Delay = Emu8kLfoDelayTicks(voice->Lfo2val);
        state->NotesStarted++;
    }
}

static VOID Emu8kDataWrite(PEMU8K_STATE state, INT dataPort, INT isHighHalf, WORD value)
{
    BYTE registerNumber = (BYTE)((state->Pointer >> EMU8K_POINTER_REGISTER_SHIFT) & EMU8K_POINTER_REGISTER_MASK), channel = (BYTE)(state->Pointer & EMU8K_POINTER_CHANNEL_MASK);
    PEMU8K_VOICE voice = &state->Voices[channel];
    if (dataPort == EMU8K_DATA0) {                   /* Data0: all doublewords            */
        switch (registerNumber) {
        case EMU8K_DATA0_CPF: voice->Cpf  = Emu8kSetHalf(voice->Cpf, isHighHalf, value); break;
        case EMU8K_DATA0_PTRX: voice->Ptrx = Emu8kSetHalf(voice->Ptrx, isHighHalf, value); break;
        case EMU8K_DATA0_CVCF: voice->Cvcf = Emu8kSetHalf(voice->Cvcf, isHighHalf, value);
                if (isHighHalf) { voice->CurrentVolume = (INT32)value << EMU8K_CURRENT_VOLUME_SHIFT; voice->CurrentVolumeStep = 0; }
                break;
        case EMU8K_DATA0_VTFT: voice->Vtft = Emu8kSetHalf(voice->Vtft, isHighHalf, value); break;
        case EMU8K_DATA0_R4: voice->Data0Register4 = Emu8kSetHalf(voice->Data0Register4, isHighHalf, value); break;
        case EMU8K_DATA0_R5: voice->Data0Register5 = Emu8kSetHalf(voice->Data0Register5, isHighHalf, value); break;
        case EMU8K_DATA0_PSST: voice->Psst = Emu8kSetHalf(voice->Psst, isHighHalf, value); break;
        case EMU8K_DATA0_CSL: voice->Csl  = Emu8kSetHalf(voice->Csl, isHighHalf, value); break;
        }
        return;
    }
    if (dataPort == EMU8K_DATA1) {                   /* Data1                             */
        switch (registerNumber) {
        case EMU8K_DATA1_CCCA:
            /* p.9: bit 27 "should always be zero" -- stored as written */
            voice->Ccca = Emu8kSetHalf(voice->Ccca, isHighHalf, value);
            Emu8kServiceStreams(state);              /* a channel may now carry a stream  */
            break;
        case EMU8K_DATA1_R1:
            switch (channel) {
            case EMU8K_CHANNEL_HWCF4:  state->Hwcf4 = Emu8kSetHalf(state->Hwcf4, isHighHalf, value); break;
            case EMU8K_CHANNEL_HWCF5: state->Hwcf5 = Emu8kSetHalf(state->Hwcf5, isHighHalf, value); break;
            case EMU8K_CHANNEL_HWCF6: state->Hwcf6 = Emu8kSetHalf(state->Hwcf6, isHighHalf, value); break;
            case EMU8K_CHANNEL_SMALR: case EMU8K_CHANNEL_SMARR: case EMU8K_CHANNEL_SMALW: case EMU8K_CHANNEL_SMARW:
                /* bits 31-24 "Don't Care on write" (p.10-11) */
                state->SoundMemoryAddress[channel - EMU8K_CHANNEL_SMALR] = Emu8kSetHalf(state->SoundMemoryAddress[channel - EMU8K_CHANNEL_SMALR], isHighHalf, value) & EMU8K_ADDR_MASK;
                break;
            case EMU8K_CHANNEL_SMLD: Emu8kSoundMemoryWrite(state, EMU8K_LEFT, value); break;  /* SMLD */
            case EMU8K_CHANNEL_HWCF1: state->Hwcf1 = value; break;
            case EMU8K_CHANNEL_HWCF2: state->Hwcf2 = value; break;
            case EMU8K_CHANNEL_HWCF3: state->Hwcf3 = value; break;
            default: state->Data1Register1[channel] = Emu8kSetHalf(state->Data1Register1[channel], isHighHalf, value); break;
            }
            break;
        case EMU8K_DATA1_INIT1: state->EffectsInit[EMU8K_INIT1][channel] = value; break;
        case EMU8K_DATA1_INIT3: state->EffectsInit[EMU8K_INIT3][channel] = value; break;
        case EMU8K_DATA1_ENVVOL: voice->Envvol = value; break;
        case EMU8K_DATA1_DCYSUSV: Emu8kWriteDcysusv(voice, value, state); break;
        case EMU8K_DATA1_ENVVAL: voice->Envval = value; break;
        case EMU8K_DATA1_DCYSUS:                     /* DCYSUS: bit 7 is always zero     */
            voice->Dcysus = (WORD)(value & ~EMU8K_ALWAYS_ZERO_BIT);
            if (value & EMU8K_DCYSUSV_RELEASE) Emu8kEnvelopeRelease(&voice->ModulationEnvelope);
            break;
        }
        return;
    }
    if (dataPort == EMU8K_DATA2) {                   /* Data2: all words                  */
        switch (registerNumber) {
        case EMU8K_DATA2_R1:
            if (channel == EMU8K_CHANNEL_SMLD) Emu8kSoundMemoryWrite(state, EMU8K_RIGHT, value);   /* SMRD */
            else if (channel != EMU8K_CHANNEL_WC) state->Data2Register1[channel] = value;     /* WC is read-only */
            break;
        case EMU8K_DATA2_INIT2: state->EffectsInit[EMU8K_INIT2][channel] = value; break;
        case EMU8K_DATA2_INIT4: state->EffectsInit[EMU8K_INIT4][channel] = value; break;
        case EMU8K_DATA2_ATKHLDV: voice->Atkhldv = (WORD)(value & ~EMU8K_ALWAYS_ZERO_BIT); break;
        case EMU8K_DATA2_LFO1VAL: voice->Lfo1val = value; break;
        case EMU8K_DATA2_ATKHLD: voice->Atkhld  = (WORD)(value & ~EMU8K_ALWAYS_ZERO_BIT); break;
        case EMU8K_DATA2_LFO2VAL: voice->Lfo2val = value; break;
        default: break;
        }
        return;
    }
    switch (registerNumber) {                        /* Data3: all words                  */
    case EMU8K_DATA3_IP: voice->Ip = value; break;
    case EMU8K_DATA3_IFATN: voice->Ifatn = value; break;
    case EMU8K_DATA3_PEFE: voice->Pefe = value; break;
    case EMU8K_DATA3_FMMOD: voice->Fmmod = value; break;
    case EMU8K_DATA3_TREMFRQ: voice->Tremfrq = value; break;
    case EMU8K_DATA3_FM2FRQ2: voice->Fm2frq2 = value; break;
    case EMU8K_DATA3_R6: voice->Data3Register6 = value; break;
    case EMU8K_DATA3_R7: voice->Data3Register7 = value; break;
    }
}

static WORD Emu8kDataRead(PEMU8K_STATE state, INT dataPort, INT isHighHalf)
{
    BYTE registerNumber = (BYTE)((state->Pointer >> EMU8K_POINTER_REGISTER_SHIFT) & EMU8K_POINTER_REGISTER_MASK), channel = (BYTE)(state->Pointer & EMU8K_POINTER_CHANNEL_MASK);
    PEMU8K_VOICE voice = &state->Voices[channel];
    if (dataPort == EMU8K_DATA0) {
        switch (registerNumber) {
        case EMU8K_DATA0_CPF: return Emu8kGetHalf(voice->Cpf, isHighHalf);
        case EMU8K_DATA0_PTRX: return Emu8kGetHalf(voice->Ptrx, isHighHalf);
        case EMU8K_DATA0_CVCF: return Emu8kGetHalf((voice->Cvcf & WORD_MASK_U) | ((DWORD)(voice->CurrentVolume >> EMU8K_CURRENT_VOLUME_SHIFT) << EMU8K_HIGH_WORD_SHIFT), isHighHalf);
        case EMU8K_DATA0_VTFT: return Emu8kGetHalf(voice->Vtft, isHighHalf);
        case EMU8K_DATA0_R4: return Emu8kGetHalf(voice->Data0Register4, isHighHalf);
        case EMU8K_DATA0_R5: return Emu8kGetHalf(voice->Data0Register5, isHighHalf);
        case EMU8K_DATA0_PSST: return Emu8kGetHalf(voice->Psst, isHighHalf);
        default: return Emu8kGetHalf(voice->Csl, isHighHalf);
        }
    }
    if (dataPort == EMU8K_DATA1) {
        switch (registerNumber) {
        case EMU8K_DATA1_CCCA: return Emu8kGetHalf(voice->Ccca, isHighHalf);
        case EMU8K_DATA1_R1:
            switch (channel) {
            case EMU8K_CHANNEL_HWCF4:  return Emu8kGetHalf(state->Hwcf4, isHighHalf);
            case EMU8K_CHANNEL_HWCF5: return Emu8kGetHalf(state->Hwcf5, isHighHalf);
            case EMU8K_CHANNEL_HWCF6: return Emu8kGetHalf(state->Hwcf6, isHighHalf);
            case EMU8K_CHANNEL_SMALR: case EMU8K_CHANNEL_SMARR:   /* bit 31 = EMPTY, 30-24 read zero   */
                return Emu8kGetHalf(state->SoundMemoryAddress[channel - EMU8K_CHANNEL_SMALR] | (state->SoundMemoryEmpty[channel - EMU8K_CHANNEL_SMALR] ? EMU8K_SMA_FLAG : 0), isHighHalf);
            case EMU8K_CHANNEL_SMALW: case EMU8K_CHANNEL_SMARW:   /* bit 31 = FULL                     */
                return Emu8kGetHalf(state->SoundMemoryAddress[channel - EMU8K_CHANNEL_SMALR] | (state->SoundMemoryFull[channel - EMU8K_CHANNEL_SMALW] ? EMU8K_SMA_FLAG : 0), isHighHalf);
            case EMU8K_CHANNEL_SMLD: return Emu8kSoundMemoryRead(state, EMU8K_LEFT);
            /* p.13: HWCF1-3 "will not be correctly read by the processor" (a VLSI error).
               What the error LOOKS like is not in the guide; the period drivers probe for
               it -- HWCF1 read back masked with 7Eh must be 58h after 59h was written, and
               HWCF2's low two bits must be set -- so that is the shape modelled. */
            case EMU8K_CHANNEL_HWCF1: return (WORD)(state->Hwcf1 & EMU8K_HWCF1_READ_MASK);
            case EMU8K_CHANNEL_HWCF2: return (WORD)(state->Hwcf2 | EMU8K_HWCF2_READ_SET);
            case EMU8K_CHANNEL_HWCF3: return state->Hwcf3;
            default: return Emu8kGetHalf(state->Data1Register1[channel], isHighHalf);
            }
        case EMU8K_DATA1_INIT1: return state->EffectsInit[EMU8K_INIT1][channel];
        case EMU8K_DATA1_INIT3: return state->EffectsInit[EMU8K_INIT3][channel];
        case EMU8K_DATA1_ENVVOL: return voice->Envvol;
        case EMU8K_DATA1_DCYSUSV: return voice->Dcysusv;
        case EMU8K_DATA1_ENVVAL: return voice->Envval;
        default: return voice->Dcysus;
        }
    }
    if (dataPort == EMU8K_DATA2) {
        switch (registerNumber) {
        case EMU8K_DATA2_R1:
            if (channel == EMU8K_CHANNEL_SMLD) return Emu8kSoundMemoryRead(state, EMU8K_RIGHT);
            if (channel == EMU8K_CHANNEL_WC) return Emu8kGetWallClock(state);
            return state->Data2Register1[channel];
        case EMU8K_DATA2_INIT2: return state->EffectsInit[EMU8K_INIT2][channel];
        case EMU8K_DATA2_INIT4: return state->EffectsInit[EMU8K_INIT4][channel];
        case EMU8K_DATA2_ATKHLDV: return voice->Atkhldv;
        case EMU8K_DATA2_LFO1VAL: return voice->Lfo1val;
        case EMU8K_DATA2_ATKHLD: return voice->Atkhld;
        case EMU8K_DATA2_LFO2VAL: return voice->Lfo2val;
        default: return 0;
        }
    }
    switch (registerNumber) {
    case EMU8K_DATA3_IP: return voice->Ip;
    case EMU8K_DATA3_IFATN: return voice->Ifatn;
    case EMU8K_DATA3_PEFE: return voice->Pefe;
    case EMU8K_DATA3_FMMOD: return voice->Fmmod;
    case EMU8K_DATA3_TREMFRQ: return voice->Tremfrq;
    case EMU8K_DATA3_FM2FRQ2: return voice->Fm2frq2;
    case EMU8K_DATA3_R6: return voice->Data3Register6;
    default: return voice->Data3Register7;
    }
}

/* ---- ports (§2) ------------------------------------------------------------------- */

/* The port offsets from the base E (§2). */
#define EMU8K_PORT_DATA0_LOW        0x000
#define EMU8K_PORT_DATA0_HIGH       0x002
#define EMU8K_PORT_DATA1_LOW        0x400
#define EMU8K_PORT_DATA1_HIGH_DATA2 0x402
#define EMU8K_PORT_DATA3            0x800
#define EMU8K_PORT_POINTER          0x802
#define EMU8K_POINTER_READ_MASK     0x00FF  /* p.5: the MS byte reads "random"; we answer 0 */
#define EMU8K_FLOATING_BUS          0xFFFF  /* an unclaimed offset                        */

/* A word transfer at offset `portOffset` from the base (000h/002h, 400h/402h, 800h/802h). */
static VOID Emu8kWordOut(PEMU8K_STATE state, WORD portOffset, WORD value)
{
    BYTE registerNumber = (BYTE)((state->Pointer >> EMU8K_POINTER_REGISTER_SHIFT) & EMU8K_POINTER_REGISTER_MASK), channel = (BYTE)(state->Pointer & EMU8K_POINTER_CHANNEL_MASK);
    switch (portOffset) {
    case EMU8K_PORT_DATA0_LOW: Emu8kDataWrite(state, EMU8K_DATA0, EMU8K_LOW_HALF, value); break;
    case EMU8K_PORT_DATA0_HIGH: Emu8kDataWrite(state, EMU8K_DATA0, EMU8K_HIGH_HALF, value); break;
    case EMU8K_PORT_DATA1_LOW: Emu8kDataWrite(state, EMU8K_DATA1, EMU8K_LOW_HALF, value); break;
    case EMU8K_PORT_DATA1_HIGH_DATA2: if (Emu8kIsData1DoubleWord(registerNumber, channel)) Emu8kDataWrite(state, EMU8K_DATA1, EMU8K_HIGH_HALF, value);
                else                             Emu8kDataWrite(state, EMU8K_DATA2, EMU8K_LOW_HALF, value);
                break;
    case EMU8K_PORT_DATA3: Emu8kDataWrite(state, EMU8K_DATA3, EMU8K_LOW_HALF, value); break;
    case EMU8K_PORT_POINTER: state->Pointer = value; break;
    default: break;
    }
}
static WORD Emu8kWordIn(PEMU8K_STATE state, WORD portOffset)
{
    BYTE registerNumber = (BYTE)((state->Pointer >> EMU8K_POINTER_REGISTER_SHIFT) & EMU8K_POINTER_REGISTER_MASK), channel = (BYTE)(state->Pointer & EMU8K_POINTER_CHANNEL_MASK);
    switch (portOffset) {
    case EMU8K_PORT_DATA0_LOW: return Emu8kDataRead(state, EMU8K_DATA0, EMU8K_LOW_HALF);
    case EMU8K_PORT_DATA0_HIGH: return Emu8kDataRead(state, EMU8K_DATA0, EMU8K_HIGH_HALF);
    case EMU8K_PORT_DATA1_LOW: return Emu8kDataRead(state, EMU8K_DATA1, EMU8K_LOW_HALF);
    case EMU8K_PORT_DATA1_HIGH_DATA2: return Emu8kIsData1DoubleWord(registerNumber, channel) ? Emu8kDataRead(state, EMU8K_DATA1, EMU8K_HIGH_HALF) : Emu8kDataRead(state, EMU8K_DATA2, EMU8K_LOW_HALF);
    case EMU8K_PORT_DATA3: return Emu8kDataRead(state, EMU8K_DATA3, EMU8K_LOW_HALF);
    /* p.5: the MS 8 bits of a Pointer read are "random (actually a VLSI test register)" --
       we answer zero there, deterministically. */
    case EMU8K_PORT_POINTER: return (WORD)(state->Pointer & EMU8K_POINTER_READ_MASK);
    default:    return EMU8K_FLOATING_BUS;
    }
}

/* The bus's access widths, and how a byte access finds its port group (offsets 000h,
   400h, 800h: bits 11-10). The bus's handler types still use its own integer names
   (VDD_BUS has not migrated -- #333), so the 32-bit value here is UINT32, the same type. */
#define EMU8K_DOUBLEWORD_ACCESS     4
#define EMU8K_WORD_ACCESS           2
#define EMU8K_HIGH_WORD_PORT        2       /* a doubleword's MS word is two ports up     */
#define EMU8K_ODD_PORT              1
#define EMU8K_PORT_GROUP_SHIFT      10

static VOID Emu8kPortOut(PVOID context, WORD port, BYTE accessWidth, UINT32 value)
{
    PEMU8K_STATE state = (PEMU8K_STATE)context;
    WORD portOffset = (WORD)(port - state->BasePort);
    state->IoWrites++;
    if (accessWidth >= EMU8K_DOUBLEWORD_ACCESS) {    /* a doubleword: LS word, then MS    */
        Emu8kWordOut(state, portOffset, (WORD)value);
        Emu8kWordOut(state, (WORD)(portOffset + EMU8K_HIGH_WORD_PORT), (WORD)(value >> EMU8K_HIGH_WORD_SHIFT));
    } else if (accessWidth == EMU8K_WORD_ACCESS) {
        Emu8kWordOut(state, portOffset, (WORD)value);
    } else {
        /* §2: "no byte I/O transactions are allowed". The guide does not say what a byte
           does; we latch the even byte and complete the word on the odd one, so a program
           that splits a word into two OUTs still lands it. */
        state->ByteIoCount++;
        if (portOffset & EMU8K_ODD_PORT) Emu8kWordOut(state, (WORD)(portOffset - EMU8K_ODD_PORT),
                                  (WORD)(state->ByteLatch[portOffset >> EMU8K_PORT_GROUP_SHIFT] | ((value & BYTE_MASK) << EMU8K_HIGH_BYTE_SHIFT)));
        else state->ByteLatch[portOffset >> EMU8K_PORT_GROUP_SHIFT] = (BYTE)value;
    }
}

static VOID Emu8kPortIn(PVOID context, WORD port, BYTE accessWidth, UINT32 *value)
{
    PEMU8K_STATE state = (PEMU8K_STATE)context;
    WORD portOffset = (WORD)(port - state->BasePort);
    state->IoReads++;
    if (accessWidth >= EMU8K_DOUBLEWORD_ACCESS) {
        UINT32 lowWord = Emu8kWordIn(state, portOffset);
        *value = lowWord | ((UINT32)Emu8kWordIn(state, (WORD)(portOffset + EMU8K_HIGH_WORD_PORT)) << EMU8K_HIGH_WORD_SHIFT);
    } else if (accessWidth == EMU8K_WORD_ACCESS) {
        *value = Emu8kWordIn(state, portOffset);
    } else {
        /* a byte read of the even port reads the word; the odd port gives its high half */
        state->ByteIoCount++;
        if (portOffset & EMU8K_ODD_PORT) *value = state->ByteLatch[portOffset >> EMU8K_PORT_GROUP_SHIFT];
        else { WORD wordRead = Emu8kWordIn(state, portOffset); state->ByteLatch[portOffset >> EMU8K_PORT_GROUP_SHIFT] = (BYTE)(wordRead >> EMU8K_HIGH_BYTE_SHIFT); *value = wordRead & BYTE_MASK; }
    }
}

/* ---- the render ------------------------------------------------------------------------- */

#define EMU8K_HWCF3_AUDIO_ENABLE    0x0004  /* §4: HWCF3 enables audio output              */
#define EMU8K_INTERPOLATOR_TAP0     1       /* p.10: CA is one word below the audio; the   */
#define EMU8K_INTERPOLATOR_TAP1     2       /* interpolator reads CA+1 and CA+2            */
#define EMU8K_CCCA_Q_SHIFT          28      /* CCCA bits 31-28: the filter's Q             */
#define EMU8K_GAIN_SHIFT            8       /* the pan gains are Q8                        */
#define EMU8K_CP_TO_STEP_SHIFT      2       /* CP 4000h = one word: the step is CP x 4     */
#define EMU8K_CCCA_CONTROL_MASK     0xFF000000u
#define EMU8K_SAMPLE_MAX            32767
#define EMU8K_SAMPLE_MIN            (-32768)

VOID VddEmu8kRenderStereo(PEMU8K_STATE state, PINT16 output, DWORD frameCount)
{
    DWORD frame, voiceIndex;
    INT isAudible = (state->Hwcf3 & EMU8K_HWCF3_AUDIO_ENABLE) != 0;  /* §4: HWCF3 enables audio output  */
    state->Renders++;
    for (frame = 0; frame < frameCount; ++frame) {
        INT32 accumulatorLeft = 0, accumulatorRight = 0, sampleLeft, sampleRight;
        if (state->TickPosition == 0)
            for (voiceIndex = 0; voiceIndex < EMU8K_VOICES; ++voiceIndex) Emu8kVoiceTick(&state->Voices[voiceIndex]);
        state->TickPosition = (state->TickPosition + 1) % EMU8K_TICK;
        for (voiceIndex = 0; voiceIndex < EMU8K_VOICES; ++voiceIndex) {
            PEMU8K_VOICE voice = &state->Voices[voiceIndex];
            DWORD currentAddress, addressFraction, loopStart, loopEnd, step;
            INT32 gain;
            if (voice->Ccca & EMU8K_CCCA_DMA) continue;     /* a DMA channel makes no sound       */
            /* current volume slews to its target (clamped: the step is a rounded division) */
            if (voice->CurrentVolumeStep) {
                INT32 target = (INT32)(voice->Vtft >> EMU8K_HIGH_WORD_SHIFT) << EMU8K_CURRENT_VOLUME_SHIFT;
                voice->CurrentVolume += voice->CurrentVolumeStep;
                if ((voice->CurrentVolumeStep > 0 && voice->CurrentVolume > target) || (voice->CurrentVolumeStep < 0 && voice->CurrentVolume < target)) {
                    voice->CurrentVolume = target; voice->CurrentVolumeStep = 0;
                }
            }
            currentAddress = voice->Ccca & EMU8K_ADDR_MASK;
            addressFraction  = voice->Cpf & WORD_MASK_U;
            gain  = voice->CurrentVolume >> EMU8K_CURRENT_VOLUME_SHIFT;
            if (gain > 0 && isAudible) {
                /* CA is "one word lower than the actual audio location" (p.10): the
                   interpolator reads CA+1 and CA+2, weighted by the fraction. */
                INT32 sample0 = Emu8kReadMemory(state, currentAddress + EMU8K_INTERPOLATOR_TAP0), sample1 = Emu8kReadMemory(state, currentAddress + EMU8K_INTERPOLATOR_TAP1), sample;
                Emu8kFilterSetup(voice, (WORD)voice->Cvcf, (BYTE)(voice->Ccca >> EMU8K_CCCA_Q_SHIFT));
                sample = sample0 + (INT32)(((INT64)(sample1 - sample0) * (INT32)addressFraction) >> EMU8K_Q16_SHIFT);
                sample = Emu8kFilterRun(voice, sample);
                sample = (INT32)(((INT64)sample * gain) >> EMU8K_Q16_SHIFT);
                accumulatorLeft += (sample * voice->GainLeft) >> EMU8K_GAIN_SHIFT;
                accumulatorRight += (sample * voice->GainRight) >> EMU8K_GAIN_SHIFT;
            }
            /* advance: CP 4000h = one word per sample (p.7), so the step in 1/65536 words
               is CP × 4. Then ALWAYS loop (§5): passing CSL returns to PSST. */
            step = addressFraction + ((voice->Cpf >> EMU8K_HIGH_WORD_SHIFT) << EMU8K_CP_TO_STEP_SHIFT);
            currentAddress = (currentAddress + (step >> EMU8K_Q16_SHIFT)) & EMU8K_ADDR_MASK;
            loopStart = voice->Psst & EMU8K_ADDR_MASK;
            loopEnd = voice->Csl & EMU8K_ADDR_MASK;
            if (loopEnd > loopStart && currentAddress >= loopEnd) currentAddress = loopStart + (currentAddress - loopEnd) % (loopEnd - loopStart);
            voice->Ccca = (voice->Ccca & EMU8K_CCCA_CONTROL_MASK) | currentAddress;
            voice->Cpf  = (voice->Cpf & HIGH_WORD_MASK_U) | (step & WORD_MASK_U);
        }
        sampleLeft = accumulatorLeft > EMU8K_SAMPLE_MAX ? EMU8K_SAMPLE_MAX : accumulatorLeft < EMU8K_SAMPLE_MIN ? EMU8K_SAMPLE_MIN : accumulatorLeft;
        sampleRight = accumulatorRight > EMU8K_SAMPLE_MAX ? EMU8K_SAMPLE_MAX : accumulatorRight < EMU8K_SAMPLE_MIN ? EMU8K_SAMPLE_MIN : accumulatorRight;
        output[EMU8K_STEREO_SIDES * frame] = (INT16)sampleLeft; output[EMU8K_STEREO_SIDES * frame + EMU8K_RIGHT] = (INT16)sampleRight;
        if (sampleLeft || sampleRight) {
            DWORD magnitudeLeft = (DWORD)(sampleLeft < 0 ? -sampleLeft : sampleLeft), magnitudeRight = (DWORD)(sampleRight < 0 ? -sampleRight : sampleRight);
            state->NonZeroSamplesOut++;
            if (magnitudeLeft > state->PeakSampleOut) state->PeakSampleOut = magnitudeLeft;
            if (magnitudeRight > state->PeakSampleOut) state->PeakSampleOut = magnitudeRight;
        }
        state->WallClock++;
    }
    state->SamplesOut += frameCount;
}

/* ---- the bus ------------------------------------------------------------------------------ */

/* ── THE RESET STATE IS THE INITIALISED CHIP, NOT POWER-UP NOISE. §4: at power-up "most
     registers contain random data" and HWCF3's audio-enable is clear; a real machine ran
     AWEUTIL /S from AUTOEXEC.BAT to run the §4 procedure before any game started, and most
     games rely on that having happened. We have no AUTOEXEC to run it from, so reset leaves
     the chip exactly as §4 leaves it: every channel's engine off and silent, the sound-memory
     addresses zero, HWCF1/2/3 = 0059h/0020h/0004h and HWCF4/5/6 = 0/83h/8000h. The INIT
     arrays hold zero -- they only program the reverb/chorus/EQ effects, which are not
     modelled. A guest that runs the §4 procedure itself gets the same state again. */
#define EMU8K_RESET_DCYSUSV         0x0080  /* §4 step 1: engine off                       */
#define EMU8K_GAIN_CENTRE           128     /* half of Q8 unity: both sides equal         */
#define EMU8K_RESET_HWCF1           0x0059
#define EMU8K_RESET_HWCF2           0x0020
#define EMU8K_RESET_HWCF3           0x0004
#define EMU8K_RESET_HWCF4           0
#define EMU8K_RESET_HWCF5           0x83
#define EMU8K_RESET_HWCF6           0x8000

VOID VddEmu8kReset(PVOID context)
{
    PEMU8K_STATE state = (PEMU8K_STATE)context;
    UINT index;
    PBYTE bytes;
    Emu8kBuildTables();
    bytes = (PBYTE)state->Voices;
    for (index = 0; index < sizeof state->Voices; ++index) bytes[index] = 0;
    for (index = 0; index < EMU8K_VOICES; ++index) {
        state->Voices[index].Dcysusv = EMU8K_RESET_DCYSUSV;                   /* §4 step 1: engine off             */
        state->Voices[index].GainLeft = state->Voices[index].GainRight = EMU8K_GAIN_CENTRE;
    }
    bytes = (PBYTE)state->EffectsInit;
    for (index = 0; index < sizeof state->EffectsInit; ++index) bytes[index] = 0;
    for (index = 0; index < EMU8K_VOICES; ++index) { state->Data1Register1[index] = 0; state->Data2Register1[index] = 0; }
    for (index = 0; index < EMU8K_STREAMS; ++index) state->SoundMemoryAddress[index] = 0;
    state->SoundMemoryReadLatch[EMU8K_LEFT] = state->SoundMemoryReadLatch[EMU8K_RIGHT] = state->SoundMemoryWriteLatch[EMU8K_LEFT] = state->SoundMemoryWriteLatch[EMU8K_RIGHT] = 0;
    state->SoundMemoryEmpty[EMU8K_LEFT] = state->SoundMemoryEmpty[EMU8K_RIGHT] = state->SoundMemoryFull[EMU8K_LEFT] = state->SoundMemoryFull[EMU8K_RIGHT] = 0;
    state->Hwcf1 = EMU8K_RESET_HWCF1; state->Hwcf2 = EMU8K_RESET_HWCF2; state->Hwcf3 = EMU8K_RESET_HWCF3;
    state->Hwcf4 = EMU8K_RESET_HWCF4; state->Hwcf5 = EMU8K_RESET_HWCF5; state->Hwcf6 = EMU8K_RESET_HWCF6;
    state->Pointer = 0; state->ByteLatch[EMU8K_DATA0] = state->ByteLatch[EMU8K_DATA1] = state->ByteLatch[EMU8K_DATA2] = 0;
    state->TickPosition = 0;
    /* WC is NOT reset: p.13 "there is no mechanism to reset this counter". */
}

/* §2: three groups of four ports, at E, E+400h and E+800h. */
#define EMU8K_GROUP_LAST_PORT       3
#define EMU8K_DATA1_GROUP           0x400
#define EMU8K_DATA1_GROUP_LAST      0x403
#define EMU8K_DATA3_GROUP           0x800
#define EMU8K_DATA3_GROUP_LAST      0x803
#define EMU8K_INITIALIZE_OK         0       /* the bus's init contract: 0 = ok            */
#define EMU8K_INITIALIZE_FAILED     (-1)

INT VddEmu8kInitialize(VDD_BUS *bus, PVOID context)
{
    PEMU8K_STATE state = (PEMU8K_STATE)context;
    state->Bus = bus;
    if (!state->BasePort) state->BasePort = EMU8K_DEFAULT_BASE;
    VddEmu8kReset(state);
    /* §2: three groups of four ports. */
    if (VddClaimPorts(bus, state->BasePort, (WORD)(state->BasePort + EMU8K_GROUP_LAST_PORT), Emu8kPortIn, Emu8kPortOut, state)) return EMU8K_INITIALIZE_FAILED;
    if (VddClaimPorts(bus, (WORD)(state->BasePort + EMU8K_DATA1_GROUP), (WORD)(state->BasePort + EMU8K_DATA1_GROUP_LAST), Emu8kPortIn, Emu8kPortOut, state)) return EMU8K_INITIALIZE_FAILED;
    if (VddClaimPorts(bus, (WORD)(state->BasePort + EMU8K_DATA3_GROUP), (WORD)(state->BasePort + EMU8K_DATA3_GROUP_LAST), Emu8kPortIn, Emu8kPortOut, state)) return EMU8K_INITIALIZE_FAILED;
    return EMU8K_INITIALIZE_OK;
}
