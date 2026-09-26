# Session 81 — the IF/VIF interrupt gate (`irq8.nested`)

**Date:** 2026-09-26 · **Branch:** `m9/completeness` · **Rig:** OFF at session start (share not
mounted, box not answering) — nothing deployed, nothing run.
**Host build:** `82489d48` built locally, **not on the rig**. `bin\` is still `ad6e25cd`.

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
