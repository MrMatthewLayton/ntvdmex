/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * 8250/16550A serial port VDD.  (GH #9)  See vdd_comm.h.
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#include "vdd_comm.h"

/* The 8250/16550 registers and bits beyond those in vdd_comm.h. */
#define COMM_REGISTERS                  8       /* Base..base+7 */
#define COMM_LAST_REGISTER              7
#define COMM_LCR_DLAB                   0x80    /* divisor latch access */
#define COMM_IER_VALID_BITS             0x0F
#define COMM_MCR_VALID_BITS             0x1F
#define COMM_FCR_TRIGGER_SHIFT          6       /* FCR bits 7:6: the receive trigger level */
#define COMM_FCR_TRIGGER_MASK           3
#define COMM_TRIGGER_LEVELS             4
#define COMM_MSR_LINES                  0xF0    /* DCD RI DSR CTS */
#define COMM_MSR_DELTAS                 0x0F    /* Their change bits */
#define COMM_MSR_DELTA_SHIFT            4
#define COMM_NO_MODEM_LINES             0x00
#define COMM_BREAK_BYTE                 0x00    /* A break arrives as a NUL */

/* IIR: the pending interrupt, highest priority first. */
#define COMM_IIR_LINE_STATUS            0x06
#define COMM_IIR_RECEIVED_DATA          0x04
#define COMM_IIR_CHARACTER_TIMEOUT      0x0C
#define COMM_IIR_THR_EMPTY              0x02
#define COMM_IIR_MODEM_STATUS           0x00
#define COMM_IIR_NO_INTERRUPT           0x01
#define COMM_IIR_FIFOS_ENABLED          0xC0
#define COMM_UNDRIVEN_BUS               0xFF
#define COMM_LOW_BYTE                   0xFF
#define COMM_DEFAULT_DIVISOR            12      /* 9600 baud, the POST value */

/* INT 14h. */
#define COMM_INT14_INITIALIZE           0x00
#define COMM_INT14_SEND                 0x01
#define COMM_INT14_RECEIVE              0x02
#define COMM_INT14_STATUS               0x03
#define COMM_INT14_TIMEOUT              0x8000  /* AH bit 7: TIMEOUT */
#define COMM_INT14_LINE_STATUS_BITS     0x7F    /* LSR, less bit 7 (TIMEOUT to the BIOS) */
#define COMM_INT14_PORT_MASK            0xFFFF  /* DX = the port number */
#define COMM_INT14_BAUD_RATES           8
#define COMM_INT14_BAUD_SHIFT           5       /* AL bits 7-5: the baud rate */
#define COMM_INT14_BAUD_MASK            7
#define COMM_INT14_LINE_BITS            0x1F    /* AL bits 4-0: parity, stop, word length */

/* The parallel port's three registers. */
#define LPT_REGISTERS                   3
#define LPT_LAST_REGISTER               2
#define LPT_DATA_REGISTER               0
#define LPT_STATUS_REGISTER             1
#define LPT_CONTROL_REGISTER            2
#define COMM_FAILED                     (-1)

/* THE INTERRUPT IDENTIFICATION REGISTER IS A PRIORITY ENCODER:
 * Not a bit field -- a NUMBER, and the order is fixed by the part: receiver
 * line status (highest), then received-data-available, then transmitter
 * holding empty, then modem status. Bit 0 is INVERTED: 1 means "no interrupt
 * pending", which is the single most misread bit in the whole device.
 *
 * [CAUTION]: Reading IIR when THRE is the reason CLEARS that reason. This is not a
 * detail a driver can work around: the standard transmit loop is "write a
 * byte, take the interrupt, read IIR, write the next byte", and an IIR that
 * keeps reporting THRE forever gives an interrupt storm instead.
 */
/* #245: THE 16550's RECEIVE FIFO AND ITS TRIGGER LEVEL (PC16550D, FCR/IIR):
 * With the FIFO enabled (FCR bit 0) the part holds up to 16 received bytes and
 * "received data available" fires only when the count reaches the TRIGGER LEVEL
 * that FCR bits 6-7 select -- 1, 4, 8 or 14. Fewer than that waiting is reported
 * as the CHARACTER TIMEOUT interrupt (IIR 0x0C) once the line has been quiet for
 * four character times; our wire delivers instantly and is then quiet, so a
 * below-trigger remainder is owed a timeout as soon as it is there. Same priority
 * as RDA (second), which is what lets a driver drain the tail of a burst.
 *
 * [CAUTION]: The host ring behind it is the WIRE, not the FIFO: bytes wait there and enter
 * the FIFO as it drains, so a 16-byte FIFO never overruns on a burst the host
 * queued -- which is a line that paces itself, not a lost byte.
 */
static BYTE CommReceiveTrigger(PCCOMM_PORT uart)
{
    static const BYTE triggerLevels[COMM_TRIGGER_LEVELS] = { 1, 4, 8, 14 };
    return (uart->Fcr & COMM_FCR_ENABLE) ? triggerLevels[(uart->Fcr >> COMM_FCR_TRIGGER_SHIFT) & COMM_FCR_TRIGGER_MASK] : 1;
}

static BYTE CommInterruptIdentification(PCOMM_PORT uart)
{
    if ((uart->Ier & COMM_IER_LINE_STATUS)  && (uart->Lsr & (COMM_LSR_OVERRUN | COMM_LSR_PARITY_ERROR | COMM_LSR_FRAMING_ERROR | COMM_LSR_BREAK)))
        return COMM_IIR_LINE_STATUS;

    if ((uart->Ier & COMM_IER_RECEIVED_DATA)  && (uart->Lsr & COMM_LSR_DATA_READY))
    {
        WORD fifoCount = (WORD)(uart->ReceiveLength < COMM_FIFO_DEPTH ? uart->ReceiveLength : COMM_FIFO_DEPTH);

        if (!(uart->Fcr & COMM_FCR_ENABLE) || fifoCount >= CommReceiveTrigger(uart))
            return COMM_IIR_RECEIVED_DATA;

        return COMM_IIR_CHARACTER_TIMEOUT;                              /* character timeout (FIFO mode) */
    }

    if ((uart->Ier & COMM_IER_THR_EMPTY) && uart->IsThrePending)
        return COMM_IIR_THR_EMPTY;

    if ((uart->Ier & COMM_IER_MODEM_STATUS)   && (uart->Msr & COMM_MSR_DELTAS))
        return COMM_IIR_MODEM_STATUS;

    return COMM_IIR_NO_INTERRUPT;                                  /* bit 0 set = nothing owed */
}

/* OUT2 IS THE INTERRUPT GATE, AND IT GATES THE PIN, NOT THE PART:
 * Raise the line if anything is owed. OUT2 gates the IRQ on a PC -- the
 * UART's interrupt pin reaches the PIC through a buffer that OUT2 enables
 * (IBM PC TechRef; docs/ref/uart.md section 4) -- which is why every DOS
 * serial driver sets MCR bit 3 and why forgetting it here would make
 * interrupt-driven receive work in emulation and nowhere else.
 *
 * [INFO]: THE GATE IS OUTSIDE THE CHIP, SO IT STOPS ONLY THE PIC FROM HEARING.
 * With OUT2 clear the part still decides an interrupt is pending and IIR
 * still names it -- CommInterruptIdentification() never looks at MCR. A driver that polls IIR
 * with its line masked off this way reads the true source, as on hardware.
 *
 * [INFO]: OPENING THE GATE ONTO A PENDING SOURCE IS AN EDGE. The part's INTR pin is
 * already high; enabling the buffer is what lets the PIC see it rise. That
 * is why the MCR write below calls this function, and why setting OUT2 late
 * -- after IER, the usual driver order -- still delivers the interrupt that
 * was waiting.
 *
 * [INFO]: LOOPBACK CLOSES THE GATE. (#245, settled by the spec, s90) PC16550D, "Modem
 * Control Register", bit 4: in the diagnostic mode "the four modem control
 * outputs (DTR, RTS, OUT1 and OUT2) are internally connected to the four modem
 * control inputs, and the modem control output PINS are forced to their inactive
 * state". On an IBM-style card OUT2's PIN is what enables the IRQ buffer, so in
 * loopback the PIC hears nothing, whatever MCR bit 3 says -- the part still
 * decides, and IIR still names, what is owed. The datasheet's own note that
 * "the receiver and transmitter interrupts are fully operational" in loopback is
 * about the part, not the board. Super I/O chips that gate OUT2 internally do it
 * differently; the spec (8250/16550 + IBM TechRef wiring) outranks them here,
 * per the project's spec-first rule. This file used to honour MCR bit 3 in
 * loopback "rather than a new guess" -- it is no longer a guess.
 */
static VOID CommUpdateIrq(PCOMM_STATE state, PCOMM_PORT uart)
{
    if (!state->Bus || !uart->IsFitted)
        return;

    if (!(uart->Mcr & COMM_MCR_OUT2) || (uart->Mcr & COMM_MCR_LOOP))
        return;

    if (CommInterruptIdentification(uart) & COMM_IIR_NO_INTERRUPT)
        return;                                                                          /* nothing pending */

    VddRaiseIrq(state->Bus, uart->Irq);
}

/* LOOPBACK IS A REWIRING, NOT A FLAG:
 * With MCR bit 4 set the part disconnects its pins and connects, internally:
 * transmitter    -> receiver
 * DTR (MCR bit0) -> DSR (MSR bit5)
 * RTS (MCR bit1) -> CTS (MSR bit4)
 * OUT1(MCR bit2) -> RI  (MSR bit6)
 * OUT2(MCR bit3) -> DCD (MSR bit7)
 * Those four mappings are the whole self-test: a driver asserts DTR/RTS and
 * checks DSR/CTS came back. Getting the pairing wrong gives a port that
 * echoes bytes and still fails every detection routine ever written, which is
 * a failure that looks like success right up until nothing uses the port.
 */
static BYTE CommLoopbackModemStatus(BYTE modemControl)
{
    BYTE lines = 0;

    if (modemControl & COMM_MCR_DTR)
        lines |= COMM_MSR_DSR;

    if (modemControl & COMM_MCR_RTS)
        lines |= COMM_MSR_CTS;

    if (modemControl & COMM_MCR_OUT1)
        lines |= COMM_MSR_RI;

    if (modemControl & COMM_MCR_OUT2)
        lines |= COMM_MSR_DCD;

    return lines;
}

/* Set the modem status, recording which lines CHANGED in the delta nibble.
 * A delta bit, once set, stays set until the guest reads MSR -- that read is
 * the acknowledgement, and it is also what clears a modem-status interrupt.
 */
static VOID CommSetModemStatus(PCOMM_PORT uart, BYTE lines)
{
    BYTE oldLines = (BYTE)(uart->Msr & COMM_MSR_LINES);
    BYTE deltas   = (BYTE)((oldLines ^ (lines & COMM_MSR_LINES)) >> COMM_MSR_DELTA_SHIFT);

    /* RI has no "changed" bit -- it has TERI, "trailing edge of ring
     * indicator", which is set only on a 1 -> 0 transition.
     */
    if (deltas & COMM_MSR_TRAILING_RI)
    {
        deltas &= (BYTE)~COMM_MSR_TRAILING_RI;

        if (oldLines & COMM_MSR_RI)
            deltas |= COMM_MSR_TRAILING_RI;
    }

    uart->Msr = (BYTE)((lines & COMM_MSR_LINES) | ((uart->Msr & COMM_MSR_DELTAS) | deltas));
}

static VOID CommPushReceive(PCOMM_PORT uart, BYTE value)
{
    if (uart->ReceiveLength >= COMM_RECEIVE_RING_SIZE)
    {
        uart->Lsr |= COMM_LSR_OVERRUN;
        return;
    }

    uart->Receive[(uart->ReceiveHead + uart->ReceiveLength) % COMM_RECEIVE_RING_SIZE] = value;
    ++uart->ReceiveLength;
    uart->Lsr |= COMM_LSR_DATA_READY;
    ++uart->ReceiveCount;
}

static BYTE CommPopReceive(PCOMM_PORT uart)
{
    BYTE value;

    if (!uart->ReceiveLength)
        return uart->Rbr;

    value = uart->Receive[uart->ReceiveHead];
    uart->ReceiveHead = (WORD)((uart->ReceiveHead + 1) % COMM_RECEIVE_RING_SIZE);
    --uart->ReceiveLength;

    if (!uart->ReceiveLength)
        uart->Lsr &= (BYTE)~COMM_LSR_DATA_READY;

    uart->Rbr = value;
    return value;
}

static PCOMM_PORT CommFind(PCOMM_STATE state, WORD port, BYTE *registerIndex)
{
    INT portIndex;

    for (portIndex = 0; portIndex < COMM_MAX_PORTS; ++portIndex)
    {
        PCOMM_PORT uart = &state->Ports[portIndex];

        if (uart->IsFitted && port >= uart->BasePort && port < (WORD)(uart->BasePort + COMM_REGISTERS))
        {
            *registerIndex = (BYTE)(port - uart->BasePort);
            return uart;
        }
    }

    return 0;
}

static VOID CommPortIn(PVOID context, WORD port, BYTE width, UINT32 *value)
{
    PCOMM_STATE state = (PCOMM_STATE)context;
    BYTE registerIndex = 0;
    PCOMM_PORT uart = CommFind(state, port, &registerIndex);

    (VOID)width;

    if (!uart)
    {
        *value = COMM_UNDRIVEN_BUS;
        return;
    }

    switch (registerIndex)
    {
    case COMM_RBR:
        if (uart->Lcr & COMM_LCR_DLAB)
        {
            *value = uart->DivisorLow;
            break;
        }

        *value = CommPopReceive(uart);
        break;

    case COMM_IER:
        *value = (uart->Lcr & COMM_LCR_DLAB) ? uart->DivisorHigh : uart->Ier;
        break;

    case COMM_IIR:
    {
        BYTE identification = CommInterruptIdentification(uart);

        if (identification == COMM_IIR_THR_EMPTY)
            uart->IsThrePending = 0;                                              /* the read IS the clear */

        /* Bits 6-7 report an ENABLED FIFO. Answering 0xC0 when the guest never
         * enabled one would tell a 16550-aware driver it may write 16 bytes
         * between interrupts on a part that is behaving like an 8250.
         */
        if (uart->Fcr & COMM_FCR_ENABLE)
            identification |= COMM_IIR_FIFOS_ENABLED;

        *value = identification;
        break; }

    case COMM_LCR:
        *value = uart->Lcr;
        break;

    case COMM_MCR:
        *value = uart->Mcr;
        break;

    case COMM_LSR:
        *value = uart->Lsr;
        /* Reading LSR clears the error bits -- that is what makes it a status
         * register rather than a log. DR and THRE are NOT errors and stay.
         */
        uart->Lsr &= (BYTE)~(COMM_LSR_OVERRUN | COMM_LSR_PARITY_ERROR | COMM_LSR_FRAMING_ERROR | COMM_LSR_BREAK | COMM_LSR_FIFO_ERROR);
        break;

    case COMM_MSR:
        *value = uart->Msr;
        uart->Msr &= COMM_MSR_LINES;                            /* the read acknowledges */
        break;

    case COMM_SCR:
        *value = uart->Scr;
        break;

    default:
        *value = COMM_UNDRIVEN_BUS;
        break;
    }
}

static VOID CommPortOut(PVOID context, WORD port, BYTE width, UINT32 value)
{
    PCOMM_STATE state = (PCOMM_STATE)context;
    BYTE registerIndex = 0;
    BYTE byteValue = (BYTE)(value & COMM_LOW_BYTE);
    PCOMM_PORT uart = CommFind(state, port, &registerIndex);

    (VOID)width;

    if (!uart)
        return;

    switch (registerIndex)
    {
    case COMM_RBR:                                 /* transmit holding */
        if (uart->Lcr & COMM_LCR_DLAB)
        {
            uart->DivisorLow = byteValue;
            break;
        }

        ++uart->TransmitCount;
        /* [CAUTION]: THE TRANSMITTER IS NEVER BUSY HERE, and that is a decision. A real
         * part clears THRE while the byte shifts out; we complete instantly,
         * so THRE stays set and a polling driver never waits. See the baud
         * note in the header -- there is no wire whose timing must be met.
         */
        if (uart->Mcr & COMM_MCR_LOOP)
            CommPushReceive(uart, byteValue);
        else if (state->Sink)
            state->Sink(state->SinkContext, (INT)(uart - state->Ports), byteValue);

        uart->IsThrePending = 1;
        CommUpdateIrq(state, uart);
        break;

    case COMM_IER:
        if (uart->Lcr & COMM_LCR_DLAB)
        {
            uart->DivisorHigh = byteValue;
            break;
        }

        /* #245: ENABLING THRE WITH THE HOLDING REGISTER ALREADY EMPTY IS ITSELF A
         * THRE INTERRUPT (PC16550D: the source is "THRE set AND ETBEI set", so the
         * 0->1 write makes it true). Drivers rely on it -- the classic kick-start
         * of an interrupt-driven transmit is "fill the queue, set ETBEI, let the
         * ISR send the first byte" -- and without it nothing is ever sent.
         */
        if (!(uart->Ier & COMM_IER_THR_EMPTY) && (byteValue & COMM_IER_THR_EMPTY) && (uart->Lsr & COMM_LSR_THR_EMPTY))
            uart->IsThrePending = 1;

        uart->Ier = (BYTE)(byteValue & COMM_IER_VALID_BITS);
        CommUpdateIrq(state, uart);
        break;

    case COMM_IIR:                                 /* write side is FCR */
        uart->Fcr = byteValue;

        if (byteValue & COMM_FCR_RX_RESET)
        {
            uart->ReceiveHead = uart->ReceiveLength = 0;
            uart->Lsr &= (BYTE)~COMM_LSR_DATA_READY;
        }

        break;

    case COMM_LCR:
    {
        /* #245: SET BREAK (bit 6) holds the TX line spacing. In loopback the
         * receiver sees it: ONE zero character enters the FIFO with BI set
         * (PC16550D, LSR bit 4: "only one zero character is loaded into the FIFO"),
         * which raises the receiver-line-status interrupt. Out of loopback it is a
         * line condition with no byte to send; it is counted, not spooled.
         */
        BYTE previousLcr = uart->Lcr;
        uart->Lcr = byteValue;

        if (!(previousLcr & COMM_LCR_BREAK) && (byteValue & COMM_LCR_BREAK))
        {
            ++uart->Breaks;

            if (uart->Mcr & COMM_MCR_LOOP)
            {
                CommPushReceive(uart, COMM_BREAK_BYTE);
                uart->Lsr |= COMM_LSR_BREAK;

                if (uart->Fcr & COMM_FCR_ENABLE)
                    uart->Lsr |= COMM_LSR_FIFO_ERROR;

                CommUpdateIrq(state, uart);
            }
        }

        break; }

    case COMM_MCR:
        uart->Mcr = (BYTE)(byteValue & COMM_MCR_VALID_BITS);
        /* Out of loopback the lines are whatever the host asserts, and with
         * nothing attached that is nothing -- NOT a convenient DSR+CTS. A guest
         * that waits for CTS with no cable should wait, because that is what
         * the hardware does; inventing the handshake is the "runs but lies"
         * class. In loopback the lines come from MCR, which is the test.
         */
        CommSetModemStatus(uart, (uart->Mcr & COMM_MCR_LOOP) ? CommLoopbackModemStatus(uart->Mcr) : COMM_NO_MODEM_LINES);
        CommUpdateIrq(state, uart);
        break;

    case COMM_LSR:
        break;                          /* read-only on real parts */

    case COMM_MSR:
        break;

    case COMM_SCR:
        uart->Scr = byteValue;
        break;

    default:
        break;
    }
}

/* INT 14h, BACKED BY THE PART RATHER THAN BY A GUESS:
 * The previous implementation lived in the host's BIOS block and answered
 * receive with TIMEOUT unconditionally, because there was nothing to receive
 * FROM. Now there is: the same FIFO the port registers use, so a byte pushed
 * in by the host arrives whichever way the guest chooses to read it, and a
 * byte sent in loopback comes back through INT 14h too.
 * AH=00 initialise (AL = parameters), 01 send AL, 02 receive, 03 status.
 * The answer is always AH = line status, AL = modem status or the character.
 */
static BYTE CommLineStatus(PCOMM_PORT uart)
{
    /* BIOS line status is the UART's LSR with bit 7 redefined as TIMEOUT. */
    return (BYTE)(uart->Lsr & COMM_INT14_LINE_STATUS_BITS);
}

static VOID CommInt14(PVOID context, PNTVDD_REGISTERS registers)
{
    PCOMM_STATE state = (PCOMM_STATE)context;
    UINT functionCode = VddGetAh(registers);
    UINT argumentByte = VddGetAl(registers);
    UINT portIndex = VddGetDx(registers) & COMM_INT14_PORT_MASK;
    PCOMM_PORT uart;

    if (portIndex >= COMM_MAX_PORTS || !state->Ports[portIndex].IsFitted)
    {
        /* No such port. TIMEOUT with everything else clear is what a BIOS
         * reports for a port that is not there, and it is also the one answer
         * that cannot make a polling guest wait forever.
         */
        VddSetAx(registers, COMM_INT14_TIMEOUT);
        return;
    }

    uart = &state->Ports[portIndex];

    switch (functionCode)
    {
    case COMM_INT14_INITIALIZE:                                     /* initialise */
    {
        /* AL packs baud (bits 7-5), parity (4-3), stop (2), word length (1-0).
         * Store the divisor so a later INT 14h or a direct DLL/DLM read agrees
         * with it -- the two routes describing the same port differently is
         * exactly the kind of inconsistency that cost COMM.DRV a session.
         */
        static const WORD baudDivisors[COMM_INT14_BAUD_RATES] = { 1047, 768, 384, 192, 96, 48, 24, 12 };
        WORD divisor = baudDivisors[(argumentByte >> COMM_INT14_BAUD_SHIFT) & COMM_INT14_BAUD_MASK];
        uart->DivisorLow = (BYTE)(divisor & COMM_LOW_BYTE);
        uart->DivisorHigh = (BYTE)(divisor >> BYTE_SHIFT);
        uart->Lcr = (BYTE)(argumentByte & COMM_INT14_LINE_BITS);
        VddSetAx(registers, (WORD)((CommLineStatus(uart) << BYTE_SHIFT) | uart->Msr));
        break; }

    case COMM_INT14_SEND:                                     /* send AL */
        ++uart->TransmitCount;

        if (uart->Mcr & COMM_MCR_LOOP)
            CommPushReceive(uart, (BYTE)argumentByte);
        else if (state->Sink)
            state->Sink(state->SinkContext, (INT)portIndex, (BYTE)argumentByte);

        /* #245: the same byte through the BIOS is the same write to THR -- it owes
         * the same THRE interrupt the port write does.
         */
        uart->IsThrePending = 1;
        CommUpdateIrq(state, uart);
        VddSetAx(registers, (WORD)((CommLineStatus(uart) << BYTE_SHIFT) | argumentByte));
        break;

    case COMM_INT14_RECEIVE:                                     /* receive -> AL */
        if (uart->ReceiveLength)
        {
            BYTE received = CommPopReceive(uart);
            VddSetAx(registers, (WORD)((CommLineStatus(uart) << BYTE_SHIFT) | received));
        }
        else
        {
            VddSetAx(registers, COMM_INT14_TIMEOUT);                       /* TIMEOUT: nothing waiting */
        }

        break;

    case COMM_INT14_STATUS:                                     /* status */
        VddSetAx(registers, (WORD)((CommLineStatus(uart) << BYTE_SHIFT) | uart->Msr));
        break;

    default:
        VddSetAx(registers, COMM_INT14_TIMEOUT);
        break;
    }
}

/* THE PARALLEL PORT. Three registers; the byte leaves on the STROBE EDGE: */
static PLPT_PORT LptFind(PCOMM_STATE state, WORD port, BYTE *registerIndex)
{
    INT portIndex;

    for (portIndex = 0; portIndex < LPT_MAX_PORTS; ++portIndex)
    {
        PLPT_PORT printer = &state->Printers[portIndex];

        if (printer->IsFitted && port >= printer->BasePort && port < (WORD)(printer->BasePort + LPT_REGISTERS))
        {
            *registerIndex = (BYTE)(port - printer->BasePort);
            return printer;
        }
    }

    return 0;
}

static VOID LptPortIn(PVOID context, WORD port, BYTE width, UINT32 *value)
{
    PCOMM_STATE state = (PCOMM_STATE)context;
    BYTE registerIndex = 0;
    PLPT_PORT printer = LptFind(state, port, &registerIndex);

    (VOID)width;

    if (!printer)
    {
        *value = COMM_UNDRIVEN_BUS;
        return;
    }

    switch (registerIndex)
    {
    case LPT_DATA_REGISTER:
        *value = printer->Data;
        break;             /* the latch reads back */

    case LPT_STATUS_REGISTER:
        /* Ready, online, no error, paper loaded, ACK idle. BUSY is INVERTED,
         * so 1 here means NOT busy -- reporting 0 is the classic way to make
         * every printing program hang on its first byte.
         */
        *value = (BYTE)(LPT_STATUS_BUSY | LPT_STATUS_ACK | LPT_STATUS_SELECT | LPT_STATUS_ERROR);
        break;

    case LPT_CONTROL_REGISTER:
        *value = printer->Control;
        break;

    default:
        *value = COMM_UNDRIVEN_BUS;
        break;
    }
}

static VOID LptPortOut(PVOID context, WORD port, BYTE width, UINT32 value)
{
    PCOMM_STATE state = (PCOMM_STATE)context;
    BYTE registerIndex = 0;
    BYTE byteValue = (BYTE)(value & COMM_LOW_BYTE);
    PLPT_PORT printer = LptFind(state, port, &registerIndex);

    (VOID)width;

    if (!printer)
        return;

    switch (registerIndex)
    {
    case LPT_DATA_REGISTER:
        printer->Data = byteValue;
        break;                /* latched, NOT yet printed */

    case LPT_STATUS_REGISTER:
        break;                             /* status is read-only */

    case LPT_CONTROL_REGISTER:
    {
        /* The printer latches on the RISING edge of STROBE. Emitting on the
         * data write instead would print a byte the program had only latched,
         * and would print nothing at all for a driver that writes the same byte
         * twice and strobes twice.
         */
        BYTE previousControl = printer->Control;
        printer->Control = byteValue;

        if (!(previousControl & LPT_CONTROL_STROBE) && (byteValue & LPT_CONTROL_STROBE))
        {
            ++printer->BytesPrinted;

            if (state->PrinterSink)
                state->PrinterSink(state->PrinterSinkContext, (INT)(printer - state->Printers), printer->Data);
        }

        break; }

    default:
        break;
    }
}

INT VddLptIsFitted(PCCOMM_STATE state, INT port)
{
    if (port < 0 || port >= LPT_MAX_PORTS)
        return 0;

    return state->Printers[port].IsFitted ? 1 : 0;
}

INT VddCommIsFitted(PCCOMM_STATE state, INT port)
{
    if (port < 0 || port >= COMM_MAX_PORTS)
        return 0;

    return state->Ports[port].IsFitted ? 1 : 0;
}

INT VddCommReceive(PCOMM_STATE state, INT port, BYTE byte)
{
    PCOMM_PORT uart;

    if (port < 0 || port >= COMM_MAX_PORTS || !state->Ports[port].IsFitted)
        return -1;

    uart = &state->Ports[port];

    if (uart->ReceiveLength >= COMM_RECEIVE_RING_SIZE)
    {
        uart->Lsr |= COMM_LSR_OVERRUN;
        ++uart->Overruns;
        return -1;
    }

    CommPushReceive(uart, byte);
    CommUpdateIrq(state, uart);
    return 0;
}

VOID VddCommReset(PVOID context)
{
    PCOMM_STATE state = (PCOMM_STATE)context;
    INT portIndex;

    for (portIndex = 0; portIndex < COMM_MAX_PORTS; ++portIndex)
    {
        PCOMM_PORT uart = &state->Ports[portIndex];
        BYTE hasBase = uart->BasePort ? 1 : 0;
        BYTE fitted = uart->IsFitted;
        WORD basePort = uart->BasePort;
        BYTE irq = uart->Irq;
        (VOID)hasBase;
        uart->Ier = uart->Lcr = uart->Mcr = uart->Scr = uart->Fcr = 0;
        uart->DivisorLow = COMM_DEFAULT_DIVISOR;
        uart->DivisorHigh = 0;                   /* 9600 baud, the POST value */
        uart->Rbr = 0;
        uart->IsThrePending = 0;
        uart->ReceiveHead = uart->ReceiveLength = 0;
        uart->TransmitCount = uart->ReceiveCount = uart->Overruns = 0;
        uart->Breaks = 0;
        /* THRE and TEMT set: the transmitter is empty on a part nobody has
         * written to yet. A driver that polls THRE before its first write
         * would otherwise hang before it ever sent a byte.
         */
        uart->Lsr = (BYTE)(COMM_LSR_THR_EMPTY | COMM_LSR_TRANSMITTER_EMPTY);
        uart->Msr = 0;
        uart->BasePort = basePort;
        uart->Irq = irq;
        uart->IsFitted = fitted;
    }

    for (portIndex = 0; portIndex < LPT_MAX_PORTS; ++portIndex)
    {
        /* The DATA LATCH SURVIVES A RESET on real hardware -- it is a latch,
         * not a register the reset line reaches -- but the control lines do
         * not, and STROBE must come back LOW or the next write to the control
         * register would look like a rising edge and print a stale byte.
         */
        state->Printers[portIndex].Control = 0;
        state->Printers[portIndex].BytesPrinted = 0;
    }
}

INT VddCommInitialize(PVDD_BUS bus, PVOID context)
{
    PCOMM_STATE state = (PCOMM_STATE)context;
    INT portIndex;
    INT anyFitted = 0;

    state->Bus = bus;

    for (portIndex = 0; portIndex < COMM_MAX_PORTS; ++portIndex)
    {
        PCOMM_PORT uart = &state->Ports[portIndex];

        if (!uart->IsFitted)
            continue;

        if (VddClaimPorts(bus, uart->BasePort, (WORD)(uart->BasePort + COMM_LAST_REGISTER),
                            CommPortIn, CommPortOut, state) != 0)
        {
            /* A refused claim must UNFIT the port, not leave it declared. The
             * MPU-401 note on the bus says why: a device that thinks it is on
             * the bus and is not gives the guest 0xFF from every register while
             * every other layer keeps insisting the hardware is there.
             */
            uart->IsFitted = 0;
            continue;
        }

        anyFitted = 1;
    }

    for (portIndex = 0; portIndex < LPT_MAX_PORTS; ++portIndex)
    {
        PLPT_PORT printer = &state->Printers[portIndex];

        if (!printer->IsFitted)
            continue;

        if (VddClaimPorts(bus, printer->BasePort, (WORD)(printer->BasePort + LPT_LAST_REGISTER),
                            LptPortIn, LptPortOut, state) != 0)
        {
            printer->IsFitted = 0;
            continue;
        }

        anyFitted = 1;
    }

    /* INT 14h is claimed even with no port fitted, so that "no such port"
     * is answered by the part that knows, in one place, rather than by a
     * fallback in the host that could disagree with it.
     */
    if (VddClaimInterrupt(bus, VECTOR_SERIAL, CommInt14, state) != 0)
        return COMM_FAILED;

    VddCommReset(state);
    return anyFitted ? 0 : 0;
}
