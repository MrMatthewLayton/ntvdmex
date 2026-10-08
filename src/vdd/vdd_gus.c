/* vdd_gus.c -- the Gravis UltraSound (GF1). See vdd_gus.h, docs/ref/gus.md (§n below)
 * and docs/inventory/gus.md.
 *
 * Built from the UltraSound SDK v2.22 (manual Ch. 2 and the SDK's own driver source),
 * not from any application: a device is in because it is in the period hardware
 * contract. heaven7 is the acceptance test, not the specification.
 */
#include "vdd_gus.h"

/* The ports, as offsets from the base (ref §1): 2X0-2XF, and 3X0-3X7 at +100h. */
#define GUS_PORT_MIX                  0x000   /* 2X0: mix control                         */
#define GUS_PORT_IRQ_STATUS           0x006
#define GUS_PORT_ADLIB_INDEX          0x008   /* read: the AdLib timer status             */
#define GUS_PORT_ADLIB_DATA           0x009
#define GUS_PORT_LATCH                0x00B   /* the IRQ / DMA latches, and banks 5/6     */
#define GUS_PORT_REGISTER_CONTROL     0x00F
#define GUS_PORT_MIDI_CONTROL         0x100   /* 3X0: 6850 control / status               */
#define GUS_PORT_MIDI_DATA            0x101
#define GUS_PORT_VOICE_SELECT         0x102
#define GUS_PORT_REGISTER_SELECT      0x103
#define GUS_PORT_DATA_LOW             0x104
#define GUS_PORT_DATA_HIGH            0x105
#define GUS_PORT_DRAM                 0x107
#define GUS_PORT_LOW_LAST             0x0F
#define GUS_PORT_HIGH_FIRST           0x100
#define GUS_PORT_HIGH_LAST            0x107
#define GUS_FLOATING_BUS              0xFF
#define GUS_FAILED                    (-1)
#define GUS_WORD_WIDTH                2       /* a 16-bit port access                     */
#define GUS_WORD_BYTES                2u
#define GUS_DMA_CHANNEL_MASK          7
#define GUS_DMA_16BIT_CHANNELS        4       /* channels 4-7 move words                  */
/* The indirect registers (ref §2.1, §2.2): voice registers are written at 0xh and
   read at 8xh; the globals are 40h-4Ch. */
#define GUS_REGISTER_VOICE_CONTROL    0x00
#define GUS_REGISTER_FREQUENCY        0x01
#define GUS_REGISTER_START_HIGH       0x02
#define GUS_REGISTER_START_LOW        0x03
#define GUS_REGISTER_END_HIGH         0x04
#define GUS_REGISTER_END_LOW          0x05
#define GUS_REGISTER_RAMP_RATE        0x06
#define GUS_REGISTER_RAMP_START       0x07
#define GUS_REGISTER_RAMP_END         0x08
#define GUS_REGISTER_VOLUME           0x09
#define GUS_REGISTER_POSITION_HIGH    0x0A
#define GUS_REGISTER_POSITION_LOW     0x0B
#define GUS_REGISTER_PAN              0x0C
#define GUS_REGISTER_VOLUME_CONTROL   0x0D
#define GUS_REGISTER_ACTIVE_VOICES    0x0E
#define GUS_REGISTER_READ             0x80    /* a voice register's read form             */
#define GUS_REGISTER_NUMBER_MASK      0x7F
#define GUS_REGISTER_READ_VOICE_CONTROL  (GUS_REGISTER_READ | GUS_REGISTER_VOICE_CONTROL)
#define GUS_REGISTER_READ_FREQUENCY      (GUS_REGISTER_READ | GUS_REGISTER_FREQUENCY)
#define GUS_REGISTER_READ_START_HIGH     (GUS_REGISTER_READ | GUS_REGISTER_START_HIGH)
#define GUS_REGISTER_READ_START_LOW      (GUS_REGISTER_READ | GUS_REGISTER_START_LOW)
#define GUS_REGISTER_READ_END_HIGH       (GUS_REGISTER_READ | GUS_REGISTER_END_HIGH)
#define GUS_REGISTER_READ_END_LOW        (GUS_REGISTER_READ | GUS_REGISTER_END_LOW)
#define GUS_REGISTER_READ_RAMP_RATE      (GUS_REGISTER_READ | GUS_REGISTER_RAMP_RATE)
#define GUS_REGISTER_READ_RAMP_START     (GUS_REGISTER_READ | GUS_REGISTER_RAMP_START)
#define GUS_REGISTER_READ_RAMP_END       (GUS_REGISTER_READ | GUS_REGISTER_RAMP_END)
#define GUS_REGISTER_READ_VOLUME         (GUS_REGISTER_READ | GUS_REGISTER_VOLUME)
#define GUS_REGISTER_READ_POSITION_HIGH  (GUS_REGISTER_READ | GUS_REGISTER_POSITION_HIGH)
#define GUS_REGISTER_READ_POSITION_LOW   (GUS_REGISTER_READ | GUS_REGISTER_POSITION_LOW)
#define GUS_REGISTER_READ_PAN            (GUS_REGISTER_READ | GUS_REGISTER_PAN)
#define GUS_REGISTER_READ_VOLUME_CONTROL (GUS_REGISTER_READ | GUS_REGISTER_VOLUME_CONTROL)
#define GUS_REGISTER_READ_ACTIVE_VOICES  (GUS_REGISTER_READ | GUS_REGISTER_ACTIVE_VOICES)
#define GUS_REGISTER_IRQ_FIFO         0x8F    /* read only                                */
#define GUS_REGISTER_GLOBAL_FIRST     0x40
#define GUS_REGISTER_DMA_CONTROL      0x41
#define GUS_REGISTER_DMA_ADDRESS      0x42
#define GUS_REGISTER_DRAM_IO_LOW      0x43
#define GUS_REGISTER_DRAM_IO_HIGH     0x44
#define GUS_REGISTER_TIMER_CONTROL    0x45
#define GUS_REGISTER_TIMER1_COUNT     0x46
#define GUS_REGISTER_TIMER2_COUNT     0x47
#define GUS_REGISTER_SAMPLE_FREQUENCY 0x48
#define GUS_REGISTER_SAMPLE_CONTROL   0x49
#define GUS_REGISTER_JOYSTICK_TRIM    0x4B
#define GUS_REGISTER_RESET            0x4C
/* Voice control (00h) and volume (ramp) control (0Dh): the same layout (ref §2.2). */
#define GUS_VOICE_STOPPED             0x01
#define GUS_VOICE_STOP                0x02
#define GUS_VOICE_STOP_BITS           0x03
#define GUS_VOICE_16BIT               0x04
#define GUS_VOICE_LOOP                0x08
#define GUS_VOICE_BIDIRECTIONAL       0x10
#define GUS_VOICE_IRQ_ENABLE          0x20
#define GUS_VOICE_DECREASING          0x40
#define GUS_VOICE_IRQ_PENDING         0x80
#define GUS_VOICE_CONTROL_BITS        0x7F    /* all but IRQ pending                      */
#define GUS_RAMP_STOPPED              0x01
#define GUS_RAMP_STOP                 0x02
#define GUS_RAMP_STOP_BITS            0x03
#define GUS_RAMP_ROLLOVER             0x04
#define GUS_RAMP_LOOP                 0x08
#define GUS_RAMP_BIDIRECTIONAL        0x10
#define GUS_RAMP_IRQ_ENABLE           0x20
#define GUS_RAMP_DECREASING           0x40
#define GUS_RAMP_IRQ_PENDING          0x80
#define GUS_RAMP_CONTROL_BITS         0x7F
#define GUS_RAMP_RATES                4
#define GUS_RAMP_RATE_SHIFT           6       /* 06h bits 7-6: the update rate            */
#define GUS_RAMP_RATE_MASK            3
#define GUS_RAMP_STEP_MASK            0x3F
#define GUS_VOLUME_MASK               0xFFF0  /* the 12-bit volume in bits 15-4           */
#define GUS_VOLUME_SHIFT              4
#define GUS_VOLUME_MAX                0xFFF
#define GUS_VOLUME_EXPONENT_SHIFT     8
#define GUS_VOLUME_EXPONENT_MASK      0x0F
#define GUS_VOLUME_MANTISSA_MASK      0xFF
#define GUS_VOLUME_MANTISSA_ONE       256u
#define GUS_VOLUME_MANTISSA_SHIFT     8
#define GUS_GAIN_SHIFT                16      /* VddGusVolumeGain is Q16                  */
#define GUS_FREQUENCY_POWER_UP        0x0400
#define GUS_VOICE_MASK                0x1F
#define GUS_MIN_ACTIVE_VOICES         14
#define GUS_ACTIVE_VOICES_READ_BITS   0xC0
#define GUS_VOICE_SERVICE_HZ          617400u /* per voice per pass: 14 voices = 44.1 kHz */
/* Positions (ref §2.2), in 1/512-sample units. */
#define GUS_POSITION_FRACTION_BITS    9
#define GUS_POSITION_FRACTION_MASK    0x1FFu
#define GUS_POSITION_LOW_ADDRESS_BITS 7
#define GUS_POSITION_LOW_ADDRESS_MASK 0x7Fu
#define GUS_POSITION_HIGH_MASK        0x1FFF
#define GUS_POSITION_HIGH_SHIFT       16
#define GUS_POSITION_FOUR_BIT_SHIFT   5       /* start/end: four fraction bits, 8-5       */
#define GUS_POSITION_FOUR_BIT_MASK    0xF
#define GUS_POSITION_FOUR_BIT_FRACTION 0x1E0
#define GUS_TRANSLATE_LOW_MASK        0x1FFFFu
#define GUS_TRANSLATE_BANK_MASK       0xC0000u
#define GUS_DRAM_ADDRESS_MASK         0xFFFFFu
#define GUS_DRAM_IO_LOW_MASK          0x0FFFFu
#define GUS_DRAM_IO_HIGH_MASK         0xF0000u
#define GUS_DRAM_IO_HIGH_BITS         0x0F
#define GUS_DRAM_IO_HIGH_SHIFT        16
/* Pan (0Ch) and output. */
#define GUS_PAN_MASK                  0x0F
#define GUS_PAN_CENTRE                7
#define GUS_PAN_RIGHT                 15      /* hard right                               */
#define GUS_PAN_GAIN_SPAN             512
#define GUS_PAN_UNITY                 256     /* Q8 1.0                                   */
#define GUS_PAN_SHIFT                 8
#define GUS_SAMPLE_MAX                32767
#define GUS_SAMPLE_MIN                (-32768)
#define GUS_FALLBACK_RATE_HZ          44100u
#define GUS_NANOSECONDS_PER_SECOND    1000000000u
/* The latches (ref §5), 2X0 and the 2XF banks. */
#define GUS_LATCH_CODES               8
#define GUS_LATCH_CODE_MASK           7
#define GUS_LATCH_SECOND_SHIFT        3       /* bits 5-3: MIDI IRQ / record DMA          */
#define GUS_LATCH_COMBINE             0x40
#define GUS_LATCH_MASK                0x7F
#define GUS_MIX_LINE_OUT_OFF          0x02
#define GUS_MIX_DRIVERS_ON            0x08
#define GUS_MIX_COMBINE_IRQS          0x10
#define GUS_MIX_MIDI_LOOPBACK         0x20
#define GUS_MIX_SELECT_IRQ_LATCH      0x40
#define GUS_MIX_ULTRINIT              0x09    /* line out on, line in off, drivers on     */
#define GUS_REGCTL_MASK               7
#define GUS_REGCTL_LATCHES            0
#define GUS_REGCTL_CLEAR_IRQS         5
#define GUS_REGCTL_JUMPER             6
#define GUS_JUMPER_MIDI_DECODE        0x02
#define GUS_JUMPER_ULTRINIT           0x06    /* MIDI and joystick decodes on             */
/* Interrupts (ref §6): 2X6 and the 8Fh FIFO. */
#define GUS_IRQ_MIDI_TRANSMIT         0x01
#define GUS_IRQ_MIDI_RECEIVE          0x02
#define GUS_IRQ_TIMER1                0x04
#define GUS_IRQ_TIMER2                0x08
#define GUS_IRQ_WAVE                  0x20
#define GUS_IRQ_VOLUME                0x40
#define GUS_IRQ_DMA                   0x80
#define GUS_FIFO_ALWAYS_SET           0x20
#define GUS_FIFO_NO_VOLUME            0x40    /* active-low                               */
#define GUS_FIFO_NO_WAVE              0x80
#define GUS_FIFO_EMPTY                0xE0
/* DRAM DMA (41h) and sampling (49h). */
#define GUS_DMA_GO                    0x01
#define GUS_DMA_TO_PC                 0x02
#define GUS_DMA_CHANNEL_16BIT         0x04
#define GUS_DMA_IRQ_ENABLE            0x20
#define GUS_DMA_DATA_16BIT            0x40    /* written                                  */
#define GUS_DMA_TERMINAL_COUNT        0x40    /* read                                     */
#define GUS_DMA_INVERT_MSB            0x80
#define GUS_DMA_READ_MASK             0xBF
#define GUS_DMA_ADDRESS_SHIFT         4
#define GUS_DMA_CHUNK                 512
#define GUS_SAMPLE_MSB                0x80
#define GUS_SAMPLE_GO                 0x01
#define GUS_SAMPLE_STEREO             0x02
#define GUS_SAMPLE_IRQ_ENABLE         0x20
#define GUS_SAMPLE_TERMINAL_COUNT     0x40
#define GUS_SAMPLE_INVERT_MSB         0x80
#define GUS_SAMPLE_READ_MASK          0xBF
#define GUS_ADC_CLOCK_HZ              9878400u
#define GUS_ADC_DIVIDER               16u
#define GUS_ADC_RATE_OFFSET           2u
#define GUS_ADC_MIDSCALE              0x80
#define GUS_ADC_MIDSCALE_SIGNED       0x00
/* Timers and reset. */
#define GUS_TIMER1_IRQ_ENABLE         0x04
#define GUS_TIMER2_IRQ_ENABLE         0x08
#define GUS_TIMER1_PERIOD_NS          80000u
#define GUS_TIMER2_PERIOD_NS          320000u
#define GUS_ADLIB_TIMER_CONTROL       4
#define GUS_ADLIB_IRQ_RESET           0x80
#define GUS_ADLIB_MASK_BITS           0x60
#define GUS_ADLIB_MASK_TIMER1         0x40
#define GUS_ADLIB_MASK_TIMER2         0x20
#define GUS_ADLIB_TIMER1_EXPIRED      0x40
#define GUS_ADLIB_TIMER2_EXPIRED      0x20
#define GUS_ADLIB_IRQ                 0x80
#define GUS_RESET_RUN                 0x01
#define GUS_RESET_RUNNING             0x03    /* run + DAC enabled                        */
#define GUS_RESET_MASTER_IRQ          0x04
/* The 6850 (ref §9). */
#define GUS_ACIA_MASTER_RESET         0x03    /* CR1-0 = 11                               */
#define GUS_ACIA_TRANSMIT_CONTROL     0x60
#define GUS_ACIA_TRANSMIT_IRQ         0x20
#define GUS_ACIA_RECEIVE_IRQ          0x80
#define GUS_ACIA_CONTROL_POWER_UP     0x00

/* ---- register-file helpers -------------------------------------------------------- */

/* Which registers are 16 bits wide (ref §2.1, §2.2). Everything else is 8 and lives at 3X5. */
static INT GusIsRegister16(BYTE registerNumber)
{
    BYTE voiceRegister;
    if (registerNumber == GUS_REGISTER_DMA_ADDRESS || registerNumber == GUS_REGISTER_DRAM_IO_LOW) return 1;
    if (registerNumber >= GUS_REGISTER_GLOBAL_FIRST && registerNumber < GUS_REGISTER_READ) return 0;             /* the other globals are 8-bit */
    voiceRegister = (BYTE)(registerNumber & GUS_REGISTER_NUMBER_MASK);   /* voice regs: write 0xh, read 8xh */
    return voiceRegister == GUS_REGISTER_FREQUENCY || (voiceRegister >= GUS_REGISTER_START_HIGH && voiceRegister <= GUS_REGISTER_END_LOW) || voiceRegister == GUS_REGISTER_VOLUME || voiceRegister == GUS_REGISTER_POSITION_HIGH || voiceRegister == GUS_REGISTER_POSITION_LOW;
}

/* A position in 1/512-sample units from the (high, low) register pair (ref §2.2):
   high bits 12-0 = address 19-7; low bits 15-9 = address 6-0 and, below that, the
   fraction -- four bits for start/end (8-5), nine for the current position (8-0). */
static UINT32 GusPositionSetHigh(UINT32 position, WORD highWord)
{
    UINT32 address = ((UINT32)(highWord & GUS_POSITION_HIGH_MASK) << GUS_POSITION_LOW_ADDRESS_BITS) | ((position >> GUS_POSITION_FRACTION_BITS) & GUS_POSITION_LOW_ADDRESS_MASK);
    return (address << GUS_POSITION_FRACTION_BITS) | (position & GUS_POSITION_FRACTION_MASK);
}
static UINT32 GusPositionSetLow(UINT32 position, WORD lowWord, INT isNineBitFraction)
{
    UINT32 address = ((position >> GUS_POSITION_FRACTION_BITS) & ~GUS_POSITION_LOW_ADDRESS_MASK) | ((UINT32)(lowWord >> GUS_POSITION_FRACTION_BITS) & GUS_POSITION_LOW_ADDRESS_MASK);
    UINT32 fraction = isNineBitFraction ? (lowWord & GUS_POSITION_FRACTION_MASK) : ((UINT32)(lowWord >> GUS_POSITION_FOUR_BIT_SHIFT) & GUS_POSITION_FOUR_BIT_MASK) << GUS_POSITION_FOUR_BIT_SHIFT;
    return (address << GUS_POSITION_FRACTION_BITS) | fraction;
}
static WORD GusPositionGetHigh(UINT32 position) { return (WORD)((position >> GUS_POSITION_HIGH_SHIFT) & GUS_POSITION_HIGH_MASK); }
static WORD GusPositionGetLow(UINT32 position, INT isNineBitFraction)
{
    WORD lowWord = (WORD)(((position >> GUS_POSITION_FRACTION_BITS) & GUS_POSITION_LOW_ADDRESS_MASK) << GUS_POSITION_FRACTION_BITS);
    return (WORD)(lowWord | (isNineBitFraction ? (position & GUS_POSITION_FRACTION_MASK) : (position & GUS_POSITION_FOUR_BIT_FRACTION)));
}

/* ---- the latches (ref §5) ---------------------------------------------------------- */

/* The two 3-bit codes of the 2XB latches. Code 0 is "no line" in both tables. */
static const BYTE g_GusIrqMap[GUS_LATCH_CODES] = { 0, 2, 5, 3, 7, 11, 12, 15 };
static const BYTE g_GusDmaMap[GUS_LATCH_CODES] = { 0, 1, 3, 5, 6, 7, 0, 0 };
static BYTE GusCodeOf(const BYTE *map, BYTE line)
{
    BYTE code;
    for (code = 1; code < GUS_LATCH_CODES; ++code) if (map[code] && map[code] == line) return code;
    return 0;
}

/* #190: decode the latches into the lines the card drives (ref §5).
     IRQ latch: bits 2-0 GF1, 5-3 MIDI, bit 6 = both on the GF1 line.
     DMA latch: bits 2-0 DRAM, 5-3 record, bit 6 = both on the DRAM channel.
   2X0 bit 4 is the mix register's own "combine the GF1 and MIDI IRQs"; either asks
   for one line. */
static VOID GusLatchDecode(PGUS_STATE state)
{
    state->Gf1IrqLine  = g_GusIrqMap[state->IrqLatch & GUS_LATCH_CODE_MASK];
    state->MidiIrqLine = ((state->IrqLatch & GUS_LATCH_COMBINE) || (state->MixControl & GUS_MIX_COMBINE_IRQS))
                      ? state->Gf1IrqLine : g_GusIrqMap[(state->IrqLatch >> GUS_LATCH_SECOND_SHIFT) & GUS_LATCH_CODE_MASK];
    state->DramDmaLine = g_GusDmaMap[state->DmaLatch & GUS_LATCH_CODE_MASK];
    state->RecordDmaLine  = (state->DmaLatch & GUS_LATCH_COMBINE) ? state->DramDmaLine
                                               : g_GusDmaMap[(state->DmaLatch >> GUS_LATCH_SECOND_SHIFT) & GUS_LATCH_CODE_MASK];
}

/* 2X0 bit 3 powers the IRQ and DMA drivers: with it clear the card drives NO line,
   whatever the latches say (ref §5). */
static INT GusAreDriversOn(PCGUS_STATE state) { return (state->MixControl & GUS_MIX_DRIVERS_ON) != 0; }
static BYTE GusDramDma(PCGUS_STATE state) { return GusAreDriversOn(state) ? state->DramDmaLine : 0; }
static BYTE GusRecordDma(PCGUS_STATE state)  { return GusAreDriversOn(state) ? state->RecordDmaLine  : 0; }

/* ---- the MIDI UART, a 6850 (ref §9) ---------------------------------------------- */

/* The ACIA's own interrupt request: receive full with CR7 (receive IRQ enable), or
   transmit empty with CR6-5 = 01 -- the only one of the four transmit-control codes
   that enables the transmit IRQ (the others are RTS high, and RTS low + break).
   While CR1-0 = 11 the ACIA is held in master reset and requests nothing. */
static INT GusMidiTransmitIrq(PCGUS_STATE state)
{
    return (state->MidiControl & GUS_ACIA_MASTER_RESET) != GUS_ACIA_MASTER_RESET && (state->MidiControl & GUS_ACIA_TRANSMIT_CONTROL) == GUS_ACIA_TRANSMIT_IRQ
        && (state->MidiStatus & GUS_ACIA_TRANSMIT_EMPTY);
}
static INT GusMidiReceiveIrq(PCGUS_STATE state)
{
    return (state->MidiControl & GUS_ACIA_MASTER_RESET) != GUS_ACIA_MASTER_RESET && (state->MidiControl & GUS_ACIA_RECEIVE_IRQ)
        && (state->MidiStatus & GUS_ACIA_RECEIVE_FULL);
}
/* 2XB bank 6 bit 1: the MIDI port's address decode. Off, 3X0/3X1 are an empty bus. */
static INT GusIsMidiDecoded(PCGUS_STATE state) { return (state->Jumper & GUS_JUMPER_MIDI_DECODE) != 0; }

/* ---- interrupts (ref §6) -------------------------------------------------------- */

static INT GusIsVoicePending(PCGUS_STATE state)
{
    UINT voiceIndex;
    for (voiceIndex = 0; voiceIndex < GUS_VOICES; ++voiceIndex)
        if ((state->Voices[voiceIndex].Control & GUS_VOICE_IRQ_PENDING) || (state->Voices[voiceIndex].VolumeControl & GUS_RAMP_IRQ_PENDING)) return 1;
    return 0;
}
static BYTE GusIrqStatus(PCGUS_STATE state)            /* 2X6 */
{
    BYTE status = 0, voiceIndex;
    INT isWave = 0, isVolume = 0;
    for (voiceIndex = 0; voiceIndex < GUS_VOICES; ++voiceIndex) {
        if (state->Voices[voiceIndex].Control & GUS_VOICE_IRQ_PENDING)  isWave = 1;
        if (state->Voices[voiceIndex].VolumeControl & GUS_RAMP_IRQ_PENDING) isVolume = 1;
    }
    if (GusMidiTransmitIrq(state)) status |= GUS_IRQ_MIDI_TRANSMIT;   /* #190: the UART's two sources */
    if (GusMidiReceiveIrq(state)) status |= GUS_IRQ_MIDI_RECEIVE;
    if (state->IsTimer1Expired) status |= GUS_IRQ_TIMER1;
    if (state->IsTimer2Expired) status |= GUS_IRQ_TIMER2;
    if (isWave)       status |= GUS_IRQ_WAVE;
    if (isVolume)        status |= GUS_IRQ_VOLUME;
    if (state->IsDmaTerminalCount || state->IsSampleTerminalCount) status |= GUS_IRQ_DMA;
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
           || (state->IsDmaTerminalCount && (state->DmaControl & GUS_DMA_IRQ_ENABLE))
           || (state->IsSampleTerminalCount && (state->SampleControl & GUS_SAMPLE_IRQ_ENABLE))
           || (state->IsTimer1Expired && (state->TimerControl & GUS_TIMER1_IRQ_ENABLE))
           || (state->IsTimer2Expired && (state->TimerControl & GUS_TIMER2_IRQ_ENABLE));
    INT isMidi = GusMidiTransmitIrq(state) || GusMidiReceiveIrq(state);
    BYTE gf1Line = state->Gf1IrqLine, midiLine = state->MidiIrqLine;
    if (!(state->ResetRegister & GUS_RESET_MASTER_IRQ) || !GusAreDriversOn(state)) isGf1 = isMidi = 0;
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
        if ((voice->Control & GUS_VOICE_IRQ_PENDING) || (voice->VolumeControl & GUS_RAMP_IRQ_PENDING)) {
            BYTE result = (BYTE)(GUS_FIFO_ALWAYS_SET | voiceIndex);
            if (!(voice->Control & GUS_VOICE_IRQ_PENDING))  result |= GUS_FIFO_NO_WAVE;
            if (!(voice->VolumeControl & GUS_RAMP_IRQ_PENDING)) result |= GUS_FIFO_NO_VOLUME;
            voice->Control  &= GUS_VOICE_CONTROL_BITS;
            voice->VolumeControl &= GUS_RAMP_CONTROL_BITS;
            GusIrqUpdate(state);
            return result;
        }
    }
    return GUS_FIFO_EMPTY;
}

/* ---- DRAM DMA (ref §3) ---------------------------------------------------------- */

/* The 16-bit-channel address translation, undone (ref §2.1). */
static UINT32 GusUntranslate16(UINT32 translated) { return ((translated & GUS_TRANSLATE_LOW_MASK) << 1) | (translated & GUS_TRANSLATE_BANK_MASK); }

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
    if (state->IsDmaWaiting && channel) mask |= (BYTE)(1u << (channel & GUS_DMA_CHANNEL_MASK));
    channel = GusRecordDma(state);
    if ((state->SampleControl & GUS_SAMPLE_GO) && channel) mask |= (BYTE)(1u << (channel & GUS_DMA_CHANNEL_MASK));
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
    BYTE buffer[GUS_DMA_CHUNK];
    if (!state->IsDmaWaiting || !state->Dram || !GusIsDmaReady(state, channel)) return;
    remaining = VddDmaRemaining(state->Dma, channel);
    isCardToPc = (state->DmaControl & GUS_DMA_TO_PC) != 0;
    address = (UINT32)state->DmaAddress << GUS_DMA_ADDRESS_SHIFT;
    if (state->DmaControl & GUS_DMA_CHANNEL_16BIT) address = GusUntranslate16(address);
    while (remaining && !isTerminalCount) {
        UINT32 byteIndex, chunk = remaining > sizeof buffer ? (UINT32)sizeof buffer : remaining;
        if (isCardToPc) {
            for (byteIndex = 0; byteIndex < chunk; ++byteIndex) {
                BYTE byteValue = state->Dram[(address + byteIndex) & (GUS_DRAM_SIZE - 1)];
                if ((state->DmaControl & GUS_DMA_INVERT_MSB) && (!(state->DmaControl & GUS_DMA_DATA_16BIT) || ((address + byteIndex) & 1))) byteValue ^= GUS_SAMPLE_MSB;
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
                if (state->DmaControl & GUS_DMA_INVERT_MSB) {
                    if (!(state->DmaControl & GUS_DMA_DATA_16BIT) || ((address + byteIndex) & 1)) byteValue ^= GUS_SAMPLE_MSB;
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
    BYTE channel = GusRecordDma(state), buffer[GUS_STEREO_CHANNELS];
    UINT32 rate, period, unit;
    INT isTerminalCount = 0;
    if (!(state->SampleControl & GUS_SAMPLE_GO)) return;
    if (!GusIsDmaReady(state, channel)) return;
    unit = (channel & GUS_DMA_16BIT_CHANNELS) ? GUS_WORD_BYTES : 1u;                  /* a 16-bit channel moves words */
    rate = GUS_ADC_CLOCK_HZ / (GUS_ADC_DIVIDER * ((UINT32)state->SampleFrequency + GUS_ADC_RATE_OFFSET));
    period  = GUS_NANOSECONDS_PER_SECOND / (rate ? rate : 1u);
    state->SampleAccumulatorNs += nanoseconds;
    while (state->SampleAccumulatorNs >= period && (state->SampleControl & GUS_SAMPLE_GO)) {
        state->SampleAccumulatorNs -= period;
        state->SamplePending = (BYTE)(state->SamplePending + ((state->SampleControl & GUS_SAMPLE_STEREO) ? GUS_STEREO_CHANNELS : 1));
        while (state->SamplePending >= unit) {
            UINT32 moved;
            buffer[0] = buffer[1] = (state->SampleControl & GUS_SAMPLE_INVERT_MSB) ? GUS_ADC_MIDSCALE_SIGNED : GUS_ADC_MIDSCALE;
            moved = VddDmaWrite(state->Dma, channel, buffer, unit, &isTerminalCount);
            if (!moved) return;                      /* masked under us: hold the ADC */
            state->SamplePending = (BYTE)(state->SamplePending - moved);
            state->SampleBytes += moved;
            if (isTerminalCount) {
                state->SampleControl &= (BYTE)~GUS_SAMPLE_GO;
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
        voice->Control = GUS_VOICE_STOP_BITS; voice->VolumeControl = GUS_RAMP_STOP_BITS;    /* stopped, voice and ramp */
        voice->FrequencyControl = GUS_FREQUENCY_POWER_UP; voice->Start = voice->End = voice->Position = 0;
        voice->RampRate = 0; voice->RampStart = 0; voice->RampEnd = 0;
        voice->Volume = 0; voice->Pan = GUS_PAN_CENTRE; voice->RampDivider = 0;
    }
    state->ActiveVoices = GUS_MIN_ACTIVE_VOICES;
    state->DmaControl = 0; state->IsDmaTerminalCount = 0; state->IsDmaWaiting = 0;
    state->TimerControl = 0; state->SampleControl = 0; state->IsSampleTerminalCount = 0;
    state->SampleAccumulatorNs = 0; state->SamplePending = 0;
    state->IsTimer1Running = state->IsTimer2Running = 0; state->IsTimer1Expired = state->IsTimer2Expired = 0;
    state->IsLineUp = 0; state->IsMidiLineUp = 0;
}

/* ---- register write / read ------------------------------------------------------- */

static VOID GusRegisterWrite(PGUS_STATE state, BYTE registerNumber, WORD value)
{
    PGUS_VOICE voice = &state->Voices[state->VoicePage & GUS_VOICE_MASK];
    BYTE byteValue = (BYTE)value;
    switch (registerNumber) {
    /* voice (ref §2.2) */
    case GUS_REGISTER_VOICE_CONTROL: {
        INT wasStopped = (voice->Control & GUS_VOICE_STOP_BITS) != 0;
        voice->Control = (BYTE)((byteValue & GUS_VOICE_CONTROL_BITS) | (voice->Control & GUS_VOICE_IRQ_PENDING));
        if (byteValue & GUS_VOICE_STOP) voice->Control |= GUS_VOICE_STOPPED;               /* stop -> stopped */
        if (!(byteValue & GUS_VOICE_IRQ_ENABLE)) voice->Control &= GUS_VOICE_CONTROL_BITS;            /* IRQ disabled: nothing pending */
        if (wasStopped && !(voice->Control & GUS_VOICE_STOP_BITS)) state->VoiceStarts++;
        GusIrqUpdate(state);
        break; }
    case GUS_REGISTER_FREQUENCY: voice->FrequencyControl = value; break;
    case GUS_REGISTER_START_HIGH: voice->Start = GusPositionSetHigh(voice->Start, value); break;
    case GUS_REGISTER_START_LOW: voice->Start = GusPositionSetLow(voice->Start, value, 0); break;
    case GUS_REGISTER_END_HIGH: voice->End   = GusPositionSetHigh(voice->End, value); break;
    case GUS_REGISTER_END_LOW: voice->End   = GusPositionSetLow(voice->End, value, 0); break;
    case GUS_REGISTER_RAMP_RATE: voice->RampRate  = byteValue; break;
    case GUS_REGISTER_RAMP_START: voice->RampStart = byteValue; break;
    case GUS_REGISTER_RAMP_END: voice->RampEnd   = byteValue; break;
    case GUS_REGISTER_VOLUME: voice->Volume = (WORD)(value & GUS_VOLUME_MASK); break;
    case GUS_REGISTER_POSITION_HIGH: voice->Position = GusPositionSetHigh(voice->Position, value); break;
    case GUS_REGISTER_POSITION_LOW: voice->Position = GusPositionSetLow(voice->Position, value, 1); break;
    case GUS_REGISTER_PAN: voice->Pan = (BYTE)(byteValue & GUS_PAN_MASK); break;
    case GUS_REGISTER_VOLUME_CONTROL:
        voice->VolumeControl = (BYTE)((byteValue & GUS_RAMP_CONTROL_BITS) | (voice->VolumeControl & GUS_RAMP_IRQ_PENDING));
        if (byteValue & GUS_RAMP_STOP) voice->VolumeControl |= GUS_RAMP_STOPPED;
        if (!(byteValue & GUS_RAMP_IRQ_ENABLE)) voice->VolumeControl &= GUS_RAMP_CONTROL_BITS;
        GusIrqUpdate(state);
        break;
    case GUS_REGISTER_ACTIVE_VOICES: {
        BYTE count = (BYTE)((byteValue & GUS_VOICE_MASK) + 1);
        state->ActiveVoices = (BYTE)(count < GUS_MIN_ACTIVE_VOICES ? GUS_MIN_ACTIVE_VOICES : count);
        break; }
    /* global (ref §2.1) */
    case GUS_REGISTER_DMA_CONTROL:
        state->DmaControl = byteValue;
        if (byteValue & GUS_DMA_GO) { state->IsDmaWaiting = 1; GusDmaTry(state); }
        else state->IsDmaWaiting = 0;
        break;
    case GUS_REGISTER_DMA_ADDRESS: state->DmaAddress = value; break;
    case GUS_REGISTER_DRAM_IO_LOW: state->DramIoAddress = (state->DramIoAddress & GUS_DRAM_IO_HIGH_MASK) | value; break;
    case GUS_REGISTER_DRAM_IO_HIGH: state->DramIoAddress = (state->DramIoAddress & GUS_DRAM_IO_LOW_MASK) | ((UINT32)(byteValue & GUS_DRAM_IO_HIGH_BITS) << GUS_DRAM_IO_HIGH_SHIFT); break;
    case GUS_REGISTER_TIMER_CONTROL:
        state->TimerControl = byteValue;
        if (!(byteValue & GUS_TIMER1_IRQ_ENABLE)) state->IsTimer1Expired = 0;
        if (!(byteValue & GUS_TIMER2_IRQ_ENABLE)) state->IsTimer2Expired = 0;
        GusIrqUpdate(state);
        break;
    case GUS_REGISTER_TIMER1_COUNT: state->Timer1Load = byteValue; state->Timer1Value = byteValue; break;
    case GUS_REGISTER_TIMER2_COUNT: state->Timer2Load = byteValue; state->Timer2Value = byteValue; break;
    case GUS_REGISTER_SAMPLE_FREQUENCY: state->SampleFrequency = byteValue; break;
    case GUS_REGISTER_SAMPLE_CONTROL:
        /* #190: a take runs through the 8237 at the 48h rate -- see GusRecord. */
        if ((byteValue & GUS_SAMPLE_GO) && !(state->SampleControl & GUS_SAMPLE_GO)) {
            state->SampleAccumulatorNs = 0; state->SamplePending = 0; state->SampleTakes++;
        }
        state->SampleControl = byteValue;
        GusIrqUpdate(state);
        break;
    case GUS_REGISTER_JOYSTICK_TRIM: state->JoystickTrim = byteValue; break;
    case GUS_REGISTER_RESET:
        if (!(byteValue & GUS_RESET_RUN)) GusChipReset(state);
        state->ResetRegister = byteValue;
        GusIrqUpdate(state);
        break;
    default: break;
    }
}

static WORD GusRegisterRead(PGUS_STATE state, BYTE registerNumber)
{
    PGUS_VOICE voice = &state->Voices[state->VoicePage & GUS_VOICE_MASK];
    switch (registerNumber) {
    case GUS_REGISTER_READ_VOICE_CONTROL: return voice->Control;
    case GUS_REGISTER_READ_FREQUENCY: return voice->FrequencyControl;
    case GUS_REGISTER_READ_START_HIGH: return GusPositionGetHigh(voice->Start);
    case GUS_REGISTER_READ_START_LOW: return GusPositionGetLow(voice->Start, 0);
    case GUS_REGISTER_READ_END_HIGH: return GusPositionGetHigh(voice->End);
    case GUS_REGISTER_READ_END_LOW: return GusPositionGetLow(voice->End, 0);
    case GUS_REGISTER_READ_RAMP_RATE: return voice->RampRate;
    case GUS_REGISTER_READ_RAMP_START: return voice->RampStart;
    case GUS_REGISTER_READ_RAMP_END: return voice->RampEnd;
    case GUS_REGISTER_READ_VOLUME: return voice->Volume;
    case GUS_REGISTER_READ_POSITION_HIGH: return GusPositionGetHigh(voice->Position);
    case GUS_REGISTER_READ_POSITION_LOW: return GusPositionGetLow(voice->Position, 1);
    case GUS_REGISTER_READ_PAN: return voice->Pan;
    case GUS_REGISTER_READ_VOLUME_CONTROL: return voice->VolumeControl;
    case GUS_REGISTER_READ_ACTIVE_VOICES: return (WORD)(GUS_ACTIVE_VOICES_READ_BITS | (state->ActiveVoices - 1));
    case GUS_REGISTER_IRQ_FIFO: return GusIrqFifo(state);
    case GUS_REGISTER_DMA_CONTROL: {                                     /* TC pending in bit 6, cleared by the read */
        BYTE value8 = (BYTE)((state->DmaControl & GUS_DMA_READ_MASK) | (state->IsDmaTerminalCount ? GUS_DMA_TERMINAL_COUNT : 0));
        state->IsDmaTerminalCount = 0; GusIrqUpdate(state);
        return value8; }
    case GUS_REGISTER_TIMER_CONTROL: return state->TimerControl;
    case GUS_REGISTER_SAMPLE_CONTROL: {
        BYTE value8 = (BYTE)((state->SampleControl & GUS_SAMPLE_READ_MASK) | (state->IsSampleTerminalCount ? GUS_SAMPLE_TERMINAL_COUNT : 0));
        state->IsSampleTerminalCount = 0; GusIrqUpdate(state);
        return value8; }
    case GUS_REGISTER_RESET: return state->ResetRegister;
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
    case GUS_PORT_MIX:
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
    case GUS_PORT_ADLIB_INDEX: state->AdlibIndex = (BYTE)value; break;
    case GUS_PORT_ADLIB_DATA:
        if (state->AdlibIndex == GUS_ADLIB_TIMER_CONTROL) {
            if (value & GUS_ADLIB_IRQ_RESET) { state->IsTimer1Expired = state->IsTimer2Expired = 0; GusIrqUpdate(state); break; }
            state->AdlibMask = (BYTE)(value & GUS_ADLIB_MASK_BITS);
            state->IsTimer1Running = (BYTE)(value & 1); state->IsTimer2Running = (BYTE)((value >> 1) & 1);
            if (state->IsTimer1Running) { state->Timer1Value = state->Timer1Load; state->Timer1AccumulatorNs = 0; }
            if (state->IsTimer2Running) { state->Timer2Value = state->Timer2Load; state->Timer2AccumulatorNs = 0; }
        }
        break;
    case GUS_PORT_LATCH:
        /* The write must be the NEXT one after 2X0, or it is locked out (ref §5). */
        if (!state->IsLatchArmed) { state->LatchLockedOut++; break; }
        /* #190: 2XF picks the bank behind 2XB (board rev 3.4+, ref §5). */
        switch (state->RegisterControl) {
        case GUS_REGCTL_LATCHES:                                      /* the classic IRQ / DMA latches */
            if (state->MixControl & GUS_MIX_SELECT_IRQ_LATCH) state->IrqLatch = (BYTE)(value & GUS_LATCH_MASK);
            else                state->DmaLatch = (BYTE)(value & GUS_LATCH_MASK);
            GusLatchDecode(state);
            GusIrqUpdate(state);
            GusDmaTry(state);
            break;
        case GUS_REGCTL_CLEAR_IRQS:
            /* "Write 0 to clear power-up IRQs": whatever the card was asserting when
               it powered up is let go. The lines drop, so a source still pending
               afterwards interrupts afresh on the next update. */
            state->RegisterClear = (BYTE)value;
            if (!(value & BYTE_MASK)) { state->IsLineUp = 0; state->IsMidiLineUp = 0; }
            break;
        case GUS_REGCTL_JUMPER:                                      /* the jumper register */
            state->Jumper = (BYTE)value;
            break;
        default: break;                              /* no register behind the rest */
        }
        break;
    case GUS_PORT_REGISTER_CONTROL: state->RegisterControl = (BYTE)(value & GUS_REGCTL_MASK); break;
    case GUS_PORT_MIDI_CONTROL:
        /* 6850 control (ref §9). CR1-0 = 11 is master reset: receive emptied, overrun
           cleared, transmitter empty -- and the ACIA held until a different code. */
        if (!GusIsMidiDecoded(state)) break;
        state->MidiControl = (BYTE)value;
        if ((value & GUS_ACIA_MASTER_RESET) == GUS_ACIA_MASTER_RESET) { state->MidiStatus = GUS_ACIA_TRANSMIT_EMPTY; state->MidiReceive = 0; }
        GusIrqUpdate(state);
        break;
    case GUS_PORT_MIDI_DATA: {
        /* 6850 transmit. The byte leaves at once (the wire is not modelled at 31 250
           baud: the synth behind the sink is not a wire), so TDRE is back before the
           guest can look -- but it DID drop: with the transmit IRQ on, each byte is a
           fresh empty edge, which is what a driver's IRQ-driven send loop waits for.
           2X0 bit 5 loops TxD to RxD inside the card: the byte is received, and does
           not reach MIDI OUT. */
        BYTE dataByte = (BYTE)value;
        if (!GusIsMidiDecoded(state) || (state->MidiControl & GUS_ACIA_MASTER_RESET) == GUS_ACIA_MASTER_RESET) break;
        state->MidiStatus &= (BYTE)~GUS_ACIA_TRANSMIT_EMPTY;
        GusIrqUpdate(state);
        if (state->MixControl & GUS_MIX_MIDI_LOOPBACK) {
            if (state->MidiStatus & GUS_ACIA_RECEIVE_FULL) state->MidiStatus |= GUS_ACIA_OVERRUN;
            state->MidiReceive = dataByte; state->MidiStatus |= GUS_ACIA_RECEIVE_FULL; state->MidiReceivedBytes++;
        } else if (state->MidiSink) state->MidiSink(state->MidiSinkContext, dataByte);
        state->MidiTransmitted++;
        state->MidiStatus |= GUS_ACIA_TRANSMIT_EMPTY;
        GusIrqUpdate(state);
        break; }
    case GUS_PORT_VOICE_SELECT: state->VoicePage = (BYTE)(value & GUS_VOICE_MASK); break;
    case GUS_PORT_REGISTER_SELECT: state->RegisterSelect = (BYTE)value; break;
    case GUS_PORT_DATA_LOW:
        if (width >= GUS_WORD_WIDTH) { GusRegisterWrite(state, state->RegisterSelect, (WORD)value); break; }
        /* A byte to 3X4 is the LOW half of a 16-bit register: latched, and the write
           to 3X5 that follows completes it (ref §2). */
        state->LowByteLatch = (WORD)(value & BYTE_MASK);
        break;
    case GUS_PORT_DATA_HIGH:
        if (GusIsRegister16(state->RegisterSelect)) GusRegisterWrite(state, state->RegisterSelect, (WORD)(((value & BYTE_MASK) << BYTE_SHIFT) | state->LowByteLatch));
        else                    GusRegisterWrite(state, state->RegisterSelect, (WORD)(value & BYTE_MASK));
        break;
    case GUS_PORT_DRAM:
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
    UINT32 result = GUS_FLOATING_BUS;
    state->IoReads++;
    switch (offset) {
    case GUS_PORT_IRQ_STATUS: result = GusIrqStatus(state); break;
    case GUS_PORT_ADLIB_INDEX: result = (UINT32)((state->IsTimer1Expired && !(state->AdlibMask & GUS_ADLIB_MASK_TIMER1) ? GUS_ADLIB_TIMER1_EXPIRED : 0)
                             | (state->IsTimer2Expired && !(state->AdlibMask & GUS_ADLIB_MASK_TIMER2) ? GUS_ADLIB_TIMER2_EXPIRED : 0));
                if (result) result |= GUS_ADLIB_IRQ;
                break;
    case GUS_PORT_REGISTER_CONTROL: result = state->RegisterControl; break;
    case GUS_PORT_MIDI_CONTROL:                                      /* 6850 status (ref §9) */
        if (!GusIsMidiDecoded(state)) break;
        result = state->MidiStatus;
        if (GusMidiTransmitIrq(state) || GusMidiReceiveIrq(state)) result |= GUS_ACIA_IRQ;
        break;
    case GUS_PORT_MIDI_DATA:                                      /* 6850 receive: clears RDRF, OVRN */
        if (!GusIsMidiDecoded(state)) break;
        result = state->MidiReceive;
        state->MidiStatus &= (BYTE)~(GUS_ACIA_RECEIVE_FULL | GUS_ACIA_OVERRUN);
        GusIrqUpdate(state);
        break;
    case GUS_PORT_VOICE_SELECT: result = state->VoicePage; break;
    case GUS_PORT_REGISTER_SELECT: result = state->RegisterSelect; break;
    case GUS_PORT_DATA_LOW: {
        WORD value16 = GusIsRegister16(state->RegisterSelect) ? GusRegisterRead(state, (BYTE)(state->RegisterSelect | GUS_REGISTER_READ)) : 0;
        result = (width >= GUS_WORD_WIDTH) ? value16 : (value16 & BYTE_MASK);
        break; }
    case GUS_PORT_DATA_HIGH: {
        BYTE select = state->RegisterSelect;
        if (select < GUS_REGISTER_GLOBAL_FIRST) select |= GUS_REGISTER_READ;           /* voice regs read at 80h+ (ref §2.2) */
        result = GusIsRegister16(state->RegisterSelect) ? (UINT32)(GusRegisterRead(state, select) >> BYTE_SHIFT) : (UINT32)(GusRegisterRead(state, select) & BYTE_MASK);
        break; }
    case GUS_PORT_DRAM:
        result = state->Dram ? state->Dram[state->DramIoAddress & (GUS_DRAM_SIZE - 1)] : GUS_FLOATING_BUS;
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
    UINT32 exponent = (volume12 >> GUS_VOLUME_EXPONENT_SHIFT) & GUS_VOLUME_EXPONENT_MASK, mantissa = volume12 & GUS_VOLUME_MANTISSA_MASK;
    if (!volume12) return 0;
    return (((GUS_VOLUME_MANTISSA_ONE + mantissa) << exponent) >> GUS_VOLUME_MANTISSA_SHIFT);   /* Q16: 0xFFF -> 65408 ≈ 1.0 */
}

UINT32 VddGusRateHz(PCGUS_STATE state)
{
    /* 1.6197 us per voice per pass (ref §4): 14 voices -> 44.1 kHz. */
    UINT32 activeVoices = state->ActiveVoices ? state->ActiveVoices : GUS_MIN_ACTIVE_VOICES;
    return GUS_VOICE_SERVICE_HZ / activeVoices;
}

static INT32 GusFetch(PCGUS_STATE state, PCGUS_VOICE voice, UINT32 address)
{
    if (voice->Control & GUS_VOICE_16BIT) {                     /* 16-bit: the address is translated */
        UINT32 offset = GusUntranslate16(address & GUS_DRAM_ADDRESS_MASK) & (GUS_DRAM_SIZE - GUS_WORD_BYTES);
        return (INT16)(state->Dram[offset] | (state->Dram[offset + 1] << BYTE_SHIFT));
    }
    return (INT32)(INT8)state->Dram[address & (GUS_DRAM_SIZE - 1)] << BYTE_SHIFT;
}

static VOID GusVoiceStep(PGUS_STATE state, PGUS_VOICE voice)
{
    UINT32 increment = (UINT32)(voice->FrequencyControl >> 1), oldPosition = voice->Position;
    if (voice->Control & GUS_VOICE_STOP_BITS) return;               /* stopped: holds its place */
    if (voice->Control & GUS_VOICE_DECREASING) {                     /* decreasing */
        voice->Position = (oldPosition >= increment) ? oldPosition - increment : 0;
        if (oldPosition > voice->Start && voice->Position <= voice->Start) {
            if (voice->VolumeControl & GUS_RAMP_ROLLOVER) { if (voice->Control & GUS_VOICE_IRQ_ENABLE) voice->Control |= GUS_VOICE_IRQ_PENDING; }
            else if (voice->Control & GUS_VOICE_LOOP) {
                if (voice->Control & GUS_VOICE_BIDIRECTIONAL) { voice->Control &= (BYTE)~GUS_VOICE_DECREASING; voice->Position = voice->Start + (voice->Start - voice->Position); }
                else                  voice->Position = voice->End - (voice->Start - voice->Position);
                if (voice->Control & GUS_VOICE_IRQ_ENABLE) voice->Control |= GUS_VOICE_IRQ_PENDING;
            } else { voice->Control |= GUS_VOICE_STOPPED; voice->Position = voice->Start; if (voice->Control & GUS_VOICE_IRQ_ENABLE) voice->Control |= GUS_VOICE_IRQ_PENDING; }
        }
    } else {
        voice->Position = oldPosition + increment;
        if (oldPosition < voice->End && voice->Position >= voice->End) {
            if (voice->VolumeControl & GUS_RAMP_ROLLOVER) { if (voice->Control & GUS_VOICE_IRQ_ENABLE) voice->Control |= GUS_VOICE_IRQ_PENDING; }     /* rollover */
            else if (voice->Control & GUS_VOICE_LOOP) {
                if (voice->Control & GUS_VOICE_BIDIRECTIONAL) { voice->Control |= GUS_VOICE_DECREASING; voice->Position = voice->End - (voice->Position - voice->End); }
                else                  voice->Position = voice->Start + (voice->Position - voice->End);
                if (voice->Control & GUS_VOICE_IRQ_ENABLE) voice->Control |= GUS_VOICE_IRQ_PENDING;
            } else { voice->Control |= GUS_VOICE_STOPPED; voice->Position = voice->End; if (voice->Control & GUS_VOICE_IRQ_ENABLE) voice->Control |= GUS_VOICE_IRQ_PENDING; }
        }
    }
}

static VOID GusRampStep(PGUS_VOICE voice)
{
    static const UINT32 divider[GUS_RAMP_RATES] = { 1, 8, 64, 512 };
    INT32 volume12, low, high, step;
    if (voice->VolumeControl & GUS_RAMP_STOP_BITS) return;
    if (++voice->RampDivider < divider[(voice->RampRate >> GUS_RAMP_RATE_SHIFT) & GUS_RAMP_RATE_MASK]) return;
    voice->RampDivider = 0;
    step = voice->RampRate & GUS_RAMP_STEP_MASK;
    volume12 = voice->Volume >> GUS_VOLUME_SHIFT;
    low = (INT32)voice->RampStart << GUS_VOLUME_SHIFT;
    high = (INT32)voice->RampEnd << GUS_VOLUME_SHIFT;
    if (voice->VolumeControl & GUS_RAMP_DECREASING) {               /* decreasing */
        volume12 -= step;
        if (volume12 <= low) {
            if (voice->VolumeControl & GUS_RAMP_LOOP) { if (voice->VolumeControl & GUS_RAMP_BIDIRECTIONAL) { voice->VolumeControl &= (BYTE)~GUS_RAMP_DECREASING; volume12 = low; } else volume12 = high; }
            else { volume12 = low; voice->VolumeControl |= GUS_RAMP_STOPPED; }
            if (voice->VolumeControl & GUS_RAMP_IRQ_ENABLE) voice->VolumeControl |= GUS_RAMP_IRQ_PENDING;
        }
    } else {
        volume12 += step;
        if (volume12 >= high) {
            if (voice->VolumeControl & GUS_RAMP_LOOP) { if (voice->VolumeControl & GUS_RAMP_BIDIRECTIONAL) { voice->VolumeControl |= GUS_RAMP_DECREASING; volume12 = high; } else volume12 = low; }
            else { volume12 = high; voice->VolumeControl |= GUS_RAMP_STOPPED; }
            if (voice->VolumeControl & GUS_RAMP_IRQ_ENABLE) voice->VolumeControl |= GUS_RAMP_IRQ_PENDING;
        }
    }
    if (volume12 < 0) volume12 = 0;
    if (volume12 > GUS_VOLUME_MAX) volume12 = GUS_VOLUME_MAX;
    voice->Volume = (WORD)(volume12 << GUS_VOLUME_SHIFT);
}

static VOID GusTimers(PGUS_STATE state, UINT32 nanoseconds)
{
    if (state->IsTimer1Running) {
        state->Timer1AccumulatorNs += nanoseconds;
        while (state->Timer1AccumulatorNs >= GUS_TIMER1_PERIOD_NS) {            /* 80 us a tick (ref §9) */
            state->Timer1AccumulatorNs -= GUS_TIMER1_PERIOD_NS;
            if (++state->Timer1Value == 0) {
                state->Timer1Value = state->Timer1Load;
                if (!(state->AdlibMask & GUS_ADLIB_MASK_TIMER1)) state->IsTimer1Expired = 1;
            }
        }
    }
    if (state->IsTimer2Running) {
        state->Timer2AccumulatorNs += nanoseconds;
        while (state->Timer2AccumulatorNs >= GUS_TIMER2_PERIOD_NS) {           /* 320 us */
            state->Timer2AccumulatorNs -= GUS_TIMER2_PERIOD_NS;
            if (++state->Timer2Value == 0) {
                state->Timer2Value = state->Timer2Load;
                if (!(state->AdlibMask & GUS_ADLIB_MASK_TIMER2)) state->IsTimer2Expired = 1;
            }
        }
    }
}

/* ── #189: PAN. The GF1 places each voice at one of 16 positions (reg 0Ch: 0 = hard
     left, 15 = hard right, 7/8 = the middle). This is a BALANCE law, not a split: a side
     stays at full level until the voice moves away from it, so a centred voice comes
     out of each channel exactly as loud as the old mono sum -- nothing a program already
     plays gets quieter -- and a hard-panned one is silent on the far side. Q8 gains. */
static INT32 GusPanLeft(BYTE pan) { INT32 gain = (INT32)(GUS_PAN_RIGHT - (pan & GUS_PAN_MASK)) * GUS_PAN_GAIN_SPAN / GUS_PAN_RIGHT; return gain > GUS_PAN_UNITY ? GUS_PAN_UNITY : gain; }
static INT32 GusPanRight(BYTE pan) { INT32 gain = (INT32)(pan & GUS_PAN_MASK) * GUS_PAN_GAIN_SPAN / GUS_PAN_RIGHT;        return gain > GUS_PAN_UNITY ? GUS_PAN_UNITY : gain; }
static INT16 GusClip(INT32 value) { return (INT16)(value > GUS_SAMPLE_MAX ? GUS_SAMPLE_MAX : (value < GUS_SAMPLE_MIN ? GUS_SAMPLE_MIN : value)); }

/* One render loop, two output shapes (as vdd_sb): `isStereo` 0 writes the mono sum it
   always did, 1 writes panned L/R pairs at output[2i], output[2i+1]. */
static VOID GusRender(PGUS_STATE state, INT16 *output, UINT32 count, INT isStereo)
{
    UINT32 sampleIndex, voiceIndex, nanoseconds = GUS_NANOSECONDS_PER_SECOND / (VddGusRateHz(state) ? VddGusRateHz(state) : GUS_FALLBACK_RATE_HZ);
    state->Renders++;
    GusDmaTry(state);                                /* a DMA that was waiting on the 8237 */
    for (sampleIndex = 0; sampleIndex < count; ++sampleIndex) {
        INT32 sum = 0, sumLeft = 0, sumRight = 0;
        INT16 mono;
        if (state->Dram && (state->ResetRegister & GUS_RESET_RUNNING) == GUS_RESET_RUNNING) {   /* running, DAC enabled */
            for (voiceIndex = 0; voiceIndex < state->ActiveVoices && voiceIndex < GUS_VOICES; ++voiceIndex) {
                PGUS_VOICE voice = &state->Voices[voiceIndex];
                UINT32 address = voice->Position >> GUS_POSITION_FRACTION_BITS, fraction = voice->Position & GUS_POSITION_FRACTION_MASK;
                INT32 sample0, sample1, sample, gain;
                gain = (INT32)VddGusVolumeGain((WORD)(voice->Volume >> GUS_VOLUME_SHIFT));
                if (gain) {
                    sample0 = GusFetch(state, voice, address);
                    sample1 = GusFetch(state, voice, address + 1);
                    sample  = sample0 + (((sample1 - sample0) * (INT32)fraction) >> GUS_POSITION_FRACTION_BITS);
                    sample  = (sample * gain) >> GUS_GAIN_SHIFT;
                    sum += sample;
                    if (isStereo) { sumLeft += (sample * GusPanLeft(voice->Pan)) >> GUS_PAN_SHIFT;
                                  sumRight += (sample * GusPanRight(voice->Pan)) >> GUS_PAN_SHIFT; }
                }
                GusVoiceStep(state, voice);
                GusRampStep(voice);
            }
        }
        GusTimers(state, nanoseconds);
        GusRecord(state, nanoseconds);
        /* #190: 2X0 bit 1 = line out DISABLED (active-high, ref §5). The voices still
           run -- the GF1 does not know the amplifier is off -- but nothing is heard. */
        if (state->MixControl & GUS_MIX_LINE_OUT_OFF) { sum = sumLeft = sumRight = 0; state->OutputMuted++; }
        mono = GusClip(sum >> 1);                    /* headroom for many voices */
        if (isStereo) { output[GUS_STEREO_CHANNELS*sampleIndex] = GusClip(sumLeft >> 1); output[GUS_STEREO_CHANNELS*sampleIndex+1] = GusClip(sumRight >> 1); }
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
    state->MixControl = GUS_MIX_ULTRINIT;
    state->Jumper = GUS_JUMPER_ULTRINIT;
    {
        BYTE midiIrq = state->MidiIrq ? state->MidiIrq : state->Irq;
        BYTE recordDma = state->RecordDma  ? state->RecordDma  : state->DmaChannel;
        state->IrqLatch = (BYTE)(GusCodeOf(g_GusIrqMap, state->Irq)
                      | (midiIrq == state->Irq ? GUS_LATCH_COMBINE : (GusCodeOf(g_GusIrqMap, midiIrq) << GUS_LATCH_SECOND_SHIFT)));
        state->DmaLatch = (BYTE)(GusCodeOf(g_GusDmaMap, state->DmaChannel)
                      | (recordDma == state->DmaChannel ? GUS_LATCH_COMBINE : (GusCodeOf(g_GusDmaMap, recordDma) << GUS_LATCH_SECOND_SHIFT)));
        state->Gf1IrqLine = state->Irq; state->DramDmaLine = state->DmaChannel;
        state->MidiIrqLine = midiIrq;     state->RecordDmaLine  = recordDma;
    }
    state->MidiControl = GUS_ACIA_CONTROL_POWER_UP; state->MidiStatus = GUS_ACIA_TRANSMIT_EMPTY; state->MidiReceive = 0;   /* reset, then released */
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
    if (VddClaimPorts(bus, state->BasePort, (WORD)(state->BasePort + GUS_PORT_LOW_LAST), GusPortIn, GusPortOut, state)) return GUS_FAILED;
    if (VddClaimPorts(bus, (WORD)(state->BasePort + GUS_PORT_HIGH_FIRST), (WORD)(state->BasePort + GUS_PORT_HIGH_LAST), GusPortIn, GusPortOut, state)) return GUS_FAILED;
    return 0;
}
