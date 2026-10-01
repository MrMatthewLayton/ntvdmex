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

/* ── THE GRANT. Mask bit AND controller-disable, asked in one place (see the header).
     Before #176 this was `if (c->masked) return 0;` inline, and the command register's
     bit 2 was stored and read by nothing -- a guest that disabled the controller to
     stop a transfer got the transfer anyway. */
int vdd_dma_grants(const dma_state *st, uint8_t ch)
{
    ch &= 7;
    if (st->ch[ch].masked) return 0;
    if (st->cmd[ch >> 2] & DMA_CMD_DISABLE) return 0;   /* ch0-3 -> cmd[0], 4-7 -> cmd[1] */
    return 1;
}

int vdd_dma_add_dreq(dma_state *st, dma_dreq_fn fn, const void *ctx)
{
    unsigned i;
    for (i = 0; i < st->dreq_n; ++i)
        if (st->dreq_fn[i] == fn && st->dreq_ctx[i] == ctx) return 0;
    if (st->dreq_n >= DMA_DREQ_MAX) return -1;
    st->dreq_fn[st->dreq_n] = fn; st->dreq_ctx[st->dreq_n] = ctx; ++st->dreq_n;
    return 0;
}

/* ── THE DREQ PINS, DERIVED. ───────────────────────────────────────────────────────
     Every device answers for itself (see dma_dreq_fn in the header). Channel 4 is not
     a device's: on an AT it is the CASCADE, wired to controller 1's HRQ, and the
     8237A raises HRQ when it has a request it is prepared to serve -- an unmasked
     channel on an enabled controller. So bit 4 is derived from bits 0-3 through the
     same grant every transfer uses. ⚠ Whatever a device claims for channel 4 is
     dropped: nothing on an AT can drive that line except controller 1. */
uint8_t vdd_dma_dreq(const dma_state *st)
{
    uint8_t m = 0, c;
    unsigned i;
    for (i = 0; i < st->dreq_n; ++i) m |= st->dreq_fn[i](st->dreq_ctx[i]);
    m &= (uint8_t)~0x10;
    for (c = 0; c < 4; ++c)
        if ((m & (1u << c)) && vdd_dma_grants(st, c)) { m |= 0x10; break; }
    return m;
}

static uint32_t dma_xfer(dma_state *st, uint8_t ch, uint8_t *dst, const uint8_t *src,
                         uint32_t n, int *tc_out)
{
    uint32_t unit = ((ch & 7) < 4) ? 1u : 2u, done = 0;
    if (tc_out) *tc_out = 0;
    if (!vdd_dma_grants(st, ch)) return 0;      /* no DACK: nothing moves, no TC   */
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
        else        st->page_spare[port & 0x0F] = val;   /* a latch all the same  */
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
    case 0x8: st->cmd[ctrl] = val; break;                /* command: bit 2 is read by
                                                            vdd_dma_grants; the rest
                                                            are stored (see the header) */
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
    /* `w` is OBSERVED but still not acted on: the 8237 is an 8-bit device and every
       read below returns one half through the flip-flop, which is faithful for the
       `in al,dx` the BIOS and every driver we have seen use. It is recorded because
       the poll RATE cannot be derived from the read count without knowing it. */

    if (port >= 0x80 && port <= 0x8F) {
        int c = dma_page_chan(port);
        /* ⚠ "Unused" was true of the CHANNEL MAPPING and false of the hardware:
             these ports are latches whether or not a channel reads them, and
             0xFF was us describing an empty bus. See page_spare in the header. */
        *val = (c >= 0) ? st->ch[c].page : st->page_spare[port & 0x0F];
        return;
    }
    ctrl = (port >= 0xC0) ? 1 : 0;
    reg  = ctrl ? ((port - 0xC0) >> 1) : port;

    if (reg < 8) {
        chan = (ctrl ? 4 : 0) + (reg >> 1);
        if (reg & 1) {
            ++st->rd_count[chan];
            ++st->count_reads;
            if      (w == 1) ++st->rd_w1;
            else if (w == 2) ++st->rd_w2;
            else             ++st->rd_w4;
        } else ++st->rd_addr[chan];
        *val = (reg & 1) ? dma_read_half(st->ch[chan].cur_count, &st->ff[ctrl])
                         : dma_read_half(st->ch[chan].cur_addr,  &st->ff[ctrl]);
        return;
    }
    if (reg == 0x8) {                            /* status: TC bits 0-3, DRQ 4-7   */
        uint8_t s = 0;
        ++st->rd_status[ctrl];
        for (i = 0; i < 4; ++i) {
            chan = (ctrl ? 4 : 0) + i;
            if (st->ch[chan].tc) s |= (uint8_t)(1 << i);
            st->ch[chan].tc = 0;                 /* reading status clears TC        */
        }
        /* ── BITS 7:4 -- "set whenever their corresponding channel is requesting
             service" (8237A datasheet). They always read 0 before #176. Derived, not
             latched, so the read that clears TC cannot touch them: a request is still
             pending after you look at it, until the device stops making it. */
        s |= (uint8_t)(((vdd_dma_dreq(st) >> (ctrl ? 4 : 0)) & 0x0F) << 4);
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
    dma_dreq_fn fn[DMA_DREQ_MAX]; const void *ctx[DMA_DREQ_MAX];
    uint8_t n = st->dreq_n;
    unsigned i; uint8_t *p = (uint8_t *)st;
    for (i = 0; i < DMA_DREQ_MAX; ++i) { fn[i] = st->dreq_fn[i]; ctx[i] = st->dreq_ctx[i]; }
    for (i = 0; i < sizeof(*st); ++i) p[i] = 0;
    st->bus = bus;
    /* the DREQ wiring is the machine's, not the chip's: a reset keeps it */
    for (i = 0; i < DMA_DREQ_MAX; ++i) { st->dreq_fn[i] = fn[i]; st->dreq_ctx[i] = ctx[i]; }
    st->dreq_n = n;
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
