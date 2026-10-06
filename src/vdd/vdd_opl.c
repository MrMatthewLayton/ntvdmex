/* vdd_opl.c -- see vdd_opl.h.  AdLib / OPL2 (YM3812) and OPL3 (YMF262) register
 * file and timers, on the VDD bus.  Pure C, no <windows.h>. */
#include "vdd_opl.h"

/* The OPL's 18 operators are addressed by a register offset that deliberately
   skips 0x06/0x07, 0x0E/0x0F: offsets are 0x00-0x05, 0x08-0x0D, 0x10-0x15, in
   three banks of six. Channel n's two operators are the n-th slot of a bank and
   the one three slots later, so channel 3 is offsets 0x08 and 0x0B -- not 0x06
   and 0x09. Getting this wrong detunes half the channels in a way that sounds
   almost right, so both directions live here and nowhere else.
   The OPL3's array 1 is the same layout again: channel 9+n is array 1's channel
   n, and its operators are 18 + (what channel n's would be). */
INT VddOplOperatorIndex(INT channel, INT isCarrier)
{
    INT arrayIndex, localChannel;
    if (channel < 0 || channel >= OPL3_CHANNELS) return -1;
    arrayIndex = channel / OPL_CHANNELS; localChannel = channel % OPL_CHANNELS;
    return arrayIndex * OPL_OPERATORS + (localChannel / 3) * 6 + (localChannel % 3) + (isCarrier ? 3 : 0);
}

/* NEW (0x105 bit 0) on a fitted OPL3. Everything the OPL3 adds is gated on it. */
INT VddOplIsNewMode(PCOPL_STATE state)
{
    return state->IsOpl3 && (state->Registers[OPL3_REGISTER_NEW] & 1);
}

/* ── 4-OPERATOR PAIRS (register 0x104, OPL3 with NEW set). Six bits, six pairs,
     and the pairs are fixed by the chip, not chosen: bit 0 joins channels 0 and 3,
     bit 1 joins 1+4, bit 2 joins 2+5, and bits 3-5 do the same in array 1 (9+12,
     10+13, 11+14). The FIRST channel of a pair owns the voice -- its F-number,
     block, key-on, feedback and output routing drive all four operators; the
     second contributes its two operators and its CNT bit (which, with the first
     channel's CNT, picks one of four algorithms; see vdd_opl_synth.c) and nothing
     else. Returns 1 for the first channel of a live pair, 2 for the second, 0 for
     a channel that is an ordinary two-operator voice. */
INT OplFourOperatorRole(PCOPL_STATE state, INT channel)
{
    INT localChannel, bit, role;
    if (!VddOplIsNewMode(state) || channel < 0 || channel >= OPL3_CHANNELS) return 0;
    localChannel = channel % OPL_CHANNELS;
    if (localChannel < 3)      { bit = localChannel;     role = 1; }
    else if (localChannel < 6) { bit = localChannel - 3; role = 2; }
    else return 0;
    if (channel >= OPL_CHANNELS) bit += 3;
    return ((state->Registers[OPL3_REGISTER_FOUR_OPERATOR] >> bit) & 1) ? role : 0;
}

/* register offset (low 5 bits of a 0x20/0x40/0x60/0x80/0xE0 register) -> operator,
   or -1 for the gaps. */
static INT OplOffsetToOperator(BYTE registerNumber)
{
    INT offset = registerNumber & 0x1F, bank = offset >> 3, slot = offset & 7;
    if (slot >= 6 || bank >= 3) return -1;
    return bank * 6 + slot;
}

/* --- timers --------------------------------------------------------------- */
/* Each timer counts UP from its preset; overflowing past 255 raises its status
   flag (unless masked) and reloads the preset, so the period is
   (256 - preset) * resolution. AdLib detection is exactly this measurement:
   preset 0xFF gives one 80us tick, which is why a detect that sees no flag
   concludes there is no card. */
/* nosb.flag: no FM chip fitted. Kept as its own flag rather than reading the Sound
   Blaster's, because the off-VM suites link these two VDDs SEPARATELY -- referencing
   vdd_sb.c's copy from here broke `opl_synth_test` with an undefined symbol and cost
   half the gate (580 checks -> 244) until run.sh caught it. */
INT g_OplAbsent = 0;

static VOID OplTimerStep(WORD *count, BYTE preset, BYTE mask,
                           BYTE flag, BYTE *status)
{
    if (++(*count) > 0xFF) {
        *count = preset;
        if (!mask) *status |= (BYTE)(flag | OPL_STATUS_IRQ);
    }
}

VOID VddOplAddMicroseconds(POPL_STATE state, UINT32 microseconds)
{
    if (state->IsTimer1Running) {
        state->Timer1FractionUs += microseconds;
        while (state->Timer1FractionUs >= OPL_TIMER1_US) {
            state->Timer1FractionUs -= OPL_TIMER1_US;
            OplTimerStep(&state->Timer1Count, state->Timer1Preset, state->IsTimer1Masked, OPL_STATUS_TIMER1, &state->Status);
        }
    }
    if (state->IsTimer2Running) {
        state->Timer2FractionUs += microseconds;
        while (state->Timer2FractionUs >= OPL_TIMER2_US) {
            state->Timer2FractionUs -= OPL_TIMER2_US;
            OplTimerStep(&state->Timer2Count, state->Timer2Preset, state->IsTimer2Masked, OPL_STATUS_TIMER2, &state->Status);
        }
    }
}

/* --- register file -------------------------------------------------------- */
/* Key one OPERATOR, rather than a channel. Rhythm mode needs this: four of the five
   percussion voices are single operators keyed independently from 0xBD, so the
   channel-wide key-on the melodic path uses cannot express them. */
static VOID OplKeyOperator(POPL_STATE state, INT operatorIndex, INT isOn)
{
    if (isOn) {
        state->Operators[operatorIndex].EnvelopeState = OPL_ENVELOPE_ATTACK;
        state->Operators[operatorIndex].Phase = 0;
    } else if (state->Operators[operatorIndex].EnvelopeState != OPL_ENVELOPE_OFF) {
        state->Operators[operatorIndex].EnvelopeState = OPL_ENVELOPE_RELEASE;
    }
}

/* 0xBD's low five bits key the percussion voices. MEASURED, not assumed -- each
   operator was silenced in turn and the drum that went quiet named the owner
   (tools/oplref/oplprobe.c, experiment M):
       bit 0 hi-hat -> op13      bit 1 cymbal -> op17     bit 2 tom-tom -> op14
       bit 3 snare  -> op16      bit 4 bass drum -> channel 6, BOTH operators
   The bass drum is an ordinary two-operator FM voice; the other four are single
   operators heard directly.

   ── A DRUM BIT AND ITS CHANNEL'S OWN KEY BIT ARE OR'D (#139). MEASURED
      (`oplprobe keyor`): with channel 8's B0 key already on, setting the tom-tom
      bit leaves the reference's output bit-identical -- no restart, because the
      operator was already keyed -- and a channel 6-8 key bit written DURING
      rhythm mode still keys that channel's operators, which then sound as the
      drums they now are. So each of operators 12-17 is keyed while (its channel's
      key bit) OR (its drum bit, in rhythm mode) is set, and restarts only on a
      rising edge of that OR. This once ignored channel keys in rhythm mode and
      re-keyed on every drum bit, which restarted drums that were already sounding
      -- harmless while three of them were silent, audible once they were not. */
static const BYTE g_OplRhythmBit[6] = { 0x10, 0x01, 0x04, 0x10, 0x08, 0x02 };  /* op12-17 */

static INT OplRhythmHeld(INT operatorIndex, BYTE bd, BYTE channelKeys)
{
    INT channel = (operatorIndex - 12) % 3;         /* op12,15->ch6 13,16->7 14,17->8 */
    return ((channelKeys >> channel) & 1) || ((bd & OPL_BD_RHYTHM) && (bd & g_OplRhythmBit[operatorIndex - 12]));
}

static BYTE OplRhythmChannelKeys(PCOPL_STATE state)
{
    return (BYTE)(state->Channels[6].IsKeyOn | (state->Channels[7].IsKeyOn << 1) | (state->Channels[8].IsKeyOn << 2));
}

/* Re-key operators 12-17 for a change of 0xBD and/or of channels 6-8's key bits
   (`ck` bit n = channel 6+n). Only an edge of the OR moves an envelope. */
static VOID OplRhythmRekey(POPL_STATE state, BYTE bdBefore, BYTE channelKeysBefore, BYTE bdAfter, BYTE channelKeysAfter)
{
    INT operatorIndex;
    for (operatorIndex = 12; operatorIndex < 18; ++operatorIndex) {
        INT wasHeld = OplRhythmHeld(operatorIndex, bdBefore, channelKeysBefore), isHeldNow = OplRhythmHeld(operatorIndex, bdAfter, channelKeysAfter);
        if (wasHeld == isHeldNow) continue;
        OplKeyOperator(state, operatorIndex, isHeldNow);
        /* Hi-hat / cymbal accumulator restarted in a running chip: each feeds the
           other's phase bit, so the restart POINT matters -- measured, see
           opl_rhythm_sample (vdd_opl_synth.c). */
        if (isHeldNow && (operatorIndex == 13 || operatorIndex == 17) && (bdAfter & OPL_BD_RHYTHM) && state->LfoCount)
            state->RhythmRestart |= (BYTE)(operatorIndex == 13 ? 1 : 2);
    }
}

static VOID OplRhythmWrite(POPL_STATE state, BYTE oldValue, BYTE value)
{
    BYTE channelKeys = OplRhythmChannelKeys(state);
    INT bit;
    /* The hit counters count DRUM-BIT rising edges, as they always have: bits 0-4
       are hi-hat, cymbal, tom-tom, snare, bass drum, ProfileRhythmHits' order. */
    if (value & OPL_BD_RHYTHM) {
        BYTE wasSet = (oldValue & OPL_BD_RHYTHM) ? oldValue : 0;     /* entering: every set bit is new */
        for (bit = 0; bit < 5; ++bit)
            if (((value & ~wasSet) >> bit) & 1) state->ProfileRhythmHits[bit]++;
    }
    OplRhythmRekey(state, oldValue, channelKeys, value, channelKeys);
}

/* Key-on / key-off edge for channel `c` -- both its operators, or all four when it
   leads a 4-operator pair (the pair is ONE voice, keyed from the first channel). */
static VOID OplKeyChannel(POPL_STATE state, INT channel, INT isKeyOn)
{
    INT channelCount = (OplFourOperatorRole(state, channel) == 1) ? 2 : 1, pairIndex;
    if (isKeyOn && !state->Channels[channel].IsKeyOn) { /* key-on edge: restart   */
        INT isAm = 0, isVibrato = 0;
        for (pairIndex = 0; pairIndex < channelCount; ++pairIndex) {
            INT modulator = VddOplOperatorIndex(channel + 3 * pairIndex, 0), carrier = VddOplOperatorIndex(channel + 3 * pairIndex, 1);
            state->Operators[modulator].EnvelopeState = OPL_ENVELOPE_ATTACK; state->Operators[modulator].Phase = 0;
            state->Operators[carrier].EnvelopeState = OPL_ENVELOPE_ATTACK; state->Operators[carrier].Phase = 0;
            isAm  |= state->Operators[modulator].AmplitudeModulation  | state->Operators[carrier].AmplitudeModulation;
            isVibrato |= state->Operators[modulator].Vibrato | state->Operators[carrier].Vibrato;
        }
        state->ProfileKeyOns++;                              /* profile: see OPL_STATE  */
        if (isAm)  state->ProfileKeyOnAm++;
        if (isVibrato) state->ProfileKeyOnVibrato++;
    } else if (!isKeyOn && state->Channels[channel].IsKeyOn) {               /* key-off edge: release  */
        for (pairIndex = 0; pairIndex < channelCount; ++pairIndex) {
            state->Operators[VddOplOperatorIndex(channel + 3 * pairIndex, 0)].EnvelopeState = OPL_ENVELOPE_RELEASE;
            state->Operators[VddOplOperatorIndex(channel + 3 * pairIndex, 1)].EnvelopeState = OPL_ENVELOPE_RELEASE;
        }
    }
    state->Channels[channel].IsKeyOn = (BYTE)isKeyOn;
}

/* ── THE REGISTER FILE, BOTH ARRAYS. `reg` is 9 bits: bit 8 is the array (A1 on
     the address write). Array 1 has the same per-operator and per-channel
     registers as array 0 at the same offsets -- 0x120-0x135 is its AM/VIB/..., 0x1A0
     its F-numbers -- driving operators 18-35 and channels 9-17. What it does NOT
     have is array 0's globals: there are no timers, no 0xBD and no WSE in array 1.
     Its only globals are its own two, 0x104 (4-op pairs) and 0x105 (NEW).
   ⚠ ARRAY 1 LATCHES WITH NEW CLEAR. Every write is decoded into the operator and
     channel state exactly as array 0's is -- a driver commonly programs its voices
     first and sets NEW after -- but nothing in it is RENDERED, keyed into the
     4-op pairing, or routed until NEW is set (vdd_opl_synth.c). */
VOID VddOplWriteRegister(POPL_STATE state, WORD registerNumber, BYTE value)
{
    INT operatorIndex, arrayIndex;
    BYTE arrayRegister, oldValue;
    if (registerNumber >= OPL3_REGISTERS) return;
    arrayIndex = registerNumber >> 8;
    if (arrayIndex && !state->IsOpl3) return;           /* an OPL2 has no array 1 */
    arrayRegister = (BYTE)registerNumber;
    oldValue = state->Registers[registerNumber];                        /* before the store: edge detection */
    state->Registers[registerNumber] = value;
    state->ProfileWrites++;                                  /* profile: see OPL_STATE */
    if (state->Trace && !arrayIndex) state->Trace(arrayRegister, value);         /* dev-only capture hook  */

    if (arrayIndex && arrayRegister < 0x20) return;      /* 0x104/0x105: stored, consulted where used;
                                           0x101-0x103, 0x108: nothing in array 1 */

    if (arrayRegister == 0x02) {                        /* timer 1 preset         */
        state->Timer1Preset = value;
        if (!state->IsTimer1Running) state->Timer1Count = value;
        return;
    }
    if (arrayRegister == 0x03) {                        /* timer 2 preset         */
        state->Timer2Preset = value;
        if (!state->IsTimer2Running) state->Timer2Count = value;
        return;
    }
    if (arrayRegister == 0x04) {                        /* timer control          */
        if (value & OPL_TIMER_CONTROL_IRQ_RESET) {      /* bit 7 resets flags and */
            state->Status = 0;                          /* does nothing else      */
            return;
        }
        state->IsTimer1Masked = (value & OPL_TIMER_CONTROL_T1_MASK) ? 1 : 0;
        state->IsTimer2Masked = (value & OPL_TIMER_CONTROL_T2_MASK) ? 1 : 0;
        { BYTE isTimer1Running = (value & OPL_TIMER_CONTROL_T1_START) ? 1 : 0;
          if (isTimer1Running && !state->IsTimer1Running) { state->Timer1Count = state->Timer1Preset; state->Timer1FractionUs = 0; }
          state->IsTimer1Running = isTimer1Running; }
        { BYTE isTimer2Running = (value & OPL_TIMER_CONTROL_T2_START) ? 1 : 0;
          if (isTimer2Running && !state->IsTimer2Running) { state->Timer2Count = state->Timer2Preset; state->Timer2FractionUs = 0; }
          state->IsTimer2Running = isTimer2Running; }
        return;
    }

    if (arrayRegister >= 0x20 && arrayRegister <= 0x35) {                   /* AM/VIB/EGT/KSR/MULT    */
        operatorIndex = OplOffsetToOperator(arrayRegister); if (operatorIndex < 0) return;
        if (value & 0x80) state->ProfileAmOperators  |= 1u << operatorIndex;      /* profile: see OPL_STATE */
        if (value & 0x40) state->ProfileVibratoOperators |= 1u << operatorIndex;      /* (slot, either array)   */
        operatorIndex += arrayIndex * OPL_OPERATORS;
        state->Operators[operatorIndex].AmplitudeModulation   = (value >> 7) & 1;
        state->Operators[operatorIndex].Vibrato  = (value >> 6) & 1;
        state->Operators[operatorIndex].EnvelopeType  = (value >> 5) & 1;
        state->Operators[operatorIndex].KeyScaleRate  = (value >> 4) & 1;
        state->Operators[operatorIndex].Multiplier = value & 0x0F;
        return;
    }
    if (arrayRegister >= 0x40 && arrayRegister <= 0x55) {                   /* KSL / total level      */
        operatorIndex = OplOffsetToOperator(arrayRegister); if (operatorIndex < 0) return;
        operatorIndex += arrayIndex * OPL_OPERATORS;
        state->Operators[operatorIndex].KeyScaleLevel = (value >> 6) & 3;
        state->Operators[operatorIndex].TotalLevel  = value & 0x3F;
        return;
    }
    if (arrayRegister >= 0x60 && arrayRegister <= 0x75) {                   /* attack / decay         */
        operatorIndex = OplOffsetToOperator(arrayRegister); if (operatorIndex < 0) return;
        operatorIndex += arrayIndex * OPL_OPERATORS;
        state->Operators[operatorIndex].AttackRate = (value >> 4) & 0x0F;
        state->Operators[operatorIndex].DecayRate = value & 0x0F;
        return;
    }
    if (arrayRegister >= 0x80 && arrayRegister <= 0x95) {                   /* sustain / release      */
        operatorIndex = OplOffsetToOperator(arrayRegister); if (operatorIndex < 0) return;
        operatorIndex += arrayIndex * OPL_OPERATORS;
        state->Operators[operatorIndex].SustainLevel = (value >> 4) & 0x0F;
        state->Operators[operatorIndex].ReleaseRate = value & 0x0F;
        return;
    }
    if (arrayRegister >= 0xE0 && arrayRegister <= 0xF5) {                   /* waveform select        */
        operatorIndex = OplOffsetToOperator(arrayRegister); if (operatorIndex < 0) return;
        operatorIndex += arrayIndex * OPL_OPERATORS;
        /* All three bits kept; which of them COUNT is the synth's call, because
           it depends on NEW / WSE as they stand when the note plays. */
        state->Operators[operatorIndex].Waveform = value & 7;
        state->ProfileWaveMask |= (BYTE)(1u << (value & 7));
        return;
    }

    if (arrayRegister >= 0xA0 && arrayRegister <= 0xA8) {                   /* F-number low           */
        INT channel = arrayRegister - 0xA0 + arrayIndex * OPL_CHANNELS;
        state->Channels[channel].FNumber = (WORD)((state->Channels[channel].FNumber & 0x300) | value);
        return;
    }
    if (arrayRegister >= 0xB0 && arrayRegister <= 0xB8) {                   /* key-on / block / F hi  */
        INT channel = arrayRegister - 0xB0 + arrayIndex * OPL_CHANNELS, isKeyOn = (value >> 5) & 1;
        state->Channels[channel].FNumber  = (WORD)((state->Channels[channel].FNumber & 0xFF) | ((value & 3) << 8));
        state->Channels[channel].Block = (value >> 2) & 7;
        /* In rhythm mode channels 6-8 ARE the percussion voices, keyed from 0xBD.
           The F-number and block above still apply -- that is how a driver tunes
           the drums -- and the key bit is OR'd with the drum bits rather than
           driving a melodic voice. Array 0 only: rhythm mode has no counterpart
           in array 1. */
        if (channel >= 6 && channel <= 8 && (state->Registers[0xBD] & OPL_BD_RHYTHM)) {
            /* ...but the key bit still counts, OR'd with the drum bits (see
               OplRhythmRekey): it keys this channel's operators as drums. */
            BYTE channelKeysBefore = OplRhythmChannelKeys(state);
            state->Channels[channel].IsKeyOn = (BYTE)isKeyOn;
            OplRhythmRekey(state, state->Registers[0xBD], channelKeysBefore, state->Registers[0xBD], OplRhythmChannelKeys(state));
            return;
        }
        /* The second channel of a 4-op pair has no key of its own: its F-number
           and block are latched above (and ignored), its key-on bit is ignored. */
        if (OplFourOperatorRole(state, channel) == 2) return;
        OplKeyChannel(state, channel, isKeyOn);
        return;
    }
    if (arrayRegister >= 0xC0 && arrayRegister <= 0xC8) {                   /* feedback / connection  */
        INT channel = arrayRegister - 0xC0 + arrayIndex * OPL_CHANNELS;
        state->Channels[channel].Feedback  = (value >> 1) & 7;
        state->Channels[channel].Connection = value & 1;
        /* bits 4-7 (output routing, OPL3) are read from Registers[] by the synth */
        return;
    }
    if (arrayIndex) return;             /* 0x1BD etc.: array 1 has no such globals */
    /* 0x01 (test/WSE), 0x08 (CSM/NTS), 0xBD (rhythm/depth) are stored in Registers[]
       and consulted by the synth; nothing to decode here. */
    if (arrayRegister == 0xBD) {
        OplRhythmWrite(state, oldValue, value);
        state->ProfileBdWrites++; state->ProfileBdOr |= value;
    }
    if (arrayRegister == 0x01 && (value & 0x20)) state->ProfileWaveformSelect = 1;
}

/* --- ports 0x388-0x38B ------------------------------------------------------ */
/* The chip has two address lines. A0 picks address (0) or data (1); A1 picks the
   register ARRAY for an address write -- which is why an OPL3 is four ports, and
   why the data port does not care which of 0x389/0x38B it is: there is one 9-bit
   latch, and a data write goes wherever it points. An OPL2 has no A1 at all
   (an AdLib decodes 0x388/0x389 only), so for it 0x38A/0x38B are not there. */
VOID VddOplWriteAddress(POPL_STATE state, INT arrayIndex, BYTE value)
{
    if (arrayIndex && !state->IsOpl3) return;           /* no array 1 on an OPL2  */
    state->AddressLatch = (WORD)((arrayIndex ? 0x100 : 0) | value);
}

VOID VddOplWriteData(POPL_STATE state, BYTE value)
{
    VddOplWriteRegister(state, state->AddressLatch, value);
}

/* ── THE STATUS BYTE, AND WHAT THE OPL3 CHANGES IN IT. Bits 7-5 are IRQ, T1, T2
     on both chips -- the AdLib detect (reset, read 0x00, run T1, read 0xC0) masks
     with 0xE0 and passes identically on either. Bits 2-1 are the difference: a
     YM3812 reads them as 1, a YMF262 as 0, and that is the OPL3 detect -- an
     OPL2 idles at 0x06 and reads 0xC6 after the timer test, an OPL3 at 0x00 and
     0xC0. Before #232 the model read 0x00 while being an OPL2, i.e. it told every
     OPL3 detect that array 1 was there when it was not.
   ► `nosb.flag` ALSO UNFITS THE OPL. The AdLib detect is a status-register
     dance (mask/reset the timers, read 0xC0, run timer 1, read 0x80) -- a
     machine with no FM chip floats the bus and reads 0xFF, which fails it.
     Doom's DMX uses ADLIB for music even with no Sound Blaster DSP, so
     withholding only the DSP's 0xAA left music running and the run still died
     in the timer ISR. One knob, no sound devices at all: that is the point of
     the knob, which is to find out what Doom does with NO music rather than to
     ship a machine without an OPL. */
BYTE VddOplReadStatus(PCOPL_STATE state)
{
    if (g_OplAbsent) return 0xFF;
    return (BYTE)(state->Status | (state->IsOpl3 ? 0 : OPL_STATUS_OPL2_ID));
}

static VOID OplPortOut(PVOID context, WORD port, BYTE width, UINT32 value)
{
    POPL_STATE state = (POPL_STATE)context;
    INT isArray1 = (port & 2) ? 1 : 0;
    (VOID)width;
    if (isArray1 && !state->IsOpl3) return;             /* OPL2: not decoded      */
    if ((port & 1) == 0) VddOplWriteAddress(state, isArray1, (BYTE)value);  /* 0x388/0x38A  */
    else                 VddOplWriteData(state, (BYTE)value);     /* 0x389/0x38B  */
}

static VOID OplPortIn(PVOID context, WORD port, BYTE width, UINT32 *value)
{
    POPL_STATE state = (POPL_STATE)context;
    (VOID)width;
    /* The data ports are write-only on both chips. 0x38A on an OPL3 reads status
       too: the datasheet's read cycle is specified with A0 low and says nothing
       of A1, and a chip that does not latch on a read has no reason to decode it
       -- ⚠ an INFERENCE, owed a check against a real SB16. On an OPL2 0x38A/0x38B
       float, exactly as on an AdLib, whose decode stops at 0x389. */
    if ((port & 1) || ((port & 2) && !state->IsOpl3)) { *value = 0xFF; return; }
    *value = VddOplReadStatus(state);
}

/* Every voice to its release, both arrays: the key-off a program that ended
   never sent. The F-number stays, so each note decays through its own envelope
   rather than clicking off. */
VOID VddOplAllNotesOff(POPL_STATE state)
{
    INT channel;
    for (channel = 0; channel < OPL3_CHANNELS; ++channel) {
        WORD registerNumber = (WORD)((channel / OPL_CHANNELS) * 0x100 + 0xB0 + channel % OPL_CHANNELS);
        if (channel >= OPL_CHANNELS && !state->IsOpl3) break;
        if (state->Registers[registerNumber] & 0x20) VddOplWriteRegister(state, registerNumber, (BYTE)(state->Registers[registerNumber] & ~0x20));
    }
    if (state->Registers[0xBD] & 0x1F)          /* rhythm drums, keyed separately */
        VddOplWriteRegister(state, 0xBD, (BYTE)(state->Registers[0xBD] & ~0x1F));
}

static VOID OplFrame(PVOID context)
{
    POPL_STATE state = (POPL_STATE)context;
    if (state->IsExternalClock) return; /* host pumps real elapsed time instead */
    VddOplAddMicroseconds(state, state->FrameUs);
}

/* --- lifecycle ------------------------------------------------------------ */
VOID VddOplReset(PVOID context)
{
    POPL_STATE state = (POPL_STATE)context;
    PVDD_BUS bus = state->Bus;
    UINT32 frameUs = state->FrameUs, sampleHz = state->SampleHz;
    BYTE  isExternalClock = state->IsExternalClock, isOpl3 = state->IsOpl3;
    UINT index; BYTE *bytes = (BYTE *)state;
    for (index = 0; index < sizeof(*state); ++index) bytes[index] = 0;
    state->Bus = bus;
    state->FrameUs  = frameUs ? frameUs : OPL_DEFAULT_FRAME_US;
    state->SampleHz = sampleHz ? sampleHz : OPL_DEFAULT_HZ;
    state->IsExternalClock = isExternalClock;
    state->IsOpl3      = isOpl3;       /* the card, not the guest's state: NEW is 0 again */
    state->Noise     = OPL_NOISE_SEED;   /* an all-zero LFSR would never leave zero  */
    /* SILENT MEANS FULLY ATTENUATED, NOT ZERO. Envelope counts attenuation, so zeroing
       the struct leaves every operator at FULL VOLUME waiting for its first note.
       Key-on does not reset Envelope -- measured: the reference resumes an interrupted
       attack from where it was rather than restarting from silence -- so the very
       first note of a run attacked instantly at full level no matter what its
       attack rate said. That reads as "too loud" and as "no attack", and it is
       both: it is why our first measured attack time was 0.00 ms at every rate.  */
    for (index = 0; index < OPL3_OPERATORS; ++index) {
        state->Operators[index].EnvelopeState = OPL_ENVELOPE_OFF;
        state->Operators[index].Envelope = OPL_ENVELOPE_FULL;
    }
}

INT VddOplInitialize(PVDD_BUS bus, PVOID context)
{
    POPL_STATE state = (POPL_STATE)context;
    state->Bus = bus;
    if (!state->FrameUs)  state->FrameUs  = OPL_DEFAULT_FRAME_US;
    if (!state->SampleHz) state->SampleHz = OPL_DEFAULT_HZ;
    /* All four ports, whichever chip: on an OPL2 the top two answer as nothing
       (see OplPortOut/OplPortIn), and the host can then change the chip without
       re-plumbing the bus. */
    if (VddClaimPorts(bus, 0x388, 0x38B, OplPortIn, OplPortOut, state)) return -1;
    if (VddOnFrame(bus, OplFrame, state)) return -1;
    return 0;
}
