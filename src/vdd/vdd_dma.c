/* vdd_dma.c -- see vdd_dma.h.  A pair of Intel 8237A DMA controllers plus the
 * AT page registers, on the VDD bus.  Pure C, no <windows.h>. */
#include "vdd_dma.h"

/* Page register port -> channel. The mapping is not sequential; it is what IBM
   wired, and getting it wrong silently corrupts the high address bits. */
static int dma_page_chan(uint16_t port)
{
    switch (port) {
    case 0x87: return 0;
    case 0x83: return 1;
    case 0x81: return 2;
    case 0x82: return 3;
    case 0x8B: return 5;
    case 0x89: return 6;
    case 0x8A: return 7;
    default:   return -1;            /* 0x80 / 0x84-0x86 / 0x88 / 0x8C-0x8F: unused */
    }
}

/* --- address arithmetic --------------------------------------------------- */
/* 8-bit channels address bytes directly; 16-bit channels address WORDS inside a
   128K page, so the address register is shifted and the page's low bit ignored. */
uint32_t vdd_dma_cur_phys(const dma_state *st, uint8_t ch)
{
    const dma_chan *c = &st->ch[ch & 7];
    if ((ch & 7) < 4) return ((uint32_t)c->page << 16) | c->cur_addr;
    return (((uint32_t)c->page & 0xFE) << 16) | ((uint32_t)c->cur_addr << 1);
}

uint32_t vdd_dma_remaining(const dma_state *st, uint8_t ch)
{
    const dma_chan *c = &st->ch[ch & 7];
    uint32_t units = (uint32_t)c->cur_count + 1;        /* 8237 counts n-1        */
    return ((ch & 7) < 4) ? units : units * 2;
}

/* One unit of transfer (1 byte on ch0-3, 1 word on ch4-7). Returns 0 when the
   channel has hit terminal count and stopped, so the caller ends the block. */
static int dma_step(dma_state *st, uint8_t ch, uint8_t *dst, const uint8_t *src,
                    uint32_t *done, int *tc_out)
{
    dma_chan *c = &st->ch[ch & 7];
    uint32_t unit = ((ch & 7) < 4) ? 1u : 2u;
    uint8_t *mem = (uint8_t *)vdd_map_lin(st->bus, vdd_dma_cur_phys(st, ch));
    uint32_t i;

    if (dst) for (i = 0; i < unit; ++i) dst[i] = mem[i];
    else     for (i = 0; i < unit; ++i) mem[i] = src[i];
    *done += unit;

    /* walk the address, then retire one transfer from the count */
    if (c->mode & DMA_MODE_DECREMENT) c->cur_addr--;
    else                              c->cur_addr++;

    if (c->cur_count == 0) {                    /* terminal count reached          */
        c->tc = 1;
        if (tc_out) *tc_out = 1;
        if (c->mode & DMA_MODE_AUTOINIT) {      /* ring buffer: reload and continue */
            c->cur_addr  = c->base_addr;
            c->cur_count = c->base_count;
            return 1;
        }
        c->masked = 1;                          /* single-cycle: the 8237 masks it  */
        return 0;
    }
    c->cur_count--;
    return 1;
}

static uint32_t dma_xfer(dma_state *st, uint8_t ch, uint8_t *dst, const uint8_t *src,
                         uint32_t n, int *tc_out)
{
    dma_chan *c = &st->ch[ch & 7];
    uint32_t unit = ((ch & 7) < 4) ? 1u : 2u, done = 0;
    if (tc_out) *tc_out = 0;
    if (c->masked) return 0;
    while (done + unit <= n) {
        if (!dma_step(st, ch, dst ? dst + done : 0, src ? src + done : 0, &done, tc_out))
            break;                              /* stopped at terminal count       */
    }
    return done;
}

uint32_t vdd_dma_read(dma_state *st, uint8_t ch, uint8_t *dst, uint32_t n, int *tc_out)
{ return dma_xfer(st, ch, dst, 0, n, tc_out); }

uint32_t vdd_dma_write(dma_state *st, uint8_t ch, const uint8_t *src, uint32_t n, int *tc_out)
{ return dma_xfer(st, ch, 0, src, n, tc_out); }

/* --- register file -------------------------------------------------------- */
/* Address and count are 16-bit registers behind an 8-bit port, so the controller
   keeps a flip-flop selecting which half the next access hits. Software clears it
   (port 0x0C / 0xD8) before programming a channel; forgetting to model it swaps
   the halves and sends DMA to a wild address. */
static void dma_write_half(uint16_t *reg, uint8_t *ff, uint8_t val)
{
    if (*ff) *reg = (uint16_t)((*reg & 0x00FF) | ((uint16_t)val << 8));
    else     *reg = (uint16_t)((*reg & 0xFF00) | val);
    *ff ^= 1;
}
static uint8_t dma_read_half(uint16_t reg, uint8_t *ff)
{
    uint8_t v = *ff ? (uint8_t)(reg >> 8) : (uint8_t)reg;
    *ff ^= 1;
    return v;
}

static void dma_master_clear(dma_state *st, int ctrl)
{
    int base = ctrl ? 4 : 0, i;
    st->ff[ctrl]  = 0;
    st->cmd[ctrl] = 0;
    for (i = base; i < base + 4; ++i) { st->ch[i].masked = 1; st->ch[i].tc = 0; }
}

static void dma_out(void *self, uint16_t port, uint8_t w, uint32_t v)
{
    dma_state *st = (dma_state *)self;
    uint8_t val = (uint8_t)v;
    int ctrl, reg, chan;
    (void)w;

    if (port >= 0x80 && port <= 0x8F) {                  /* page registers        */
        int c = dma_page_chan(port);
        if (c >= 0) st->ch[c].page = val;
        return;
    }
    ctrl = (port >= 0xC0) ? 1 : 0;
    reg  = ctrl ? ((port - 0xC0) >> 1) : port;           /* controller 2: 2x spacing */

    if (reg < 8) {                                       /* per-channel addr/count */
        chan = (ctrl ? 4 : 0) + (reg >> 1);
        if (reg & 1) {                                   /* count                  */
            dma_write_half(&st->ch[chan].base_count, &st->ff[ctrl], val);
            st->ch[chan].cur_count = st->ch[chan].base_count;
        } else {                                         /* address                */
            dma_write_half(&st->ch[chan].base_addr, &st->ff[ctrl], val);
            st->ch[chan].cur_addr = st->ch[chan].base_addr;
        }
        return;
    }
    switch (reg) {
    case 0x8: st->cmd[ctrl] = val; break;                /* command                */
    case 0x9: break;                                     /* software DRQ: unused   */
    case 0xA:                                            /* single mask bit        */
        chan = (ctrl ? 4 : 0) + (val & 3);
        st->ch[chan].masked = (val & 4) ? 1 : 0;
        break;
    case 0xB:                                            /* mode                   */
        chan = (ctrl ? 4 : 0) + (val & DMA_MODE_CHAN);
        st->ch[chan].mode = val;
        break;
    case 0xC: st->ff[ctrl] = 0; break;                   /* clear byte pointer     */
    case 0xD: dma_master_clear(st, ctrl); break;         /* master clear           */
    case 0xE:                                            /* clear mask register    */
        for (chan = ctrl ? 4 : 0; chan < (ctrl ? 8 : 4); ++chan) st->ch[chan].masked = 0;
        break;
    case 0xF:                                            /* write all mask bits    */
        for (chan = 0; chan < 4; ++chan)
            st->ch[(ctrl ? 4 : 0) + chan].masked = (val >> chan) & 1;
        break;
    default: break;
    }
}

static void dma_in(void *self, uint16_t port, uint8_t w, uint32_t *val)
{
    dma_state *st = (dma_state *)self;
    int ctrl, reg, chan, i;
    (void)w;

    if (port >= 0x80 && port <= 0x8F) {
        int c = dma_page_chan(port);
        *val = (c >= 0) ? st->ch[c].page : 0xFF;
        return;
    }
    ctrl = (port >= 0xC0) ? 1 : 0;
    reg  = ctrl ? ((port - 0xC0) >> 1) : port;

    if (reg < 8) {
        chan = (ctrl ? 4 : 0) + (reg >> 1);
        *val = (reg & 1) ? dma_read_half(st->ch[chan].cur_count, &st->ff[ctrl])
                         : dma_read_half(st->ch[chan].cur_addr,  &st->ff[ctrl]);
        return;
    }
    if (reg == 0x8) {                            /* status: TC bits 0-3, DRQ 4-7   */
        uint8_t s = 0;
        for (i = 0; i < 4; ++i) {
            chan = (ctrl ? 4 : 0) + i;
            if (st->ch[chan].tc) s |= (uint8_t)(1 << i);
            st->ch[chan].tc = 0;                 /* reading status clears TC        */
        }
        *val = s;
        return;
    }
    *val = 0xFF;
}

/* --- lifecycle ------------------------------------------------------------ */
void vdd_dma_reset(void *self)
{
    dma_state *st = (dma_state *)self;
    vdd_bus *bus = st->bus;
    unsigned i; uint8_t *p = (uint8_t *)st;
    for (i = 0; i < sizeof(*st); ++i) p[i] = 0;
    st->bus = bus;
    dma_master_clear(st, 0);
    dma_master_clear(st, 1);
}

int vdd_dma_init(vdd_bus *b, void *self)
{
    dma_state *st = (dma_state *)self;
    st->bus = b;
    dma_master_clear(st, 0);
    dma_master_clear(st, 1);
    if (vdd_claim_ports(b, 0x00, 0x0F, dma_in, dma_out, st)) return -1;  /* controller 1 */
    if (vdd_claim_ports(b, 0x80, 0x8F, dma_in, dma_out, st)) return -1;  /* page regs    */
    if (vdd_claim_ports(b, 0xC0, 0xDF, dma_in, dma_out, st)) return -1;  /* controller 2 */
    return 0;
}
