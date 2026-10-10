# Inventory — 8259A Programmable Interrupt Controller

**Spec:** Intel 8259A datasheet; IBM AT TechRef for the wiring.
**▶ The hardware reference is [`../ref/pic.md`](../ref/pic.md)** — what the chip *does*.
This file is the companion: what **we** do about it.
**Our implementation:** `src/vdd/vdd_pic.c` (364 lines), `src/vdd/vdd_pic.h`;
the host's delivery decisions in `src/host/main.c`.
**Oracles:** MS-DOS 6.22 (QEMU), dosbox-x, PCem (real AMI 486 BIOS). Off-VM:
`tests/unit/pic_test.c`. DOS probe: `tests/probes/dos/p_pic.asm`.
**Marked:** 2026-09-23, **from the code**, with citations. **Re-marked 2026-10-01 for #174**
(rotation, Special Mask Mode, ICW1's resets, SFNM) — line numbers below are post-#174.

---

## Headline

**The delivery path is right and it was expensive to get right.** Everything a handler
does — mask, EOI, read the ISR, not be re-entered — works and has a regression test.
**Since #174 (2026-10-01) the programming interface is the datasheet's too:** priority
rotation (all four OCW2 forms), Special Mask Mode, ICW1's side effects and SFNM. Those
are **spec-derived and off-VM-tested only** — no oracle probe asks them yet, apart from
`E0h`'s EOI half. Still dropped: ICW3 (correct in effect on a PC), ICW4's µPM and BUF and
ICW1's LTIM (no visible effect without a bus), and the host's *choice between* pending
lines under rotation (§4).

> ⚠ **The previous version of this file said "10 fields, all AGREE" and left it there**,
> in the retired PARITY vocabulary, with a note that re-marking against the code was
> owed. That is the same state the 8254 was in before it was marked from the code and
> turned out to have seven gaps. **An all-AGREE probe is not a verified surface — it is
> a verified list of questions.** The rows below are what the probe never asked.

| Group | Marked from the code |
|---|---|
| IRR / ISR / IMR and the priority rule | **IMPL** — and it is the good part; the rule follows the rotation since #174 |
| ICW1–ICW4 sequence | **IMPL** for everything a PC can see — ICW1's resets, ICW4's AEOI and SFNM; ICW3, µPM, BUF, LTIM dropped (§3) |
| OCW1 (mask) | **IMPL** |
| OCW2 (EOI family) | **IMPL** in the chip *(#174)* — ⚠ the host still walks pending lines in the fixed order (§4) |
| OCW3 (read select / poll / SMM) | **IMPL** *(SMM: #174)* |
| Cascade | **IMPL** *(#174)* — hard-wired to IRQ2 (correct on a PC); fully nested by default, SFNM on request |

---

## 1. Ports

| Port | Access | Register | Status | Evidence |
|---|---|---|---|---|
| `20h`/`A0h` | W | ICW1 · OCW2 · OCW3 | **IMPL** | `pic_cmd_write`, `vdd_pic.c:101` — decodes all three, see §3–§5 |
| `20h`/`A0h` | R | IRR · ISR · **poll** | **IMPL** | `pic_in`, `vdd_pic.c:246`; the poll arm at `:255` |
| `21h`/`A1h` | W | ICW2/3/4 · OCW1 | **PART** | `pic_data_write`, `vdd_pic.c:198` — ICW3 dropped (§3) |
| `21h`/`A1h` | R | IMR | **IMPL** | `vdd_pic.c:252` |

## 2. The three registers and the priority rule — the part that works

| Item | Status | Evidence |
|---|---|---|
| IMR blocks delivery | **IMPL** | `pic_line_open`, `vdd_pic.c:55`; `vdd_pic_can_deliver`, `:267` |
| ISR blocks its own line **and all lower priority** — in the current rotation | **IMPL** | `pic_blockers`, `vdd_pic.c:40`; ranks from `pic_rank`, `:11` |
| ISR set at delivery unless AEOI | **IMPL** | `pic_intack`, `vdd_pic.c:92`, from `vdd_pic_acknowledge`, `:292` |
| Non-specific EOI clears the **highest-priority** ISR bit | **IMPL** | `vdd_pic.c:156` via `pic_top` (`:17`) |
| Specific EOI clears the named bit | **IMPL** | `vdd_pic.c:160` |
| A slave line also puts IR2 in service, and both need an EOI | **IMPL** | `vdd_pic.c:301` and `:326` |
| Host-vectored lines the guest will never EOI are auto-EOI'd | **IMPL** | `vdd_pic_ack_autoeoi`, `vdd_pic.c:316` |
| ISR/IRR updates are **atomic** across threads | **IMPL** | `ISR_SET`/`ISR_CLR`/`IRR_CLR`, `vdd_pic.c:80-84` |

★ **The default state is pinned EXHAUSTIVELY** (`pic_test.c`, *"default: master resolver ==
the old lowest-bit-first rule"*): every ISR byte × four IMRs × eight lines answers exactly
as the pre-#174 test `isr & ((bit << 1) - 1)` did. With `prio_low = 7`, `pic_rank` is the
identity, so rotation costs a guest that never rotates nothing.

✅ **This is where the two worst bugs in the project lived** — the keyboard
re-entrancy hang and Lemmings' timer — and both are now regression-tested, off-VM and
by probe. Nothing below detracts from that.

⚠ **`vdd_pic_raise` is a plain read-modify-write on `irr` (`vdd_pic.c:261`)** while
everything around it is atomic. **NOT a defect, and marked N/A rather than PART:** the
host does not call it for master lines — `main.c:3149` does its own
`__sync_fetch_and_or` — and the comment there records that nothing raises a *slave*
line cross-lock. Marked here so the next reader does not "fix" it and quietly move the
timer's raise onto a slower path.

✅ **The DPMI / protected-mode arms hold IRQ0 in service too (#173, s81).** The async PM
arm used to EOI on delivery and the two synchronous injectors never told the PIC, so a
client's non-specific `out 20h,20h` cleared a *lower* line's in-service bit. They now use
`irq0_ack()` / `irq0_pm_claim()`, and the default PM INT 08h handler EOIs as the BIOS does.
Test machine: Doom `irq0_isr` strict=4061 against 4066 raises, 0 blocked, 0 timeouts, no fallback;
Skyroads `n8=0 max_ms=7`; Win16 Notepad opens and closes. [[irq0-must-be-held-in-service]]

## 3. Initialisation — ICW1–ICW4

| Item | Status | Evidence |
|---|---|---|
| ICW1 recognised, sequence walked | **IMPL** | `vdd_pic.c:103-125`, `:198-210` |
| ICW1 clears the IMR | **IMPL** | `vdd_pic.c:107` |
| **ICW1 resets the status read to IRR** | ✅ **IMPL** *(#174 — spec-derived, see below)* | `vdd_pic.c:119` |
| **ICW1 assigns IR7 the lowest priority** | ✅ **IMPL** *(#174)* | `vdd_pic.c:120` |
| **ICW1 clears Special Mask Mode** | ✅ **IMPL** *(#174)* | `vdd_pic.c:120` |
| ICW1 clears rotate-in-AEOI | **IMPL** — *not on Intel's list*; QEMU's init reset does it | `vdd_pic.c:120` |
| **ICW1 with IC4 = 0 zeroes the ICW4 functions** | ✅ **IMPL** *(#174)* | `vdd_pic.c:123` — AEOI and SFNM |
| ICW1 resets the edge-sense circuit | **IMPL** in effect | `vdd_pic.c:106` clears the IRR (see the 2026-09-23 split below) |
| ICW1 sets the slave-mode address to 7 | **N/A** | no slave-address state; ICW3 is dropped |
| ICW2 sets the vector base | **IMPL** | `vdd_pic.c:201`; `vdd_pic_vector`, `:334` |
| **ICW3 — the cascade map** | ⛔ **DROP** | `vdd_pic.c:202` consumes the byte and discards it; the cascade is hard-coded to IRQ2 (`:288`, `:301`) |
| ICW4 bit 1 — AEOI | **IMPL** | `vdd_pic.c:203` |
| ICW4 bit 4 — **SFNM** | ✅ **IMPL** *(#174)* | `vdd_pic.c:207`; used by `pic_blockers`, `:47` |
| ICW4 bit 0 (µPM), bit 3 (BUF) · ICW1 LTIM | **DROP** | no effect a host without a bus cycle can show |

⚠ **ICW1's read-select reset is SPEC-DERIVED AND UNVERIFIED BY ORACLE.** Implemented to the
datasheet (#174). The only oracle whose answer discriminates it is PCem, and PCem does
**not** reset it (2026-09-23 table below); QEMU and dosbox-x clear the IRR at ICW1, so their
read cannot say which register came back. One emulator against the datasheet is not a
second oracle. ⚠ **Our own `pic.icw1.readsel` answer does not move** (`0000` before and
after): we clear the IRR at ICW1, so both registers read zero and the probe cannot see
the fix on our host either. A discriminating probe needs a request latched *after* ICW1
and before the read — the shape `pic_test.c` uses ("icw1: ...so a read with no OCW3
returns the IRR").

⚠ **ICW3 being dropped is correct in effect on a PC** (the slave is always on IR2) but
it is an *assumption*, not a decision, until it is written down. It is now.

## 4. OCW2 — the EOI family

| Command | Status | Evidence |
|---|---|---|
| `20h` non-specific EOI | **IMPL** | `vdd_pic.c:156` — the highest in the *current* rotation |
| `60h` specific EOI | **IMPL** | `vdd_pic.c:160` |
| `A0h` rotate on non-specific EOI | ✅ **IMPL** *(#174)* | `vdd_pic.c:165` — EOIs, then the ended line is the lowest; nothing in service ⇒ no rotation (as QEMU) |
| `E0h` rotate on specific EOI | ✅ **IMPL** *(EOI 2026-09-23, rotation #174)* | `vdd_pic.c:177` |
| `C0h` set priority | ✅ **IMPL** *(#174)* | `vdd_pic.c:182` — not an EOI |
| `80h`/`00h` rotate in AEOI | ✅ **IMPL** *(#174)* | `vdd_pic.c:187-188`; acts at acknowledge — `pic_intack` `:92`, `vdd_pic_ack_autoeoi` `:323` |
| `40h` no-op | **IMPL** | `default:` |

✅ **Priority is a ring now** (`prio_low`, the lowest-priority IR; `pic_rank`). It drives
the ISR blocking rule, the non-specific EOI and the poll — everything the *chip* decides.
Spec-derived; no probe asks any rotation question yet.

⚠ **NOT DONE: the host's choice between pending lines.** The 8259A delivers the
highest-priority pending request; ours has no wire, so the host picks. It walks
`g_irq_order` (`main.c:2822`: `0, 1, [2 → 8–15], 3–7`, IRQ0/IRQ1 on dedicated paths) and
asks `vdd_pic_can_deliver` line by line — the fixed order. After a guest rotates,
`can_deliver` still refuses everything an in-service line outranks, so **re-entrancy and
nesting follow the rotation**; but with two lines pending and neither in service, the host
may take the one the rotated chip ranks lower. Fixing it means re-ordering three host loops
plus the IRQ0/IRQ1 paths. Consulting the IRR from inside `can_deliver` instead is
**unsafe**: the host clears its own latch without clearing the IRR when it drops an
unhooked line (`main.c:25579`), so a stale IRR bit would block a lower line for good.
Recorded, not faked. A PC BIOS never rotates and DOS software rarely does.

## 5. OCW3 — read select, Poll, Special Mask Mode

| Field | Status | Evidence |
|---|---|---|
| RR/RIS read select, and only when bit 1 is set | **IMPL** | `vdd_pic.c:127` |
| **P — Poll command** | ✅ **IMPL** *(2026-09-23)* | `pic_poll_read`, `vdd_pic.c:235`; armed at `:146`, one-shot at `:255`; follows rotation and SMM since #174 |
| **ESMM/SMM — Special Mask Mode** | ✅ **IMPL** *(#174)* | set/clear `vdd_pic.c:130`; the effect is in `pic_blockers`, `:44` |

**SMM as modelled:** the resolver sees `ISR & ~IMR` — a *masked* in-service line stops
blocking anything; an *unmasked* in-service line still blocks itself and below (QEMU, MAME
and Bochs read the datasheet the same way). The non-specific EOI still works from the full
ISR: the datasheet says a guest in SMM must use a specific EOI. Spec-derived; no probe.

⛔⛔ **POLL WAS THE DANGEROUS ONE, because it is the "runs but lies" shape** — and that
is why it was worth implementing on a 1-vs-2 oracle split. A poll read and a status read
are *the same `IN` on the same port*; only the last OCW3 tells them apart. So a guest
that polled did not get an error — it got the **IRR byte**, and read it as
`bit 7 = interrupt pending, bits 2:0 = level`. An IRR of `0x01` (IRQ0 requested) reads as
*"no interrupt pending"* because bit 7 is clear; an IRR of `0x80` (IRQ7) reads as
*"interrupt pending, level 0"*. Plausible, wrong, and silent. **Fixed 2026-09-23.**

## 6. The cascade

| Item | Status | Evidence |
|---|---|---|
| Slave lines gated by the master's IRQ2 mask | **IMPL** | `vdd_pic.c:288` → `pic_line_open(&m, 2)` |
| Slave lines outranked by whatever outranks IR2 on the master | **IMPL** | `vdd_pic.c:288` — IRQ0/IRQ1 by default, the rotation otherwise |
| Slave line sets IR2 in service; EOI of the last slave line releases it | **IMPL** | `vdd_pic.c:301`, `:326` |
| **A second slave line while IR2 is in service** | ✅ **IMPL** *(#174)* | held (fully nested) unless the master's ICW4 set SFNM; `pic_blockers`, `vdd_pic.c:47` |

✅ **Fully nested by default, SFNM on request (#174).** `can_deliver` used to check the
master's ISR for bits 0:1 only, so a second slave interrupt got in while IR2 was still in
service — SFNM without anyone asking. Now IR2 in service holds the whole slave until the
master is EOI'd, as on an AT whose BIOS wrote ICW4 = `01h`. A master ICW4 with bit 4 set
lets a *higher* slave line through (the slave's own ISR still nests); IR2 in service still
blocks IRQ3–7 in either mode.

⚠ **THIS IS THE ONE DEFAULT-STATE BEHAVIOUR CHANGE IN #174.** It is safe for the host
because **IR2 in service already blocked IRQ3–7** — any path that left IR2 set was
already killing five master lines. Every host path that sets it releases it:
`vdd_pic_eoi` on a slave line clears IR2 with the last slave ISR bit (our stubs, the PM
default-handler reflection at `main.c:22314`, a failed PM inject at `main.c:30336`),
`vdd_pic_ack_autoeoi` does acknowledge + EOI, and a guest handler's own `out 20h,20h`
does the rest. What it CAN change: a guest whose slave handler EOIs the master and `sti`s
long before its `iret` now gets the next slave interrupt only then (as on an AT); one that
never EOIs the master already lost IRQ3–7 and now loses the rest of the slave too.

---

## Measured, 2026-09-23 — three new probe cases against three oracles and the test machine

`p_pic.asm` grew three cases aimed squarely at the gaps above. **All ten of its existing
questions still agree; none of them was on the list, which was the point.**

| case | 6.22/QEMU | dosbox-x | PCem | ours (was → now) |
|---|---|---|---|---|
| `pic.ocw2.rot.speoi` — does `E0h` end the interrupt? | `0000` | `0000` | `0000` | `0001` → **`0000`** ✅ |
| `pic.ocw3.poll` — poll with nothing pending | **`0000`** | `0001` | `0001` | `0001` → **`0000`** ✅ |
| `pic.icw1.readsel` — what ICW1 resets | `0000` | `0000` | `0001` | `0000` — *implemented to the datasheet by #174; this case cannot see it (§3)* |

### ✅ 1. `E0h` is still an EOI — unanimous, so no judgement was needed

All three oracles clear the in-service bit. We had `E0h` filed under *"other
rotate/priority forms: nop"* and did neither half. **An ISR bit that is never cleared
does not cost one interrupt — it kills that priority level and everything below it for
the rest of the run.** Fixed; the rotation half is still absent and still recorded (§4).

### ✅ 2. The Poll command — implemented on a 1-vs-2 split, and the reasoning matters

**Only QEMU implements it.** dosbox-x and PCem both drop the P bit and hand back the
selected register, so the case reads `0x01` — the ISR it was told to select — instead of
`0x00`. Two of three do not model the feature, so their answer is *the absence of a
measurement, not a measurement of absence*: the same footing as the 8254's BCD bit.

⇒ **Better evidenced than BCD, though**, because there the sole implementer (dosbox-x)
had nothing corroborating it, and here the sole implementer **agrees with the
datasheet**. Abstention recorded in `oracle-rules.json`.

⛔ **And it was worth doing on a split because of the failure shape.** A poll read and a
status read are *the same `IN` on the same port*; only the last OCW3 tells them apart. A
host that drops P therefore fails silently and plausibly — an IRR of `0x01` reads as *"no
interrupt pending"* because bit 7 is clear, an IRR of `0x80` reads as *"pending, level
0"*. `pic_test.c` pins the acknowledge side too (a poll read sets ISR and clears IRR) and
the one-shot rule. **6 of its checks failed against the previous code.**

### ⚠ 3. ICW1's read-select reset — implemented to the DATASHEET by #174, still unverified

> *(2026-10-01)* The fix went in on the datasheet's authority alone (§3). Everything below
> about the oracles still stands: no second machine has been seen to answer.

All three hosts answer `AH = 0x00`, but **that only discriminates on PCem.** QEMU and
dosbox-x report `AL = 0x00` as well — their ICW1 clears the IRR, so both registers are
zero and the read cannot say which one it returned. Only PCem keeps the pending bit
(`AL = 0x01`), and its `AH = 0x00` says the select was **not** reset.

> **One oracle is not a pass** — this session's own lesson, three times over. The gap
> stays recorded and unfixed until a second machine can see it.

⚠ The `AL` half is a genuine 2–1 split about whether ICW1's edge-sense reset clears a
*latched* request. We clear it, i.e. we follow the majority **by accident rather than by
decision** — recorded here so that stays visible.

⚠ **The first cut of this case was measuring the wrong thing** and it is worth keeping:
it restored the real IMR immediately after the ICW sequence, which unmasks IRQ0 — so with
interrupts enabled the latched request was *delivered* before the read, consuming the
very bit the case exists to see. The case was measuring interrupt latency. It now keeps
IRQ0 masked across both reads and unmasks afterwards; PCem's answer survived the fix, so
the split is real.

## Measured, 2026-09-25 (s80) — the slave's lines reach a guest now; `p_irq8.com`

The chip model always had the slave and the cascade (`vdd_pic.c:165`). The **host** did not:
device IRQs were latched in `g_irqn_pending[8]` and every delivery path walked lines 2–7, so an
IRQ 8–15 was raised and never delivered. North star 2 needs IRQ 11 for the GUS.

**Now:** a sixteen-line latch, every cooperative path walking the AT's priority order
(`0, 1, [2 → 8–15], 3–7`, `g_irq_order`), vectors from the PIC's programmed bases, the DPMI
PM vectors `70h–77h` for the slave (`irq_pm_vec`), and the **asynchronous** injector covering
the slave too. That last part is not optional: with slave lines cooperative-only, a guest
spinning in a loop got its RTC interrupt once per timer tick — **5 in 5 BIOS ticks against
~280** on the oracles.

| case | 6.22/QEMU | dosbox-x | PCem | ours |
|---|---|---|---|---|
| `irq8.fired` — the RTC's 1024 Hz periodic IRQ reaches a hooked INT 70h | `1` | `1` | `1` | **`1`** ✅ (was `0`) |
| `irq8.regc.pf` — PF as the handler sees it | `1` | `1` | `1` | `1` ✅ |
| `irq8.nested` — handler re-entered while running | `0` | `0` | `0` | **`0`** ✅ (was `4`; fixed s81) |

### ✅ `irq8.nested`: FIXED s81 — the async gate's live IF was never the guest's

**Measured (s81, census `ifv_note`):** in a *live* V86 frame, the one the async injector reads
with `GetThreadContext`, IF was 1 in every sample. That includes samples inside a handler
that had not executed `sti`, and reads straight after we `SetThreadContext` it clear. VIF is
the only live signal. The VTIB after an event exit is different: there IF *is* the virtual
flag. The chain that re-entered the handler: an IRQ 0 let in while VIF was clear pushed a
FLAGS image with IF=1, and its `iret` turned the RTC handler's interrupts **on**. The
cooperative path then read an honest VTIB that had been made wrong. **Fix:** the async V86
gate tests VIF alone once any live frame has shown VIF set (proof that VME keeps it); before
that it falls back to IF-or-VIF. Skyroads' timing is unchanged, the Win16 close works, and
ZAR's and Doom's real-mode stretches never reach that gate. See
session 81.

The original analysis, kept for the record:

The probe's handler EOIs *before* its `iret`. A real CPU cleared IF on the way in, so the next
IRQ 8 waits for the `iret`. Ours re-entered 4 times. The host's gate asks **"IF or VIF"**
(`if_or_vif`, and the same test inline in `async_inject_irq`), because under VME a V86 guest's
`cli`/`sti` move only VIF while the kernel keeps IF set — and a guest that never executes `sti`
has VIF clear with interrupts logically on. So the gate cannot see a handler's closed window.
**Every line has this**, IRQ 0 and 1 included; `pic.hook.nested` passes only because its
handler EOIs at the very end.

Not fixed here, on purpose: making VIF authoritative (once it has ever been seen set) changes
what nested real-mode calls get — DPMI `0301`/`0302` enter V86 with IF set and VIF clear, and
ZAR's sound init depends on an interrupt arriving there. That fix needs the timing and sound
regressions re-proven on its own, not as a side effect of the GUS.

## What to fix, in order

Tracked in GitHub: [#174](https://github.com/MrMatthewLayton/ntvdmex/issues/174), [#173](https://github.com/MrMatthewLayton/ntvdmex/issues/173) (the list that was here was moved there verbatim, 2026-09-27).

