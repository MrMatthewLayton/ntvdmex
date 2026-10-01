# Inventory — 8254 Programmable Interval Timer

**Spec:** Intel 8253/8254 datasheets; IBM TechRef for the board wiring.
**▶ The hardware reference is [`../ref/pit.md`](../ref/pit.md)** — what the chip *does*.
This file is the companion: what **we** do about it.
**Our implementation:** `src/vdd/vdd_pit.c` (669 lines), `src/vdd/vdd_pit.h`;
port `61h` in `src/vdd/vdd_speaker.c`. How time reaches the chip (the 1 ms pacer, IRQ0
delivery) is host code — §7.
**Oracle:** MS-DOS 6.22 (`scripts/oracle.sh`) for BIOS-level behaviour; PCem for chip
timing. Off-VM: `tools/dostest/pit_test.c`.
**Measured:** 2026-09-23, from the code, with citations. **Re-cited 2026-10-01** after #175
(s82/s83) and #238 (s85): §1, §4 and §6 were stale against the code and are re-marked.
The BIOS services on top of the chip (INT 08h, INT 1Ah) are [bios-misc.md](bios-misc.md).

---

## Headline

**⇒ As of 2026-09-23 this surface has ZERO mismatches against oracle consensus** — the
sections below record what was found and what each fix cost. The original headline was:
*"Counter 0 is modelled well. Counters 1 and 2 barely exist, and the 8254's defining feature
— the Read-Back Command — is not implemented at all."*

> **The diagnosis that drove all of it:** everything we had was shaped around *"the thing
> that raises IRQ0 at 18.2 Hz"* — counter 0 in a periodic mode. That is the counter a guest
> needs to **boot**. It is not the one a guest uses to **measure** anything: for that it reads
> counter 0's count, polls counter 2's OUT pin at `61h` bit 5, or issues a Read-Back. Two of
> those three we did not answer, and the third we answered without consulting the counter.
>
> ⚠ **Counter 0's mode is a BIOS choice and it took three oracles to settle.** This file and
> `ref/pit.md` said mode 3 from memory; QEMU measured mode 2 and I "corrected" both; dosbox-x
> and then **PCem, on a real AMI BIOS, said mode 3** — the original was right. Both give the
> same interrupt rate, which is exactly why the error was invisible to the only check anyone
> runs.

| Group | Units | IMPL | PART | STORE | MISS | N/A |
|---|---|---|---|---|---|---|
| §1 Ports | 8 | 6 | 1 | — | — | 1 |
| §2 Control Word fields | 4 | 4 | — | — | — | — |
| §3 Read-Back Command | 7 | 7 | — | — | — | — |
| §4 Modes | 6 | 5 | 1 | — | — | — |
| §5 GATE inputs | 3 | 1 | — | — | — | 2 |
| §6 Port `61h` | 6 | 3 | 2 | 1 | — | — |
| **Total** | **34** | **26** | **4** | **1** | **—** | **3** |

*(2026-10-01. The s69–s72 "before → after" table that stood here is in the repository
history; the narrative below keeps each fix and what it cost.)*

---

## 1. Ports

| Port | Access | Register | Status | Evidence |
|---|---|---|---|---|
| `40h` | W | Counter 0 count write | **IMPL** | `vdd_pit.c:416-455` — buffered: a half-written count is not applied; LSB-only / MSB-only zero the other half; committed through `pit_load` (`:254-271`) |
| `40h` | R | Counter 0 count read | **IMPL** | `vdd_pit.c:497-505` — latch, access mode and the lo/hi toggle all honoured |
| `41h` | W | Counter 1 count write | **IMPL** | `chan_write_count(&st->c1)` (`:469-470`, `:138-157`) |
| `41h` | R | Counter 1 count read | **IMPL** | `chan_read_count` (`:493`, `:160-170`) — free-running from POST (`:639`, `:659-662`) |
| `42h` | W | Counter 2 count write | **PART** | the **counter view** is right (`chan_write_count`, `:467-468`), but the **speaker divisor** `ch2_reload` is still read-modify-written (`:457-463`): an LSB-only or MSB-only write keeps the stale other half — the defect counter 0 had fixed (`:438-442`) |
| `42h` | R | Counter 2 count read | **IMPL** | `chan_read_count(&st->c2)` (`:494`) |
| `43h` | W | Control Word / Read-Back | **IMPL** | `:375-415`, all four counter selects, incl. `11` = Read-Back (`:384`) |
| `43h` | R | *(undefined on hardware)* | **N/A** | `:497` returns `0xFF`; consistent, and recorded here so it is a decision rather than an accident |

✅ **`41h` and `42h` are real counters** *(FIXED 2026-09-23)*. They used to return `0xFF`, so a
timing loop that latched either and read it back got `0xFFFF` forever — "no time is passing",
the *"runs but lies"* shape this project treats as the most expensive kind. `p_pit`
`ch1.counting` and `ch2.counting` both moved MISMATCH → AGREE.

## 2. The Control Word (`43h`)

| Field | Bits | Status | Evidence |
|---|---|---|---|
| Select Counter | 7:6 | **IMPL** | all four: `00`/`01`/`10` counters, `11` Read-Back (`vdd_pit.c:376-384`) |
| Access / Latch | 5:4 | **IMPL** | latch command and all three access modes (`:385-387`, `:175-181`) |
| Mode | 3:1 | **IMPL** | normalised into `mode`, kept raw in `mode_raw` for Read-Back (`:409-410`, `:183-184`) |
| **BCD** | 0 | **IMPL** | decoded on every count write, encoded on every count read; the divisor, the wrap and the maximum count all follow it |

✅ **BCD counts** *(FIXED 2026-09-23)*. The flag used to be *stored and never consumed*, so a
counter programmed for four-decade BCD went on counting in binary and a maximum count meant
65536 where the guest had asked for 10000.

**It is implemented as a boundary format, not a second arithmetic.** Everything inside the
device — `reload`, the latch, the count laws, the IRQ0 divisor — stays binary; a count is
**decoded** where it arrives through `40h`–`42h` (`pit_count_value`, `vdd_pit.c:68-69`) and
**encoded** where it leaves (`pit_count_bytes`, `vdd_pit.c:65-66`). The counting element is then literally the same code in both bases,
which is the point: a second arithmetic path is a second thing to get wrong, and only one of
the two would ever be exercised. Three things fall out of `pit_wrap()` rather than being
special-cased — a written `0000` means 10000, mode 0 runs past terminal count back to 9999
instead of through `0xFFFF`, and `pit_ch2_hz` decodes the speaker's divisor so a BCD tone is
not 4.096× wrong.

⚠ **Invalid digits are undefined by the datasheet**, and we decode each nibble at its decade
weight (`0x1A` → 20) because that is the sequence four decade down-counters actually produce
next — `0x1A`, `0x19`, `0x18` — rather than a clamp we would have invented.

⛔⛔ **NO ORACLE CAN VERIFY IT — and that only became visible after a FALSE PASS.** The probe's
first BCD case took **one** sample, and a binary counter passes that whenever its four nibbles
happen to be ≤ 9, about one value in six. It reported a clean pass while **nothing implements
BCD**. Strengthened to 16 spread samples:

| | 6.22 (QEMU) | dosbox-x | PCem (real BIOS) | ours |
|---|---|---|---|---|
| `pit.bcd.valid` | `0` | **`1`** | `0` | ~~`0`~~ → **`1`** |

**Two of three — including the real-BIOS machine — do not model BCD at all.** So the majority
answer is about emulator coverage, not about hardware, and an AGREE here is *agreement on
absence*.

⇒ **Implemented from the datasheet and marked spec-implemented / unverifiable.** The
abstention rule is recorded in `oracle-rules.json` (probe `pit`, case `pit.bcd.valid`), so
`msdos622` and `pcem` abstain, the rationale prints on every run, and our correct answer stops
reporting itself as a regression. **This is the one place on this surface where the spec
outranks the oracles** — a `0` from a host that does not implement the feature is the absence
of a measurement, not a measurement of absence.

⚠ **And "no oracle" is why the evidence had to move off the probe.** `pit.bcd.valid` asks a
*property* — are all four nibbles decimal — which is exactly the question a binary counter
answers correctly one time in six, and did. The real evidence is `tools/dostest/pit_test.c`
**T12**, which pins **exact counts** instead: `0x9989` ten clocks after `0x9999` (the units
decade *borrows*, it does not step to `0x8F`), `0x0000` at 9999 clocks, the wrap back to
`0x9999` at 10000, a `0000` count raising IRQ0 on the ten-thousandth clock, and the same
counter in **binary** failing the nibble test — the negative control, without which the
positive one proves nothing. **8 of the 14 checks failed against the previous code**; the 6
that passed were the coincidences.

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
| Read-Back command decode (`43h` bits 7:6 = `11`) | **IMPL** | `pit_readback` (`vdd_pit.c:350-370`); both latch bits honoured **active-low** |
| Latch-count for multiple counters in one command | **IMPL** | `pit_readback` loops the three select bits (`:353-354`) |
| Latch-status | **IMPL** | `st_latch[]` / `st_latched[]`, first latch wins (`:355-359`) |
| Status bit 7 — **OUT pin** | **IMPL** | `pit_out_pin` (`:296-317`), `chan_out` (`:122-127`) — **derived** from mode + elapsed, not stored, so it cannot go stale |
| Status bit 6 — **Null Count** | **IMPL** | counter 0 from `cw_armed`/`next_pending` (`:329`); channels from `null_cnt` (`:337`) |
| Status bits 5:0 — access / mode / BCD | **IMPL** | `pit_status_of` (`:323-343`); **`mode_raw`, not the normalised mode** |
| Status read before count when both latched | **IMPL** | served ahead of everything in `pit_in_locked` (`:490-492`) |

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
| 0 | Interrupt on Terminal Count | **IMPL** | count law `vdd_pit.c:38-45` (one-shot, wraps past zero); OUT `:300-302`; one IRQ0 per count loaded (`:205-211`, `irq_armed` `:268`) |
| 1 | Hardware Retriggerable One-Shot | **IMPL** | one-shot count law (`:38`); counter 2 is **triggered** by a gate rising edge (`:105-109`), OUT high until triggered (`:124`); counter 0's gate is tied high, so it never starts and raises **no** IRQ0 (`:202-204`) |
| 2 | Rate Generator | **IMPL** | `:51`; IRQ0 from the accumulator (`:213-226`); on counter 2 a gate low forces OUT high and a rising edge reloads (`:110-113`, `:125`) |
| 3 | Square Wave | **PART** | **decrements by two**, odd-count half `(R+1)/2` (`:46-50`); OUT `:306-310`. ⚠ A bare count write takes over at the end of the **full** period, where the datasheet says the half-cycle (`:217-219`, *"approximated here as the full one"*) |
| 4 | Software Triggered Strobe | **IMPL** | one-shot count law; the one-clock OUT strobe at TC (`:311-313`); one IRQ0 per count loaded |
| 5 | Hardware Triggered Strobe | **IMPL** | as mode 1 for the trigger, as mode 4 for OUT; never starts on counter 0 |

✅ **#175 (s82/s83) closed the old PART/MISS rows** for modes 1, 4 and 5 — the GATE
triggers on counter 2, the one-shot count laws, and the mode-4 strobe — with `p_pit`
section H as the evidence.

✅ **IRQ0 in each mode (#175, s83).** IRQ0 is counter 0's OUT pin and the PIC counts rising
edges. Mode 2/3: one per period. Modes 0 and 4: **one per count loaded** (at terminal count),
a bare re-write after TC included. Modes 1 and 5: **none**, because counter 0's GATE is tied
high and never rises. Until s83 every mode raised once per period.
Evidence: `tools/dostest/p_pit0.asm` on QEMU, DOSBox-X and PCem (modes 0, 4 and the bare
re-write agree at 1; modes 1 and 5 split from the datasheet, with rationales in
`oracle-rules.json`), and `pit_test.c` T_IRQ0. NTVDMEX agrees with the reference on every row.

✅ **Counter 2's "time it" idiom, twice in a row (s83).** Gate low, a new mode-0 count, gate
high, poll 61h bit 5: the SECOND wait returned at once, because a gate-low edge froze the
elapsed count past terminal count and a new count did not clear it. That is what made `p_pit0`
read 0 on NTVDMEX at first (every 220 ms wait collapsed). A count or control word now clears the
frozen elapsed; `pit_test.c` T_WAIT.

✅ **The load rule is right, and it was expensive to learn:** a Control Word arms the next
count write to load and restart; a bare count in a periodic mode waits for the end of the
current period (`pit_load`, `vdd_pit.c:229-271`). The comment there records that the
datasheet halts counting on the first byte of a **count**, not on the Control Word — which
is what lets Lemmings' calibration exit keep ticking.

## 5. GATE inputs

| Counter | GATE source | Status | Evidence |
|---|---|---|---|
| 0 | tied high on the PC | **N/A** | correct by construction — nothing to model |
| 1 | tied high | **N/A** | |
| 2 | **port `61h` bit 0** | **IMPL** | `vdd_pit_ch2_gate` (`vdd_pit.c:94-117`), pushed from `vdd_speaker.c:15`; per mode: enable (0/4), stop-and-reload (2/3), trigger (1/5) |

✅ **Counter 2's gate works** *(FIXED 2026-09-23)*. A high-to-low edge freezes the elapsed
count; low-to-high **resumes** from there rather than restarting — the difference between a
paused stopwatch and a reset one. The setter is idempotent because a guest polling `61h`
rewrites the whole byte constantly.

## 6. Port `61h` (PPI port B) — the PIT's other face

| Bit | Function | Status | Evidence |
|---|---|---|---|
| 0 | Timer-2 GATE | **IMPL** | `vdd_speaker.c:14-15` → `vdd_pit_ch2_gate`. ⚠ The 2026-09-23 row said STORE; the gate was wired the same day and the row was never updated |
| 1 | Speaker data enable | **PART** | the speaker sounds a square wave at counter 2's rate only while bits 0 **and** 1 are set (`vdd_speaker.h:27`, `vdd_audio.c:191-192`). Toggling bit 1 by hand — the PWM trick "RealSound"-style games use to play samples — is **not** heard |
| 2–3 | RAM parity / I/O channel check enables | **STORE** | read back as written (`vdd_speaker.c:29`); nothing raises a parity error, so nothing consumes them |
| 4 | DRAM refresh toggle | **PART** | `vdd_speaker.c:28` — **toggled on every read**, not derived from a 15 µs clock. Deliberate: it makes delay loops terminate. A guest *calibrating* against it gets a number with no relation to time |
| **5** | **Timer-2 OUT** | **IMPL** | `vdd_pit_ch2_out` (`vdd_pit.c:130-133`), derived from mode + elapsed + gate |
| 6–7 | parity / I/O-check status (read) | **IMPL** | read back as written (`:29`); no error source exists, so a guest that never writes them reads 0, the idle answer |

✅ **Bit 5 reports counter 2's OUT pin** *(FIXED 2026-09-23)*. It used to echo whatever bit 5
had been *written*, so the classic "measure elapsed time without interrupts" loop — program a
count, poll bit 5, count the iterations — saw a constant and either fell straight through or
span forever. All three oracles agreed it must move; now `p_pit pit.61h.bit5.toggles` AGREES
with all of them.

⚠ **Bit 4 is still synthesised on purpose** — the DRAM-refresh toggle, flipped on every read so
refresh-poll delay loops terminate. A guest that *calibrates* against it gets a number with no
relation to time. A recorded approximation, not an oversight.

## 7. How time reaches the chip (host side)

Not a register; recorded because every IRQ0-timing defect has lived here rather than in
the chip. The chip advances only when the host adds clocks (`vdd_pit_add_clocks`,
`vdd_pit.c:193-227`). The host's **pacer** (`pit_pacer_thread`, `main.c:5613`) wakes on a
1 ms multimedia timer since #238 (`timeSetEvent`, `main.c:5600-5605`; `Sleep(1)` woke
~485 times a second on XP), advances the chip (`host_pit_sync`, `main.c:13349`) and hands
delivery to `host_pit_deliver` (`main.c:13237`). IRQ0 is held in service until the guest's
EOI ([pic.md](pic.md)). With a pacer, `frame_us = 0` and the VDD's own frame hook does
nothing (`vdd_pit.c:277-289`).

---

## What to fix, in order

Tracked in GitHub: [#175](https://github.com/MrMatthewLayton/ntvdmex/issues/175) (the list that was here was moved there verbatim, 2026-09-27).
Found while re-citing (2026-10-01): the `42h` speaker-divisor half-write (§1), mode 3's
half-cycle load (§4), and `61h` bit 1 as a sample output (§6).

## Re-verified on a quiet rig (2026-09-23) — ⚠ historical

*This records the run **before** the Read-Back and counter 1/2 fixes above landed. Its
"7 of 7 still MISMATCH" is the starting point, not the current state.*

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
