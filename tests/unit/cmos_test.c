/* cmos_test.c -- off-VM battery for the MC146818 RTC + CMOS VDD (vdd_cmos.c).
 *
 * The clock is INJECTED here, so every time value below is pinned rather than
 * read off the wall: the whole point of the device is that the chip and INT 1Ah
 * are two doors onto ONE clock, and a battery that read the real time could not
 * tell agreement from coincidence.
 *
 * ⛔ The gap this device closed was a HANG, not a wrong answer. Nothing claimed
 *   0x70/0x71, an unclaimed ISA port reads 0xFF on this host (deliberately --
 *   see the V86 I/O trap), and bit 7 of Status Register A is UIP. The canonical
 *   read sequence is "poll 0Ah until UIP is clear, then read the time", so 0xFF
 *   meant that loop never exited. See docs/ref/rtc.md 2.
 */
#include <stdio.h>
#include <string.h>
#include "vdd_cmos.h"

static int total = 0, fails = 0;
#define CHECK(cond, msg) do {                                  \
        total++;                                               \
        if (cond) { printf("  PASS  %s\n", (msg)); }           \
        else      { printf("  FAIL  %s\n", (msg)); fails++; }  \
    } while (0)

/* A fixed instant, so every expectation is exact: 2026-09-23, 14:07:42. */
static void fake_rtc(void *ctx, struct vdd_rtc *out)
{
    (void)ctx;
    out->cent = 20; out->year = 26; out->month = 9; out->day = 23;
    out->hour = 14; out->min = 7;   out->sec = 42; out->dow = 4;   /* 2026-09-23 was a Wednesday */
}

/* GH #261: the host's side of a clock write, recorded rather than applied. */
static struct vdd_rtc g_set[4];
static int g_set_what[4], g_nset;
static int fake_set(void *ctx, const struct vdd_rtc *in, int what)
{
    (void)ctx;
    if (g_nset < 4) { g_set[g_nset] = *in; g_set_what[g_nset] = what; }
    g_nset++;
    return 1;
}

static uint8_t g_flat[0x1000];
static int g_irq8;
static void irq_sink(void *ctx, uint8_t irq) { (void)ctx; if (irq == 8) g_irq8++; }

static uint8_t rd(vdd_bus *bus, uint8_t reg)
{
    uint32_t v = reg; vdd_bus_io(bus, 0x70, 1, 0, &v);
    v = 0;            vdd_bus_io(bus, 0x71, 1, 1, &v);
    return (uint8_t)v;
}
static void wr(vdd_bus *bus, uint8_t reg, uint8_t val)
{
    uint32_t v = reg; vdd_bus_io(bus, 0x70, 1, 0, &v);
    v = val;          vdd_bus_io(bus, 0x71, 1, 0, &v);
}

int main(void)
{
    vdd_bus bus;
    static cmos_state cm;
    ntvdd dev;
    uint32_t v;

    memset(&cm, 0, sizeof cm);
    cm.rtc_now = fake_rtc;
    dev = vdd_cmos_device(&cm);
    vdd_bus_init(&bus, g_flat);
    vdd_bus_set_sinks(&bus, irq_sink, 0, 0, 0);

    printf("-- MC146818 RTC + CMOS --\n");
    CHECK(vdd_bus_add(&bus, &dev) == 0, "add: cmos init ok");

    /* THE CLOCK, IN BCD AND 24-HOUR -- which is what Status B says a PC leaves,
       and what every program reading this chip by hand assumes. 0x14 in the
       hours register is two in the afternoon, not twenty. */
    CHECK(rd(&bus, CMOS_SEC)   == 0x42, "clock: seconds are BCD");
    CHECK(rd(&bus, CMOS_MIN)   == 0x07, "clock: minutes are BCD");
    CHECK(rd(&bus, CMOS_HOUR)  == 0x14, "clock: 14:07 reads 0x14, not 0x0E");
    CHECK(rd(&bus, CMOS_DOM)   == 0x23, "clock: day of month is BCD");
    CHECK(rd(&bus, CMOS_MONTH) == 0x09, "clock: month is BCD");
    CHECK(rd(&bus, CMOS_YEAR)  == 0x26, "clock: year is BCD");
    CHECK(rd(&bus, CMOS_CENTURY) == 0x20, "clock: the century lives in CMOS 32h");
    /* #182: the weekday is the host's (1 = Sunday), and the extended-memory bytes
       agree with INT 15h AH=88h's 15 MB. */
    CHECK(rd(&bus, CMOS_DOW) == 0x04, "clock: day of week from the host (Wednesday = 4)");
    CHECK(rd(&bus, 0x17) == 0x00 && rd(&bus, 0x18) == 0x3C, "ext mem: 17h/18h = 0x3C00 KB");
    CHECK(rd(&bus, 0x30) == 0x00 && rd(&bus, 0x31) == 0x3C, "ext mem: 30h/31h = 0x3C00 KB");

    /* ⛔ THE BIT THE HANG WAS ABOUT. UIP must be CLEAR: with 0xFF coming back
       from an unclaimed port it was set for ever, and the canonical
       "poll until UIP clears" loop never exited. */
    CHECK((rd(&bus, CMOS_STATUS_A) & 0x80) == 0,
          "status A: UIP is CLEAR -- the poll-then-read loop terminates");

    /* ★ MEASURED 0x02 on 6.22-under-QEMU and on PCem's real AMI BIOS. */
    CHECK(rd(&bus, CMOS_STATUS_B) == 0x02, "status B: BCD (DM=0) and 24-hour");
    CHECK((rd(&bus, CMOS_STATUS_D) & 0x80) != 0,
          "status D: VRT set -- the CMOS is not claimed to be garbage");

    /* STATUS C IS CLEARED BY BEING READ. That is how IRQ8 is acknowledged at
       the chip; a handler that does not read it gets one interrupt and silence. */
    cm.status_c = 0x90;                      /* IRQF + UF, as an update would set */
    CHECK(rd(&bus, CMOS_STATUS_C) == 0x90, "status C: the latched flags read out");
    CHECK(rd(&bus, CMOS_STATUS_C) == 0x00, "status C: ...and the READ cleared them");

    /* CMOS RAM IS RAM. Above 0x0D it takes what it is given. */
    wr(&bus, 0x40, 0xA5);
    CHECK(rd(&bus, 0x40) == 0xA5, "ram: a byte above 0x0D is storage");

    /* ...AND THE CLOCK IS NOT WRITEABLE. We cannot move the host's clock, and
       accepting the write while changing nothing is the "runs but lies" shape.
       Same reasoning as INT 1Ah AH=03h/05h, deliberately not answered. */
    wr(&bus, CMOS_HOUR, 0x09);
    CHECK(rd(&bus, CMOS_HOUR) == 0x14, "clock: a write to the hours register is refused");

    /* ⚠ THIS CHECK USED TO READ "status B: read-only here" AND IT WAS WRONG THE
         MOMENT THE PERIODIC INTERRUPT LANDED. Status B carries PIE, AIE, UIE,
         the data mode and the 12/24 bit -- all of them the GUEST's to set -- and
         refusing the write is exactly what made the periodic interrupt
         unreachable. The clock registers stay refused because we cannot move the
         host's clock; a control register is a different thing, and collapsing
         the two into "everything below 0x0E is read-only" was the error.
       ⛔ It also LEAKED: writing 0xFF here left DM and the 12-hour bit set, and
         the next check -- the NMI-mask one, twenty lines down -- then read the
         hour in binary 12-hour format and failed for a reason that had nothing
         to do with what it was testing. A test that leaves state behind
         misattributes the next failure. */
    wr(&bus, CMOS_STATUS_B, 0x42);
    CHECK(rd(&bus, CMOS_STATUS_B) == 0x42, "status B: PIE and the mode bits are writable");
    wr(&bus, CMOS_STATUS_B, 0x02);                 /* ...and put it back */
    CHECK(rd(&bus, CMOS_STATUS_B) == 0x02, "status B: back to the BIOS default");

    /* ⛔ BIT 7 OF PORT 0x70 IS THE NMI MASK, NOT PART OF THE REGISTER NUMBER.
       Software sets it constantly -- masking NMI across a CMOS access is
       standard BIOS practice -- so a model that takes the whole byte as an index
       looks up register 0x8A and finds nothing. */
    v = (uint32_t)(0x80 | CMOS_HOUR); vdd_bus_io(&bus, 0x70, 1, 0, &v);
    v = 0;                            vdd_bus_io(&bus, 0x71, 1, 1, &v);
    CHECK(v == 0x14, "index: bit 7 is the NMI mask and does not change the register");
    CHECK(cm.nmi_disabled == 1 && cm.nmi_mask_writes == 1,
          "index: ...and the NMI mask is recorded rather than acted on");

    /* Port 0x70 is WRITE-ONLY on the part; a read is undefined. Answer
       consistently rather than plausibly. */
    v = 0; vdd_bus_io(&bus, 0x70, 1, 1, &v);
    CHECK(v == 0xFF, "index: port 70h reads 0xFF -- write-only on the part");

    /* POST leaves a machine's CMOS populated; a guest that reads the equipment
       byte on a machine DOS is running on does not get zero.
       ⚠ Its low nibble is NOT adjudicable across hosts -- it describes the
         MACHINE (video type, coprocessor) and p_rtc measured three different
         answers on three oracles. What is pinned here is that it is populated. */
    CHECK(rd(&bus, CMOS_EQUIP) != 0x00, "post: the equipment byte is not zero");
    CHECK(rd(&bus, 0x15) == 0x80 && rd(&bus, 0x16) == 0x02,
          "post: base memory reads 640 KB");
    /* #136: Settings > Conventional Memory sets what is FITTED; it survives reset (it is
       how the board is populated) and the checksum over 10h-2Dh covers it. */
    {   unsigned i, sum = 0;
        cm.base_kb = 512;
        vdd_cmos_reset(&cm);
        CHECK(cm.base_kb == 512 && cm.ram[0x15] == 0x00 && cm.ram[0x16] == 0x02,
              "base_kb 512: 15h/16h = 0200h, and the field survives reset");
        for (i = 0x10; i <= 0x2D; ++i) sum += cm.ram[i];
        CHECK(cm.ram[0x2E] == (uint8_t)(sum >> 8) && cm.ram[0x2F] == (uint8_t)sum,
              "base_kb 512: checksum 2Eh/2Fh still matches 10h-2Dh");
        cm.base_kb = 0;
        vdd_cmos_reset(&cm);
        CHECK(cm.ram[0x15] == 0x80 && cm.ram[0x16] == 0x02, "base_kb 0: 640 KB, as before");
    }

    /* ── THE PERIODIC INTERRUPT. docs/ref/rtc.md 4. ───────────────────────────
       IRQ8 at the rate in Status A bits 3:0 -- a fast, steady tick INDEPENDENT
       of the 8254, which is why Windows and DOS extenders use it.
       ⚠ DORMANT BY DEFAULT, AND THAT IS WHAT MAKES IT SAFE TO ADD: nothing is
         raised unless the guest sets PIE *and* a non-zero rate, and even then
         nothing reaches it until IRQ8 is unmasked on the PIC. */
    {
        g_irq8 = 0;
        vdd_cmos_reset(&cm);
        cm.rtc_now = fake_rtc;

        /* The rate table. 1 and 2 are special cases; from 3 up it is
           32768 >> (RS-1), so RS=6 is the 1024 Hz a PC BIOS leaves. */
        cm.status_a = 0x20; CHECK(vdd_cmos_periodic_hz(&cm) == 0, "periodic: RS=0 is no rate");
        cm.status_a = 0x21; CHECK(vdd_cmos_periodic_hz(&cm) == 256,  "periodic: RS=1 is 256 Hz");
        cm.status_a = 0x22; CHECK(vdd_cmos_periodic_hz(&cm) == 128,  "periodic: RS=2 is 128 Hz");
        cm.status_a = 0x23; CHECK(vdd_cmos_periodic_hz(&cm) == 8192, "periodic: RS=3 is 8192 Hz");
        cm.status_a = 0x26; CHECK(vdd_cmos_periodic_hz(&cm) == 1024, "periodic: RS=6 is 1024 Hz");
        cm.status_a = 0x2F; CHECK(vdd_cmos_periodic_hz(&cm) == 2,    "periodic: RS=15 is 2 Hz");

        /* ⛔ DORMANT UNTIL ASKED. A second of clocks with PIE clear must raise
           nothing at all -- this is the check that says adding the device cannot
           disturb a guest that never programs it. */
        cm.status_a = 0x26;
        vdd_cmos_add_clocks(&cm, PIT_INPUT_HZ);
        CHECK(g_irq8 == 0, "periodic: PIE clear -> not one interrupt in a whole second");
        CHECK(cm.status_c == 0, "periodic: ...and no flag set either");

        /* Now enable it: 1024 Hz for one second is 1024 interrupts. */
        wr(&bus, CMOS_STATUS_B, 0x42);          /* PIE | 24-hour */
        CHECK((rd(&bus, CMOS_STATUS_B) & 0x40) != 0, "periodic: PIE is writable");
        g_irq8 = 0;
        vdd_cmos_add_clocks(&cm, PIT_INPUT_HZ);
        CHECK(g_irq8 == 1024, "periodic: 1024 Hz for one second is 1024 interrupts");
        CHECK(cm.pf_raised == 1024, "periodic: ...and the run can say so");

        /* The flags a handler sees, and the acknowledge that clears them. */
        CHECK((rd(&bus, CMOS_STATUS_C) & 0xC0) == 0xC0, "periodic: PF and IRQF are set");
        CHECK(rd(&bus, CMOS_STATUS_C) == 0x00, "periodic: ...and the read cleared them");

        /* Turning PIE off drops the part-accumulated tick, so turning it back on
           starts from now instead of firing immediately off a stale remainder. */
        wr(&bus, CMOS_STATUS_B, 0x02);
        vdd_cmos_add_clocks(&cm, 1000);
        CHECK(cm.pf_accum == 0, "periodic: disabling PIE drops the partial tick");
        g_irq8 = 0;
        wr(&bus, CMOS_STATUS_B, 0x42);
        vdd_cmos_add_clocks(&cm, 1);
        CHECK(g_irq8 == 0, "periodic: re-enabling starts from now, not from a backlog");

        /* UIP is READ-ONLY: it is the chip telling software when it may read,
           never software telling the chip anything. */
        wr(&bus, CMOS_STATUS_A, 0xFF);
        CHECK((rd(&bus, CMOS_STATUS_A) & 0x80) == 0, "status A: UIP cannot be written");
        CHECK(vdd_cmos_periodic_hz(&cm) == 2, "status A: ...but the rate select can");
    }

    /* ── DATA MODE AND 12-HOUR MODE ARE HONOURED, NOT IGNORED. ───────────────
       A PC BIOS leaves BCD/24-hour and nothing on the shelf changes it -- but a
       model that ignores the bits hands a guest that DID set binary mode a BCD
       byte, and tells a 12-hour guest that 14:00 is 2 AM. */
    {
        vdd_cmos_reset(&cm);
        cm.rtc_now = fake_rtc;                   /* 14:07:42 */
        wr(&bus, CMOS_STATUS_B, 0x06);           /* DM=1 (binary), 24-hour */
        CHECK(rd(&bus, CMOS_SEC) == 42, "data mode: binary seconds are 42, not 0x42");
        CHECK(rd(&bus, CMOS_HOUR) == 14, "data mode: binary hours are 14");

        wr(&bus, CMOS_STATUS_B, 0x00);           /* BCD, 12-hour */
        CHECK(rd(&bus, CMOS_HOUR) == 0x82,
              "12-hour: 14:00 is 2 PM -- 0x02 with bit 7 set, not 0x02 alone");
        cm.status_b = 0x00;
        CHECK(rd(&bus, CMOS_MIN) == 0x07, "12-hour: the other fields are unaffected");
    }

    /* ── THE ALARM AND UPDATE-ENDED INTERRUPTS. docs/ref/rtc.md 4. ────────────
       ⛔ THE ALARM REGISTERS WERE BEING REFUSED, and that was a defect I put in
         myself: "everything below 0x0E is read-only" swept up 01h, 03h and 05h
         -- the seconds/minutes/hours ALARM -- which is the only way to set an
         alarm at all. The rule is not "low registers are read-only"; it is WE
         CANNOT MOVE THE HOST'S CLOCK, which applies to 00/02/04 and the date,
         not to a comparison value the guest owns. Exactly the same shape as
         refusing Status B and making the periodic interrupt unreachable. */
    {
        vdd_cmos_reset(&cm);
        cm.rtc_now = fake_rtc;                     /* 14:07:42 */
        g_irq8 = 0;

        wr(&bus, 0x01, 0x42);
        CHECK(rd(&bus, 0x01) == 0x42, "alarm: the seconds alarm register is writable");
        wr(&bus, 0x03, 0x07);
        wr(&bus, 0x05, 0x14);
        CHECK(rd(&bus, 0x05) == 0x14, "alarm: ...and the hours alarm too");

        /* UPDATE ENDED: once a second, if UIE is set. */
        wr(&bus, CMOS_STATUS_B, 0x12);             /* UIE | 24-hour */
        vdd_cmos_add_clocks(&cm, PIT_INPUT_HZ);
        CHECK(cm.uf_raised == 1, "update: one second is one update-ended flag");
        CHECK(g_irq8 == 1, "update: ...and one IRQ8");
        CHECK((rd(&bus, CMOS_STATUS_C) & 0x90) == 0x90, "update: UF and IRQF are set");

        /* THE ALARM FIRES WHEN THE CLOCK MATCHES -- 14:07:42, which is what the
           three registers above were set to. */
        vdd_cmos_reset(&cm); cm.rtc_now = fake_rtc; g_irq8 = 0;
        wr(&bus, 0x01, 0x42); wr(&bus, 0x03, 0x07); wr(&bus, 0x05, 0x14);
        wr(&bus, CMOS_STATUS_B, 0x22);             /* AIE | 24-hour */
        vdd_cmos_add_clocks(&cm, PIT_INPUT_HZ);
        CHECK(cm.af_raised == 1, "alarm: it fires when the clock matches");
        CHECK((rd(&bus, CMOS_STATUS_C) & 0x20) != 0, "alarm: AF is set");

        /* ...and NOT when it does not. */
        vdd_cmos_reset(&cm); cm.rtc_now = fake_rtc; g_irq8 = 0;
        wr(&bus, 0x01, 0x11); wr(&bus, 0x03, 0x22); wr(&bus, 0x05, 0x09);
        wr(&bus, CMOS_STATUS_B, 0x22);
        vdd_cmos_add_clocks(&cm, PIT_INPUT_HZ);
        CHECK(cm.af_raised == 0, "alarm: a different time does not fire it");
        CHECK(g_irq8 == 0, "alarm: ...and raises no interrupt");

        /* ⚠ THE MATCH RULE IS NOT "EQUAL". An alarm byte with its top two bits
           set is a DON'T CARE -- that is how "every minute at 42 seconds" is
           programmed, and a model that only compares for equality cannot
           express it at all. */
        vdd_cmos_reset(&cm); cm.rtc_now = fake_rtc; g_irq8 = 0;
        wr(&bus, 0x01, 0x42); wr(&bus, 0x03, 0xFF); wr(&bus, 0x05, 0xFF);
        wr(&bus, CMOS_STATUS_B, 0x22);
        vdd_cmos_add_clocks(&cm, PIT_INPUT_HZ);
        CHECK(cm.af_raised == 1, "alarm: don't-care fields (>= 0xC0) match anything");

        /* Both disabled: the second accumulator is dropped, so enabling later
           starts from now rather than firing off a stale remainder. */
        vdd_cmos_reset(&cm); cm.rtc_now = fake_rtc; g_irq8 = 0;
        vdd_cmos_add_clocks(&cm, PIT_INPUT_HZ * 3);
        CHECK(g_irq8 == 0 && cm.sec_accum == 0,
              "update/alarm: dormant with UIE and AIE clear");
    }

    /* THE CHECKSUM OVER 10h-2Dh, which a BIOS verifies at boot. Leaving it zero
       tells anything that checks that our CMOS is corrupt -- the same wrong
       answer Status D's VRT bit used to give. */
    {
        unsigned i, sum = 0;
        vdd_cmos_reset(&cm);
        for (i = 0x10; i <= 0x2D; ++i) sum += rd(&bus, (uint8_t)i);
        CHECK(rd(&bus, 0x2E) == ((sum >> 8) & 0xFF) && rd(&bus, 0x2F) == (sum & 0xFF),
              "cmos: the checksum at 2Eh/2Fh covers 10h-2Dh");
    }

    /* ── GH #261: WRITING THE CLOCK, with the host hook present. ──────────────
         Without it (every check above) a clock write is refused. With it: a write
         outside SET goes to the host at once; with SET held, reads are frozen,
         writes collect, and releasing SET commits date then time in one step. */
    {
        vdd_cmos_reset(&cm); cm.rtc_now = fake_rtc; cm.rtc_set = fake_set; g_nset = 0;
        wr(&bus, CMOS_HOUR, 0x09);
        CHECK(g_nset == 1 && g_set_what[0] == 0 && g_set[0].hour == 9
              && g_set[0].min == 7 && g_set[0].sec == 42,
              "clock write: hours 0x09 BCD -> host time 09:07:42, outside SET");
        wr(&bus, CMOS_DOW, 0x02);
        CHECK(g_nset == 1, "clock write: the day of week stays refused");

        g_nset = 0;
        wr(&bus, CMOS_STATUS_B, 0x92);          /* SET + UIE + 24h */
        CHECK(rd(&bus, CMOS_STATUS_B) == 0x82, "SET: going high clears UIE (datasheet)");
        wr(&bus, CMOS_HOUR, 0x12); wr(&bus, CMOS_MIN, 0x34); wr(&bus, CMOS_SEC, 0x56);
        CHECK(g_nset == 0, "SET held: writes are not committed yet");
        CHECK(rd(&bus, CMOS_HOUR) == 0x12 && rd(&bus, CMOS_MIN) == 0x34
              && rd(&bus, CMOS_SEC) == 0x56, "SET held: reads show the frozen, written copy");
        wr(&bus, CMOS_DOM, 0x15); wr(&bus, CMOS_MONTH, 0x06);
        wr(&bus, CMOS_YEAR, 0x99); wr(&bus, CMOS_CENTURY, 0x19);
        wr(&bus, CMOS_STATUS_B, 0x02);
        CHECK(g_nset == 2 && g_set_what[0] == 1 && g_set_what[1] == 0,
              "SET released: one date commit, then one time commit");
        CHECK(g_set[0].cent == 19 && g_set[0].year == 99 && g_set[0].month == 6
              && g_set[0].day == 15 && g_set[1].hour == 12 && g_set[1].min == 34
              && g_set[1].sec == 56, "SET released: 1999-06-15 12:34:56 committed");
        CHECK(rd(&bus, CMOS_HOUR) == 0x14, "SET released: reads follow the host clock again");

        g_nset = 0;
        wr(&bus, CMOS_STATUS_B, 0x06);          /* binary, 24h */
        wr(&bus, CMOS_MIN, 45);
        CHECK(g_nset == 1 && g_set[0].min == 45, "DM binary: 45 is forty-five, not 0x45");
        g_nset = 0;
        wr(&bus, CMOS_STATUS_B, 0x00);          /* BCD, 12-hour */
        wr(&bus, CMOS_HOUR, 0x83);
        CHECK(g_nset == 1 && g_set[0].hour == 15, "12-hour: 0x83 is 3 PM = 15:00");
        g_nset = 0;
        wr(&bus, CMOS_HOUR, 0x92);
        CHECK(g_nset == 1 && g_set[0].hour == 12, "12-hour: 0x92 is 12 PM = noon");
        g_nset = 0;
        wr(&bus, CMOS_HOUR, 0x12);
        CHECK(g_nset == 1 && g_set[0].hour == 0, "12-hour: 0x12 is 12 AM = midnight");
        vdd_cmos_reset(&cm);
        CHECK(cm.rtc_set == fake_set && cm.rtc_now == fake_rtc,
              "reset keeps both host hooks");
        cm.rtc_set = 0;
    }

    printf("\n%d checks, %d failed\n", total, fails);
    return fails ? 1 : 0;
}
