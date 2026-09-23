# Inventory — 8250 / 16450 / 16550 serial port

**Spec:** National Semiconductor 8250/16450/16550A datasheets; IBM PC TechRef.
**▶ The hardware reference is [`../ref/uart.md`](../ref/uart.md)** — what the chip *does*.
**Our implementation:** `src/vdd/vdd_comm.c` (402 lines), `src/vdd/vdd_comm.h`.
**Oracles:** MS-DOS 6.22 (QEMU), dosbox-x, PCem. DOS probe: `tools/dostest/p_uart.asm`.
**Marked:** 2026-09-23, **from the code**.

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

## 2. What is not there

| Item | Status | Notes |
|---|---|---|
| A real 16-byte **FIFO** | ⛔ **PART** | FCR is stored and gates IIR's bits, but there is no depth behind it. A 16550-aware driver told it may write 16 bytes is served one at a time — correct, just not faster |
| The **baud divisor has no effect** | ⚠ **N/A-by-design** | there is no wire and no timing to slow down; the value round-trips, which is all a driver checks |
| **MCR bit 3 (OUT2) as the interrupt gate** | ⛔ **MISS** | on a PC the IRQ line runs through a buffer OUT2 enables. A guest that clears OUT2 should get **no interrupts**; we deliver them anyway. The direction is permissive, so nothing breaks — but a driver using OUT2 to mask its own port is not served |
| Break generation (LCR bit 6) | ⛔ **MISS** | |
| COM3 / COM4 | ⛔ **MISS** | `COMM_MAX_PORTS 2` |

---

## What to fix, in order

1. **OUT2 as the interrupt gate.** Small, well-defined, and the only row here where a
   guest's explicit instruction is ignored. ⚠ It needs a probe case that can see an
   interrupt, which this one cannot — loopback plus IER plus a real IRQ4 is a bigger test
   than anything here so far.
2. **A real FIFO**, if anything is ever measured wanting one. Recorded, not scheduled:
   the visible behaviour is already correct.
3. **COM3/COM4.** Two more entries in a fixed array; no oracle disagreement drives it.

⚠ **Nothing above is a defect the probe found.** They came from marking the code against
the datasheet, which is the point — the probe confirmed the parts that *are* implemented
are implemented right.
