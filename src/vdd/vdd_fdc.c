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
#include "../dos/bios_bda_fields.h"     /* #178: the BIOS's diskette fields */

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
#define FDC_DRIVE_0                     0
#define FDC_DRIVE_1                     1
#define FDC_DRIVE_2                     2
#define FDC_DRIVE_3                     3

/* Status bytes. */
#define FDC_MSR_IN_RESET                0x00
#define FDC_ST0_INVALID_COMMAND         0x80    /* Interrupt code 10: invalid command */
#define FDC_ST0_READY_CHANGED           0xC0    /* Interrupt code 11: polling */
#define FDC_ST0_SEEK_END                0x20
#define FDC_ST1_END_OF_CYLINDER         0x80    /* #178: ran past EOT without a TC */
#define FDC_ST1_OVERRUN                 0x10    /* #178: the DMA did not keep up (or never answered) */
#define FDC_ST1_NOT_WRITABLE            0x02
#define FDC_ST1_MISSING_ADDRESS_MARK    0x01    /* #178: no medium -- nothing on the track at all */
#define FDC_ST2_CONTROL_MARK            0x40    /* #178: READ DELETED found a normal data mark */
#define FDC_ST2_WRONG_CYLINDER          0x10    /* #178: the ID's C is not the track under the head */
#define FDC_MULTI_TRACK                 0x80    /* Opcode bit 7: MT */
#define FDC_SPECIFY_NON_DMA             0x01    /* SPECIFY byte 2 bit 0: ND */
#define FDC_PARAMETER_1                 1
#define FDC_ID_CYLINDER                 0       /* FORMAT's four ID bytes */
#define FDC_ID_HEAD                     1
#define FDC_ID_SECTOR                   2
#define FDC_ID_SIZE_CODE                3
#define FDC_FIRST_SECTOR                1
#define FDC_DOR_AFTER_MOTOR_OFF         0x0C    /* What the BIOS writes when the countdown ends */
#define FDC_ST3_READY_TWO_SIDE          0x28    /* READY | TWO SIDE */
#define FDC_ST3_TRACK_0                 0x10
#define FDC_VERSION_82077AA             0x90
#define FDC_CONFIGURE_POLL_DISABLE      0x10    /* CONFIGURE byte 2 bit 4 */
#define FDC_LOCK_BIT                    0x80    /* LOCK: opcode bit 7 */
#define FDC_LOCK_RESULT_LOCKED          0x10
#define FDC_LOCK_RESULT_UNLOCKED        0x00

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
 * #178: NON-DMA (bit 5) is set through a non-DMA execution phase, and only then.
 */
BYTE VddFdcMainStatus(PCFDC_STATE state)
{
    BYTE status;

    /* Held in reset: the chip is not ready for anything, and says so. A guest
     * that writes 00h to DOR and then polls has hung a real machine too.
     */
    if (state->IsInReset)
        return FDC_MSR_IN_RESET;

    status = FDC_MSR_RQM;

    /* #178: the non-DMA execution phase -- every data byte through 3F5h, and DIO says
     * which way: set while the chip is handing a READ's data out.
     */
    if (state->Phase == FDC_PHASE_EXECUTION)
        return (BYTE)(status | FDC_MSR_NDMA | FDC_MSR_CB
                      | ((state->ExecOpcode == FDC_CMD_READ_DATA || state->ExecOpcode == FDC_CMD_READ_TRACK) ? FDC_MSR_DIO : 0));

    if (state->Phase == FDC_PHASE_RESULT)
        status |= FDC_MSR_DIO | FDC_MSR_CB;
    else if (state->CommandLength)
        status |= FDC_MSR_CB;

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
    if (!(state->Dor & FDC_DOR_DMA_GATE))
        return;

    if (state->Bus)
        VddRaiseIrq(state->Bus, FDC_IRQ);
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
    case FDC_CMD_SPECIFY:
        return FDC_SPECIFY_BYTES;        /* SPECIFY */

    case FDC_CMD_SENSE_DRIVE_STATUS:
        return FDC_DRIVE_COMMAND_BYTES;        /* SENSE DRIVE STATUS */

    case FDC_CMD_RECALIBRATE:
        return FDC_DRIVE_COMMAND_BYTES;        /* RECALIBRATE */

    case FDC_CMD_SENSE_INTERRUPT:
        return 1;        /* SENSE INTERRUPT STATUS */

    case FDC_CMD_READ_ID:
        return FDC_DRIVE_COMMAND_BYTES;        /* READ ID */

    case FDC_CMD_DUMPREG:
        return 1;        /* DUMPREG */

    case FDC_CMD_SEEK:
        return FDC_SEEK_BYTES;        /* SEEK */

    case FDC_CMD_VERSION:
        return 1;        /* VERSION */

    case FDC_CMD_PERPENDICULAR:
        return FDC_DRIVE_COMMAND_BYTES;        /* PERPENDICULAR MODE */

    case FDC_CMD_CONFIGURE:
        return FDC_CONFIGURE_BYTES;        /* CONFIGURE */

    case FDC_CMD_LOCK:
        return 1;        /* LOCK */

    /* [CAUTION]: 09h IS WRITE DELETED DATA and it is easy to miss -- it sits in a gap
     * between 08h and 0Ah and no detection routine ever issues it. Omitting it
     * would have made it an "invalid command" that consumed ONE byte, and its
     * eight parameters would then have been read as eight more commands.
     *
     * [CAUTION]: 11h (SCAN EQUAL) is a uPD765 command that the 82077AA DOES NOT HAVE. We
     * identify as a 90h part, so it must fall through to invalid.
     */
    case FDC_CMD_READ_TRACK:
    case FDC_CMD_WRITE_DATA:
    case FDC_CMD_READ_DATA:
    case FDC_CMD_WRITE_DELETED_DATA:
    case FDC_CMD_READ_DELETED_DATA:
        return FDC_DATA_COMMAND_BYTES;               /* READ TRACK / WRITE / READ / WR+RD DELETED */

    case FDC_CMD_FORMAT_TRACK:
        return FDC_FORMAT_BYTES;        /* FORMAT TRACK */

    default:
        return 1;        /* invalid */
    }
}

static VOID FdcResult(PFDC_STATE state, const BYTE *bytes, BYTE count)
{
    BYTE byteIndex;

    for (byteIndex = 0; byteIndex < count && byteIndex < sizeof(state->Result); ++byteIndex)
        state->Result[byteIndex] = bytes[byteIndex];

    state->ResultLength = byteIndex;
    state->ResultPosition = 0;
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
static VOID FdcDataResult(
    PFDC_STATE state,
    BYTE status0,
    BYTE status1,
    BYTE status2,
    BYTE cylinder,
    BYTE headAddress,
    BYTE sector,
    BYTE sizeCode)
{
    BYTE bytes[FDC_DATA_RESULT_BYTES];

    bytes[FDC_RESULT_ST0] = status0;
    bytes[FDC_RESULT_ST1] = status1;
    bytes[FDC_RESULT_ST2] = status2;
    bytes[FDC_RESULT_CYLINDER] = cylinder;
    bytes[FDC_RESULT_HEAD] = headAddress;
    bytes[FDC_RESULT_SECTOR] = sector;
    bytes[FDC_RESULT_SIZE_CODE] = sizeCode;
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
    state->Phase = FDC_PHASE_COMMAND;
    state->CommandLength = 0;
    state->CommandWanted = 0;
    state->ResultLength = 0;
    state->ResultPosition = 0;

    if (!state->IsLocked)
    {
        state->ConfigureByte2 = 0;
        state->ConfigurePrecompTrack = 0;
        state->Perpendicular = 0;
    }

    /* Drive polling: four sense-interrupts are now owed, one per drive. */
    state->PollDrive = state->IsPollDisabled ? FDC_DRIVES : 0;
    state->IsIrqPending = 0;
    state->PendingSt0 = 0;
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

/* #178: THE DATA COMMANDS:
 * The medium is the host's image, lent through MediaGeometry/MediaTransfer; the bytes
 * cross on the 8237's channel 2 (docs/ref/fdc.md 1), or -- with SPECIFY's ND bit set --
 * through 3F5h one at a time with MSR bit 5 up (fdc.md 4). A command runs from its R
 * to EOT (and, with MT, on to side 1), and stops at the 8237's TERMINAL COUNT: that is
 * how a PC says "this many sectors" -- the BIOS programs the DMA count, not the chip.
 *
 * [CAUTION]: THE DMA TRANSFER IS SYNCHRONOUS. A real part moves a byte per DREQ over
 * milliseconds; ours finishes inside the OUT that completes the command, as our seeks
 * do. A guest can tell only by timing, and only in the direction that cannot hang it.
 */
static INT FdcIsNonDma(PCFDC_STATE state)
{
    return (state->HeadLoadNonDma & FDC_SPECIFY_NON_DMA) != 0;
}

static INT FdcIsReadCommand(BYTE opcode)
{
    return opcode == FDC_CMD_READ_DATA || opcode == FDC_CMD_READ_TRACK;
}

/* The end of a data command: the seven result bytes and IRQ6. C H R N are where the
 * command ENDED, not where it was asked to start (fdc.md 6).
 */
static VOID FdcDataDone(PFDC_STATE state, BYTE interruptCode, BYTE status1, BYTE status2)
{
    BYTE status0 = (BYTE)(interruptCode | (state->ExecHead << FDC_HEAD_SHIFT) | state->ExecDrive);

    FdcDataResult(state, status0, status1, status2,
                  state->ExecCylinder, state->ExecHeadAddress, state->ExecSector, state->ExecSizeCode);
    state->PendingSt0 = status0;
    state->IsIrqPending = 1;
    FdcRaiseIrq(state);
}

/* THE C/H/R THE CHIP MOVES ON TO (82077AA datasheet, the result-phase C/H/R/N table):
 * R+1 within the track; at EOT with MT on side 0, on to side 1 sector 1 (same C); at
 * EOT otherwise, C+1 and R = 1 -- H complemented if MT was set. Returns 1 in that last
 * case: the command has run off its cylinder.
 */
static INT FdcAdvance(PFDC_STATE state)
{
    if (state->ExecSector < state->ExecEot)
    {
        state->ExecSector++;
        return 0;
    }

    state->ExecSector = FDC_FIRST_SECTOR;

    if (state->ExecIsMultiTrack && !(state->ExecHeadAddress & 1))
    {
        state->ExecHeadAddress ^= 1;
        state->ExecHead ^= 1;
        return 0;
    }

    if (state->ExecIsMultiTrack)
        state->ExecHeadAddress ^= 1;

    state->ExecCylinder++;
    return 1;
}

/* Is the sector the command names actually under the head? 1 = yes; otherwise the
 * ST1/ST2 bits that say why, as the chip reports a failed ID search.
 */
static INT FdcFindSector(PFDC_STATE state, PBYTE status1, PBYTE status2)
{
    FDC_GEOMETRY geometry;
    BYTE track = state->PresentCylinder[state->ExecDrive];

    *status1 = 0;
    *status2 = FDC_ST2_CLEAR;

    if (!state->MediaGeometry || !state->MediaTransfer
        || !state->MediaGeometry(state->MediaContext, state->ExecDrive, &geometry)
        || track >= geometry.Cylinders || state->ExecHead >= geometry.Heads)
    {
        *status1 = FDC_ST1_MISSING_ADDRESS_MARK;         /* no medium, or no track there */
        return 0;
    }

    if (state->ExecCylinder != track)
    {
        *status1 = FDC_ST1_NO_DATA;                      /* the IDs on this track say another C */
        *status2 = FDC_ST2_WRONG_CYLINDER;
        return 0;
    }

    if (state->ExecHeadAddress != state->ExecHead || state->ExecSizeCode != FDC_SIZE_CODE_512
        || state->ExecSector < FDC_FIRST_SECTOR || state->ExecSector > geometry.SectorsPerTrack)
    {
        *status1 = FDC_ST1_NO_DATA;
        return 0;
    }

    return 1;
}

static INT FdcSectorIo(PFDC_STATE state, PBYTE buffer, INT isWrite)
{
    return state->MediaTransfer(state->MediaContext, state->ExecDrive, state->PresentCylinder[state->ExecDrive],
                                state->ExecHead, state->ExecSector, buffer, isWrite);
}

/* Can channel 2 move a byte at all? DMAGATE clear disconnects the request line (fdc.md 7). */
static INT FdcDmaConnected(PCFDC_STATE state)
{
    return state->Dma && (state->Dor & FDC_DOR_DMA_GATE);
}

/* DMA: every sector of the command now, until the 8237 says TC or the track runs out. */
static VOID FdcRunDma(PFDC_STATE state)
{
    INT isWrite = !FdcIsReadCommand(state->ExecOpcode);

    for (;;)
    {
        BYTE status1;
        BYTE status2;
        BYTE buffer[FDC_SECTOR_SIZE];
        INT isTerminalCount = 0;
        INT isEndOfCylinder;
        UINT32 moved;

        if (!FdcFindSector(state, &status1, &status2))
        {
            FdcDataDone(state, FDC_ST0_ABNORMAL, status1, status2);
            return;
        }

        if (!FdcDmaConnected(state))
        {
            FdcDataDone(state, FDC_ST0_ABNORMAL, FDC_ST1_OVERRUN, FDC_ST2_CLEAR);
            return;
        }

        if (isWrite)
        {
            moved = VddDmaRead(state->Dma, FDC_DMA_CHANNEL, buffer, FDC_SECTOR_SIZE, &isTerminalCount);

            if (moved < FDC_SECTOR_SIZE && !isTerminalCount)
            {
                FdcDataDone(state, FDC_ST0_ABNORMAL, FDC_ST1_OVERRUN, FDC_ST2_CLEAR);
                return;
            }

            if (!FdcSectorIo(state, buffer, TRUE))
            {
                FdcDataDone(state, FDC_ST0_ABNORMAL, FDC_ST1_NOT_WRITABLE, FDC_ST2_CLEAR);
                return;
            }

            state->SectorsWritten++;
        }
        else
        {
            if (!FdcSectorIo(state, buffer, FALSE))
            {
                FdcDataDone(state, FDC_ST0_ABNORMAL, FDC_ST1_NO_DATA, FDC_ST2_CLEAR);
                return;
            }

            moved = VddDmaWrite(state->Dma, FDC_DMA_CHANNEL, buffer, FDC_SECTOR_SIZE, &isTerminalCount);

            if (moved < FDC_SECTOR_SIZE && !isTerminalCount)
            {
                FdcDataDone(state, FDC_ST0_ABNORMAL, FDC_ST1_OVERRUN, FDC_ST2_CLEAR);
                return;
            }

            state->SectorsRead++;
        }

        isEndOfCylinder = FdcAdvance(state);

        if (isTerminalCount)
        {
            FdcDataDone(state, FDC_ST0_NORMAL, 0, FDC_ST2_CLEAR);
            return;
        }

        if (isEndOfCylinder)                    /* EOT passed with no TC: the datasheet's EN */
        {
            FdcDataDone(state, FDC_ST0_ABNORMAL, FDC_ST1_END_OF_CYLINDER, FDC_ST2_CLEAR);
            return;
        }
    }
}

/* Non-DMA: open the sector at C/H/R for the host to move through 3F5h. 0 = the command
 * has ended (with its result) instead.
 */
static INT FdcOpenSector(PFDC_STATE state)
{
    BYTE status1;
    BYTE status2;

    if (!FdcFindSector(state, &status1, &status2))
    {
        FdcDataDone(state, FDC_ST0_ABNORMAL, status1, status2);
        return 0;
    }

    if (FdcIsReadCommand(state->ExecOpcode) && !FdcSectorIo(state, state->ExecBuffer, FALSE))
    {
        FdcDataDone(state, FDC_ST0_ABNORMAL, FDC_ST1_NO_DATA, FDC_ST2_CLEAR);
        return 0;
    }

    state->ExecPosition = 0;
    state->ExecLength = FDC_SECTOR_SIZE;
    state->Phase = FDC_PHASE_EXECUTION;
    return 1;
}

/* Non-DMA: the sector in flight is complete. Without the 8237 there is no TC, so the
 * command runs to EOT and ends there with EN -- the datasheet's answer, and what a
 * non-DMA driver expects.
 */
static VOID FdcSectorDone(PFDC_STATE state)
{
    if (!FdcIsReadCommand(state->ExecOpcode))
    {
        if (!FdcSectorIo(state, state->ExecBuffer, TRUE))
        {
            FdcDataDone(state, FDC_ST0_ABNORMAL, FDC_ST1_NOT_WRITABLE, FDC_ST2_CLEAR);
            return;
        }

        state->SectorsWritten++;
    }
    else
    {
        state->SectorsRead++;
    }

    if (FdcAdvance(state))
    {
        FdcDataDone(state, FDC_ST0_ABNORMAL, FDC_ST1_END_OF_CYLINDER, FDC_ST2_CLEAR);
        return;
    }

    FdcOpenSector(state);
}

/* FORMAT TRACK, one sector: `identity` is its C H R N, and the sector is filled with D.
 * The ID is written where the head IS -- the physical track -- whatever C it names.
 * 0 = the command has ended (with its result).
 */
static INT FdcFormatSector(PFDC_STATE state, const BYTE *identity)
{
    BYTE status1;
    BYTE status2;
    UINT byteIndex;

    state->ExecCylinder = state->PresentCylinder[state->ExecDrive];      /* judged on the physical track */
    state->ExecHeadAddress = state->ExecHead;
    state->ExecSector = identity[FDC_ID_SECTOR];
    state->ExecSizeCode = identity[FDC_ID_SIZE_CODE];

    if (!FdcFindSector(state, &status1, &status2))
    {
        FdcDataDone(state, FDC_ST0_ABNORMAL, status1, status2);
        return 0;
    }

    for (byteIndex = 0; byteIndex < FDC_SECTOR_SIZE; ++byteIndex)
        state->ExecBuffer[byteIndex] = state->ExecFill;

    if (!FdcSectorIo(state, state->ExecBuffer, TRUE))
    {
        FdcDataDone(state, FDC_ST0_ABNORMAL, FDC_ST1_NOT_WRITABLE, FDC_ST2_CLEAR);
        return 0;
    }

    state->ExecCylinder = identity[FDC_ID_CYLINDER];                    /* the result reports the ID */
    state->ExecHeadAddress = identity[FDC_ID_HEAD];

    if (--state->ExecSectorsLeft == 0)
    {
        state->TracksFormatted++;
        FdcDataDone(state, FDC_ST0_NORMAL, 0, FDC_ST2_CLEAR);
        return 0;
    }

    return 1;
}

static VOID FdcFormat(PFDC_STATE state)
{
    state->ExecOpcode = FDC_CMD_FORMAT_TRACK;
    state->ExecSizeCode = state->Command[FDC_PARAMETER_2];
    state->ExecSectorsLeft = state->Command[FDC_PARAMETER_3];
    state->ExecFill = state->Command[FDC_PARAMETER_5];
    state->ExecCylinder = state->PresentCylinder[state->ExecDrive];
    state->ExecHeadAddress = state->ExecHead;
    state->ExecSector = FDC_FIRST_SECTOR;
    state->LastEot = state->ExecSectorsLeft;

    if (!state->ExecSectorsLeft)
    {
        FdcDataDone(state, FDC_ST0_NORMAL, 0, FDC_ST2_CLEAR);
        return;
    }

    if (FdcIsNonDma(state))                             /* the IDs come through 3F5h */
    {
        state->ExecPosition = 0;
        state->ExecLength = FDC_ID_BYTES;
        state->Phase = FDC_PHASE_EXECUTION;
        return;
    }

    for (;;)
    {
        BYTE identity[FDC_ID_BYTES];
        INT isTerminalCount = 0;

        if (!FdcDmaConnected(state)
            || VddDmaRead(state->Dma, FDC_DMA_CHANNEL, identity, FDC_ID_BYTES, &isTerminalCount) < FDC_ID_BYTES)
        {
            FdcDataDone(state, FDC_ST0_ABNORMAL, FDC_ST1_OVERRUN, FDC_ST2_CLEAR);
            return;
        }

        if (!FdcFormatSector(state, identity))
            return;
    }
}

/* READ DATA, WRITE DATA, READ TRACK, and the two DELETED forms. */
static VOID FdcDataCommand(PFDC_STATE state, BYTE opcode)
{
    state->ExecOpcode = opcode;
    state->ExecCylinder = state->Command[FDC_PARAMETER_2];
    state->ExecHeadAddress = state->Command[FDC_PARAMETER_3];
    state->ExecSector = state->Command[FDC_PARAMETER_4];
    state->ExecSizeCode = state->Command[FDC_PARAMETER_5];
    state->ExecEot = state->Command[FDC_PARAMETER_6];
    state->ExecIsMultiTrack = (BYTE)((state->Command[0] & FDC_MULTI_TRACK) ? TRUE : FALSE);
    state->LastEot = state->ExecEot;

    if (opcode == FDC_CMD_READ_TRACK)                      /* the whole track, from its first sector */
    {
        state->ExecSector = FDC_FIRST_SECTOR;
        state->ExecIsMultiTrack = 0;
    }

    /* [CAUTION]: AN IMAGE HAS NO DELETED-DATA MARKS. READ DELETED finds only normal data,
     * which the chip reports as CONTROL MARK; WRITE DELETED writes the data, and its mark
     * is lost with the image format. Neither has been seen on the shelf.
     */
    if (opcode == FDC_CMD_READ_DELETED_DATA)
    {
        BYTE status1;
        BYTE status2;

        if (!FdcFindSector(state, &status1, &status2))
            FdcDataDone(state, FDC_ST0_ABNORMAL, status1, status2);
        else
            FdcDataDone(state, FDC_ST0_ABNORMAL, FDC_ST1_NO_DATA, FDC_ST2_CONTROL_MARK);

        return;
    }

    if (opcode == FDC_CMD_WRITE_DELETED_DATA)
        state->ExecOpcode = FDC_CMD_WRITE_DATA;

    if (FdcIsNonDma(state))
        FdcOpenSector(state);
    else
        FdcRunDma(state);
}

/* Every parameter byte has arrived: carry the command out. */
static VOID FdcExecute(PFDC_STATE state)
{
    BYTE opcode = (BYTE)(state->Command[0] & FDC_OPCODE_MASK);
    BYTE drive;
    BYTE head;

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

        if (!state->PresentCylinder[drive])
            resultByte |= FDC_ST3_TRACK_0;

        FdcResult(state, &resultByte, 1);
        break;
    }

    case FDC_CMD_DUMPREG:                  /* DUMPREG -- ten bytes, no side effects */
    {
        BYTE bytes[FDC_DUMPREG_BYTES];
        bytes[FDC_DUMPREG_PCN0] = state->PresentCylinder[FDC_DRIVE_0];
        bytes[FDC_DUMPREG_PCN1] = state->PresentCylinder[FDC_DRIVE_1];
        bytes[FDC_DUMPREG_PCN2] = state->PresentCylinder[FDC_DRIVE_2];
        bytes[FDC_DUMPREG_PCN3] = state->PresentCylinder[FDC_DRIVE_3];
        bytes[FDC_DUMPREG_SRT_HUT] = state->StepRateHeadUnload;
        bytes[FDC_DUMPREG_HLT_ND] = state->HeadLoadNonDma;
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

    /* THE DATA COMMANDS (#178) -- against the host's image, over channel 2 or through
     * 3F5h. See FdcDataCommand. A failure is the chip's own: ST0 interrupt code 01 with
     * the ST1/ST2 reason, never "invalid command" -- a 90h part knows these commands, and
     * a driver can act on a media error where it cannot act on a contradiction.
     */
    case FDC_CMD_READ_TRACK:
    case FDC_CMD_WRITE_DATA:
    case FDC_CMD_READ_DATA:
    case FDC_CMD_WRITE_DELETED_DATA:
    case FDC_CMD_READ_DELETED_DATA:
        state->ExecDrive = (BYTE)(state->Command[1] & FDC_DRIVE_MASK);
        state->ExecHead  = (BYTE)((state->Command[1] >> FDC_HEAD_SHIFT) & 1);
        FdcDataCommand(state, opcode);
        break;

    case FDC_CMD_FORMAT_TRACK:                  /* FORMAT TRACK -- destructive, as on the part */
        state->ExecDrive = (BYTE)(state->Command[1] & FDC_DRIVE_MASK);
        state->ExecHead  = (BYTE)((state->Command[1] >> FDC_HEAD_SHIFT) & 1);
        FdcFormat(state);
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
    if (state->Phase == FDC_PHASE_RESULT)
        return;

    /* #178: non-DMA execution, the host supplying data -- a WRITE's sector, or FORMAT's
     * four ID bytes per sector. (During a READ the host should not be writing; ignored.)
     */
    if (state->Phase == FDC_PHASE_EXECUTION)
    {
        if (FdcIsReadCommand(state->ExecOpcode))
            return;

        state->ExecBuffer[state->ExecPosition++] = value;

        if (state->ExecPosition < state->ExecLength)
            return;

        if (state->ExecOpcode == FDC_CMD_FORMAT_TRACK)
        {
            BYTE identity[FDC_ID_BYTES];
            UINT byteIndex;

            for (byteIndex = 0; byteIndex < FDC_ID_BYTES; ++byteIndex)
                identity[byteIndex] = state->ExecBuffer[byteIndex];

            if (FdcFormatSector(state, identity))
                state->ExecPosition = 0;                /* the next sector's ID */

            return;
        }

        FdcSectorDone(state);
        return;
    }

    if (!state->CommandLength)
        state->CommandWanted = FdcCommandLength(value);

    if (state->CommandLength < sizeof(state->Command))
        state->Command[state->CommandLength] = value;

    state->CommandLength++;

    if (state->CommandLength >= state->CommandWanted)
        FdcExecute(state);
}

static BYTE FdcFifoRead(PFDC_STATE state)
{
    BYTE value;

    /* #178: non-DMA execution, the chip handing a READ's data out byte by byte. */
    if (state->Phase == FDC_PHASE_EXECUTION)
    {
        if (!FdcIsReadCommand(state->ExecOpcode))
            return 0;

        value = state->ExecBuffer[state->ExecPosition++];

        if (state->ExecPosition >= state->ExecLength)
            FdcSectorDone(state);

        return value;
    }

    /* Reading when there is nothing to read hands back the last byte on a real
     * part. Returning 0xFF here would re-create the very ambiguity this device
     * exists to remove.
     */
    if (state->Phase != FDC_PHASE_RESULT)
        return state->ResultLength ? state->Result[state->ResultLength - 1] : FDC_ST0_INVALID_COMMAND;

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
        state->Phase = FDC_PHASE_COMMAND;
        state->ResultPosition = 0;
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

    case FDC_TDR:
        state->Tdr = byteValue;
        break;

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

    case FDC_FIFO:
        FdcFifoWrite(state, byteValue);
        break;

    case FDC_DIR:
        state->Ccr = (BYTE)(byteValue & FDC_CCR_DATA_RATE);
        break;  /* the write side is CCR */

    default:
        break;
    }
}

VOID VddFdcPortIn(PVOID context, WORD port, BYTE width, UINT32 *value)
{
    PFDC_STATE state = (PFDC_STATE)context;

    (VOID)width;

    switch (port)
    {
    case FDC_DOR:
        *value = state->Dor;
        break;       /* readable on an 82077AA */

    case FDC_TDR:
        *value = state->Tdr;
        break;

    case FDC_MSR:
        *value = VddFdcMainStatus(state);
        break;

    case FDC_FIFO:
        *value = FdcFifoRead(state);
        break;

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

    default:
        *value = FDC_UNDRIVEN_BUS;
        break;
    }
}

VOID VddFdcReset(PVOID context)
{
    PFDC_STATE state = (PFDC_STATE)context;
    PVDD_BUS bus = state->Bus;
    PDMA_STATE dma = state->Dma;                                     /* #178: links, not chip state */
    PFDC_MEDIA_GEOMETRY mediaGeometry = state->MediaGeometry;
    PFDC_MEDIA_TRANSFER mediaTransfer = state->MediaTransfer;
    PVOID mediaContext = state->MediaContext;
    UINT byteIndex;

    for (byteIndex = 0; byteIndex < sizeof(*state); ++byteIndex)
        ((BYTE *)state)[byteIndex] = 0;

    state->Bus = bus;
    state->Dma = dma;
    state->MediaGeometry = mediaGeometry;
    state->MediaTransfer = mediaTransfer;
    state->MediaContext = mediaContext;
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

VOID VddFdcSetCylinder(PFDC_STATE state, BYTE drive, BYTE cylinder)
{
    state->PresentCylinder[drive & FDC_DRIVE_MASK] = cylinder;
}

VOID VddFdcBiosMotorTick(PVOID context, volatile BYTE *biosData)
{
    PFDC_STATE state = (PFDC_STATE)context;
    BYTE count = biosData[BIOS_BDA_DISKETTE_MOTOR_COUNT];

    if (!count)
        return;                                 /* nothing counting down */

    biosData[BIOS_BDA_DISKETTE_MOTOR_COUNT] = --count;

    if (count)
        return;

    /* The count ran out: the motors go off, in the BIOS's record and at the chip. */
    biosData[BIOS_BDA_DISKETTE_MOTOR_STATUS] &= (BYTE)~BIOS_DISKETTE_MOTOR_BITS;

    if (state)
        VddFdcPortOut(state, FDC_DOR, 1, FDC_DOR_AFTER_MOTOR_OFF);
}

INT VddFdcInitialize(PVDD_BUS bus, PVOID context)
{
    PFDC_STATE state = (PFDC_STATE)context;

    state->Bus = bus;

    if (!state->Dor)
        VddFdcReset(state);                         /* the host builds us zeroed */

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
    if (VddClaimPorts(bus, FDC_DOR, FDC_FIFO, VddFdcPortIn, VddFdcPortOut, state))
        return FDC_FAILED;

    if (VddClaimPorts(bus, FDC_DIR, FDC_DIR,  VddFdcPortIn, VddFdcPortOut, state))
        return FDC_FAILED;

    return FDC_OK;
}
