/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * Off-VM battery for the 82077AA floppy controller (vdd_fdc.c).
 *
 * [WARNING]: The gap this device closed was a HANG, not a wrong answer. Nothing claimed
 * 3F0h-3F7h, an unclaimed ISA port reads 0xFF on this host (deliberately --
 * see the V86 I/O trap), and 3F4h is the MAIN STATUS REGISTER. 0xFF there is
 * RQM=1 with DIO=1: "ready, and I am the one talking". The datasheet's own
 * command-write loop -- `in al,3F4h / and al,0C0h / cmp al,80h / jne` -- then
 * never matches and never exits. See docs/ref/fdc.md 3.
 *
 * HOW THESE CHECKS WERE WRITTEN:
 * From the datasheet and from p_fdc.asm's two-oracle run, NOT from vdd_fdc.c.
 * `FdcTestDriverSend`/`FdcTestDriverDrain` below are deliberately the loops a REAL driver
 * runs, polling MSR between every byte, rather than calls into the model: a test
 * that reached into the struct could not have caught the hang, because the hang
 * is in the handshake and not in any value.
 *
 * [INFO]: PINNED BY TWO ORACLES that agree (6.22 under QEMU, and PCem with a real AMI
 * 486 BIOS): MSR at rest is 80h, VERSION answers 90h in exactly one result
 * byte, and DUMPREG answers in exactly ten.
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#include <stdio.h>
#include <string.h>
#include "vdd_fdc.h"

static INT g_Total = 0;
static INT g_Failures = 0;
#define CHECK(condition, message) do {                                  \
        g_Total++;                                               \
        if (condition) { printf("  PASS  %s\n", (message)); }           \
        else      { printf("  FAIL  %s\n", (message)); g_Failures++; }  \
    } while (0)

static BYTE g_GuestMemory[0x1000];
static INT g_Irq6Count;
static VOID FdcTestIrqSink(PVOID context, BYTE irq)
{
    (VOID)context;
    if (irq == 6)
        g_Irq6Count++;
}

static BYTE FdcTestRead(PVDD_BUS bus, WORD port)
{
    UINT32 value = 0;

    VddBusIo(bus, port, 1, 1, &value);
    return (BYTE)value;
}

static VOID FdcTestWrite(PVDD_BUS bus, WORD port, BYTE byteValue)
{
    UINT32 value = byteValue;

    VddBusIo(bus, port, 1, 0, &value);
}

/* THE DRIVER'S OWN SEND LOOP, BOUNDED. Returns 0 if it would have hung:
 *
 * [WARNING]: AND THE RETURN VALUE IS NOT OPTIONAL. The first version of this file
 * ignored it, and that quietly gutted a check: with 09h missing from the
 * command length table the chip executed on byte one, went to result phase,
 * and every one of the eight parameter sends that followed FAILED SILENTLY --
 * after which the drain found the 7 result bytes it was looking for and the
 * test passed. A send that would have hung is the single most important thing
 * this file can observe, so it is counted and asserted rather than returned
 * into nothing. (The project's own rule: a guard that returns success is a lie
 * the whole stack repeats.)
 */
static INT g_SendFailures;
static INT FdcTestDriverSend(PVDD_BUS bus, BYTE byte)
{
    INT spin;

    for (spin = 0; spin < 10000; ++spin)
        if ((FdcTestRead(bus, FDC_MSR) & 0xC0) == 0x80)
        {
            FdcTestWrite(bus, FDC_FIFO, byte);
            return 1;
        }
    g_SendFailures++;
    return 0;
}

/* -- THE LENGTH-FREE DRAIN: read while RQM=1 && DIO=1, stop when CMD BSY clears.
 * This is how a driver reads a result whose length it does not know, and it is
 * what p_fdc.asm does on the real machines. Returns the count.
 */
static INT FdcTestDriverDrain(PVDD_BUS bus, PBYTE output, INT maximum)
{
    INT count = 0;
    INT spin;

    for (spin = 0; spin < 10000 && count < maximum; ++spin)
    {
        BYTE status = FdcTestRead(bus, FDC_MSR);
        if (!(status & FDC_MSR_CB))
            break;
        if ((status & 0xC0) == 0xC0)
            output[count++] = FdcTestRead(bus, FDC_FIFO);
    }
    return count;
}

INT main(VOID)
{
    VDD_BUS bus;
    FDC_STATE fdc;
    NTVDD_DEVICE device;
    BYTE result[16];
    INT count;

    memset(&fdc, 0, sizeof(fdc));
    VddBusInitialize(&bus, g_GuestMemory);
    VddBusSetSinks(&bus, FdcTestIrqSink, NULL, NULL, NULL);
    device = VddFdcDevice(&fdc);
    CHECK(VddBusAdd(&bus, &device) == 0, "fdc: gets on the bus");

    /* 1. THE HANG:
     * The single load-bearing byte in the chip. 80h = RQM set, DIO clear (it
     * wants a command), CMD BSY clear, no drive seeking. Measured at 0080 on
     * BOTH oracles. FFh -- what an absent chip gave -- is the value that spins
     * the caller for ever.
     */
    CHECK(FdcTestRead(&bus, FDC_MSR) == 0x80, "msr: at rest reads 80h, not FFh");
    CHECK(FdcTestDriverSend(&bus, 0x10), "msr: the datasheet's command-write loop EXITS");

    /* 2. VERSION -- the detection command:
     * Just sent above. One result byte, 90h ("enhanced 82077AA"), and nothing
     * left behind. Both oracles: 0190.
     */
    count = FdcTestDriverDrain(&bus, result, 16);
    CHECK(count == 1 && result[0] == 0x90, "version: exactly one result byte, and it is 90h");
    CHECK(FdcTestRead(&bus, FDC_MSR) == 0x80, "version: the chip is idle again afterwards");

    /* 3. CMD BSY IS THE FRAME:
     * Set from the first command byte, cleared when the LAST result byte is
     * read. It is the only thing that lets a driver drain a result of unknown
     * length, so it is worth pinning at both edges. SENSE DRIVE STATUS takes
     * one parameter and gives one result, which exercises both.
     */
    FdcTestDriverSend(&bus, 0x04);
    CHECK((FdcTestRead(&bus, FDC_MSR) & (FDC_MSR_CB | FDC_MSR_DIO)) == FDC_MSR_CB,
          "phases: CMD BSY set mid-command, DIO still host-to-chip");
    FdcTestDriverSend(&bus, 0x00);
    CHECK((FdcTestRead(&bus, FDC_MSR) & (FDC_MSR_CB | FDC_MSR_DIO))
              == (FDC_MSR_CB | FDC_MSR_DIO),
          "phases: DIO flips once a result is waiting");
    count = FdcTestDriverDrain(&bus, result, 16);
    CHECK(count == 1 && (result[0] & 0x20) && (result[0] & 0x10),
          "sense drive: ST3 says READY and TRACK0 at cylinder 0");
    CHECK(!(FdcTestRead(&bus, FDC_MSR) & FDC_MSR_CB),
          "phases: CMD BSY clears on the LAST result byte, not before");

    /* 4. DUMPREG -- ten bytes, drained without a length table:
     * Both oracles answered a count of exactly 10 (0A01). A model that gives a
     * different number breaks every driver that uses the CMD BSY rule.
     */
    FdcTestDriverSend(&bus, 0x0E);
    count = FdcTestDriverDrain(&bus, result, 16);
    CHECK(count == 10, "dumpreg: exactly ten result bytes");

    /* 5. SPECIFY HAS NO RESULT PHASE, AND DUMPREG HANDS IT BACK:
     * A command with zero results must leave DIO low and CMD BSY clear the
     * moment its last parameter lands -- if it left DIO high, the next thing
     * the driver did would be to read a byte that does not exist.
     */
    FdcTestDriverSend(&bus, 0x03);
    FdcTestDriverSend(&bus, 0xDF);
    FdcTestDriverSend(&bus, 0x02);
    CHECK(FdcTestRead(&bus, FDC_MSR) == 0x80, "specify: no result phase, chip idle at once");
    FdcTestDriverSend(&bus, 0x0E);
    count = FdcTestDriverDrain(&bus, result, 16);
    CHECK(count == 10 && result[4] == 0xDF && result[5] == 0x02,
          "dumpreg: reports the SPECIFY bytes that actually arrived");

    /* 6. AN INVALID COMMAND IS ONE BYTE OF 80h -- AND STAYS IN FRAME:
     * The reply matters less than the framing: an unknown opcode that consumed
     * a guessed number of parameters would eat the NEXT real command. Checked
     * by issuing a good command immediately afterwards.
     */
    FdcTestDriverSend(&bus, 0x1F);
    count = FdcTestDriverDrain(&bus, result, 16);
    CHECK(count == 1 && result[0] == 0x80, "invalid: one byte, ST0 = 80h");
    FdcTestDriverSend(&bus, 0x10);
    count = FdcTestDriverDrain(&bus, result, 16);
    CHECK(count == 1 && result[0] == 0x90,
          "invalid: does NOT swallow the next command as a parameter");

    /* 7. SEEK, ITS INTERRUPT, AND THE COMMAND THAT COLLECTS IT:
     * SEEK has no result phase at all: the interrupt IS the report, and SENSE
     * INTERRUPT STATUS is the only way to learn it finished. ST0 bit 5 is SEEK
     * END; the second byte is the present cylinder.
     */
    g_Irq6Count = 0;
    FdcTestDriverSend(&bus, 0x0F);
    FdcTestDriverSend(&bus, 0x00);
    FdcTestDriverSend(&bus, 0x27);
    CHECK(g_Irq6Count == 1, "seek: raises IRQ6 exactly once");
    CHECK(FdcTestRead(&bus, FDC_MSR) == 0x80, "seek: no result phase");
    FdcTestDriverSend(&bus, 0x08);
    count = FdcTestDriverDrain(&bus, result, 16);
    CHECK(count == 2 && (result[0] & 0x20) && result[1] == 0x27,
          "sense int: SEEK END, and the present cylinder is where we sent it");
    FdcTestDriverSend(&bus, 0x08);
    count = FdcTestDriverDrain(&bus, result, 16);
    CHECK(count == 1 && result[0] == 0x80,
          "sense int: asked twice, the second says 80h -- nothing pending");

    /* 8. DMAGATE IS NOT DECORATION:
     * With DOR bit 3 clear the chip still works and the interrupt simply never
     * reaches the PIC. A model that ignores the bit silently disobeys a driver
     * that cleared it -- and there is no wrong VALUE anywhere to notice.
     */
    g_Irq6Count = 0;
    FdcTestWrite(&bus, FDC_DOR, (BYTE)(FDC_DOR_NRESET));      /* gate OFF, out of reset */
    FdcTestDriverSend(&bus, 0x07);
    FdcTestDriverSend(&bus, 0x00);  /* RECALIBRATE */
    CHECK(g_Irq6Count == 0, "dmagate: clear -> IRQ6 is not delivered");
    FdcTestWrite(&bus, FDC_DOR, (BYTE)(FDC_DOR_NRESET | FDC_DOR_DMA_GATE));
    g_Irq6Count = 0;
    FdcTestDriverSend(&bus, 0x07);
    FdcTestDriverSend(&bus, 0x00);
    CHECK(g_Irq6Count == 1, "dmagate: set -> IRQ6 is delivered");

    /* 9. DOR BIT 2 IS AN ACTIVE-LOW RESET, AND IT REALLY HOLDS:
     * A plausible "turn everything off" of 00h holds the chip down. MSR then
     * reads 00h -- which hangs a poller, on a real machine too. Faithful, and
     * worth a check precisely because it looks like a bug.
     */
    FdcTestWrite(&bus, FDC_DOR, 0x00);
    CHECK(FdcTestRead(&bus, FDC_MSR) == 0x00, "reset: held low -> MSR reads 00h");
    CHECK(FdcTestRead(&bus, FDC_DOR) == 0x00, "dor: reads back what was written");

    /* 10. AND RELEASING IT RUNS THE RESET SEQUENCE -- FOUR SENSES:
     * Reset leaves the chip expecting FOUR sense-interrupts, one per drive,
     * each answering C0h|drive ("ready changed"). Four, not one: a model that
     * answers a single sense hands a driver that issued four an 80h for three
     * of them. docs/ref/fdc.md 8.
     */
    g_Irq6Count = 0;
    FdcTestWrite(&bus, FDC_DOR, (BYTE)(FDC_DOR_NRESET | FDC_DOR_DMA_GATE));
    CHECK(FdcTestRead(&bus, FDC_MSR) == 0x80, "reset: released -> ready for a command");
    CHECK(g_Irq6Count == 1, "reset: raises one interrupt");
    {
        INT index;
        INT isOk = 1;
        for (index = 0; index < 4; ++index)
        {
            FdcTestDriverSend(&bus, 0x08);
            count = FdcTestDriverDrain(&bus, result, 16);
            if (count != 2 || result[0] != (BYTE)(0xC0 | index))
                isOk = 0;
        }
        CHECK(isOk, "reset: four sense-interrupts, C0h|drive, one per drive");
        FdcTestDriverSend(&bus, 0x08);
        count = FdcTestDriverDrain(&bus, result, 16);
        CHECK(count == 1 && result[0] == 0x80, "reset: the fifth sense says 80h");
    }

    /* ==== 11. DSR BIT 7 IS THE OTHER RESET DOOR, AND THE DATA RATE SURVIVES.
     * A driver is entitled to reset through DSR and not re-select the rate.
     */
    FdcTestWrite(&bus, FDC_DIR, 0x02);                  /* CCR: 250 kbps */
    FdcTestWrite(&bus, FDC_MSR, 0x81);                  /* DSR: reset, rate 01 */
    CHECK(FdcTestRead(&bus, FDC_MSR) == 0x80, "dsr: software reset leaves the chip ready");
    CHECK((fdc.Dsr & 0x03) == 0x01 && fdc.Ccr == 0x02,
          "dsr: the data rate survives the reset it asked for");

    /* 12. DIR BIT 7 IS DSKCHG AND WE ANSWER 0:
     * FFh there means "the disk has been changed" on every access for ever,
     * which makes the drive look permanently unreliable rather than absent.
     * Both oracles read 0. It is also the same claim INT 13h AH=15h already
     * makes through the other door: one medium, two doors, one answer.
     */
    CHECK((FdcTestRead(&bus, FDC_DIR) & 0x80) == 0x00, "dir: DSKCHG clear, not FFh");

    /* 13. LOCK KEEPS CONFIGURE ACROSS A SOFTWARE RESET:
     * That is the whole purpose of the command, and the only observable
     * difference between a locked and an unlocked part.
     */
    FdcTestDriverSend(&bus, 0x13);                          /* CONFIGURE */
    FdcTestDriverSend(&bus, 0x00);
    FdcTestDriverSend(&bus, 0x2F);
    FdcTestDriverSend(&bus, 0x05);
    FdcTestDriverSend(&bus, 0x94);
    FdcTestDriverDrain(&bus, result, 16);   /* LOCK (bit 7 set) */
    FdcTestWrite(&bus, FDC_MSR, 0x80);                          /* DSR software reset */
    FdcTestDriverSend(&bus, 0x0E);
    count = FdcTestDriverDrain(&bus, result, 16);
    CHECK(count == 10 && result[8] == 0x2F && result[9] == 0x05,
          "lock: CONFIGURE survives a software reset while locked");

    /* ==== 14. THE DATA COMMANDS ARE **PART**, AND FAIL THE WAY THE CHIP DOES.
     * READ DATA has no data path yet. It must NOT answer "invalid command" --
     * that would be a chip contradicting the 90h it just gave for VERSION. It
     * terminates the way a real controller terminates over an unformatted
     * track: ST0 interrupt code 01 (abnormal), ST1 bit 2 (no data), seven
     * bytes. [CAUTION] THIS CHECK ASSERTS A KNOWN GAP. It changes when the DMA path
     * lands, and it is here so that landing cannot be silent.
     */
    g_SendFailures = 0;
    FdcTestDriverSend(&bus, 0xE6);                          /* READ DATA, MT+MFM+SK */
    FdcTestDriverSend(&bus, 0x00);
    FdcTestDriverSend(&bus, 0x00);
    FdcTestDriverSend(&bus, 0x00);
    FdcTestDriverSend(&bus, 0x01);
    FdcTestDriverSend(&bus, 0x02);
    FdcTestDriverSend(&bus, 0x12);
    FdcTestDriverSend(&bus, 0x1B);
    FdcTestDriverSend(&bus, 0xFF);
    count = FdcTestDriverDrain(&bus, result, 16);
    CHECK(g_SendFailures == 0 && count == 7,
          "read data: all nine bytes accepted, seven result bytes back");
    CHECK((result[0] & 0xC0) == 0x40 && (result[1] & 0x04),
          "read data: abnormal termination + no data -- NOT 'invalid command'");

    /* 15. THE COMMAND LENGTH TABLE IS A FRAMING DECISION:
     *
     * [CAUTION]: 09h WRITE DELETED DATA sits in a gap between 08h and 0Ah and no
     * detection routine ever issues it -- I had left it out. Omitted, it
     * becomes an "invalid command" that consumes ONE byte, and its eight
     * parameters are then read as EIGHT MORE COMMANDS. Getting one entry
     * wrong does not produce one wrong answer; it desynchronises everything
     * after it. So the check is not "what did 09h reply" but "is the chip
     * still in frame afterwards".
     *
     * [CAUTION]: 11h (SCAN EQUAL) and 18h (a National part-ID command) are the opposite
     * mistake: uPD765/PC8477 commands the 82077AA does NOT have. A part that
     * answers 90h to VERSION and then accepts them describes a chip that
     * does not exist.
     */
    g_SendFailures = 0;
    FdcTestDriverSend(&bus, 0x09);                          /* WRITE DELETED DATA */
    FdcTestDriverSend(&bus, 0x00);
    FdcTestDriverSend(&bus, 0x00);
    FdcTestDriverSend(&bus, 0x00);
    FdcTestDriverSend(&bus, 0x01);
    FdcTestDriverSend(&bus, 0x02);
    FdcTestDriverSend(&bus, 0x12);
    FdcTestDriverSend(&bus, 0x1B);
    FdcTestDriverSend(&bus, 0xFF);
    count = FdcTestDriverDrain(&bus, result, 16);
    /* [INFO]: g_SendFailures IS THE WHOLE CHECK. If 09h is missing from the length table
     * the chip executes on byte one, flips to result phase, and the next eight
     * sends are refused -- while the drain still finds 7 bytes and `n == 7`
     * still passes. Only the refused sends can tell the two apart.
     */
    CHECK(g_SendFailures == 0 && count == 7,
          "09h: eight parameters accepted -- not executed on byte one");
    FdcTestDriverSend(&bus, 0x10);
    count = FdcTestDriverDrain(&bus, result, 16);
    CHECK(count == 1 && result[0] == 0x90, "09h: the chip is still in frame afterwards");

    FdcTestDriverSend(&bus, 0x11);                          /* SCAN EQUAL: not ours */
    count = FdcTestDriverDrain(&bus, result, 16);
    CHECK(count == 1 && result[0] == 0x80, "11h: a 765 command an 82077AA does not have");
    FdcTestDriverSend(&bus, 0x18);                          /* National part ID */
    count = FdcTestDriverDrain(&bus, result, 16);
    CHECK(count == 1 && result[0] == 0x80, "18h: not an Intel 82077AA command either");
    FdcTestDriverSend(&bus, 0x10);
    count = FdcTestDriverDrain(&bus, result, 16);
    CHECK(count == 1 && result[0] == 0x90, "invalid opcodes leave the chip in frame");

    printf("\n%d checks, %d failed\n", g_Total, g_Failures);
    return g_Failures ? 1 : 0;
}
