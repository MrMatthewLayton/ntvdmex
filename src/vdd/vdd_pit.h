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

/* A wall-clock reading, in ordinary binary -- INT 1Ah converts to BCD at the edge. */
struct vdd_rtc { unsigned cent, year, month, day, hour, min, sec; };

/* ── COUNTERS 1 AND 2, AS COUNTERS. ──────────────────────────────────────────
   Counter 0 keeps its own flat fields below and is DELIBERATELY not folded in
   here. It carries the IRQ0 engine, the accumulator and the pacer's guard, it is
   the hottest path in the device, and s61 measured what happens when its timing
   is disturbed. A refactor that moved it would be a second change riding along
   with this one; the duplication is the cheaper risk.

   What these two need is only what a guest can OBSERVE: a reload, an access mode
   and its read/write phases, a latch, and the moment the count was loaded --
   because a count read is computed from elapsed clocks, not stored. */
typedef struct {
    uint16_t reload;        /* 0 => 65536 effective                             */
    uint8_t  access;        /* 1=lo, 2=hi, 3=lo/hi                              */
    uint8_t  mode;          /* EFFECTIVE mode 0-5 (6/7 normalised)              */
    uint8_t  mode_raw;      /* as programmed -- for the Read-Back status byte    */
    uint8_t  bcd;           /* control word bit 0                               */
    uint8_t  wr_flip;       /* lo/hi write phase                                */
    uint8_t  wr_lo;         /* LSB buffered in lo/hi mode                       */
    uint8_t  rd_flip;       /* lo/hi read phase                                 */
    uint8_t  latched;       /* a snapshot is frozen for reading                 */
    uint16_t latch;         /* ...that snapshot                                 */
    uint64_t load_clocks;   /* total_clocks when the count was last loaded      */
    uint8_t  null_cnt;      /* a count is written but not yet in the counting element */
    uint8_t  gate;          /* GATE input: 1 = counting. Counter 2's comes from
                               port 61h bit 0; counters 0 and 1 are tied high on a
                               PC and never use this.                            */
    uint64_t gate_elapsed;  /* clocks counted when the gate last went LOW, so a
                               gate that comes back high RESUMES rather than
                               restarts -- which is the difference between a
                               paused stopwatch and a reset one.                 */
} pit_chan;

typedef struct pit_state {
    vdd_bus *bus;
    uint16_t reload;        /* channel-0 reload latch (0 => 65536 effective)    */
    uint8_t  access;        /* access mode: 1=lo, 2=hi, 3=lo/hi                  */
    uint8_t  mode;          /* EFFECTIVE mode 0-5 (shapes the count read-back)   */
    uint8_t  mode_raw;      /* the three bits AS PROGRAMMED. Modes 6 and 7 do not
                               exist -- 110 IS mode 2 and 111 IS mode 3 -- so `mode`
                               is normalised for BEHAVIOUR. But MEASURED on a real
                               8254 (tools/dostest/p_pit.asm, pit.mode6.readback =
                               0x0C): the Read-Back status byte reports the bits the
                               guest WROTE, un-normalised. Keeping both is what lets
                               the behaviour be right and the read-back be honest. */
    uint8_t  wr_flip;       /* lo/hi write phase (0 => lo next)                 */
    uint8_t  wr_lo;         /* the LSB written so far in lo/hi mode -- see pit_out */
    uint8_t  rd_flip;       /* lo/hi read phase                                 */
    uint8_t  latched;       /* a count snapshot is latched for reading          */
    uint16_t latch;         /* the latched count                                */
    uint64_t total_clocks;  /* monotonic PIT input clocks (for count reads)     */
    uint64_t load_clocks;   /* total_clocks when the count was last loaded: the
                               counting element runs from HERE (see pit_current_count) */
    uint64_t accum;         /* clocks not yet turned into IRQ0 pulses           */
    uint8_t  cw_armed;      /* a Control Word was written since the last load: the
                               next count write LOADS and RESTARTS the period, in every
                               mode (8254: "synchronized by software"). See pit_load. */
    uint8_t  next_pending;  /* modes 2/3, count written WITHOUT a Control Word: it is
                               held here and loaded at the end of the current period  */
    uint16_t next_reload;   /* ...that held count                                */
    uint32_t restarts;      /* loads that RESTARTED the period (CW+count / one-shot
                               modes); the host watches it -- see host_pit_resync_check */
    uint32_t frame_us;      /* microseconds per bus frame tick                  */
    uint16_t ch2_reload;    /* channel-2 reload (the PC-speaker tone divisor)   */
    uint8_t  ch2_access;    /* channel-2 access mode (1=lo, 2=hi, 3=lo/hi)      */
    uint8_t  ch2_wr_flip;   /* channel-2 lo/hi write phase                      */
    /* ⚠ ch2_reload/ch2_access/ch2_wr_flip above are the SPEAKER's view and stay
         authoritative for pit_ch2_hz(); c2 below mirrors them and adds what a
         COUNTER needs. Two views of one counter is not lovely, but rewiring the
         audio path is a separate change from making the port readable. */
    uint8_t  bcd;           /* counter 0's control-word BCD bit (read-back)     */
    uint8_t  st_latched[3]; /* a Read-Back status byte is latched for this counter */
    uint8_t  st_latch[3];   /* ...that byte                                     */
    pit_chan c1;            /* counter 1 -- DRAM refresh, free-running          */
    pit_chan c2;            /* counter 2 -- the PC speaker, as a counter        */
    /* ── HOST SERIALIZATION HOOK (may be NULL, e.g. in the off-VM tests). ─────────
       A real 8254 counts on its own crystal, in parallel with the CPU; this model
       only counts when a thread runs its code, and s61 measured what happens when
       that thread has to queue behind the video renderer for the DEVICE lock: the
       clock stops, then lurches (86% of a played session's timing stalls). So the
       host drives vdd_pit_add_clocks from a pacer thread under a PIT-ONLY lock --
       and these handlers, which arrive under the DEVICE lock, must take that same
       PIT lock or a guest reprogramming the reload races the pacer mid-count.
       The hook keeps this file pure C: enter=1 before counter state, enter=0 after. */
    void   (*guard)(void *ctx, int enter);
    void    *guard_ctx;
    /* ── THE REAL-TIME CLOCK BEHIND INT 1Ah AH=02h/04h. ──────────────────────────
         Injectable so the battery can pin the exact BCD a known instant produces;
         NULL means the C library clock, which is what the host uses. The PIT owns
         these because it already owns INT 1Ah (the tick half of the same service). */
    void   (*rtc_now)(void *ctx, struct vdd_rtc *out);
    void    *rtc_ctx;
} pit_state;

/* effective reload (0 means 65536 on the 8254). */
static inline uint32_t pit_eff_reload(const pit_state *st)
{ return st->reload ? st->reload : 0x10000u; }

/* Channel-2 output frequency in Hz (the PC-speaker tone); 0 if not programmed. */
static inline uint32_t pit_ch2_hz(const pit_state *st)
{ uint32_t r = st->ch2_reload ? st->ch2_reload : 0x10000u; return PIT_INPUT_HZ / r; }

/* Build the device descriptor to hand to vdd_bus_add(). */
int  vdd_pit_init(vdd_bus *b, void *self);
void vdd_pit_reset(void *self);
static inline ntvdd vdd_pit_device(pit_state *st)
{ ntvdd d; d.name = "pit"; d.init = vdd_pit_init; d.reset = vdd_pit_reset;
  d.shutdown = 0; d.self = st; return d; }

/* Advance time by `clocks` PIT input clocks, emitting IRQ0 per elapsed reload.
   Exposed (not just driven by the frame tick) so tests can feed exact counts. */
void vdd_pit_add_clocks(pit_state *st, uint32_t clocks);

/* ── COUNTER 2'S GATE AND OUT PIN -- the PC's only software-visible pair. ─────
   The speaker VDD owns port 61h, so it pushes bit 0 in here and reads bit 5 back
   out. Keeping the PIT ignorant of the speaker (rather than having it reach for
   port 61h itself) is what lets the off-VM battery drive the gate directly. */
void vdd_pit_ch2_gate(pit_state *st, int on);
int  vdd_pit_ch2_out(const pit_state *st);

#endif /* NTVDMEX_VDD_PIT_H */
