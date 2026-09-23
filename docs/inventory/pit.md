# Inventory — 8254 Programmable Interval Timer

**Spec:** Intel 8253/8254 datasheets; IBM TechRef for the board wiring.
**▶ The hardware reference is [`../ref/pit.md`](../ref/pit.md)** — what the chip *does*.
This file is the companion: what **we** do about it.
**Our implementation:** `src/vdd/vdd_pit.c` (346 lines), `src/vdd/vdd_pit.h`;
port `61h` in `src/vdd/vdd_speaker.c`.
**Oracle:** MS-DOS 6.22 (`scripts/oracle.sh`) for BIOS-level behaviour; PCem for chip
timing. Off-VM: `tools/dostest/pit_test.c`.
**Measured:** 2026-09-23, from the code, with citations. Re-measure after any change.

---

## Headline

**Counter 0 is modelled well. Counters 1 and 2 barely exist, and the 8254's defining
feature — the Read-Back Command — is not implemented at all.**

> Everything we have is shaped around *"the thing that raises IRQ0 at 18.2 Hz"* — counter 0
> in a periodic mode (**measured: mode 2, not the mode 3 this file first said**; see
> [`../ref/pit.md`](../ref/pit.md) §6, and note that both give the same rate, which is why
> the error was invisible). That is the counter a guest needs to boot. It is not the one a
> guest uses to **measure** anything — for that it reads counter 0's count, polls counter 2's
> OUT pin at port `61h` bit 5, or issues a Read-Back. Two of those three we do not answer,
> and the third we answer without ever consulting the counter.

| Group | IMPL | PART | STORE | MISS |
|---|---|---|---|---|
| Counter 0 | 5 | 3 | 0 | 2 |
| Counter 1 | 0 | 0 | 0 | 4 |
| Counter 2 | 0 | 2 | 2 | 4 |
| Control Word fields | 2 | 1 | 0 | 1 |
| Read-Back Command | 0 | 0 | 0 | 6 |
| Modes | 2 | 3 | 0 | 1 |
| Port 61h | 1 | 1 | 1 | 1 |

---

## 1. Ports

| Port | Access | Register | Status | Evidence |
|---|---|---|---|---|
| `40h` | W | Counter 0 count write | **IMPL** | `vdd_pit.c:153` — and it is careful: a half-written count is not applied |
| `40h` | R | Counter 0 count read | **IMPL** | `vdd_pit.c:214` — latch, access mode and the lo/hi toggle all honoured |
| `41h` | W | Counter 1 count write | **MISS** | `vdd_pit.c:202` — *"port 0x41 (DRAM refresh) not modelled"* |
| `41h` | R | Counter 1 count read | **MISS** | `vdd_pit.c:217` — returns `0xFF` |
| `42h` | W | Counter 2 count write | **PART** | `vdd_pit.c:193` — stored as a speaker *tone divisor*, not as a counter |
| `42h` | R | Counter 2 count read | **MISS** | `vdd_pit.c:217` — returns `0xFF` |
| `43h` | W | Control Word | **PART** | `vdd_pit.c:126` — see §2 |
| `43h` | R | *(undefined on hardware)* | **N/A** | returns `0xFF`; consistent, and recorded here so it is a decision rather than an accident |

⛔ **`41h` and `42h` both read `0xFF`.** On hardware they return a count. A timing loop that
latches counter 2 and reads it back gets `0xFFFF` every time — i.e. "no time is passing",
which is the *"runs but lies"* shape this project treats as the most expensive kind.

## 2. The Control Word (`43h`)

| Field | Bits | Status | Evidence |
|---|---|---|---|
| Select Counter | 7:6 | **PART** | `vdd_pit.c:127` — `00` handled, `10` partly (tone only), `01` and `11` fall through `if (ch != 0) return;` |
| Access / Latch | 5:4 | **IMPL** | `vdd_pit.c:129,133` — latch command and all three access modes |
| Mode | 3:1 | **PART** | `vdd_pit.c:150` — stored as the raw 3 bits |
| **BCD** | 0 | **MISS** | `vdd_pit.c:150` — `(val >> 1) & 7` **drops bit 0 entirely** |

⛔ **BCD is dropped.** A guest that programs BCD counts gets binary ones, and its maximum
count means 65536 where it asked for 10000. Nothing reads back the flag either, because
read-back does not exist.

⛔ **Modes 6 and 7 are not aliased to 2 and 3.** `vdd_pit.c:90` tests
`periodic = (st->mode == 2 || st->mode == 3)`, so a guest that programs `110` — legal, and
identical to mode 2 on hardware — **gets no periodic interrupt at all**. This is a
one-line fix with a real failure behind it.

## 3. The Read-Back Command — **entirely absent**

| Unit | Status | Evidence |
|---|---|---|
| Read-Back command decode (`43h` bits 7:6 = `11`) | **MISS** | `vdd_pit.c:131` — `if (ch != 0) return;` swallows it |
| Latch-count for multiple counters | **MISS** | — |
| Latch-status | **MISS** | — |
| Status bit 7 — **OUT pin** | **MISS** | no OUT state exists for any counter |
| Status bit 6 — **Null Count** | **MISS** | `cw_armed`/`next_pending` track something adjacent (`vdd_pit.h:42,43`) but nothing is exposed |
| Status bits 5:0 — programmed access / mode / BCD | **MISS** | the values are held (`vdd_pit.h:29,30`); nothing reports them |

⇒ **This is the largest single gap on the surface.** It is the 8254's whole addition over
the 8253, it is the only way to read a counter's OUT pin or how it was programmed, and a
guest that issues one gets its command silently discarded and then reads `0xFF` from a
port that should be handing back a status byte.

## 4. The six modes

| Mode | Name | Status | Evidence |
|---|---|---|---|
| 0 | Interrupt on Terminal Count | **IMPL** | `vdd_pit.c:31` — its own count law, wrapping past zero |
| 1 | Hardware Retriggerable One-Shot | **PART** | falls to the generic `R - (elapsed % R)` at `vdd_pit.c:38`; no GATE, so it can never be triggered |
| 2 | Rate Generator | **IMPL** | periodic at `vdd_pit.c:90`; IRQ0 from `vdd_pit_add_clocks` |
| 3 | Square Wave | **IMPL** | `vdd_pit.c:33` — **decrements by two**, and the odd-count half is `(R+1)/2` |
| 4 | Software Triggered Strobe | **PART** | generic count law; the one-clock OUT strobe is not modelled |
| 5 | Hardware Triggered Strobe | **MISS** | needs a GATE edge, which does not exist |

✅ **The load rule is right, and it was expensive to learn:** a Control Word arms the next
count write to load and restart; a bare count in a periodic mode waits for the end of the
current period (`vdd_pit.c:137-150`, `vdd_pit.h:42-47`). The comment there records that the
datasheet halts counting on the first byte of a **count**, not on the Control Word — which
is what lets Lemmings' calibration exit keep ticking.

## 5. GATE inputs

| Counter | GATE source | Status | Evidence |
|---|---|---|---|
| 0 | tied high on the PC | **N/A** | correct by construction — nothing to model |
| 1 | tied high | **N/A** | |
| 2 | **port `61h` bit 0** | **MISS** | `vdd_speaker.c:9` stores port 61h; the PIT never reads it |

⛔ **Counter 2 counts regardless of its gate.** Clearing `61h` bit 0 must stop it; we keep
going. Combined with the missing OUT pin this makes the whole gate-and-poll idiom
unavailable.

## 6. Port `61h` (PPI port B) — the PIT's other face

| Bit | Function | Status | Evidence |
|---|---|---|---|
| 0 | Timer-2 GATE | **STORE** | `vdd_speaker.c:9` — written value kept, never consumed |
| 1 | Speaker data enable | **IMPL** | consumed by `vdd_audio.c:135` |
| 4 | DRAM refresh toggle | **PART** | `vdd_speaker.c:13` — **toggled on every read**, not derived from a 15 µs clock. Deliberate: it makes delay loops terminate. A guest *calibrating* against it gets a number with no relation to time. |
| **5** | **Timer-2 OUT** | **MISS** | `vdd_speaker.c:14` returns `(port61 & ~0x10) \| refresh` — bit 5 is **echoed from what was written**, never from counter 2 |

⛔ **Bit 5 is the classic "measure time without interrupts" surface**: program counter 2,
poll bit 5, count the loops. We return a constant, so such a loop either exits instantly
or never.

---

## What to fix, in order

1. **Alias modes 6 and 7 to 2 and 3** (`vdd_pit.c:90`, `:150`). One line; today a legal
   programming makes IRQ0 stop.
2. **Make `41h`/`42h` readable** as real counters. Removes a whole class of "no time is
   passing" answers.
3. **Implement the Read-Back Command and the status byte**, including the OUT pin and the
   null-count flag. The largest gap, and self-contained.
4. **Consume counter 2's GATE from `61h` bit 0, and report its OUT at `61h` bit 5.**
   Together with (2) and (3) this completes the polling idiom.
5. **Honour the BCD bit.**
6. **Model the OUT pin as state for all six modes**, which is what (3) and (4) both need.

⚠ **None of this is verified against an oracle yet.** `tools/dostest/pit_test.c` is an
off-VM battery written against our own model, so it encodes our behaviour, not the
datasheet's — the audit the inventory README calls for applies here. **A DOS probe
(`p_pit.asm`) asking the six questions above of both NTVDMEX and 6.22 does not exist and
is the next thing to write**, before any of the fixes.

---

## Re-verified on a quiet rig (2026-09-23)

The first `p_pit` run happened while the user was on the rig taking screenshots, so it was
**not a controlled run** — the same category of evidence as the `db4c059` verdict this
project has already been burned by. Re-run with the machine idle, host `77b9b0bd`:

**7 of 7 still MISMATCH.** Every gap above stands.

⚠ **But one value moved: `rdback.st0` was `0x21`, now `0x30`.** That is not noise — **it is
the diagnosis.** A status byte cannot change between two identical runs; a counter must. The
variance confirms directly that the Read-Back Command is being discarded and the following
`IN` is handing back a **live count**.

⇒ The probe header's claim that *"every case is deterministic by construction"* was too
strong and is corrected: the three read-back cases are deterministic **only on a host that
implements read-back**. The *verdict* is stable; the mismatching *value* is not, so
`pit.ref.txt` no longer records one run's figure as though it were fixed.
