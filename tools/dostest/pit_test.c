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

/* Read counter 0 the way a guest does: Counter Latch Command, then two INs.
   Deliberately NOT a peek at pit_current_count -- that is static, and the port
   path is the contract a guest actually depends on. */
static unsigned pit_latched_count(vdd_bus *bus)
{
    uint32_t lo = 0, hi = 0, cw = 0x00;      /* ch0, access 00 = latch */
    vdd_bus_io(bus, 0x43, 1, 0, &cw);
    vdd_bus_io(bus, 0x40, 1, 1, &lo);
    vdd_bus_io(bus, 0x40, 1, 1, &hi);
    return (unsigned)((lo & 0xFF) | ((hi & 0xFF) << 8));
}

/* Counter 2 through its ports, the way a guest reads it. */
static unsigned pit_latched_ch2(vdd_bus *bus)
{
    uint32_t lo = 0, hi = 0, cw = 0x80;      /* ch2, access 00 = latch */
    vdd_bus_io(bus, 0x43, 1, 0, &cw);
    vdd_bus_io(bus, 0x42, 1, 1, &lo);
    vdd_bus_io(bus, 0x42, 1, 1, &hi);
    return (unsigned)((lo & 0xFF) | ((hi & 0xFF) << 8));
}

static int g_irq = 0;
static void irq_sink(void *ctx, uint8_t irq) { (void)ctx; if (irq == 0) g_irq++; }

static uint8_t  g_flat[0x100000];          /* guest low memory (BDA at 0x400)   */
static uint32_t *bda_tick(void) { return (uint32_t *)(g_flat + 0x46C); }   /* 0040:006C */
static uint8_t  *bda_flag(void) { return g_flat + 0x470; }                 /* 0040:0070 */

/* A fixed instant, so the BCD conversion is pinned rather than read off the wall:
   2026-09-15 23:41:07. */
static void fake_rtc(void *ctx, struct vdd_rtc *out)
{
    (void)ctx;
    out->cent = 20; out->year = 26; out->month = 9; out->day = 15;
    out->hour = 23; out->min = 41; out->sec = 7;
}

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

    /* T10: reset restores defaults but keeps the bus link ---------------- */
    pit.reload = 0x9999; pit.accum = 777; pit.total_clocks = 999;
    vdd_pit_reset(&pit);
    CHECK(pit.reload == 0 && pit.accum == 0 && pit.total_clocks == 0, "reset: counters cleared");
    CHECK(pit.bus == &bus && pit.access == 3 && pit.frame_us == PIT_DEFAULT_FRAME_US,
          "reset: bus link + defaults restored");

    /* ── T11-T15: WHEN A LOAD RESTARTS THE PERIOD, from the Intel 8254 datasheet ───────
       (231164-005, Mode Definitions). Every expectation below is a quote, not a belief
       -- s69 shipped two opposite models, each certified by a test written from memory.
       Lemmings' HP-mode timer ISR is the shape under test: Control Word + count on every
       tick ("synchronized by software"), and the calibration is a mode-0 read-back. */

    /* T11: mode 0 read-back counts from the LOAD instant, not the free-running phase.
       "After the Control Word and initial count are written ... the initial count will
        be loaded on the next CLK pulse" -- Lemmings loads 0xFFFF, waits 320 hblanks
       (~12,100 clocks), latches, and uses 0xFFFF - latch as its tick. */
    vdd_pit_reset(&pit);
    vdd_pit_add_clocks(&pit, 54321);                      /* an arbitrary prior phase */
    v = 0x30; vdd_bus_io(&bus, 0x43, 1, 0, &v);           /* ch0, lo/hi, MODE 0      */
    v = 0xFF; vdd_bus_io(&bus, 0x40, 1, 0, &v);
    v = 0xFF; vdd_bus_io(&bus, 0x40, 1, 0, &v);           /* count 0xFFFF, loaded NOW */
    vdd_pit_add_clocks(&pit, 12100);
    v = 0x00; vdd_bus_io(&bus, 0x43, 1, 0, &v);           /* latch                    */
    v = 0x36; vdd_bus_io(&bus, 0x43, 1, 0, &v);           /* Lemmings: mode 3 CW next */
    {   uint32_t lo = 0, hi = 0;
        vdd_bus_io(&bus, 0x40, 1, 1, &lo); vdd_bus_io(&bus, 0x40, 1, 1, &hi);
        CHECK(0xFFFF - ((hi << 8) | lo) == 12100,
              "8254 mode 0: latched count = 0xFFFF - clocks since the LOAD (phase-free)");
    }

    /* T12: Control Word + count RESTARTS the period in mode 3.
       "After writing a Control Word and initial count, the Counter will be loaded on
        the next CLK pulse. This allows the Counter to be synchronized by software." */
    vdd_pit_reset(&pit); g_irq = 0;
    v = 0x36; vdd_bus_io(&bus, 0x43, 1, 0, &v);
    v = 0x00; vdd_bus_io(&bus, 0x40, 1, 0, &v);
    v = 0x10; vdd_bus_io(&bus, 0x40, 1, 0, &v);           /* N = 0x1000              */
    vdd_pit_add_clocks(&pit, 0x0C00);                     /* 3/4 through the period  */
    v = 0x36; vdd_bus_io(&bus, 0x43, 1, 0, &v);           /* CW + the SAME count ... */
    v = 0x00; vdd_bus_io(&bus, 0x40, 1, 0, &v);
    v = 0x10; vdd_bus_io(&bus, 0x40, 1, 0, &v);
    vdd_pit_add_clocks(&pit, 0x0FFF);
    CHECK(g_irq == 0, "8254 mode 3: CW+count restarts -- no IRQ0 at the OLD period's end");
    vdd_pit_add_clocks(&pit, 1);
    CHECK(g_irq == 1, "8254 mode 3: CW+count restarts -- IRQ0 exactly N clocks after the load");

    /* T13: a BARE count write in mode 2/3 does NOT disturb the period in flight.
       "Writing a new count while counting does not affect the current counting
        sequence ... the new count will be loaded at the end of the current counting
        cycle." (mode 2; mode 3 identically, modulo its half-cycle) */
    vdd_pit_reset(&pit); g_irq = 0;
    v = 0x34; vdd_bus_io(&bus, 0x43, 1, 0, &v);           /* ch0, lo/hi, MODE 2      */
    v = 0x00; vdd_bus_io(&bus, 0x40, 1, 0, &v);
    v = 0x10; vdd_bus_io(&bus, 0x40, 1, 0, &v);           /* N = 0x1000              */
    vdd_pit_add_clocks(&pit, 0x0C00);
    v = 0x00; vdd_bus_io(&bus, 0x40, 1, 0, &v);
    v = 0x02; vdd_bus_io(&bus, 0x40, 1, 0, &v);           /* bare write: M = 0x200   */
    vdd_pit_add_clocks(&pit, 0x03FF);
    CHECK(g_irq == 0, "8254 mode 2: bare count write leaves the current period alone");
    vdd_pit_add_clocks(&pit, 1);
    CHECK(g_irq == 1, "8254 mode 2: ...IRQ0 at the OLD period's end");
    CHECK(pit.reload == 0x200, "8254 mode 2: the new count takes over at that boundary");
    g_irq = 0; vdd_pit_add_clocks(&pit, 0x200 * 4);
    CHECK(g_irq == 4, "8254 mode 2: ...and the new period runs from there");

    /* T14: THE LEMMINGS SHAPE. CW+count re-written after every IRQ, N clocks apart plus
       a spin: the tick is one per (N + spin), never faster, never free-running. */
    vdd_pit_reset(&pit); g_irq = 0;
    {   int tick, spin = 3500, N = 12904;                  /* measured on the rig      */
        v = 0x36; vdd_bus_io(&bus, 0x43, 1, 0, &v);
        v = (uint32_t)(N & 0xFF); vdd_bus_io(&bus, 0x40, 1, 0, &v);
        v = (uint32_t)(N >> 8);   vdd_bus_io(&bus, 0x40, 1, 0, &v);
        for (tick = 0; tick < 10; ++tick) {
            int before = g_irq;
            vdd_pit_add_clocks(&pit, (uint32_t)(N - 1));
            if (g_irq != before) break;                   /* early: free-running      */
            vdd_pit_add_clocks(&pit, 1);                  /* the IRQ: ISR entered     */
            vdd_pit_add_clocks(&pit, (uint32_t)spin);     /* ISR spins for retrace    */
            v = 0x36; vdd_bus_io(&bus, 0x43, 1, 0, &v);   /* ...then reprograms       */
            v = (uint32_t)(N & 0xFF); vdd_bus_io(&bus, 0x40, 1, 0, &v);
            v = (uint32_t)(N >> 8);   vdd_bus_io(&bus, 0x40, 1, 0, &v);
        }
        CHECK(g_irq == 10, "8254 Lemmings shape: exactly one IRQ0 per (N + spin), locked");
    }

    /* T15: one-shot modes restart on any count write.
       Mode 0: "If a new count is written to the Counter, it will be loaded on the next
        CLK pulse and counting will continue from the new count." */
    vdd_pit_reset(&pit);
    v = 0x30; vdd_bus_io(&bus, 0x43, 1, 0, &v);
    v = 0x00; vdd_bus_io(&bus, 0x40, 1, 0, &v);
    v = 0x10; vdd_bus_io(&bus, 0x40, 1, 0, &v);
    vdd_pit_add_clocks(&pit, 0x0800);
    v = 0x00; vdd_bus_io(&bus, 0x40, 1, 0, &v);
    v = 0x20; vdd_bus_io(&bus, 0x40, 1, 0, &v);           /* bare write, mode 0      */
    vdd_pit_add_clocks(&pit, 0x0100);
    v = 0x00; vdd_bus_io(&bus, 0x43, 1, 0, &v);
    {   uint32_t lo = 0, hi = 0;
        vdd_bus_io(&bus, 0x40, 1, 1, &lo); vdd_bus_io(&bus, 0x40, 1, 1, &hi);
        CHECK(((hi << 8) | lo) == 0x2000 - 0x100,
              "8254 mode 0: a bare count write loads and counts from the new count");
    }
    /* T16: ★ INT 1Ah's RTC HALF -- AH=02h/04h, in BCD.
         Both fell into `default:` ("RTC subfns not modelled yet"), which leaves every
         register exactly as the caller passed it. p_bios.asm found it by POISONing
         CX/DX and getting the poison straight back, where the 6.22 oracle answers
         with the time and date. BCD is the contract: a guest reads these as BCD
         because that is what a BIOS returns, so a binary 34 would be read as 22. */
    {   ntvdd_regs r;
        pit_state *ps = &pit;
        ps->rtc_now = fake_rtc; ps->rtc_ctx = 0;
        memset(&r, 0, sizeof r); s_ah(&r, 0x02);
        r.ecx = 0xC1C1; r.edx = 0xD1D1;          /* the probe's poison, same idea */
        vdd_bus_deliver_int(&bus, 0x1A, &r);
        /* 23 -> 0x23, 41 -> 0x41. (I first wrote 0x1729 here from carelessness and
           this check failed on its first run, which is the point of writing it.) */
        CHECK(r_cx(&r) == 0x2341, "int1a/02: 23:41 comes back as BCD 2341, not binary 174A");
        CHECK((r_dx(&r) >> 8) == 0x07, "int1a/02: 7 seconds -> DH=07");
        CHECK(r.cf == 0, "int1a/02: answered, CF=0");

        memset(&r, 0, sizeof r); s_ah(&r, 0x04);
        r.ecx = 0xC1C1; r.edx = 0xD1D1;
        vdd_bus_deliver_int(&bus, 0x1A, &r);
        CHECK(r_cx(&r) == 0x2026, "int1a/04: century+year 2026 -> CX=2026 BCD");
        CHECK(r_dx(&r) == 0x0915, "int1a/04: 15 September -> DX=0915 BCD");
        CHECK(r.cf == 0, "int1a/04: answered, CF=0");

        /* ...and with NO clock installed the call is NOT answered. Fabricating a date
           would be worse than silence: a guest would stamp every file with it. */
        ps->rtc_now = 0;
        memset(&r, 0, sizeof r); s_ah(&r, 0x02);
        r.ecx = 0xC1C1;
        vdd_bus_deliver_int(&bus, 0x1A, &r);
        CHECK(r_cx(&r) == 0xC1C1, "int1a/02: no clock installed -> left alone, not invented");
        ps->rtc_now = fake_rtc;
    }


    /* T9: MODES 6 AND 7 ARE ALIASES FOR 2 AND 3. -----------------------------
       docs/ref/pit.md 3: the Control Word's mode field is three bits, but only
       six modes exist -- 110 IS mode 2 and 111 IS mode 3 on real silicon. Our
       model stores the raw three bits, so anything that tests `mode == 2` or
       `mode == 3` silently excludes a guest that programmed the alias.

       ⚠ THE FAILURE IS NARROWER THAN THE INVENTORY FIRST CLAIMED, and the
         difference matters. `periodic` does NOT gate IRQ0 -- vdd_pit_add_clocks
         raises it from the accumulator regardless of mode -- so an aliased guest
         still gets its interrupt. What it loses is:
           * the BARE-COUNT LOAD RULE (a count written with no Control Word must
             wait for the end of the current period in modes 2/3, not restart);
           * mode 3's DECREMENT-BY-TWO count law on read-back.
         Both are timing/read-back defects, not a dead timer. Measuring before
         asserting is the whole point. */
    {
        uint32_t w;
        /* Mode 2, the reference behaviour: Control Word + count loads at once... */
        w = 0x34; vdd_bus_io(&bus, 0x43, 1, 0, &w);      /* ch0 lo/hi mode 2   */
        w = 0x00; vdd_bus_io(&bus, 0x40, 1, 0, &w);
        w = 0x10; vdd_bus_io(&bus, 0x40, 1, 0, &w);      /* -> 0x1000          */
        CHECK(pit.reload == 0x1000 && !pit.next_pending,
              "mode2: control word + count loads immediately");
        /* ...and a BARE count parks until the end of the period. */
        w = 0x00; vdd_bus_io(&bus, 0x40, 1, 0, &w);
        w = 0x20; vdd_bus_io(&bus, 0x40, 1, 0, &w);      /* -> 0x2000, bare    */
        CHECK(pit.reload == 0x1000 && pit.next_pending,
              "mode2: a BARE count parks until the period ends");

        /* Mode 6 must do exactly the same. */
        w = 0x3C; vdd_bus_io(&bus, 0x43, 1, 0, &w);      /* ch0 lo/hi mode 6   */
        w = 0x00; vdd_bus_io(&bus, 0x40, 1, 0, &w);
        w = 0x10; vdd_bus_io(&bus, 0x40, 1, 0, &w);
        CHECK(pit.reload == 0x1000 && !pit.next_pending,
              "mode6: control word + count loads immediately");
        w = 0x00; vdd_bus_io(&bus, 0x40, 1, 0, &w);
        w = 0x20; vdd_bus_io(&bus, 0x40, 1, 0, &w);
        CHECK(pit.reload == 0x1000 && pit.next_pending,
              "mode6 == mode2: a BARE count parks until the period ends");

        /* Mode 3 counts DOWN BY TWO; mode 7 must read back the same way.
           Read it the way a guest does -- latch, then two INs -- rather than by
           reaching into the model: the port path is the contract. */
        w = 0x36; vdd_bus_io(&bus, 0x43, 1, 0, &w);      /* ch0 lo/hi mode 3   */
        w = 0x00; vdd_bus_io(&bus, 0x40, 1, 0, &w);
        w = 0x10; vdd_bus_io(&bus, 0x40, 1, 0, &w);      /* -> 0x1000          */
        pit.total_clocks = pit.load_clocks + 4;
        CHECK(pit_latched_count(&bus) == 0x1000 - 8,
              "mode3: the count decrements by two per clock");

        w = 0x3E; vdd_bus_io(&bus, 0x43, 1, 0, &w);      /* ch0 lo/hi mode 7   */
        w = 0x00; vdd_bus_io(&bus, 0x40, 1, 0, &w);
        w = 0x10; vdd_bus_io(&bus, 0x40, 1, 0, &w);
        pit.total_clocks = pit.load_clocks + 4;
        CHECK(pit_latched_count(&bus) == 0x1000 - 8,
              "mode7 == mode3: the count decrements by two per clock");

        /* Leave counter 0 as the BIOS would: periodic, 65536. */
        w = 0x34; vdd_bus_io(&bus, 0x43, 1, 0, &w);
        w = 0x00; vdd_bus_io(&bus, 0x40, 1, 0, &w);
        w = 0x00; vdd_bus_io(&bus, 0x40, 1, 0, &w);
    }

    /* T10: THE READ-BACK COMMAND AND ITS STATUS BYTE. ------------------------
       docs/ref/pit.md 4. Control word 11xxxxxx, and ⚠ THE TWO LATCH BITS ARE
       ACTIVE LOW: bit 5 = 0 latches the count, bit 4 = 0 latches the status.
       Bits 3/2/1 select counters 2/1/0.

       Status byte: b7 = OUT pin, b6 = null count, b5:4 = access, b3:1 = mode,
       b0 = BCD.

       ⚠ b3:1 CARRIES THE MODE AS PROGRAMMED, NOT NORMALISED. Measured on a real
         8254 (p_pit.asm pit.mode6.readback = 0x0C): programming 110 reads back
         110, even though the counter BEHAVES as mode 2. That is why the T9 fix
         keeps mode_raw alongside mode, and this case is what would catch a
         regression that collapsed them. */
    {
        uint32_t w, r1, r2;
        /* Counter 0: lo/hi, mode 2, binary -> status bits 5:0 = 0x34. */
        w = 0x34; vdd_bus_io(&bus, 0x43, 1, 0, &w);
        w = 0x00; vdd_bus_io(&bus, 0x40, 1, 0, &w);
        w = 0x10; vdd_bus_io(&bus, 0x40, 1, 0, &w);

        w = 0xE2; vdd_bus_io(&bus, 0x43, 1, 0, &w);   /* rb: status only, ch0  */
        r1 = 0; vdd_bus_io(&bus, 0x40, 1, 1, &r1);
        CHECK((r1 & 0x3F) == 0x34,
              "readback: status reports lo/hi + mode 2 + binary");

        /* WITH BOTH LATCH BITS CLEAR, THE STATUS COMES OUT FIRST AND THE COUNT
           AFTER IT (ref/pit.md 4). That order is the property worth asserting --
           an earlier draft of this case tested `... || 1`, which is a check that
           cannot fail, i.e. exactly the thing flagged this morning as worse than
           no case at all. */
        w = 0xC2; vdd_bus_io(&bus, 0x43, 1, 0, &w);   /* rb: status AND count  */
        r1 = 0; vdd_bus_io(&bus, 0x40, 1, 1, &r1);    /* -> status             */
        CHECK((r1 & 0x3F) == 0x34, "readback: status is read FIRST when both are latched");
        r1 = 0; vdd_bus_io(&bus, 0x40, 1, 1, &r1);    /* -> count lo           */
        r2 = 0; vdd_bus_io(&bus, 0x40, 1, 1, &r2);    /* -> count hi           */
        CHECK(((r2 << 8) | r1) <= 0x1000,
              "readback: ...and the latched COUNT follows it");

        /* Mode 6 must read back as 110, un-normalised. */
        w = 0x3C; vdd_bus_io(&bus, 0x43, 1, 0, &w);   /* ch0 lo/hi mode 6      */
        w = 0x00; vdd_bus_io(&bus, 0x40, 1, 0, &w);
        w = 0x10; vdd_bus_io(&bus, 0x40, 1, 0, &w);
        w = 0xE2; vdd_bus_io(&bus, 0x43, 1, 0, &w);
        r1 = 0; vdd_bus_io(&bus, 0x40, 1, 1, &r1);
        CHECK((r1 & 0x0E) == 0x0C,
              "readback: mode 6 reads back as 110, NOT normalised to 010");

        /* NULL COUNT: set once a control word is written, cleared when the
           count reaches the counting element. */
        w = 0x34; vdd_bus_io(&bus, 0x43, 1, 0, &w);   /* CW, no count yet      */
        w = 0xE2; vdd_bus_io(&bus, 0x43, 1, 0, &w);
        r1 = 0; vdd_bus_io(&bus, 0x40, 1, 1, &r1);
        CHECK((r1 & 0x40) != 0, "readback: null count SET after a control word");
        w = 0x00; vdd_bus_io(&bus, 0x40, 1, 0, &w);
        w = 0x10; vdd_bus_io(&bus, 0x40, 1, 0, &w);   /* now it loads          */
        w = 0xE2; vdd_bus_io(&bus, 0x43, 1, 0, &w);
        r1 = 0; vdd_bus_io(&bus, 0x40, 1, 1, &r1);
        CHECK((r1 & 0x40) == 0, "readback: null count CLEARED once the count loads");

        /* Counter 2 through the same command, selected by bit 3. */
        w = 0xB0; vdd_bus_io(&bus, 0x43, 1, 0, &w);   /* ch2 lo/hi mode 0      */
        w = 0x00; vdd_bus_io(&bus, 0x42, 1, 0, &w);
        w = 0x80; vdd_bus_io(&bus, 0x42, 1, 0, &w);
        w = 0xE8; vdd_bus_io(&bus, 0x43, 1, 0, &w);   /* rb: status only, ch2  */
        r1 = 0; vdd_bus_io(&bus, 0x42, 1, 1, &r1);
        CHECK((r1 & 0x3F) == 0x30,
              "readback: counter 2 status reports lo/hi + mode 0 + binary");

        /* Leave counter 0 as the BIOS would. */
        w = 0x34; vdd_bus_io(&bus, 0x43, 1, 0, &w);
        w = 0x00; vdd_bus_io(&bus, 0x40, 1, 0, &w);
        w = 0x00; vdd_bus_io(&bus, 0x40, 1, 0, &w);
    }

    /* T11: COUNTER 2'S OUT PIN, AND ITS GATE. --------------------------------
       docs/ref/pit.md 2 and 5. Counter 2 is the only counter whose OUT a PC
       exposes directly -- port 61h bit 5 -- and the only one whose GATE software
       controls, at port 61h bit 0. Together they are the classic "measure time
       without interrupts" idiom: program a count, poll bit 5, count the loops.
       All three oracles agree the bit must move (p_pit pit.61h.bit5.toggles = 1
       on 6.22, dosbox-x AND PCem); we alone return a constant. */
    {
        uint32_t w; unsigned a, b2;
        vdd_pit_ch2_gate(&pit, 1);                      /* gate high: counting  */
        w = 0xB6; vdd_bus_io(&bus, 0x43, 1, 0, &w);     /* ch2 lo/hi mode 3     */
        w = 0x40; vdd_bus_io(&bus, 0x42, 1, 0, &w);
        w = 0x00; vdd_bus_io(&bus, 0x42, 1, 0, &w);     /* reload 0x0040        */

        /* Mode 3 is a square wave: OUT high for the first half of the period,
           low for the second. R = 64, so the half is 32. */
        pit.total_clocks = pit.c2.load_clocks;
        CHECK(vdd_pit_ch2_out(&pit) == 1, "ch2 OUT is high at the start of the period");
        pit.total_clocks = pit.c2.load_clocks + 0x30;   /* 48 of 64: past half  */
        CHECK(vdd_pit_ch2_out(&pit) == 0, "ch2 OUT is low in the second half");
        pit.total_clocks = pit.c2.load_clocks + 0x40;   /* a full period on     */
        CHECK(vdd_pit_ch2_out(&pit) == 1, "ch2 OUT is high again a period later");

        /* THE GATE STOPS THE COUNTER. Clearing 61h bit 0 must freeze it; setting
           the bit must resume from where it stopped, not restart. */
        a = pit_latched_ch2(&bus);
        vdd_pit_ch2_gate(&pit, 0);
        pit.total_clocks += 1000;
        b2 = pit_latched_ch2(&bus);
        CHECK(a == b2, "gate LOW freezes counter 2");
        vdd_pit_ch2_gate(&pit, 1);
        pit.total_clocks += 8;
        CHECK(pit_latched_ch2(&bus) != b2, "gate HIGH resumes counter 2");
    }

    printf("\n%d checks, %d failed\n", total, fails);
    return fails ? 1 : 0;
}
