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
#define CHECK(condition, message) do {                                  \
        g_Total++;                                               \
        if (condition) { printf("  PASS  %s\n", (message)); }           \
        else      { printf("  FAIL  %s\n", (message)); g_Failures++; }  \
    } while (0)

/* #178: AN IN-MEMORY DISKETTE, lent to the chip the way the host lends its image.
 * Small on purpose -- 2 cylinders x 2 heads x 18 sectors -- and every byte names its
 * own sector, so a transfer that lands on the wrong one cannot pass.
 */
#define FDC_TEST_CYLINDERS  2
#define FDC_TEST_HEADS      2
#define FDC_TEST_SECTORS    18

static INT g_Total = 0;
static INT g_Failures = 0;

static BYTE g_GuestMemory[0x10000];
static INT g_Irq6Count;
static BYTE g_Image[FDC_TEST_CYLINDERS * FDC_TEST_HEADS * FDC_TEST_SECTORS][FDC_SECTOR_SIZE];
static INT g_IsMediumIn;

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

static INT FdcTestGeometry(PVOID context, BYTE drive, PFDC_GEOMETRY geometry)
{
    (VOID)context;

    if (!g_IsMediumIn || drive != 0)
        return 0;

    geometry->Cylinders = FDC_TEST_CYLINDERS;
    geometry->Heads = FDC_TEST_HEADS;
    geometry->SectorsPerTrack = FDC_TEST_SECTORS;
    return 1;
}

static INT FdcTestTransfer(
    PVOID context,
    BYTE drive,
    WORD cylinder,
    BYTE head,
    BYTE sector,
    PBYTE buffer,
    INT isWrite)
{
    UINT index = ((UINT)cylinder * FDC_TEST_HEADS + head) * FDC_TEST_SECTORS + (sector - 1u);

    (VOID)context;

    if (!g_IsMediumIn || drive != 0 || cylinder >= FDC_TEST_CYLINDERS || head >= FDC_TEST_HEADS
        || sector < 1 || sector > FDC_TEST_SECTORS)
        return 0;

    if (isWrite)
        memcpy(g_Image[index], buffer, FDC_SECTOR_SIZE);
    else
        memcpy(buffer, g_Image[index], FDC_SECTOR_SIZE);

    return 1;
}

/* The pattern a sector holds: its own C/H/R in every byte, offset by the position. */
static BYTE FdcTestPattern(UINT cylinder, UINT head, UINT sector, UINT position)
{
    return (BYTE)((cylinder * 0x40) ^ (head * 0x20) ^ sector ^ position);
}

static VOID FdcTestFillImage(VOID)
{
    UINT cylinder;
    UINT head;
    UINT sector;
    UINT position;

    for (cylinder = 0; cylinder < FDC_TEST_CYLINDERS; ++cylinder)
        for (head = 0; head < FDC_TEST_HEADS; ++head)
            for (sector = 1; sector <= FDC_TEST_SECTORS; ++sector)
                for (position = 0; position < FDC_SECTOR_SIZE; ++position)
                    g_Image[(cylinder * FDC_TEST_HEADS + head) * FDC_TEST_SECTORS + sector - 1][position]
                        = FdcTestPattern(cylinder, head, sector, position);
}

static INT FdcTestSectorIs(PCBYTE bytes, UINT cylinder, UINT head, UINT sector)
{
    UINT position;

    for (position = 0; position < FDC_SECTOR_SIZE; ++position)
        if (bytes[position] != FdcTestPattern(cylinder, head, sector, position))
            return 0;

    return 1;
}

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

/* THE LENGTH-FREE DRAIN: read while RQM=1 && DIO=1, stop when CMD BSY clears.
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

/* Program the 8237's channel 2 the way the BIOS does: mask, mode, flip-flop, address,
 * page, count (bytes - 1), unmask. `mode` 46h = write to memory (an FDC READ), 4Ah =
 * read from memory (WRITE, FORMAT).
 */
static VOID FdcTestDmaSetup(PVDD_BUS bus, BYTE mode, UINT32 physical, UINT bytes)
{
    FdcTestWrite(bus, 0x0A, 0x06);                          /* mask channel 2 */
    FdcTestWrite(bus, 0x0B, mode);
    FdcTestWrite(bus, 0x0C, 0x00);                          /* clear the flip-flop */
    FdcTestWrite(bus, 0x04, (BYTE)physical);
    FdcTestWrite(bus, 0x04, (BYTE)(physical >> 8));
    FdcTestWrite(bus, 0x81, (BYTE)(physical >> 16));
    FdcTestWrite(bus, 0x05, (BYTE)(bytes - 1));
    FdcTestWrite(bus, 0x05, (BYTE)((bytes - 1) >> 8));
    FdcTestWrite(bus, 0x0A, 0x02);                          /* unmask channel 2 */
}

/* A data command's nine bytes, through the driver's own send loop. */
static VOID FdcTestDataCommand(
    PVDD_BUS bus,
    BYTE opcode,
    BYTE headDrive,
    BYTE cylinder,
    BYTE headAddress,
    BYTE sector,
    BYTE endOfTrack)
{
    FdcTestDriverSend(bus, opcode);
    FdcTestDriverSend(bus, headDrive);
    FdcTestDriverSend(bus, cylinder);
    FdcTestDriverSend(bus, headAddress);
    FdcTestDriverSend(bus, sector);
    FdcTestDriverSend(bus, FDC_SIZE_CODE_512);
    FdcTestDriverSend(bus, endOfTrack);
    FdcTestDriverSend(bus, 0x1B);                           /* GPL */
    FdcTestDriverSend(bus, 0xFF);                           /* DTL */
}

static VOID FdcTestSeek(PVDD_BUS bus, BYTE headDrive, BYTE cylinder)
{
    BYTE result[16];

    FdcTestDriverSend(bus, 0x0F);
    FdcTestDriverSend(bus, headDrive);
    FdcTestDriverSend(bus, cylinder);
    FdcTestDriverSend(bus, 0x08);                           /* sense: collect the seek */
    FdcTestDriverDrain(bus, result, 16);
}

static VOID FdcTestSpecify(PVDD_BUS bus, INT isNonDma)
{
    FdcTestDriverSend(bus, 0x03);
    FdcTestDriverSend(bus, 0xDF);
    FdcTestDriverSend(bus, (BYTE)(0x02 | (isNonDma ? 1 : 0)));
}

/* #178: THE DATA PATH, end to end -- the chip, the 8237's channel 2 and guest memory. */
static VOID FdcTestDataPath(PVDD_BUS bus, PFDC_STATE fdc)
{
    DMA_STATE dma;
    NTVDD_DEVICE dmaDevice;
    BYTE result[16];
    INT count;
    INT irqsBefore;
    INT index;

    printf("== #178: the data commands ==\n");
    memset(&dma, 0, sizeof dma);
    dmaDevice = VddDmaDevice(&dma);
    CHECK(VddBusAdd(bus, &dmaDevice) == 0, "data: the 8237 is on the bus too");
    fdc->Dma = &dma;
    fdc->MediaGeometry = FdcTestGeometry;
    fdc->MediaTransfer = FdcTestTransfer;
    fdc->MediaContext = NULL;
    g_IsMediumIn = 1;
    FdcTestFillImage();
    FdcTestWrite(bus, FDC_DOR, 0x1C);                       /* motor 0 on, DMAGATE, out of reset */
    FdcTestSpecify(bus, FALSE);
    FdcTestSeek(bus, 0x00, 1);

    /* READ DATA, MT, from C1 H0 R17: the DMA count says two sectors, so it reads R17,
     * R18, stops at TC -- and with MT the next sector is side 1, sector 1.
     */
    memset(g_GuestMemory + 0x2000, 0, 2 * FDC_SECTOR_SIZE);
    FdcTestDmaSetup(bus, 0x46, 0x2000, 2 * FDC_SECTOR_SIZE);
    irqsBefore = g_Irq6Count;
    g_SendFailures = 0;
    FdcTestDataCommand(bus, 0xC6, 0x00, 1, 0, 17, 18);
    count = FdcTestDriverDrain(bus, result, 16);
    CHECK(g_SendFailures == 0 && count == 7, "read: nine bytes in, seven result bytes out");
    CHECK(result[0] == 0x04 && result[1] == 0x00 && result[2] == 0x00, "read: normal termination at TC (ST0 says side 1, where it ended)");
    CHECK(result[3] == 1 && result[4] == 1 && result[5] == 1 && result[6] == 2,
          "read: C H R N = 1 1 1 2 -- with MT, the sector after side 0's last is side 1's first");
    CHECK(FdcTestSectorIs(g_GuestMemory + 0x2000, 1, 0, 17) && FdcTestSectorIs(g_GuestMemory + 0x2200, 1, 0, 18),
          "read: C1 H0 R17 and R18 are in guest memory, in order, over channel 2");
    CHECK(g_Irq6Count == irqsBefore + 1, "read: IRQ6 once, at the end");
    CHECK(fdc->SectorsRead == 2, "read: two sectors counted");

    /* ...across the side: R18 of side 0 and R1 of side 1 in one MT read. */
    FdcTestDmaSetup(bus, 0x46, 0x3000, 2 * FDC_SECTOR_SIZE);
    FdcTestDataCommand(bus, 0xC6, 0x00, 1, 0, 18, 18);
    count = FdcTestDriverDrain(bus, result, 16);
    CHECK(count == 7 && result[0] == 0x04 && result[3] == 1 && result[4] == 1 && result[5] == 2,
          "read MT: ends on side 1 (ST0 head bit), C1 H1 R2");
    CHECK(FdcTestSectorIs(g_GuestMemory + 0x3000, 1, 0, 18) && FdcTestSectorIs(g_GuestMemory + 0x3200, 1, 1, 1),
          "read MT: side 0's last sector, then side 1's first");

    /* WRITE DATA: one sector out of guest memory, onto C1 H0 R5. */
    for (index = 0; index < FDC_SECTOR_SIZE; ++index)
        g_GuestMemory[0x4000 + index] = (BYTE)(0xA5 ^ index);

    FdcTestDmaSetup(bus, 0x4A, 0x4000, FDC_SECTOR_SIZE);
    FdcTestDataCommand(bus, 0x45, 0x00, 1, 0, 5, 18);
    count = FdcTestDriverDrain(bus, result, 16);
    CHECK(count == 7 && result[0] == 0x00 && result[5] == 6, "write: normal at TC, and R moves on to 6");
    CHECK(memcmp(g_Image[(1 * 2 + 0) * 18 + 4], g_GuestMemory + 0x4000, FDC_SECTOR_SIZE) == 0,
          "write: the image now holds what guest memory held");
    CHECK(FdcTestSectorIs(g_Image[(1 * 2 + 0) * 18 + 5], 1, 0, 6), "write: the next sector is untouched");

    /* EOT REACHED WITHOUT TC: the DMA count is four sectors, the track ends after one. */
    FdcTestDmaSetup(bus, 0x46, 0x5000, 4 * FDC_SECTOR_SIZE);
    FdcTestDataCommand(bus, 0x46, 0x00, 1, 0, 18, 18);              /* no MT */
    count = FdcTestDriverDrain(bus, result, 16);
    CHECK(count == 7 && (result[0] & 0xC0) == 0x40 && result[1] == 0x80,
          "read past EOT with no TC: abnormal, ST1 end of cylinder (the datasheet's EN)");
    CHECK(result[3] == 2 && result[5] == 1, "...and C H R moves on to C+1, R1");

    /* THE ID SEARCH: the head is on cylinder 1, the command names 0. */
    FdcTestDmaSetup(bus, 0x46, 0x5000, FDC_SECTOR_SIZE);
    FdcTestDataCommand(bus, 0x46, 0x00, 0, 0, 1, 18);
    count = FdcTestDriverDrain(bus, result, 16);
    CHECK(count == 7 && (result[0] & 0xC0) == 0x40 && result[1] == 0x04 && result[2] == 0x10,
          "wrong cylinder: abnormal, no data, ST2 wrong cylinder -- nothing read from elsewhere");

    FdcTestDataCommand(bus, 0x46, 0x00, 1, 0, 19, 19);              /* no sector 19 */
    count = FdcTestDriverDrain(bus, result, 16);
    CHECK(count == 7 && (result[0] & 0xC0) == 0x40 && result[1] == 0x04, "a sector past the track's last: no data");

    /* CHANNEL 2 MASKED: the request is never answered, so the transfer overruns. */
    FdcTestWrite(bus, 0x0A, 0x06);
    FdcTestDataCommand(bus, 0x46, 0x00, 1, 0, 1, 18);
    count = FdcTestDriverDrain(bus, result, 16);
    CHECK(count == 7 && (result[0] & 0xC0) == 0x40 && result[1] == 0x10, "dma masked: abnormal, overrun");

    /* NO MEDIUM: nothing on the track at all. */
    g_IsMediumIn = 0;
    FdcTestDmaSetup(bus, 0x46, 0x5000, FDC_SECTOR_SIZE);
    FdcTestDataCommand(bus, 0x46, 0x00, 1, 0, 1, 18);
    count = FdcTestDriverDrain(bus, result, 16);
    CHECK(count == 7 && (result[0] & 0xC0) == 0x40 && result[1] == 0x01, "no medium: abnormal, missing address mark");
    g_IsMediumIn = 1;

    /* FORMAT TRACK over DMA: two IDs, fill E5h. */
    {   BYTE identities[] = { 1, 0, 3, 2,   1, 0, 4, 2 };
        memcpy(g_GuestMemory + 0x6000, identities, sizeof identities);
        FdcTestDmaSetup(bus, 0x4A, 0x6000, sizeof identities);
        FdcTestDriverSend(bus, 0x4D);                           /* FORMAT, MFM */
        FdcTestDriverSend(bus, 0x00);
        FdcTestDriverSend(bus, FDC_SIZE_CODE_512);
        FdcTestDriverSend(bus, 2);                              /* SC */
        FdcTestDriverSend(bus, 0x54);                           /* GPL */
        FdcTestDriverSend(bus, 0xE5);                           /* D */
        count = FdcTestDriverDrain(bus, result, 16);
        CHECK(count == 7 && result[0] == 0x00 && fdc->TracksFormatted == 1, "format: normal termination, one track");
        CHECK(g_Image[(1 * 2) * 18 + 2][0] == 0xE5 && g_Image[(1 * 2) * 18 + 3][511] == 0xE5
              && FdcTestSectorIs(g_Image[(1 * 2) * 18 + 6], 1, 0, 7),
              "format: R3 and R4 filled with E5h, R7 untouched"); }

    /* NON-DMA: SPECIFY's ND bit. The data comes through 3F5h, MSR bit 5 up. */
    FdcTestSpecify(bus, TRUE);
    FdcTestDataCommand(bus, 0x46, 0x04, 1, 1, 7, 7);
    CHECK((FdcTestRead(bus, FDC_MSR) & 0xF0) == 0xF0, "non-dma read: MSR = RQM DIO NDMA CB");
    {   BYTE sector[FDC_SECTOR_SIZE];

        for (index = 0; index < FDC_SECTOR_SIZE; ++index)
            sector[index] = FdcTestRead(bus, FDC_FIFO);

        CHECK(FdcTestSectorIs(sector, 1, 1, 7), "non-dma read: C1 H1 R7 through 3F5h, byte by byte"); }
    count = FdcTestDriverDrain(bus, result, 16);
    CHECK(count == 7 && (result[0] & 0xC0) == 0x40 && result[1] == 0x80,
          "non-dma read: no TC without the 8237, so it ends at EOT with EN");

    FdcTestDataCommand(bus, 0x45, 0x04, 1, 1, 9, 9);
    CHECK((FdcTestRead(bus, FDC_MSR) & 0xF0) == 0xB0, "non-dma write: MSR = RQM NDMA CB, DIO clear");

    for (index = 0; index < FDC_SECTOR_SIZE; ++index)
        FdcTestWrite(bus, FDC_FIFO, (BYTE)(0x3C ^ index));

    count = FdcTestDriverDrain(bus, result, 16);
    CHECK(count == 7 && g_Image[(1 * 2 + 1) * 18 + 8][0] == 0x3C && g_Image[(1 * 2 + 1) * 18 + 8][1] == 0x3D,
          "non-dma write: 3F5h's 512 bytes land on C1 H1 R9");
    FdcTestSpecify(bus, FALSE);

    /* INT 13h MOVES THE SAME HEAD: what the BIOS side sets, DUMPREG reports. */
    VddFdcSetCylinder(fdc, 0, 0x4F);
    FdcTestDriverSend(bus, 0x0E);
    count = FdcTestDriverDrain(bus, result, 16);
    CHECK(count == 10 && result[0] == 0x4F, "VddFdcSetCylinder: DUMPREG's PCN 0 follows INT 13h");

    /* THE BIOS's MOTOR COUNTDOWN (as a PIT BiosTickHook). */
    {   BYTE bda[0x100];
        memset(bda, 0, sizeof bda);
        bda[0x3F] = 0x81;
        bda[0x40] = 2;
        VddFdcBiosMotorTick(fdc, bda);
        CHECK(bda[0x40] == 1 && (bda[0x3F] & 0x0F) == 0x01, "motor: one tick down, still on");
        VddFdcBiosMotorTick(fdc, bda);
        CHECK(bda[0x40] == 0 && (bda[0x3F] & 0x0F) == 0 && FdcTestRead(bus, FDC_DOR) == 0x0C,
              "motor: at zero the motor bits clear and DOR goes back to 0Ch");
        VddFdcBiosMotorTick(fdc, bda);
        CHECK(bda[0x40] == 0 && FdcTestRead(bus, FDC_DOR) == 0x0C, "motor: a count of zero stays zero"); }

    fdc->Dma = NULL;
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

    /* ==== 14. WITH NO MEDIUM, A DATA COMMAND FAILS THE WAY THE CHIP DOES.
     * It must NOT answer "invalid command" -- that would be a chip contradicting
     * the 90h it just gave for VERSION. Nothing is lent to this chip yet (no
     * MediaGeometry), so there is no track to find: ST0 interrupt code 01
     * (abnormal), ST1 bit 0 (missing address mark), seven bytes. (Until #178 this
     * case asserted the data-path GAP -- ST1 "no data" -- so that landing it could
     * not be silent; it did not.)
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
    CHECK((result[0] & 0xC0) == 0x40 && (result[1] & 0x01),
          "read data, no medium: abnormal termination + missing address mark -- NOT 'invalid command'");

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

    FdcTestDataPath(&bus, &fdc);

    printf("\n%d checks, %d failed\n", g_Total, g_Failures);
    return g_Failures ? 1 : 0;
}
