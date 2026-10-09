/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * 8250/16550A serial port VDD.  (GH #9)
 *
 * The gap this closes is not "INT 14h is missing" -- INT 14h has answered since
 * GH #45. It is that there was NO UART. Ports 0x3F8..0x3FF were unclaimed, so a
 * guest that drives the hardware directly (which is every terminal program, every
 * mouse driver that probes for a serial mouse, and MSD) read 0xFF from every
 * register; and INT 14h's receive arm returned TIMEOUT unconditionally, so no
 * byte could ever arrive by either route. The port was declared in the equipment
 * word and could never be used.
 *
 * WHY LOOPBACK IS THE CENTRE OF THIS FILE, NOT A CURIOSITY:
 * MCR bit 4 is LOCAL LOOPBACK, and on real silicon it disconnects the pins and
 * feeds the transmitter back into the receiver. It is how every serial driver
 * ever written self-tests, it is what MSD does to decide a port exists, and it
 * needs no cable, no peer and no host device. That makes it the one part of a
 * UART whose correctness can be established completely, on a bare rig, with a
 * twelve-line DOS program -- so it is implemented exactly, including the modem
 * control lines looping to the modem status lines, rather than approximated.
 * A port that passes its own loopback test is a port a driver will believe in.
 *
 * WHAT BACKS IT WHEN LOOPBACK IS OFF:
 * A transmitted byte goes to an injected sink and received bytes are pushed in
 * by the host through VddCommReceive(). The device therefore has no idea whether it
 * is wired to a real \\.\COMn, a file, or nothing -- which is what lets
 * tests/unit/comm_test.c exercise the whole register model off-VM with no
 * host serial hardware at all. Pure C, no <windows.h>, same rule as vdd_mpu.
 *
 * [CAUTION]: NOT MODELLED, DELIBERATELY: baud rate has no effect on timing. The divisor
 * latch is stored and read back exactly (drivers check it, and MSD reports it)
 * but bytes move as fast as the guest can ask for them. Pacing a virtual UART
 * to 9600 baud would slow a guest down to no purpose -- there is no wire on
 * the other end whose timing has to be met. Said here so that a later reader
 * does not take the stored divisor for a rate limiter.
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#ifndef NTVDMEX_VDD_COMM_H
#define NTVDMEX_VDD_COMM_H

#include "vdd_bus.h"

/* FOUR SLOTS, BECAUSE THE PC HAS FOUR COM ADDRESSES. (GH #181):
 * COM1 3F8h/IRQ4, COM2 2F8h/IRQ3, COM3 3E8h/IRQ4, COM4 2E8h/IRQ3 -- the
 * IBM PC TechRef assignments, docs/ref/uart.md section 1. The device holds
 * all four and claims whichever the HOST marks fitted; it does not decide
 * how many ports the machine has. That decision is the host's, and it is
 * the one that has to agree with the equipment word (INT 11h bits 9-11) and
 * the BDA base table at 0040:0000 -- see main.c, which derives both from
 * VddCommIsFitted() rather than from a second list.
 *
 * [CAUTION]: COM3 AND COM4 SHARE A LINE WITH COM1 AND COM2. Nothing here arbitrates:
 * each port raises its own IRQ number and the PIC sees one edge per raise,
 * which is what two UARTs on one wire do. Whether the guest's drivers
 * cooperate over the shared line is their business, as it was in 1990.
 */
#define COMM_MAX_PORTS              4
#define COMM_COM1_BASE              0x03F8
#define COMM_COM2_BASE              0x02F8
#define COMM_COM3_BASE              0x03E8
#define COMM_COM4_BASE              0x02E8
#define COMM_COM1_IRQ               4       /* COM3 shares it */
#define COMM_COM2_IRQ               3       /* COM4 shares it */
#define LPT_DEFAULT_BASE            0x0378  /* LPT1: the one parallel port fitted */
#define COMM_DEVICE_NAME            "comm"
#define COMM_RECEIVE_RING_SIZE      256     /* Host input waiting for the guest */

/* register offsets from the port base */
#define COMM_RBR                    0       /* Read: receive buffer write: transmit holding */
#define COMM_IER                    1       /* Interrupt enable (DLM when LCR.DLAB) */
#define COMM_IIR                    2       /* Read: interrupt ident write: FIFO control */
#define COMM_LCR                    3       /* Line control (bit 7 = DLAB) */
#define COMM_MCR                    4       /* Modem control (bit 4 = LOOP) */
#define COMM_LSR                    5       /* Line status */
#define COMM_MSR                    6       /* Modem status */
#define COMM_SCR                    7       /* Scratch -- no function, but drivers PROBE with it */

/* LSR */
#define COMM_LSR_DATA_READY         0x01    /* Data ready */
#define COMM_LSR_OVERRUN            0x02    /* Overrun */
#define COMM_LSR_PARITY_ERROR       0x04
#define COMM_LSR_FRAMING_ERROR      0x08
#define COMM_LSR_BREAK              0x10
#define COMM_LSR_THR_EMPTY          0x20    /* Transmit holding empty */
#define COMM_LSR_TRANSMITTER_EMPTY  0x40    /* Transmitter empty */
#define COMM_LSR_FIFO_ERROR         0x80    /* FIFO mode: an error char is somewhere in the RCVR FIFO */

/* FCR (written at the IIR address) and LCR bits used by the #245 FIFO/break model */
#define COMM_FCR_ENABLE             0x01
#define COMM_FCR_RX_RESET           0x02
#define COMM_FCR_TX_RESET           0x04
#define COMM_FIFO_DEPTH             16      /* The 16550's receive FIFO */
#define COMM_LCR_BREAK              0x40    /* "set break": the TX line held spacing */

/* MCR */
#define COMM_MCR_DTR                0x01
#define COMM_MCR_RTS                0x02
#define COMM_MCR_OUT1               0x04
#define COMM_MCR_OUT2               0x08    /* Also gates the IRQ line on a PC */
#define COMM_MCR_LOOP               0x10

/* MSR: the low nibble is the DELTA bits, cleared by reading the register */
#define COMM_MSR_DELTA_CTS          0x01
#define COMM_MSR_DELTA_DSR          0x02
#define COMM_MSR_TRAILING_RI        0x04
#define COMM_MSR_DELTA_DCD          0x08
#define COMM_MSR_CTS                0x10
#define COMM_MSR_DSR                0x20
#define COMM_MSR_RI                 0x40
#define COMM_MSR_DCD                0x80

/* IER */
#define COMM_IER_RECEIVED_DATA      0x01    /* Received data available */
#define COMM_IER_THR_EMPTY          0x02    /* Transmitter holding empty */
#define COMM_IER_LINE_STATUS        0x04    /* Receiver line status */
#define COMM_IER_MODEM_STATUS       0x08    /* Modem status */

/* A byte the guest transmitted, on its way to whatever the host has attached.
 * `port` is the index, not the base -- the sink does not care where it lives.
 */
typedef VOID (*PCOMM_TX_SINK)(PVOID context, INT port, BYTE byte);

typedef struct _COMM_PORT
{
    WORD BasePort;
    BYTE  Irq;
    BYTE  IsFitted;            /* 0 = the ports are not claimed and read 0xFF */

    BYTE  Ier, Lcr, Mcr, Scr, Fcr;
    BYTE  Lsr, Msr;
    BYTE  DivisorLow, DivisorHigh;          /* divisor latch, stored and read back exactly */
    BYTE  Rbr;               /* the byte the guest will read next */
    BYTE  IsThrePending;      /* a THRE interrupt is owed, until IIR is read */

    BYTE  Receive[COMM_RECEIVE_RING_SIZE];
    WORD ReceiveHead, ReceiveLength;

    UINT32 TransmitCount, ReceiveCount, Overruns;
    UINT32 Breaks;            /* #245: break conditions the guest sent */
} COMM_PORT, *PCOMM_PORT;

typedef const COMM_PORT *PCCOMM_PORT;

/* THE PARALLEL PORT, WHICH IS THREE REGISTERS AND A STROBE:
 * INT 17h has printed to a spool file since GH #45, but the PORTS were
 * unclaimed -- so a program that drives the hardware directly read 0xFF from
 * 0x379 and saw BUSY low, PAPER OUT high and no ACK: a printer that is out of
 * paper and never ready. That is most DOS printing utilities and every
 * "print screen" TSR, because bit-banging the port is faster than INT 17h.
 * The Centronics handshake is the whole device: the program writes a byte to
 * the DATA register (0x378), then PULSES STROBE (bit 0 of the CONTROL
 * register, 0x37A) low-to-high, and the printer latches the byte on that
 * edge. So the byte is emitted on the STROBE EDGE, not on the data write --
 * getting that wrong prints every byte twice, or prints whatever was left in
 * the latch when the program only meant to read the status.
 *
 * [CAUTION]: STATUS BIT 7 (BUSY) IS INVERTED ON THE WIRE: a ready printer reads it as
 * 1. Bit 6 (ACK) is active low and pulses after each byte; we present it as
 * idle-high, which is what a polling driver reads between bytes.
 */
#define LPT_MAX_PORTS               1

#define LPT_STATUS_ERROR            0x08    /* Active low: 1 = no error */
#define LPT_STATUS_SELECT           0x10    /* 1 = printer selected/online */
#define LPT_STATUS_PAPER_OUT        0x20    /* 1 = OUT OF PAPER */
#define LPT_STATUS_ACK              0x40    /* Active low, pulses per byte; idle high */
#define LPT_STATUS_BUSY             0x80    /* INVERTED: 1 = not busy */

#define LPT_CONTROL_STROBE          0x01
#define LPT_CONTROL_AUTO_LINE_FEED  0x02
#define LPT_CONTROL_INIT            0x04    /* Active low */
#define LPT_CONTROL_SELECT          0x08
#define LPT_CONTROL_IRQ_ENABLE      0x10

typedef struct _LPT_PORT
{
    WORD BasePort;
    BYTE  IsFitted;
    BYTE  Data;           /* the output latch */
    BYTE  Control;
    UINT32 BytesPrinted;          /* strobed out (tests + diagnostics) */
} LPT_PORT, *PLPT_PORT;

typedef const LPT_PORT *PCLPT_PORT;

typedef struct _COMM_STATE
{
    PVDD_BUS Bus;
    COMM_PORT Ports[COMM_MAX_PORTS];
    LPT_PORT  Printers[LPT_MAX_PORTS];
    PCOMM_TX_SINK Sink;
    PVOID SinkContext;      /* serial */
    PCOMM_TX_SINK PrinterSink;
    PVOID PrinterSinkContext;  /* parallel */
} COMM_STATE, *PCOMM_STATE;

typedef const COMM_STATE *PCCOMM_STATE;

INT  VddCommInitialize(_In_ PVDD_BUS bus, _In_ PVOID context);
VOID VddCommReset(_In_ PVOID context);

/* Host -> guest. Returns 0 if the byte was queued, -1 if the ring was full (and
 * the overrun bit is set, because a UART that silently drops a byte is a UART
 * that makes a driver's flow control look broken).
 */
INT  VddCommReceive(_Inout_ PCOMM_STATE state, _In_ INT port, _In_ BYTE byte);

/* Is this port configured at all? INT 14h and the BDA both need to know, and
 * they must agree -- see the equipment-word note in main.c.
 */
INT  VddCommIsFitted(_In_ PCCOMM_STATE state, _In_ INT port);
INT  VddLptIsFitted(_In_ PCCOMM_STATE state, _In_ INT port);

static inline NTVDD_DEVICE VddCommDevice(_In_ PCOMM_STATE state)
{ NTVDD_DEVICE device;
device.Name = COMM_DEVICE_NAME;
device.Initialize = VddCommInitialize;
device.Reset = VddCommReset;
  device.Shutdown = 0;
  device.Context = state;
  return device; }

#endif /* NTVDMEX_VDD_COMM_H */
