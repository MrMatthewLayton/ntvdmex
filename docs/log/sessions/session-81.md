# Session 81 — the IF/VIF interrupt gate (`irq8.nested`)

**Date:** 2026-09-26 · **Branch:** `m9/completeness` · **Rig:** on all along. At first I read
the unmounted share as "rig off"; `mount_smbfs` it after every reboot.
**Host build:** **`f484fc3d`** in `bin\` (displaced `ad6e25cd` archived under `debug\prev\`;
`ntvdmhost_prev.exe` = `a0294462`, untouched). **Rig-verified, not yet confirmed by hand.**

---

## ✅ Result

`irq8.nested` **4 → 0**, AGREE with msdos622 / dosbox-x / pcem / pcem-vesa. Regressions:
the rest of `p_pic` is unchanged (its two DISPUTED rows are the known ones); Skyroads
`n8=0 max_ms=7`, with the same `irq0_inj` as before the fix; Notepad launches, X closes it,
the host exits; off-VM battery 1670/0. Logs in `runs/s81_ifv/`.

### What the census measured (runs 1–5, `p_irq8`)

| Question | Answer |
|---|---|
| IF in a live V86 frame (async path) | **1 in every sample** — no `00`/`01` states at all |
| VIF after we `SetThreadContext` it clear, read straight back | stays clear (`10` ×0x8b) — the write sticks |
| IF in the VTIB after an event exit | the **virtual** flag (IF=0 in 0x25c samples inside the handler) |
| VIF in the VTIB | never set, in any sample |
| `0x714` bit 9 | 0 in every sample — uninformative here |
| Where the re-entries came from | the **cooperative** device path, at the handler's own EOI `out` (`0100:02F9`), VTIB `0x30246` |

**The chain:** an async IRQ 0 was let in while the RTC handler had VIF clear, because live
IF=1 satisfied "IF or VIF". It pushed a FLAGS image with IF=1; INT 08h's `iret` set VIF
*inside the RTC handler*; its next I/O exit then honestly reported interrupts on, and the
cooperative gate re-entered IRQ 8. 5 such IRQ 0 deliveries ↔ 5 corrupted handler runs ↔
`nested` 4–5.

### The fix

The async V86 gate tests **VIF alone** once any live frame has shown VIF set
(`g_vif_live_seen`, i.e. proof that VME maintains it). Until then, or on a CPU without VME,
it keeps the old IF-or-VIF test, so nothing starves. The cooperative gates are unchanged:
their IF is honest.

### Starvation risk: measured, not argued

| Guest | live samples | VIF-clear stretches | Note |
|---|---|---|---|
| Skyroads | all `110` | 0 | identical before/after |
| ZAR | none | — | its real-mode stretches never reach this gate |
| Doom | none | — | same |
| `p_irq8` after fix | `100` ×0x38 (refused, correctly) | max 16 ms | the handler windows |

### Kept as standing instruments
`STAGE2: IFV census …` (per path, per IF/VIF state, plus shadow and re-entry counts) and the
`STAGE2: IFV irqNN` delivery trace. Both are now also printed on the headless forced exit
(`ifv_report()`), which is how ZAR's runs end. The per-delivery `GetThreadContext` readback
has been removed: its question is answered and it cost a syscall on every injection.

### Owed by hand
**ZAR** (its sound init was the named risk; still silent for unrelated reasons, so check
that it *renders and plays as before*), **Skyroads by ear**, plus s80's three settings and
Heretic items.

---

## The original plan (kept)

---

## The choice

The user picked **the IF/VIF interrupt gate** from s80's candidate list. The three by-hand
checks s80 left owed (Settings → DOS note, Settings → Sound / GUS untick, Heretic low-detail
3D view) are still owed; the rig was off.

## The problem, restated from the code

`p_irq8.com`'s handler EOIs before its `iret`; three oracles re-enter it 0 times, we re-enter
it 4. Every host gate asks `if_or_vif()` — IF **or** VIF — and that shape was set in s11
because two things were true at once:

1. Under VME a V86 `cli`/`sti` moves only VIF, so an IF-only gate never saw Skyroads' `sti`.
2. Seeding VIF through the VTIB at entry is sanitised away, so a guest that never executes
   `sti` could run with VIF clear while its interrupts are logically on.

**The hypothesis this session adds (unmeasured):** the async injector writes the guest's
frame back with `SetThreadContext`, and NT forces IF on in any user-mode frame it is handed.
If so, IF in a **live** V86 frame is always 1 and carries no information — which by itself
makes `if_or_vif()` always true on the async path, and is exactly `irq8.nested`: we clear
VIF on injection, IF comes back set, the next IRQ 8 is let in.

The fear that parked the fix is (2): a VIF-only gate starving a guest whose VIF reads clear
while it is logically on — DPMI `0301`/`0302` enter V86 with the frame at `0x20202` (IF set,
VIF clear), and ZAR's sound init needs an interrupt to arrive there.

## What was built: a census, not a fix

`82489d48` changes no decision. It counts what the bits actually read:

```
STAGE2: IFV census (IF,VIF) live{00= 01= 10= 11=} vtib{00= 01= 10= 11=} starve_max_ms= stretches= shadow{ irqNN= }
```

- `live` — the async injector's V86 frame (`ifv_note(0, …)` just before the V86 gate).
- `vtib` — the cooperative IRQ0/IRQ1 gates reading the VTIB after an event exit.
- `shadow` — per line, deliveries the current gate made with VIF clear, i.e. what a VIF-only
  gate would have refused.
- `starve_max_ms` — the longest unbroken run of `live` samples with VIF clear: how long a
  VIF-only gate would have held **every** line off.

## What to run when the rig is back

Deploy `82489d48` to `bin\` (instrument only; `prev` untouched), then:

| Guest | Why | Read |
|---|---|---|
| `p_irq8.com` | the defect itself | expect `live{10}` > 0 and `shadow{irq08}` ≈ 4 |
| Skyroads | the s11 case (STI inside INT 1Ch) | `starve_max_ms` should stay small |
| Doom (DPMI, `0302` file I/O) | V86 via `0x20202` | `starve_max_ms` across real-mode calls |
| ZAR | the named risk | `starve_max_ms` and `shadow{irq05/07}` |
| 6.22 `COMMAND.COM` idle at the prompt | a guest that may never `sti` | `live{10}` share |

**Decision rule.** If `live` shows no `00`/`01`s — IF always set — the hypothesis holds and
VIF is the only live signal. If `starve_max_ms` stays at a few ticks on every guest, the
VIF-only gate is safe as-is. If some guest shows a long stretch, the fix is to make the
host's own V86 entry frames coherent (VIF with IF: `0301`/`0302`, the `0303` callback return,
program start) before the gate changes — and that needs the s11 "sanitised" result
re-measured, because it was measured through the VTIB, not through `SetThreadContext`.
