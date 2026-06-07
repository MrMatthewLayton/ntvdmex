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
    pit.reload = 0x1234; pit.access = 3; pit.total_clocks = 0; pit.latched = 0;
    v = 0x00; vdd_bus_io(&bus, 0x43, 1, 0, &v);          /* latch ch0 count   */
    {
        uint32_t lo = 0, hi = 0;
        vdd_bus_io(&bus, 0x40, 1, 1, &lo);              /* lo byte           */
        vdd_bus_io(&bus, 0x40, 1, 1, &hi);              /* hi byte           */
        CHECK(((hi << 8) | lo) == 0x1234, "8254: latched count reads lo/hi = 0x1234");
    }

    /* T10: reset restores defaults but keeps the bus link ---------------- */
    pit.reload = 0x9999; pit.accum = 777; pit.total_clocks = 999;
    vdd_pit_reset(&pit);
    CHECK(pit.reload == 0 && pit.accum == 0 && pit.total_clocks == 0, "reset: counters cleared");
    CHECK(pit.bus == &bus && pit.access == 3 && pit.frame_us == PIT_DEFAULT_FRAME_US,
          "reset: bus link + defaults restored");

    printf("\n%d checks, %d failed\n", total, fails);
    return fails ? 1 : 0;
}
