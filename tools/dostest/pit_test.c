/* pit_test.c -- off-VM unit battery for the PIT timer VDD (vdd_pit.c).
 *
 * M3 slice-2: exercise the 8254 channel-0 model, the clocks->IRQ0 time engine,
 * and the BIOS INT 08h / INT 1Ah services natively on the build host -- the
 * same no-VM discipline as the MCB and bus batteries. The PIT runs on a real
 * vdd_bus with a flat memory buffer (for 0040:006C) and a counting IRQ sink.
 */
#include <stdio.h>
#include <string.h>
#include "vdd_pit.h"

static int total = 0, fails = 0;
#define CHECK(cond, msg) do {                                  \
        total++;                                               \
        if (cond) { printf("  PASS  %s\n", (msg)); }           \
        else      { printf("  FAIL  %s\n", (msg)); fails++; }  \
    } while (0)

static int g_irq = 0;
static void irq_sink(void *ctx, uint8_t irq) { (void)ctx; if (irq == 0) g_irq++; }

static uint8_t  g_flat[0x100000];          /* guest low memory (BDA at 0x400)   */
static uint32_t *bda_tick(void) { return (uint32_t *)(g_flat + 0x46C); }   /* 0040:006C */
static uint8_t  *bda_flag(void) { return g_flat + 0x470; }                 /* 0040:0070 */

int main(void)
{
    vdd_bus bus;
    pit_state pit; memset(&pit, 0, sizeof pit);
    ntvdd dev = vdd_pit_device(&pit);
    uint32_t v; ntvdd_regs r;

    printf("== M3 slice-2 PIT timer battery ==\n");

    vdd_bus_init(&bus, g_flat);
    vdd_bus_set_sinks(&bus, irq_sink, 0, 0, 0);

    /* T0: device registers its hooks ------------------------------------- */
    CHECK(vdd_bus_add(&bus, &dev) == 0, "add: pit init ok");
    CHECK(bus.n_ports == 1 && bus.n_frame == 1, "add: ports + frame claimed");
    CHECK(bus.ints[0x08].svc && bus.ints[0x1A].svc, "add: INT 08h + 1Ah claimed");
    CHECK(pit_eff_reload(&pit) == 0x10000, "init: default reload = 65536 (18.2 Hz)");

    /* T1: program channel 0 reload via 0x43 (lo/hi) + two 0x40 writes ----- */
    v = 0x36; vdd_bus_io(&bus, 0x43, 1, 0, &v);          /* ch0, lo/hi, mode3 */
    v = 0x00; vdd_bus_io(&bus, 0x40, 1, 0, &v);          /* lo                */
    v = 0x10; vdd_bus_io(&bus, 0x40, 1, 0, &v);          /* hi -> 0x1000      */
    CHECK(pit.reload == 0x1000, "8254: lo/hi reload programmed to 0x1000");

    /* T1b: A HALF-WRITTEN COUNT IS NOT A COUNT. -------------------------------
       The 8254 buffers the LSB and loads the count register when the MSB
       arrives, so between a guest's two `out 40h` instructions the rate must not
       move. Read-modify-writing `reload` per byte instead is what gave ZAR
       (GH #23) a divisor of 2 -- 596 kHz -- for the whole gap between its two
       writes, and 29,657 IRQ0 raises in 45 s against a programmed 36.4 Hz.
       Starting from 0x1000 and programming 0x8000 is the exact shape: the old
       MSB (0x10) with the new LSB (0x00) is 0x1000, and the OTHER order --
       old 0x8000 with a new LSB of 0x02 -- is the catastrophic one. */
    v = 0x36; vdd_bus_io(&bus, 0x43, 1, 0, &v);          /* ch0, lo/hi, mode3 */
    v = 0x00; vdd_bus_io(&bus, 0x40, 1, 0, &v);          /* LSB only so far   */
    CHECK(pit.reload == 0x1000, "8254: LSB alone does NOT change the rate");
    v = 0x80; vdd_bus_io(&bus, 0x40, 1, 0, &v);          /* MSB -> commit     */
    CHECK(pit.reload == 0x8000, "8254: the MSB write commits both bytes at once");
    /* ...and the pathological order, which is ZAR's: a small LSB against a large
       standing MSB must not be visible as a divisor of 2 even for one clock. */
    v = 0x36; vdd_bus_io(&bus, 0x43, 1, 0, &v);
    v = 0x02; vdd_bus_io(&bus, 0x40, 1, 0, &v);
    CHECK(pit.reload == 0x8000, "8254: a small LSB cannot transiently mean 596 kHz");
    v = 0x11; vdd_bus_io(&bus, 0x40, 1, 0, &v);
    CHECK(pit.reload == 0x1102, "8254: ...and the pair still lands where asked");

    /* T1c: LSB-only and MSB-only ZERO the other half (Intel 8254 datasheet).
       The same read-modify-write mistake in a second dress -- a guest that re-rates
       a channel with a single MSB write would inherit whatever LSB was standing.
       ZAR does not take this path (it uses lo/hi); this is fidelity on its own
       merits, from the datasheet rather than from a run. */
    v = 0x16; vdd_bus_io(&bus, 0x43, 1, 0, &v);          /* ch0, LSB only     */
    v = 0x34; vdd_bus_io(&bus, 0x40, 1, 0, &v);
    CHECK(pit.reload == 0x0034, "8254: LSB-only write zeroes the MSB");
    v = 0x26; vdd_bus_io(&bus, 0x43, 1, 0, &v);          /* ch0, MSB only     */
    v = 0x80; vdd_bus_io(&bus, 0x40, 1, 0, &v);
    CHECK(pit.reload == 0x8000, "8254: MSB-only write zeroes the LSB");

    /* restore what the rest of the battery expects */
    v = 0x36; vdd_bus_io(&bus, 0x43, 1, 0, &v);
    v = 0x00; vdd_bus_io(&bus, 0x40, 1, 0, &v);
    v = 0x10; vdd_bus_io(&bus, 0x40, 1, 0, &v);
    CHECK(pit.reload == 0x1000, "8254: reprogrammed back to 0x1000");

    /* T2: the time engine emits one IRQ0 per elapsed reload --------------- */
    g_irq = 0; pit.accum = 0; pit.total_clocks = 0;
    vdd_pit_add_clocks(&pit, 0x1000 * 3);                /* exactly 3 periods */
    CHECK(g_irq == 3, "engine: 3 reloads of clocks -> 3 IRQ0");
    g_irq = 0;
    vdd_pit_add_clocks(&pit, 0x1000 - 1);               /* just under one    */
    CHECK(g_irq == 0, "engine: sub-reload clocks -> no IRQ0");
    vdd_pit_add_clocks(&pit, 1);                         /* crosses the edge  */
    CHECK(g_irq == 1, "engine: accumulator carries across calls");

    /* T3: default-rate sanity -- 65536 clocks == exactly one tick --------- */
    pit.reload = 0; pit.access = 3; pit.accum = 0; pit.total_clocks = 0; g_irq = 0;
    vdd_pit_add_clocks(&pit, 0x10000);
    CHECK(g_irq == 1, "engine: 65536 clocks at default reload -> 1 IRQ0");

    /* T4: the frame tick converts ~1/60 s into the right number of ticks -- */
    pit.reload = 0; pit.accum = 0; pit.total_clocks = 0; g_irq = 0;
    pit.frame_us = PIT_DEFAULT_FRAME_US;
    /* 60 frames ~= 1 second ~= 18 ticks (18.2065 Hz) */
    {
        int f; for (f = 0; f < 60; ++f) vdd_bus_frame(&bus);
    }
    CHECK(g_irq == 18, "frame: ~60 frames (1 s) -> 18 INT-8 ticks");

    /* T5: INT 08h increments the BIOS tick at 0040:006C ------------------- */
    *bda_tick() = 5; *bda_flag() = 0;
    memset(&r, 0, sizeof r);
    CHECK(vdd_bus_deliver_int(&bus, 0x08, &r) == 1, "int08: delivered");
    CHECK(*bda_tick() == 6, "int08: tick count 5 -> 6");

    /* T6: INT 08h midnight rollover ------------------------------------- */
    *bda_tick() = PIT_TICKS_PER_DAY - 1; *bda_flag() = 0;
    vdd_bus_deliver_int(&bus, 0x08, &r);
    CHECK(*bda_tick() == 0 && *bda_flag() == 1, "int08: rollover -> 0 + midnight flag");

    /* T7: INT 1Ah AH=00 reads the tick and clears the midnight flag ------- */
    *bda_tick() = 0x00ABCDEF; *bda_flag() = 1;
    memset(&r, 0, sizeof r); s_ah(&r, 0x00); r.cf = 1;
    vdd_bus_deliver_int(&bus, 0x1A, &r);
    CHECK(r_cx(&r) == 0x00AB && r_dx(&r) == 0xCDEF, "int1a/00: CX:DX = tick count");
    CHECK(r_al(&r) == 1 && *bda_flag() == 0 && r.cf == 0, "int1a/00: AL=flag, flag cleared, CF=0");

    /* T8: INT 1Ah AH=01 sets the tick count ------------------------------ */
    memset(&r, 0, sizeof r); s_ah(&r, 0x01); s_cx(&r, 0x0012); s_dx(&r, 0x3456);
    *bda_flag() = 1;
    vdd_bus_deliver_int(&bus, 0x1A, &r);
    CHECK(*bda_tick() == 0x00123456 && *bda_flag() == 0, "int1a/01: tick set, flag cleared");

    /* T9: a latched count reads back lo then hi via port 0x40 ------------- */
    pit.reload = 0x1234; pit.access = 3; pit.total_clocks = 0; pit.load_clocks = 0;
    pit.latched = 0;
    v = 0x00; vdd_bus_io(&bus, 0x43, 1, 0, &v);          /* latch ch0 count   */
    {
        uint32_t lo = 0, hi = 0;
        vdd_bus_io(&bus, 0x40, 1, 1, &lo);              /* lo byte           */
        vdd_bus_io(&bus, 0x40, 1, 1, &hi);              /* hi byte           */
        CHECK(((hi << 8) | lo) == 0x1234, "8254: latched count reads lo/hi = 0x1234");
    }

    /* T11: THE COUNT STARTS WHEN THE GUEST LOADS IT (Intel 8254, mode 0). -------
       Lemmings' "High Performance PC" option calibrates its game tick by loading
       counter 0 with 0xFFFF in mode 0, waiting 160 scanlines, latching, and using
       0xFFFF - latch as the PIT reload (guest code at CS:15BB..1633, read from the
       real-DOS dump). The model used to derive the count from the FREE-RUNNING
       phase `total_clocks % reload`, unrelated to the moment of the load, so the
       game read a value uniformly random in 0..65535: the user's by-hand run got
       0x4BB9 (61.5 Hz) where 160 lines at 31.47 kHz is 6067 clocks (~197 Hz). On
       real silicon the load restarts the counting element, so the read-back is the
       elapsed clocks -- whatever the phase was before the load. */
    vdd_pit_add_clocks(&pit, 12345);                     /* an arbitrary phase */
    v = 0x30; vdd_bus_io(&bus, 0x43, 1, 0, &v);          /* ch0, lo/hi, mode 0 */
    v = 0xFF; vdd_bus_io(&bus, 0x40, 1, 0, &v);
    v = 0xFF; vdd_bus_io(&bus, 0x40, 1, 0, &v);          /* count = 0xFFFF     */
    vdd_pit_add_clocks(&pit, 6067);                      /* 160 scanlines      */
    v = 0x06; vdd_bus_io(&bus, 0x43, 1, 0, &v);          /* latch (the game's byte) */
    v = 0x36; vdd_bus_io(&bus, 0x43, 1, 0, &v);          /* control word BEFORE the read */
    {
        uint32_t lo = 0, hi = 0;
        vdd_bus_io(&bus, 0x40, 1, 1, &lo);
        vdd_bus_io(&bus, 0x40, 1, 1, &hi);
        CHECK(0xFFFF - ((hi << 8) | lo) == 6067,
              "8254: mode-0 count = load - elapsed, from the LOAD instant, not the phase");
    }
    /* mode 0 runs on past terminal count: 0x100 loaded, 0x180 later reads 0xFF80 */
    v = 0x30; vdd_bus_io(&bus, 0x43, 1, 0, &v);
    v = 0x00; vdd_bus_io(&bus, 0x40, 1, 0, &v);
    v = 0x01; vdd_bus_io(&bus, 0x40, 1, 0, &v);
    vdd_pit_add_clocks(&pit, 0x180);
    v = 0x00; vdd_bus_io(&bus, 0x43, 1, 0, &v);
    {
        uint32_t lo = 0, hi = 0;
        vdd_bus_io(&bus, 0x40, 1, 1, &lo);
        vdd_bus_io(&bus, 0x40, 1, 1, &hi);
        CHECK(((hi << 8) | lo) == 0xFF80, "8254: mode 0 wraps past terminal count");
    }

    /* T12: modes 2 and 3 reload at the period; mode 3 steps by TWO per clock.
       (Datasheet: in mode 3 the counter decrements by two and reloads at zero,
       so a read never shows an odd LSB; DOSBox masks it the same way.) */
    /* ⚠ The PHASE is established directly, not by a count write: in modes 2/3 a write
       deliberately does NOT restart the counting element (T13), so going through the
       port here would be testing load semantics instead of the read-back arithmetic
       this case exists to pin. The control word only sets mode/access. */
    v = 0x34; vdd_bus_io(&bus, 0x43, 1, 0, &v);          /* mode 2, lo/hi     */
    pit.reload = 0x1000; pit.total_clocks = 0; pit.load_clocks = 0; pit.accum = 0;
    vdd_pit_add_clocks(&pit, 100);
    v = 0x00; vdd_bus_io(&bus, 0x43, 1, 0, &v);
    {
        uint32_t lo = 0, hi = 0;
        vdd_bus_io(&bus, 0x40, 1, 1, &lo);
        vdd_bus_io(&bus, 0x40, 1, 1, &hi);
        CHECK(((hi << 8) | lo) == 0x1000 - 100, "8254: mode 2 counts one per clock from the load");
    }
    vdd_pit_add_clocks(&pit, 0x1000);                    /* one full period later: same */
    v = 0x00; vdd_bus_io(&bus, 0x43, 1, 0, &v);
    {
        uint32_t lo = 0, hi = 0;
        vdd_bus_io(&bus, 0x40, 1, 1, &lo);
        vdd_bus_io(&bus, 0x40, 1, 1, &hi);
        CHECK(((hi << 8) | lo) == 0x1000 - 100, "8254: mode 2 reloads at the period");
    }
    v = 0x36; vdd_bus_io(&bus, 0x43, 1, 0, &v);          /* mode 3, lo/hi     */
    pit.reload = 0x1000; pit.total_clocks = 0; pit.load_clocks = 0; pit.accum = 0;
    vdd_pit_add_clocks(&pit, 100);
    v = 0x00; vdd_bus_io(&bus, 0x43, 1, 0, &v);
    {
        uint32_t lo = 0, hi = 0;
        vdd_bus_io(&bus, 0x40, 1, 1, &lo);
        vdd_bus_io(&bus, 0x40, 1, 1, &hi);
        CHECK(((hi << 8) | lo) == 0x1000 - 200, "8254: mode 3 counts TWO per clock");
    }
    vdd_pit_add_clocks(&pit, 0x800 - 100 + 1);           /* half a period + 1 */
    v = 0x00; vdd_bus_io(&bus, 0x43, 1, 0, &v);
    {
        uint32_t lo = 0, hi = 0;
        vdd_bus_io(&bus, 0x40, 1, 1, &lo);
        vdd_bus_io(&bus, 0x40, 1, 1, &hi);
        CHECK(((hi << 8) | lo) == 0x1000 - 2, "8254: mode 3 reloads every HALF period");
    }

    /* ── T13: ★★★★★ A COUNT WRITE MUST NOT RESTART THE PERIOD IN MODE 2 OR 3. ───────
       ⛔ THE s69 REGRESSION, AND THIS TEST USED TO ASSERT THE OPPOSITE -- I wrote the
          expectation from my belief that "the control word stops the counter", which
          is true only of the one-shot modes. Intel 8254, modes 2/3: a count written
          between CLK pulses is not loaded until the end of the current cycle.
       ★ THE SHAPE THAT BROKE LEMMINGS: it reprograms counter 0 with the SAME reload
          once per frame from its vblank sync routine. If each write resets the
          accumulator, IRQ0 stops coming at the programmed rate and comes out at the
          REPROGRAM rate instead -- measured on the rig at 69.95/s against 92.5 Hz
          programmed. So drive exactly that: a guest re-writing the same count more
          often than the period must still get the programmed rate. */
    v = 0x36; vdd_bus_io(&bus, 0x43, 1, 0, &v);          /* mode 3            */
    v = 0x00; vdd_bus_io(&bus, 0x40, 1, 0, &v);
    v = 0x10; vdd_bus_io(&bus, 0x40, 1, 0, &v);          /* 0x1000 = 4096     */
    g_irq = 0;
    {   /* 16 rounds of "advance a third of a period, then reprogram the same count".
           48 periods' worth of clocks must yield 16 IRQ0, not 0. */
        int k;
        for (k = 0; k < 48; ++k) {
            vdd_pit_add_clocks(&pit, 0x1000 / 3);
            if (k % 3 == 0) {                            /* reprogram, same value */
                v = 0x36; vdd_bus_io(&bus, 0x43, 1, 0, &v);
                v = 0x00; vdd_bus_io(&bus, 0x40, 1, 0, &v);
                v = 0x10; vdd_bus_io(&bus, 0x40, 1, 0, &v);
            }
        }
        CHECK(g_irq >= 15 && g_irq <= 16,
              "8254: mode 3 re-written faster than its period still ticks at the PROGRAMMED rate");
    }
    /* ...and a guest reprogramming faster than the period cannot silence IRQ0. */
    {   int k; g_irq = 0;
        for (k = 0; k < 40; ++k) {
            vdd_pit_add_clocks(&pit, 0x1000 / 4);
            v = 0x36; vdd_bus_io(&bus, 0x43, 1, 0, &v);  /* every single round */
            v = 0x00; vdd_bus_io(&bus, 0x40, 1, 0, &v);
            v = 0x10; vdd_bus_io(&bus, 0x40, 1, 0, &v);
        }
        CHECK(g_irq >= 9, "8254: reprogramming every quarter-period does NOT silence the timer");
    }
    /* The one-shot modes DO restart on a write -- that is the calibration path (T11),
       and it is the half of the original claim that was right. */
    v = 0x30; vdd_bus_io(&bus, 0x43, 1, 0, &v);          /* mode 0            */
    v = 0x00; vdd_bus_io(&bus, 0x40, 1, 0, &v);
    v = 0x10; vdd_bus_io(&bus, 0x40, 1, 0, &v);          /* 0x1000            */
    vdd_pit_add_clocks(&pit, 0x0F00);
    g_irq = 0;
    v = 0x30; vdd_bus_io(&bus, 0x43, 1, 0, &v);
    v = 0x00; vdd_bus_io(&bus, 0x40, 1, 0, &v);
    v = 0x10; vdd_bus_io(&bus, 0x40, 1, 0, &v);          /* re-arm            */
    vdd_pit_add_clocks(&pit, 0x1000 - 1);
    CHECK(g_irq == 0, "8254: a MODE 0 re-arm does restart the period (no inherited phase)");
    vdd_pit_add_clocks(&pit, 1);
    CHECK(g_irq == 1, "8254: ...and fires exactly one reload after the re-arm");

    /* T10: reset restores defaults but keeps the bus link ---------------- */
    pit.reload = 0x9999; pit.accum = 777; pit.total_clocks = 999;
    vdd_pit_reset(&pit);
    CHECK(pit.reload == 0 && pit.accum == 0 && pit.total_clocks == 0, "reset: counters cleared");
    CHECK(pit.bus == &bus && pit.access == 3 && pit.frame_us == PIT_DEFAULT_FRAME_US,
          "reset: bus link + defaults restored");

    printf("\n%d checks, %d failed\n", total, fails);
    return fails ? 1 : 0;
}
