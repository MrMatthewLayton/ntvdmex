# Inventory — 8259A Programmable Interrupt Controller

**Spec:** Intel 8259A datasheet; IBM AT TechRef for the wiring.
**▶ The hardware reference is [`../ref/pic.md`](../ref/pic.md)** — what the chip *does*.
This file is the companion: what **we** do about it.
**Our implementation:** `src/vdd/vdd_pic.c` (199 lines), `src/vdd/vdd_pic.h`;
the host's delivery decisions in `src/host/main.c`.
**Oracles:** MS-DOS 6.22 (QEMU), dosbox-x, PCem (real AMI 486 BIOS). Off-VM:
`tools/dostest/pic_test.c`. DOS probe: `tools/dostest/p_pic.asm`.
**Marked:** 2026-09-23, **from the code**, with citations. Re-mark after any change.

---

## Headline

**The delivery path is right and it was expensive to get right; the PROGRAMMING
INTERFACE is half a chip.** Everything a handler does — mask, EOI, read the ISR, not be
re-entered — works and has a regression test. Everything a guest might do to
*reconfigure* the chip is either dropped or silently approximated.

> ⚠ **The previous version of this file said "10 fields, all AGREE" and left it there**,
> in the retired PARITY vocabulary, with a note that re-marking against the code was
> owed. That is the same state the 8254 was in before it was marked from the code and
> turned out to have seven gaps. **An all-AGREE probe is not a verified surface — it is
> a verified list of questions.** The rows below are what the probe never asked.

| Group | Marked from the code |
|---|---|
| IRR / ISR / IMR and the priority rule | **IMPL** — and it is the good part |
| ICW1–ICW4 sequence | **PART** — the sequence is walked, but 3 of ICW1's 6 side effects and most of ICW4 are dropped |
| OCW1 (mask) | **IMPL** |
| OCW2 (EOI family) | **PART** — the two EOIs are right, all four **rotation** forms are not modelled |
| OCW3 (read select / poll / SMM) | **PART** — read select and **Poll** done; Special Mask Mode absent |
| Cascade | **PART** — hard-wired to IRQ2 (correct on a PC) but **behaves as if SFNM were on** |

---

## 1. Ports

| Port | Access | Register | Status | Evidence |
|---|---|---|---|---|
| `20h`/`A0h` | W | ICW1 · OCW2 · OCW3 | **PART** | `pic_cmd_write`, `vdd_pic.c:39` — decodes all three, see §3–§5 |
| `20h`/`A0h` | R | IRR · ISR · **poll** | **IMPL** | `pic_in`, `vdd_pic.c:148`; the poll arm at `:157` |
| `21h`/`A1h` | W | ICW2/3/4 · OCW1 | **PART** | `pic_data_write`, `vdd_pic.c:71` |
| `21h`/`A1h` | R | IMR | **IMPL** | `vdd_pic.c:153` |

## 2. The three registers and the priority rule — the part that works

| Item | Status | Evidence |
|---|---|---|
| IMR blocks delivery | **IMPL** | `vdd_pic_can_deliver`, `vdd_pic.c:176` |
| ISR blocks its own line **and all lower priority** | **IMPL** | `vdd_pic.c:122` — `isr & ((bit << 1) - 1)` |
| ISR set at delivery unless AEOI | **IMPL** | `vdd_pic_acknowledge`, `vdd_pic.c:139` |
| Non-specific EOI clears the **highest-priority** ISR bit | **IMPL** | `vdd_pic.c:54` via `pic_top` |
| Specific EOI clears the named bit | **IMPL** | `vdd_pic.c:58` |
| A slave line also puts IR2 in service, and both need an EOI | **IMPL** | `vdd_pic.c:140` and `:166` |
| Host-vectored lines the guest will never EOI are auto-EOI'd | **IMPL** | `vdd_pic_ack_autoeoi`, `vdd_pic.c:155` |
| ISR/IRR updates are **atomic** across threads | **IMPL** | `ISR_SET`/`ISR_CLR`/`IRR_CLR`, `vdd_pic.c:30-34` |

✅ **This is where the two worst bugs in the project lived** — the keyboard
re-entrancy hang and Lemmings' timer — and both are now regression-tested, off-VM and
by probe. Nothing below detracts from that.

⚠ **`vdd_pic_raise` is a plain read-modify-write on `irr` (`vdd_pic.c:108`)** while
everything around it is atomic. **NOT a defect, and marked N/A rather than PART:** the
host does not call it for master lines — `main.c:3149` does its own
`__sync_fetch_and_or` — and the comment there records that nothing raises a *slave*
line cross-lock. Marked here so the next reader does not "fix" it and quietly move the
timer's raise onto a slower path.

⚠ **The DPMI / protected-mode delivery arm still auto-EOIs IRQ0.** That is Doom's path,
it is by-hand only, and no probe here says anything about it. [[irq0-must-be-held-in-service]]

## 3. Initialisation — ICW1–ICW4

| Item | Status | Evidence |
|---|---|---|
| ICW1 recognised, sequence walked | **IMPL** | `vdd_pic.c:41-46`, `:73-79` |
| ICW1 clears the IMR | **IMPL** | `vdd_pic.c:45` |
| **ICW1 resets the status read to IRR** | ⛔ **MISS** | nothing clears `read_isr`; `vdd_pic.c:41-46` |
| **ICW1 resets priority rotation / slave address / edge sense** | **N/A** | there is no rotation or edge-sense state to reset — see §4, §6 |
| **ICW1 clears Special Mask Mode** | **N/A** | SMM does not exist — §5 |
| ICW2 sets the vector base | **IMPL** | `vdd_pic.c:74`; `vdd_pic_vector`, `:169` |
| **ICW3 — the cascade map** | ⛔ **DROP** | `vdd_pic.c:75` consumes the byte and discards it; the cascade is hard-coded to IRQ2 (`:125`, `:140`) |
| ICW4 bit 1 — AEOI | **IMPL** | `vdd_pic.c:76` |
| **ICW4 bit 0 (µPM), bit 3 (BUF), bit 4 (SFNM)** | ⛔ **DROP** | `vdd_pic.c:76` reads only bit 1 |

⛔ **ICW1's read-select reset is the one that is guest-visible and cheap.** The
datasheet lists it with the IMR clear, and we implement one and not the other. A guest
that selects the ISR, re-initialises the chip and then reads `20h` gets our ISR where
hardware gives the IRR.

⚠ **ICW3 being dropped is correct in effect on a PC** (the slave is always on IR2) but
it is an *assumption*, not a decision, until it is written down. It is now.

## 4. OCW2 — the EOI family

| Command | Status | Evidence |
|---|---|---|
| `20h` non-specific EOI | **IMPL** | `vdd_pic.c:54` |
| `60h` specific EOI | **IMPL** | `vdd_pic.c:58` |
| `A0h` rotate on non-specific EOI | ⛔ **PART** | `vdd_pic.c:61` — **clears the bit, does not rotate** |
| `E0h` rotate on specific EOI | ✅ **PART** *(2026-09-23)* | `vdd_pic.c:91` — now **EOIs**; the rotation half still has no priority state |
| `C0h` set priority | ⛔ **MISS** | `default:` |
| `80h`/`00h` rotate in AEOI | ⛔ **MISS** | `default:` |

⛔ **There is no priority state at all.** `pic_top` (`vdd_pic.c:6`) is hard-coded
lowest-bit-first, so "rotate" has nothing to rotate — both rotate forms end the
interrupt and then do not rotate. That is the *right* half to have: **an ISR bit that is
never cleared does not cost one interrupt, it kills that priority level and everything
below it for the rest of the run**, whereas a missing rotation only costs fairness
between devices in a scheme a PC BIOS never programs.

⚠ **A PC BIOS never rotates and DOS software rarely does**, so this is low-frequency —
but "low-frequency" is a guess about guests, and the scope rule says a device is in
because it is in the hardware contract. Recorded as a real gap, priced honestly.

## 5. OCW3 — read select, Poll, Special Mask Mode

| Field | Status | Evidence |
|---|---|---|
| RR/RIS read select, and only when bit 1 is set | **IMPL** | `vdd_pic.c:49` |
| **P — Poll command** | ✅ **IMPL** *(2026-09-23)* | `pic_poll_read`, `vdd_pic.c:133`; armed at `:66`, one-shot at `:157` |
| **ESMM/SMM — Special Mask Mode** | ⛔ **MISS** | bits 6:5 discarded |

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
| Slave lines gated by the master's IRQ2 mask | **IMPL** | `vdd_pic.c:125` |
| Slave lines outranked by master IRQ0/IRQ1 | **IMPL** | `vdd_pic.c:126` |
| Slave line sets IR2 in service; EOI of the last slave line releases it | **IMPL** | `vdd_pic.c:140`, `:166` |
| **A second slave line while IR2 is in service** | ⛔ **WRONG DEFAULT** | `vdd_pic.c:122-127` — the master's IR2-in-service is never consulted |

⛔ **We implement Special Fully Nested Mode without being asked for it.** `can_deliver`
checks the *slave's* own ISR for priority and the master's ISR only for bits 0:1, so a
second slave interrupt gets through while IR2 is still in service. On a real AT, with
SFNM off (which is what the BIOS programs), it must not. ICW4 bit 4 — the bit that
would *request* this behaviour — is discarded (§3).

⚠ Direction of the error matters: we are **more permissive** than the hardware, so the
symptom is a slave handler being re-entered, not an interrupt going missing. Nothing on
the shelf has shown it, because almost nothing here uses IRQ8–15 heavily.

---

## Measured, 2026-09-23 — three new probe cases against three oracles and the rig

`p_pic.asm` grew three cases aimed squarely at the gaps above. **All ten of its existing
questions still agree; none of them was on the list, which was the point.**

| case | 6.22/QEMU | dosbox-x | PCem | ours (was → now) |
|---|---|---|---|---|
| `pic.ocw2.rot.speoi` — does `E0h` end the interrupt? | `0000` | `0000` | `0000` | `0001` → **`0000`** ✅ |
| `pic.ocw3.poll` — poll with nothing pending | **`0000`** | `0001` | `0001` | `0001` → **`0000`** ✅ |
| `pic.icw1.readsel` — what ICW1 resets | `0000` | `0000` | `0001` | `0000` ⛔ **open** |

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

### ⛔ 3. ICW1's read-select reset — OPEN, and deliberately not fixed

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
[`log/sessions/session-81.md`](../log/sessions/session-81.md).

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

1. ✅ ~~Make `E0h` end the interrupt.~~ **DONE 2026-09-23** — unanimous.
2. ✅ ~~Implement the Poll command.~~ **DONE 2026-09-23** — spec-implemented on a 1–2
   split, abstention recorded.
3. ⛔ **ICW1 must reset the read select to IRR.** Cheap and guest-visible, but **blocked
   on a second oracle** — see above. Do not fix it on PCem alone.
4. **Consult the master's IR2-in-service for slave delivery**, and honour ICW4 bit 4, so
   Special Fully Nested Mode is something a guest asks for rather than something we do
   unconditionally (§6). Needs a probe that can get two slave lines in flight, which
   nothing here can do yet.
5. **Priority rotation** (a rotation register, then `A0h`/`C0h`/`E0h` rotating), and then
   **Special Mask Mode** on top of it. Lowest priority: no oracle disagreement to chase
   and nothing on the shelf exercises either.
