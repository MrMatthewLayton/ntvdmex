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

---

## Part 2 — ZAR sound (added by the user; "never worked")

**Host `cd5f9f12` in `bin\`. Rig-verified; NOT yet heard by a human.**
`prev` = `f484fc3d` (the user confirmed ZAR/Skyroads/Doom play on it).

### Result, measured headless (`runs/s81_ifv/zar_refl6.log`)
Of 349 SB blocks, 268 carry signal (81 flat, which quiet stretches are); `REPLAYED_LOUD=0`;
10% inserted silence, all of it at startup. 270 IRQ-5 reflections into the Miles ISR, none
failed. Before: one block, then a wedge (reflection on) or no SB access at all (off).

### It was three gaps, one behind the other
1. **The nested `0301/0302` loop never set `g_in_exec`.** Every IRQ offered during a
   real-mode call bailed `why=0x14`. s59 read that as `HOST_CS` (14 decimal); 0x14 is **20,
   `not_in_exec`**. Now `g_nested_rm` + `g_in_exec` bracket that `v86_run` **only**, and the
   `g_simint_busy` guard (the E1M1 crash) lets through only device lines whose real-mode
   vector is guest code. ⇒ Miles' single-cycle self-test IRQ 5 arrived.
2. **The BIOS tick stood still inside a nested call.** Miles times its self-test on
   `0040:006C` (SBLASTER.DIG `+0xa53`: `cmp ax,es:[46Ch] / je`). IRQ 0 cannot be delivered
   there (its vector is our BOP stub), so now the host does the BIOS bookkeeping, using the
   PM `no_app_timer` rule: billed against owed ticks, pending consumed (`why=31
   nested_tick`). ⇒ init completed.
3. **The PM default IRQ stub could not reflect to a guest-owned real-mode ISR.** DOS/4GW's
   PM pass-up handler for INT 0Dh chains to our `177:0027` stub. The s80 arm handled only
   IVT-is-ours, so every streaming IRQ was "PM ISR ABANDONED" (3,323 in one run). New:
   `dpmi_reflect_irq_to_rm()`, which is 0302 without an RMCS: IRET frame to the catcher,
   IF clear on entry, own stack at `code_base:FB00`. ⇒ streaming.

Then **`0300` reflection is ON by default** (`simintrefl_off.flag` opts out; the old
opt-in file was removed from the rig). The DPMI spec says `0300` runs the real-mode
handler; the only reason it was off was the wedge.

### Two host crashes found on the way, both in the heartbeat thread
- **My own:** the new `pic{}/aw5{}` fields overflowed `char b[640]`, the same trap the file
  already warns about. Now 2048, with `aw5{}` capped.
- **Pre-existing, found on Mario:** `pal2668`/`code@csip` read guest memory check-then-deref;
  the A0000 window was remapped in between and the heartbeat AV'd. All heartbeat guest
  reads now go through `ReadProcessMemory`. Mario was clean ×2 afterwards.

### Regression (all on the default-on build)
Skyroads `n8=0 max_ms=7`, `irq0_inj` unchanged · Doom SB blocks 0x97b/0x9a2 against 0x95b
before (a forced headless ending in 1 of 3 runs is historical variance: 9 of 40 older Doom
logs end that way) · Duke3D, Heretic, Hexen, Wolf3D, Heaven7 (GUS voices + nonzero samples)
run with no FATAL, wedge or abandon · `p_irq8`/`p_pic` unchanged · Notepad opens and closes ·
off-VM 1670/0.

### Owed by hand
**ZAR with sound, by ear** (music + effects), plus Doom sound and Duke3D sound, because
both now pass through the new reflection/nested paths.

---

## Part 3 — the shelf sweep (`runs/s81_sweep/sweep.txt`, the user's own words)

Candidate zip `dist\ntvdmex-20260927-f5d0f4c.zip` (host `cd5f9f12`). **Not promoted.**
User decisions (2026-09-27): **sweep defects first**, then the host-UI programme, with
**Save/Load State removed + a GH issue**, **Renderer = GDI + DirectDraw only** (the menu
choice made real, D3D9/OpenGL removed), and **unimplemented Machine/Debug/Help items
removed + GH issues**.

### Fixed this part (host `e37cb278` in `bin\`, NOT yet re-checked by hand)
- **DIR showed only `.` and `..`** — two defects. (1) AH=29h ignored AL's control bits.
  XP's DIR parses `*` with AL=0Eh into a pre-filled `???????????`, and we blanked the
  extension. `p_fcb` grew five cases, all four oracles unanimous. (2) DOS searches matched
  LONG names; they now match each entry's 8.3 name against the 11-byte template, as NTVDM
  does. Live: `dir` lists `LONGFI~1.TXT` and every other file.
- **`exit` did nothing** — stock launches XP's shell `/P <dir>`, and the bare launch had
  lost that. With `/P`, EXIT hands back through BOP 54 sub 01 (GetNextVDMCommand, NT's
  CMDINFO block) a second time; a bare session now ends the VDM there. Sub 00 got an arm
  too. Live: dir → an EXEC'd program → exit closes the window.
- **Settings → DOS note** rewritten in plain language.

### Measured, not fixed
- **Doom's quit sound.** Recorded (new WAV recorder, `cfg\wavrec.flag`). `IRQ0TL`: the
  timer runs ~0x85/s in play and collapses to **0x15–0x22/s** for the seconds of the quit
  wait (`I_WaitVBL` polling 3DAh) — **identical on `a0294462`**, so pre-existing, not an s81
  regression. Delivery is 99% of raises; the ticks are never *raised* in those seconds —
  consistent with the port-trap ceiling (every 3DAh read traps under `g_lock`, starving the
  pacer). In-game "95% OK" is the known one-block replay race (`REPLAYED_LOUD` 0x14 this
  run, 0x36 before s81).
- **Notepad Edit → Paste** — by design today: the WOW clipboard answers "empty" because
  there is no bridge that puts host clipboard text into guest global memory
  (`wowuser.h`, "THE CLIPBOARD PAIR"). Needs the host to call krnl386's GlobalAlloc
  (`wowcall.h` has the host→16-bit mechanism). A WOW feature, not a bug.
- **Paint tools/resize** — not yet investigated.
- **Menu inventory** (agent report, all `main.c`): Close Program has an ID and no handler;
  Open Executable/Recent, Save/Load State, all of Edit, Machine (except Limit Speed/Capture
  Mouse), Debug, Capture recording, Help Quick Start are `IDM_STUB`; Renderer is read by
  nothing; hq2x is a no-op; Show Host Cursor was removed earlier (only the Settings
  checkbox remains); View ends in a stray separator; Settings' Aspect is a checkbox over a
  four-way setting; Take Screenshot silently does nothing outside 8-bit frames.

### Still the user's
Mario side-scroll jitter (needs a stock comparison), install-from-zip, 6.22's COMMAND.COM.
- **Follow-up (user):** after `/P` the prompt was `C>`. A `/P` shell builds a fresh environment
  and asks NTVDM for the rest via BOP 54 sub 0F; we answered "none". Implemented the two-call
  protocol (`1ab02af`, host `ab108d55`); the prompt is the full path again.

---

## Part 4 — review: all outstanding work moved to GitHub (2026-09-27)

User: *"Sort any remaining work so it all lives in GH and no outstanding work items are
persisted locally."* Everything above that reads as open/owed/next is now an issue.

- **17 closed** with evidence (done: #3 #4 #6 #13 #16 #23 #44 #45 #49 #50 #52 #133 #134 #135;
  obsolete: #15 #17 #129); **20 commented** with what remains; 4 retitled; 11 relabelled.
- **60 new issues** (#141–#200), one per surface for small items; **#201** host-UI epic;
  **#202** the agreed work order, pinned.
- Local lists removed: `STATE.md` (open defects, parked, owed, candidates, the "Next actions"
  log → archived verbatim to `log/state-archive-2026-09-27.md`); 11 inventory "what to fix"
  sections (moved verbatim into their issues as comments); `scripts/gh-bootstrap-issues.sh`
  (retired); the memory index's open lists.
- New anchor zip `dist\ntvdmex-20260927-a286862.zip` (host `0e6f5156` = `prev`).
- Artefacts (local, `runs/` is gitignored): `runs/s81_review/` -- triage.md, harvest.md and the scripts that made every change.

---

## Part 5 — Tier 1 quick wins and the fullscreen blur (`a120883`, `91c83bf`)

- Menus: Save/Load State removed (#145); unimplemented Machine/Debug/Help items removed, not
  greyed (#148); View cleanup (#156); Renderer = GDI/DirectDraw and actually switching (#147);
  Show Host Cursor back as a View toggle, greyed while the guest owns the mouse (#157); release
  hint in fullscreen (#138); Aspect as a 4-way dropdown (#159); Ctrl+Tab in Settings (#137).
- Also closed: #186 (VGA parity 690/690), #195 (`/uninstall` lockout + `/force`), #182, #169, #198.
- ⛔ `install_test` printed `ALL PASS: n/n`, a dialect the off-VM runner never parsed: its 30
  checks counted as zero for as long as it existed. Battery now 1,708 checks.
- **Blur:** the user's saved Renderer ("DirectDraw", picked while it did nothing) went live with
  #147 and switched fullscreen to driver-smoothed exclusive mode. The setting moved to a new name
  so stale values are ignored. User: **LGTM** on host `0e5b10e7`, now `debug\prev\ntvdmhost_prev.exe`.

## Part 6 — File > Close Program (#152), and a watchdog that killed the shell

- **Design.** The menu raises `g_close_req`; the exec thread takes it at the top of the V86 loop
  or the PM loop (the IRQ-delivery boundary) and ends the INNERMOST program through the same
  child-terminate path a real `AH=4Ch` takes (`dos_terminate`; for a DPMI client, the s80
  "child with a parent" path). Greyed at a shell's own prompt, in a Win16 VDM, and once the run
  is over. At depth 0 with no shell it ends the run, like the program's own exit.
- **What a killed program leaves broken.** It never unhooks: INT 08h/09h would point into the
  block `dos_terminate` frees and the shell dies on the next tick or key. EXEC now snapshots the
  IVT, PIC masks, BDA video mode and PIT channel-0 period (`g_exec_mach`); a FORCED close
  restores them, clears PIC in-service, resets SB/OPL/GUS/MPU/speaker (an auto-init block would
  play on) and the INT 33h driver (its event handler is in the freed block). A program's own
  exit is untouched — DOS does none of this.
- **Rig, unattended** (`scripts/bm/closeprog.bat <sky|doom|skyxp|doomxp>`: key script starts the
  game, `rigshot cmd 3` posts Close Program, then `ver` is typed): Skyroads under 6.22's shell and
  Doom under XP's both come back to a text-mode prompt that answers `ver`. Logs + shots in
  `runs/s81_closeprog/`.
- **Found on the way — the free-what-it-owned sweep was capped at 64 passes.** Skyroads owns more;
  closing it freed exactly 0x40 blocks and stopped. Now 4096.
- ⛔ **Found on the way — quitting a DPMI program to the prompt got the host killed 3 s later.**
  The DPMI watchdog stands down only on `g_dpmi_done` (the whole run over). A client that returns
  to its parent never sets it, the idle prompt never bumps `g_dpmi_iter`, and V86 code never
  counts as "moving" — so `STAGE3-DPMI: watchdog terminating (wedged)`. Each watchdog now carries
  a generation (`g_dpmi_wd_gen`) bumped when its client returns to a parent. Verified: the doomxp
  run logs `watchdog stand-down (client returned to its parent)` and the host is alive at the end.
- **Not ours, filed separately:** DOS/4GW started from **6.22's** `COMMAND.COM` in this harness
  #GPs in its own start-up (null ES at `01a7:2efe`) and exits FFh — identical on the previous
  build `0e5b10e7`, so pre-existing.
- Guards: Skyroads `n8=0 max_ms=6/7` (×3; one run showed a single 797 ms IFV starve stretch, two
  re-runs 0); Win16 Notepad launches and closes; off-VM battery 1,708/0.

## Part 7 — every setting logs its value and its source (#144)

- `STAGE2: settings -- value, source, and whether the host uses it` follows the preamble in
  every log: one row per setting with its value (combo text too), `[default]` / `[registry]` /
  `[registry value OUT OF RANGE -> default]`, `OVERRIDDEN by <file> -> <value>` where a share
  file or the NTVDM-aware shell took over, and `(stored only -- not used, GH #136)` for the
  rows nothing reads.
- Overrides noted where they are read: `dosver.txt`, the XP shell's forced 5.00, `pitpace.txt`,
  `uitick.txt`, `msens.txt`, `cpuspd.txt`, `nogus.flag`, `ddrawfs.flag`.
- ⚠ First cut appended the table next to the other STAGE0 lines and it vanished: startup lines
  collect in an 8 KB buffer that a later `log_write` TRUNCATES the file with. It is now printed
  right after that last truncating write.
- Rig (`runs/s81_settings/`): with `cfg\dosver.txt`=5.0 the DOS rows read `6/22 [registry]
  OVERRIDDEN by cfg\dosver.txt -> 5/0`. The rig's stored KeyboardLayout is **United Kingdom**,
  a row the host never reads.
