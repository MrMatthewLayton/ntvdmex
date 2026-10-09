/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * Off-VM battery for the MC146818 RTC + CMOS VDD (vdd_cmos.c).
 *
 * The clock is INJECTED here, so every time value below is pinned rather than
 * read off the wall: the whole point of the device is that the chip and INT 1Ah
 * are two doors onto ONE clock, and a battery that read the real time could not
 * tell agreement from coincidence.
 *
 * [WARNING]: The gap this device closed was a HANG, not a wrong answer. Nothing claimed
 * 0x70/0x71, an unclaimed ISA port reads 0xFF on this host (deliberately --
 * see the V86 I/O trap), and bit 7 of Status Register A is UIP. The canonical
 * read sequence is "poll 0Ah until UIP is clear, then read the time", so 0xFF
 * meant that loop never exited. See docs/ref/rtc.md 2.
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#include <stdio.h>
#include <string.h>
#include "vdd_cmos.h"
#define CHECK(condition, message) do {                                  \
        g_Total++;                                               \
        if (condition) { printf("  PASS  %s\n", (message)); }           \
        else      { printf("  FAIL  %s\n", (message)); g_Failures++; }  \
    } while (0)

static INT g_Total = 0;
static INT g_Failures = 0;

/* GH #261: the host's side of a clock write, recorded rather than applied. */
static PIT_RTC_READING g_SetReadings[4];
static INT g_SetFields[4];
static INT g_SetCount;

static BYTE g_GuestMemory[0x1000];
static INT g_Irq8Count;

/* A fixed instant, so every expectation is exact: 2026-09-23, 14:07:42. */
static VOID CmosTestFakeRtc(PVOID context, PPIT_RTC_READING output)
{
    (VOID)context;
    output->Century = 20;
    output->Year = 26;
    output->Month = 9;
    output->Day = 23;
    output->Hour = 14;
    output->Minute = 7;
    output->Second = 42;
    output->DayOfWeek = 4;   /* 2026-09-23 was a Wednesday */
}

static INT CmosTestFakeSet(PVOID context, PCPIT_RTC_READING input, INT fields)
{
    (VOID)context;

    if (g_SetCount < 4)
    {
        g_SetReadings[g_SetCount] = *input;
        g_SetFields[g_SetCount] = fields;
    }

    g_SetCount++;
    return 1;
}

static VOID CmosTestIrqSink(PVOID context, BYTE irq)
{
    (VOID)context;

    if (irq == 8)
        g_Irq8Count++;
}

static BYTE CmosTestRead(PVDD_BUS bus, BYTE registerIndex)
{
    UINT32 value = registerIndex;

    VddBusIo(bus, 0x70, 1, 0, &value);
    value = 0;
    VddBusIo(bus, 0x71, 1, 1, &value);
    return (BYTE)value;
}

static VOID CmosTestWrite(PVDD_BUS bus, BYTE registerIndex, BYTE byteValue)
{
    UINT32 value = registerIndex;

    VddBusIo(bus, 0x70, 1, 0, &value);
    value = byteValue;
    VddBusIo(bus, 0x71, 1, 0, &value);
}

INT main(VOID)
{
    VDD_BUS bus;
    static CMOS_STATE cmos;
    NTVDD_DEVICE device;
    UINT32 value;

    memset(&cmos, 0, sizeof cmos);
    cmos.RtcNow = CmosTestFakeRtc;
    device = VddCmosDevice(&cmos);
    VddBusInitialize(&bus, g_GuestMemory);
    VddBusSetSinks(&bus, CmosTestIrqSink, 0, 0, 0);

    printf("-- MC146818 RTC + CMOS --\n");
    CHECK(VddBusAdd(&bus, &device) == 0, "add: cmos init ok");

    /* THE CLOCK, IN BCD AND 24-HOUR -- which is what Status B says a PC leaves,
     * and what every program reading this chip by hand assumes. 0x14 in the
     * hours register is two in the afternoon, not twenty.
     */
    CHECK(CmosTestRead(&bus, CMOS_SECONDS)   == 0x42, "clock: seconds are BCD");
    CHECK(CmosTestRead(&bus, CMOS_MINUTES)   == 0x07, "clock: minutes are BCD");
    CHECK(CmosTestRead(&bus, CMOS_HOURS)  == 0x14, "clock: 14:07 reads 0x14, not 0x0E");
    CHECK(CmosTestRead(&bus, CMOS_DAY_OF_MONTH)   == 0x23, "clock: day of month is BCD");
    CHECK(CmosTestRead(&bus, CMOS_MONTH) == 0x09, "clock: month is BCD");
    CHECK(CmosTestRead(&bus, CMOS_YEAR)  == 0x26, "clock: year is BCD");
    CHECK(CmosTestRead(&bus, CMOS_CENTURY) == 0x20, "clock: the century lives in CMOS 32h");
    /* #182: the weekday is the host's (1 = Sunday), and the extended-memory bytes
     * agree with INT 15h AH=88h's 15 MB.
     */
    CHECK(CmosTestRead(&bus, CMOS_DAY_OF_WEEK) == 0x04, "clock: day of week from the host (Wednesday = 4)");
    CHECK(CmosTestRead(&bus, 0x17) == 0x00 && CmosTestRead(&bus, 0x18) == 0x3C, "ext mem: 17h/18h = 0x3C00 KB");
    CHECK(CmosTestRead(&bus, 0x30) == 0x00 && CmosTestRead(&bus, 0x31) == 0x3C, "ext mem: 30h/31h = 0x3C00 KB");

    /* [WARNING]: THE BIT THE HANG WAS ABOUT. UIP must be CLEAR: with 0xFF coming back
     * from an unclaimed port it was set for ever, and the canonical
     * "poll until UIP clears" loop never exited.
     */
    CHECK((CmosTestRead(&bus, CMOS_STATUS_A) & 0x80) == 0,
          "status A: UIP is CLEAR -- the poll-then-read loop terminates");

    /* [INFO]: MEASURED 0x02 on 6.22-under-QEMU and on PCem's real AMI BIOS. */
    CHECK(CmosTestRead(&bus, CMOS_STATUS_B) == 0x02, "status B: BCD (DM=0) and 24-hour");
    CHECK((CmosTestRead(&bus, CMOS_STATUS_D) & 0x80) != 0,
          "status D: VRT set -- the CMOS is not claimed to be garbage");

    /* STATUS C IS CLEARED BY BEING READ. That is how IRQ8 is acknowledged at
     * the chip; a handler that does not read it gets one interrupt and silence.
     */
    cmos.StatusC = 0x90;                      /* IRQF + UF, as an update would set */
    CHECK(CmosTestRead(&bus, CMOS_STATUS_C) == 0x90, "status C: the latched flags read out");
    CHECK(CmosTestRead(&bus, CMOS_STATUS_C) == 0x00, "status C: ...and the READ cleared them");

    /* CMOS RAM IS RAM. Above 0x0D it takes what it is given. */
    CmosTestWrite(&bus, 0x40, 0xA5);
    CHECK(CmosTestRead(&bus, 0x40) == 0xA5, "ram: a byte above 0x0D is storage");

    /* ...AND THE CLOCK IS NOT WRITEABLE. We cannot move the host's clock, and
     * accepting the write while changing nothing is the "runs but lies" shape.
     * Same reasoning as INT 1Ah AH=03h/05h, deliberately not answered.
     */
    CmosTestWrite(&bus, CMOS_HOURS, 0x09);
    CHECK(CmosTestRead(&bus, CMOS_HOURS) == 0x14, "clock: a write to the hours register is refused");

    /* [CAUTION]: THIS CHECK USED TO READ "status B: read-only here" AND IT WAS WRONG THE
     * MOMENT THE PERIODIC INTERRUPT LANDED. Status B carries PIE, AIE, UIE,
     * the data mode and the 12/24 bit -- all of them the GUEST's to set -- and
     * refusing the write is exactly what made the periodic interrupt
     * unreachable. The clock registers stay refused because we cannot move the
     * host's clock; a control register is a different thing, and collapsing
     * the two into "everything below 0x0E is read-only" was the error.
     *
     * [WARNING]: It also LEAKED: writing 0xFF here left DM and the 12-hour bit set, and
     * the next check -- the NMI-mask one, twenty lines down -- then read the
     * hour in binary 12-hour format and failed for a reason that had nothing
     * to do with what it was testing. A test that leaves state behind
     * misattributes the next failure.
     */
    CmosTestWrite(&bus, CMOS_STATUS_B, 0x42);
    CHECK(CmosTestRead(&bus, CMOS_STATUS_B) == 0x42, "status B: PIE and the mode bits are writable");
    CmosTestWrite(&bus, CMOS_STATUS_B, 0x02);                 /* ...and put it back */
    CHECK(CmosTestRead(&bus, CMOS_STATUS_B) == 0x02, "status B: back to the BIOS default");

    /* [WARNING]: BIT 7 OF PORT 0x70 IS THE NMI MASK, NOT PART OF THE REGISTER NUMBER.
     * Software sets it constantly -- masking NMI across a CMOS access is
     * standard BIOS practice -- so a model that takes the whole byte as an index
     * looks up register 0x8A and finds nothing.
     */
    value = (UINT32)(0x80 | CMOS_HOURS);
    VddBusIo(&bus, 0x70, 1, 0, &value);
    value = 0;
    VddBusIo(&bus, 0x71, 1, 1, &value);
    CHECK(value == 0x14, "index: bit 7 is the NMI mask and does not change the register");
    CHECK(cmos.IsNmiDisabled == 1 && cmos.NmiMaskWrites == 1,
          "index: ...and the NMI mask is recorded rather than acted on");

    /* Port 0x70 is WRITE-ONLY on the part; a read is undefined. Answer
     * consistently rather than plausibly.
     */
    value = 0;
    VddBusIo(&bus, 0x70, 1, 1, &value);
    CHECK(value == 0xFF, "index: port 70h reads 0xFF -- write-only on the part");

    /* POST leaves a machine's CMOS populated; a guest that reads the equipment
     * byte on a machine DOS is running on does not get zero.
     *
     * [CAUTION]: Its low nibble is NOT adjudicable across hosts -- it describes the
     * MACHINE (video type, coprocessor) and p_rtc measured three different
     * answers on three oracles. What is pinned here is that it is populated.
     */
    CHECK(CmosTestRead(&bus, CMOS_EQUIPMENT) != 0x00, "post: the equipment byte is not zero");
    CHECK(CmosTestRead(&bus, 0x15) == 0x80 && CmosTestRead(&bus, 0x16) == 0x02,
          "post: base memory reads 640 KB");
    /* #136: Settings > Conventional Memory sets what is FITTED; it survives reset (it is
     * how the board is populated) and the checksum over 10h-2Dh covers it.
     */
    {   UINT index, sum = 0;
        cmos.BaseKb = 512;
        VddCmosReset(&cmos);
        CHECK(cmos.BaseKb == 512 && cmos.Ram[0x15] == 0x00 && cmos.Ram[0x16] == 0x02,
              "base_kb 512: 15h/16h = 0200h, and the field survives reset");

        for (index = 0x10; index <= 0x2D; ++index)
            sum += cmos.Ram[index];

        CHECK(cmos.Ram[0x2E] == (BYTE)(sum >> 8) && cmos.Ram[0x2F] == (BYTE)sum,
              "base_kb 512: checksum 2Eh/2Fh still matches 10h-2Dh");
        cmos.BaseKb = 0;
        VddCmosReset(&cmos);
        CHECK(cmos.Ram[0x15] == 0x80 && cmos.Ram[0x16] == 0x02, "base_kb 0: 640 KB, as before");
    }

    /* THE PERIODIC INTERRUPT. docs/ref/rtc.md 4:
     * IRQ8 at the rate in Status A bits 3:0 -- a fast, steady tick INDEPENDENT
     * of the 8254, which is why Windows and DOS extenders use it.
     *
     * [CAUTION]: DORMANT BY DEFAULT, AND THAT IS WHAT MAKES IT SAFE TO ADD: nothing is
     * raised unless the guest sets PIE *and* a non-zero rate, and even then
     * nothing reaches it until IRQ8 is unmasked on the PIC.
     */
    {
        g_Irq8Count = 0;
        VddCmosReset(&cmos);
        cmos.RtcNow = CmosTestFakeRtc;

        /* The rate table. 1 and 2 are special cases; from 3 up it is
         * 32768 >> (RS-1), so RS=6 is the 1024 Hz a PC BIOS leaves.
         */
        cmos.StatusA = 0x20;
        CHECK(VddCmosPeriodicHz(&cmos) == 0, "periodic: RS=0 is no rate");
        cmos.StatusA = 0x21;
        CHECK(VddCmosPeriodicHz(&cmos) == 256,  "periodic: RS=1 is 256 Hz");
        cmos.StatusA = 0x22;
        CHECK(VddCmosPeriodicHz(&cmos) == 128,  "periodic: RS=2 is 128 Hz");
        cmos.StatusA = 0x23;
        CHECK(VddCmosPeriodicHz(&cmos) == 8192, "periodic: RS=3 is 8192 Hz");
        cmos.StatusA = 0x26;
        CHECK(VddCmosPeriodicHz(&cmos) == 1024, "periodic: RS=6 is 1024 Hz");
        cmos.StatusA = 0x2F;
        CHECK(VddCmosPeriodicHz(&cmos) == 2,    "periodic: RS=15 is 2 Hz");

        /* [WARNING]: DORMANT UNTIL ASKED. A second of clocks with PIE clear must raise
         * nothing at all -- this is the check that says adding the device cannot
         * disturb a guest that never programs it.
         */
        cmos.StatusA = 0x26;
        VddCmosAddClocks(&cmos, PIT_INPUT_HZ);
        CHECK(g_Irq8Count == 0, "periodic: PIE clear -> not one interrupt in a whole second");
        CHECK(cmos.StatusC == 0, "periodic: ...and no flag set either");

        /* Now enable it: 1024 Hz for one second is 1024 interrupts. */
        CmosTestWrite(&bus, CMOS_STATUS_B, 0x42);          /* PIE | 24-hour */
        CHECK((CmosTestRead(&bus, CMOS_STATUS_B) & 0x40) != 0, "periodic: PIE is writable");
        g_Irq8Count = 0;
        VddCmosAddClocks(&cmos, PIT_INPUT_HZ);
        CHECK(g_Irq8Count == 1024, "periodic: 1024 Hz for one second is 1024 interrupts");
        CHECK(cmos.PeriodicRaised == 1024, "periodic: ...and the run can say so");

        /* The flags a handler sees, and the acknowledge that clears them. */
        CHECK((CmosTestRead(&bus, CMOS_STATUS_C) & 0xC0) == 0xC0, "periodic: PF and IRQF are set");
        CHECK(CmosTestRead(&bus, CMOS_STATUS_C) == 0x00, "periodic: ...and the read cleared them");

        /* Turning PIE off drops the part-accumulated tick, so turning it back on
         * starts from now instead of firing immediately off a stale remainder.
         */
        CmosTestWrite(&bus, CMOS_STATUS_B, 0x02);
        VddCmosAddClocks(&cmos, 1000);
        CHECK(cmos.PeriodicAccumulator == 0, "periodic: disabling PIE drops the partial tick");
        g_Irq8Count = 0;
        CmosTestWrite(&bus, CMOS_STATUS_B, 0x42);
        VddCmosAddClocks(&cmos, 1);
        CHECK(g_Irq8Count == 0, "periodic: re-enabling starts from now, not from a backlog");

        /* UIP is READ-ONLY: it is the chip telling software when it may read,
         * never software telling the chip anything.
         */
        CmosTestWrite(&bus, CMOS_STATUS_A, 0xFF);
        CHECK((CmosTestRead(&bus, CMOS_STATUS_A) & 0x80) == 0, "status A: UIP cannot be written");
        CHECK(VddCmosPeriodicHz(&cmos) == 2, "status A: ...but the rate select can");
    }

    /* DATA MODE AND 12-HOUR MODE ARE HONOURED, NOT IGNORED:
     * A PC BIOS leaves BCD/24-hour and nothing on the shelf changes it -- but a
     * model that ignores the bits hands a guest that DID set binary mode a BCD
     * byte, and tells a 12-hour guest that 14:00 is 2 AM.
     */
    {
        VddCmosReset(&cmos);
        cmos.RtcNow = CmosTestFakeRtc;                   /* 14:07:42 */
        CmosTestWrite(&bus, CMOS_STATUS_B, 0x06);           /* DM=1 (binary), 24-hour */
        CHECK(CmosTestRead(&bus, CMOS_SECONDS) == 42, "data mode: binary seconds are 42, not 0x42");
        CHECK(CmosTestRead(&bus, CMOS_HOURS) == 14, "data mode: binary hours are 14");

        CmosTestWrite(&bus, CMOS_STATUS_B, 0x00);           /* BCD, 12-hour */
        CHECK(CmosTestRead(&bus, CMOS_HOURS) == 0x82,
              "12-hour: 14:00 is 2 PM -- 0x02 with bit 7 set, not 0x02 alone");
        cmos.StatusB = 0x00;
        CHECK(CmosTestRead(&bus, CMOS_MINUTES) == 0x07, "12-hour: the other fields are unaffected");
    }

    /* THE ALARM AND UPDATE-ENDED INTERRUPTS. docs/ref/rtc.md 4:
     *
     * [WARNING]: THE ALARM REGISTERS WERE BEING REFUSED, and that was a defect I put in
     * myself: "everything below 0x0E is read-only" swept up 01h, 03h and 05h
     * -- the seconds/minutes/hours ALARM -- which is the only way to set an
     * alarm at all. The rule is not "low registers are read-only"; it is WE
     * CANNOT MOVE THE HOST'S CLOCK, which applies to 00/02/04 and the date,
     * not to a comparison value the guest owns. Exactly the same shape as
     * refusing Status B and making the periodic interrupt unreachable.
     */
    {
        VddCmosReset(&cmos);
        cmos.RtcNow = CmosTestFakeRtc;                     /* 14:07:42 */
        g_Irq8Count = 0;

        CmosTestWrite(&bus, 0x01, 0x42);
        CHECK(CmosTestRead(&bus, 0x01) == 0x42, "alarm: the seconds alarm register is writable");
        CmosTestWrite(&bus, 0x03, 0x07);
        CmosTestWrite(&bus, 0x05, 0x14);
        CHECK(CmosTestRead(&bus, 0x05) == 0x14, "alarm: ...and the hours alarm too");

        /* UPDATE ENDED: once a second, if UIE is set. */
        CmosTestWrite(&bus, CMOS_STATUS_B, 0x12);             /* UIE | 24-hour */
        VddCmosAddClocks(&cmos, PIT_INPUT_HZ);
        CHECK(cmos.UpdateEndedRaised == 1, "update: one second is one update-ended flag");
        CHECK(g_Irq8Count == 1, "update: ...and one IRQ8");
        CHECK((CmosTestRead(&bus, CMOS_STATUS_C) & 0x90) == 0x90, "update: UF and IRQF are set");

        /* THE ALARM FIRES WHEN THE CLOCK MATCHES -- 14:07:42, which is what the
         * three registers above were set to.
         */
        VddCmosReset(&cmos);
        cmos.RtcNow = CmosTestFakeRtc;
        g_Irq8Count = 0;
        CmosTestWrite(&bus, 0x01, 0x42);
        CmosTestWrite(&bus, 0x03, 0x07);
        CmosTestWrite(&bus, 0x05, 0x14);
        CmosTestWrite(&bus, CMOS_STATUS_B, 0x22);             /* AIE | 24-hour */
        VddCmosAddClocks(&cmos, PIT_INPUT_HZ);
        CHECK(cmos.AlarmRaised == 1, "alarm: it fires when the clock matches");
        CHECK((CmosTestRead(&bus, CMOS_STATUS_C) & 0x20) != 0, "alarm: AF is set");

        /* ...and NOT when it does not. */
        VddCmosReset(&cmos);
        cmos.RtcNow = CmosTestFakeRtc;
        g_Irq8Count = 0;
        CmosTestWrite(&bus, 0x01, 0x11);
        CmosTestWrite(&bus, 0x03, 0x22);
        CmosTestWrite(&bus, 0x05, 0x09);
        CmosTestWrite(&bus, CMOS_STATUS_B, 0x22);
        VddCmosAddClocks(&cmos, PIT_INPUT_HZ);
        CHECK(cmos.AlarmRaised == 0, "alarm: a different time does not fire it");
        CHECK(g_Irq8Count == 0, "alarm: ...and raises no interrupt");

        /* [CAUTION]: THE MATCH RULE IS NOT "EQUAL". An alarm byte with its top two bits
         * set is a DON'T CARE -- that is how "every minute at 42 seconds" is
         * programmed, and a model that only compares for equality cannot
         * express it at all.
         */
        VddCmosReset(&cmos);
        cmos.RtcNow = CmosTestFakeRtc;
        g_Irq8Count = 0;
        CmosTestWrite(&bus, 0x01, 0x42);
        CmosTestWrite(&bus, 0x03, 0xFF);
        CmosTestWrite(&bus, 0x05, 0xFF);
        CmosTestWrite(&bus, CMOS_STATUS_B, 0x22);
        VddCmosAddClocks(&cmos, PIT_INPUT_HZ);
        CHECK(cmos.AlarmRaised == 1, "alarm: don't-care fields (>= 0xC0) match anything");

        /* Both disabled: the second accumulator is dropped, so enabling later
         * starts from now rather than firing off a stale remainder.
         */
        VddCmosReset(&cmos);
        cmos.RtcNow = CmosTestFakeRtc;
        g_Irq8Count = 0;
        VddCmosAddClocks(&cmos, PIT_INPUT_HZ * 3);
        CHECK(g_Irq8Count == 0 && cmos.SecondAccumulator == 0,
              "update/alarm: dormant with UIE and AIE clear");
    }

    /* THE CHECKSUM OVER 10h-2Dh, which a BIOS verifies at boot. Leaving it zero
     * tells anything that checks that our CMOS is corrupt -- the same wrong
     * answer Status D's VRT bit used to give.
     */
    {
        UINT index;
        UINT sum = 0;
        VddCmosReset(&cmos);

        for (index = 0x10; index <= 0x2D; ++index)
            sum += CmosTestRead(&bus, (BYTE)index);

        CHECK(CmosTestRead(&bus, 0x2E) == ((sum >> 8) & 0xFF) && CmosTestRead(&bus, 0x2F) == (sum & 0xFF),
              "cmos: the checksum at 2Eh/2Fh covers 10h-2Dh");
    }

    /* GH #261: WRITING THE CLOCK, with the host hook present:
     * Without it (every check above) a clock write is refused. With it: a write
     * outside SET goes to the host at once; with SET held, reads are frozen,
     * writes collect, and releasing SET commits date then time in one step.
     */
    {
        VddCmosReset(&cmos);
        cmos.RtcNow = CmosTestFakeRtc;
        cmos.RtcSet = CmosTestFakeSet;
        g_SetCount = 0;
        CmosTestWrite(&bus, CMOS_HOURS, 0x09);
        CHECK(g_SetCount == 1 && g_SetFields[0] == 0 && g_SetReadings[0].Hour == 9
              && g_SetReadings[0].Minute == 7 && g_SetReadings[0].Second == 42,
              "clock write: hours 0x09 BCD -> host time 09:07:42, outside SET");
        CmosTestWrite(&bus, CMOS_DAY_OF_WEEK, 0x02);
        CHECK(g_SetCount == 1, "clock write: the day of week stays refused");

        g_SetCount = 0;
        CmosTestWrite(&bus, CMOS_STATUS_B, 0x92);          /* SET + UIE + 24h */
        CHECK(CmosTestRead(&bus, CMOS_STATUS_B) == 0x82, "SET: going high clears UIE (datasheet)");
        CmosTestWrite(&bus, CMOS_HOURS, 0x12);
        CmosTestWrite(&bus, CMOS_MINUTES, 0x34);
        CmosTestWrite(&bus, CMOS_SECONDS, 0x56);
        CHECK(g_SetCount == 0, "SET held: writes are not committed yet");
        CHECK(CmosTestRead(&bus, CMOS_HOURS) == 0x12 && CmosTestRead(&bus, CMOS_MINUTES) == 0x34
              && CmosTestRead(&bus, CMOS_SECONDS) == 0x56, "SET held: reads show the frozen, written copy");
        CmosTestWrite(&bus, CMOS_DAY_OF_MONTH, 0x15);
        CmosTestWrite(&bus, CMOS_MONTH, 0x06);
        CmosTestWrite(&bus, CMOS_YEAR, 0x99);
        CmosTestWrite(&bus, CMOS_CENTURY, 0x19);
        CmosTestWrite(&bus, CMOS_STATUS_B, 0x02);
        CHECK(g_SetCount == 2 && g_SetFields[0] == 1 && g_SetFields[1] == 0,
              "SET released: one date commit, then one time commit");
        CHECK(g_SetReadings[0].Century == 19 && g_SetReadings[0].Year == 99 && g_SetReadings[0].Month == 6
              && g_SetReadings[0].Day == 15 && g_SetReadings[1].Hour == 12 && g_SetReadings[1].Minute == 34
              && g_SetReadings[1].Second == 56, "SET released: 1999-06-15 12:34:56 committed");
        CHECK(CmosTestRead(&bus, CMOS_HOURS) == 0x14, "SET released: reads follow the host clock again");

        g_SetCount = 0;
        CmosTestWrite(&bus, CMOS_STATUS_B, 0x06);          /* binary, 24h */
        CmosTestWrite(&bus, CMOS_MINUTES, 45);
        CHECK(g_SetCount == 1 && g_SetReadings[0].Minute == 45, "DM binary: 45 is forty-five, not 0x45");
        g_SetCount = 0;
        CmosTestWrite(&bus, CMOS_STATUS_B, 0x00);          /* BCD, 12-hour */
        CmosTestWrite(&bus, CMOS_HOURS, 0x83);
        CHECK(g_SetCount == 1 && g_SetReadings[0].Hour == 15, "12-hour: 0x83 is 3 PM = 15:00");
        g_SetCount = 0;
        CmosTestWrite(&bus, CMOS_HOURS, 0x92);
        CHECK(g_SetCount == 1 && g_SetReadings[0].Hour == 12, "12-hour: 0x92 is 12 PM = noon");
        g_SetCount = 0;
        CmosTestWrite(&bus, CMOS_HOURS, 0x12);
        CHECK(g_SetCount == 1 && g_SetReadings[0].Hour == 0, "12-hour: 0x12 is 12 AM = midnight");
        VddCmosReset(&cmos);
        CHECK(cmos.RtcSet == CmosTestFakeSet && cmos.RtcNow == CmosTestFakeRtc,
              "reset keeps both host hooks");
        cmos.RtcSet = 0;
    }

    printf("\n%d checks, %d failed\n", g_Total, g_Failures);
    return g_Failures ? 1 : 0;
}
