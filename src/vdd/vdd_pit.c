/* vdd_pit.c -- see vdd_pit.h.  Intel 8254 channel-0 model + BIOS INT 08h/1Ah,
 * on the VDD bus.  Pure C, no <windows.h>. */
#include "vdd_pit.h"

/* ── ★★★★ THE COUNT STARTS WHEN THE GUEST LOADS IT. ─────────────────────────────
     This used to be `R - (total_clocks % R)`: a free-running divider whose phase had
     nothing to do with the moment the guest wrote the count. For a guest that only
     ever waits for IRQ0 that is invisible. For one that READS THE COUNTER it is a
     random number generator.
   ★ MEASURED ON LEMMINGS (s69). Its "High Performance PC" option calibrates the game
     tick from the CRT: load counter 0 with 0xFFFF in mode 0, count 320 hblanks on
     0x3DA bit 0, latch, and use 0xFFFF - latch as the reload (guest CS:15BB..1643 in
     the real-DOS dump). The user's by-hand run got 0x4BB9 (61.5 Hz -- "the music is
     slower"), the headless rig 0xC40C (23.8 Hz), every run a different number,
     because the read-back was `total_clocks % 0xFFFF` at the latch: uniformly random.
   ► So keep the instant of the load (`load_clocks`) and derive the counting element
     from the clocks since it, per mode (Intel 8254 datasheet, 231164-005):
       mode 0   N - elapsed, on past terminal count through 0xFFFF (one-shot)
       mode 3   "decremented by two on succeeding CLK pulses" and reloaded at zero,
                so the count runs N, N-2, ... twice per period; never an odd LSB
       others   N - (elapsed mod N), reloading at the period (modes 2, 4, 5; mode 1)
   ⚠ s69 HISTORY: this model was shipped, blamed for a blank screen, and reverted. The
     blank screen was never the count -- it was IRQ0 re-entering the game's timer ISR
     (the host auto-EOI'd IRQ0; see irq0_ack in the host) once the tick was correct.
     Both halves are now fixed; see docs/STATE.md session 70. */
static int pit_out_pin(uint8_t mode, uint32_t R, uint64_t elapsed);

/* The count law, by mode. Split out of pit_current_count so counters 1 and 2 can
   use the same arithmetic instead of a second copy that drifts from it. */
static uint16_t pit_count_law(uint8_t mode, uint32_t R, uint64_t elapsed)
{
    if (mode == 0)
        return (uint16_t)(R - elapsed);      /* wraps through 0xFFFF past zero   */
    if (mode == 3) {
        uint32_t half  = (R + 1) / 2;
        uint32_t phase = (uint32_t)(elapsed % half);
        return (uint16_t)(R - 2 * phase);    /* R when phase==0 (65536 -> 0)     */
    }
    return (uint16_t)(R - (uint32_t)(elapsed % R));
}

static uint16_t pit_current_count(const pit_state *st)
{
    uint64_t elapsed = (st->total_clocks >= st->load_clocks)
                     ? st->total_clocks - st->load_clocks : 0;
    return pit_count_law(st->mode, pit_eff_reload(st), elapsed);
}

/* Counters 1 and 2. They have no IRQ engine and no accumulator: a read is simply
   "how far has the counting element got since it was loaded". */
/* Clocks this counter has actually counted. ⚠ NOT simply total - load: while the
   GATE is low the counting element is stopped, so the elapsed count freezes at
   whatever it had reached. */
static uint64_t chan_elapsed(const pit_state *st, const pit_chan *c)
{
    if (!c->gate) return c->gate_elapsed;
    return (st->total_clocks >= c->load_clocks) ? st->total_clocks - c->load_clocks : 0;
}

static uint16_t chan_count(const pit_state *st, const pit_chan *c)
{
    uint32_t R = c->reload ? (uint32_t)c->reload : 0x10000u;
    return pit_count_law(c->mode, R, chan_elapsed(st, c));
}

/* Port 61h bit 0 -> counter 2's GATE. A high-to-low edge freezes the elapsed
   count; a low-to-high edge slides load_clocks forward so counting RESUMES from
   there. Idempotent: writing the same level twice must not nudge the count,
   because a guest polling port 61h rewrites the whole byte constantly. */
void vdd_pit_ch2_gate(pit_state *st, int on)
{
    pit_chan *c = &st->c2;
    if (!!on == !!c->gate) return;
    if (on) { c->load_clocks = st->total_clocks - c->gate_elapsed; c->gate = 1; }
    else    { c->gate_elapsed = chan_elapsed(st, c);               c->gate = 0; }
}

/* Counter 2's OUT pin, as port 61h bit 5 reports it. */
int vdd_pit_ch2_out(const pit_state *st)
{
    const pit_chan *c = &st->c2;
    uint32_t R = c->reload ? (uint32_t)c->reload : 0x10000u;
    return pit_out_pin(c->mode, R, chan_elapsed(st, c));
}

/* A count write, lo/hi buffered exactly as counter 0 does it: a HALF-WRITTEN
   COUNT IS NOT A COUNT (see the note on port 0x40 below -- the same defect would
   otherwise be reintroduced here). */
static void chan_write_count(pit_state *st, pit_chan *c, uint8_t val)
{
    uint16_t v;
    if (c->access == 1)      v = val;                        /* lo only         */
    else if (c->access == 2) v = (uint16_t)(val << 8);       /* hi only         */
    else {                                                   /* lo then hi      */
        if (!c->wr_flip) { c->wr_lo = val; c->wr_flip = 1; return; }
        v = (uint16_t)((val << 8) | c->wr_lo);
        c->wr_flip = 0;
    }
    c->reload = v;
    c->null_cnt = 0;                 /* the count has reached the CE        */
    c->load_clocks = st->total_clocks;
}

/* A read, honouring the latch and the access mode -- the same contract as 0x40. */
static void chan_read_count(pit_state *st, pit_chan *c, uint32_t *val)
{
    uint16_t v = c->latched ? c->latch : chan_count(st, c);
    uint8_t acc = c->access ? c->access : 3;
    if (acc == 1)      { *val = v & 0xFF;        c->latched = 0; }
    else if (acc == 2) { *val = (v >> 8) & 0xFF; c->latched = 0; }
    else {
        if (!c->rd_flip) { *val = v & 0xFF; c->rd_flip = 1; }
        else { *val = (v >> 8) & 0xFF; c->rd_flip = 0; c->latched = 0; }
    }
}

/* One control word, applied to counter 1 or 2. */
static void chan_control(pit_state *st, pit_chan *c, uint8_t val)
{
    uint8_t acc = (uint8_t)((val >> 4) & 3);
    if (acc == 0) {                              /* Counter Latch Command       */
        c->latch = chan_count(st, c);
        c->latched = 1; c->rd_flip = 0;
        return;
    }
    c->access   = acc;
    c->mode_raw = (uint8_t)((val >> 1) & 7);
    c->mode     = (c->mode_raw >= 6) ? (uint8_t)(c->mode_raw - 4) : c->mode_raw;
    c->bcd      = (uint8_t)(val & 1);
    c->wr_flip  = 0;
    c->null_cnt = 1;                 /* CR written-to-be, CE not yet loaded */
    c->load_clocks = st->total_clocks;
}

/* --- the time engine: clocks -> IRQ0 pulses ------------------------------- */
void vdd_pit_add_clocks(pit_state *st, uint32_t clocks)
{
    uint32_t R = pit_eff_reload(st);
    int guard = 0;
    st->total_clocks += clocks;
    st->accum += clocks;
    while (st->accum >= R && guard++ < 100000) {
        st->accum -= R;
        vdd_raise_irq(st->bus, 0);           /* IRQ0 -> guest takes INT 08h     */
        /* A count written WITHOUT a Control Word in modes 2/3 is "loaded at the end
           of the current counting cycle" (datasheet, mode 2; mode 3 says half-cycle,
           approximated here as the full one). This is that end. */
        if (st->next_pending) {
            st->reload       = st->next_reload;
            st->next_pending = 0;
            st->load_clocks  = st->total_clocks - st->accum;   /* the period boundary */
            R = pit_eff_reload(st);
        }
    }
}

/* ── ★★★★★ WHEN A COUNT WRITE RESTARTS THE PERIOD -- FROM THE DATASHEET, NOT BELIEF. ──
     Intel 8254 (231164-005), the modes every DOS timer uses:
       Mode 2 / Mode 3: "After writing a Control Word and initial count, the Counter will
         be loaded on the next CLK pulse. This allows the Counter to be synchronized by
         software." -- i.e. Control Word THEN count = load now, period restarts.
       Mode 2 / Mode 3: "Writing a new count while counting does not affect the current
         counting sequence ... the new count will be loaded at the end of the current
         counting cycle." -- i.e. a BARE count write (no Control Word since the last
         load) leaves the period in flight alone and takes over at its end.
       Modes 0/1/4/5: "If a new count is written to the Counter, it will be loaded on the
         next CLK pulse and counting will continue from the new count." -- restart.
   ⛔⛔ THE s69 REGRESSION, BOTH WAYS. 59ee731 restarted on EVERY count write (wrong for a
     bare write in 2/3); 9eec369 "corrected" it to NEVER restart in 2/3 -- also wrong,
     because Lemmings writes `out 43h,36h` BEFORE its count on every tick, which is the
     "synchronized by software" case. Its game ISR runs at IRQ0, spins on 0x3DA for the
     vertical retrace, and THEN reprograms the count: the tick is meant to be locked to
     the retrace, with the IRQ landing ~320 lines after it. Never restarting made the IRQ
     free-run at ~98 Hz against a 70 Hz retrace, so the ISR was re-entered before it
     could finish and the game loop starved (the "stuck palette fade"). Each of the two
     wrong models had a test written to certify it. These are written from the quotes
     above; [[m9-completeness-programme]] rule: never from memory.
   ► `cw_armed` is set by a Control Word for this counter and cleared by the load it
     arms; `next_pending` parks a bare mode-2/3 count until vdd_pit_add_clocks reaches
     the end of the period. `load_clocks` moves only when the counting element really
     (re)starts, because that is what pit_current_count measures the read-back from. */
static void pit_load(pit_state *st, uint16_t count)
{
    int periodic = (st->mode == 2 || st->mode == 3);
    if (periodic && !st->cw_armed) {         /* bare write: takes over at period end */
        st->next_reload  = count;
        st->next_pending = 1;
        return;
    }
    st->reload       = count;                /* Control Word + count, or a one-shot mode */
    st->load_clocks  = st->total_clocks;
    st->accum        = 0;
    st->cw_armed     = 0;
    st->next_pending = 0;
    st->restarts++;
}

/* See pit_state.guard in the header: every touch of counter state from a caller
   the HOST does not already serialize against the pacer goes through this. */
#define PIT_GUARD(st, e) do { if ((st)->guard) (st)->guard((st)->guard_ctx, (e)); } while (0)

static void pit_frame(void *self)
{
    pit_state *st = (pit_state *)self;
    uint32_t clocks;
    /* The host that drives time from a pacer sets frame_us = 0; add_clocks(0) is
       a no-op ARITHMETICALLY but its u64 read-modify-writes still race the pacer
       on a 32-bit build, so do not touch the state at all. */
    if (!st->frame_us) return;
    clocks = (uint32_t)(((uint64_t)PIT_INPUT_HZ * st->frame_us) / 1000000u);
    PIT_GUARD(st, 1);
    vdd_pit_add_clocks(st, clocks);
    PIT_GUARD(st, 0);
}


/* ── THE OUT PIN, DERIVED RATHER THAN STORED. ────────────────────────────────
   OUT is a function of the mode and how far the count has got, so there is no
   state to keep in step -- and keeping it derived means it cannot go stale the
   way a cached flag would. docs/ref/pit.md 5 is the table this implements. */
static int pit_out_pin(uint8_t mode, uint32_t R, uint64_t elapsed)
{
    uint32_t phase;
    switch (mode) {
    case 0:                                  /* low while counting, high at TC  */
    case 1:
        return elapsed >= R;
    case 2:                                  /* high, low for ONE clock at 1    */
        phase = (uint32_t)(elapsed % R);
        return phase != (R - 1);
    case 3: {                                /* square wave: high half, low half */
        uint32_t half = (R + 1) / 2;
        phase = (uint32_t)(elapsed % R);
        return phase < half;
    }
    case 4:                                  /* high, one-clock strobe at TC    */
    case 5:
        return elapsed != R;
    default:
        return 1;
    }
}

/* Status byte: b7 OUT, b6 null count, b5:4 access, b3:1 mode, b0 BCD.
   ⚠ b3:1 IS mode_raw, NOT the normalised mode. Measured on a real 8254
     (p_pit.asm pit.mode6.readback = 0x0C): programming 110 reads back 110 even
     though the counter behaves as mode 2. */
static uint8_t pit_status_of(const pit_state *st, int n)
{
    uint8_t acc, mode_raw, bcd, null_cnt;
    uint32_t R; uint64_t elapsed; int out;
    if (n == 0) {
        acc = st->access ? st->access : 3; mode_raw = st->mode_raw; bcd = st->bcd;
        null_cnt = (uint8_t)(st->cw_armed || st->next_pending);
        R = pit_eff_reload(st);
        elapsed = (st->total_clocks >= st->load_clocks)
                ? st->total_clocks - st->load_clocks : 0;
        out = pit_out_pin(st->mode, R, elapsed);
    } else {
        const pit_chan *c = (n == 1) ? &st->c1 : &st->c2;
        acc = c->access ? c->access : 3; mode_raw = c->mode_raw; bcd = c->bcd;
        null_cnt = c->null_cnt;
        R = c->reload ? (uint32_t)c->reload : 0x10000u;
        elapsed = (st->total_clocks >= c->load_clocks)
                ? st->total_clocks - c->load_clocks : 0;
        out = pit_out_pin(c->mode, R, elapsed);
    }
    return (uint8_t)((out ? 0x80 : 0) | (null_cnt ? 0x40 : 0)
                     | ((acc & 3) << 4) | ((mode_raw & 7) << 1) | (bcd & 1));
}

/* ── THE READ-BACK COMMAND. ──────────────────────────────────────────────────
   ⚠ BOTH LATCH BITS ARE ACTIVE LOW: bit 5 = 0 latches the count, bit 4 = 0
     latches the status. Writing 1s asks for nothing, which is the natural-looking
     mistake. Bits 3/2/1 select counters 2/1/0, and one command may address
     several -- that is the whole point of having it. */
static void pit_readback(pit_state *st, uint8_t val)
{
    int n;
    for (n = 0; n < 3; ++n) {
        if (!(val & (1u << (n + 1)))) continue;          /* not selected        */
        if (!(val & 0x10)) {                             /* latch STATUS        */
            if (!st->st_latched[n]) {                    /* first latch wins    */
                st->st_latch[n] = pit_status_of(st, n);
                st->st_latched[n] = 1;
            }
        }
        if (!(val & 0x20)) {                             /* latch COUNT         */
            if (n == 0) {
                if (!st->latched) { st->latch = pit_current_count(st); st->latched = 1; st->rd_flip = 0; }
            } else {
                pit_chan *c = (n == 1) ? &st->c1 : &st->c2;
                if (!c->latched) { c->latch = chan_count(st, c); c->latched = 1; c->rd_flip = 0; }
            }
        }
    }
}

/* --- 8254 ports 0x40-0x43 ------------------------------------------------- */
static void pit_out_locked(pit_state *st, uint16_t port, uint8_t val)
{
    if (port == 0x43) {                      /* mode/command register           */
        uint8_t ch = (uint8_t)(val >> 6);
        uint8_t acc = (uint8_t)((val >> 4) & 3);
        if (ch == 2) {                       /* channel 2: speaker tone AND a counter */
            if (acc != 0) { st->ch2_access = acc; st->ch2_wr_flip = 0; }
            chan_control(st, &st->c2, val);  /* latch command included          */
            return;
        }
        if (ch == 1) { chan_control(st, &st->c1, val); return; }
        if (ch == 3) { pit_readback(st, val); return; }
        if (acc == 0) {                      /* latch count command             */
            st->latch = pit_current_count(st);
            st->latched = 1; st->rd_flip = 0;
        } else {
            /* A Control Word arms the next count write to load and restart (see
               pit_load) and clears the count register ("CRM and CRL are cleared when
               the Counter is programmed"), so a parked bare write is discarded. The
               counting element is NOT stopped: the datasheet halts counting on the first
               byte of a COUNT, not on the Control Word, and a guest that programs a
               mode and never writes a count (Lemmings' calibration exit: `out 43h,36h`
               then a read) must keep ticking at the old rate, as the real part does.
               ⚠ A latched count is deliberately KEPT across this: "(or until the
               Counter is reprogrammed)" would unlatch it, but the value the OL would
               then follow is the same CE a handful of clocks on, and Lemmings reads
               it right here -- keeping the latch is the same number without the
               mode-3 by-two arithmetic being applied to a mode-0 count. */
            /* ── MODES 6 AND 7 ARE NOT MODES. ─────────────────────────────────
                 The field is three bits wide but only six modes exist: 110 IS mode 2
                 and 111 IS mode 3 (docs/ref/pit.md 3). Storing the raw bits let every
                 later test -- `mode == 2` for the periodic bare-count load rule,
                 `mode == 3` for the decrement-by-two count law -- silently exclude a
                 guest that programmed the alias. Normalise for BEHAVIOUR here and
                 keep the raw bits for the read-back status byte, which a real 8254
                 reports UN-normalised (measured: pit.mode6.readback = 0x0C). */
            st->mode_raw = (uint8_t)((val >> 1) & 7);
            st->mode = (st->mode_raw >= 6) ? (uint8_t)(st->mode_raw - 4) : st->mode_raw;
            st->access = acc; st->bcd = (uint8_t)(val & 1); st->wr_flip = 0;
            st->cw_armed = 1; st->next_pending = 0;
        }
    } else if (port == 0x40) {               /* channel-0 reload write          */
        /* ── ★★★★ A HALF-WRITTEN COUNT IS NOT A COUNT. ──────────────────────────────
             This used to READ-MODIFY-WRITE `reload` on every byte, so between a guest's
             two `out 40h` instructions the divisor was (old MSB | new LSB) -- a value
             the guest never asked for and, when the two halves disagree, one that can
             be arbitrarily small.
           ★ MEASURED ON ZAR (GH #23): it programs 0x8002 (36.41 Hz, near enough 2x the
             BIOS rate) as lo=0x02 then hi=0x80, arriving with reload=0 (65536). Under
             the old rule the FIRST byte alone left `reload = (0 & 0xFF00) | 0x02` =
             **2, i.e. 596,591 Hz**, and it stayed there for the whole gap between the
             two port writes. That gap is not microseconds for us: each `out`
             is a trap out of protected mode and back, and host_pit_sync() runs on ports
             0x40-0x43, so the transient rate got sampled. The run's own counters show
             the damage exactly: `raises=29657` in 45 s against a programmed 36.4 Hz,
             i.e. ~29,000 interrupts the 8254 never generated, each costing a
             SuspendThread round trip under the device lock and saturating the owed
             backlog (`owed_max=64`). The tell was `PIT-RELOAD 0x2 (hz=0x91a6f)` sitting
             in the log with nothing else out of range.
           ► THE 8254 BUFFERS. Writing the LSB does not load the counter; the count
             register is loaded when the MSB arrives. So buffer it and commit once --
             the guest's two writes become ONE rate change, which is what the hardware
             does and what every guest is written against.
           ⚠ AND LSB-ONLY / MSB-ONLY ZERO THE OTHER HALF (Intel 8254 datasheet). That
             is the same read-modify-write mistake in a second dress -- a guest that
             re-rates a channel with a single MSB write would inherit whatever LSB
             happened to be standing. ZAR does not take this path (it uses lo/hi), so
             this half is fidelity, fixed on its own merits and not on its evidence.
           ⚠⚠ THE PIT IS THE MOST SHARED PATH IN THIS PROJECT -- see the note on
             host_irq_sink's throttle, where a change measured on Doom cost SKYROADS a
             fifth of its clock. Re-gate BOTH before believing this. */
        /* The committed count goes through pit_load, which decides (per mode and per
           whether a Control Word preceded it) between "load and restart now" and
           "take over at the end of the period". See the note above pit_load. */
        uint8_t acc = st->access ? st->access : 3;
        if (acc == 1)      pit_load(st, val);                        /* LSB only: MSB := 0 */
        else if (acc == 2) pit_load(st, (uint16_t)((uint16_t)val << 8));  /* MSB only: LSB := 0 */
        else {                               /* lo then hi -- ONE atomic load   */
            if (!st->wr_flip) { st->wr_lo = val; st->wr_flip = 1; }
            else { pit_load(st, (uint16_t)(((uint16_t)val << 8) | st->wr_lo)); st->wr_flip = 0; }
        }
    } else if (port == 0x42) {               /* channel-2 reload (speaker tone) */
        uint8_t acc = st->ch2_access ? st->ch2_access : 3;
        if (acc == 1) st->ch2_reload = (uint16_t)((st->ch2_reload & 0xFF00) | val);
        else if (acc == 2) st->ch2_reload = (uint16_t)((st->ch2_reload & 0x00FF) | ((uint16_t)val << 8));
        else {                               /* lo then hi                      */
            if (!st->ch2_wr_flip) { st->ch2_reload = (uint16_t)((st->ch2_reload & 0xFF00) | val); st->ch2_wr_flip = 1; }
            else { st->ch2_reload = (uint16_t)((st->ch2_reload & 0x00FF) | ((uint16_t)val << 8)); st->ch2_wr_flip = 0; }
        }
        /* ...and the same byte, into the COUNTER view. The speaker only ever
           wanted a divisor; a guest that programs counter 2 to MEASURE something
           needs the load moment recorded too. */
        st->c2.access = acc;
        chan_write_count(st, &st->c2, val);
    } else if (port == 0x41) {               /* counter 1 -- DRAM refresh       */
        chan_write_count(st, &st->c1, val);
    }
}

static void pit_out(void *self, uint16_t port, uint8_t w, uint32_t v)
{
    pit_state *st = (pit_state *)self;
    (void)w;
    PIT_GUARD(st, 1);
    pit_out_locked(st, port, (uint8_t)v);
    PIT_GUARD(st, 0);
}

static void pit_in_locked(pit_state *st, uint16_t port, uint32_t *val)
{
    uint16_t v; uint8_t acc;
    int n = (port == 0x40) ? 0 : (port == 0x41) ? 1 : (port == 0x42) ? 2 : -1;
    /* A LATCHED STATUS IS READ BEFORE ANYTHING ELSE, and consumed by that read.
       When read-back latched both, the status comes out first and the count
       after it (docs/ref/pit.md 4). */
    if (n >= 0 && st->st_latched[n]) {
        *val = st->st_latch[n]; st->st_latched[n] = 0; return;
    }
    if (port == 0x41) { chan_read_count(st, &st->c1, val); return; }
    if (port == 0x42) { chan_read_count(st, &st->c2, val); return; }
    /* 0x43 is write-only on the part; a read is undefined. Answer consistently
       rather than plausibly -- see docs/inventory/pit.md 1. */
    if (port != 0x40) { *val = 0xFF; return; }
    v = st->latched ? st->latch : pit_current_count(st);
    acc = st->access ? st->access : 3;
    if (acc == 1) { *val = v & 0xFF; st->latched = 0; }
    else if (acc == 2) { *val = (v >> 8) & 0xFF; st->latched = 0; }
    else {                                   /* lo then hi                      */
        if (!st->rd_flip) { *val = v & 0xFF; st->rd_flip = 1; }
        else { *val = (v >> 8) & 0xFF; st->rd_flip = 0; st->latched = 0; }
    }
}

static void pit_in(void *self, uint16_t port, uint8_t w, uint32_t *val)
{
    pit_state *st = (pit_state *)self;
    (void)w;
    PIT_GUARD(st, 1);
    pit_in_locked(st, port, val);
    PIT_GUARD(st, 0);
}

/* --- BIOS timer services -------------------------------------------------- */
/* The BIOS data area lives at segment 0x40; tick count = DWORD at 0040:006C,
   the 24-hour-rollover flag = BYTE at 0040:0070. */
static uint8_t *pit_bda(pit_state *st) { return (uint8_t *)vdd_map_flat(st->bus, 0x40, 0); }

/* INT 08h -- the BIOS timer tick (runs when the guest takes IRQ0). Increments
   the tick count and handles the midnight rollover.
   TODO(v86 wiring): the authentic path also chains INT 1Ch and EOIs the PIC;
   that needs the IVT/re-entry plumbing from slice-1b, so it is deferred. */
static void pit_int08(void *self, ntvdd_regs *r)
{
    pit_state *st = (pit_state *)self;
    uint8_t *bda = pit_bda(st);
    uint32_t *tick = (uint32_t *)(bda + 0x6C);
    (void)r;
    *tick += 1;
    if (*tick >= PIT_TICKS_PER_DAY) { *tick = 0; bda[0x70] = 1; }
}

/* INT 1Ah -- BIOS time-of-day. AH=00 get tick count (+ clear midnight flag),
   AH=01 set tick count. */
/* ── INT 1Ah's RTC HALF. ─────────────────────────────────────────────────────────
     AH=02h/04h fell into `default:` -- "RTC subfns not modelled yet" -- which leaves
     every register exactly as the caller passed it. Found by p_bios.asm: the probe
     poisons CX/DX and the poison came straight back (C1C1/D1D1), where the 6.22
     oracle answers with the time and date in BCD.
   ⚠ BCD IS THE CONTRACT. A guest that parses these as binary (every one of them --
     that is what the BIOS returns) reads garbage from a binary answer: 0x22 seconds
     is twenty-two, not thirty-four. */
static unsigned pit_bcd(unsigned v) { return ((v / 10) % 10) * 16 + (v % 10); }

/* The clock is the HOST's to supply -- this file stays free of <time.h>, which the
   XP-targeting CRT does not link anyway. No clock installed means the call is NOT
   answered, exactly as before: a fabricated date would be worse than no answer. */
static int pit_rtc(pit_state *st, struct vdd_rtc *out)
{
    if (!st->rtc_now) return 0;
    out->cent = 20; out->year = 0; out->month = 1; out->day = 1;
    out->hour = 0;  out->min = 0;  out->sec = 0;
    st->rtc_now(st->rtc_ctx, out);
    return 1;
}

static void pit_int1a(void *self, ntvdd_regs *r)
{
    pit_state *st = (pit_state *)self;
    uint8_t *bda = pit_bda(st);
    uint32_t *tick = (uint32_t *)(bda + 0x6C);
    switch (r_ah(r)) {
    case 0x00:
        s_cx(r, (uint16_t)(*tick >> 16));
        s_dx(r, (uint16_t)(*tick & 0xFFFF));
        s_al(r, bda[0x70]);
        bda[0x70] = 0;
        r->cf = 0;
        break;
    case 0x01:
        *tick = ((uint32_t)r_cx(r) << 16) | r_dx(r);
        bda[0x70] = 0;
        r->cf = 0;
        break;
    case 0x02: {                             /* get RTC time, BCD               */
        struct vdd_rtc n; if (!pit_rtc(st, &n)) break;
        s_cx(r, (uint16_t)((pit_bcd(n.hour) << 8) | pit_bcd(n.min)));
        /* DL = daylight-saving flag. 0 = standard time; we do not track a DST rule
           the guest could act on, and saying 1 would invite one. */
        s_dx(r, (uint16_t)(pit_bcd(n.sec) << 8));
        r->cf = 0;
        break; }
    case 0x04: {                             /* get RTC date, BCD               */
        struct vdd_rtc n; if (!pit_rtc(st, &n)) break;
        s_cx(r, (uint16_t)((pit_bcd(n.cent) << 8) | pit_bcd(n.year)));
        s_dx(r, (uint16_t)((pit_bcd(n.month) << 8) | pit_bcd(n.day)));
        r->cf = 0;
        break; }
    default:
        /* 03h/05h (set time/date) and 06h/07h (alarm) are deliberately NOT answered:
           we cannot move the host's clock, and accepting the call with CF=0 would be
           the "runs but lies" shape -- TIME would report success and change nothing.
           Unmeasured on the reference too; p_bios.asm is where to add them. */
        break;
    }
}

/* --- lifecycle ------------------------------------------------------------ */
void vdd_pit_reset(void *self)
{
    pit_state *st = (pit_state *)self;
    vdd_bus *bus = st->bus; uint32_t fus = st->frame_us;
    void (*guard)(void *, int) = st->guard; void *gctx = st->guard_ctx;
    unsigned i; uint8_t *p = (uint8_t *)st;
    for (i = 0; i < sizeof(*st); ++i) p[i] = 0;     /* zero, then restore links */
    st->bus = bus;
    st->access = 3;
    /* ── COUNTER 0 COMES OUT OF POST IN A PERIODIC MODE, NOT MODE 0. ─────────
         Reset zeroes everything, which left mode = 0 -- a one-shot. Nothing had
         noticed, because the IRQ0 engine raises from the accumulator regardless
         of mode; what it got wrong was everything a guest can ASK. Read-Back
         reported mode 0 where a real machine says mode 2 (measured:
         p_pit.asm pit.rdback.st0 = 0x34 vs our 0x30), and the bare-count load
         rule took the one-shot path -- reloading immediately instead of at the
         end of the period.
       ⚠⚠ THE MODE IS A BIOS CHOICE, AND PCem HAS NOW SETTLED IT: **MODE 3**.
         Three answers, and the majority was wrong: SeaBIOS/QEMU leaves mode 2,
         dosbox-x mode 3, and a REAL AMI 486 BIOS with a genuine IBM VGA ROM
         leaves mode 3 (p_pit pit.rdback.st0 = 0x36). This project targets a
         period-correct PC, so 3 is the answer that matters.
       ⛔ AND docs/ref/pit.md SAID MODE 3 FIRST, FROM MEMORY, AND I "CORRECTED" IT
         TO 2 ON THE STRENGTH OF ONE ORACLE. The original claim was right. A
         single-oracle measurement is not more trustworthy than a remembered fact
         just because it is a measurement -- it is one machine's answer, and this
         one happened to be the unrepresentative machine. */
    st->mode = 3; st->mode_raw = 3;
    st->frame_us = fus ? fus : PIT_DEFAULT_FRAME_US;
    st->guard = guard; st->guard_ctx = gctx;        /* the lock survives a reset */
    /* ── COUNTER 1 IS FREE-RUNNING BEFORE ANYONE PROGRAMS IT. ────────────────
         On a PC the BIOS sets it to mode 2, divisor 18 for DRAM refresh and then
         leaves it alone forever; its gate is tied high. A guest that simply READS
         it -- which is the whole reason timing loops use counter 1, nothing else
         touches it -- must see a counter in motion. Left at reset's zeros it
         would read a constant, which is the "no time is passing" answer this
         surface was measured to be giving. */
    st->c1.reload = 18; st->c1.access = 3; st->c1.mode = 2; st->c1.mode_raw = 2;
    st->c1.gate = 1;                        /* tied high on the board */
    /* Counter 2 is NOT free-running: its gate is port 61h bit 0 and software
       decides. Left at reset defaults until a guest programs it. */
    st->c2.access = 3;
}

int vdd_pit_init(vdd_bus *b, void *self)
{
    pit_state *st = (pit_state *)self;
    st->bus = b;
    if (!st->frame_us) st->frame_us = PIT_DEFAULT_FRAME_US;
    if (!st->access)   st->access = 3;
    /* ⚠ THE POST DEFAULTS BELONG HERE TOO, NOT ONLY IN vdd_pit_reset. The host
         builds g_pit as a zeroed global and calls init; it does NOT call reset on
         the startup path, so a default written only into reset is a default the
         running host never gets. Putting mode 2 in reset alone left Read-Back
         still reporting mode 0 on the rig -- the change measured as having done
         nothing, which is how a fix that is not wired up looks. */
    if (!st->mode && !st->mode_raw) { st->mode = 3; st->mode_raw = 3; }
    if (!st->c1.reload) {                       /* counter 1: free-running refresh */
        st->c1.reload = 18; st->c1.access = 3; st->c1.mode = 2; st->c1.mode_raw = 2;
        st->c1.gate = 1;                    /* tied high on the board */
    }
    if (!st->c2.access) st->c2.access = 3;
    if (vdd_claim_ports(b, 0x40, 0x43, pit_in, pit_out, st)) return -1;
    if (vdd_claim_int(b, 0x08, pit_int08, st)) return -1;
    if (vdd_claim_int(b, 0x1A, pit_int1a, st)) return -1;
    if (vdd_on_frame(b, pit_frame, st)) return -1;
    return 0;
}
