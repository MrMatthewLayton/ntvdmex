/* vdd_gus.c -- the Gravis UltraSound (GF1). See vdd_gus.h, docs/ref/gus.md (§n below)
 * and docs/inventory/gus.md.
 *
 * Built from the UltraSound SDK v2.22 (manual Ch. 2 and the SDK's own driver source),
 * not from any application: a device is in because it is in the period hardware
 * contract. heaven7 is the acceptance test, not the specification.
 */
#include "vdd_gus.h"

/* ---- register-file helpers -------------------------------------------------------- */

/* Which registers are 16 bits wide (ref §2.1, §2.2). Everything else is 8 and lives at 3X5. */
static INT GusIsRegister16(BYTE registerNumber)
{
    BYTE voiceRegister;
    if (registerNumber == 0x42 || registerNumber == 0x43) return 1;
    if (registerNumber >= 0x40 && registerNumber < 0x80) return 0;             /* the other globals are 8-bit */
    voiceRegister = (BYTE)(registerNumber & 0x7F);   /* voice regs: write 0xh, read 8xh */
    return voiceRegister == 0x01 || (voiceRegister >= 0x02 && voiceRegister <= 0x05) || voiceRegister == 0x09 || voiceRegister == 0x0A || voiceRegister == 0x0B;
}

/* A position in 1/512-sample units from the (high, low) register pair (ref §2.2):
   high bits 12-0 = address 19-7; low bits 15-9 = address 6-0 and, below that, the
   fraction -- four bits for start/end (8-5), nine for the current position (8-0). */
static UINT32 GusPositionSetHigh(UINT32 position, WORD highWord)
{
    UINT32 address = ((UINT32)(highWord & 0x1FFF) << 7) | ((position >> 9) & 0x7F);
    return (address << 9) | (position & 0x1FF);
}
static UINT32 GusPositionSetLow(UINT32 position, WORD lowWord, INT isNineBitFraction)
{
    UINT32 address = ((position >> 9) & ~0x7Fu) | ((UINT32)(lowWord >> 9) & 0x7F);
    UINT32 fraction = isNineBitFraction ? (lowWord & 0x1FFu) : ((UINT32)(lowWord >> 5) & 0xF) << 5;
    return (address << 9) | fraction;
}
static WORD GusPositionGetHigh(UINT32 position) { return (WORD)((position >> 16) & 0x1FFF); }
static WORD GusPositionGetLow(UINT32 position, INT isNineBitFraction)
{
    WORD lowWord = (WORD)(((position >> 9) & 0x7F) << 9);
    return (WORD)(lowWord | (isNineBitFraction ? (position & 0x1FF) : (position & 0x1E0)));
}

/* ---- the latches (ref §5) ---------------------------------------------------------- */

/* The two 3-bit codes of the 2XB latches. Code 0 is "no line" in both tables. */
static const BYTE g_GusIrqMap[8] = { 0, 2, 5, 3, 7, 11, 12, 15 };
static const BYTE g_GusDmaMap[8] = { 0, 1, 3, 5, 6, 7, 0, 0 };
static BYTE GusCodeOf(const BYTE *map, BYTE line)
{
    BYTE code;
    for (code = 1; code < 8; ++code) if (map[code] && map[code] == line) return code;
    return 0;
}

/* #190: decode the latches into the lines the card drives (ref §5).
     IRQ latch: bits 2-0 GF1, 5-3 MIDI, bit 6 = both on the GF1 line.
     DMA latch: bits 2-0 DRAM, 5-3 record, bit 6 = both on the DRAM channel.
   2X0 bit 4 is the mix register's own "combine the GF1 and MIDI IRQs"; either asks
   for one line. */
static VOID GusLatchDecode(PGUS_STATE state)
{
    state->Gf1IrqLine  = g_GusIrqMap[state->IrqLatch & 7];
    state->MidiIrqLine = ((state->IrqLatch & 0x40) || (state->MixControl & 0x10))
                      ? state->Gf1IrqLine : g_GusIrqMap[(state->IrqLatch >> 3) & 7];
    state->DramDmaLine = g_GusDmaMap[state->DmaLatch & 7];
    state->RecordDmaLine  = (state->DmaLatch & 0x40) ? state->DramDmaLine
                                               : g_GusDmaMap[(state->DmaLatch >> 3) & 7];
}

/* 2X0 bit 3 powers the IRQ and DMA drivers: with it clear the card drives NO line,
   whatever the latches say (ref §5). */
static INT GusAreDriversOn(PCGUS_STATE state) { return (state->MixControl & 0x08) != 0; }
static BYTE GusDramDma(PCGUS_STATE state) { return GusAreDriversOn(state) ? state->DramDmaLine : 0; }
static BYTE GusRecordDma(PCGUS_STATE state)  { return GusAreDriversOn(state) ? state->RecordDmaLine  : 0; }

/* ---- the MIDI UART, a 6850 (ref §9) ---------------------------------------------- */

/* The ACIA's own interrupt request: receive full with CR7 (receive IRQ enable), or
   transmit empty with CR6-5 = 01 -- the only one of the four transmit-control codes
   that enables the transmit IRQ (the others are RTS high, and RTS low + break).
   While CR1-0 = 11 the ACIA is held in master reset and requests nothing. */
static INT GusMidiTransmitIrq(PCGUS_STATE state)
{
    return (state->MidiControl & 0x03) != 0x03 && (state->MidiControl & 0x60) == 0x20
        && (state->MidiStatus & GUS_ACIA_TRANSMIT_EMPTY);
}
static INT GusMidiReceiveIrq(PCGUS_STATE state)
{
    return (state->MidiControl & 0x03) != 0x03 && (state->MidiControl & 0x80)
        && (state->MidiStatus & GUS_ACIA_RECEIVE_FULL);
}
/* 2XB bank 6 bit 1: the MIDI port's address decode. Off, 3X0/3X1 are an empty bus. */
static INT GusIsMidiDecoded(PCGUS_STATE state) { return (state->Jumper & 0x02) != 0; }

/* ---- interrupts (ref §6) -------------------------------------------------------- */

static INT GusIsVoicePending(PCGUS_STATE state)
{
    UINT voiceIndex;
    for (voiceIndex = 0; voiceIndex < GUS_VOICES; ++voiceIndex)
        if ((state->Voices[voiceIndex].Control & 0x80) || (state->Voices[voiceIndex].VolumeControl & 0x80)) return 1;
    return 0;
}
static BYTE GusIrqStatus(PCGUS_STATE state)            /* 2X6 */
{
    BYTE status = 0, voiceIndex;
    INT isWave = 0, isVolume = 0;
    for (voiceIndex = 0; voiceIndex < GUS_VOICES; ++voiceIndex) {
        if (state->Voices[voiceIndex].Control & 0x80)  isWave = 1;
        if (state->Voices[voiceIndex].VolumeControl & 0x80) isVolume = 1;
    }
    if (GusMidiTransmitIrq(state)) status |= 0x01;   /* #190: the UART's two sources */
    if (GusMidiReceiveIrq(state)) status |= 0x02;
    if (state->IsTimer1Expired) status |= 0x04;
    if (state->IsTimer2Expired) status |= 0x08;
    if (isWave)       status |= 0x20;
    if (isVolume)        status |= 0x40;
    if (state->IsDmaTerminalCount || state->IsSampleTerminalCount) status |= 0x80;
    return status;
}
/* One physical line: raise on its rising edge only (the host latches an interrupt per
   call, so a source the guest has not cleared must not interrupt twice). */
static VOID GusLine(PGUS_STATE state, BYTE line, INT level, BYTE *isUp)
{
    if (!line) level = 0;
    if (level && !*isUp && state->Bus) {
        VddRaiseIrq(state->Bus, line);
        state->IrqsRaised++;
    }
    *isUp = (BYTE)(level != 0);
}
/* The card's lines are asserted while any ENABLED source is pending, the master IRQ
   enable (4Ch bit 2) is on and 2X0 bit 3 powers the drivers (ref §5, §6).
   #190: the GF1 sources go out on the IRQ latch's GF1 line, the UART's on its MIDI
   line -- which is the GF1 line itself when the latch or 2X0 bit 4 combines them. */
static VOID GusIrqUpdate(PGUS_STATE state)
{
    INT isGf1 = GusIsVoicePending(state)
           || (state->IsDmaTerminalCount && (state->DmaControl & 0x20))
           || (state->IsSampleTerminalCount && (state->SampleControl & 0x20))
           || (state->IsTimer1Expired && (state->TimerControl & 0x04))
           || (state->IsTimer2Expired && (state->TimerControl & 0x08));
    INT isMidi = GusMidiTransmitIrq(state) || GusMidiReceiveIrq(state);
    BYTE gf1Line = state->Gf1IrqLine, midiLine = state->MidiIrqLine;
    if (!(state->ResetRegister & 0x04) || !GusAreDriversOn(state)) isGf1 = isMidi = 0;
    if (midiLine == gf1Line) { GusLine(state, gf1Line, isGf1 || isMidi, &state->IsLineUp); state->IsMidiLineUp = 0; }
    else          { GusLine(state, gf1Line, isGf1, &state->IsLineUp); GusLine(state, midiLine, isMidi, &state->IsMidiLineUp); }
}

/* 8Fh: one pending voice event per read, cleared by the read (ref §6). Bits 7/6 are
   ACTIVE-LOW; both 1 means nothing is left. */
static BYTE GusIrqFifo(PGUS_STATE state)
{
    UINT voiceIndex;
    state->FifoReads++;
    for (voiceIndex = 0; voiceIndex < GUS_VOICES; ++voiceIndex) {
        PGUS_VOICE voice = &state->Voices[voiceIndex];
        if ((voice->Control & 0x80) || (voice->VolumeControl & 0x80)) {
            BYTE result = (BYTE)(0x20 | voiceIndex);
            if (!(voice->Control & 0x80))  result |= 0x80;
            if (!(voice->VolumeControl & 0x80)) result |= 0x40;
            voice->Control  &= 0x7F;
            voice->VolumeControl &= 0x7F;
            GusIrqUpdate(state);
            return result;
        }
    }
    return 0xE0;
}

/* ---- DRAM DMA (ref §3) ---------------------------------------------------------- */

/* The 16-bit-channel address translation, undone (ref §2.1). */
static UINT32 GusUntranslate16(UINT32 translated) { return ((translated & 0x1FFFFu) << 1) | (translated & 0xC0000u); }

/* Is the 8237 ready to serve a DRQ on `ch`? A masked channel is not, and since #176
   neither is one whose controller is disabled (command bit 2) -- both are the 8237's
   one question, VddDmaGrants: the card holds DRQ and waits, exactly as it would on
   the bus. (VddDmaRemaining cannot say so -- it is count + 1 and never 0.) */
static INT GusIsDmaReady(PCGUS_STATE state, BYTE channel)
{
    return state->Dma && channel && VddDmaGrants(state->Dma, channel);
}

/* #176: the card's DREQ lines, for the 8237's status bits 7:4. The DRAM DMA requests
   from the 41h "go" until the 8237 has served the whole block (IsDmaWaiting -- which
   only stays set while the channel is refused, since an answered request completes at
   once); the ADC requests while sampling is on. Channel 0 means "no line driven"
   (drivers off, or nothing latched), as everywhere in this file. */
static BYTE GusDreq(PCVOID context)
{
    PCGUS_STATE state = (PCGUS_STATE )context;
    BYTE mask = 0, channel;
    channel = GusDramDma(state);
    if (state->IsDmaWaiting && channel) mask |= (BYTE)(1u << (channel & 7));
    channel = GusRecordDma(state);
    if ((state->SampleControl & 0x01) && channel) mask |= (BYTE)(1u << (channel & 7));
    return mask;
}

/* The DRAM DMA (ref §3), both directions, instantaneous once the 8237 serves it.
     41h bit 1 = 0: PC -> card, an upload (VddDmaRead pulls guest memory).
     41h bit 1 = 1: card -> PC, #190: DRAM contents pushed into guest memory through
                    VddDmaWrite -- the guest programs its 8237 channel for a WRITE
                    (device -> memory) transfer, as for any card that is read.
   Bit 7 inverts the MSB of the data passing through, in both directions: it is a
   sign conversion, and converting on the way out undoes converting on the way in. */
static VOID GusDmaTry(PGUS_STATE state)
{
    BYTE channel = GusDramDma(state);
    UINT32 remaining, address, moved;
    INT isTerminalCount = 0, isCardToPc;
    BYTE buffer[512];
    if (!state->IsDmaWaiting || !state->Dram || !GusIsDmaReady(state, channel)) return;
    remaining = VddDmaRemaining(state->Dma, channel);
    isCardToPc = (state->DmaControl & 0x02) != 0;
    address = (UINT32)state->DmaAddress << 4;
    if (state->DmaControl & 0x04) address = GusUntranslate16(address);
    while (remaining && !isTerminalCount) {
        UINT32 byteIndex, chunk = remaining > sizeof buffer ? (UINT32)sizeof buffer : remaining;
        if (isCardToPc) {
            for (byteIndex = 0; byteIndex < chunk; ++byteIndex) {
                BYTE byteValue = state->Dram[(address + byteIndex) & (GUS_DRAM_SIZE - 1)];
                if ((state->DmaControl & 0x80) && (!(state->DmaControl & 0x40) || ((address + byteIndex) & 1))) byteValue ^= 0x80;
                buffer[byteIndex] = byteValue;
            }
            moved = VddDmaWrite(state->Dma, channel, buffer, chunk, &isTerminalCount);
            state->DmaDownloadBytes += moved;
        } else {
            moved = VddDmaRead(state->Dma, channel, buffer, chunk, &isTerminalCount);
            for (byteIndex = 0; byteIndex < moved; ++byteIndex) {
                BYTE byteValue = buffer[byteIndex];
                /* bit 7 = invert the MSB: bit 7 of every byte for 8-bit data, bit 15 of
                   every word (the odd byte) for 16-bit data (41h bit 6 written = 16-bit). */
                if (state->DmaControl & 0x80) {
                    if (!(state->DmaControl & 0x40) || ((address + byteIndex) & 1)) byteValue ^= 0x80;
                }
                state->Dram[(address + byteIndex) & (GUS_DRAM_SIZE - 1)] = byteValue;
            }
            state->DmaBytes += moved;
        }
        if (!moved) break;
        address += moved; remaining -= moved;
    }
    state->IsDmaWaiting = 0;
    if (isCardToPc) state->DmaDownloads++; else state->DmaUploads++;
    state->IsDmaTerminalCount = 1;
    GusIrqUpdate(state);
}

/* ---- the record path (ref §2.1: 48h rate, 49h control) ------------------------------ */

/* #190: sampling. 49h bit 0 starts the ADC; each sample goes card -> PC through the
   8237 on the RECORD DMA channel (the DMA latch's bits 5-3, or the DRAM channel when
   bit 6 combines them) at 9 878 400 / (16 x (48h + 2)) Hz, two bytes a sample when
   bit 1 asks for stereo. There is no input device behind line in or the mic, so every
   byte is the ADC's MIDSCALE: 80h -- 8-bit offset binary, the PC's unsigned sample
   format -- or 00h when 49h bit 7 inverts the MSB for signed data. Paced by rendered
   GF1 time, like the timers, so a program that times its take sees the rate it asked
   for; at terminal count the take stops (bit 0 is dropped) and 49h reports TC pending
   in bit 6, interrupting when bit 5 asked. A masked channel holds the ADC, as on the
   bus. */
static VOID GusRecord(PGUS_STATE state, UINT32 nanoseconds)
{
    BYTE channel = GusRecordDma(state), buffer[2];
    UINT32 rate, period, unit;
    INT isTerminalCount = 0;
    if (!(state->SampleControl & 0x01)) return;
    if (!GusIsDmaReady(state, channel)) return;
    unit = (channel & 4) ? 2u : 1u;                  /* a 16-bit channel moves words */
    rate = 9878400u / (16u * ((UINT32)state->SampleFrequency + 2u));
    period  = 1000000000u / (rate ? rate : 1u);
    state->SampleAccumulatorNs += nanoseconds;
    while (state->SampleAccumulatorNs >= period && (state->SampleControl & 0x01)) {
        state->SampleAccumulatorNs -= period;
        state->SamplePending = (BYTE)(state->SamplePending + ((state->SampleControl & 0x02) ? 2 : 1));
        while (state->SamplePending >= unit) {
            UINT32 moved;
            buffer[0] = buffer[1] = (state->SampleControl & 0x80) ? 0x00 : 0x80;
            moved = VddDmaWrite(state->Dma, channel, buffer, unit, &isTerminalCount);
            if (!moved) return;                      /* masked under us: hold the ADC */
            state->SamplePending = (BYTE)(state->SamplePending - moved);
            state->SampleBytes += moved;
            if (isTerminalCount) {
                state->SampleControl &= (BYTE)~0x01;
                state->IsSampleTerminalCount = 1; state->SampleAccumulatorNs = 0; state->SamplePending = 0;
                GusIrqUpdate(state);
                return;
            }
        }
    }
}

/* ---- the chip reset (4Ch bit 0 = 0) ------------------------------------------------- */

static VOID GusChipReset(PGUS_STATE state)
{
    UINT voiceIndex;
    for (voiceIndex = 0; voiceIndex < GUS_VOICES; ++voiceIndex) {
        PGUS_VOICE voice = &state->Voices[voiceIndex];
        voice->Control = 0x03; voice->VolumeControl = 0x03;    /* stopped, voice and ramp */
        voice->FrequencyControl = 0x0400; voice->Start = voice->End = voice->Position = 0;
        voice->RampRate = 0; voice->RampStart = 0; voice->RampEnd = 0;
        voice->Volume = 0; voice->Pan = 7; voice->RampDivider = 0;
    }
    state->ActiveVoices = 14;
    state->DmaControl = 0; state->IsDmaTerminalCount = 0; state->IsDmaWaiting = 0;
    state->TimerControl = 0; state->SampleControl = 0; state->IsSampleTerminalCount = 0;
    state->SampleAccumulatorNs = 0; state->SamplePending = 0;
    state->IsTimer1Running = state->IsTimer2Running = 0; state->IsTimer1Expired = state->IsTimer2Expired = 0;
    state->IsLineUp = 0; state->IsMidiLineUp = 0;
}

/* ---- register write / read ------------------------------------------------------- */

static VOID GusRegisterWrite(PGUS_STATE state, BYTE registerNumber, WORD value)
{
    PGUS_VOICE voice = &state->Voices[state->VoicePage & 0x1F];
    BYTE byteValue = (BYTE)value;
    switch (registerNumber) {
    /* voice (ref §2.2) */
    case 0x00: {
        INT wasStopped = (voice->Control & 0x03) != 0;
        voice->Control = (BYTE)((byteValue & 0x7F) | (voice->Control & 0x80));
        if (byteValue & 0x02) voice->Control |= 0x01;               /* stop -> stopped */
        if (!(byteValue & 0x20)) voice->Control &= 0x7F;            /* IRQ disabled: nothing pending */
        if (wasStopped && !(voice->Control & 0x03)) state->VoiceStarts++;
        GusIrqUpdate(state);
        break; }
    case 0x01: voice->FrequencyControl = value; break;
    case 0x02: voice->Start = GusPositionSetHigh(voice->Start, value); break;
    case 0x03: voice->Start = GusPositionSetLow(voice->Start, value, 0); break;
    case 0x04: voice->End   = GusPositionSetHigh(voice->End, value); break;
    case 0x05: voice->End   = GusPositionSetLow(voice->End, value, 0); break;
    case 0x06: voice->RampRate  = byteValue; break;
    case 0x07: voice->RampStart = byteValue; break;
    case 0x08: voice->RampEnd   = byteValue; break;
    case 0x09: voice->Volume = (WORD)(value & 0xFFF0); break;
    case 0x0A: voice->Position = GusPositionSetHigh(voice->Position, value); break;
    case 0x0B: voice->Position = GusPositionSetLow(voice->Position, value, 1); break;
    case 0x0C: voice->Pan = (BYTE)(byteValue & 0x0F); break;
    case 0x0D:
        voice->VolumeControl = (BYTE)((byteValue & 0x7F) | (voice->VolumeControl & 0x80));
        if (byteValue & 0x02) voice->VolumeControl |= 0x01;
        if (!(byteValue & 0x20)) voice->VolumeControl &= 0x7F;
        GusIrqUpdate(state);
        break;
    case 0x0E: {
        BYTE count = (BYTE)((byteValue & 0x1F) + 1);
        state->ActiveVoices = (BYTE)(count < 14 ? 14 : count);
        break; }
    /* global (ref §2.1) */
    case 0x41:
        state->DmaControl = byteValue;
        if (byteValue & 0x01) { state->IsDmaWaiting = 1; GusDmaTry(state); }
        else state->IsDmaWaiting = 0;
        break;
    case 0x42: state->DmaAddress = value; break;
    case 0x43: state->DramIoAddress = (state->DramIoAddress & 0xF0000u) | value; break;
    case 0x44: state->DramIoAddress = (state->DramIoAddress & 0x0FFFFu) | ((UINT32)(byteValue & 0x0F) << 16); break;
    case 0x45:
        state->TimerControl = byteValue;
        if (!(byteValue & 0x04)) state->IsTimer1Expired = 0;
        if (!(byteValue & 0x08)) state->IsTimer2Expired = 0;
        GusIrqUpdate(state);
        break;
    case 0x46: state->Timer1Load = byteValue; state->Timer1Value = byteValue; break;
    case 0x47: state->Timer2Load = byteValue; state->Timer2Value = byteValue; break;
    case 0x48: state->SampleFrequency = byteValue; break;
    case 0x49:
        /* #190: a take runs through the 8237 at the 48h rate -- see GusRecord. */
        if ((byteValue & 0x01) && !(state->SampleControl & 0x01)) {
            state->SampleAccumulatorNs = 0; state->SamplePending = 0; state->SampleTakes++;
        }
        state->SampleControl = byteValue;
        GusIrqUpdate(state);
        break;
    case 0x4B: state->JoystickTrim = byteValue; break;
    case 0x4C:
        if (!(byteValue & 0x01)) GusChipReset(state);
        state->ResetRegister = byteValue;
        GusIrqUpdate(state);
        break;
    default: break;
    }
}

static WORD GusRegisterRead(PGUS_STATE state, BYTE registerNumber)
{
    PGUS_VOICE voice = &state->Voices[state->VoicePage & 0x1F];
    switch (registerNumber) {
    case 0x80: return voice->Control;
    case 0x81: return voice->FrequencyControl;
    case 0x82: return GusPositionGetHigh(voice->Start);
    case 0x83: return GusPositionGetLow(voice->Start, 0);
    case 0x84: return GusPositionGetHigh(voice->End);
    case 0x85: return GusPositionGetLow(voice->End, 0);
    case 0x86: return voice->RampRate;
    case 0x87: return voice->RampStart;
    case 0x88: return voice->RampEnd;
    case 0x89: return voice->Volume;
    case 0x8A: return GusPositionGetHigh(voice->Position);
    case 0x8B: return GusPositionGetLow(voice->Position, 1);
    case 0x8C: return voice->Pan;
    case 0x8D: return voice->VolumeControl;
    case 0x8E: return (WORD)(0xC0 | (state->ActiveVoices - 1));
    case 0x8F: return GusIrqFifo(state);
    case 0x41: {                                     /* TC pending in bit 6, cleared by the read */
        BYTE value8 = (BYTE)((state->DmaControl & 0xBF) | (state->IsDmaTerminalCount ? 0x40 : 0));
        state->IsDmaTerminalCount = 0; GusIrqUpdate(state);
        return value8; }
    case 0x45: return state->TimerControl;
    case 0x49: {
        BYTE value8 = (BYTE)((state->SampleControl & 0xBF) | (state->IsSampleTerminalCount ? 0x40 : 0));
        state->IsSampleTerminalCount = 0; GusIrqUpdate(state);
        return value8; }
    case 0x4C: return state->ResetRegister;
    default:   return 0;
    }
}

/* ---- ports (ref §1) ---------------------------------------------------------- */

static VOID GusPortOut(PVOID context, WORD port, BYTE width, UINT32 value)
{
    PGUS_STATE state = (PGUS_STATE)context;
    WORD offset = (WORD)(port - state->BasePort);    /* 0x000-0x00F, or 0x100-0x107 */
    INT isArm = 0;
    state->IoWrites++;
    switch (offset) {
    case 0x000:
        /* Mix control (ref §5). Bit 0 line in off and bit 2 mic on reach nothing here --
           there is no input device, the ADC hears silence either way; bit 1 (line out
           off) mutes the render; bit 3 powers the IRQ/DMA drivers; bit 4 combines the
           GF1 and MIDI IRQs; bit 5 loops the UART's transmit back to its receive; bit 6
           picks which latch the next 2XB write reaches. */
        state->MixControl = (BYTE)value; isArm = 1;
        GusLatchDecode(state);
        GusIrqUpdate(state);
        GusDmaTry(state);                            /* drivers just powered: a DRQ waits */
        break;
    case 0x008: state->AdlibIndex = (BYTE)value; break;
    case 0x009:
        if (state->AdlibIndex == 4) {
            if (value & 0x80) { state->IsTimer1Expired = state->IsTimer2Expired = 0; GusIrqUpdate(state); break; }
            state->AdlibMask = (BYTE)(value & 0x60);
            state->IsTimer1Running = (BYTE)(value & 1); state->IsTimer2Running = (BYTE)((value >> 1) & 1);
            if (state->IsTimer1Running) { state->Timer1Value = state->Timer1Load; state->Timer1AccumulatorNs = 0; }
            if (state->IsTimer2Running) { state->Timer2Value = state->Timer2Load; state->Timer2AccumulatorNs = 0; }
        }
        break;
    case 0x00B:
        /* The write must be the NEXT one after 2X0, or it is locked out (ref §5). */
        if (!state->IsLatchArmed) { state->LatchLockedOut++; break; }
        /* #190: 2XF picks the bank behind 2XB (board rev 3.4+, ref §5). */
        switch (state->RegisterControl) {
        case 0:                                      /* the classic IRQ / DMA latches */
            if (state->MixControl & 0x40) state->IrqLatch = (BYTE)(value & 0x7F);
            else                state->DmaLatch = (BYTE)(value & 0x7F);
            GusLatchDecode(state);
            GusIrqUpdate(state);
            GusDmaTry(state);
            break;
        case 5:
            /* "Write 0 to clear power-up IRQs": whatever the card was asserting when
               it powered up is let go. The lines drop, so a source still pending
               afterwards interrupts afresh on the next update. */
            state->RegisterClear = (BYTE)value;
            if (!(value & 0xFF)) { state->IsLineUp = 0; state->IsMidiLineUp = 0; }
            break;
        case 6:                                      /* the jumper register */
            state->Jumper = (BYTE)value;
            break;
        default: break;                              /* no register behind the rest */
        }
        break;
    case 0x00F: state->RegisterControl = (BYTE)(value & 7); break;
    case 0x100:
        /* 6850 control (ref §9). CR1-0 = 11 is master reset: receive emptied, overrun
           cleared, transmitter empty -- and the ACIA held until a different code. */
        if (!GusIsMidiDecoded(state)) break;
        state->MidiControl = (BYTE)value;
        if ((value & 0x03) == 0x03) { state->MidiStatus = GUS_ACIA_TRANSMIT_EMPTY; state->MidiReceive = 0; }
        GusIrqUpdate(state);
        break;
    case 0x101: {
        /* 6850 transmit. The byte leaves at once (the wire is not modelled at 31 250
           baud: the synth behind the sink is not a wire), so TDRE is back before the
           guest can look -- but it DID drop: with the transmit IRQ on, each byte is a
           fresh empty edge, which is what a driver's IRQ-driven send loop waits for.
           2X0 bit 5 loops TxD to RxD inside the card: the byte is received, and does
           not reach MIDI OUT. */
        BYTE dataByte = (BYTE)value;
        if (!GusIsMidiDecoded(state) || (state->MidiControl & 0x03) == 0x03) break;
        state->MidiStatus &= (BYTE)~GUS_ACIA_TRANSMIT_EMPTY;
        GusIrqUpdate(state);
        if (state->MixControl & 0x20) {
            if (state->MidiStatus & GUS_ACIA_RECEIVE_FULL) state->MidiStatus |= GUS_ACIA_OVERRUN;
            state->MidiReceive = dataByte; state->MidiStatus |= GUS_ACIA_RECEIVE_FULL; state->MidiReceivedBytes++;
        } else if (state->MidiSink) state->MidiSink(state->MidiSinkContext, dataByte);
        state->MidiTransmitted++;
        state->MidiStatus |= GUS_ACIA_TRANSMIT_EMPTY;
        GusIrqUpdate(state);
        break; }
    case 0x102: state->VoicePage = (BYTE)(value & 0x1F); break;
    case 0x103: state->RegisterSelect = (BYTE)value; break;
    case 0x104:
        if (width >= 2) { GusRegisterWrite(state, state->RegisterSelect, (WORD)value); break; }
        /* A byte to 3X4 is the LOW half of a 16-bit register: latched, and the write
           to 3X5 that follows completes it (ref §2). */
        state->LowByteLatch = (WORD)(value & 0xFF);
        break;
    case 0x105:
        if (GusIsRegister16(state->RegisterSelect)) GusRegisterWrite(state, state->RegisterSelect, (WORD)(((value & 0xFF) << 8) | state->LowByteLatch));
        else                    GusRegisterWrite(state, state->RegisterSelect, (WORD)(value & 0xFF));
        break;
    case 0x107:
        if (state->Dram) state->Dram[state->DramIoAddress & (GUS_DRAM_SIZE - 1)] = (BYTE)value;
        state->DramPokes++;
        break;
    default: break;
    }
    state->IsLatchArmed = (BYTE)isArm;
}

static VOID GusPortIn(PVOID context, WORD port, BYTE width, UINT32 *value)
{
    PGUS_STATE state = (PGUS_STATE)context;
    WORD offset = (WORD)(port - state->BasePort);
    UINT32 result = 0xFF;
    state->IoReads++;
    switch (offset) {
    case 0x006: result = GusIrqStatus(state); break;
    case 0x008: result = (UINT32)((state->IsTimer1Expired && !(state->AdlibMask & 0x40) ? 0x40 : 0)
                             | (state->IsTimer2Expired && !(state->AdlibMask & 0x20) ? 0x20 : 0));
                if (result) result |= 0x80;
                break;
    case 0x00F: result = state->RegisterControl; break;
    case 0x100:                                      /* 6850 status (ref §9) */
        if (!GusIsMidiDecoded(state)) break;
        result = state->MidiStatus;
        if (GusMidiTransmitIrq(state) || GusMidiReceiveIrq(state)) result |= GUS_ACIA_IRQ;
        break;
    case 0x101:                                      /* 6850 receive: clears RDRF, OVRN */
        if (!GusIsMidiDecoded(state)) break;
        result = state->MidiReceive;
        state->MidiStatus &= (BYTE)~(GUS_ACIA_RECEIVE_FULL | GUS_ACIA_OVERRUN);
        GusIrqUpdate(state);
        break;
    case 0x102: result = state->VoicePage; break;
    case 0x103: result = state->RegisterSelect; break;
    case 0x104: {
        WORD value16 = GusIsRegister16(state->RegisterSelect) ? GusRegisterRead(state, (BYTE)(state->RegisterSelect | 0x80)) : 0;
        result = (width >= 2) ? value16 : (value16 & 0xFF);
        break; }
    case 0x105: {
        BYTE select = state->RegisterSelect;
        if (select < 0x40) select |= 0x80;           /* voice regs read at 80h+ (ref §2.2) */
        result = GusIsRegister16(state->RegisterSelect) ? (UINT32)(GusRegisterRead(state, select) >> 8) : (UINT32)(GusRegisterRead(state, select) & 0xFF);
        break; }
    case 0x107:
        result = state->Dram ? state->Dram[state->DramIoAddress & (GUS_DRAM_SIZE - 1)] : 0xFF;
        state->DramPeeks++;
        break;
    default: break;
    }
    *value = result;
}

/* ---- the voice engine (ref §4, §7) --------------------------------------------- */

UINT32 VddGusVolumeGain(WORD volume12)
{
    /* ref §7: the SDK's own linear table pins the curve -- one exponent step per
       octave, the mantissa linear within it: amplitude ∝ 2^E × (256 + M) / 256. */
    UINT32 exponent = (volume12 >> 8) & 0x0F, mantissa = volume12 & 0xFF;
    if (!volume12) return 0;
    return (((256u + mantissa) << exponent) >> 8);   /* Q16: 0xFFF -> 65408 ≈ 1.0 */
}

UINT32 VddGusRateHz(PCGUS_STATE state)
{
    /* 1.6197 us per voice per pass (ref §4): 14 voices -> 44.1 kHz. */
    UINT32 activeVoices = state->ActiveVoices ? state->ActiveVoices : 14;
    return 617400u / activeVoices;
}

static INT32 GusFetch(PCGUS_STATE state, PCGUS_VOICE voice, UINT32 address)
{
    if (voice->Control & 0x04) {                     /* 16-bit: the address is translated */
        UINT32 offset = GusUntranslate16(address & 0xFFFFFu) & (GUS_DRAM_SIZE - 2);
        return (INT16)(state->Dram[offset] | (state->Dram[offset + 1] << 8));
    }
    return (INT32)(INT8)state->Dram[address & (GUS_DRAM_SIZE - 1)] << 8;
}

static VOID GusVoiceStep(PGUS_STATE state, PGUS_VOICE voice)
{
    UINT32 increment = (UINT32)(voice->FrequencyControl >> 1), oldPosition = voice->Position;
    if (voice->Control & 0x03) return;               /* stopped: holds its place */
    if (voice->Control & 0x40) {                     /* decreasing */
        voice->Position = (oldPosition >= increment) ? oldPosition - increment : 0;
        if (oldPosition > voice->Start && voice->Position <= voice->Start) {
            if (voice->VolumeControl & 0x04) { if (voice->Control & 0x20) voice->Control |= 0x80; }
            else if (voice->Control & 0x08) {
                if (voice->Control & 0x10) { voice->Control &= (BYTE)~0x40; voice->Position = voice->Start + (voice->Start - voice->Position); }
                else                  voice->Position = voice->End - (voice->Start - voice->Position);
                if (voice->Control & 0x20) voice->Control |= 0x80;
            } else { voice->Control |= 0x01; voice->Position = voice->Start; if (voice->Control & 0x20) voice->Control |= 0x80; }
        }
    } else {
        voice->Position = oldPosition + increment;
        if (oldPosition < voice->End && voice->Position >= voice->End) {
            if (voice->VolumeControl & 0x04) { if (voice->Control & 0x20) voice->Control |= 0x80; }     /* rollover */
            else if (voice->Control & 0x08) {
                if (voice->Control & 0x10) { voice->Control |= 0x40; voice->Position = voice->End - (voice->Position - voice->End); }
                else                  voice->Position = voice->Start + (voice->Position - voice->End);
                if (voice->Control & 0x20) voice->Control |= 0x80;
            } else { voice->Control |= 0x01; voice->Position = voice->End; if (voice->Control & 0x20) voice->Control |= 0x80; }
        }
    }
}

static VOID GusRampStep(PGUS_VOICE voice)
{
    static const UINT32 divider[4] = { 1, 8, 64, 512 };
    INT32 volume12, low, high, step;
    if (voice->VolumeControl & 0x03) return;
    if (++voice->RampDivider < divider[(voice->RampRate >> 6) & 3]) return;
    voice->RampDivider = 0;
    step = voice->RampRate & 0x3F;
    volume12 = voice->Volume >> 4;
    low = (INT32)voice->RampStart << 4;
    high = (INT32)voice->RampEnd << 4;
    if (voice->VolumeControl & 0x40) {               /* decreasing */
        volume12 -= step;
        if (volume12 <= low) {
            if (voice->VolumeControl & 0x08) { if (voice->VolumeControl & 0x10) { voice->VolumeControl &= (BYTE)~0x40; volume12 = low; } else volume12 = high; }
            else { volume12 = low; voice->VolumeControl |= 0x01; }
            if (voice->VolumeControl & 0x20) voice->VolumeControl |= 0x80;
        }
    } else {
        volume12 += step;
        if (volume12 >= high) {
            if (voice->VolumeControl & 0x08) { if (voice->VolumeControl & 0x10) { voice->VolumeControl |= 0x40; volume12 = high; } else volume12 = low; }
            else { volume12 = high; voice->VolumeControl |= 0x01; }
            if (voice->VolumeControl & 0x20) voice->VolumeControl |= 0x80;
        }
    }
    if (volume12 < 0) volume12 = 0;
    if (volume12 > 0xFFF) volume12 = 0xFFF;
    voice->Volume = (WORD)(volume12 << 4);
}

static VOID GusTimers(PGUS_STATE state, UINT32 nanoseconds)
{
    if (state->IsTimer1Running) {
        state->Timer1AccumulatorNs += nanoseconds;
        while (state->Timer1AccumulatorNs >= 80000u) {            /* 80 us a tick (ref §9) */
            state->Timer1AccumulatorNs -= 80000u;
            if (++state->Timer1Value == 0) {
                state->Timer1Value = state->Timer1Load;
                if (!(state->AdlibMask & 0x40)) state->IsTimer1Expired = 1;
            }
        }
    }
    if (state->IsTimer2Running) {
        state->Timer2AccumulatorNs += nanoseconds;
        while (state->Timer2AccumulatorNs >= 320000u) {           /* 320 us */
            state->Timer2AccumulatorNs -= 320000u;
            if (++state->Timer2Value == 0) {
                state->Timer2Value = state->Timer2Load;
                if (!(state->AdlibMask & 0x20)) state->IsTimer2Expired = 1;
            }
        }
    }
}

/* ── #189: PAN. The GF1 places each voice at one of 16 positions (reg 0Ch: 0 = hard
     left, 15 = hard right, 7/8 = the middle). This is a BALANCE law, not a split: a side
     stays at full level until the voice moves away from it, so a centred voice comes
     out of each channel exactly as loud as the old mono sum -- nothing a program already
     plays gets quieter -- and a hard-panned one is silent on the far side. Q8 gains. */
static INT32 GusPanLeft(BYTE pan) { INT32 gain = (INT32)(15 - (pan & 15)) * 512 / 15; return gain > 256 ? 256 : gain; }
static INT32 GusPanRight(BYTE pan) { INT32 gain = (INT32)(pan & 15) * 512 / 15;        return gain > 256 ? 256 : gain; }
static INT16 GusClip(INT32 value) { return (INT16)(value > 32767 ? 32767 : (value < -32768 ? -32768 : value)); }

/* One render loop, two output shapes (as vdd_sb): `isStereo` 0 writes the mono sum it
   always did, 1 writes panned L/R pairs at output[2i], output[2i+1]. */
static VOID GusRender(PGUS_STATE state, INT16 *output, UINT32 count, INT isStereo)
{
    UINT32 sampleIndex, voiceIndex, nanoseconds = 1000000000u / (VddGusRateHz(state) ? VddGusRateHz(state) : 44100u);
    state->Renders++;
    GusDmaTry(state);                                /* a DMA that was waiting on the 8237 */
    for (sampleIndex = 0; sampleIndex < count; ++sampleIndex) {
        INT32 sum = 0, sumLeft = 0, sumRight = 0;
        INT16 mono;
        if (state->Dram && (state->ResetRegister & 0x03) == 0x03) {   /* running, DAC enabled */
            for (voiceIndex = 0; voiceIndex < state->ActiveVoices && voiceIndex < GUS_VOICES; ++voiceIndex) {
                PGUS_VOICE voice = &state->Voices[voiceIndex];
                UINT32 address = voice->Position >> 9, fraction = voice->Position & 0x1FF;
                INT32 sample0, sample1, sample, gain;
                gain = (INT32)VddGusVolumeGain((WORD)(voice->Volume >> 4));
                if (gain) {
                    sample0 = GusFetch(state, voice, address);
                    sample1 = GusFetch(state, voice, address + 1);
                    sample  = sample0 + (((sample1 - sample0) * (INT32)fraction) >> 9);
                    sample  = (sample * gain) >> 16;
                    sum += sample;
                    if (isStereo) { sumLeft += (sample * GusPanLeft(voice->Pan)) >> 8;
                                  sumRight += (sample * GusPanRight(voice->Pan)) >> 8; }
                }
                GusVoiceStep(state, voice);
                GusRampStep(voice);
            }
        }
        GusTimers(state, nanoseconds);
        GusRecord(state, nanoseconds);
        /* #190: 2X0 bit 1 = line out DISABLED (active-high, ref §5). The voices still
           run -- the GF1 does not know the amplifier is off -- but nothing is heard. */
        if (state->MixControl & 0x02) { sum = sumLeft = sumRight = 0; state->OutputMuted++; }
        mono = GusClip(sum >> 1);                    /* headroom for many voices */
        if (isStereo) { output[2*sampleIndex] = GusClip(sumLeft >> 1); output[2*sampleIndex+1] = GusClip(sumRight >> 1); }
        else          output[sampleIndex] = mono;
        if (mono) {                                  /* is anything actually audible? */
            UINT32 magnitude = (UINT32)(mono < 0 ? -mono : mono);
            state->OutputNonZero++;
            if (magnitude > state->OutputPeak) state->OutputPeak = magnitude;
        }
    }
    state->SamplesOut += count;
    GusIrqUpdate(state);
}

VOID VddGusRender(PGUS_STATE state, INT16 *output, UINT32 count)    { GusRender(state, output, count, 0); }
VOID VddGusRenderStereo(PGUS_STATE state, INT16 *output, UINT32 count) { GusRender(state, output, count, 1); }

/* ---- the bus ------------------------------------------------------------------- */

VOID VddGusReset(PVOID context)
{
    PGUS_STATE state = (PGUS_STATE)context;
    state->VoicePage = state->RegisterSelect = 0; state->LowByteLatch = 0;
    state->ResetRegister = 0; state->DramIoAddress = 0; state->DmaAddress = 0;
    state->IsLatchArmed = 0; state->RegisterControl = 0; state->RegisterClear = 0;
    state->AdlibIndex = 0; state->AdlibMask = 0;
    /* #190: the card as ULTRINIT leaves it, because that is the card every DOS program
       meets -- nothing on a PC runs before the boot-time init that a real GUS owner
       has in AUTOEXEC.BAT, and a program that only POLLS (heaven7) never programs
       the board itself. So: the latches hold ULTRASND's own numbers, combined where
       they are equal (as the SDK's UltraSetInterface does), line out on, line in off,
       drivers powered (2X0 = 09h, the SDK's final write, ref §5), both decodes
       enabled in the jumper register. A value with no code in the latch table cannot
       be latched, and is driven as it is. */
    state->MixControl = 0x09;
    state->Jumper = 0x06;
    {
        BYTE midiIrq = state->MidiIrq ? state->MidiIrq : state->Irq;
        BYTE recordDma = state->RecordDma  ? state->RecordDma  : state->DmaChannel;
        state->IrqLatch = (BYTE)(GusCodeOf(g_GusIrqMap, state->Irq)
                      | (midiIrq == state->Irq ? 0x40 : (GusCodeOf(g_GusIrqMap, midiIrq) << 3)));
        state->DmaLatch = (BYTE)(GusCodeOf(g_GusDmaMap, state->DmaChannel)
                      | (recordDma == state->DmaChannel ? 0x40 : (GusCodeOf(g_GusDmaMap, recordDma) << 3)));
        state->Gf1IrqLine = state->Irq; state->DramDmaLine = state->DmaChannel;
        state->MidiIrqLine = midiIrq;     state->RecordDmaLine  = recordDma;
    }
    state->MidiControl = 0x00; state->MidiStatus = GUS_ACIA_TRANSMIT_EMPTY; state->MidiReceive = 0;   /* reset, then released */
    GusChipReset(state);
}

INT VddGusInitialize(PVDD_BUS bus, PVOID context)
{
    PGUS_STATE state = (PGUS_STATE)context;
    state->Bus = bus;
    if (!state->BasePort)   state->BasePort   = GUS_DEFAULT_BASE;
    if (!state->Irq)    state->Irq    = GUS_DEFAULT_IRQ;
    if (!state->DmaChannel) state->DmaChannel = GUS_DEFAULT_DMA;
    if (state->Dma) VddDmaAddDreq(state->Dma, GusDreq, state);
    VddGusReset(state);
    if (VddClaimPorts(bus, state->BasePort, (WORD)(state->BasePort + 0x0F), GusPortIn, GusPortOut, state)) return -1;
    if (VddClaimPorts(bus, (WORD)(state->BasePort + 0x100), (WORD)(state->BasePort + 0x107), GusPortIn, GusPortOut, state)) return -1;
    return 0;
}
