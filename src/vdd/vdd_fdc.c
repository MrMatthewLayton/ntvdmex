/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * See vdd_fdc.h.  Intel 82077AA on the VDD bus.  Pure C.
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#include "vdd_fdc.h"

/* The 82077AA command set (opcode bits 4:0) and how many bytes each takes, opcode
 * included.
 */
#define FDC_OPCODE_MASK                 0x1F
#define FDC_CMD_READ_TRACK              0x02
#define FDC_CMD_SPECIFY                 0x03
#define FDC_CMD_SENSE_DRIVE_STATUS      0x04
#define FDC_CMD_WRITE_DATA              0x05
#define FDC_CMD_READ_DATA               0x06
#define FDC_CMD_RECALIBRATE             0x07
#define FDC_CMD_SENSE_INTERRUPT         0x08
#define FDC_CMD_WRITE_DELETED_DATA      0x09
#define FDC_CMD_READ_ID                 0x0A
#define FDC_CMD_READ_DELETED_DATA       0x0C
#define FDC_CMD_FORMAT_TRACK            0x0D
#define FDC_CMD_DUMPREG                 0x0E
#define FDC_CMD_SEEK                    0x0F
#define FDC_CMD_VERSION                 0x10
#define FDC_CMD_PERPENDICULAR           0x12
#define FDC_CMD_CONFIGURE               0x13
#define FDC_CMD_LOCK                    0x14
#define FDC_SPECIFY_BYTES               3
#define FDC_DRIVE_COMMAND_BYTES         2       /* opcode + the drive/head byte */
#define FDC_SEEK_BYTES                  3
#define FDC_CONFIGURE_BYTES             4
#define FDC_DATA_COMMAND_BYTES          9
#define FDC_FORMAT_BYTES                6

/* Parameter positions, and the drive/head byte (parameter 1). */
#define FDC_PARAMETER_2                 2
#define FDC_PARAMETER_3                 3
#define FDC_PARAMETER_4                 4
#define FDC_PARAMETER_5                 5
#define FDC_PARAMETER_6                 6
#define FDC_DRIVE_MASK                  3
#define FDC_HEAD_SHIFT                  2
#define FDC_DRIVE_0                     0
#define FDC_DRIVE_1                     1
#define FDC_DRIVE_2                     2
#define FDC_DRIVE_3                     3

/* Status bytes. */
#define FDC_MSR_IN_RESET                0x00
#define FDC_ST0_INVALID_COMMAND         0x80    /* Interrupt code 10: invalid command */
#define FDC_ST0_READY_CHANGED           0xC0    /* Interrupt code 11: polling */
#define FDC_ST0_ABNORMAL                0x40    /* Interrupt code 01: abnormal termination */
#define FDC_ST0_SEEK_END                0x20
#define FDC_ST1_NO_DATA                 0x04    /* sector not found */
#define FDC_ST2_CLEAR                   0x00
#define FDC_ST3_READY_TWO_SIDE          0x28    /* READY | TWO SIDE */
#define FDC_ST3_TRACK_0                 0x10
#define FDC_VERSION_82077AA             0x90
#define FDC_CONFIGURE_POLL_DISABLE      0x10    /* CONFIGURE byte 2 bit 4 */
#define FDC_LOCK_BIT                    0x80    /* LOCK: opcode bit 7 */
#define FDC_LOCK_RESULT_LOCKED          0x10
#define FDC_LOCK_RESULT_UNLOCKED        0x00
#define FDC_SIZE_CODE_512               2       /* N = 2: 512-byte sectors */

/* Result layouts. */
#define FDC_DATA_RESULT_BYTES           7
#define FDC_RESULT_ST0                  0
#define FDC_RESULT_ST1                  1
#define FDC_RESULT_ST2                  2
#define FDC_RESULT_CYLINDER             3
#define FDC_RESULT_HEAD                 4
#define FDC_RESULT_SECTOR               5
#define FDC_RESULT_SIZE_CODE            6
#define FDC_SENSE_INTERRUPT_BYTES       2
#define FDC_DUMPREG_BYTES               10
#define FDC_DUMPREG_PCN0                0
#define FDC_DUMPREG_PCN1                1
#define FDC_DUMPREG_PCN2                2
#define FDC_DUMPREG_PCN3                3
#define FDC_DUMPREG_SRT_HUT             4
#define FDC_DUMPREG_HLT_ND              5
#define FDC_DUMPREG_EOT                 6
#define FDC_DUMPREG_LOCK_PERPENDICULAR  7
#define FDC_DUMPREG_CONFIGURE           8
#define FDC_DUMPREG_PRECOMP_TRACK       9
#define FDC_DUMPREG_LOCK_BIT            0x80
#define FDC_DUMPREG_PERPENDICULAR_MASK  0x7F

/* The other registers. */
#define FDC_DSR_SOFTWARE_RESET          0x80
#define FDC_DSR_KEPT_BITS               0x7F
#define FDC_CCR_DATA_RATE               0x03
#define FDC_DIR_NO_CHANGE               0x00
#define FDC_UNDRIVEN_BUS                0xFF
#define FDC_IRQ                         6
#define FDC_OK                          0
#define FDC_FAILED                      (-1)

/* THE MAIN STATUS REGISTER IS DERIVED, NEVER STORED:
 * Every bit of it is a statement about state that lives somewhere else, so a
 * stored copy is a second opinion waiting to drift. This is the same lesson the
 * 8254's OUT pin taught -- "derive it, do not latch it" -- and it is a stronger
 * rule here, because MSR is the register the whole protocol turns on.
 *
 * [CAUTION]: RQM IS ALWAYS SET WHEN THE CHIP IS OUT OF RESET. A real part drops it for a
 * few microseconds between bytes while the FIFO moves; we have no latency to
 * model, so we are always ready. That is a difference a guest can only observe
 * by timing, and it is in the direction that cannot hang anyone.
 *
 * [CAUTION]: NON-DMA (bit 5) IS NEVER SET, because there is no execution phase yet to be
 * in. It becomes real with the data commands.
 */
BYTE VddFdcMainStatus(PCFDC_STATE state)
{
    BYTE status;
    /* Held in reset: the chip is not ready for anything, and says so. A guest
     * that writes 00h to DOR and then polls has hung a real machine too.
     */
    if (state->IsInReset) return FDC_MSR_IN_RESET;
    status = FDC_MSR_RQM;
    if (state->Phase == FDC_PHASE_RESULT) status |= FDC_MSR_DIO | FDC_MSR_CB;
    else if (state->CommandLength)           status |= FDC_MSR_CB;
    /* Bits 3:0 are "drive n is seeking". Our seeks complete inside the OUT that
     * starts them, so no drive is ever mid-seek when software can look.
     */
    return status;
}

static VOID FdcRaiseIrq(PFDC_STATE state)
{
    state->Irqs++;
    /* [WARNING]: DMAGATE IS NOT DECORATION. With DOR bit 3 clear the chip still works and
     * the interrupt simply never reaches the PIC -- every operation then times
     * out at the BIOS layer with nothing reporting a wrong value anywhere. A
     * model that ignores the bit serves a driver that set it and silently
     * disobeys one that cleared it.
     */
    if (!(state->Dor & FDC_DOR_DMA_GATE)) return;
    if (state->Bus) VddRaiseIrq(state->Bus, FDC_IRQ);
}

/* HOW MANY BYTES DOES THIS COMMAND TAKE? (docs/ref/fdc.md 5):
 * Counting the opcode itself. The low five bits are the opcode; bits 7:5 are
 * MT/MFM/SK modifiers and are not part of it. A command we do not know takes
 * exactly one byte and answers 80h -- which is the documented reply and is also
 * what keeps an unknown opcode from eating the next real command as a
 * parameter.
 */
static BYTE FdcCommandLength(BYTE opcode)
{
    switch (opcode & FDC_OPCODE_MASK)
    {
    case FDC_CMD_SPECIFY: return FDC_SPECIFY_BYTES;        /* SPECIFY */
    case FDC_CMD_SENSE_DRIVE_STATUS: return FDC_DRIVE_COMMAND_BYTES;        /* SENSE DRIVE STATUS */
    case FDC_CMD_RECALIBRATE: return FDC_DRIVE_COMMAND_BYTES;        /* RECALIBRATE */
    case FDC_CMD_SENSE_INTERRUPT: return 1;        /* SENSE INTERRUPT STATUS */
    case FDC_CMD_READ_ID: return FDC_DRIVE_COMMAND_BYTES;        /* READ ID */
    case FDC_CMD_DUMPREG: return 1;        /* DUMPREG */
    case FDC_CMD_SEEK: return FDC_SEEK_BYTES;        /* SEEK */
    case FDC_CMD_VERSION: return 1;        /* VERSION */
    case FDC_CMD_PERPENDICULAR: return FDC_DRIVE_COMMAND_BYTES;        /* PERPENDICULAR MODE */
    case FDC_CMD_CONFIGURE: return FDC_CONFIGURE_BYTES;        /* CONFIGURE */
    case FDC_CMD_LOCK: return 1;        /* LOCK */
    /* [CAUTION]: 09h IS WRITE DELETED DATA and it is easy to miss -- it sits in a gap
     * between 08h and 0Ah and no detection routine ever issues it. Omitting it
     * would have made it an "invalid command" that consumed ONE byte, and its
     * eight parameters would then have been read as eight more commands.
     *
     * [CAUTION]: 11h (SCAN EQUAL) is a uPD765 command that the 82077AA DOES NOT HAVE. We
     * identify as a 90h part, so it must fall through to invalid.
     */
    case FDC_CMD_READ_TRACK: case FDC_CMD_WRITE_DATA: case FDC_CMD_READ_DATA: case FDC_CMD_WRITE_DELETED_DATA: case FDC_CMD_READ_DELETED_DATA:
        return FDC_DATA_COMMAND_BYTES;               /* READ TRACK / WRITE / READ / WR+RD DELETED */
    case FDC_CMD_FORMAT_TRACK: return FDC_FORMAT_BYTES;        /* FORMAT TRACK */
    default:   return 1;        /* invalid */
    }
}

static VOID FdcResult(PFDC_STATE state, const BYTE *bytes, BYTE count)
{
    BYTE byteIndex;
    for (byteIndex = 0; byteIndex < count && byteIndex < sizeof(state->Result); ++byteIndex) state->Result[byteIndex] = bytes[byteIndex];
    state->ResultLength = byteIndex; state->ResultPosition = 0;
    /* A command with no result bytes is over the moment its last parameter
     * lands: the host sees CMD BSY clear and DIO stay low. One with results is
     * not over until they are read, which is what lets a driver drain a result
     * of unknown length by watching CMD BSY.
     */
    state->Phase   = byteIndex ? FDC_PHASE_RESULT : FDC_PHASE_COMMAND;
    state->CommandLength = 0;
}

static VOID FdcInvalid(PFDC_STATE state)
{
    BYTE resultByte = FDC_ST0_INVALID_COMMAND;           /* ST0 with the invalid-command interrupt code */
    state->InvalidCommands++;
    FdcResult(state, &resultByte, 1);
}

/* The seven result bytes every data command ends with: ST0 ST1 ST2 C H R N. */
static VOID FdcDataResult(PFDC_STATE state, BYTE status0, BYTE status1, BYTE status2,
                            BYTE cylinder, BYTE headAddress, BYTE sector, BYTE sizeCode)
{
    BYTE bytes[FDC_DATA_RESULT_BYTES];
    bytes[FDC_RESULT_ST0] = status0; bytes[FDC_RESULT_ST1] = status1; bytes[FDC_RESULT_ST2] = status2; bytes[FDC_RESULT_CYLINDER] = cylinder; bytes[FDC_RESULT_HEAD] = headAddress; bytes[FDC_RESULT_SECTOR] = sector; bytes[FDC_RESULT_SIZE_CODE] = sizeCode;
    FdcResult(state, bytes, FDC_DATA_RESULT_BYTES);
}

/* THE RESET SEQUENCE. (docs/ref/fdc.md 8):
 * Reached from DOR bit 2 going low-then-high, from DSR bit 7, and from power
 * on. The data rate SURVIVES a software reset -- a driver is entitled not to
 * re-select it -- and so does the CONFIGURE state if LOCK was set, which is the
 * entire point of the LOCK command.
 */
static VOID FdcSoftReset(PFDC_STATE state)
{
    state->Resets++;
    state->Phase = FDC_PHASE_COMMAND; state->CommandLength = 0; state->CommandWanted = 0;
    state->ResultLength = 0; state->ResultPosition = 0;
    if (!state->IsLocked)
    {
        state->ConfigureByte2 = 0;
        state->ConfigurePrecompTrack = 0;
        state->Perpendicular = 0;
    }
    /* Drive polling: four sense-interrupts are now owed, one per drive. */
    state->PollDrive = state->IsPollDisabled ? FDC_DRIVES : 0;
    state->IsIrqPending = 0; state->PendingSt0 = 0;
    FdcRaiseIrq(state);
}

/* SENSE INTERRUPT STATUS -- the other half of every interrupt:
 * SEEK and RECALIBRATE have no result phase at all, so this is the only way to
 * learn that they finished, and the chip keeps the reason until it is asked.
 * Three cases, in priority order, and the third is as documented as the others:
 * asked when nothing is pending, the answer is 80h, and that is how a driver
 * discovers there is nothing pending.
 */
static VOID FdcSenseInterrupt(PFDC_STATE state)
{
    BYTE bytes[FDC_SENSE_INTERRUPT_BYTES];
    if (state->IsIrqPending)
    {
        bytes[0] = state->PendingSt0;
        bytes[1] = state->PresentCylinder[state->PendingSt0 & FDC_DRIVE_MASK];
        state->IsIrqPending = 0;
        FdcResult(state, bytes, FDC_SENSE_INTERRUPT_BYTES);
    }
    else if (state->PollDrive < FDC_DRIVES)
    {
        bytes[0] = (BYTE)(FDC_ST0_READY_CHANGED | state->PollDrive);    /* ready changed, drive n */
        bytes[1] = state->PresentCylinder[state->PollDrive];
        state->PollDrive++;
        FdcResult(state, bytes, FDC_SENSE_INTERRUPT_BYTES);
    }
    else
    {
        FdcInvalid(state);        /* nothing pending: 80h, by the datasheet */
    }
}

static VOID FdcSeekDone(PFDC_STATE state, BYTE drive, BYTE head)
{
    state->PendingSt0 = (BYTE)(FDC_ST0_SEEK_END | (head << FDC_HEAD_SHIFT) | (drive & FDC_DRIVE_MASK)); /* SEEK END */
    state->IsIrqPending = 1;
    FdcRaiseIrq(state);
}

/* Every parameter byte has arrived: carry the command out. */
static VOID FdcExecute(PFDC_STATE state)
{
    BYTE opcode = (BYTE)(state->Command[0] & FDC_OPCODE_MASK);
    BYTE drive, head;

    state->Commands++;
    switch (opcode)
    {

    case FDC_CMD_VERSION:                  /* VERSION -- the detection command */
    {
        /* 90h says "enhanced 82077AA": CONFIGURE, LOCK and PERPENDICULAR MODE
         * exist. MEASURED at 90h on both oracles, one of them a real AMI BIOS.
         */
        BYTE resultByte = FDC_VERSION_82077AA;
        FdcResult(state, &resultByte, 1);
        break;
    }

    case FDC_CMD_SENSE_INTERRUPT:                  /* SENSE INTERRUPT STATUS */
        FdcSenseInterrupt(state);
        break;

    case FDC_CMD_SPECIFY:                  /* SPECIFY -- step rate, head load, non-DMA */
        state->StepRateHeadUnload = state->Command[1];
        state->HeadLoadNonDma  = state->Command[FDC_PARAMETER_2];
        FdcResult(state, 0, 0);   /* no result phase, and no interrupt */
        break;

    case FDC_CMD_CONFIGURE:                  /* CONFIGURE */
        state->ConfigureByte2  = state->Command[FDC_PARAMETER_2];
        state->ConfigurePrecompTrack = state->Command[FDC_PARAMETER_3];
        /* Bit 4 of byte 2 is POLL: SET means drive polling is DISABLED. */
        state->IsPollDisabled   = (BYTE)((state->Command[FDC_PARAMETER_2] & FDC_CONFIGURE_POLL_DISABLE) ? TRUE : FALSE);
        FdcResult(state, 0, 0);
        break;

    case FDC_CMD_PERPENDICULAR:                  /* PERPENDICULAR MODE */
        state->Perpendicular = state->Command[1];
        FdcResult(state, 0, 0);
        break;

    case FDC_CMD_LOCK:                  /* LOCK -- bit 7 of the opcode is the new value */
    {
        state->IsLocked = (BYTE)((state->Command[0] & FDC_LOCK_BIT) ? TRUE : FALSE);
        { BYTE resultByte = (BYTE)(state->IsLocked ? FDC_LOCK_RESULT_LOCKED : FDC_LOCK_RESULT_UNLOCKED);
          FdcResult(state, &resultByte, 1); }
        break;
    }

    case FDC_CMD_SENSE_DRIVE_STATUS:                  /* SENSE DRIVE STATUS -> ST3 */
    {
        BYTE resultByte;
        drive = (BYTE)(state->Command[1] & FDC_DRIVE_MASK);
        head  = (BYTE)((state->Command[1] >> FDC_HEAD_SHIFT) & 1);
        /* Bit 5 READY is always 1 on the 82077AA; bit 3 TWO SIDE is 1 for the
         * only drive we advertise; bit 4 TRACK0 is a fact about where the head
         * is; bit 6 WRITE PROTECTED is 0 because the image handle is opened for
         * writing (INT 13h writes through it).
         */
        resultByte = (BYTE)(FDC_ST3_READY_TWO_SIDE | (head << FDC_HEAD_SHIFT) | drive);
        if (!state->PresentCylinder[drive]) resultByte |= FDC_ST3_TRACK_0;
        FdcResult(state, &resultByte, 1);
        break;
    }

    case FDC_CMD_DUMPREG:                  /* DUMPREG -- ten bytes, no side effects */
    {
        BYTE bytes[FDC_DUMPREG_BYTES];
        bytes[FDC_DUMPREG_PCN0] = state->PresentCylinder[FDC_DRIVE_0]; bytes[FDC_DUMPREG_PCN1] = state->PresentCylinder[FDC_DRIVE_1];
        bytes[FDC_DUMPREG_PCN2] = state->PresentCylinder[FDC_DRIVE_2]; bytes[FDC_DUMPREG_PCN3] = state->PresentCylinder[FDC_DRIVE_3];
        bytes[FDC_DUMPREG_SRT_HUT] = state->StepRateHeadUnload; bytes[FDC_DUMPREG_HLT_ND] = state->HeadLoadNonDma;
        bytes[FDC_DUMPREG_EOT] = state->LastEot;
        bytes[FDC_DUMPREG_LOCK_PERPENDICULAR] = (BYTE)((state->IsLocked ? FDC_DUMPREG_LOCK_BIT : 0) | (state->Perpendicular & FDC_DUMPREG_PERPENDICULAR_MASK));
        bytes[FDC_DUMPREG_CONFIGURE] = state->ConfigureByte2;
        bytes[FDC_DUMPREG_PRECOMP_TRACK] = state->ConfigurePrecompTrack;
        FdcResult(state, bytes, FDC_DUMPREG_BYTES);
        break;
    }

    case FDC_CMD_RECALIBRATE:                  /* RECALIBRATE -- seek to cylinder 0 */
        drive = (BYTE)(state->Command[1] & FDC_DRIVE_MASK);
        state->PresentCylinder[drive] = 0;
        FdcResult(state, 0, 0);   /* no result phase: the interrupt is the report */
        FdcSeekDone(state, drive, 0);
        break;

    case FDC_CMD_SEEK:                  /* SEEK */
        drive = (BYTE)(state->Command[1] & FDC_DRIVE_MASK);
        head  = (BYTE)((state->Command[1] >> FDC_HEAD_SHIFT) & 1);
        state->PresentCylinder[drive] = state->Command[FDC_PARAMETER_2];
        FdcResult(state, 0, 0);
        FdcSeekDone(state, drive, head);
        break;

    case FDC_CMD_READ_ID:                  /* READ ID -- what is under the head right now */
        drive = (BYTE)(state->Command[1] & FDC_DRIVE_MASK);
        head  = (BYTE)((state->Command[1] >> FDC_HEAD_SHIFT) & 1);
        FdcDataResult(state, (BYTE)((head << FDC_HEAD_SHIFT) | drive), 0, 0,
                        state->PresentCylinder[drive], head, 1, FDC_SIZE_CODE_512);
        state->PendingSt0 = (BYTE)((head << FDC_HEAD_SHIFT) | drive);
        state->IsIrqPending = 1;
        FdcRaiseIrq(state);
        break;

    /* THE DATA COMMANDS. RECOGNISED, AND HONESTLY FAILED:
     *
     * [WARNING]: THIS IS **PART** AND IT IS MARKED PART. They need the 8237A pointed at
     * the image handle INT 13h already holds, which is the next step. Until
     * then they terminate the way a real controller terminates over an
     * unformatted track: ST0 interrupt code 01 (abnormal termination) with
     * ST1 bit 2 (no data -- sector not found), the full seven result bytes,
     * and the interrupt a real one would raise.
     *
     * [CAUTION]: The alternative -- answering 80h, "invalid command" -- would be a
     * different and worse lie: it says the CONTROLLER does not know the
     * command, on a chip that has just identified itself as a 90h part that
     * by definition does. A driver can act on a media error. It cannot act on
     * a controller that contradicts itself.
     */
    case FDC_CMD_READ_TRACK: case FDC_CMD_WRITE_DATA: case FDC_CMD_READ_DATA: case FDC_CMD_WRITE_DELETED_DATA: case FDC_CMD_READ_DELETED_DATA:
    {
        BYTE cylinder, headAddress, sector, sizeCode;
        drive = (BYTE)(state->Command[1] & FDC_DRIVE_MASK);
        head  = (BYTE)((state->Command[1] >> FDC_HEAD_SHIFT) & 1);
        cylinder = state->Command[FDC_PARAMETER_2]; headAddress = state->Command[FDC_PARAMETER_3]; sector = state->Command[FDC_PARAMETER_4]; sizeCode = state->Command[FDC_PARAMETER_5];
        state->LastEot = state->Command[FDC_PARAMETER_6];
        FdcDataResult(state, (BYTE)(FDC_ST0_ABNORMAL | (head << FDC_HEAD_SHIFT) | drive), FDC_ST1_NO_DATA, FDC_ST2_CLEAR,
                        cylinder, headAddress, sector, sizeCode);
        state->PendingSt0 = (BYTE)(FDC_ST0_ABNORMAL | (head << FDC_HEAD_SHIFT) | drive);
        state->IsIrqPending = 1;
        FdcRaiseIrq(state);
        break;
    }

    case FDC_CMD_FORMAT_TRACK:                  /* FORMAT TRACK -- same, and never destructive */
        drive = (BYTE)(state->Command[1] & FDC_DRIVE_MASK);
        head  = (BYTE)((state->Command[1] >> FDC_HEAD_SHIFT) & 1);
        state->LastEot = state->Command[FDC_PARAMETER_3];
        FdcDataResult(state, (BYTE)(FDC_ST0_ABNORMAL | (head << FDC_HEAD_SHIFT) | drive), FDC_ST1_NO_DATA, FDC_ST2_CLEAR,
                        state->PresentCylinder[drive], head, 1, state->Command[FDC_PARAMETER_2]);
        state->PendingSt0 = (BYTE)(FDC_ST0_ABNORMAL | (head << FDC_HEAD_SHIFT) | drive);
        state->IsIrqPending = 1;
        FdcRaiseIrq(state);
        break;

    default:
        FdcInvalid(state);
        break;
    }
}

/* THE FIFO, WHICH IS THE WHOLE PROTOCOL: */
static VOID FdcFifoWrite(PFDC_STATE state, BYTE value)
{
    /* Writing while a result is waiting is a protocol error by the host. A real
     * part ignores it; so do we, rather than letting it become the first byte of
     * a command the driver never issued.
     */
    if (state->Phase == FDC_PHASE_RESULT) return;
    if (!state->CommandLength) state->CommandWanted = FdcCommandLength(value);
    if (state->CommandLength < sizeof(state->Command)) state->Command[state->CommandLength] = value;
    state->CommandLength++;
    if (state->CommandLength >= state->CommandWanted) FdcExecute(state);
}

static BYTE FdcFifoRead(PFDC_STATE state)
{
    BYTE value;
    /* Reading when there is nothing to read hands back the last byte on a real
     * part. Returning 0xFF here would re-create the very ambiguity this device
     * exists to remove.
     */
    if (state->Phase != FDC_PHASE_RESULT) return state->ResultLength ? state->Result[state->ResultLength - 1] : FDC_ST0_INVALID_COMMAND;
    value = state->Result[state->ResultPosition++];
    if (state->ResultPosition >= state->ResultLength)
    {
        /* The last result byte has been read: CMD BSY clears and the chip is
         * idle again. This edge is what a length-free drain watches for.
         *
         * [CAUTION]: res_len is NOT cleared, so a stray read past the end hands back the
         * last byte the way a real part does, rather than 80h -- which would
         * look like an invalid-command reply to something that had simply read
         * one byte too many.
         */
        state->Phase = FDC_PHASE_COMMAND; state->ResultPosition = 0;
    }
    return value;
}

VOID VddFdcPortOut(PVOID context, WORD port, BYTE width, UINT32 value)
{
    PFDC_STATE state = (PFDC_STATE)context;
    BYTE byteValue = (BYTE)value;
    (VOID)width;
    switch (port)
    {
    case FDC_DOR:
    {
        BYTE previousDor = state->Dor;
        state->Dor = byteValue;
        /* [WARNING]: BIT 2 IS ACTIVE LOW AND IT IS THE SOFTWARE RESET EVERY BIOS USES.
         * A plausible-looking "turn everything off" of 00h holds the chip in
         * reset, ungates the interrupt AND stops the motors, all at once.
         */
        if (!(byteValue & FDC_DOR_NRESET))
        {
            state->IsInReset = 1;
        }
        else if (state->IsInReset || !(previousDor & FDC_DOR_NRESET))
        {
            state->IsInReset = 0;
            FdcSoftReset(state);
        }
        break;
    }
    case FDC_TDR:  state->Tdr = byteValue; break;
    case FDC_MSR:                               /* the WRITE side is DSR */
        state->Dsr = byteValue;
        if (byteValue & FDC_DSR_SOFTWARE_RESET)                           /* bit 7: software reset */
        {
            /* Self-clearing: the datasheet's reset bit resets and releases. The
             * data rate in bits 1:0 SURVIVES, which is why a driver may reset
             * through this door and not re-select it.
             */
            state->Dsr = (BYTE)(byteValue & FDC_DSR_KEPT_BITS);
            state->IsInReset = 0;
            FdcSoftReset(state);
        }
        break;
    case FDC_FIFO: FdcFifoWrite(state, byteValue); break;
    case FDC_DIR:  state->Ccr = (BYTE)(byteValue & FDC_CCR_DATA_RATE); break;  /* the write side is CCR */
    default: break;
    }
}

VOID VddFdcPortIn(PVOID context, WORD port, BYTE width, UINT32 *value)
{
    PFDC_STATE state = (PFDC_STATE)context;
    (VOID)width;
    switch (port)
    {
    case FDC_DOR:  *value = state->Dor; break;       /* readable on an 82077AA */
    case FDC_TDR:  *value = state->Tdr; break;
    case FDC_MSR:  *value = VddFdcMainStatus(state); break;
    case FDC_FIFO: *value = FdcFifoRead(state); break;
    case FDC_DIR:
        /* DIR BIT 7 IS DSKCHG, AND WE ANSWER 0:
         * The medium behind us is a file that cannot be swapped while the VDM
         * runs, so "the door has been opened" is false -- and it is the same
         * truthful claim INT 13h AH=15h already makes through the other door
         * ("floppy without change-line support"). One medium, two doors, one
         * answer, which is the rule the A20 gate and the RTC both arrived at.
         *
         * [CAUTION]: BITS 6:0 ARE NOT ADJUDICABLE. A PC/AT-mode part does not drive them;
         * our two oracles both DO drive them and disagree (6.22/QEMU 00h, PCem
         * 01h), so there is no answer to copy. Zero is recorded as a choice,
         * not measured.
         */
        *value = FDC_DIR_NO_CHANGE;
        break;
    default: *value = FDC_UNDRIVEN_BUS; break;
    }
}

VOID VddFdcReset(PVOID context)
{
    PFDC_STATE state = (PFDC_STATE)context;
    PVDD_BUS bus = state->Bus;
    UINT byteIndex;
    for (byteIndex = 0; byteIndex < sizeof(*state); ++byteIndex) ((BYTE *)state)[byteIndex] = 0;
    state->Bus = bus;
    /* WHAT POST LEAVES:
     * Out of reset, DMA and interrupts gated through, drive 0 selected, motors
     * off: DOR = 0Ch. MEASURED on 6.22/QEMU, which reads exactly 0Ch. PCem's
     * AMI BIOS leaves 0Fh -- the same two chip bits with drive 3 selected --
     * so bits 3:2 are the adjudicable part and both oracles agree on them.
     *
     * [CAUTION]: The BIOS's own SPECIFY values are not guessed here. They arrive through
     * the port when a driver programs them, and DUMPREG hands back what
     * arrived rather than an invented default.
     */
    state->Dor      = FDC_DOR_NRESET | FDC_DOR_DMA_GATE;    /* 0x0C */
    state->IsInReset = 0;
    state->Phase    = FDC_PHASE_COMMAND;
    /* A machine that has been through POST has already had its four
     * sense-interrupts collected by the BIOS. Starting at 4 means the first
     * thing a guest asks is answered "nothing pending" rather than with three
     * phantom drives that changed while it was not looking.
     */
    state->PollDrive = FDC_DRIVES;
}

INT VddFdcInitialize(PVDD_BUS bus, PVOID context)
{
    PFDC_STATE state = (PFDC_STATE)context;
    state->Bus = bus;
    if (!state->Dor) VddFdcReset(state);            /* the host builds us zeroed */
    state->Bus = bus;
    /* TWO CLAIMS, AND THE GAPS ARE DELIBERATE:
     * 3F0h/3F1h (SRA/SRB) are driven ONLY by a part strapped for PS/2 mode. We
     * present a PC/AT machine, so they are left unclaimed and float to FFh --
     * which is what 6.22/QEMU AND PCem both report for 3F0h. (They both drive
     * 3F1h and disagree about its value, C1h against 51h, so there is nothing
     * there to copy either.)
     *
     * [WARNING]: 3F6h IS NOT OURS AT ALL. The FDC does not decode offset 6; on a PC/AT it
     * is the hard-disk controller's alternate status register, and both oracles
     * answer 50h there (DRDY|DSC) from their IDE side while we answer FFh.
     * THAT IS A REAL GAP AND IT BELONGS TO THE ATA SURFACE, NOT THIS ONE.
     * Claiming the whole eight-port block would have "fixed" the row by taking
     * a register that is somebody else's.
     */
    if (VddClaimPorts(bus, FDC_DOR, FDC_FIFO, VddFdcPortIn, VddFdcPortOut, state)) return FDC_FAILED;
    if (VddClaimPorts(bus, FDC_DIR, FDC_DIR,  VddFdcPortIn, VddFdcPortOut, state)) return FDC_FAILED;
    return FDC_OK;
}
