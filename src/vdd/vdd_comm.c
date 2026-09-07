/*
 * vdd_comm.c -- 8250/16550A serial port VDD.  (GH #9)  See vdd_comm.h.
 */
#include "vdd_comm.h"

/* ── THE INTERRUPT IDENTIFICATION REGISTER IS A PRIORITY ENCODER. ────────────
     Not a bit field -- a NUMBER, and the order is fixed by the part: receiver
     line status (highest), then received-data-available, then transmitter
     holding empty, then modem status. Bit 0 is INVERTED: 1 means "no interrupt
     pending", which is the single most misread bit in the whole device.
   ⚠ Reading IIR when THRE is the reason CLEARS that reason. This is not a
     detail a driver can work around: the standard transmit loop is "write a
     byte, take the interrupt, read IIR, write the next byte", and an IIR that
     keeps reporting THRE forever gives an interrupt storm instead. */
static uint8_t comm_iir(comm_port *c)
{
    if ((c->ier & IER_RLS)  && (c->lsr & (LSR_OE | LSR_PE | LSR_FE | LSR_BI)))
        return 0x06;
    if ((c->ier & IER_RDA)  && (c->lsr & LSR_DR))    return 0x04;
    if ((c->ier & IER_THRE) && c->thre_pending)      return 0x02;
    if ((c->ier & IER_MS)   && (c->msr & 0x0F))      return 0x00;
    return 0x01;                                  /* bit 0 set = nothing owed  */
}

/* Raise the line if anything is owed. OUT2 gates the IRQ on a PC -- the UART's
   interrupt pin reaches the PIC through a buffer that OUT2 enables -- which is
   why every DOS serial driver sets MCR bit 3 and why forgetting it here would
   make interrupt-driven receive work in emulation and nowhere else. */
static void comm_update_irq(comm_state *st, comm_port *c)
{
    if (!st->bus || !c->fitted) return;
    if (!(c->mcr & MCR_OUT2)) return;
    if (comm_iir(c) & 0x01) return;               /* nothing pending           */
    vdd_raise_irq(st->bus, c->irq);
}

/* ── LOOPBACK IS A REWIRING, NOT A FLAG. ─────────────────────────────────────
     With MCR bit 4 set the part disconnects its pins and connects, internally:
       transmitter    -> receiver
       DTR (MCR bit0) -> DSR (MSR bit5)
       RTS (MCR bit1) -> CTS (MSR bit4)
       OUT1(MCR bit2) -> RI  (MSR bit6)
       OUT2(MCR bit3) -> DCD (MSR bit7)
     Those four mappings are the whole self-test: a driver asserts DTR/RTS and
     checks DSR/CTS came back. Getting the pairing wrong gives a port that
     echoes bytes and still fails every detection routine ever written, which is
     a failure that looks like success right up until nothing uses the port. */
static uint8_t comm_loop_msr(uint8_t mcr)
{
    uint8_t m = 0;
    if (mcr & MCR_DTR)  m |= MSR_DSR;
    if (mcr & MCR_RTS)  m |= MSR_CTS;
    if (mcr & MCR_OUT1) m |= MSR_RI;
    if (mcr & MCR_OUT2) m |= MSR_DCD;
    return m;
}

/* Set the modem status, recording which lines CHANGED in the delta nibble.
   A delta bit, once set, stays set until the guest reads MSR -- that read is
   the acknowledgement, and it is also what clears a modem-status interrupt. */
static void comm_set_msr(comm_port *c, uint8_t lines)
{
    uint8_t old = (uint8_t)(c->msr & 0xF0);
    uint8_t d   = (uint8_t)((old ^ (lines & 0xF0)) >> 4);
    /* RI has no "changed" bit -- it has TERI, "trailing edge of ring
       indicator", which is set only on a 1 -> 0 transition. */
    if (d & 0x04) { d &= (uint8_t)~0x04; if (old & MSR_RI) d |= 0x04; }
    c->msr = (uint8_t)((lines & 0xF0) | ((c->msr & 0x0F) | d));
}

static void comm_push_rx(comm_port *c, uint8_t b)
{
    if (c->rx_len >= COMM_RX_RING) { c->lsr |= LSR_OE; return; }
    c->rx[(c->rx_head + c->rx_len) % COMM_RX_RING] = b;
    ++c->rx_len;
    c->lsr |= LSR_DR;
    ++c->rx_count;
}

static uint8_t comm_pop_rx(comm_port *c)
{
    uint8_t b;
    if (!c->rx_len) return c->rbr;
    b = c->rx[c->rx_head];
    c->rx_head = (uint16_t)((c->rx_head + 1) % COMM_RX_RING);
    --c->rx_len;
    if (!c->rx_len) c->lsr &= (uint8_t)~LSR_DR;
    c->rbr = b;
    return b;
}

static comm_port *comm_find(comm_state *st, uint16_t port, uint8_t *reg)
{
    int i;
    for (i = 0; i < COMM_MAX_PORTS; ++i) {
        comm_port *c = &st->p[i];
        if (c->fitted && port >= c->base && port < (uint16_t)(c->base + 8)) {
            *reg = (uint8_t)(port - c->base);
            return c;
        }
    }
    return 0;
}

static void comm_in(void *self, uint16_t port, uint8_t width, uint32_t *val)
{
    comm_state *st = (comm_state *)self;
    uint8_t reg = 0;
    comm_port *c = comm_find(st, port, &reg);
    (void)width;
    if (!c) { *val = 0xFF; return; }
    switch (reg) {
    case COMM_RBR:
        if (c->lcr & 0x80) { *val = c->dll; break; }
        *val = comm_pop_rx(c);
        break;
    case COMM_IER:
        *val = (c->lcr & 0x80) ? c->dlm : c->ier;
        break;
    case COMM_IIR: {
        uint8_t v = comm_iir(c);
        if (v == 0x02) c->thre_pending = 0;        /* the read IS the clear     */
        /* Bits 6-7 report an ENABLED FIFO. Answering 0xC0 when the guest never
           enabled one would tell a 16550-aware driver it may write 16 bytes
           between interrupts on a part that is behaving like an 8250. */
        if (c->fcr & 0x01) v |= 0xC0;
        *val = v;
        break; }
    case COMM_LCR: *val = c->lcr; break;
    case COMM_MCR: *val = c->mcr; break;
    case COMM_LSR:
        *val = c->lsr;
        /* Reading LSR clears the error bits -- that is what makes it a status
           register rather than a log. DR and THRE are NOT errors and stay. */
        c->lsr &= (uint8_t)~(LSR_OE | LSR_PE | LSR_FE | LSR_BI);
        break;
    case COMM_MSR:
        *val = c->msr;
        c->msr &= 0xF0;                            /* the read acknowledges     */
        break;
    case COMM_SCR: *val = c->scr; break;
    default: *val = 0xFF; break;
    }
}

static void comm_out(void *self, uint16_t port, uint8_t width, uint32_t val)
{
    comm_state *st = (comm_state *)self;
    uint8_t reg = 0, v = (uint8_t)(val & 0xFF);
    comm_port *c = comm_find(st, port, &reg);
    (void)width;
    if (!c) return;
    switch (reg) {
    case COMM_RBR:                                 /* transmit holding          */
        if (c->lcr & 0x80) { c->dll = v; break; }
        ++c->tx_count;
        /* ⚠ THE TRANSMITTER IS NEVER BUSY HERE, and that is a decision. A real
             part clears THRE while the byte shifts out; we complete instantly,
             so THRE stays set and a polling driver never waits. See the baud
             note in the header -- there is no wire whose timing must be met. */
        if (c->mcr & MCR_LOOP) comm_push_rx(c, v);
        else if (st->sink)     st->sink(st->sink_ctx, (int)(c - st->p), v);
        c->thre_pending = 1;
        comm_update_irq(st, c);
        break;
    case COMM_IER:
        if (c->lcr & 0x80) { c->dlm = v; break; }
        c->ier = (uint8_t)(v & 0x0F);
        comm_update_irq(st, c);
        break;
    case COMM_IIR:                                 /* write side is FCR         */
        c->fcr = v;
        if (v & 0x02) { c->rx_head = c->rx_len = 0; c->lsr &= (uint8_t)~LSR_DR; }
        break;
    case COMM_LCR: c->lcr = v; break;
    case COMM_MCR:
        c->mcr = (uint8_t)(v & 0x1F);
        /* Out of loopback the lines are whatever the host asserts, and with
           nothing attached that is nothing -- NOT a convenient DSR+CTS. A guest
           that waits for CTS with no cable should wait, because that is what
           the hardware does; inventing the handshake is the "runs but lies"
           class. In loopback the lines come from MCR, which is the test. */
        comm_set_msr(c, (c->mcr & MCR_LOOP) ? comm_loop_msr(c->mcr) : 0x00);
        comm_update_irq(st, c);
        break;
    case COMM_LSR: break;                          /* read-only on real parts   */
    case COMM_MSR: break;
    case COMM_SCR: c->scr = v; break;
    default: break;
    }
}

/* ── INT 14h, BACKED BY THE PART RATHER THAN BY A GUESS. ─────────────────────
     The previous implementation lived in the host's BIOS block and answered
     receive with TIMEOUT unconditionally, because there was nothing to receive
     FROM. Now there is: the same FIFO the port registers use, so a byte pushed
     in by the host arrives whichever way the guest chooses to read it, and a
     byte sent in loopback comes back through INT 14h too.
   AH=00 initialise (AL = parameters), 01 send AL, 02 receive, 03 status.
   The answer is always AH = line status, AL = modem status or the character. */
static uint8_t comm_line_status(comm_port *c)
{
    /* BIOS line status is the UART's LSR with bit 7 redefined as TIMEOUT. */
    return (uint8_t)(c->lsr & 0x7F);
}

static void comm_int14(void *self, ntvdd_regs *r)
{
    comm_state *st = (comm_state *)self;
    unsigned ah = r_ah(r), al = r_al(r), pi = r_dx(r) & 0xFFFF;
    comm_port *c;
    if (pi >= COMM_MAX_PORTS || !st->p[pi].fitted) {
        /* No such port. TIMEOUT with everything else clear is what a BIOS
           reports for a port that is not there, and it is also the one answer
           that cannot make a polling guest wait forever. */
        s_ax(r, 0x8000);
        return;
    }
    c = &st->p[pi];
    switch (ah) {
    case 0x00: {                                   /* initialise                */
        /* AL packs baud (bits 7-5), parity (4-3), stop (2), word length (1-0).
           Store the divisor so a later INT 14h or a direct DLL/DLM read agrees
           with it -- the two routes describing the same port differently is
           exactly the kind of inconsistency that cost COMM.DRV a session. */
        static const uint16_t div_of[8] = { 1047, 768, 384, 192, 96, 48, 24, 12 };
        uint16_t d = div_of[(al >> 5) & 7];
        c->dll = (uint8_t)(d & 0xFF);
        c->dlm = (uint8_t)(d >> 8);
        c->lcr = (uint8_t)(al & 0x1F);
        s_ax(r, (uint16_t)((comm_line_status(c) << 8) | c->msr));
        break; }
    case 0x01:                                     /* send AL                   */
        ++c->tx_count;
        if (c->mcr & MCR_LOOP) comm_push_rx(c, (uint8_t)al);
        else if (st->sink)     st->sink(st->sink_ctx, (int)pi, (uint8_t)al);
        s_ax(r, (uint16_t)((comm_line_status(c) << 8) | al));
        break;
    case 0x02:                                     /* receive -> AL             */
        if (c->rx_len) {
            uint8_t b = comm_pop_rx(c);
            s_ax(r, (uint16_t)((comm_line_status(c) << 8) | b));
        } else {
            s_ax(r, 0x8000);                       /* TIMEOUT: nothing waiting  */
        }
        break;
    case 0x03:                                     /* status                    */
        s_ax(r, (uint16_t)((comm_line_status(c) << 8) | c->msr));
        break;
    default:
        s_ax(r, 0x8000);
        break;
    }
}

/* ── THE PARALLEL PORT. Three registers; the byte leaves on the STROBE EDGE. ─ */
static lpt_port *lpt_find(comm_state *st, uint16_t port, uint8_t *reg)
{
    int i;
    for (i = 0; i < LPT_MAX_PORTS; ++i) {
        lpt_port *l = &st->l[i];
        if (l->fitted && port >= l->base && port < (uint16_t)(l->base + 3)) {
            *reg = (uint8_t)(port - l->base);
            return l;
        }
    }
    return 0;
}

static void lpt_in(void *self, uint16_t port, uint8_t width, uint32_t *val)
{
    comm_state *st = (comm_state *)self;
    uint8_t reg = 0;
    lpt_port *l = lpt_find(st, port, &reg);
    (void)width;
    if (!l) { *val = 0xFF; return; }
    switch (reg) {
    case 0: *val = l->data; break;             /* the latch reads back         */
    case 1:
        /* Ready, online, no error, paper loaded, ACK idle. BUSY is INVERTED,
           so 1 here means NOT busy -- reporting 0 is the classic way to make
           every printing program hang on its first byte. */
        *val = (uint8_t)(LPT_ST_BUSY | LPT_ST_ACK | LPT_ST_SELECT | LPT_ST_ERROR);
        break;
    case 2: *val = l->ctrl; break;
    default: *val = 0xFF; break;
    }
}

static void lpt_out(void *self, uint16_t port, uint8_t width, uint32_t val)
{
    comm_state *st = (comm_state *)self;
    uint8_t reg = 0, v = (uint8_t)(val & 0xFF);
    lpt_port *l = lpt_find(st, port, &reg);
    (void)width;
    if (!l) return;
    switch (reg) {
    case 0: l->data = v; break;                /* latched, NOT yet printed     */
    case 1: break;                             /* status is read-only          */
    case 2: {
        /* The printer latches on the RISING edge of STROBE. Emitting on the
           data write instead would print a byte the program had only latched,
           and would print nothing at all for a driver that writes the same byte
           twice and strobes twice. */
        uint8_t was = l->ctrl;
        l->ctrl = v;
        if (!(was & LPT_CT_STROBE) && (v & LPT_CT_STROBE)) {
            ++l->bytes;
            if (st->lpt_sink) st->lpt_sink(st->lpt_sink_ctx, (int)(l - st->l), l->data);
        }
        break; }
    default: break;
    }
}

int vdd_lpt_fitted(const comm_state *st, int port)
{
    if (port < 0 || port >= LPT_MAX_PORTS) return 0;
    return st->l[port].fitted ? 1 : 0;
}

int vdd_comm_fitted(const comm_state *st, int port)
{
    if (port < 0 || port >= COMM_MAX_PORTS) return 0;
    return st->p[port].fitted ? 1 : 0;
}

int vdd_comm_rx(comm_state *st, int port, uint8_t byte)
{
    comm_port *c;
    if (port < 0 || port >= COMM_MAX_PORTS || !st->p[port].fitted) return -1;
    c = &st->p[port];
    if (c->rx_len >= COMM_RX_RING) { c->lsr |= LSR_OE; ++c->overruns; return -1; }
    comm_push_rx(c, byte);
    comm_update_irq(st, c);
    return 0;
}

void vdd_comm_reset(void *self)
{
    comm_state *st = (comm_state *)self;
    int i;
    for (i = 0; i < COMM_MAX_PORTS; ++i) {
        comm_port *c = &st->p[i];
        uint8_t base = c->base ? 1 : 0, fitted = c->fitted;
        uint16_t b = c->base; uint8_t q = c->irq;
        (void)base;
        c->ier = c->lcr = c->mcr = c->scr = c->fcr = 0;
        c->dll = 12; c->dlm = 0;                   /* 9600 baud, the POST value */
        c->rbr = 0; c->thre_pending = 0;
        c->rx_head = c->rx_len = 0;
        c->tx_count = c->rx_count = c->overruns = 0;
        /* THRE and TEMT set: the transmitter is empty on a part nobody has
           written to yet. A driver that polls THRE before its first write
           would otherwise hang before it ever sent a byte. */
        c->lsr = (uint8_t)(LSR_THRE | LSR_TEMT);
        c->msr = 0;
        c->base = b; c->irq = q; c->fitted = fitted;
    }
    for (i = 0; i < LPT_MAX_PORTS; ++i) {
        /* The DATA LATCH SURVIVES A RESET on real hardware -- it is a latch,
           not a register the reset line reaches -- but the control lines do
           not, and STROBE must come back LOW or the next write to the control
           register would look like a rising edge and print a stale byte. */
        st->l[i].ctrl = 0;
        st->l[i].bytes = 0;
    }
}

int vdd_comm_init(vdd_bus *b, void *self)
{
    comm_state *st = (comm_state *)self;
    int i, any = 0;
    st->bus = b;
    for (i = 0; i < COMM_MAX_PORTS; ++i) {
        comm_port *c = &st->p[i];
        if (!c->fitted) continue;
        if (vdd_claim_ports(b, c->base, (uint16_t)(c->base + 7),
                            comm_in, comm_out, st) != 0) {
            /* A refused claim must UNFIT the port, not leave it declared. The
               MPU-401 note on the bus says why: a device that thinks it is on
               the bus and is not gives the guest 0xFF from every register while
               every other layer keeps insisting the hardware is there. */
            c->fitted = 0;
            continue;
        }
        any = 1;
    }
    for (i = 0; i < LPT_MAX_PORTS; ++i) {
        lpt_port *l = &st->l[i];
        if (!l->fitted) continue;
        if (vdd_claim_ports(b, l->base, (uint16_t)(l->base + 2),
                            lpt_in, lpt_out, st) != 0) { l->fitted = 0; continue; }
        any = 1;
    }
    /* INT 14h is claimed even with no port fitted, so that "no such port"
       is answered by the part that knows, in one place, rather than by a
       fallback in the host that could disagree with it. */
    if (vdd_claim_int(b, 0x14, comm_int14, st) != 0) return -1;
    vdd_comm_reset(st);
    return any ? 0 : 0;
}
