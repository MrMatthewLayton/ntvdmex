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
    out->hour = 14; out->min = 7;   out->sec = 42;
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

    printf("\n%d checks, %d failed\n", total, fails);
    return fails ? 1 : 0;
}
