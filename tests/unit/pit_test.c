/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * Off-VM unit battery for the PIT timer VDD (vdd_pit.c).
 *
 * M3 slice-2: exercise the 8254 channel-0 model, the clocks->IRQ0 time engine,
 * and the BIOS INT 08h / INT 1Ah services natively on the build host -- the
 * same no-VM discipline as the MCB and bus batteries. The PIT runs on a real
 * VDD_BUS with a flat memory buffer (for 0040:006C) and a counting IRQ sink.
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#include <stdio.h>
#include <string.h>
#include "vdd_pit.h"
#define CHECK(condition, message) do {                                  \
        g_Total++;                                               \
        if (condition) { printf("  PASS  %s\n", (message)); }           \
        else      { printf("  FAIL  %s\n", (message)); g_Failures++; }  \
    } while (0)

static INT g_Total = 0;
static INT g_Failures = 0;

static INT g_Irq0Count = 0;

static BYTE  g_GuestMemory[0x100000];          /* guest low memory (BDA at 0x400) */

/* Read counter 0 the way a guest does: Counter Latch Command, then two INs.
 * Deliberately NOT a peek at PitCurrentCount -- that is static, and the port
 * path is the contract a guest actually depends on.
 */
static UINT PitTestLatchedCount(PVDD_BUS bus)
{
    /* ch0, access 00 = latch */
    UINT32 low = 0;
    UINT32 high = 0;
    UINT32 controlWord = 0x00;

    VddBusIo(bus, 0x43, 1, 0, &controlWord);
    VddBusIo(bus, 0x40, 1, 1, &low);
    VddBusIo(bus, 0x40, 1, 1, &high);
    return (UINT)((low & 0xFF) | ((high & 0xFF) << 8));
}

/* Counter 2 through its ports, the way a guest reads it. */
static UINT PitTestLatchedCounter2(PVDD_BUS bus)
{
    /* ch2, access 00 = latch */
    UINT32 low = 0;
    UINT32 high = 0;
    UINT32 controlWord = 0x80;

    VddBusIo(bus, 0x43, 1, 0, &controlWord);
    VddBusIo(bus, 0x42, 1, 1, &low);
    VddBusIo(bus, 0x42, 1, 1, &high);
    return (UINT)((low & 0xFF) | ((high & 0xFF) << 8));
}

static VOID PitTestIrqSink(PVOID context, BYTE irq)
{
    (VOID)context;

    if (irq == 0)
        g_Irq0Count++;
}

static PUINT32 PitTestBdaTick(VOID) /* 0040:006C */
{
    return (PUINT32)(g_GuestMemory + 0x46C);
}

static PBYTE PitTestBdaFlag(VOID) /* 0040:0070 */
{
    return g_GuestMemory + 0x470;
}

/* A fixed instant, so the BCD conversion is pinned rather than read off the wall:
 * 2026-09-15 23:41:07.
 */
static VOID PitTestFakeRtc(PVOID context, PPIT_RTC_READING output)
{
    (VOID)context;
    output->Century = 20;
    output->Year = 26;
    output->Month = 9;
    output->Day = 15;
    output->Hour = 23;
    output->Minute = 41;
    output->Second = 7;
}

INT main(VOID)
{
    VDD_BUS bus;
    PIT_STATE pit;

    memset(&pit, 0, sizeof pit);
    NTVDD_DEVICE device = VddPitDevice(&pit);
    UINT32 value;
    NTVDD_REGISTERS registers;

    printf("== M3 slice-2 PIT timer battery ==\n");

    VddBusInitialize(&bus, g_GuestMemory);
    VddBusSetSinks(&bus, PitTestIrqSink, 0, 0, 0);

    /* T0: device registers its hooks ------------------------------------- */
    CHECK(VddBusAdd(&bus, &device) == 0, "add: pit init ok");
    CHECK(bus.PortCount == 1 && bus.FrameCount == 1, "add: ports + frame claimed");
    CHECK(bus.Interrupts[0x08].Service && bus.Interrupts[0x1A].Service, "add: INT 08h + 1Ah claimed");
    CHECK(VddPitEffectiveReload(&pit) == 0x10000, "init: default reload = 65536 (18.2 Hz)");

    /* T1: program channel 0 reload via 0x43 (lo/hi) + two 0x40 writes ----- */
    value = 0x36;
    VddBusIo(&bus, 0x43, 1, 0, &value);          /* ch0, lo/hi, mode3 */
    value = 0x00;
    VddBusIo(&bus, 0x40, 1, 0, &value);          /* lo */
    value = 0x10;
    VddBusIo(&bus, 0x40, 1, 0, &value);          /* hi -> 0x1000 */
    CHECK(pit.Reload == 0x1000, "8254: lo/hi reload programmed to 0x1000");

    /* T1b: A HALF-WRITTEN COUNT IS NOT A COUNT. -------------------------------
     * The 8254 buffers the LSB and loads the count register when the MSB
     * arrives, so between a guest's two `out 40h` instructions the rate must not
     * move. Read-modify-writing `reload` per byte instead is what gave ZAR
     * (GH #23) a divisor of 2 -- 596 kHz -- for the whole gap between its two
     * writes, and 29,657 IRQ0 raises in 45 s against a programmed 36.4 Hz.
     * Starting from 0x1000 and programming 0x8000 is the exact shape: the old
     * MSB (0x10) with the new LSB (0x00) is 0x1000, and the OTHER order --
     * old 0x8000 with a new LSB of 0x02 -- is the catastrophic one.
     */
    value = 0x36;
    VddBusIo(&bus, 0x43, 1, 0, &value);          /* ch0, lo/hi, mode3 */
    value = 0x00;
    VddBusIo(&bus, 0x40, 1, 0, &value);          /* LSB only so far */
    CHECK(pit.Reload == 0x1000, "8254: LSB alone does NOT change the rate");
    value = 0x80;
    VddBusIo(&bus, 0x40, 1, 0, &value);          /* MSB -> commit */
    CHECK(pit.Reload == 0x8000, "8254: the MSB write commits both bytes at once");
    /* ...and the pathological order, which is ZAR's: a small LSB against a large
     * standing MSB must not be visible as a divisor of 2 even for one clock.
     */
    value = 0x36;
    VddBusIo(&bus, 0x43, 1, 0, &value);
    value = 0x02;
    VddBusIo(&bus, 0x40, 1, 0, &value);
    CHECK(pit.Reload == 0x8000, "8254: a small LSB cannot transiently mean 596 kHz");
    value = 0x11;
    VddBusIo(&bus, 0x40, 1, 0, &value);
    CHECK(pit.Reload == 0x1102, "8254: ...and the pair still lands where asked");

    /* T1c: LSB-only and MSB-only ZERO the other half (Intel 8254 datasheet).
     * The same read-modify-write mistake in a second dress -- a guest that re-rates
     * a channel with a single MSB write would inherit whatever LSB was standing.
     * ZAR does not take this path (it uses lo/hi); this is fidelity on its own
     * merits, from the datasheet rather than from a run.
     */
    value = 0x16;
    VddBusIo(&bus, 0x43, 1, 0, &value);          /* ch0, LSB only */
    value = 0x34;
    VddBusIo(&bus, 0x40, 1, 0, &value);
    CHECK(pit.Reload == 0x0034, "8254: LSB-only write zeroes the MSB");
    value = 0x26;
    VddBusIo(&bus, 0x43, 1, 0, &value);          /* ch0, MSB only */
    value = 0x80;
    VddBusIo(&bus, 0x40, 1, 0, &value);
    CHECK(pit.Reload == 0x8000, "8254: MSB-only write zeroes the LSB");

    /* restore what the rest of the battery expects */
    value = 0x36;
    VddBusIo(&bus, 0x43, 1, 0, &value);
    value = 0x00;
    VddBusIo(&bus, 0x40, 1, 0, &value);
    value = 0x10;
    VddBusIo(&bus, 0x40, 1, 0, &value);
    CHECK(pit.Reload == 0x1000, "8254: reprogrammed back to 0x1000");

    /* T2: the time engine emits one IRQ0 per elapsed reload --------------- */
    g_Irq0Count = 0;
    pit.Accumulator = 0;
    pit.TotalClocks = 0;
    VddPitAddClocks(&pit, 0x1000 * 3);                /* exactly 3 periods */
    CHECK(g_Irq0Count == 3, "engine: 3 reloads of clocks -> 3 IRQ0");
    g_Irq0Count = 0;
    VddPitAddClocks(&pit, 0x1000 - 1);               /* just under one */
    CHECK(g_Irq0Count == 0, "engine: sub-reload clocks -> no IRQ0");
    VddPitAddClocks(&pit, 1);                         /* crosses the edge */
    CHECK(g_Irq0Count == 1, "engine: accumulator carries across calls");

    /* T3: default-rate sanity -- 65536 clocks == exactly one tick --------- */
    pit.Reload = 0;
    pit.Access = 3;
    pit.Accumulator = 0;
    pit.TotalClocks = 0;
    g_Irq0Count = 0;
    VddPitAddClocks(&pit, 0x10000);
    CHECK(g_Irq0Count == 1, "engine: 65536 clocks at default reload -> 1 IRQ0");

    /* T4: the frame tick converts ~1/60 s into the right number of ticks -- */
    pit.Reload = 0;
    pit.Accumulator = 0;
    pit.TotalClocks = 0;
    g_Irq0Count = 0;
    pit.FrameMicroseconds = PIT_DEFAULT_FRAME_US;
    /* 60 frames ~= 1 second ~= 18 ticks (18.2065 Hz) */
    {
        INT frame;

        for (frame = 0; frame < 60; ++frame)
            VddBusFrame(&bus);
    }
    CHECK(g_Irq0Count == 18, "frame: ~60 frames (1 s) -> 18 INT-8 ticks");

    /* T5: INT 08h increments the BIOS tick at 0040:006C ------------------- */
    *PitTestBdaTick() = 5;
    *PitTestBdaFlag() = 0;
    memset(&registers, 0, sizeof registers);
    CHECK(VddBusDeliverInterrupt(&bus, 0x08, &registers) == 1, "int08: delivered");
    CHECK(*PitTestBdaTick() == 6, "int08: tick count 5 -> 6");

    /* T6: INT 08h midnight rollover ------------------------------------- */
    *PitTestBdaTick() = PIT_TICKS_PER_DAY - 1;
    *PitTestBdaFlag() = 0;
    VddBusDeliverInterrupt(&bus, 0x08, &registers);
    CHECK(*PitTestBdaTick() == 0 && *PitTestBdaFlag() == 1, "int08: rollover -> 0 + midnight flag");

    /* T7: INT 1Ah AH=00 reads the tick and clears the midnight flag ------- */
    *PitTestBdaTick() = 0x00ABCDEF;
    *PitTestBdaFlag() = 1;
    memset(&registers, 0, sizeof registers);
    VddSetAh(&registers, 0x00);
    registers.CarryFlag = 1;
    VddBusDeliverInterrupt(&bus, 0x1A, &registers);
    CHECK(VddGetCx(&registers) == 0x00AB && VddGetDx(&registers) == 0xCDEF, "int1a/00: CX:DX = tick count");
    CHECK(VddGetAl(&registers) == 1 && *PitTestBdaFlag() == 0 && registers.CarryFlag == 0, "int1a/00: AL=flag, flag cleared, CF=0");

    /* T8: INT 1Ah AH=01 sets the tick count ------------------------------ */
    memset(&registers, 0, sizeof registers);
    VddSetAh(&registers, 0x01);
    VddSetCx(&registers, 0x0012);
    VddSetDx(&registers, 0x3456);
    *PitTestBdaFlag() = 1;
    VddBusDeliverInterrupt(&bus, 0x1A, &registers);
    CHECK(*PitTestBdaTick() == 0x00123456 && *PitTestBdaFlag() == 0, "int1a/01: tick set, flag cleared");

    /* T8b: GH #262 case B -- the WITNESS: was 0040:006C last written by the BIOS? */
    {
        UINT32 takenTicks = 0xEEEE;
        UINT32 takenWraps = 0xEEEE;
        UINT32 takenSince = 0xEEEE;
        CHECK(pit.TickWitness == 0x00123456 && !pit.IsTickForeign,
              "witness: AH=01h with no host hook owns its count");
        CHECK(VddPitTickTake(&pit, *PitTestBdaTick(), &takenTicks, &takenWraps, &takenSince) == 0 && takenTicks == 0xEEEE,
              "witness: nothing stored -> take says so, outputs untouched");
        VddBusDeliverInterrupt(&bus, 0x08, &registers);
        VddBusDeliverInterrupt(&bus, 0x08, &registers);
        CHECK(!pit.IsTickForeign && pit.TickWitness == 0x00123458,
              "witness: the BIOS's own ticks are never foreign");
        /* a guest stores straight into 006C (p_tick2c case B), then two ticks */
        *PitTestBdaTick() = 0x000B8277u;
        VddBusDeliverInterrupt(&bus, 0x08, &registers);
        CHECK(pit.IsTickForeign == 1 && *PitTestBdaTick() == 0x000B8278u,
              "witness: the next tick sees the store, and still counts from it");
        VddBusDeliverInterrupt(&bus, 0x08, &registers);
        CHECK(VddPitTickTake(&pit, *PitTestBdaTick(), &takenTicks, &takenWraps, &takenSince) == 1
              && takenTicks == 0x000B8279u && takenWraps == 0 && takenSince == 2,
              "witness: take -> count, 0 wraps, 2 ticks since");
        CHECK(VddPitTickTake(&pit, *PitTestBdaTick(), &takenTicks, &takenWraps, &takenSince) == 0,
              "witness: ...once; the count is the BIOS's own from there");
        /* a store read before any tick: seen by the take itself, since = 0 */
        *PitTestBdaTick() = 0x1234;
        CHECK(VddPitTickTake(&pit, *PitTestBdaTick(), &takenTicks, &takenWraps, &takenSince) == 1
              && takenTicks == 0x1234 && takenWraps == 0 && takenSince == 0, "witness: a store with no tick since");
        /* a store just before midnight; the BIOS wraps it -- one day for DOS */
        *PitTestBdaTick() = PIT_TICKS_PER_DAY - 2;
        *PitTestBdaFlag() = 0;
        VddBusDeliverInterrupt(&bus, 0x08, &registers);
        VddBusDeliverInterrupt(&bus, 0x08, &registers);
        VddBusDeliverInterrupt(&bus, 0x08, &registers);
        CHECK(*PitTestBdaTick() == 1 && *PitTestBdaFlag() == 1, "witness: the stored count wraps as ever");
        CHECK(VddPitTickTake(&pit, *PitTestBdaTick(), &takenTicks, &takenWraps, &takenSince) == 1 && takenTicks == 1 && takenWraps == 1 && takenSince == 3,
              "witness: take -> 1 wrap, 3 ticks since");
        /* a NATURAL midnight is not counted: the host-time clock already turns the day */
        *PitTestBdaTick() = PIT_TICKS_PER_DAY - 1;
        VddPitTickOwned(&pit, *PitTestBdaTick());
        VddBusDeliverInterrupt(&bus, 0x08, &registers);
        CHECK(*PitTestBdaTick() == 0 && !pit.IsTickForeign && pit.TickWraps == 0,
              "witness: the BIOS's own midnight is not a store");
        /* the seed (POST) owns its count */
        pit.RtcNow = PitTestFakeRtc;
        CHECK(VddPitSeedTimeOfDay(&pit) == 1 && pit.TickWitness == *PitTestBdaTick()
              && !pit.IsTickForeign, "witness: the seed owns its count");
        pit.RtcNow = 0;
        /* and reset does not forget it: 006C is memory, not the chip */
        { UINT32 keep = pit.TickWitness;
          VddPitReset(&pit);
          CHECK(pit.TickWitness == keep, "witness: survives vdd_pit_reset"); }
        *PitTestBdaFlag() = 0;
    }

    /* T9: a latched count reads back lo then hi via port 0x40 ------------- */
    pit.Reload = 0x1234;
    pit.Access = 3;
    pit.TotalClocks = 0;
    pit.LoadClocks = 0;
    pit.IsLatched = 0;
    value = 0x00;
    VddBusIo(&bus, 0x43, 1, 0, &value);          /* latch ch0 count */
    {
        UINT32 low = 0;
        UINT32 high = 0;
        VddBusIo(&bus, 0x40, 1, 1, &low);              /* lo byte */
        VddBusIo(&bus, 0x40, 1, 1, &high);              /* hi byte */
        CHECK(((high << 8) | low) == 0x1234, "8254: latched count reads lo/hi = 0x1234");
    }

    /* T10: reset restores defaults but keeps the bus link ---------------- */
    pit.Reload = 0x9999;
    pit.Accumulator = 777;
    pit.TotalClocks = 999;
    VddPitReset(&pit);
    CHECK(pit.Reload == 0 && pit.Accumulator == 0 && pit.TotalClocks == 0, "reset: counters cleared");
    CHECK(pit.Bus == &bus && pit.Access == 3 && pit.FrameMicroseconds == PIT_DEFAULT_FRAME_US,
          "reset: bus link + defaults restored");

    /* T11-T15: WHEN A LOAD RESTARTS THE PERIOD, from the Intel 8254 datasheet:
     * (231164-005, Mode Definitions). Every expectation below is a quote, not a belief
     * -- s69 shipped two opposite models, each certified by a test written from memory.
     * Lemmings' HP-mode timer ISR is the shape under test: Control Word + count on every
     * tick ("synchronized by software"), and the calibration is a mode-0 read-back.
     */

    /* T11: mode 0 read-back counts from the LOAD instant, not the free-running phase.
     * "After the Control Word and initial count are written ... the initial count will
     *  be loaded on the next CLK pulse" -- Lemmings loads 0xFFFF, waits 320 hblanks
     * (~12,100 clocks), latches, and uses 0xFFFF - latch as its tick.
     */
    VddPitReset(&pit);
    VddPitAddClocks(&pit, 54321);                      /* an arbitrary prior phase */
    value = 0x30;
    VddBusIo(&bus, 0x43, 1, 0, &value);           /* ch0, lo/hi, MODE 0 */
    value = 0xFF;
    VddBusIo(&bus, 0x40, 1, 0, &value);
    value = 0xFF;
    VddBusIo(&bus, 0x40, 1, 0, &value);           /* count 0xFFFF, loaded NOW */
    VddPitAddClocks(&pit, 12100);
    value = 0x00;
    VddBusIo(&bus, 0x43, 1, 0, &value);           /* latch */
    value = 0x36;
    VddBusIo(&bus, 0x43, 1, 0, &value);           /* Lemmings: mode 3 CW next */
    {   UINT32 low = 0, high = 0;
        VddBusIo(&bus, 0x40, 1, 1, &low);
        VddBusIo(&bus, 0x40, 1, 1, &high);
        CHECK(0xFFFF - ((high << 8) | low) == 12100,
              "8254 mode 0: latched count = 0xFFFF - clocks since the LOAD (phase-free)");
    }

    /* T12: Control Word + count RESTARTS the period in mode 3.
     * "After writing a Control Word and initial count, the Counter will be loaded on
     *  the next CLK pulse. This allows the Counter to be synchronized by software."
     */
    VddPitReset(&pit);
    g_Irq0Count = 0;
    value = 0x36;
    VddBusIo(&bus, 0x43, 1, 0, &value);
    value = 0x00;
    VddBusIo(&bus, 0x40, 1, 0, &value);
    value = 0x10;
    VddBusIo(&bus, 0x40, 1, 0, &value);           /* N = 0x1000 */
    VddPitAddClocks(&pit, 0x0C00);                     /* 3/4 through the period */
    value = 0x36;
    VddBusIo(&bus, 0x43, 1, 0, &value);           /* CW + the SAME count ... */
    value = 0x00;
    VddBusIo(&bus, 0x40, 1, 0, &value);
    value = 0x10;
    VddBusIo(&bus, 0x40, 1, 0, &value);
    VddPitAddClocks(&pit, 0x0FFF);
    CHECK(g_Irq0Count == 0, "8254 mode 3: CW+count restarts -- no IRQ0 at the OLD period's end");
    VddPitAddClocks(&pit, 1);
    CHECK(g_Irq0Count == 1, "8254 mode 3: CW+count restarts -- IRQ0 exactly N clocks after the load");

    /* T13: a BARE count write in mode 2/3 does NOT disturb the period in flight.
     * "Writing a new count while counting does not affect the current counting
     *  sequence ... the new count will be loaded at the end of the current counting
     *  cycle." (mode 2; mode 3 identically, modulo its half-cycle)
     */
    VddPitReset(&pit);
    g_Irq0Count = 0;
    value = 0x34;
    VddBusIo(&bus, 0x43, 1, 0, &value);           /* ch0, lo/hi, MODE 2 */
    value = 0x00;
    VddBusIo(&bus, 0x40, 1, 0, &value);
    value = 0x10;
    VddBusIo(&bus, 0x40, 1, 0, &value);           /* N = 0x1000 */
    VddPitAddClocks(&pit, 0x0C00);
    value = 0x00;
    VddBusIo(&bus, 0x40, 1, 0, &value);
    value = 0x02;
    VddBusIo(&bus, 0x40, 1, 0, &value);           /* bare write: M = 0x200 */
    VddPitAddClocks(&pit, 0x03FF);
    CHECK(g_Irq0Count == 0, "8254 mode 2: bare count write leaves the current period alone");
    VddPitAddClocks(&pit, 1);
    CHECK(g_Irq0Count == 1, "8254 mode 2: ...IRQ0 at the OLD period's end");
    CHECK(pit.Reload == 0x200, "8254 mode 2: the new count takes over at that boundary");
    g_Irq0Count = 0;
    VddPitAddClocks(&pit, 0x200 * 4);
    CHECK(g_Irq0Count == 4, "8254 mode 2: ...and the new period runs from there");

    /* T14: THE LEMMINGS SHAPE. CW+count re-written after every IRQ, N clocks apart plus
     * a spin: the tick is one per (N + spin), never faster, never free-running.
     */
    VddPitReset(&pit);
    g_Irq0Count = 0;
    {   INT tick, spin = 3500, reload = 12904;                  /* measured on the test machine */
        value = 0x36;
        VddBusIo(&bus, 0x43, 1, 0, &value);
        value = (UINT32)(reload & 0xFF);
        VddBusIo(&bus, 0x40, 1, 0, &value);
        value = (UINT32)(reload >> 8);
        VddBusIo(&bus, 0x40, 1, 0, &value);

        for (tick = 0; tick < 10; ++tick)
        {
            INT before = g_Irq0Count;
            VddPitAddClocks(&pit, (UINT32)(reload - 1));

            if (g_Irq0Count != before)
                break;                                          /* early: free-running */

            VddPitAddClocks(&pit, 1);                  /* the IRQ: ISR entered */
            VddPitAddClocks(&pit, (UINT32)spin);     /* ISR spins for retrace */
            value = 0x36;
            VddBusIo(&bus, 0x43, 1, 0, &value);   /* ...then reprograms */
            value = (UINT32)(reload & 0xFF);
            VddBusIo(&bus, 0x40, 1, 0, &value);
            value = (UINT32)(reload >> 8);
            VddBusIo(&bus, 0x40, 1, 0, &value);
        }

        CHECK(g_Irq0Count == 10, "8254 Lemmings shape: exactly one IRQ0 per (N + spin), locked");
    }

    /* T15: one-shot modes restart on any count write.
     * Mode 0: "If a new count is written to the Counter, it will be loaded on the next
     *  CLK pulse and counting will continue from the new count."
     */
    VddPitReset(&pit);
    value = 0x30;
    VddBusIo(&bus, 0x43, 1, 0, &value);
    value = 0x00;
    VddBusIo(&bus, 0x40, 1, 0, &value);
    value = 0x10;
    VddBusIo(&bus, 0x40, 1, 0, &value);
    VddPitAddClocks(&pit, 0x0800);
    value = 0x00;
    VddBusIo(&bus, 0x40, 1, 0, &value);
    value = 0x20;
    VddBusIo(&bus, 0x40, 1, 0, &value);           /* bare write, mode 0 */
    VddPitAddClocks(&pit, 0x0100);
    value = 0x00;
    VddBusIo(&bus, 0x43, 1, 0, &value);
    {   UINT32 low = 0, high = 0;
        VddBusIo(&bus, 0x40, 1, 1, &low);
        VddBusIo(&bus, 0x40, 1, 1, &high);
        CHECK(((high << 8) | low) == 0x2000 - 0x100,
              "8254 mode 0: a bare count write loads and counts from the new count");
    }
    /* T16: INT 1Ah's RTC HALF -- AH=02h/04h, in BCD.
     * Both fell into `default:` ("RTC subfns not modelled yet"), which leaves every
     * register exactly as the caller passed it. p_bios.asm found it by POISONing
     * CX/DX and getting the poison straight back, where the 6.22 oracle answers
     * with the time and date. BCD is the contract: a guest reads these as BCD
     * because that is what a BIOS returns, so a binary 34 would be read as 22.
     */
    {   NTVDD_REGISTERS registers;
        PPIT_STATE pitPointer = &pit;
        pitPointer->RtcNow = PitTestFakeRtc;
        pitPointer->RtcContext = 0;
        memset(&registers, 0, sizeof registers);
        VddSetAh(&registers, 0x02);
        registers.Ecx = 0xC1C1;
        registers.Edx = 0xD1D1;          /* the probe's poison, same idea */
        VddBusDeliverInterrupt(&bus, 0x1A, &registers);
        /* 23 -> 0x23, 41 -> 0x41. (I first wrote 0x1729 here from carelessness and
         * this check failed on its first run, which is the point of writing it.)
         */
        CHECK(VddGetCx(&registers) == 0x2341, "int1a/02: 23:41 comes back as BCD 2341, not binary 174A");
        CHECK((VddGetDx(&registers) >> 8) == 0x07, "int1a/02: 7 seconds -> DH=07");
        CHECK(registers.CarryFlag == 0, "int1a/02: answered, CF=0");

        memset(&registers, 0, sizeof registers);
        VddSetAh(&registers, 0x04);
        registers.Ecx = 0xC1C1;
        registers.Edx = 0xD1D1;
        VddBusDeliverInterrupt(&bus, 0x1A, &registers);
        CHECK(VddGetCx(&registers) == 0x2026, "int1a/04: century+year 2026 -> CX=2026 BCD");
        CHECK(VddGetDx(&registers) == 0x0915, "int1a/04: 15 September -> DX=0915 BCD");
        CHECK(registers.CarryFlag == 0, "int1a/04: answered, CF=0");

        /* ...and with NO clock installed the call is NOT answered. Fabricating a date
         * would be worse than silence: a guest would stamp every file with it.
         */
        pitPointer->RtcNow = 0;
        memset(&registers, 0, sizeof registers);
        VddSetAh(&registers, 0x02);
        registers.Ecx = 0xC1C1;
        VddBusDeliverInterrupt(&bus, 0x1A, &registers);
        CHECK(VddGetCx(&registers) == 0xC1C1, "int1a/02: no clock installed -> left alone, not invented");
        pitPointer->RtcNow = PitTestFakeRtc;
    }

    /* T17: 0040:006C IS SEEDED WITH THE TIME OF DAY. (GH #253)
     * Nothing set it, so INT 1Ah AH=00h counted from 0 -- midnight -- at every launch
     * while AH=02h read the real time. The seed is ticks since midnight at the PIT's
     * own 1193182/65536 Hz, from the SAME clock AH=02h uses, so the two halves of
     * INT 1Ah agree: a seeded count read back through AH=00h divides back to the
     * second AH=02h reports. Expected values worked by hand, not by the function:
     * 00:00:00 -> 0;  01:00:00 -> 3600*1193182/65536 = 65543.04 -> 65543 (the IBM
     * BIOS's own hourly constant);  23:41:07 (PitTestFakeRtc) -> 85267 s -> 1552414.6
     * -> 1552414;  23:59:59 -> 1573024, under the 1573040 rollover.
     */
    {   PPIT_STATE pitPointer = &pit;
        CHECK(VddPitTicksSinceMidnight(0, 0, 0) == 0, "tod: 00:00:00 -> 0 ticks");
        CHECK(VddPitTicksSinceMidnight(1, 0, 0) == 65543u, "tod: one hour -> 65543 ticks (IBM's constant)");
        CHECK(VddPitTicksSinceMidnight(23, 59, 59) == 1573024u
              && VddPitTicksSinceMidnight(23, 59, 59) < PIT_TICKS_PER_DAY,
              "tod: 23:59:59 -> 1573024, below the midnight rollover");

        *PitTestBdaTick() = 0xDEADBEEF;
        *PitTestBdaFlag() = 1;
        pitPointer->RtcNow = PitTestFakeRtc;
        CHECK(VddPitSeedTimeOfDay(pitPointer) == 1, "seed: clock present -> seeded");
        CHECK(*PitTestBdaTick() == 1552414u, "seed: 23:41:07 -> 0040:006C = 1552414");
        CHECK(*PitTestBdaFlag() == 0, "seed: midnight flag cleared");

        memset(&registers, 0, sizeof registers);
        VddSetAh(&registers, 0x00);
        VddBusDeliverInterrupt(&bus, 0x1A, &registers);
        {   UINT32 ticks = ((UINT32)VddGetCx(&registers) << 16) | VddGetDx(&registers);
            UINT32 seconds = (UINT32)(((UINT64)ticks * 65536u + PIT_INPUT_HZ - 1) / PIT_INPUT_HZ);
            CHECK(ticks == 1552414u && VddGetAl(&registers) == 0, "seed: INT 1Ah AH=00h returns the seeded count, AL=0");
            CHECK(seconds == 23u * 3600u + 41u * 60u + 7u,
                  "seed: AH=00h's count and AH=02h's 23:41:07 are the same second");
        }

        pitPointer->RtcNow = 0;
        *PitTestBdaTick() = 0x1234;
        *PitTestBdaFlag() = 1;
        CHECK(VddPitSeedTimeOfDay(pitPointer) == 0 && *PitTestBdaTick() == 0x1234 && *PitTestBdaFlag() == 1,
              "seed: no clock -> count and flag left alone, not invented");
        pitPointer->RtcNow = PitTestFakeRtc;
    }

    /* T9: MODES 6 AND 7 ARE ALIASES FOR 2 AND 3. -----------------------------
     * docs/ref/pit.md 3: the Control Word's mode field is three bits, but only
     * six modes exist -- 110 IS mode 2 and 111 IS mode 3 on real silicon. Our
     * model stores the raw three bits, so anything that tests `mode == 2` or
     * `mode == 3` silently excludes a guest that programmed the alias.
     *
     * [CAUTION]: THE FAILURE IS NARROWER THAN THE INVENTORY FIRST CLAIMED, and the
     * difference matters. `periodic` does NOT gate IRQ0 -- VddPitAddClocks
     * raises it from the accumulator regardless of mode -- so an aliased guest
     * still gets its interrupt. What it loses is:
     *   * the BARE-COUNT LOAD RULE (a count written with no Control Word must
     *     wait for the end of the current period in modes 2/3, not restart);
     *   * mode 3's DECREMENT-BY-TWO count law on read-back.
     * Both are timing/read-back defects, not a dead timer. Measuring before
     * asserting is the whole point.
     */
    {
        UINT32 portValue;
        /* Mode 2, the reference behaviour: Control Word + count loads at once... */
        portValue = 0x34;
        VddBusIo(&bus, 0x43, 1, 0, &portValue);      /* ch0 lo/hi mode 2 */
        portValue = 0x00;
        VddBusIo(&bus, 0x40, 1, 0, &portValue);
        portValue = 0x10;
        VddBusIo(&bus, 0x40, 1, 0, &portValue);      /* -> 0x1000 */
        CHECK(pit.Reload == 0x1000 && !pit.IsNextPending,
              "mode2: control word + count loads immediately");
        /* ...and a BARE count parks until the end of the period. */
        portValue = 0x00;
        VddBusIo(&bus, 0x40, 1, 0, &portValue);
        portValue = 0x20;
        VddBusIo(&bus, 0x40, 1, 0, &portValue);      /* -> 0x2000, bare */
        CHECK(pit.Reload == 0x1000 && pit.IsNextPending,
              "mode2: a BARE count parks until the period ends");

        /* Mode 6 must do exactly the same. */
        portValue = 0x3C;
        VddBusIo(&bus, 0x43, 1, 0, &portValue);      /* ch0 lo/hi mode 6 */
        portValue = 0x00;
        VddBusIo(&bus, 0x40, 1, 0, &portValue);
        portValue = 0x10;
        VddBusIo(&bus, 0x40, 1, 0, &portValue);
        CHECK(pit.Reload == 0x1000 && !pit.IsNextPending,
              "mode6: control word + count loads immediately");
        portValue = 0x00;
        VddBusIo(&bus, 0x40, 1, 0, &portValue);
        portValue = 0x20;
        VddBusIo(&bus, 0x40, 1, 0, &portValue);
        CHECK(pit.Reload == 0x1000 && pit.IsNextPending,
              "mode6 == mode2: a BARE count parks until the period ends");

        /* Mode 3 counts DOWN BY TWO; mode 7 must read back the same way.
         * Read it the way a guest does -- latch, then two INs -- rather than by
         * reaching into the model: the port path is the contract.
         */
        portValue = 0x36;
        VddBusIo(&bus, 0x43, 1, 0, &portValue);      /* ch0 lo/hi mode 3 */
        portValue = 0x00;
        VddBusIo(&bus, 0x40, 1, 0, &portValue);
        portValue = 0x10;
        VddBusIo(&bus, 0x40, 1, 0, &portValue);      /* -> 0x1000 */
        pit.TotalClocks = pit.LoadClocks + 4;
        CHECK(PitTestLatchedCount(&bus) == 0x1000 - 8,
              "mode3: the count decrements by two per clock");

        portValue = 0x3E;
        VddBusIo(&bus, 0x43, 1, 0, &portValue);      /* ch0 lo/hi mode 7 */
        portValue = 0x00;
        VddBusIo(&bus, 0x40, 1, 0, &portValue);
        portValue = 0x10;
        VddBusIo(&bus, 0x40, 1, 0, &portValue);
        pit.TotalClocks = pit.LoadClocks + 4;
        CHECK(PitTestLatchedCount(&bus) == 0x1000 - 8,
              "mode7 == mode3: the count decrements by two per clock");

        /* Leave counter 0 as the BIOS would: periodic, 65536. */
        portValue = 0x34;
        VddBusIo(&bus, 0x43, 1, 0, &portValue);
        portValue = 0x00;
        VddBusIo(&bus, 0x40, 1, 0, &portValue);
        portValue = 0x00;
        VddBusIo(&bus, 0x40, 1, 0, &portValue);
    }

    /* T10: THE READ-BACK COMMAND AND ITS STATUS BYTE. ------------------------
     * docs/ref/pit.md 4. Control word 11xxxxxx, and [CAUTION] THE TWO LATCH BITS ARE
     * ACTIVE LOW: bit 5 = 0 latches the count, bit 4 = 0 latches the status.
     * Bits 3/2/1 select counters 2/1/0.
     *
     * Status byte: b7 = OUT pin, b6 = null count, b5:4 = access, b3:1 = mode,
     * b0 = BCD.
     *
     * [CAUTION]: b3:1 CARRIES THE MODE AS PROGRAMMED, NOT NORMALISED. Measured on a real
     * 8254 (p_pit.asm pit.mode6.readback = 0x0C): programming 110 reads back
     * 110, even though the counter BEHAVES as mode 2. That is why the T9 fix
     * keeps mode_raw alongside mode, and this case is what would catch a
     * regression that collapsed them.
     */
    {
        UINT32 portValue;
        UINT32 firstStatus;
        UINT32 secondStatus;
        /* Counter 0: lo/hi, mode 2, binary -> status bits 5:0 = 0x34. */
        portValue = 0x34;
        VddBusIo(&bus, 0x43, 1, 0, &portValue);
        portValue = 0x00;
        VddBusIo(&bus, 0x40, 1, 0, &portValue);
        portValue = 0x10;
        VddBusIo(&bus, 0x40, 1, 0, &portValue);

        portValue = 0xE2;
        VddBusIo(&bus, 0x43, 1, 0, &portValue);   /* rb: status only, ch0 */
        firstStatus = 0;
        VddBusIo(&bus, 0x40, 1, 1, &firstStatus);
        CHECK((firstStatus & 0x3F) == 0x34,
              "readback: status reports lo/hi + mode 2 + binary");

        /* WITH BOTH LATCH BITS CLEAR, THE STATUS COMES OUT FIRST AND THE COUNT
         * AFTER IT (ref/pit.md 4). That order is the property worth asserting --
         * an earlier draft of this case tested `... || 1`, which is a check that
         * cannot fail, i.e. exactly the thing flagged this morning as worse than
         * no case at all.
         */
        portValue = 0xC2;
        VddBusIo(&bus, 0x43, 1, 0, &portValue);   /* rb: status AND count */
        firstStatus = 0;
        VddBusIo(&bus, 0x40, 1, 1, &firstStatus);    /* -> status */
        CHECK((firstStatus & 0x3F) == 0x34, "readback: status is read FIRST when both are latched");
        firstStatus = 0;
        VddBusIo(&bus, 0x40, 1, 1, &firstStatus);    /* -> count lo */
        secondStatus = 0;
        VddBusIo(&bus, 0x40, 1, 1, &secondStatus);    /* -> count hi */
        CHECK(((secondStatus << 8) | firstStatus) <= 0x1000,
              "readback: ...and the latched COUNT follows it");

        /* Mode 6 must read back as 110, un-normalised. */
        portValue = 0x3C;
        VddBusIo(&bus, 0x43, 1, 0, &portValue);   /* ch0 lo/hi mode 6 */
        portValue = 0x00;
        VddBusIo(&bus, 0x40, 1, 0, &portValue);
        portValue = 0x10;
        VddBusIo(&bus, 0x40, 1, 0, &portValue);
        portValue = 0xE2;
        VddBusIo(&bus, 0x43, 1, 0, &portValue);
        firstStatus = 0;
        VddBusIo(&bus, 0x40, 1, 1, &firstStatus);
        CHECK((firstStatus & 0x0E) == 0x0C,
              "readback: mode 6 reads back as 110, NOT normalised to 010");

        /* NULL COUNT: set once a control word is written, cleared when the
         * count reaches the counting element.
         */
        portValue = 0x34;
        VddBusIo(&bus, 0x43, 1, 0, &portValue);   /* CW, no count yet */
        portValue = 0xE2;
        VddBusIo(&bus, 0x43, 1, 0, &portValue);
        firstStatus = 0;
        VddBusIo(&bus, 0x40, 1, 1, &firstStatus);
        CHECK((firstStatus & 0x40) != 0, "readback: null count SET after a control word");
        portValue = 0x00;
        VddBusIo(&bus, 0x40, 1, 0, &portValue);
        portValue = 0x10;
        VddBusIo(&bus, 0x40, 1, 0, &portValue);   /* now it loads */
        portValue = 0xE2;
        VddBusIo(&bus, 0x43, 1, 0, &portValue);
        firstStatus = 0;
        VddBusIo(&bus, 0x40, 1, 1, &firstStatus);
        CHECK((firstStatus & 0x40) == 0, "readback: null count CLEARED once the count loads");

        /* Counter 2 through the same command, selected by bit 3. */
        portValue = 0xB0;
        VddBusIo(&bus, 0x43, 1, 0, &portValue);   /* ch2 lo/hi mode 0 */
        portValue = 0x00;
        VddBusIo(&bus, 0x42, 1, 0, &portValue);
        portValue = 0x80;
        VddBusIo(&bus, 0x42, 1, 0, &portValue);
        portValue = 0xE8;
        VddBusIo(&bus, 0x43, 1, 0, &portValue);   /* rb: status only, ch2 */
        firstStatus = 0;
        VddBusIo(&bus, 0x42, 1, 1, &firstStatus);
        CHECK((firstStatus & 0x3F) == 0x30,
              "readback: counter 2 status reports lo/hi + mode 0 + binary");

        /* Leave counter 0 as the BIOS would. */
        portValue = 0x34;
        VddBusIo(&bus, 0x43, 1, 0, &portValue);
        portValue = 0x00;
        VddBusIo(&bus, 0x40, 1, 0, &portValue);
        portValue = 0x00;
        VddBusIo(&bus, 0x40, 1, 0, &portValue);
    }

    /* T11: COUNTER 2'S OUT PIN, AND ITS GATE. --------------------------------
     * docs/ref/pit.md 2 and 5. Counter 2 is the only counter whose OUT a PC
     * exposes directly -- port 61h bit 5 -- and the only one whose GATE software
     * controls, at port 61h bit 0. Together they are the classic "measure time
     * without interrupts" idiom: program a count, poll bit 5, count the loops.
     * All three oracles agree the bit must move (p_pit pit.61h.bit5.toggles = 1
     * on 6.22, dosbox-x AND PCem); we alone return a constant.
     */
    {
        UINT32 portValue;
        UINT firstOut;
        UINT secondOut;
        VddPitCounter2Gate(&pit, 1);                      /* gate high: counting */
        portValue = 0xB6;
        VddBusIo(&bus, 0x43, 1, 0, &portValue);     /* ch2 lo/hi mode 3 */
        portValue = 0x40;
        VddBusIo(&bus, 0x42, 1, 0, &portValue);
        portValue = 0x00;
        VddBusIo(&bus, 0x42, 1, 0, &portValue);     /* reload 0x0040 */

        /* Mode 3 is a square wave: OUT high for the first half of the period,
         * low for the second. R = 64, so the half is 32.
         */
        pit.TotalClocks = pit.Counter2.LoadClocks;
        CHECK(VddPitCounter2Out(&pit) == 1, "ch2 OUT is high at the start of the period");
        pit.TotalClocks = pit.Counter2.LoadClocks + 0x30;   /* 48 of 64: past half */
        CHECK(VddPitCounter2Out(&pit) == 0, "ch2 OUT is low in the second half");
        pit.TotalClocks = pit.Counter2.LoadClocks + 0x40;   /* a full period on */
        CHECK(VddPitCounter2Out(&pit) == 1, "ch2 OUT is high again a period later");

        /* THE GATE STOPS THE COUNTER. Clearing 61h bit 0 must freeze it; setting
         * the bit must resume from where it stopped, not restart.
         */
        firstOut = PitTestLatchedCounter2(&bus);
        VddPitCounter2Gate(&pit, 0);
        pit.TotalClocks += 1000;
        secondOut = PitTestLatchedCounter2(&bus);
        CHECK(firstOut == secondOut, "gate LOW freezes counter 2");
        VddPitCounter2Gate(&pit, 1);
        pit.TotalClocks += 8;
        CHECK(PitTestLatchedCounter2(&bus) != secondOut, "gate HIGH resumes counter 2");
    }

    /* T12: BCD COUNTING -- CONTROL WORD BIT 0. -------------------------------
     * docs/ref/pit.md 3. Four decade counters: the sequence is 9999 -> 0000 and
     * a written count of 0000 means 10000, not 65536.
     *
     * [WARNING]: THIS IS THE ONE SURFACE HERE WHERE THE DATASHEET OUTRANKS THE ORACLES.
     * p_pit's pit.bcd.valid says 6.22-under-QEMU = 0, PCem (real AMI BIOS) = 0,
     * dosbox-x = 1: two of the three emulators, including the one with genuine
     * firmware, do not model BCD AT ALL. There is nothing to diff against, so
     * these checks are written from Intel 231164-005 and are the only evidence
     * this behaviour has. They are deliberately EXACT VALUES, not properties:
     * a property test ("all nibbles <= 9") is what the DOS probe already asks,
     * and it is exactly the test a binary counter passes by luck one time in
     * six -- which it did, on this project, in this file's sibling.
     *
     * [CAUTION]: SO: if one of these ever fails, the datasheet is the referee. Do not
     * "correct" it towards an emulator that answers 0 to the question of
     * whether it implements the feature at all.
     */
    {
        UINT32 portValue;
        UINT value;
        INT index;
        INT isAllBcd;

        /* a read is BCD, digit by digit, including the decade borrow: */
        VddPitCounter2Gate(&pit, 1);
        portValue = 0xB1;
        VddBusIo(&bus, 0x43, 1, 0, &portValue);   /* ch2 lo/hi mode 0 BCD */
        portValue = 0x99;
        VddBusIo(&bus, 0x42, 1, 0, &portValue);
        portValue = 0x99;
        VddBusIo(&bus, 0x42, 1, 0, &portValue);   /* count 9999 */
        CHECK(pit.Counter2.Reload == 9999,
              "bcd: the written 0x9999 is DECODED to 9999, not stored as 39321");
        pit.TotalClocks = pit.Counter2.LoadClocks;
        CHECK(PitTestLatchedCounter2(&bus) == 0x9999, "bcd: reads back 0x9999 at load");
        pit.TotalClocks = pit.Counter2.LoadClocks + 1;
        CHECK(PitTestLatchedCounter2(&bus) == 0x9998, "bcd: one clock -> 0x9998");
        pit.TotalClocks = pit.Counter2.LoadClocks + 10;
        CHECK(PitTestLatchedCounter2(&bus) == 0x9989,
              "bcd: ten clocks -> 0x9989 (the units decade borrows, it does not go to 0x8F)");
        pit.TotalClocks = pit.Counter2.LoadClocks + 9999;
        CHECK(PitTestLatchedCounter2(&bus) == 0x0000, "bcd: 9999 clocks -> 0x0000");
        pit.TotalClocks = pit.Counter2.LoadClocks + 10000;
        CHECK(PitTestLatchedCounter2(&bus) == 0x9999,
              "bcd mode 0: past terminal count it wraps to 9999, NOT through 0xFFFF");

        /* The DOS probe's own question, asked here where it is cheap to spread:
         * no sample of a BCD count may contain a nibble above 9.
         */
        isAllBcd = 1;

        for (index = 0; index < 400; ++index)
        {
            pit.TotalClocks = pit.Counter2.LoadClocks + (UINT64)index * 37u;
            value = PitTestLatchedCounter2(&bus);

            if ((value & 0xF) > 9 || ((value >> 4) & 0xF) > 9
             || ((value >> 8) & 0xF) > 9 || ((value >> 12) & 0xF) > 9)
                isAllBcd = 0;
        }

        CHECK(isAllBcd, "bcd: 400 spread samples, every nibble a decimal digit");

        /* and the NEGATIVE control, without which the above proves nothing.
         * The same counter in BINARY must fail that test, or "all nibbles are
         * digits" is being satisfied by something other than BCD.
         */
        portValue = 0xB0;
        VddBusIo(&bus, 0x43, 1, 0, &portValue);   /* ch2 lo/hi mode 0 BIN */
        portValue = 0x99;
        VddBusIo(&bus, 0x42, 1, 0, &portValue);
        portValue = 0x99;
        VddBusIo(&bus, 0x42, 1, 0, &portValue);
        isAllBcd = 1;

        for (index = 0; index < 400; ++index)
        {
            pit.TotalClocks = pit.Counter2.LoadClocks + (UINT64)index * 37u;
            value = PitTestLatchedCounter2(&bus);

            if ((value & 0xF) > 9 || ((value >> 4) & 0xF) > 9
             || ((value >> 8) & 0xF) > 9 || ((value >> 12) & 0xF) > 9)
                isAllBcd = 0;
        }

        CHECK(!isAllBcd, "bcd: the SAME counter in binary walks through non-decimal nibbles");

        /* a count of zero is 10000 in BCD, and it is the IRQ0 divisor: */
        portValue = 0x35;
        VddBusIo(&bus, 0x43, 1, 0, &portValue);   /* ch0 lo/hi mode 2 BCD */
        portValue = 0x00;
        VddBusIo(&bus, 0x40, 1, 0, &portValue);
        portValue = 0x00;
        VddBusIo(&bus, 0x40, 1, 0, &portValue);   /* count 0000 = 10000 */
        CHECK(VddPitEffectiveReload(&pit) == 10000,
              "bcd: a written count of 0000 is 10000, not 65536");
        g_Irq0Count = 0;
        VddPitAddClocks(&pit, 9999);
        CHECK(g_Irq0Count == 0, "bcd: 9999 clocks is one short of the period");
        VddPitAddClocks(&pit, 1);
        CHECK(g_Irq0Count == 1, "bcd: the 10000th clock raises IRQ0");

        /* the status byte reports the base it was programmed with: */
        portValue = 0xE2;
        VddBusIo(&bus, 0x43, 1, 0, &portValue);   /* read-back, status, ch0 */
        value = 0;
        VddBusIo(&bus, 0x40, 1, 1, &value);
        CHECK((value & 0x3F) == 0x35, "bcd: read-back status reports lo/hi + mode 2 + BCD");

        /* and the speaker's divisor is decoded too, or the tone is wrong: */
        portValue = 0xB6;
        VddBusIo(&bus, 0x43, 1, 0, &portValue);   /* ch2 lo/hi mode 3 BIN */
        portValue = 0x00;
        VddBusIo(&bus, 0x42, 1, 0, &portValue);
        portValue = 0x10;
        VddBusIo(&bus, 0x42, 1, 0, &portValue);   /* 0x1000 binary = 4096 */
        CHECK(VddPitCounter2Hz(&pit) == PIT_INPUT_HZ / 4096u, "bcd: binary tone divisor is 4096");
        portValue = 0xB7;
        VddBusIo(&bus, 0x43, 1, 0, &portValue);   /* ch2 lo/hi mode 3 BCD */
        portValue = 0x00;
        VddBusIo(&bus, 0x42, 1, 0, &portValue);
        portValue = 0x10;
        VddBusIo(&bus, 0x42, 1, 0, &portValue);   /* 0x1000 BCD = 1000 */
        CHECK(VddPitCounter2Hz(&pit) == PIT_INPUT_HZ / 1000u,
              "bcd: the SAME bytes in BCD are a divisor of 1000, so the tone differs");

        /* #256: the speaker divisor is not read-modify-written: */
        portValue = 0xB6;
        VddBusIo(&bus, 0x43, 1, 0, &portValue);   /* ch2 lo/hi, binary */
        portValue = 0x34;
        VddBusIo(&bus, 0x42, 1, 0, &portValue);
        CHECK(VddPitCounter2Hz(&pit) == PIT_INPUT_HZ / 4096u,
              "ch2 lo/hi: the LSB alone does not re-tune (still 0x1000)");
        portValue = 0x12;
        VddBusIo(&bus, 0x42, 1, 0, &portValue);
        CHECK(pit.Counter2Reload == 0x1234, "ch2 lo/hi: LSB+MSB commit 0x1234 at once");
        portValue = 0x96;
        VddBusIo(&bus, 0x43, 1, 0, &portValue);   /* ch2 LSB only, mode 3 */
        portValue = 0x80;
        VddBusIo(&bus, 0x42, 1, 0, &portValue);
        CHECK(pit.Counter2Reload == 0x0080, "ch2 LSB-only: MSB becomes 0 (was 0x12)");
        portValue = 0xA6;
        VddBusIo(&bus, 0x43, 1, 0, &portValue);   /* ch2 MSB only, mode 3 */
        portValue = 0x05;
        VddBusIo(&bus, 0x42, 1, 0, &portValue);
        CHECK(pit.Counter2Reload == 0x0500, "ch2 MSB-only: LSB becomes 0 (was 0x80)");

        /* Leave counter 0 as the BIOS would, binary, for anything after this. */
        portValue = 0x34;
        VddBusIo(&bus, 0x43, 1, 0, &portValue);
        portValue = 0x00;
        VddBusIo(&bus, 0x40, 1, 0, &portValue);
        portValue = 0x00;
        VddBusIo(&bus, 0x40, 1, 0, &portValue);
    }

    /* #175: THE GATE AS A TRIGGER (docs/ref/pit.md section 5; p_pit section H): */
    {   UINT32 portValue;
        UINT firstOut;
        UINT secondOut;
        VddPitCounter2Gate(&pit, 0);
        portValue = 0xB2;
        VddBusIo(&bus, 0x43, 1, 0, &portValue);          /* ch2 lo/hi mode 1 */
        portValue = 0x00;
        VddBusIo(&bus, 0x42, 1, 0, &portValue);
        portValue = 0x80;
        VddBusIo(&bus, 0x42, 1, 0, &portValue);          /* 0x8000 */
        VddPitAddClocks(&pit, 100);
        CHECK(VddPitCounter2Out(&pit) == 1, "mode 1: OUT high until a GATE trigger");
        VddPitCounter2Gate(&pit, 1);                           /* rising edge */
        VddPitAddClocks(&pit, 10);
        CHECK(VddPitCounter2Out(&pit) == 0, "mode 1: triggered -- OUT low while counting");
        firstOut = PitTestLatchedCounter2(&bus);
        VddPitCounter2Gate(&pit, 0);                           /* level low: ignored */
        VddPitAddClocks(&pit, 50);
        secondOut = PitTestLatchedCounter2(&bus);
        CHECK(firstOut - secondOut == 50, "mode 1: the count runs on with GATE low (a trigger, not an enable)");
        VddPitAddClocks(&pit, 0x8000);
        CHECK(VddPitCounter2Out(&pit) == 1, "mode 1: OUT high again at terminal count");

        portValue = 0xB4;
        VddBusIo(&bus, 0x43, 1, 0, &portValue);          /* ch2 lo/hi mode 2 */
        portValue = 0x00;
        VddBusIo(&bus, 0x42, 1, 0, &portValue);
        portValue = 0xF0;
        VddBusIo(&bus, 0x42, 1, 0, &portValue);          /* 0xF000 */
        VddPitCounter2Gate(&pit, 1);
        VddPitAddClocks(&pit, 0x4000);
        firstOut = PitTestLatchedCounter2(&bus);
        VddPitCounter2Gate(&pit, 0);
        CHECK(VddPitCounter2Out(&pit) == 1, "mode 2: GATE low forces OUT high");
        VddPitCounter2Gate(&pit, 1);                           /* rising: reload */
        secondOut = PitTestLatchedCounter2(&bus);
        CHECK(firstOut == 0xB000 && secondOut == 0xF000,
              "mode 2: a GATE rising edge RELOADS the count (all three oracles agree)");

        portValue = 0xB8;
        VddBusIo(&bus, 0x43, 1, 0, &portValue);          /* ch2 lo/hi mode 4 */
        portValue = 0x10;
        VddBusIo(&bus, 0x42, 1, 0, &portValue);
        portValue = 0x00;
        VddBusIo(&bus, 0x42, 1, 0, &portValue);          /* 0x0010 */
        VddPitAddClocks(&pit, 0x10);
        CHECK(VddPitCounter2Out(&pit) == 0, "mode 4: the one-clock strobe at terminal count");
        VddPitAddClocks(&pit, 0x10);
        firstOut = PitTestLatchedCounter2(&bus);
        CHECK(firstOut == 0xFFF0 && VddPitCounter2Out(&pit) == 1,
              "mode 4: a one-shot -- past TC it runs on through FFFFh, no reload, no 2nd strobe");
    }

    /* T_WAIT: THE "TIME IT WITH COUNTER 2" IDIOM, TWICE (#175, s83). Gate low, mode 0,
     * count 0xFFFF, gate high, poll 61h bit 5 until OUT rises -- then do it again. The
     * gate-low edge froze the elapsed count at "past terminal count", and a new count did
     * not clear it, so the SECOND wait resumed from the old count and returned at once:
     * p_pit0's 220 ms waits collapsed to one 55 ms, and the probe saw no IRQ0s at all.
     */
    {
        UINT32 portValue;
        INT round;
        unsigned long steps[2];

        for (round = 0; round < 2; ++round)
        {
            VddPitCounter2Gate(&pit, 0);
            portValue = 0xB0;
            VddBusIo(&bus, 0x43, 1, 0, &portValue);          /* ch2 lo/hi mode 0 */
            portValue = 0xFF;
            VddBusIo(&bus, 0x42, 1, 0, &portValue);
            portValue = 0xFF;
            VddBusIo(&bus, 0x42, 1, 0, &portValue);          /* 0xFFFF */
            VddPitCounter2Gate(&pit, 1);
            steps[round] = 0;

            while (!VddPitCounter2Out(&pit) && steps[round] < 100000ul)
            {
                VddPitAddClocks(&pit, 64);
                ++steps[round];
            }
        }

        CHECK(steps[0] >= 1020 && steps[0] <= 1025, "counter 2 mode 0: a 0xFFFF wait takes 65535 clocks");
        CHECK(steps[1] == steps[0], "counter 2 mode 0: the SECOND wait is as long -- a new count clears the gate-frozen elapsed");
    }

    /* T_IRQ0: IRQ0 IS COUNTER 0's OUT PIN, and the PIC counts its RISING EDGES (#175).
     * Measured by tests/probes/dos/p_pit0.asm on QEMU, DOSBox-X and PCem, and the Intel 8254
     * datasheet (231164-005) where they split:
     *   mode 2        a pulse every period -> one IRQ0 per period (all agree)
     *   mode 0        OUT low at the CW, high at TC, stays high -> ONE (all agree)
     *   mode 0 bare   a new count after TC, no CW -> ONE more (all agree; datasheet silent)
     *   mode 4        one-clock strobe at TC -> ONE (all agree)
     *   mode 1 / 5    wait for a GATE rising edge; counter 0's gate is tied high and never
     *                 rises -> NONE (datasheet; oracles split, see oracle-rules.json)
     */
    {
        UINT32 portValue;
        static const struct
        {
            BYTE ControlWord;
            INT ExpectedIrqs;
            PCSTR Description;
        }
        modes[] = {
            { 0x34, 64, "mode 2: IRQ0 every period (64 periods -> 64)" },
            { 0x30,  1, "mode 0: ONE IRQ0, at terminal count" },
            { 0x38,  1, "mode 4: ONE IRQ0, at the strobe" },
            { 0x32,  0, "mode 1: NO IRQ0 -- counter 0's GATE never rises" },
            { 0x3A,  0, "mode 5: NO IRQ0 -- counter 0's GATE never rises" },
        };
        UINT index;

        for (index = 0; index < sizeof modes / sizeof modes[0]; ++index)
        {
            portValue = modes[index].ControlWord;
            VddBusIo(&bus, 0x43, 1, 0, &portValue);
            portValue = 0x00;
            VddBusIo(&bus, 0x40, 1, 0, &portValue);
            portValue = 0x10;
            VddBusIo(&bus, 0x40, 1, 0, &portValue);       /* 0x1000 */
            g_Irq0Count = 0;
            VddPitAddClocks(&pit, 64u * 0x1000u);
            CHECK(g_Irq0Count == modes[index].ExpectedIrqs, modes[index].Description);

            if (modes[index].ControlWord == 0x30)                                 /* bare rewrite */
            {
                portValue = 0x00;
                VddBusIo(&bus, 0x40, 1, 0, &portValue);
                portValue = 0x10;
                VddBusIo(&bus, 0x40, 1, 0, &portValue);
                g_Irq0Count = 0;
                VddPitAddClocks(&pit, 0x0FFF);
                CHECK(g_Irq0Count == 0, "mode 0 bare rewrite: nothing before the new terminal count");
                VddPitAddClocks(&pit, 64u * 0x1000u);
                CHECK(g_Irq0Count == 1, "mode 0 bare rewrite after TC: ONE more IRQ0 (all three oracles)");
            }
        }

        /* The BIOS's own mode comes back periodic: a one-shot must not leave it latched. */
        portValue = 0x34;
        VddBusIo(&bus, 0x43, 1, 0, &portValue);
        portValue = 0x00;
        VddBusIo(&bus, 0x40, 1, 0, &portValue);
        portValue = 0x10;
        VddBusIo(&bus, 0x40, 1, 0, &portValue);
        g_Irq0Count = 0;
        VddPitAddClocks(&pit, 4u * 0x1000u);
        CHECK(g_Irq0Count == 4, "back to mode 2 after the one-shots: periodic again");
    }

    printf("\n%d checks, %d failed\n", g_Total, g_Failures);
    return g_Failures ? 1 : 0;
}
