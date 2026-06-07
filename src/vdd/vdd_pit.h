/*
 * vdd_pit.h -- the PIT (8254) timer VDD.  (M3 slice-2, ADR-0008)
 *
 * The simplest device: it validates the bus end-to-end before the heavy video
 * work.  It models Intel 8254 channel 0 (ports 0x40-0x43), converts elapsed
 * wall-clock (delivered as the bus frame tick) into IRQ0 pulses at the
 * programmed rate (~18.2065 Hz by default), and provides the BIOS timer
 * services INT 08h (tick bookkeeping at 0040:006C) and INT 1Ah (time-of-day).
 *
 * Pure C, no <windows.h>: all device state is explicit and the only outside
 * effects go through the bus (raise_irq, map_flat), so the whole VDD is
 * exercised off-VM by tools/dostest/pit_test.c.
 */
#ifndef NTVDMEX_VDD_PIT_H
#define NTVDMEX_VDD_PIT_H

#include "vdd_bus.h"

#define PIT_INPUT_HZ     1193182u      /* 8254 input clock                      */
#define PIT_TICKS_PER_DAY 0x1800B0u    /* 1,573,040 INT 8 ticks / 24h (BIOS)    */
#define PIT_DEFAULT_FRAME_US 16667u    /* ~60 Hz host frame tick                */

typedef struct pit_state {
    vdd_bus *bus;
    uint16_t reload;        /* channel-0 reload latch (0 => 65536 effective)    */
    uint8_t  access;        /* access mode: 1=lo, 2=hi, 3=lo/hi                  */
    uint8_t  mode;          /* operating mode 0-5 (informational)               */
    uint8_t  wr_flip;       /* lo/hi write phase (0 => lo next)                 */
    uint8_t  rd_flip;       /* lo/hi read phase                                 */
    uint8_t  latched;       /* a count snapshot is latched for reading          */
    uint16_t latch;         /* the latched count                                */
    uint64_t total_clocks;  /* monotonic PIT input clocks (for count reads)     */
    uint64_t accum;         /* clocks not yet turned into IRQ0 pulses           */
    uint32_t frame_us;      /* microseconds per bus frame tick                  */
} pit_state;

/* effective reload (0 means 65536 on the 8254). */
static inline uint32_t pit_eff_reload(const pit_state *st)
{ return st->reload ? st->reload : 0x10000u; }

/* Build the device descriptor to hand to vdd_bus_add(). */
int  vdd_pit_init(vdd_bus *b, void *self);
void vdd_pit_reset(void *self);
static inline ntvdd vdd_pit_device(pit_state *st)
{ ntvdd d; d.name = "pit"; d.init = vdd_pit_init; d.reset = vdd_pit_reset;
  d.shutdown = 0; d.self = st; return d; }

/* Advance time by `clocks` PIT input clocks, emitting IRQ0 per elapsed reload.
   Exposed (not just driven by the frame tick) so tests can feed exact counts. */
void vdd_pit_add_clocks(pit_state *st, uint32_t clocks);

#endif /* NTVDMEX_VDD_PIT_H */
