/* vdd_pit.c -- see vdd_pit.h.  Intel 8254 channel-0 model + BIOS INT 08h/1Ah,
 * on the VDD bus.  Pure C, no <windows.h>. */
#include "vdd_pit.h"

/* ── ★★★★ THE COUNT STARTS WHEN THE GUEST LOADS IT. ─────────────────────────────
     This used to be `R - (total_clocks % R)`: a free-running divider whose phase had
     nothing to do with the moment the guest wrote the count. For a guest that only
     ever waits for IRQ0 that is invisible. For one that READS THE COUNTER it is a
     random number generator.
   ★ MEASURED ON LEMMINGS (s69). Its "High Performance PC" option calibrates the game
     tick from the CRT: load counter 0 with 0xFFFF in mode 0, count 160 scanlines on
     0x3DA bit 0, latch, and use 0xFFFF - latch as the reload (guest CS:15BB..1633 in
     the real-DOS dump). 160 lines at 31.47 kHz is 6,067 clocks; the user's by-hand
     run got 0x4BB9 (19,385 -- 61.5 Hz, a third of the intended tick rate, and the
     "music is slower" they reported), and every run would get a different one, because
     the read-back was `total_clocks % 0xFFFF` at the latch -- uniformly random.
   ► So keep the instant of the load (`load_clocks`) and derive the counting element
     from the clocks since it, per mode (Intel 8254 datasheet):
       mode 0   load - elapsed, and on past terminal count through 0xFFFF (one-shot)
       mode 3   decrements by TWO per clock and reloads at zero, so the count runs
                R, R-2, ... 2 twice per period and a read never shows an odd LSB
       others   R - (elapsed mod R), reloading at the period (modes 2, 4, 5; mode 1)
     The IRQ engine below is untouched: it still fires once per reload of elapsed
     clocks, which is right for the periodic modes and a harmless over-approximation
     of mode 0 (whose single terminal count no guest here waits on). */
static uint16_t pit_current_count(const pit_state *st)
{
    uint32_t R = pit_eff_reload(st);
    uint64_t elapsed = (st->total_clocks >= st->load_clocks)
                     ? st->total_clocks - st->load_clocks : 0;
    if (st->mode == 0)
        return (uint16_t)(R - elapsed);      /* wraps through 0xFFFF past zero   */
    if (st->mode == 3) {
        uint32_t half  = (R + 1) / 2;
        uint32_t phase = (uint32_t)(elapsed % half);
        return (uint16_t)(R - 2 * phase);    /* R when phase==0 (65536 -> 0)     */
    }
    return (uint16_t)(R - (uint32_t)(elapsed % R));
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
    }
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

/* The count register is loaded: the counting element restarts from it NOW, and so
   does the output period -- the control word that precedes a load stops the
   counter (OUT high, modes 2/3) and the count write starts it, so the first IRQ0
   after a reprogram is one full reload away, not early by the old period's
   remainder. The latch is left alone: a latched count is held until it is read. */
static void pit_load(pit_state *st, uint16_t count)
{
    st->reload      = count;
    st->load_clocks = st->total_clocks;
    st->accum       = 0;
}

/* --- 8254 ports 0x40-0x43 ------------------------------------------------- */
static void pit_out_locked(pit_state *st, uint16_t port, uint8_t val)
{
    if (port == 0x43) {                      /* mode/command register           */
        uint8_t ch = (uint8_t)(val >> 6);
        uint8_t acc = (uint8_t)((val >> 4) & 3);
        if (ch == 2) {                       /* channel 2 = PC-speaker tone     */
            if (acc != 0) { st->ch2_access = acc; st->ch2_wr_flip = 0; }
            return;                          /* (ch2 latch-count not modelled)  */
        }
        if (ch != 0) return;                 /* channels 1 (refresh) ignored    */
        if (acc == 0) {                      /* latch count command             */
            st->latch = pit_current_count(st);
            st->latched = 1; st->rd_flip = 0;
        } else {
            st->access = acc; st->mode = (uint8_t)((val >> 1) & 7); st->wr_flip = 0;
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
    }
    /* port 0x41 (DRAM refresh) not modelled */
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
    default:
        break;                               /* RTC subfns not modelled yet     */
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
    st->frame_us = fus ? fus : PIT_DEFAULT_FRAME_US;
    st->guard = guard; st->guard_ctx = gctx;        /* the lock survives a reset */
}

int vdd_pit_init(vdd_bus *b, void *self)
{
    pit_state *st = (pit_state *)self;
    st->bus = b;
    if (!st->frame_us) st->frame_us = PIT_DEFAULT_FRAME_US;
    if (!st->access)   st->access = 3;
    if (vdd_claim_ports(b, 0x40, 0x43, pit_in, pit_out, st)) return -1;
    if (vdd_claim_int(b, 0x08, pit_int08, st)) return -1;
    if (vdd_claim_int(b, 0x1A, pit_int1a, st)) return -1;
    if (vdd_on_frame(b, pit_frame, st)) return -1;
    return 0;
}
