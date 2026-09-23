# The 8250 / 16450 / 16550 serial port

**Spec:** National Semiconductor 8250/16450/16550A datasheets; IBM PC Technical
Reference for the port addresses and the IRQ wiring.
**Companion:** [`../inventory/uart.md`](../inventory/uart.md) — what *we* do about it.

> **The thesis.** Eight registers at eight consecutive ports, except that **there are
> ten**: one bit in the line-control register switches the first two addresses to a
> different pair. Nearly every UART bug is either that bank switch or the fact that
> **three of the registers are destructive to read**.

---

## 1. The ports

| COM | Base | IRQ |
|---|---|---|
| COM1 | `3F8h` | 4 |
| COM2 | `2F8h` | 3 |
| COM3 | `3E8h` | 4 |
| COM4 | `2E8h` | 3 |

⚠ **COM1 and COM3 share IRQ4; COM2 and COM4 share IRQ3.** Two ports on one line need
the drivers to cooperate, and most DOS-era ones do not.

## 2. The register file — and the bank switch

| Offset | Read | Write |
|---|---|---|
| 0 | **RBR** receive buffer | **THR** transmit holding |
| 1 | **IER** interrupt enable | IER |
| 2 | **IIR** interrupt identification | **FCR** FIFO control *(16550)* |
| 3 | **LCR** line control | LCR |
| 4 | **MCR** modem control | MCR |
| 5 | **LSR** line status | — |
| 6 | **MSR** modem status | — |
| 7 | **SCR** scratch | SCR |

⚠⚠ **With LCR bit 7 (DLAB) set, offsets 0 and 1 are not RBR/THR and IER — they are the
low and high halves of the baud-rate divisor.** A model that stores the bit without
switching the bank writes the divisor into the receive buffer and the interrupt enables,
which is silent until the first interrupt.

## 3. The registers that change when you read them

**LSR — line status.** Bit 0 DR (data ready), 1 OE (overrun), 2 PE (parity), 3 FE
(framing), 4 BI (break), **5 THRE** (transmit holding empty), **6 TEMT** (transmitter
completely empty).

- **Reading LSR clears the error bits** (OE/PE/FE/BI). DR and THRE are not errors and
  stay. That is what makes it a status register rather than a log — and it means two
  pieces of code polling LSR steal each other's errors.
- **THRE and TEMT are different and both are used.** A driver's *"may I transmit"* loop
  waits on THRE; a driver that drops RTS after its last byte waits on **TEMT**, because
  THRE only says the holding register is free, not that the last bit has left the wire. A
  model with one of the two serves one of those drivers and hangs the other.

**MSR — modem status.** Bits 7:4 are the live lines DCD, RI, DSR, CTS; **bits 3:0 are
the *delta* bits**, "this changed since you last looked", and **reading clears them**.

**IIR — interrupt identification.** Reading it *is* the acknowledgement for a
transmit-holding-empty interrupt. Priority order, highest first:

| Value | Source |
|---|---|
| `06h` | receiver line status (an error) |
| `04h` | received data available |
| `02h` | transmit holding empty — **cleared by this read** |
| `00h` | modem status change |
| `01h` | **nothing pending** (bit 0 set means *no* interrupt) |

⚠ **Bit 0 set means "nothing", not "something".** It is the one inverted bit in the part
and it is a classic misread.

**SCR — scratch.** It has no function whatsoever, and that is exactly why drivers use
it: **an 8250 does not have one and reads `FFh`; a 16450 and later store a byte.** It is
how the part is identified.

## 4. Loopback — the self-test every driver runs

**MCR bit 4** disconnects the pins and rewires the part to itself:

| | becomes |
|---|---|
| transmitter | receiver |
| DTR (MCR bit 0) | **DSR** (MSR bit 5) |
| RTS (MCR bit 1) | **CTS** (MSR bit 4) |
| OUT1 (MCR bit 2) | **RI** (MSR bit 6) |
| OUT2 (MCR bit 3) | **DCD** (MSR bit 7) |

A driver asserts DTR and RTS, checks DSR and CTS came back, writes a byte and checks it
returns. **Getting the pairing wrong gives a port that echoes bytes and still fails every
detection routine ever written** — a failure that looks like success right up until
nothing uses the port.

⚠ **MCR bit 3 (OUT2) is the interrupt gate on a PC.** The line is wired through a buffer
that OUT2 enables, so a port with OUT2 clear is correctly programmed, transmits and
receives fine, and **delivers no interrupts at all**.

---

## 5. What else is on the wire — and why a probe must not ask

A serial port's readings depend on **what is plugged into it**, which is a property of
the machine rather than of the chip. The sharpest example, and one this project measured
by accident: **a Microsoft serial mouse announces itself by sending `'M'` (`4Dh`) when
DTR and RTS are asserted.** Any test that toggles those lines on a machine with a mouse
attached will find a byte waiting afterwards, on a port it never wrote to.

⇒ Loopback exists precisely so a test can ask about the chip alone — but **the exit from
loopback re-applies MCR to the real pins**, and that transition is itself a stimulus. A
probe can isolate the chip while it is *inside* loopback and not on the way out.

---

## Sources

- **National Semiconductor 8250A / 16450 / 16550A datasheets** — the register file, the
  DLAB bank switch, the read-destructive registers, the IIR priority order, and loopback.
- **IBM PC Technical Reference** — the port and IRQ assignments and the OUT2 interrupt
  gate.
- **Microsoft Mouse Programmer's Reference** — the serial mouse identification protocol.
