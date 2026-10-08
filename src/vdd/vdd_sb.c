/* vdd_sb.c -- see vdd_sb.h.  Sound Blaster 16 DSP, mixer and DMA playback engine,
 * on the VDD bus.  Pure C, no <windows.h>. */
#include "vdd_sb.h"

/* The card's ports, as offsets from its base. */
#define SB_PORT_FM_ADDRESS          0x0
#define SB_PORT_FM_DATA             0x1
#define SB_PORT_FM_ADDRESS_HIGH     0x2     /* array 1 on an OPL3                        */
#define SB_PORT_FM_DATA_HIGH        0x3
#define SB_PORT_MIXER_ADDRESS       0x4
#define SB_PORT_MIXER_DATA          0x5
#define SB_PORT_DSP_RESET           0x6
#define SB_PORT_ADLIB_ADDRESS       0x8     /* the AdLib-compatible pair                 */
#define SB_PORT_ADLIB_DATA          0x9
#define SB_PORT_DSP_READ            0xA
#define SB_PORT_DSP_WRITE           0xC
#define SB_PORT_DSP_READ_STATUS     0xE     /* and the 8-bit IRQ ack                     */
#define SB_PORT_DSP_ACK16           0xF
#define SB_FLOATING_BUS             0xFF
#define SB_FAILED                   (-1)
/* The DSP. */
#define SB_DSP_RESET_ASSERT         1
#define SB_DSP_READY                0xAA    /* the reset handshake's answer              */
#define SB_DSP_WRITE_READY          0x00    /* write status: never busy                  */
#define SB_DSP_DATA_AVAILABLE       0xFF    /* read status bit 7: a byte is waiting      */
#define SB_DSP_NO_DATA              0x7F
#define SB_DSP_MAJOR_SB16           4
#define SB_DSP_DIRECT_DAC           0x10
#define SB_DSP_DMA8_SINGLE          0x14
#define SB_DSP_DMA8_SINGLE_X16      0x16
#define SB_DSP_DMA8_SINGLE_X17      0x17
#define SB_DSP_DMA8_AUTO            0x1C
#define SB_DSP_DMA8_AUTO_X2C        0x2C
#define SB_DSP_TIME_CONSTANT        0x40
#define SB_DSP_OUTPUT_RATE          0x41
#define SB_DSP_INPUT_RATE           0x42
#define SB_DSP_BLOCK_SIZE           0x48
#define SB_DSP_SILENCE              0x80
#define SB_DSP_DMA8_AUTO_HIGH_SPEED 0x90
#define SB_DSP_DMA8_SINGLE_HIGH_SPEED 0x91
#define SB_DSP_PROGRAMMED_FIRST     0xB0    /* SB16 programmed transfers, B0h-CFh        */
#define SB_DSP_PROGRAMMED_LAST      0xCF
#define SB_DSP_PROGRAMMED_TYPE_MASK 0xF0
#define SB_DSP_PROGRAMMED_16BIT     0xB0
#define SB_DSP_PROGRAMMED_AUTO_INIT 0x04
#define SB_DSP_PROGRAMMED_INPUT     0x08
#define SB_DSP_MODE_SIGNED          0x10    /* the programmed transfer's mode byte       */
#define SB_DSP_MODE_STEREO          0x20
#define SB_DSP_PAUSE_DMA8           0xD0
#define SB_DSP_SPEAKER_ON           0xD1
#define SB_DSP_SPEAKER_OFF          0xD3
#define SB_DSP_CONTINUE_DMA8        0xD4
#define SB_DSP_PAUSE_DMA16          0xD5
#define SB_DSP_CONTINUE_DMA16       0xD6
#define SB_DSP_EXIT_AUTO_DMA16      0xD9
#define SB_DSP_EXIT_AUTO_DMA8       0xDA
#define SB_DSP_IDENTIFY             0xE0
#define SB_DSP_VERSION              0xE1
#define SB_DSP_COPYRIGHT            0xE3
#define SB_DSP_WRITE_TEST           0xE4
#define SB_DSP_FORCE_IRQ_XF2        0xF2
#define SB_DSP_FORCE_IRQ_XF3        0xF3
#define SB_ARGUMENTS_WORD           2
#define SB_ARGUMENTS_PROGRAMMED     3       /* mode byte + 16-bit length                 */
#define SB_TIME_CONSTANT_BASE       256u    /* the DSP stores 256 - 1000000/rate         */
#define SB_MICROSECONDS_PER_SECOND  1000000u
#define SB_FALLBACK_RATE_HZ         4000u
#define SB_DEFAULT_RATE_HZ          22050
/* The mixer. */
#define SB_MIXER_VOICE_VOLUME       0x04
#define SB_MIXER_STEREO_SWITCH      0x0E
#define SB_MIXER_STEREO_BIT         0x02
#define SB_MIXER_MASTER_VOLUME      0x22
#define SB_MIXER_VOLUME_POWER_UP    0xCC
#define SB_MIXER_IRQ_SELECT         0x80
#define SB_MIXER_DMA_SELECT         0x81
#define SB_MIXER_IRQ_STATUS         0x82
#define SB_IRQ_SELECT_2             0x01
#define SB_IRQ_SELECT_5             0x02
#define SB_IRQ_SELECT_7             0x04
#define SB_IRQ_SELECT_10            0x08
#define SB_IRQ_STATUS_NONE          0x00
#define SB_IRQ_STATUS_DMA8          0x01
#define SB_IRQ_STATUS_DMA16         0x02
#define SB_DMA8_CHANNELS            4
#define SB_DMA16_FIRST              5
#define SB_DMA16_LAST               7
/* Samples. */
#define SB_DMA_CHANNEL_MASK         7
#define SB_FRAME_BYTES_MAX          4       /* a 16-bit stereo frame                     */
#define SB_SAMPLE16_BYTES           2u
#define SB_SAMPLE8_SILENCE          128     /* 8-bit SB data is unsigned: 0x80 is silence */
#define SB_SAMPLE8_SCALE            256
#define SB_MONO_FOLD_DIVISOR        2       /* the average of L and R                    */
/* The gate and the replay detector. */
#define SB_GATE_SAFETY_BLOCKS       2u
#define SB_RUN_LAST_BUCKET          7
#define SB_REPLAY_NUMERATOR         9u      /* >= 9/10 identical: a replayed block       */
#define SB_REPLAY_DENOMINATOR       10u
#define SB_BLOCK_MIN_EMPTY          0xFFFFFFFFu
#define SB_LAP_OFFSET_NONE          0xFFFFFFFFu

/* --- the DSP's little output queue ---------------------------------------- */
/* Reads from 2xA come from here, and 2xE reports whether anything is waiting.
   The reset handshake, the version query and the identify command all answer
   through this queue, which is why detection is really a queue test. */
static VOID SbOutputQueuePush(PSB_STATE state, BYTE value)
{
    if (state->OutputQueueLength >= SB_OUTPUT_QUEUE_MAX) return;
    state->OutputQueue[(state->OutputQueueHead + state->OutputQueueLength) % SB_OUTPUT_QUEUE_MAX] = value;
    state->OutputQueueLength++;
}
static BYTE SbOutputQueuePop(PSB_STATE state)
{
    BYTE value;
    if (!state->OutputQueueLength) return SB_FLOATING_BUS; /* nothing waiting: bus floats     */
    value = state->OutputQueue[state->OutputQueueHead];
    state->OutputQueueHead = (BYTE)((state->OutputQueueHead + 1) % SB_OUTPUT_QUEUE_MAX);
    state->OutputQueueLength--;
    return value;
}

INT g_SbAbsent = 0;       /* nosb.flag -- see the DSP-reset case below */
BYTE g_SbVersionMajor = SB_DSP_VERSION_MAJOR;   /* dspver.txt -- see vdd_sb.h */
/* ⚠ THE ACK GATE IS A DELIBERATE DEVIATION FROM THE HARDWARE AND DEFAULTS OFF.
     A real SB16 in auto-init raises an IRQ per block whether or not the previous one
     was acknowledged, and tests/unit/sb_test.c asserts exactly that ('auto-init: an
     IRQ per block, continuously'). Arming the gate by default would have quietly
     broken that assertion -- the battery caught it on the first build, which is what
     it is for. So the gate is opt-in until it has been shown to earn the trade. */
INT g_SbGate = 0;         /* sbgate.txt */
BYTE g_SbVersionMinor = SB_DSP_VERSION_MINOR;

static VOID SbDspSoftReset(PSB_STATE state)
{
    state->Command = 0; state->ArgumentCount = 0; state->ArgumentsWanted = 0;
    state->OutputQueueHead = state->OutputQueueLength = 0;
    state->TransferMode = SB_TRANSFER_IDLE;
    state->BlockRemaining = 0;
    state->IsPaused = 0;
    state->IsIrqPending = 0;
}

/* --- transfer programming ------------------------------------------------- */
static VOID SbStartBlock(PSB_STATE state, UINT32 bytes, INT isAutoInit)
{
    /* SB16 only, exactly as VDMSound scopes it (getDSPVersion() >= 0x0400). */
    state->GateMode    = (BYTE)((g_SbGate == SB_GATE_ACK && g_SbVersionMajor >= SB_DSP_MAJOR_SB16) ? SB_GATE_ACK
                             : (g_SbGate == SB_GATE_POLL) ? SB_GATE_POLL : SB_GATE_OFF);
    state->GateWait  = 0;
    state->BlockLength  = bytes ? bytes : 1;
    state->BlockRemaining = state->BlockLength;
    state->TransferMode  = isAutoInit ? SB_TRANSFER_AUTO : SB_TRANSFER_SINGLE;
    state->IsPaused     = 0;
}

/* Time constant -> sample rate. The DSP stores 256 - 1000000/rate, so a game that
   asks for 11025 Hz writes 165; we invert it exactly the same way. */
static UINT32 SbRateFromTimeConstant(BYTE timeConstant)
{
    UINT32 divisor = SB_TIME_CONSTANT_BASE - timeConstant;
    return divisor ? (SB_MICROSECONDS_PER_SECOND / divisor) : SB_FALLBACK_RATE_HZ;
}

/* #231: the commands a DSP 3.xx (SB Pro) does not have -- the SB16's rate commands,
   programmed transfers and 16-bit DMA pause/continue/exit. On an SB Pro they are
   unknown opcodes: ignored, and taking NO argument bytes, as on the real card. */
static INT SbIsSb16Only(BYTE command)
{
    return (command >= SB_DSP_PROGRAMMED_FIRST && command <= SB_DSP_PROGRAMMED_LAST) || command == SB_DSP_OUTPUT_RATE || command == SB_DSP_INPUT_RATE
        || command == SB_DSP_PAUSE_DMA16 || command == SB_DSP_CONTINUE_DMA16 || command == SB_DSP_EXIT_AUTO_DMA16;
}

/* How many argument bytes each command consumes after its opcode. */
static BYTE SbCommandArguments(PCSB_STATE state, BYTE command)
{
    if (state->Model == SB_MODEL_SBPRO && SbIsSb16Only(command)) return 0;   /* #231 */
    if (command >= SB_DSP_PROGRAMMED_FIRST && command <= SB_DSP_PROGRAMMED_LAST) return SB_ARGUMENTS_PROGRAMMED;       /* mode byte + 16-bit length      */
    switch (command) {
    case SB_DSP_DIRECT_DAC: return 1;                        /* direct DAC sample              */
    case SB_DSP_DMA8_SINGLE: case SB_DSP_DMA8_SINGLE_X16: case SB_DSP_DMA8_SINGLE_X17: return SB_ARGUMENTS_WORD;  /* 8-bit single-cycle DMA length  */
    case SB_DSP_TIME_CONSTANT: return 1;                        /* time constant                  */
    case SB_DSP_OUTPUT_RATE: case SB_DSP_INPUT_RATE: return SB_ARGUMENTS_WORD;             /* output / input rate (big-endian) */
    case SB_DSP_BLOCK_SIZE: return SB_ARGUMENTS_WORD;                        /* DMA block size                 */
    case SB_DSP_SILENCE: return SB_ARGUMENTS_WORD;                        /* silence period                 */
    case SB_DSP_IDENTIFY: return 1;                        /* identify                       */
    case SB_DSP_WRITE_TEST: return 1;                        /* write test register            */
    default:   return 0;
    }
}

static VOID SbExecute(PSB_STATE state)
{
    BYTE command = state->Command;
    const BYTE *arguments = state->Arguments;

    /* Which transfer command does the guest actually use? 0x14 is SINGLE-CYCLE (the
       DSP stops at every block end); 0x1C/0x2C and 0xB0-0xCF with bit 2 are AUTO-INIT
       (it streams). That one bit decides whether the output is gapped by construction,
       and it has never been recorded. */
    state->CommandHistogram[command]++;
    if (state->Model == SB_MODEL_SBPRO && SbIsSb16Only(command)) return;     /* #231: not a 3.02 command */

    if (command >= SB_DSP_PROGRAMMED_FIRST && command <= SB_DSP_PROGRAMMED_LAST) {   /* SB16 programmed transfers      */
        INT is16Bit   = (command & SB_DSP_PROGRAMMED_TYPE_MASK) == SB_DSP_PROGRAMMED_16BIT;
        INT isAutoInit = (command & SB_DSP_PROGRAMMED_AUTO_INIT) != 0;
        INT isInput  = (command & SB_DSP_PROGRAMMED_INPUT) != 0;   /* A/D: we do not record           */
        UINT32 units = (UINT32)arguments[1] | ((UINT32)arguments[2] << BYTE_SHIFT);
        state->Is16Bit  = (BYTE)is16Bit;
        state->IsSigned = (arguments[0] & SB_DSP_MODE_SIGNED) ? 1 : 0;
        state->IsStereo = (arguments[0] & SB_DSP_MODE_STEREO) ? 1 : 0;
        state->IsLegacyTransfer = 0;            /* SB16: the rate IS the frame rate */
        if (isInput) { state->TransferMode = SB_TRANSFER_IDLE; return; }
        SbStartBlock(state, (units + 1) * (is16Bit ? SB_SAMPLE16_BYTES : 1u), isAutoInit);
        return;
    }
    switch (command) {
    case SB_DSP_DIRECT_DAC:                                  /* direct DAC write: no DMA       */
        break;
    /* ── #189: THE SB PRO'S STEREO IS A MIXER SWITCH. A DSP 1.x-3.x output command is
         mono or stereo according to mixer register 0Eh bit 1 at the moment it starts,
         and the time constant was programmed for BOTH channels -- so a stereo frame
         (one byte each side) comes at half the byte rate (IsLegacyTransfer + VddSbFrameHz).
         0x1C used to keep whatever stereo flag the last SB16 command had left behind.
         0x90/0x91 are the high-speed forms (length from 0x48), which is how an SB Pro
         program plays 22 kHz stereo; they were not modelled at all. */
    case SB_DSP_DMA8_SINGLE: case SB_DSP_DMA8_SINGLE_X16: case SB_DSP_DMA8_SINGLE_X17:            /* 8-bit single-cycle DMA output  */
        state->Is16Bit = 0; state->IsSigned = 0; state->IsLegacyTransfer = 1;
        state->IsStereo = (state->Mixer[SB_MIXER_STEREO_SWITCH] & SB_MIXER_STEREO_BIT) ? 1 : 0;
        SbStartBlock(state, ((UINT32)arguments[0] | ((UINT32)arguments[1] << BYTE_SHIFT)) + 1, 0);
        break;
    case SB_DSP_DMA8_AUTO: case SB_DSP_DMA8_AUTO_X2C: case SB_DSP_DMA8_AUTO_HIGH_SPEED:            /* 8-bit auto-init (0x90: high-speed) */
        state->Is16Bit = 0; state->IsSigned = 0; state->IsLegacyTransfer = 1;
        state->IsStereo = (state->Mixer[SB_MIXER_STEREO_SWITCH] & SB_MIXER_STEREO_BIT) ? 1 : 0;
        SbStartBlock(state, state->BlockLength, 1);
        break;
    case SB_DSP_DMA8_SINGLE_HIGH_SPEED:                                  /* high-speed 8-bit single-cycle   */
        state->Is16Bit = 0; state->IsSigned = 0; state->IsLegacyTransfer = 1;
        state->IsStereo = (state->Mixer[SB_MIXER_STEREO_SWITCH] & SB_MIXER_STEREO_BIT) ? 1 : 0;
        SbStartBlock(state, state->BlockLength, 0);
        break;
    case SB_DSP_TIME_CONSTANT:
        state->RateHz = SbRateFromTimeConstant(arguments[0]);
        break;
    case SB_DSP_OUTPUT_RATE: case SB_DSP_INPUT_RATE:                       /* rate is BIG-endian here         */
        state->RateHz = ((UINT32)arguments[0] << BYTE_SHIFT) | arguments[1];
        break;
    case SB_DSP_BLOCK_SIZE:                                  /* block size for auto-init        */
        state->BlockLength = ((UINT32)arguments[0] | ((UINT32)arguments[1] << BYTE_SHIFT)) + 1;
        break;
    case SB_DSP_PAUSE_DMA8: state->IsPaused = 1; break;      /* pause 8-bit DMA                 */
    case SB_DSP_SPEAKER_ON: state->IsSpeakerOn = 1; break;
    case SB_DSP_SPEAKER_OFF: state->IsSpeakerOn = 0; break;
    case SB_DSP_CONTINUE_DMA8: state->IsPaused = 0; break;      /* continue 8-bit DMA              */
    case SB_DSP_PAUSE_DMA16: state->IsPaused = 1; break;      /* pause 16-bit DMA                */
    case SB_DSP_CONTINUE_DMA16: state->IsPaused = 0; break;      /* continue 16-bit DMA             */
    case SB_DSP_EXIT_AUTO_DMA8: case SB_DSP_EXIT_AUTO_DMA16:                       /* leave auto-init after this block */
        if (state->TransferMode == SB_TRANSFER_AUTO) state->TransferMode = SB_TRANSFER_SINGLE;
        break;
    case SB_DSP_IDENTIFY:                                  /* identify: reply with ~arg       */
        SbOutputQueuePush(state, (BYTE)~arguments[0]);
        break;
    case SB_DSP_VERSION:                                  /* DSP version                     */
        SbOutputQueuePush(state, g_SbVersionMajor);
        SbOutputQueuePush(state, g_SbVersionMinor);
        break;
    case SB_DSP_COPYRIGHT:                                  /* copyright string: NUL is enough */
        SbOutputQueuePush(state, 0);
        break;
    case SB_DSP_FORCE_IRQ_XF2: case SB_DSP_FORCE_IRQ_XF3:                       /* force an IRQ (drivers test wiring) */
        state->IsIrqPending = 1;
        VddRaiseIrq(state->Bus, state->Irq);
        break;
    default:
        break;                                  /* unknown commands are ignored     */
    }
}

static VOID SbDspWrite(PSB_STATE state, BYTE value)
{
    state->DspWrites++;
    if (state->ArgumentsWanted) {               /* collecting arguments            */
        if (state->ArgumentCount < SB_ARGUMENTS_MAX) state->Arguments[state->ArgumentCount++] = value;
        if (state->ArgumentCount >= state->ArgumentsWanted) { SbExecute(state); state->Command = 0; state->ArgumentsWanted = 0; state->ArgumentCount = 0; }
        return;
    }
    state->Command = value;
    state->ArgumentCount = 0;
    state->ArgumentsWanted = SbCommandArguments(state, value);
    if (!state->ArgumentsWanted) { SbExecute(state); state->Command = 0; }
}

/* --- ports ---------------------------------------------------------------- */
static VOID SbPortOut(PVOID context, WORD port, BYTE width, UINT32 value)
{
    PSB_STATE state = (PSB_STATE)context;
    BYTE offset = (BYTE)(port - state->BasePort), byteValue = (BYTE)value;
    (VOID)width;
    switch (offset) {
    /* ── FM. 2x8/2x9 are the AdLib-compatible pair (0x388/0x389) on every card.
         2x0-2x3 are the chip's own four ports on an SB16/AWE32's OPL3: 2x0 the
         array-0 address, 2x2 the ARRAY-1 address (A1 high, = 0x38A), 2x1/2x3 data
         (#232). With an OPL2 fitted there is no array 1, and 2x2 keeps its old
         meaning of another array-0 address mirror -- the SB Pro 1's second (right)
         OPL2 is not modelled. */
    case SB_PORT_FM_ADDRESS: case SB_PORT_ADLIB_ADDRESS:                         /* FM address, array 0             */
        if (state->Opl) VddOplWriteAddress(state->Opl, 0, byteValue);
        break;
    case SB_PORT_FM_ADDRESS_HIGH:                                   /* FM address, array 1 on an OPL3  */
        if (state->Opl) VddOplWriteAddress(state->Opl, state->Opl->IsOpl3 ? 1 : 0, byteValue);
        break;
    case SB_PORT_FM_DATA: case SB_PORT_FM_DATA_HIGH: case SB_PORT_ADLIB_DATA:               /* FM data                         */
        if (state->Opl) VddOplWriteData(state->Opl, byteValue);
        break;
    case SB_PORT_MIXER_ADDRESS: state->MixerIndex = byteValue; break;
    case SB_PORT_MIXER_DATA: state->Mixer[state->MixerIndex] = byteValue; break;
    case SB_PORT_DSP_RESET:                                   /* DSP reset                       */
        /* The handshake: 1 then 0. Only the falling edge arms 0xAA, which is what
           a detect is really looking for. */
        if (byteValue & SB_DSP_RESET_ASSERT) { state->IsResetAsserted = 1; SbDspSoftReset(state); }
        else if (state->IsResetAsserted) {
            state->IsResetAsserted = 0;
            /* ► `nosb.flag`: ANSWER THE PROBE WITH SILENCE, i.e. behave as a machine
                 with no Sound Blaster fitted. Withholding the 0xAA is exactly how a
                 card-less PC fails a detect, so a client takes its own no-sound path
                 rather than being lied to about a device we then cannot service.
                 This is a diagnostic knob, not a policy: Doom dies inside DMX sound
                 init, and this is the way to find out what it does WITHOUT sound
                 without going through the command line (which is separately broken).
                 Absent file = fitted, exactly as before. */
            if (!g_SbAbsent) SbOutputQueuePush(state, SB_DSP_READY);
        }
        break;
    case SB_PORT_DSP_WRITE: SbDspWrite(state, byteValue); break;     /* DSP command / data              */
    default: break;
    }
}

static VOID SbPortIn(PVOID context, WORD port, BYTE width, UINT32 *value)
{
    PSB_STATE state = (PSB_STATE)context;
    BYTE offset = (BYTE)(port - state->BasePort);
    (VOID)width;
    switch (offset) {
    case SB_PORT_FM_ADDRESS: case SB_PORT_ADLIB_ADDRESS:                         /* FM status through the mirror    */
        *value = state->Opl ? VddOplReadStatus(state->Opl) : SB_FLOATING_BUS;
        break;
    case SB_PORT_FM_ADDRESS_HIGH:                                   /* OPL3: status at A1 high too     */
        *value = (state->Opl && state->Opl->IsOpl3) ? VddOplReadStatus(state->Opl) : SB_FLOATING_BUS;
        break;
    case SB_PORT_MIXER_DATA:                                   /* mixer data                      */
        /* 0x82 is the IRQ-status register: bit 0 = 8-bit DMA, bit 1 = 16-bit. */
        if (state->MixerIndex == SB_MIXER_IRQ_STATUS) {
            /* ── THIS ANSWER DECIDES WHETHER THE GUEST REFILLS. ──────────────────
                 DMX's SB IRQ handler (DOOM.EXE 0x53274) does almost nothing
                 unconditionally: for a DSP reporting >= 4.00 it asks mixer register
                 0x82 whether the interrupt was really the Blaster's, and if neither
                 the 8-bit nor the 16-bit bit is set it RETURNS WITHOUT REFILLING.
                 Blocks complete 81/s, IRQ5 is delivered 99.5% of the time, and the
                 mixer is only entered 58/s -- so ~28% of delivered SB interrupts are
                 being turned away, and this register is the only thing that can turn
                 them away. Count what we answer.
                 `IsIrqPending` is a single FLAG, and `case 0xE` clears it -- but 0xE
                 is also the DSP data-ready poll, so any read of it for another
                 purpose disarms the next ISR's check. */
            BYTE status82 = (BYTE)(state->IsIrqPending ? (state->Is16Bit ? SB_IRQ_STATUS_DMA16 : SB_IRQ_STATUS_DMA8) : SB_IRQ_STATUS_NONE);
            state->Mixer82Reads++;
            if (!status82) state->Mixer82Zero++;
            *value = status82;
        }
        /* ── ★★★★ 0x80 / 0x81 ARE "WHICH IRQ AND DMA AM I ON?", AND WE ANSWERED
             **NONE**. (session 59) ──────────────────────────────────────────────────
             These fell through to the plain mixer RAM, which is zero until something
             writes it -- and on a real SB16 a zero here does not mean "default", it
             means NO IRQ SELECTED and NO DMA CHANNEL SELECTED. A driver that
             autodetects instead of trusting BLASTER therefore learns the card is
             unconfigured, and any transfer it starts can never be announced to it.
           ★ MEASURED ON ZAR (GH #23). Its Miles driver resets the DSP (we answer 0xAA
             correctly), asks for the version, then:
                 out 224 <- 80 / in 225 -> 00      "which IRQ?"  -> none
                 out 224 <- 81 / in 225 -> 00      "which DMA?"  -> none
             ...and then programs `40 D3` (22222 Hz) and `14 0F 00` -- an 8-bit
             SINGLE-CYCLE 16-byte transfer, which is the classic init-time DMA/IRQ
             SELF-TEST -- and waits for a completion interrupt it has no idea how to
             receive. The guest spins in real mode at 0x34d3:0x06b1 until the watchdog
             kills it. The block itself is fine: `blocks=1` and TransferMode back to
             SB_TRANSFER_IDLE is exactly what a completed single-cycle transfer looks like.
           ► ANSWER FROM THE CARD'S OWN CONFIGURATION, DERIVED RATHER THAN STORED, so
             it cannot drift from state->Irq / state->Dma8 / state->Dma16 -- which are the same
             numbers that go into BLASTER (see dos_env.h and the note in main.c). A
             guest that reads these and a guest that parses BLASTER must not be told
             two different things about one card.
           ⚠ The bit assignments are the SB16's and are not a free choice:
             0x80 bit0=IRQ2 bit1=IRQ5 bit2=IRQ7 bit3=IRQ10;
             0x81 bit0=DMA0 bit1=DMA1 bit3=DMA3 bit5=DMA5 bit6=DMA6 bit7=DMA7.
             An IRQ or channel outside those sets has no encoding, so it reports 0 --
             the honest answer, and the same one the hardware would give. */
        else if (state->MixerIndex == SB_MIXER_IRQ_SELECT) {
            BYTE mask = 0;
            if (state->Irq == 2)  mask = SB_IRQ_SELECT_2;
            else if (state->Irq == 5)  mask = SB_IRQ_SELECT_5;
            else if (state->Irq == 7)  mask = SB_IRQ_SELECT_7;
            else if (state->Irq == 10) mask = SB_IRQ_SELECT_10;
            *value = mask;
        }
        else if (state->MixerIndex == SB_MIXER_DMA_SELECT) {
            BYTE mask = 0;
            if (state->Dma8 < SB_DMA8_CHANNELS)  mask |= (BYTE)(1u << state->Dma8);
            if (state->Dma16 >= SB_DMA16_FIRST && state->Dma16 <= SB_DMA16_LAST) mask |= (BYTE)(1u << state->Dma16);
            *value = mask;
        }
        else
            *value = state->Mixer[state->MixerIndex];
        break;
    case SB_PORT_DSP_READ: *value = SbOutputQueuePop(state); break;      /* DSP read data                   */
    case SB_PORT_DSP_WRITE: *value = SB_DSP_WRITE_READY; break;             /* write status: never busy        */
    case SB_PORT_DSP_READ_STATUS:                                   /* read status + 8-bit IRQ ack     */
        *value = (BYTE)(state->OutputQueueLength ? SB_DSP_DATA_AVAILABLE : SB_DSP_NO_DATA);   /* bit 7 = data available    */
        if (!state->Is16Bit) state->IsIrqPending = 0;
        break;
    case SB_PORT_DSP_ACK16:                                   /* 16-bit IRQ ack                  */
        state->IsIrqPending = 0;
        *value = SB_FLOATING_BUS;
        break;
    default: *value = SB_FLOATING_BUS; break;
    }
}

/* --- playback ------------------------------------------------------------- */
/* Pull one sample's worth of bytes through the DMA controller and turn it into a
   signed 16-bit value. 8-bit SB data is UNSIGNED (0x80 is silence) unless the
   game said otherwise; 16-bit is signed. Returns the MONO fold (the average, as it
   always has); the L/R pair itself is left in LastLeft/LastRight for the stereo render. */
static INT16 SbFetchSample(PSB_STATE state, INT *isEnded)
{
    BYTE channel = state->Is16Bit ? state->Dma16 : state->Dma8;
    BYTE rawBytes[SB_FRAME_BYTES_MAX];
    UINT32 bytesWanted = (UINT32)(state->Is16Bit ? SB_SAMPLE16_BYTES : 1) * (state->IsStereo ? SB_STEREO_CHANNELS : 1u);
    UINT32 bytesRead;
    INT32 left = 0, right = 0;
    INT isTerminalCount = 0;

    *isEnded = 0;
    if (!state->Dma) return 0;

    /* ── REPLAY CHECK: capture the ring offset BEFORE the fetch advances it. ──────
         Ring offset is the distance of the current address from the channel's base;
         everything else about the comparison hangs off that, so it must be sampled
         before VddDmaRead() walks the 8237. */
    { PCDMA_CHANNEL channelState = &state->Dma->Channels[channel & SB_DMA_CHANNEL_MASK];
      UINT32 ringLength = (UINT32)channelState->BaseCount + 1u;
      UINT32 ringOffset = (UINT32)(WORD)(channelState->CurrentAddress - channelState->BaseAddress);
      if (ringLength != state->LapLength) {   /* (re)programmed: start a fresh lap */
          state->LapLength  = ringLength;
          state->LapSeen = 0;
          state->BlockSame = state->BlockBytes = 0;
          state->BlockMin  = SB_BLOCK_MIN_EMPTY; state->BlockMax = 0;
          if (ringLength > SB_LAP_MAX) ++state->LapTooBig;
      }
      state->LapOffset = (ringLength && ringLength <= SB_LAP_MAX) ? (ringOffset % ringLength) : SB_LAP_OFFSET_NONE; }

    bytesRead = VddDmaRead(state->Dma, channel, rawBytes, bytesWanted, &isTerminalCount);
    if (bytesRead < bytesWanted) { *isEnded = 1; return 0; }

    if (state->LapOffset != SB_LAP_OFFSET_NONE) {
        UINT32 byteIndex;
        for (byteIndex = 0; byteIndex < bytesWanted; ++byteIndex) {
            UINT32 lapIndex = (state->LapOffset + byteIndex) % state->LapLength;
            /* Only compare once a whole lap has been recorded, or the shadow's
               zero-fill would read as a mountain of false "replays" at startup. */
            if (state->LapSeen >= state->LapLength) {
                ++state->BlockBytes; ++state->LapTotal;
                if (state->LapBuffer[lapIndex] == rawBytes[byteIndex]) { ++state->BlockSame; ++state->LapSame; }
                /* The block's own dynamic range, from the bytes already in hand. */
                if (rawBytes[byteIndex] < state->BlockMin) state->BlockMin = rawBytes[byteIndex];
                if (rawBytes[byteIndex] > state->BlockMax) state->BlockMax = rawBytes[byteIndex];
            }
            state->LapBuffer[lapIndex] = rawBytes[byteIndex];
        }
        state->LapSeen += bytesWanted;
    }

    if (state->Is16Bit) {
        left = (INT16)((WORD)rawBytes[0] | ((WORD)rawBytes[1] << BYTE_SHIFT));
        right = state->IsStereo ? (INT16)((WORD)rawBytes[2] | ((WORD)rawBytes[3] << BYTE_SHIFT)) : left;
    } else {
        left = state->IsSigned ? (INT8)rawBytes[0] * SB_SAMPLE8_SCALE : ((INT32)rawBytes[0] - SB_SAMPLE8_SILENCE) * SB_SAMPLE8_SCALE;
        right = state->IsStereo
              ? (state->IsSigned ? (INT8)rawBytes[1] * SB_SAMPLE8_SCALE : ((INT32)rawBytes[1] - SB_SAMPLE8_SILENCE) * SB_SAMPLE8_SCALE)
              : left;
    }
    if (state->CaptureBuffer && state->CaptureLength + bytesWanted <= state->CaptureCapacity) {
        UINT32 byteIndex;
        for (byteIndex = 0; byteIndex < bytesWanted; ++byteIndex) state->CaptureBuffer[state->CaptureLength++] = rawBytes[byteIndex];
    }
    state->BlockRemaining = (state->BlockRemaining > bytesWanted) ? (state->BlockRemaining - bytesWanted) : 0;
    state->LastLeft = (INT16)left; state->LastRight = (INT16)right;   /* #189: the pair, for stereo */
    return (INT16)((left + right) / SB_MONO_FOLD_DIVISOR);
}

/* #189: ONE render loop, two output shapes. `isStereo` 0 writes output[n] as it always
   did (the average); 1 writes the pair at output[2n], output[2n+1]. The mono entry point is
   kept so sb_test still checks exactly what it checked. */
#define SB_PUT(frame, mono, left, right) do { if (isStereo) { output[2*(frame)] = (left); output[2*(frame)+1] = (right); } \
                                   else output[(frame)] = (mono); } while (0)
static UINT32 SbRender(PSB_STATE state, INT16 *output, UINT32 frames, INT isStereo)
{
    UINT32 frame;
    for (frame = 0; frame < frames; ++frame) {
        INT isEnded = 0;
        if (state->TransferMode == SB_TRANSFER_IDLE || state->IsPaused) {
            SB_PUT(frame, 0, 0, 0);
            if (state->IsPaused) state->OutputPaused++; else state->OutputIdle++;
            state->IdleRun++;
            continue;
        }
        /* ── ⚠ THE ACK GATE. DO NOT ENTER THE NEXT BLOCK UNTIL THE GUEST HAS
             ACKNOWLEDGED THE LAST ONE. ─────────────────────────────────────────────
             Straight from VDMSound (SBCompatCtl.cpp HandleTransfer):
                 // On SB16: if the last IRQ was not acknowledged, don't process any bytes
                 if ((getDSPVersion() >= 0x0400) && get8BitIRQ()) return S_OK;
             which is a mature implementation that plays Doom correctly under STOCK
             NTVDM -- i.e. on a worse clock than ours -- so this is copied, not invented.
           ► WHY IT IS THE RIGHT SHAPE HERE, measured. DMX refills exactly ONE block per
             mixer call and targets `(DMA position / blocksize) + 1` (DOOM.EXE 0x56884),
             so every block our read pointer crosses while its mixer is not looking is a
             block that is never filled -- it plays the previous ring lap instead. The
             arithmetic came out exactly: stale = 1 - (mixer 58/s / blocks 86/s) = 33%,
             against 30-33% measured, and the seam analysis showed that when DMX DOES
             refill both sides of a boundary the join is perfectly clean (9.23 against
             9.17 within-block). So there is nothing else to fix -- only the phase.
             This does NOT throttle the rate. DMX acknowledges 86 times a second, once
             per block, so the gate is almost never closed; what it stops is our pointer
             DRIFTING AHEAD between the guest's mixer calls.
           ⚠ SAFETY, AND VDMSound'S OWN TRAP. A guest that stops acknowledging must not
             be able to silence us forever, so the gate yields after a bounded wait. But
             their first attempt used a 64-tick safety, it fired CONSTANTLY, and they
             read 21 blocks ahead of an 8-block ring -- lapping it 2.6x into garbage,
             which is worse than the fault being fixed. So the safety here is two whole
             blocks, and every yield is COUNTED: if `GateForced` is not near zero the
             gate is not doing its job and this must not be believed.
           ⚠ SB16 ONLY, exactly as VDMSound scopes it -- an older DSP has no
             acknowledged-IRQ concept and games driving it do not expect the stall. */
        /* ⚠⚠ "STALL ONLY ON A BLOCK THAT IS STILL UNFILLED" WAS TRIED AND IS WRONG
             BY DESIGN -- DO NOT RE-DERIVE IT. It looks like the obvious refinement of
             mode 2 (which stalls on every block and so freezes the output ~3500
             samples/s), and it cannot work: DMX NEVER REFILLS THE BLOCK WE ARE IN. Its
             mixer targets `(DMA position / blocksize) + 1` (DOOM.EXE 0x56884), always
             the block AHEAD -- so waiting for the current block to change is waiting
             for something that will never happen, and every wait ends at the safety.
             Measured over three attempts: stalling on unchanged content gated almost
             continuously (3% playback) because the ring is mostly silence; exempting
             flat blocks gated 11 samples/s instead of the ~3400 due, because the offset
             it compared at was one sample stale; and with that fixed it ran at 18%
             playback with 748 forced yields. The design is the problem, not the
             implementation.
             Mode 2 is right precisely BECAUSE it waits for the mixer to RUN -- that is
             what fills the block ahead, which we are about to enter. */
        /* gate mode 2: hold until the guest's mixer POLLS the DMA position, which is
           the only signal that the refill has actually begun. Mode 1 (the ACK, as
           VDMSound uses) is kept for comparison but is measured not to help here --
           DMX acks before it refills, so it re-opens the gate too early. */
        if (state->GateMode == SB_GATE_POLL && state->Dma
            && state->GateMark && state->Dma->CountReads == state->GateMark
            && !state->Is16Bit) {
            if (state->GateWait < state->BlockLength * SB_GATE_SAFETY_BLOCKS) {
                state->GateWait++; state->GateStalled++;
                SB_PUT(frame, state->LastSample, state->LastLeft, state->LastRight);
                continue;
            }
            state->GateForced++;
        }
        if (state->GateMode == SB_GATE_ACK && state->IsIrqPending && !state->Is16Bit) {
            if (state->GateWait < state->BlockLength * SB_GATE_SAFETY_BLOCKS) {
                state->GateWait++;
                state->GateStalled++;
                SB_PUT(frame, state->LastSample, state->LastLeft, state->LastRight);   /* hold, do not inject a zero: a DC hold
                                                   is inaudible for a few samples where
                                                   a silence notch is a click */
                continue;
            }
            state->GateForced++;                /* guest stopped acking -- yield */
        }
        state->GateWait = 0;

        /* ── #176: NO DACK, NO SAMPLE -- AND NO END OF BLOCK. ────────────────────────
             The DSP asks for its next byte on DREQ and waits for the 8237's DACK.
             If the channel is masked or its controller is disabled (command bit 2)
             the DACK never comes: the DSP's block counter does not move, it does
             not interrupt, and it carries on the moment the guest re-enables the
             channel. Before #176 a refused fetch came back short and was read as
             "the block ended" -- an IRQ the card never raised, and the transfer
             dropped to IDLE so that re-enabling resumed nothing.
           ⚠ Silence while it waits, counted, and part of the same inserted-zero run
             the gap histogram measures: from the speaker's side it IS a gap. */
        if (state->Dma && !VddDmaGrants(state->Dma, state->Is16Bit ? state->Dma16 : state->Dma8)) {
            SB_PUT(frame, 0, 0, 0);
            state->OutputNoDack++;
            state->IdleRun++;
            continue;
        }

        if (state->IdleRun) {                   /* a gap just ended: bucket its length */
            UINT32 runLength = state->IdleRun, bucket = 0;
            while (runLength > 1 && bucket < SB_RUN_LAST_BUCKET) { runLength >>= 1; ++bucket; }
            state->IdleRuns[bucket]++;
            state->IdleRun = 0;
        }
        state->OutputActive++;

        state->LastSample = SbFetchSample(state, &isEnded);
        if (isEnded) state->LastLeft = state->LastRight = 0;          /* the fetch returned 0: no pair */
        SB_PUT(frame, state->LastSample, state->LastLeft, state->LastRight);

        if (state->BlockRemaining == 0 || isEnded) {
            /* Block complete: this is the interrupt the game is waiting for. In
               auto-init it reloads and keeps streaming (the ring buffer every DOS
               game uses); single-cycle stops until reprogrammed. */
            /* ► RECORD THE BOUNDARY BEFORE ANYTHING REACTS TO IT. The IRQ below can
                 reach the guest and the reload has already happened inside DmaStep,
                 so this is the only instant at which the three candidate culprits are
                 still distinguishable. Taken before VddRaiseIrq() deliberately. */
            if (state->Blocks < SB_BLOCK_LOG_MAX && state->Dma) {
                PSB_BLOCK_RECORD record = &state->BlockLog[state->Blocks];
                BYTE channel = state->Is16Bit ? state->Dma16 : state->Dma8;
                PCDMA_CHANNEL channelState = &state->Dma->Channels[channel & SB_DMA_CHANNEL_MASK];
                record->CaptureOffset    = state->CaptureLength;
                record->BlockLength  = state->BlockLength;
                record->Physical       = VddDmaCurrentPhysical(state->Dma, channel);
                record->CurrentCount  = channelState->CurrentCount;
                record->BaseAddress  = channelState->BaseAddress;
                record->BaseCount = channelState->BaseCount;
                record->Page       = channelState->Page;
                record->Mode       = channelState->Mode;
                record->Ended      = (BYTE)isEnded;
                /* CurrentAddress back at BaseAddress means the 8237 wrapped this fetch: the block
                   we just finished and the ring's end coincide. If they routinely do
                   NOT coincide, our block accounting and the guest's disagree, which
                   is exactly the two-frame skew being hunted. */
                record->Reloaded   = (BYTE)(channelState->CurrentAddress == channelState->BaseAddress);
                state->BlockLogCount  = state->Blocks + 1;      /* entries actually filled */
            }
            /* Score the block that just finished: >=90% identical to the same ring
               offsets one lap ago means DMX never rewrote it and we played the
               previous lap's audio again -- one echo, 186 ms after the original. */
            if (state->BlockBytes) {
                /* SB_FLAT_RANGE: 8-bit PCM silence is a run of 0x80, and DMX's own
                   fades settle to it. A span this small cannot be audible content, so
                   a "replay" verdict on such a block is uninformative either way --
                   see the note in vdd_sb.h. */
                INT isReplayed = (state->BlockSame * SB_REPLAY_DENOMINATOR >= state->BlockBytes * SB_REPLAY_NUMERATOR);
                INT isFlat     = (state->BlockMax - state->BlockMin) <= SB_FLAT_RANGE;
                ++state->BlocksChecked;
                if (isReplayed)          ++state->BlocksReplayed;
                if (isFlat)              ++state->BlocksFlat;
                if (isReplayed && !isFlat) ++state->BlocksReplayedLoud;
                /* ── A SILENT BLOCK IS NOT A DEFECT. AN ISOLATED SILENT BLOCK IS. ────
                     20% of blocks are flat, but the demo is genuinely quiet much of
                     the time, so the raw count cannot say how much of that the user
                     hears as a GAP. A long run of flat blocks is real silence; ONE
                     flat block between loud ones is a dropout, and that is exactly the
                     reported symptom -- |- - - - - -| instead of |------|.
                     Same run-length treatment the replay counter already gets, for the
                     same reason: a rate cannot show a shape. */
                if (isFlat) {
                    ++state->FlatRun;
                } else if (state->FlatRun) {
                    UINT32 runLength = state->FlatRun, bucket;
                    if      (runLength < 4)  bucket = runLength - 1;          /* 1, 2, 3 exactly */
                    else if (runLength < 8)  bucket = 3;
                    else if (runLength < 16) bucket = 4;
                    else if (runLength < 32) bucket = 5;
                    else if (runLength < 64) bucket = 6;
                    else             bucket = 7;
                    ++state->FlatRuns[bucket];
                    state->FlatRun = 0;
                }
                /* Close the run on the first block that is NOT an audible repeat --
                   flat blocks end it too, since a silent block carries no evidence
                   either way and bridging across one would invent a longer run. */
                if (isReplayed && !isFlat) {
                    ++state->ReplayRun;
                    if (state->ReplayRun > state->ReplayRunMax)
                        state->ReplayRunMax = state->ReplayRun;
                } else if (state->ReplayRun) {
                    UINT32 runLength = state->ReplayRun, bucket;
                    if      (runLength < 4)  bucket = runLength - 1;          /* 1, 2, 3 exactly */
                    else if (runLength < 8)  bucket = 3;
                    else if (runLength < 16) bucket = 4;
                    else if (runLength < 32) bucket = 5;
                    else if (runLength < 64) bucket = 6;
                    else             bucket = 7;
                    ++state->ReplayRuns[bucket];
                    state->ReplayRun = 0;
                }
            }
            state->BlockSame = state->BlockBytes = 0;
            state->BlockMin = SB_BLOCK_MIN_EMPTY; state->BlockMax = 0;
            state->IsIrqPending = 1;
            state->Blocks++;
            if (state->Dma) state->GateMark = state->Dma->CountReads;  /* gate mode 2 */
            VddRaiseIrq(state->Bus, state->Irq);
            if (state->TransferMode == SB_TRANSFER_AUTO && !isEnded) state->BlockRemaining = state->BlockLength;
            else                                        state->TransferMode  = SB_TRANSFER_IDLE;
        }
    }
    return frames;
}
#undef SB_PUT

UINT32 VddSbRender(PSB_STATE state, INT16 *output, UINT32 frames)
{ return SbRender(state, output, frames, 0); }
UINT32 VddSbFrameHz(PCSB_STATE state)
{ return (state->IsLegacyTransfer && state->IsStereo) ? state->RateHz / SB_STEREO_CHANNELS : state->RateHz; }
UINT32 VddSbRenderStereo(PSB_STATE state, INT16 *output, UINT32 frames)
{ return SbRender(state, output, frames, 1); }

/* --- lifecycle ------------------------------------------------------------ */
VOID VddSbReset(PVOID context)
{
    PSB_STATE state = (PSB_STATE)context;
    PVDD_BUS bus = state->Bus; PDMA_STATE dma = state->Dma; OPL_STATE *opl = state->Opl;
    WORD basePort = state->BasePort;
    BYTE irq = state->Irq, dma8 = state->Dma8, dma16 = state->Dma16;
    UINT32 dspWrites = state->DspWrites, blocks = state->Blocks;
    UINT byteIndex; BYTE *bytes = (BYTE *)state;
    for (byteIndex = 0; byteIndex < sizeof(*state); ++byteIndex) bytes[byteIndex] = 0;
    state->Bus = bus; state->Dma = dma; state->Opl = opl;
    state->BasePort = basePort; state->Irq = irq; state->Dma8 = dma8; state->Dma16 = dma16;
    state->DspWrites = dspWrites; state->Blocks = blocks;
    state->RateHz = SB_DEFAULT_RATE_HZ;
    state->BlockLength = 1;
    state->Mixer[SB_MIXER_MASTER_VOLUME] = SB_MIXER_VOLUME_POWER_UP;                  /* master volume, powered-up value */
    state->Mixer[SB_MIXER_VOICE_VOLUME] = SB_MIXER_VOLUME_POWER_UP;                  /* voice volume                     */
}

/* ── #176: THE DSP'S DREQ, AS THE 8237's STATUS REGISTER SEES IT. ──────────────────
     Asserted on the transfer's channel for as long as a transfer is armed and not
     paused -- whether or not the 8237 is answering, which is the point: a masked or
     disabled channel with an SB waiting on it is a PENDING request (status bit 4+n).
   ⚠ On the wire DREQ pulses once per byte (dropped by each DACK, raised again when
     the DSP's FIFO wants the next). We have no byte clock on the CPU's side of the
     card, so "a transfer is running" is the whole request; a guest that polls the
     bit sees it steady rather than flickering. */
static BYTE SbDreq(PCVOID context)
{
    PCSB_STATE state = (PCSB_STATE)context;
    BYTE channel;
    if (!VddSbIsActive(state)) return 0;
    channel = state->Is16Bit ? state->Dma16 : state->Dma8;
    return (BYTE)(1u << (channel & SB_DMA_CHANNEL_MASK));
}

INT VddSbInitialize(PVDD_BUS bus, PVOID context)
{
    PSB_STATE state = (PSB_STATE)context;
    state->Bus = bus;
    if (state->Dma) VddDmaAddDreq(state->Dma, SbDreq, state);
    if (!state->BasePort)  state->BasePort  = SB_DEFAULT_BASE;
    if (!state->Irq)   state->Irq   = SB_DEFAULT_IRQ;
    if (!state->Dma8)  state->Dma8  = SB_DEFAULT_DMA8;
    if (!state->Dma16) state->Dma16 = SB_DEFAULT_DMA16;
    if (!state->RateHz)   state->RateHz = SB_DEFAULT_RATE_HZ;
    if (!state->BlockLength) state->BlockLength = 1;
    if (VddClaimPorts(bus, state->BasePort, (WORD)(state->BasePort + SB_PORT_LAST), SbPortIn, SbPortOut, state))
        return SB_FAILED;
    return 0;
}
