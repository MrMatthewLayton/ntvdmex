/* vdd_pit.c -- see vdd_pit.h.  Intel 8254 channel-0 model + BIOS INT 08h/1Ah,
 * on the VDD bus.  Pure C, no <windows.h>. */
#include "vdd_pit.h"

/* current down-counter value (counts reload..1 repeatedly). */
static uint16_t pit_current_count(const pit_state *st)
{
    uint32_t R = pit_eff_reload(st);
    uint32_t phase = (uint32_t)(st->total_clocks % R);
    return (uint16_t)(R - phase);            /* R when phase==0 (65536 -> 0)     */
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

static void pit_frame(void *self)
{
    pit_state *st = (pit_state *)self;
    uint32_t clocks = (uint32_t)(((uint64_t)PIT_INPUT_HZ * st->frame_us) / 1000000u);
    vdd_pit_add_clocks(st, clocks);
}

/* --- 8254 ports 0x40-0x43 ------------------------------------------------- */
static void pit_out(void *self, uint16_t port, uint8_t w, uint32_t v)
{
    pit_state *st = (pit_state *)self;
    uint8_t val = (uint8_t)v; (void)w;
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
        uint8_t acc = st->access ? st->access : 3;
        if (acc == 1) st->reload = (uint16_t)((st->reload & 0xFF00) | val);
        else if (acc == 2) st->reload = (uint16_t)((st->reload & 0x00FF) | ((uint16_t)val << 8));
        else {                               /* lo then hi                      */
            if (!st->wr_flip) { st->reload = (uint16_t)((st->reload & 0xFF00) | val); st->wr_flip = 1; }
            else { st->reload = (uint16_t)((st->reload & 0x00FF) | ((uint16_t)val << 8)); st->wr_flip = 0; }
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

static void pit_in(void *self, uint16_t port, uint8_t w, uint32_t *val)
{
    pit_state *st = (pit_state *)self;
    uint16_t v; uint8_t acc;
    (void)w;
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
    unsigned i; uint8_t *p = (uint8_t *)st;
    for (i = 0; i < sizeof(*st); ++i) p[i] = 0;     /* zero, then restore links */
    st->bus = bus;
    st->access = 3;
    st->frame_us = fus ? fus : PIT_DEFAULT_FRAME_US;
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
