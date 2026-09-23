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

⛔ **BCD is dropped** — the flag is now *stored* (`vdd_pit.c:99`) and still never consumed,
so counts stay binary and a maximum count means 65536 where the guest asked for 10000.

⛔⛔ **AND IT IS BLOCKED ON PCem, WHICH ONLY BECAME VISIBLE AFTER A FALSE PASS.** The probe's
first BCD case took **one** sample, and a binary counter passes that whenever its four
nibbles happen to be ≤ 9 — about one value in six. It reported a clean pass on both hosts
while **neither implements BCD**. Strengthened to 16 spread samples, the **oracle fails it
too**: QEMU's PIT does not model BCD. Both hosts now answer `0`, which `dosdiff` scores
AGREE — **agreement on absence, not verification**.

⇒ **Do not implement BCD against this oracle.** It joins the VGA modes 04/06 row and the DAC
pixel mask in the queue of things only a real chip model can settle.

✅ **Modes 6 and 7 are now aliased to 2 and 3.** *(FIXED 2026-09-23.)*

⚠ **And the claim this row first made was too strong — corrected here rather than quietly
edited.** It said a guest programming `110` *"gets no periodic interrupt at all"*. It does
get one: `vdd_pit_add_clocks` raises IRQ0 from the accumulator **regardless of mode**, and
`periodic` only ever gated the **bare-count load rule**. The real defect was narrower:

- a bare count in mode 6 **restarted immediately** instead of waiting for the end of the
  current period (a jitter/rate defect, not a dead timer);
- mode 7 read back a count stepping **by one** where mode 3 steps **by two**.

Both are now pinned by `tools/dostest/pit_test.c` T9, which **failed on the old code and
passes on the new** — and whose mode-2/mode-3 reference cases passed throughout, which is
what proves the tests measure the alias rather than something incidental.

⇒ **The fix keeps BOTH values.** `mode` is normalised for behaviour; new `mode_raw` holds
the bits as programmed, because a real 8254 reports them **un-normalised** in the Read-Back
status byte (measured: `pit.mode6.readback` = `0x0C`, not `0x04`). Normalising in one place
and reporting from the other is what lets the behaviour be right *and* the read-back honest
once §3 is implemented.

## 3. The Read-Back Command — ✅ **IMPLEMENTED 2026-09-23**

| Unit | Status | Evidence |
|---|---|---|
| Read-Back command decode (`43h` bits 7:6 = `11`) | **IMPL** | `pit_readback`; both latch bits honoured **active-low** |
| Latch-count for multiple counters in one command | **IMPL** | `pit_readback` loops the three select bits |
| Latch-status | **IMPL** | `st_latch[]` / `st_latched[]`, first latch wins |
| Status bit 7 — **OUT pin** | **IMPL** | `pit_out_pin` — **derived** from mode + elapsed, not stored, so it cannot go stale |
| Status bit 6 — **Null Count** | **IMPL** | counter 0 from `cw_armed`/`next_pending`; channels from `null_cnt` |
| Status bits 5:0 — access / mode / BCD | **IMPL** | `pit_status_of`; **`mode_raw`, not the normalised mode** |
| Status read before count when both latched | **IMPL** | served ahead of everything in `pit_in_locked` |

**Measured:** `p_pit` went **4 mismatches → 1**. `rdback.st0`, `rdback.st2` and
`mode6.readback` all moved to AGREE — and `mode6.readback` agreeing at `0x0C` is the
`mode_raw` decision from fix 1 confirmed against a real kernel: hardware reports the mode
**as programmed**, not normalised.

⚠ **And it exposed a second defect that had nothing to do with read-back.** Our counter 0
came out of startup in **mode 0** — a one-shot — where a real machine leaves it periodic.
Nothing had noticed because the IRQ0 engine raises from the accumulator *regardless of
mode*; what it got wrong was everything a guest can **ask**, plus the bare-count load rule,
which took the one-shot path. Fixed by giving counter 0 and counter 1 their POST defaults.

⛔ **The first attempt at that fix was inert, and the canary "passed" on it.** The defaults
went into `vdd_pit_reset` only — but the host builds `g_pit` as a zeroed global and calls
`vdd_pit_init`, never `reset`, on the startup path. Read-Back still reported mode 0 on the
rig. **A Skyroads run that certifies a change which is not wired up certifies nothing**, so
the canary was re-run once the defaults were live: `n8=0 max_ms=7`, within the documented
guard.

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

1. ✅ ~~Alias modes 6 and 7 to 2 and 3.~~ **DONE 2026-09-23.** Skyroads re-run as the
   timing canary after it: `n8=0 max_ms=6`, the documented guard, unchanged.
2. ✅ ~~Make `41h`/`42h` readable as real counters.~~ **DONE 2026-09-23.** `p_pit`
   `ch1.counting` and `ch2.counting` both moved MISMATCH → AGREE. Skyroads re-run:
   `n8=0 max_ms=6`, unchanged.
3. ✅ ~~Implement the Read-Back Command and the status byte.~~ **DONE 2026-09-23** —
   `p_pit` 4 mismatches → 1.
4. **Consume counter 2's GATE from `61h` bit 0, and report its OUT at `61h` bit 5.**
   Together with (2) and (3) this completes the polling idiom.
5. ⛔ ~~Honour the BCD bit.~~ **BLOCKED ON PCem** — the oracle cannot say what correct is
   (see §2). Implementing from the datasheet alone would be writing an expectation from
   memory, which is the one thing this programme exists to stop.
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
