# Inventory — 8250 / 16450 / 16550 serial port

**Spec:** National Semiconductor 8250/16450/16550A datasheets; IBM PC TechRef.
**▶ The hardware reference is [`../ref/uart.md`](../ref/uart.md)** — what the chip *does*.
**Our implementation:** `src/vdd/vdd_comm.c` (419 lines), `src/vdd/vdd_comm.h`.
**Oracles:** MS-DOS 6.22 (QEMU), dosbox-x, PCem. DOS probe: `tools/dostest/p_uart.asm`.
**Marked:** 2026-09-23, **from the code**; OUT2 and COM3/COM4 rows re-marked 2026-10-01 (#181).

---

## Headline — this one was already right, and the probe says so

`vdd_comm.c` was written carefully and nothing had ever checked it against a machine.
It is the same position the 8259 was in — *"10 fields, all AGREE"* with five gaps
underneath — so the probe asks the questions a **driver** asks rather than the ones our
code makes easy.

**Four of five cases agree across all four hosts**, and they are not soft ones:

| case | 6.22/QEMU | dosbox-x | PCem | ours |
|---|---|---|---|---|
| `uart.loop.echo` — a byte through loopback | `005A` | `005A` | `005A` | **`005A`** ✅ |
| `uart.loop.msr` — the four modem lines | `00F0` | `00F0` | `00F0` | **`00F0`** ✅ |
| `uart.scratch` — the part-identification register | `005A` | `005A` | `005A` | **`005A`** ✅ |
| `uart.dlab.divisor` — the bank switch | `0060` | `0060` | `0060` | **`0060`** ✅ |
| `uart.idle.lsr.iir` | `6001` | `6001` | **`6101`** | `6001` ⚠ |
| `uart.idle.pending` | `0000` | `0000` | **`014D`** | `0000` ⚠ |

✅ **The loopback pairing is right**, which is the one that matters most: a port that
echoes bytes but maps DTR to CTS instead of DSR passes the echo test and fails every real
detection routine — *success that looks like success*. ✅ **DLAB really switches the
bank** rather than being stored. ✅ The scratch register identifies us as a 16450-or-later,
which is what we claim to be elsewhere.

⇒ **Marked as a verified surface**, with the caveat the README insists on: verified means
*these questions* were asked of three machines, not that the chip is complete.

---

## ⛔⛔ The one disagreement was a serial mouse, and my probe caused it

PCem alone reported `LSR = 61h` — Data Ready set — where the other three said `60h`. A
*value* says the hosts differ; it does not say why, so a second case drained the byte and
named it:

> **`4Dh`. That is `'M'` — the byte a Microsoft serial mouse sends to identify itself
> when DTR and RTS are asserted.**

PCem's config is the only one of the three with a `mouse_type` set. Three independent
things point the same way: the byte is *the* Microsoft mouse ID, it appeared on COM1
immediately after a DTR/RTS transition, and only on the host configured with a mouse.

⚠ **And the probe broke its own stated rule to produce it.** Its header says *"no case
reads a byte that the outside world would have to supply"* — the whole reason the tests
are built on loopback. But **the exit from loopback re-applies MCR to the real pins**, and
that transition is itself a stimulus. A probe can isolate the chip while it is *inside*
loopback; it cannot isolate it on the way out.

⇒ Both rows are **not adjudicable** — they measure what is plugged into COM1. Abstained,
with the mechanism recorded rather than the value.

---

## 1. The register file

| Item | Status | Evidence |
|---|---|---|
| RBR/THR, IER, LCR, MCR, SCR | **IMPL** | `comm_in`/`comm_out` |
| **DLAB switches offsets 0–1 to the divisor latch** | ✅ **IMPL** | measured; agrees on all four hosts |
| LSR — and **reading clears the error bits only** | **IMPL** | DR and THRE are not errors and stay |
| MSR — and **reading clears the delta bits** | **IMPL** | `c->msr &= 0xF0` |
| IIR — priority order RLS > RDA > THRE > MS, bit 0 set = nothing | **IMPL** | `comm_iir`; matches the datasheet order exactly |
| IIR — the read **is** the THRE acknowledgement | **IMPL** | |
| IIR bits 7:6 gated on FCR bit 0 | **IMPL** | answering `C0h` on a part behaving like an 8250 would tell a 16550-aware driver it may write 16 bytes between interrupts |
| **Loopback as a rewiring, with the correct four-line pairing** | ✅ **IMPL** | measured |
| **MCR bit 3 (OUT2) as the interrupt gate** | **IMPL** | `vdd_comm.c:49` — `comm_update_irq` returns before `vdd_raise_irq` with OUT2 clear; `comm_iir` never reads MCR, so **IIR still names the source**; the MCR write re-runs the check, so setting OUT2 onto a pending source raises at once. Off-VM: `comm_test.c:201` (fails 5 checks with the gate removed). ⚠ **This row was marked MISS on 2026-09-23 and the gate had been there since the first commit (`f547336`)** — the mark was taken from the inventory's expectation, not the line |
| OUT2 gate **in loopback** | ⚠ **PART** | the bit is honoured as written in loopback too (the IRQ is delivered with OUT2 set). The 8250/16550 datasheets say loopback forces the modem-control output *pins* inactive, which on an IBM-style card would close the buffer regardless; Super I/O parts that gate OUT2 internally document it differently. **Unsettled — needs an oracle**, not pinned by a test |

## 2. What is not there

| Item | Status | Notes |
|---|---|---|
| A real 16-byte **FIFO** | ⛔ **PART** | FCR is stored and gates IIR's bits, but there is no depth behind it. A 16550-aware driver told it may write 16 bytes is served one at a time — correct, just not faster |
| The **baud divisor has no effect** | ⚠ **N/A-by-design** | there is no wire and no timing to slow down; the value round-trips, which is all a driver checks |
| Break generation (LCR bit 6) | ⛔ **MISS** | |
| COM3 / COM4 | **PART** | the device has four slots (`vdd_comm.h:53`, `COMM_MAX_PORTS 4`) and a fitted COM3 3E8h/IRQ4 or COM4 2E8h/IRQ3 works on every route (`comm_test.c:267`). **The host fits only COM1/COM2** (`main.c:27775`); the BDA rows 0040:0000–0007 (`main.c:27797`) and the INT 11h serial count (`main.c:2474`) are both derived from `vdd_comm_fitted`, so fitting one is a single line. Not done because it changes INT 11h (2 → 4 ports) and the BDA — an oracle question (#181) |

---

## What to fix, in order

Tracked in GitHub: [#181](https://github.com/MrMatthewLayton/ntvdmex/issues/181) (the list that was here was moved there verbatim, 2026-09-27).

