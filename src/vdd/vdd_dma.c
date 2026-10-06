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

/* ── ONE TRANSFER RETIRED: walk the address, count down, and at terminal count do
     what the 8237 does -- latch TC, clear the channel's software request ("cleared
     upon generation of a TC", datasheet), and either reload (auto-initialise) or set
     the channel's own mask bit. Returns 1 if this transfer was the terminal one. */
static int dma_advance(dma_state *st, uint8_t ch, int *tc_out)
{
    dma_chan *c = &st->ch[ch & 7];
    if (c->mode & DMA_MODE_DECREMENT) c->cur_addr--;
    else                              c->cur_addr++;
    if (c->cur_count == 0) {                    /* terminal count reached          */
        c->tc = 1;
        st->req[(ch & 7) >> 2] &= (uint8_t)~(1u << (ch & 3));
        if (tc_out) *tc_out = 1;
        if (c->mode & DMA_MODE_AUTOINIT) {      /* ring buffer: reload             */
            c->cur_addr  = c->base_addr;
            c->cur_count = c->base_count;
        } else {
            /* ★ "TC when the word count goes from 0000h to FFFFh" (datasheet): the
                 count really does wrap, and a driver that waits for a single-cycle
                 block by polling the count for FFFFh is waiting for exactly this.
                 Before #246 it rested at 0000h -- p_dma2's TC rows showed it. */
            c->cur_count = 0xFFFF;
            c->masked = 1;                      /* single-cycle: the 8237 masks it  */
        }
        return 1;
    }
    c->cur_count--;
    return 0;
}

/* One unit of transfer (1 byte on ch0-3, 1 word on ch4-7). Returns 0 when the
   channel has hit terminal count and stopped, so the caller ends the block. */
static int dma_step(dma_state *st, uint8_t ch, uint8_t *dst, const uint8_t *src,
                    uint32_t *done, int *tc_out)
{
    dma_chan *c = &st->ch[ch & 7];
    uint32_t unit = ((ch & 7) < 4) ? 1u : 2u;
    uint8_t *mem = (uint8_t *)VddMapLinear(st->bus, vdd_dma_cur_phys(st, ch));
    uint32_t i;

    if (dst) for (i = 0; i < unit; ++i) dst[i] = mem[i];
    else     for (i = 0; i < unit; ++i) mem[i] = src[i];
    *done += unit;

    /* a ring keeps streaming through its TC; a single-cycle block stops there */
    if (dma_advance(st, ch, tc_out) && !(c->mode & DMA_MODE_AUTOINIT)) return 0;
    return 1;
}

/* ── THE GRANT. Mask bit AND controller-disable, asked in one place (see the header).
     Before #176 this was `if (c->masked) return 0;` inline, and the command register's
     bit 2 was stored and read by nothing -- a guest that disabled the controller to
     stop a transfer got the transfer anyway. */
/* The grant as ONE controller sees it: its own mask bit, its own disable bit. This
   is what decides whether controller 1 raises HRQ (= DREQ4) at all. */
static int dma_ctrl_grants(const dma_state *st, uint8_t ch)
{
    ch &= 7;
    if (st->ch[ch].masked) return 0;
    if (st->cmd[ch >> 2] & DMA_CMD_DISABLE) return 0;   /* ch0-3 -> cmd[0], 4-7 -> cmd[1] */
    return 1;
}

/* ── IS CONTROLLER 1 CONNECTED TO THE BUS? (the AT cascade, #246) ─────────────────
     Its HRQ is controller 2's DREQ4 and its HLDA is controller 2's DACK4, so it is
     served only while channel 4 is unmasked and controller 2 is enabled. Channel 4's
     MODE is not asked: a guest that reprograms it out of cascade mode has broken the
     board too, but whether a real part then starves or runs channel 4's own cycles is
     a bus question we have no answer for, and no guest is known to do it. */
static int dma_cascade_up(const dma_state *st)
{
    return !st->ch[4].masked && !(st->cmd[1] & DMA_CMD_DISABLE);
}

int vdd_dma_grants(const dma_state *st, uint8_t ch)
{
    ch &= 7;
    if (!dma_ctrl_grants(st, ch)) return 0;
    if (ch < 4 && !dma_cascade_up(st)) return 0;
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
    /* HRQ from controller 1: a hardware request it would serve (its own mask and
       disable bits -- NOT the cascade's, which is the far end of this very wire), or a
       software request, which the datasheet calls non-maskable. */
    for (c = 0; c < 4; ++c)
        if (((m & (1u << c)) && dma_ctrl_grants(st, c))
         || ((st->req[0] & (1u << c)) && !(st->cmd[0] & DMA_CMD_DISABLE))) { m |= 0x10; break; }
    /* the request register shows in status 7:4 like any DREQ (#246); a software
       request "on" channel 4 is not a line anything drives, so it is dropped too */
    m |= (uint8_t)(st->req[0] | ((st->req[1] & 0x0E) << 4));
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

/* ── THE 8237 AS ITS OWN BUS MASTER: SOFTWARE REQUESTS. (#246) ────────────────────
     Every transfer above is PULLED by a device. A request-register bit is a DREQ no
     device made, so the controller has to carry the cycles out itself -- and nothing
     is on the other end of DACK. What that means, per transfer type:
       verify (00)  address and count walk; no memory cycle at all (the datasheet's
                    "pseudo transfers")
       read   (10)  memory is read onto the bus for a device that is not listening:
                    nothing observable but the walk
       write  (01)  memory is WRITTEN from a data bus nobody drives: the bytes are
                    FFh, the same float every unclaimed port reads on this host
       11           illegal; treated as verify, which touches nothing
     The request stays asserted until TC ("cleared upon generation of a TC"), so in
     demand, single and block mode alike the channel runs to terminal count. A
     channel in CASCADE mode does no cycles of its own and ignores it.
   ⚠ MEMORY OUTSIDE THE VDM IS NOT TOUCHED. A guest linear address is a host VA only
     for the V86 space (the first 1 MB + the HMA); the 8237's 24-bit physical address
     has no meaning beyond it here, and a write there could land anywhere in the host
     process. Cycles beyond 110000h walk and count, and move nothing. */
#define DMA_PHYS_LIMIT 0x110000u

static uint8_t *dma_mem(dma_state *st, uint32_t phys)
{
    if (phys >= DMA_PHYS_LIMIT) return 0;
    return (uint8_t *)VddMapLinear(st->bus, phys);
}

static void dma_soft_block(dma_state *st, uint8_t ch)
{
    dma_chan *c = &st->ch[ch];
    uint32_t unit = (ch < 4) ? 1u : 2u, i, guard;
    st->soft_runs++;
    for (guard = 0; guard < 0x10001u; ++guard) {
        if ((c->mode & DMA_MODE_XFER) == DMA_MODE_XFER_WRITE) {
            uint8_t *mem = dma_mem(st, vdd_dma_cur_phys(st, ch));
            if (mem) for (i = 0; i < unit; ++i) mem[i] = 0xFF;
        }
        if (dma_advance(st, ch, 0)) break;
    }
}

/* ── MEMORY-TO-MEMORY, controller 1 only (command bit 0). 8237A datasheet: started by
     a software request on channel 0; each byte is read at channel 0's current address
     into the TEMPORARY register, then written at channel 1's; both addresses step
     (channel 0's held still when command bit 1 is set -- "a single word written to a
     block of memory"); CHANNEL 1's word count is the one that runs, and its TC ends
     the service. Channel 0's count is not consulted.
   ⚠ At that EOP each of the two channels auto-initialises or masks itself by its own
     mode bit, and only channel 1 latches TC. That is a reading of the datasheet, not
     a measurement (see docs/inventory/dma.md for what the oracles said). */
static void dma_m2m(dma_state *st)
{
    dma_chan *c0 = &st->ch[0], *c1 = &st->ch[1];
    uint32_t guard;
    st->m2m_runs++;
    for (guard = 0; guard < 0x10001u; ++guard) {
        uint8_t *src = dma_mem(st, vdd_dma_cur_phys(st, 0));
        uint8_t *dst = dma_mem(st, vdd_dma_cur_phys(st, 1));
        st->temp[0] = src ? *src : 0xFF;
        if (dst) *dst = st->temp[0];
        if (!(st->cmd[0] & DMA_CMD_ADDRHOLD)) {
            if (c0->mode & DMA_MODE_DECREMENT) c0->cur_addr--; else c0->cur_addr++;
        }
        if (c1->mode & DMA_MODE_DECREMENT) c1->cur_addr--; else c1->cur_addr++;
        if (c1->cur_count == 0) break;
        c1->cur_count--;
    }
    c1->tc = 1;
    st->req[0] &= (uint8_t)~0x03;
    if (c0->mode & DMA_MODE_AUTOINIT) { c0->cur_addr = c0->base_addr; c0->cur_count = c0->base_count; }
    else c0->masked = 1;
    if (c1->mode & DMA_MODE_AUTOINIT) { c1->cur_addr = c1->base_addr; c1->cur_count = c1->base_count; }
    else { c1->cur_count = 0xFFFF; c1->masked = 1; }    /* the count wraps, as above */
}

/* Serve every pending software request the controllers are in a position to serve.
   Called after every register write, because a request written while its controller
   is disabled -- or while the cascade is down -- is served the moment that changes. */
static void dma_soft_service(dma_state *st)
{
    int ctrl, c;
    for (ctrl = 0; ctrl < 2; ++ctrl) {
        if (!st->req[ctrl]) continue;
        if (st->cmd[ctrl] & DMA_CMD_DISABLE) continue;
        if (ctrl == 0 && !dma_cascade_up(st)) continue;
        for (c = 0; c < 4; ++c) {               /* fixed priority: 0 highest        */
            uint8_t ch = (uint8_t)(ctrl * 4 + c);
            if (!(st->req[ctrl] & (1u << c))) continue;
            if (ch == 4) continue;               /* the cascade itself              */
            if ((st->ch[ch].mode & DMA_MODE_SELECT) == DMA_MODE_SELECT) continue;
            if (ctrl == 0 && (st->cmd[0] & DMA_CMD_MEM2MEM)) {
                /* in memory-to-memory mode channels 0 and 1 belong to the copy:
                   channel 0's request starts it, channel 1's alone starts nothing */
                if (c == 0) dma_m2m(st);
                if (c <= 1) continue;
            }
            dma_soft_block(st, ch);
        }
    }
}

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
    st->req[ctrl] = 0;                  /* "the entire register is cleared by a Reset" */
    st->temp[ctrl] = 0;
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
    case 0x9:                                            /* request register (#246) */
        if (val & 4) st->req[ctrl] |= (uint8_t)(1u << (val & 3));
        else         st->req[ctrl] &= (uint8_t)~(1u << (val & 3));
        break;
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
    /* any write can make a pending software request servable (the request itself, a
       re-enable, the cascade coming up) -- and there is nothing else to wake it */
    if (st->req[0] | st->req[1]) dma_soft_service(st);
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
    if (reg == 0xD) { *val = st->temp[ctrl]; return; }   /* temporary register (#246) */
    *val = 0xFF;
}

/* --- lifecycle ------------------------------------------------------------ */
/* ── WHAT POST LEAVES ON AN AT: CHANNEL 4 IN CASCADE MODE, UNMASKED. (#246) ──────
     The BIOS does this once at power-on (mode C0h to D6h, then unmask channel 4 at
     D4h), and every DOS program inherits it -- it is what connects controller 1 to
     the bus at all. Without it the cascade could not be honoured: the power-on master
     clear leaves channel 4 masked, and every 8-bit channel would be starved. */
static void dma_post(dma_state *st)
{
    st->ch[4].mode   = DMA_MODE_SELECT;         /* C0h: cascade, channel 0 of ctrl 2 */
    st->ch[4].masked = 0;
}

void vdd_dma_reset(void *self)
{
    dma_state *st = (dma_state *)self;
    VDD_BUS *bus = st->bus;
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
    dma_post(st);
}

int vdd_dma_init(VDD_BUS *b, void *self)
{
    dma_state *st = (dma_state *)self;
    st->bus = b;
    dma_master_clear(st, 0);
    dma_master_clear(st, 1);
    dma_post(st);
    if (VddClaimPorts(b, 0x00, 0x0F, dma_in, dma_out, st)) return -1;  /* controller 1 */
    if (VddClaimPorts(b, 0x80, 0x8F, dma_in, dma_out, st)) return -1;  /* page regs    */
    if (VddClaimPorts(b, 0xC0, 0xDF, dma_in, dma_out, st)) return -1;  /* controller 2 */
    return 0;
}
