# The 8254 Programmable Interval Timer — a technical reference

> **What this is.** What the chip does, in our own words, derived from the Intel 8253/8254
> datasheets and the IBM technical references. Written so that a PIT could be implemented
> from it without seeing our code. What *we* implement is
> [`docs/inventory/pit.md`](../inventory/pit.md).
>
> **Confidence.** Register layouts, the mode table and the load rules are `[FACT]` from the
> datasheet. PC-specific wiring (which channel does what, where the gates come from) is
> `[FACT]` from the IBM TechRef but is a *board* fact, not a chip fact, and is marked as
> such. See [SOURCES.md](SOURCES.md).

---

## 1. The idea

Three **independent 16-bit down-counters** sharing one input clock and one command port.
Each counter has a **GATE** input, a **CLK** input and an **OUT** output, and each can be
programmed to one of six modes that decide what OUT does as the count runs down.

The three counters are genuinely independent: programming one does not disturb another.
Almost every timing bug in a PC emulator comes from treating the PIT as "the tick" rather
than as three counters, and from treating a count write as instantaneous when the hardware
defines exactly when it takes effect (§5).

**The input clock is 1.193182 MHz** — 14.31818 MHz (the NTSC colour-burst crystal ×4)
divided by 12. Every period a guest programs is `divisor / 1193182` seconds.

### Each counter has three registers, and they are not the same thing

This distinction *is* the read-back semantics, and collapsing it is why naive models
return wrong counts:

| | |
|---|---|
| **CR** — Count Register | Where a written count lands first. Two bytes, written one at a time in lo/hi access. |
| **CE** — Counting Element | The actual down-counter. Loaded *from* CR at a defined moment (§5), not when the guest writes. |
| **OL** — Output Latch | Follows CE continuously **until a latch command freezes it**. Reads come from here. |

A **null count** flag records "a count has been written to CR but not yet loaded into CE",
and it is readable through the read-back status byte (§4).

---

## 2. Ports, and what each channel is for on a PC

| Port | Access | Function |
|---|---|---|
| `40h` | R/W | Counter 0 — count read / write |
| `41h` | R/W | Counter 1 — count read / write |
| `42h` | R/W | Counter 2 — count read / write |
| `43h` | W | Control Word / Read-Back Command (**write-only**; a read is undefined) |

**PC wiring** *(board fact, not chip fact)*:

| Counter | Role | GATE | OUT goes to |
|---|---|---|---|
| **0** | System timer | tied **high** — always counting | **IRQ0** |
| **1** | DRAM refresh request | tied high | refresh logic |
| **2** | PC speaker tone | **port `61h` bit 0** (software-controlled) | AND'd with port `61h` bit 1 → speaker; also **readable at port `61h` bit 5** |

⚠ **Counter 2's OUT being readable at `61h` bit 5 is a real, used surface.** It is how
software measures elapsed time without interrupts, and how several copy-protection and
speed-calibration routines work: program a count, poll bit 5, count the loops.

⚠ **Counter 1 is not decorative.** Nothing on a modern box depends on refresh, but its
*count is still readable*, and timing loops have used it precisely because nothing else
touches it.

---

## 3. The Control Word — write to `43h`

| Bits | Field | Values |
|---|---|---|
| 7:6 | **Select Counter** | `00`=0, `01`=1, `10`=2, `11`= **Read-Back Command** (8254 only — see §4) |
| 5:4 | **Read/Write (access)** | `00` = **Counter Latch Command**, `01` = LSB only, `10` = MSB only, `11` = LSB then MSB |
| 3:1 | **Mode** | 0–5; see §5. **`110` aliases to mode 2 and `111` to mode 3.** |
| 0 | **BCD** | `0` = 16-bit binary, `1` = four-decade **BCD** |

⚠ **Two things here are routinely dropped by implementations and both are guest-visible:**

- **Modes 6 and 7 are not distinct modes, they are aliases** for 2 and 3. A model that
  stores the raw three bits and then tests `mode == 2` for "is periodic" will treat a
  guest that programmed `110` as non-periodic and stop generating its interrupt.
- **BCD is a real counting mode.** In BCD the counter runs `9999 → 0000`, and a maximum
  count of `0000` means **10000**, not 65536. A guest that programs BCD and reads back
  binary sees a count it cannot reconcile.

### Access modes, and the byte order rule

In `11` (LSB-then-MSB) each counter keeps **its own** toggle for reads and for writes, and
they are independent of each other. A guest may interleave a read of one counter with a
write of another without disturbing either toggle.

### The Counter Latch Command — access bits `00`

Latches the current CE into OL for the selected counter. The latched value is held until
read out, **or until the counter is reprogrammed**. Further latch commands before the read
are ignored — the first latched value survives.

⚠ **A latch command does not affect counting.** The counter keeps running; only the
snapshot is frozen. Reading without latching gives whatever the CE happens to hold, which
for a counter in flight can be a byte-tearing hazard — which is why latching exists.

---

## 4. The Read-Back Command — 8254 only

Written to `43h` with bits 7:6 = `11`:

| Bit | Meaning |
|---|---|
| 7:6 | `11` — read-back |
| 5 | `0` = **latch count** for the selected counters |
| 4 | `0` = **latch status** for the selected counters |
| 3 | select counter 2 |
| 2 | select counter 1 |
| 1 | select counter 0 |
| 0 | reserved, `0` |

⚠ **The two latch bits are active LOW.** `0` means *do it*.

It can latch count and status for **several counters in one command**, which is what makes
it worth having. If both are latched, the **status byte is read first**, then the count.

### The status byte

| Bit | Meaning |
|---|---|
| 7 | **OUT pin** — the current state of this counter's output |
| 6 | **Null Count** — `1` = a count has been written but not yet loaded into CE |
| 5:4 | the programmed access mode |
| 3:1 | the programmed mode |
| 0 | the programmed BCD flag |

**This is the only way software can read the OUT pin of counters 0 and 1**, and the only
way to read back how a counter was programmed. A model that does not implement read-back
forces every guest that uses it onto a fallback path — or, worse, answers the `43h` read
with bus garbage that the guest takes for a status byte.

---

## 5. The six modes, and when a written count takes effect

**The load rule is the single most important behaviour in this chip**, and it is *not* "the
count takes effect when written":

> **Writing a Control Word** stops nothing, but it clears CR and arms the counter: the
> **next** count write loads CE immediately and restarts the sequence.
>
> **Writing a bare count with no preceding Control Word:**
> - in **modes 0, 1, 4, 5** (one-shot family) the new count loads on the next CLK;
> - in **modes 2 and 3** (periodic family) the new count is held in CR and loaded **at the
>   end of the current period** — the counter finishes the cycle it is in.

⚠ **This is the rule this project learned the hard way:** *Control Word + count = RESTART;
bare count = takes effect at the end of the current period.* A model that reloads
immediately on every count write runs periodic guests fast and jitters them.

| Mode | Name | OUT behaviour | GATE |
|---|---|---|---|
| **0** | Interrupt on Terminal Count | Goes **low** on the Control Word; stays low while counting; goes **high** at terminal count and **stays high**. | high enables counting |
| **1** | Hardware Retriggerable One-Shot | OUT high; a **rising edge on GATE** starts the count and drives OUT low; OUT high at terminal count. Retriggerable mid-count. | rising edge triggers |
| **2** | Rate Generator | Divide-by-N. OUT high, pulses **low for one CLK** when the count reaches 1, then reloads automatically. Periodic. | high enables; falling edge forces OUT high and reloads |
| **3** | Square Wave | Divide-by-N with a ~50% duty cycle: OUT high for half, low for half. The counter decrements **by two**. For **odd N** the high period gets the extra clock. | as mode 2 |
| **4** | Software Triggered Strobe | OUT high; when the count expires OUT goes **low for one CLK** then high. Triggered by the count write. | high enables |
| **5** | Hardware Triggered Strobe | As mode 4, but triggered by a **rising edge on GATE**. | rising edge triggers |

**A count of 0 means the maximum**: 65536 in binary, 10000 in BCD. In mode 3 an odd count
is legal and the asymmetry is observable by software timing the OUT pin.

⚠ **Mode 3's decrement-by-two matters to read-back.** The count a guest reads is the CE,
which in mode 3 steps in twos, so a model that decrements by one returns values a mode-3
guest can detect as impossible.

---

## 6. What the PC BIOS builds on top

*(Firmware, not the chip — a separate surface; see* `docs/inventory/bios-misc.md`*.)*

- Counter 0 is programmed with **divisor 0** (65536) → **18.2065 Hz**, and its OUT drives
  IRQ0 → `INT 08h`.
  ⚠ **The MODE is a BIOS choice, not a chip fact, and this document asserted the wrong
  one.** It said mode 3; **measured** via Read-Back on the 6.22 oracle, counter 0 comes back
  as **mode 2** (status `0x34` = lo/hi access, mode 2, binary). Mode 2 and mode 3 both
  produce a periodic IRQ0 at the same rate, which is why the error survived being written
  down. Real period-correct BIOSes vary; PCem with a genuine AMI ROM is what settles which
  to expect where. **Ask Read-Back, do not assume.**
- `INT 08h` increments the BDA tick count at **`0040:006C`** (a 32-bit count of ticks since
  midnight) and rolls it at 24 hours, setting the overflow flag at `0040:0070`.
- `INT 1Ah` reads and writes that pair.
- `INT 15h AH=86h` (WAIT) and the speaker tone routines program counter 2.

⚠ **The BDA tick is firmware state, not chip state.** A guest that reprograms counter 0 to
a faster rate and leaves the BIOS handler installed gets a tick count that runs fast —
which is correct behaviour, and games rely on it by hooking `INT 08h` and chaining at 18.2 Hz.

---

## 7. Implementation notes that are easy to get wrong

1. **The OUT pin is state, not an event.** Modes 2 and 3 hold OUT low for a defined
   duration; software polls it. Modelling only the edge (the IRQ) loses every polling guest.
2. **IRQ0 must be held in service**, not auto-EOI'd — an interrupt that is acknowledged
   before the handler runs breaks the 8259's priority logic.
3. **Latch, read LSB, then reprogram** is a legal sequence and the latched MSB must still
   be readable.
4. **Reading `43h` is undefined**, so returning a plausible value is worse than returning a
   consistent one — decide and record which.
5. **A counter still counts while latched**, and while its Control Word has been written
   but no count has followed.
6. **GATE transitions are not just enables** for modes 1, 2, 3 and 5; a falling edge in
   modes 2/3 forces OUT high and reloads on the next rising edge.
