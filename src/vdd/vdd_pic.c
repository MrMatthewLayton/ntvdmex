/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * See vdd_pic.h.  8259A pair on the VDD bus.  No Windows calls, only Windows types.
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#include "vdd_pic.h"

/* The 8259A's lines and the PC's two chips. */
#define PIC_LINE_MASK                       7       /* A line number within one chip */
#define PIC_LINES                           16      /* Master IRQ0-7, slave IRQ8-15 */
#define PIC_CASCADE_LINE                    2       /* The master's IR2 carries the slave */
#define PIC_CASCADE_BIT                     0x04
#define PIC_FIXED_LOWEST                    7       /* IR7 lowest: the fixed order a BIOS leaves */
#define PIC_NO_LINE                         (-1)
#define PIC_ALL_MASKED                      0xFF
#define PIC_BIOS_MASTER_MASK                0xFC    /* IRQ0 + IRQ1 enabled */
#define PIC_NO_VECTOR                       0

/* Ports. */
#define PIC_MASTER_COMMAND                  0x20
#define PIC_MASTER_DATA                     0x21
#define PIC_SLAVE_COMMAND                   0xA0
#define PIC_SLAVE_DATA                      0xA1

/* ICW1 / OCW3 / OCW2 command bits (8259A datasheet). */
#define PIC_ICW1                            0x10    /* bit 4: begin initialisation */
#define PIC_ICW1_IC4                        0x01    /* An ICW4 will follow */
#define PIC_OCW3                            0x08    /* bit 3 (with bit 4 clear): OCW3 */
#define PIC_OCW3_RR                         0x02    /* Read register command */
#define PIC_OCW3_RIS                        0x01    /* 0 = IRR, 1 = ISR */
#define PIC_OCW3_ESMM                       0x40    /* Enable special mask mode change */
#define PIC_OCW3_SMM                        0x20    /* Special mask mode */
#define PIC_OCW3_POLL                       0x04
#define PIC_OCW2_COMMAND_MASK               0xE0    /* R, SL, EOI */
#define PIC_OCW2_NONSPECIFIC_EOI            0x20
#define PIC_OCW2_SPECIFIC_EOI               0x60
#define PIC_OCW2_ROTATE_NONSPECIFIC_EOI     0xA0
#define PIC_OCW2_ROTATE_SPECIFIC_EOI        0xE0
#define PIC_OCW2_SET_PRIORITY               0xC0
#define PIC_OCW2_ROTATE_AEOI_SET            0x80
#define PIC_OCW2_ROTATE_AEOI_CLEAR          0x00
#define PIC_ICW4_AEOI                       0x02
#define PIC_ICW4_SFNM                       0x10

/* ICW sequence steps. */
#define PIC_ICW_RUNNING                     0
#define PIC_ICW_EXPECT_ICW2                 1
#define PIC_ICW_EXPECT_ICW3                 2
#define PIC_ICW_EXPECT_ICW4                 3

/* A poll read. */
#define PIC_POLL_PENDING                    0x80    /* bit 7: an interrupt is takeable */
#define PIC_POLL_NONE                       0x00
#define PIC_OK                              0
#define PIC_FAILED                          (-1)

/* IN-SERVICE UPDATES ARE ATOMIC:
 * The host acknowledges IRQ0 from its tick courier, which by design runs with the
 * guest thread frozen and NO device lock held, while the guest's own EOIs (and every
 * other line's acknowledge) arrive under that lock. A plain `isr |= bit` / `isr &=
 * ~bit` is a byte read-modify-write, and two of them racing lose one bit -- IRQ1's
 * in-service bit IS the keyboard re-entrancy guard, so losing it is "press a key and
 * everything hangs" all over again. The host already OR's IRR atomically for the
 * same reason (host_irq_sink); ISR gets the same treatment here so that IRQ0 can be
 * held in service across threads. GCC/Clang builtins, so this file stays portable C.
 */
#define PIC_ISR_SET(chip, bit)              ((VOID)__sync_fetch_and_or (&(chip)->Isr, (BYTE)(bit)))
#define PIC_ISR_CLEAR(chip, bit)            ((VOID)__sync_fetch_and_and(&(chip)->Isr, (BYTE)~(BYTE)(bit)))

/* IRR likewise: the host raises with an atomic OR from the PIT's lock, so the clear
 * at acknowledge must be atomic too or a raise can be lost under it.
 */
#define PIC_IRR_CLEAR(chip, bit)            ((VOID)__sync_fetch_and_and(&(chip)->Irr, (BYTE)~(BYTE)(bit)))

/* PRIORITY IS A RING, NOT A BIT NUMBER. (#174):
 * The 8259A's priorities rotate: OCW2 can name any IR line the LOWEST, and the one
 * after it (mod 8) becomes the highest. `LowestPriority` = 7 is the fixed order every PC
 * BIOS leaves behind (IR0 highest, IR7 lowest), and in that state PicRank(chip, n) == n,
 * so every rule below reduces EXACTLY to the lowest-bit-first test this file used
 * before rotation existed. A guest that never writes C0h/A0h/E0h/80h sees no change.
 * - rank 0 = highest priority.
 */
static INT PicRank(PCPIC_CHIP chip, INT line)
{
    return (line - (INT)chip->LowestPriority - 1) & PIC_LINE_MASK;
}

/* The highest-priority line set in `mask`, in the chip's CURRENT rotation; -1 if none. */
static INT PicTopLine(PCPIC_CHIP chip, BYTE mask)
{
    INT rank;

    for (rank = 0; rank < PIC_LINES_PER_CHIP; ++rank)
    {
        INT line = ((INT)chip->LowestPriority + 1 + rank) & PIC_LINE_MASK;

        if (mask & (1u << line))
            return line;
    }

    return PIC_NO_LINE;
}

/* The in-service bits that stop `line` being delivered on this chip.
 * - FULLY NESTED: every in-service line at the same or higher priority.
 * - SPECIAL MASK MODE (OCW3 ESMM/SMM): a MASKED line's in-service bit stops counting.
 *   The datasheet: "when a mask bit is set in OCW1, it inhibits further interrupts at
 *   that level and enables interrupts from all other levels (lower as well as higher)
 *   that are not masked." It is how a handler masks its own line and lets everything
 *   else -- including LOWER priorities -- in before it EOIs. An UNMASKED in-service
 *   line still blocks as usual (QEMU, MAME and Bochs all model it this way: the
 *   resolver sees ISR & ~IMR).
 * - SPECIAL FULLY NESTED MODE (ICW4 SFNM, master only): IR2 in service does not block
 *   IR2 itself, so a HIGHER-priority slave line can interrupt a lower one's handler --
 *   the slave's own resolver decides that. IR2 in service still blocks IR3-7: SFNM
 *   relaxes the equal-priority rule for the cascade input and nothing else.
 */
static BYTE PicBlockers(PCPIC_CHIP chip, INT line, INT isMaster)
{
    BYTE effective = chip->Isr;
    BYTE blockers = 0;
    INT otherLine;
    INT lineRank = PicRank(chip, line);

    if (chip->IsSpecialMaskMode)
        effective = (BYTE)(effective & ~chip->Imr);

    for (otherLine = 0; otherLine < PIC_LINES_PER_CHIP; ++otherLine)
    {
        if (!(effective & (1u << otherLine)))
            continue;

        if (isMaster && chip->IsSpecialFullyNested && line == PIC_CASCADE_LINE && otherLine == PIC_CASCADE_LINE)
            continue;

        if (PicRank(chip, otherLine) <= lineRank)
            blockers = (BYTE)(blockers | (1u << otherLine));
    }

    return blockers;
}

/* May this chip pass `line` on right now? Masked, or outranked by something in service,
 * is a no. The one resolver every path uses -- delivery, the cascade, and the poll.
 */
static INT PicIsLineOpen(PCPIC_CHIP chip, INT line, INT isMaster)
{
    if (chip->Imr & (1u << line))
        return FALSE;

    return PicBlockers(chip, line, isMaster) == 0;
}

static VOID PicChipReset(PPIC_CHIP chip, BYTE base)
{
    chip->Imr = PIC_ALL_MASKED;    /* everything masked until the guest unmasks */
    chip->Irr = chip->Isr = 0;
    chip->VectorBase = base;
    chip->IcwStep = PIC_ICW_RUNNING;
    chip->IsIcw4Needed = FALSE;
    chip->IsIsrSelected = FALSE;
    chip->IsAutoEoi = FALSE;
    chip->IsPollArmed = FALSE;
    chip->LowestPriority = PIC_FIXED_LOWEST;
    chip->IsRotateInAutoEoi = FALSE;
    chip->IsSpecialMaskMode = FALSE;
    chip->IsSpecialFullyNested = FALSE;   /* a BIOS's chip */
}

/* The in-service half of an acknowledge cycle, on one chip: ISR set -- or, in AEOI,
 * the automatic EOI, which in ROTATE-IN-AEOI mode also makes this line the lowest
 * priority (it is a "rotate on non-specific EOI" with the EOI done for you).
 *
 * [CAUTION]: prio_low is a plain byte store. It only moves when a guest has programmed AEOI +
 * rotate, which no PC BIOS does; a byte store is not a read-modify-write, so it
 * cannot lose a concurrent ISR/IRR update the way `isr |= bit` could.
 */
static VOID PicInterruptAcknowledge(PPIC_CHIP chip, INT line)
{
    if (!chip->IsAutoEoi)
        PIC_ISR_SET(chip, 1u << line);
    else if (chip->IsRotateInAutoEoi)
        chip->LowestPriority = (BYTE)line;
}

/* port side: */

/* base port (0x20 / 0xA0): ICW1, OCW2 (EOI), OCW3 (read select) */
static VOID PicCommandWrite(PPIC_CHIP chip, BYTE value)
{
    if (value & PIC_ICW1)                       /* ICW1: begin initialisation */
    {
        chip->IcwStep = PIC_ICW_EXPECT_ICW2;
        chip->IsIcw4Needed = (BYTE)(value & PIC_ICW1_IC4);
        chip->Isr = chip->Irr = 0;
        chip->Imr = 0;                         /* ICW1 clears the mask register */
        /* THE REST OF ICW1's SIDE EFFECTS (#174), from the datasheet's list:
         * IR7 is assigned the lowest priority, Special Mask Mode is cleared, and
         * the status read is set to the IRR. The rotate-in-AEOI flag is not on
         * Intel's list; it is cleared here too, as QEMU's init reset does, on the
         * grounds that a re-initialised chip is a fresh one.
         *
         * [CAUTION]: THE READ-SELECT RESET IS SPEC-DERIVED AND UNVERIFIED BY ORACLE. p_pic's
         * pic.icw1.readsel only discriminates on PCem, and PCem does NOT reset it
         * (AH=00, AL=01). QEMU and dosbox-x clear the IRR at ICW1, so their reads
         * cannot say which register came back. One emulator that disagrees with the
         * datasheet is not a second oracle; the datasheet wins until real silicon
         * says otherwise. docs/inventory/pic.md 3.
         */
        chip->IsIsrSelected = FALSE;
        chip->LowestPriority = PIC_FIXED_LOWEST;
        chip->IsSpecialMaskMode = FALSE;
        chip->IsRotateInAutoEoi = FALSE;
        /* "If IC4 = 0, then all functions selected in ICW4 are set to zero" -- with
         * no ICW4 coming, AEOI and SFNM must not survive from the last init.
         */
        if (!chip->IsIcw4Needed)
        {
            chip->IsAutoEoi = FALSE;
            chip->IsSpecialFullyNested = FALSE;
        }

        return;
    }

    if (value & PIC_OCW3)                       /* OCW3 */
    {
        if (value & PIC_OCW3_RR)
            chip->IsIsrSelected = (BYTE)(value & PIC_OCW3_RIS);                        /* 0=IRR, 1=ISR */

        /* Special Mask Mode: ESMM (bit 6) enables the SMM bit (bit 5) to mean
         * anything; ESMM clear leaves the mode alone. 11 = set, 10 = clear.
         */
        if (value & PIC_OCW3_ESMM)
            chip->IsSpecialMaskMode = (BYTE)((value & PIC_OCW3_SMM) ? TRUE : FALSE);

        /* THE POLL COMMAND, AND WHY DROPPING IT WAS THE "RUNS BUT LIES"
         * SHAPE. A poll read and a status read are THE SAME `IN` ON THE SAME
         * PORT; the only thing that tells them apart is which OCW3 was written
         * last. So a guest that polls never got an error -- it got whatever
         * register the read select happened to name, and read it as
         * "bit 7 = an interrupt is pending, bits 2:0 = its level". An IRR of
         * 0x01 (IRQ0 requested) reads as NO INTERRUPT because bit 7 is clear;
         * an IRR of 0x80 (IRQ7) reads as "pending, level 0". Plausible, wrong,
         * silent.
         *
         * [CAUTION]: ORACLES: MS-DOS 6.22 under QEMU implements it (p_pic pic.ocw3.poll =
         * 0x00); dosbox-x and PCem both drop the P bit and answer 0x01, the
         * selected ISR. Two of three do not model the feature, so their answer
         * is the absence of a measurement rather than a measurement of absence
         * -- the same footing as the 8254's BCD bit, except that here the one
         * host which DOES implement it agrees with the datasheet.
         */
        if (value & PIC_OCW3_POLL)
            chip->IsPollArmed = TRUE;

        return;
    }

    /* OCW2: the EOI family. */
    /* [CAUTION]: NON-SPECIFIC EOI CLEARS THE HIGHEST-PRIORITY IN-SERVICE BIT IN THE CURRENT
     * ROTATION, against the full ISR -- Special Mask Mode does not hide masked
     * bits from it. The datasheet says a guest that disturbs the nesting (SMM,
     * rotation mid-handler) must use a SPECIFIC EOI; this is what the chip does
     * when it does not.
     */
    switch (value & PIC_OCW2_COMMAND_MASK)
    {
    case PIC_OCW2_NONSPECIFIC_EOI:                              /* non-specific EOI */
    {
        INT topLine = PicTopLine(chip, chip->Isr);

        if (topLine >= 0)
            PIC_ISR_CLEAR(chip, 1u << topLine);

        break; }

    case PIC_OCW2_SPECIFIC_EOI:             /* specific EOI */
        PIC_ISR_CLEAR(chip, 1u << (value & PIC_LINE_MASK));
        break;

    /* Rotate on non-specific EOI: the line it ends becomes the LOWEST priority. With
     * nothing in service there is no line to name, so nothing rotates (QEMU agrees).
     */
    case PIC_OCW2_ROTATE_NONSPECIFIC_EOI:
    {
        INT topLine = PicTopLine(chip, chip->Isr);

        if (topLine >= 0)
        {
            PIC_ISR_CLEAR(chip, 1u << topLine);
            chip->LowestPriority = (BYTE)topLine;
        }

        break; }

    /* ROTATE ON SPECIFIC EOI. IT IS STILL AN EOI, AND THIS USED TO BE A NOP:
     * E0h+L2:L0 ends the interrupt on L AND makes L the lowest priority. Until #174
     * only the EOI half was modelled; THE EOI HALF IS THE ONE THAT IS NOT OPTIONAL:
     * an in-service bit that is never cleared does not cost one interrupt, it kills
     * that priority level and everything below it for the rest of the run.
     *
     * [INFO]: ALL THREE ORACLES AGREE the bit goes (p_pic pic.ocw2.rot.speoi = 0 on
     * 6.22, dosbox-x and PCem alike). Unanimous, so no judgement was needed.
     * The rotation half is datasheet only -- no probe asks it yet.
     */
    case PIC_OCW2_ROTATE_SPECIFIC_EOI:
        PIC_ISR_CLEAR(chip, 1u << (value & PIC_LINE_MASK));
        chip->LowestPriority = (BYTE)(value & PIC_LINE_MASK);
        break;

    /* Set priority: L becomes the lowest, L+1 the highest. NOT an EOI. */
    case PIC_OCW2_SET_PRIORITY:
        chip->LowestPriority = (BYTE)(value & PIC_LINE_MASK);
        break;

    /* Rotate in auto-EOI mode, set (80h) / clear (00h). Takes effect at the next
     * acknowledge, and only while ICW4 selected AEOI (PicInterruptAcknowledge). NOT an EOI.
     */
    case PIC_OCW2_ROTATE_AEOI_SET:
        chip->IsRotateInAutoEoi = TRUE;
        break;

    case PIC_OCW2_ROTATE_AEOI_CLEAR:
        chip->IsRotateInAutoEoi = FALSE;
        break;

    default:
        /* 40h is the datasheet's "no operation". None of C0h/80h/00h/40h ends an
         * interrupt -- treating the whole 0xE0 field as "some kind of EOI" would
         * break the re-entrancy guard in the other direction.
         */
        break;
    }
}

/* data port (0x21 / 0xA1): ICW2/3/4 during init, otherwise OCW1 = the mask */
static VOID PicDataWrite(PPIC_CHIP chip, BYTE value)
{
    switch (chip->IcwStep)
    {
    case PIC_ICW_EXPECT_ICW2:
        chip->VectorBase = value;
        chip->IcwStep = PIC_ICW_EXPECT_ICW3;
        return;          /* ICW2: vector base */

    case PIC_ICW_EXPECT_ICW3:
        chip->IcwStep = chip->IsIcw4Needed ? PIC_ICW_EXPECT_ICW4 : PIC_ICW_RUNNING;
        return;  /* ICW3: cascade map */

    case PIC_ICW_EXPECT_ICW4:
        chip->IsAutoEoi = (BYTE)((value & PIC_ICW4_AEOI) ? TRUE : FALSE);   /* ICW4 */
            /* SFNM, bit 4. A PC BIOS writes 01h -- off -- so the default stays the
             * restrictive fully nested mode (PicBlockers). uPM and BUF have no
             * effect a host without a bus cycle can show.
             */
            chip->IsSpecialFullyNested = (BYTE)((value & PIC_ICW4_SFNM) ? TRUE : FALSE);
            chip->IcwStep = PIC_ICW_RUNNING;
            return;

    default:
        chip->Imr = value;
        return;                           /* OCW1: mask */
    }
}

static VOID PicPortOut(PVOID context, WORD port, BYTE width, UINT32 value)
{
    PPIC_STATE state = (PPIC_STATE)context;
    BYTE byteValue = (BYTE)value;

    (VOID)width;

    switch (port)
    {
    case PIC_MASTER_COMMAND:
        PicCommandWrite(&state->Master, byteValue);
        break;

    case PIC_MASTER_DATA:
        PicDataWrite(&state->Master, byteValue);
        break;

    case PIC_SLAVE_COMMAND:
        PicCommandWrite(&state->Slave, byteValue);
        break;

    case PIC_SLAVE_DATA:
        PicDataWrite(&state->Slave, byteValue);
        break;

    default:
        break;
    }
}

/* A poll read: report the highest-priority PENDING line and acknowledge it.
 *
 * [CAUTION]: PENDING, NOT IN SERVICE. Bit 7 answers "is there an interrupt to take", and a
 * line already in service is not one -- which is what makes the idle-with-IRQ0-
 * in-service case (p_pic) discriminating at all. Requests that are masked, or
 * outranked by something already in service, are not available either: the poll
 * goes through the same resolver a delivery would -- rotation and SMM included.
 * - Only the highest-priority pending line is a candidate: the chip offers that one
 *   or nothing. Nothing is lost by not looking further -- whatever in-service bit
 *   blocks it outranks every pending line below it too, in SMM as well.
 */
static BYTE PicPollRead(PPIC_CHIP chip, INT isMaster)
{
    BYTE readyLines = (BYTE)(chip->Irr & ~chip->Imr);
    INT topLine = PicTopLine(chip, readyLines);

    if (topLine < 0 || !PicIsLineOpen(chip, topLine, isMaster))
        return PIC_POLL_NONE;                    /* bit 7 clear: none takeable */

    PIC_IRR_CLEAR(chip, 1u << topLine);
    PicInterruptAcknowledge(chip, topLine);
    return (BYTE)(PIC_POLL_PENDING | topLine);
}

static VOID PicPortIn(PVOID context, WORD port, BYTE width, UINT32 *value)
{
    PPIC_STATE state = (PPIC_STATE)context;
    PPIC_CHIP chip = (port < PIC_SLAVE_COMMAND) ? &state->Master : &state->Slave;

    (VOID)width;

    if (port == PIC_MASTER_DATA || port == PIC_SLAVE_DATA)
    {
        *value = chip->Imr;
        return;
    }

    /* A POLL IS A ONE-SHOT: OCW3's P bit arms the NEXT read only, and this read
     * consumes it. Leaving it armed would mean a guest that polls once never sees
     * a status byte again.
     */
    if (chip->IsPollArmed)
    {
        chip->IsPollArmed = FALSE;
        *value = PicPollRead(chip, chip == &state->Master);
        return;
    }

    *value = chip->IsIsrSelected ? chip->Isr : chip->Irr;
}

/* host side: */

VOID VddPicRaise(PPIC_STATE state, BYTE irq)
{
    if (irq < PIC_LINES_PER_CHIP)
        state->Master.Irr |= (BYTE)(1u << irq);
    else if (irq < PIC_LINES)
        state->Slave.Irr |= (BYTE)(1u << (irq - PIC_LINES_PER_CHIP));
}

INT VddPicCanDeliver(PPIC_STATE state, BYTE irq)
{
    PPIC_CHIP chip;

    if (irq >= PIC_LINES)
        return FALSE;

    chip   = (irq < PIC_LINES_PER_CHIP) ? &state->Master : &state->Slave;
    /* Masked, or in service at this or higher priority, blocks delivery -- this is
     * what stops a handler being re-entered before it EOIs.
     */
    if (!PicIsLineOpen(chip, irq & PIC_LINE_MASK, irq < PIC_LINES_PER_CHIP))
        return FALSE;

    /* A SLAVE LINE MUST ALSO GET THROUGH THE MASTER'S IR2, AND IN FULLY NESTED
     * MODE IR2 IN SERVICE IS A "NO". (#174) --------------------------------------
     * This used to test the master's mask and its bits 0:1 only, so a second slave
     * interrupt got in while IR2 was still in service -- Special Fully Nested Mode,
     * implemented without anyone asking for it. A PC BIOS programs SFNM OFF, so on
     * an AT the slave is locked out from the moment one of its lines is delivered
     * until the handler's master EOI (docs/ref/pic.md 5). ICW4 bit 4 now asks for
     * the permissive behaviour explicitly.
     * - WHY THE HOST CAN AFFORD IT: IR2 in service already blocked IRQ3-7, so any path
     *   that failed to release it was already killing five master lines; every host
     *   path that sets it releases it (VddPicEndOfInterrupt clears IR2 once the slave's ISR is
     *   empty -- our stubs, the PM reflection, a failed PM inject -- and a guest
     *   handler's own `out 20h` does the rest).
     */
    if (irq >= PIC_LINES_PER_CHIP && !PicIsLineOpen(&state->Master, PIC_CASCADE_LINE, PIC_CHIP_MASTER))
        return FALSE;

    return TRUE;
}

VOID VddPicAcknowledge(PPIC_STATE state, BYTE irq)
{
    PPIC_CHIP chip;
    BYTE bit;

    if (irq >= PIC_LINES)
        return;

    chip   = (irq < PIC_LINES_PER_CHIP) ? &state->Master : &state->Slave;
    bit = (BYTE)(1u << (irq & PIC_LINE_MASK));
    PIC_IRR_CLEAR(chip, bit);
    PicInterruptAcknowledge(chip, irq & PIC_LINE_MASK);

    if (irq >= PIC_LINES_PER_CHIP)
        PicInterruptAcknowledge(&state->Master, PIC_CASCADE_LINE);                                   /* the cascade: IR2 in service */
}

/* Acknowledge a line the HOST auto-EOIs -- IRQ0, and any line still vectored at one of
 * our own do-nothing stubs. Net effect is identical to acknowledge() followed by eoi():
 * the request bit is cleared and ISR ends exactly where it started.
 * - WHY IT IS A FUNCTION RATHER THAN THE PAIR. The pair SETS the in-service bit and
 *   then clears it, and that transient is a read-modify-write on a byte shared with
 *   every other line. A caller that does not hold the device lock -- the tick courier
 *   in the host, which by construction runs while the guest thread is frozen and no
 *   lock is held -- could interleave with a concurrent acknowledge() for a DIFFERENT
 *   line and lose ITS in-service bit. IRQ1's in-service bit is the keyboard
 *   re-entrancy guard, so losing it is the "press a key and everything hangs" fault.
 *   Doing the net operation directly touches only IRR, which no delivery decision
 *   reads (can_deliver consults IMR and ISR), so it is safe from any thread.
 */
VOID VddPicAcknowledgeAutoEoi(PPIC_STATE state, BYTE irq)
{
    if (irq >= PIC_LINES_PER_CHIP)
    {
        VddPicAcknowledge(state, irq);
        VddPicEndOfInterrupt(state, irq);
        return;
    }

    PIC_IRR_CLEAR(&state->Master, 1u << irq);
    /* The acknowledge half of a chip in rotate-in-AEOI mode moves the priority; the
     * host's specific EOI that follows does not. A single byte store, and never taken
     * unless a guest programmed AEOI + rotate.
     */
    if (state->Master.IsAutoEoi && state->Master.IsRotateInAutoEoi)
        state->Master.LowestPriority = irq;
}

VOID VddPicEndOfInterrupt(PPIC_STATE state, BYTE irq)
{
    if (irq >= PIC_LINES)
        return;

    if (irq < PIC_LINES_PER_CHIP)
        PIC_ISR_CLEAR(&state->Master, 1u << irq);
    else       { PIC_ISR_CLEAR(&state->Slave, 1u << (irq - PIC_LINES_PER_CHIP));

                 if (!state->Slave.Isr)
                     PIC_ISR_CLEAR(&state->Master, PIC_CASCADE_BIT); }   /* release the cascade */
}

BYTE VddPicVector(PPIC_STATE state, BYTE irq)
{
    if (irq < PIC_LINES_PER_CHIP)
        return (BYTE)(state->Master.VectorBase + irq);

    if (irq < PIC_LINES)
        return (BYTE)(state->Slave.VectorBase + (irq - PIC_LINES_PER_CHIP));

    return PIC_NO_VECTOR;
}

VOID VddPicReset(PVOID context)
{
    PPIC_STATE state = (PPIC_STATE)context;
    PVDD_BUS bus = state->Bus;

    PicChipReset(&state->Master, PIC_MASTER_VECTOR_BASE);
    PicChipReset(&state->Slave, PIC_SLAVE_VECTOR_BASE);
    /* A PC's BIOS leaves the timer and keyboard unmasked before handing control to
     * the program, and a DOS game inherits that; masking everything here would mean a
     * game that never touches the PIC (many do not) got no interrupts at all.
     */
    state->Master.Imr = PIC_BIOS_MASTER_MASK;            /* IRQ0 + IRQ1 enabled */
    state->Slave.Imr = PIC_ALL_MASKED;
    state->Bus = bus;
}

INT VddPicInitialize(PVDD_BUS bus, PVOID context)
{
    PPIC_STATE state = (PPIC_STATE)context;

    state->Bus = bus;
    VddPicReset(state);
    state->Bus = bus;

    if (VddClaimPorts(bus, PIC_MASTER_COMMAND, PIC_MASTER_DATA, PicPortIn, PicPortOut, state))
        return PIC_FAILED;

    if (VddClaimPorts(bus, PIC_SLAVE_COMMAND, PIC_SLAVE_DATA, PicPortIn, PicPortOut, state))
        return PIC_FAILED;

    return PIC_OK;
}
