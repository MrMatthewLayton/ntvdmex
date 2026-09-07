/*
 * vdd_comm.h -- 8250/16550A serial port VDD.  (GH #9)
 *
 * The gap this closes is not "INT 14h is missing" -- INT 14h has answered since
 * GH #45. It is that there was NO UART. Ports 0x3F8..0x3FF were unclaimed, so a
 * guest that drives the hardware directly (which is every terminal program, every
 * mouse driver that probes for a serial mouse, and MSD) read 0xFF from every
 * register; and INT 14h's receive arm returned TIMEOUT unconditionally, so no
 * byte could ever arrive by either route. The port was declared in the equipment
 * word and could never be used.
 *
 * ── WHY LOOPBACK IS THE CENTRE OF THIS FILE, NOT A CURIOSITY ────────────────
 * MCR bit 4 is LOCAL LOOPBACK, and on real silicon it disconnects the pins and
 * feeds the transmitter back into the receiver. It is how every serial driver
 * ever written self-tests, it is what MSD does to decide a port exists, and it
 * needs no cable, no peer and no host device. That makes it the one part of a
 * UART whose correctness can be established completely, on a bare rig, with a
 * twelve-line DOS program -- so it is implemented exactly, including the modem
 * control lines looping to the modem status lines, rather than approximated.
 * A port that passes its own loopback test is a port a driver will believe in.
 *
 * ── WHAT BACKS IT WHEN LOOPBACK IS OFF ──────────────────────────────────────
 * A transmitted byte goes to an injected sink and received bytes are pushed in
 * by the host through vdd_comm_rx(). The device therefore has no idea whether it
 * is wired to a real \\.\COMn, a file, or nothing -- which is what lets
 * tools/dostest/comm_test.c exercise the whole register model off-VM with no
 * host serial hardware at all. Pure C, no <windows.h>, same rule as vdd_mpu.
 *
 * ⚠ NOT MODELLED, DELIBERATELY: baud rate has no effect on timing. The divisor
 *   latch is stored and read back exactly (drivers check it, and MSD reports it)
 *   but bytes move as fast as the guest can ask for them. Pacing a virtual UART
 *   to 9600 baud would slow a guest down to no purpose -- there is no wire on
 *   the other end whose timing has to be met. Said here so that a later reader
 *   does not take the stored divisor for a rate limiter.
 */
#ifndef NTVDMEX_VDD_COMM_H
#define NTVDMEX_VDD_COMM_H

#include "vdd_bus.h"

#define COMM_MAX_PORTS 2
#define COMM_RX_RING   256      /* host input waiting for the guest             */

/* register offsets from the port base */
#define COMM_RBR 0   /* read: receive buffer      write: transmit holding       */
#define COMM_IER 1   /* interrupt enable          (DLM when LCR.DLAB)           */
#define COMM_IIR 2   /* read: interrupt ident     write: FIFO control           */
#define COMM_LCR 3   /* line control (bit 7 = DLAB)                             */
#define COMM_MCR 4   /* modem control (bit 4 = LOOP)                            */
#define COMM_LSR 5   /* line status                                             */
#define COMM_MSR 6   /* modem status                                            */
#define COMM_SCR 7   /* scratch -- no function, but drivers PROBE with it       */

/* LSR */
#define LSR_DR   0x01   /* data ready                                           */
#define LSR_OE   0x02   /* overrun                                              */
#define LSR_PE   0x04
#define LSR_FE   0x08
#define LSR_BI   0x10
#define LSR_THRE 0x20   /* transmit holding empty                               */
#define LSR_TEMT 0x40   /* transmitter empty                                    */

/* MCR */
#define MCR_DTR  0x01
#define MCR_RTS  0x02
#define MCR_OUT1 0x04
#define MCR_OUT2 0x08   /* also gates the IRQ line on a PC                      */
#define MCR_LOOP 0x10

/* MSR: the low nibble is the DELTA bits, cleared by reading the register */
#define MSR_DCTS 0x01
#define MSR_DDSR 0x02
#define MSR_TERI 0x04
#define MSR_DDCD 0x08
#define MSR_CTS  0x10
#define MSR_DSR  0x20
#define MSR_RI   0x40
#define MSR_DCD  0x80

/* IER */
#define IER_RDA  0x01   /* received data available                              */
#define IER_THRE 0x02   /* transmitter holding empty                            */
#define IER_RLS  0x04   /* receiver line status                                 */
#define IER_MS   0x08   /* modem status                                         */

/* A byte the guest transmitted, on its way to whatever the host has attached.
   `port` is the index, not the base -- the sink does not care where it lives. */
typedef void (*comm_tx_sink)(void *ctx, int port, uint8_t byte);

typedef struct comm_port {
    uint16_t base;
    uint8_t  irq;
    uint8_t  fitted;            /* 0 = the ports are not claimed and read 0xFF  */

    uint8_t  ier, lcr, mcr, scr, fcr;
    uint8_t  lsr, msr;
    uint8_t  dll, dlm;          /* divisor latch, stored and read back exactly  */
    uint8_t  rbr;               /* the byte the guest will read next            */
    uint8_t  thre_pending;      /* a THRE interrupt is owed, until IIR is read  */

    uint8_t  rx[COMM_RX_RING];
    uint16_t rx_head, rx_len;

    uint32_t tx_count, rx_count, overruns;
} comm_port;

/* ── THE PARALLEL PORT, WHICH IS THREE REGISTERS AND A STROBE. ───────────────
     INT 17h has printed to a spool file since GH #45, but the PORTS were
     unclaimed -- so a program that drives the hardware directly read 0xFF from
     0x379 and saw BUSY low, PAPER OUT high and no ACK: a printer that is out of
     paper and never ready. That is most DOS printing utilities and every
     "print screen" TSR, because bit-banging the port is faster than INT 17h.
     The Centronics handshake is the whole device: the program writes a byte to
     the DATA register (0x378), then PULSES STROBE (bit 0 of the CONTROL
     register, 0x37A) low-to-high, and the printer latches the byte on that
     edge. So the byte is emitted on the STROBE EDGE, not on the data write --
     getting that wrong prints every byte twice, or prints whatever was left in
     the latch when the program only meant to read the status.
   ⚠ STATUS BIT 7 (BUSY) IS INVERTED ON THE WIRE: a ready printer reads it as
     1. Bit 6 (ACK) is active low and pulses after each byte; we present it as
     idle-high, which is what a polling driver reads between bytes. */
#define LPT_MAX_PORTS 1

#define LPT_ST_ERROR  0x08   /* active low: 1 = no error                       */
#define LPT_ST_SELECT 0x10   /* 1 = printer selected/online                    */
#define LPT_ST_PAPER  0x20   /* 1 = OUT OF PAPER                               */
#define LPT_ST_ACK    0x40   /* active low, pulses per byte; idle high         */
#define LPT_ST_BUSY   0x80   /* INVERTED: 1 = not busy                         */

#define LPT_CT_STROBE 0x01
#define LPT_CT_AUTOLF 0x02
#define LPT_CT_INIT   0x04   /* active low                                     */
#define LPT_CT_SELECT 0x08
#define LPT_CT_IRQEN  0x10

typedef struct lpt_port {
    uint16_t base;
    uint8_t  fitted;
    uint8_t  data;           /* the output latch                               */
    uint8_t  ctrl;
    uint32_t bytes;          /* strobed out (tests + diagnostics)              */
} lpt_port;

typedef struct comm_state {
    vdd_bus *bus;
    comm_port p[COMM_MAX_PORTS];
    lpt_port  l[LPT_MAX_PORTS];
    comm_tx_sink sink;     void *sink_ctx;      /* serial                      */
    comm_tx_sink lpt_sink; void *lpt_sink_ctx;  /* parallel                    */
} comm_state;

int  vdd_comm_init(vdd_bus *b, void *self);
void vdd_comm_reset(void *self);

/* Host -> guest. Returns 0 if the byte was queued, -1 if the ring was full (and
   the overrun bit is set, because a UART that silently drops a byte is a UART
   that makes a driver's flow control look broken). */
int  vdd_comm_rx(comm_state *st, int port, uint8_t byte);

/* Is this port configured at all? INT 14h and the BDA both need to know, and
   they must agree -- see the equipment-word note in main.c. */
int  vdd_comm_fitted(const comm_state *st, int port);
int  vdd_lpt_fitted (const comm_state *st, int port);

static inline ntvdd vdd_comm_device(comm_state *st)
{ ntvdd d; d.name = "comm"; d.init = vdd_comm_init; d.reset = vdd_comm_reset;
  d.shutdown = 0; d.self = st; return d; }

#endif /* NTVDMEX_VDD_COMM_H */
