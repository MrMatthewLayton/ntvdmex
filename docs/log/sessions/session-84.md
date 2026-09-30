# Session 84 — an unattended night: Tier 1 and Tier 2

**Date:** 2026-09-28 23:50 → 2026-09-29 · **Branch:** `m9/completeness` (every commit pushed)
**Mode:** unattended overnight. Decisions asked before the user left (recorded on the issues):
#207 skipped; #219 always pause, no setting; #217 both new settings default on; stock-NTVDM
runs allowed unattended (IFEO bracket); #167 one "behave like" setting; #165 6901h
session-only; #221 closed, remainder filed as #222.
**Host build:** rig `bin\` = **`af53da5c`** (everything below). Started the night at `6b833b1f`. `debug\prev\ntvdmhost_prev.exe` = `0e5b10e7`,
untouched. **Nothing tonight is user-confirmed.** Morning checklist: `checks.txt` on the share.

---

## Tier 1 (checks and decisions)

| # | Result | Evidence |
|---|---|---|
| #221 | Closed on the user's verdict; the 70 Hz-vs-60 Hz and mid-frame-palette gap is **#222** | — |
| #142 | Stock NTVDM answers `p_int53f` **identically redirected and not** (5305 → AL=01 both ways). Redirection is not the context that flips AL=5; the caller (shell vs child) is what's left. The shell branch stays provisional. | `runs/s84/stock/` |
| #143 | GetWinFlags stock re-run AGREE (`4C29`); **Doom twice** from XP's shell passes; Doom over a **real-mode TSR** (`t_tsr1c.com`, INT 1Ch hook) passes | `runs/s84/chain/`, `chain.bat twice xp` / `tsr xp` |
| #58 | **Melt is pixel-exact.** One captured melt frame: 61 moving title columns, all exact against TITLEPIC, none defect-shaped (`wipecheck.py`) | `runs/s84/wipe2/shot_doom_shot08.bmp` |
| #168 | Forced 5.0 via `cfg\dosver.txt`: AH=30h/3306h report 5, graded against the log's configured value | — |
| #216 | Surveyed: **Zoom In works** (needs the click on the area); Zoom Out and Cursor Position step over USER `0x16C` LookupIconIdFromDirectoryEx and `0x217` NotifyWow (non-accelerator kind) — cursor/icon loading is the next build | `runs/s84/pbcmd/` |

## Tier 2 (fixes), each gated on the rig before push

| # | Commit | What |
|---|---|---|
| #212 | `74b10a4` | **Programs start with IF=1.** EXEC handed the child the parent's flags from inside our INT 21h stub, where IF is clear. `p_ifst` AGREEs with 6.22, DOSBox-X and PCem. |
| #165 | `9c391ee` | 4B05h (AX=0 CF=0, PSP unchanged), 6901h session-only, and **4B01h makes the child the current PSP** (all three oracles). New probe `p_4b05`. |
| #166 | `a2fed21` | The rest of DOS's register-only INT 21h functions reach DOS from PM (not the keyboard reads, which re-enter). |
| #214 | `bf6767d` | Every child exit silences XP's MIDI synth (sustain off, all sound/notes off, reset controllers, `midiOutReset`) and keys off the OPL. |
| #215 | `3bb1c46` | **ASCII accelerators match WM_CHAR** — Calc's Ctrl+C/V, and Paint's and Write's cut/copy/paste/undo. |
| #187 | `82397dc` | Input Status 0 bit 7 (vertical-retrace latch) to the IBM spec; no IRQ 2. |
| #217 | `551c6d9` | Windowed picture composed off-screen and blitted once; Settings > Display gains *Draw off-screen first* and *Show on-screen messages* (both on). |
| #219 | `7b30c20` | **Only the focused window runs.** CPU suspended in guest code (the injectors' handshake), paused time dropped (no tick burst), audio devices frozen, MIDI/speaker silenced, DPMI watchdog held. Not Win16, not headless. |
| #188 | `9a91149` | PCem answered what it was waited on for: 0040:0065/0066 CGA table (modes 00h–07h only); page sizes and AH=05h verified; modes 00h/01h/02h/05h measured and in the register table (mode 01h cursor `0D0E`); INT 16h AH=00h/01h discard enhanced-only keys (IBM K1S), AH=09h = `B1h`, AH=0Ah = `41ABh`. |
| #167 | `430adee` | Settings > General **"Where MS-DOS and NTVDM differ, behave like:"**; XMS 08h's BH (`AAh` under 6.22) is its first row. Default NTVDM already matched stock. |
| #151 | `4272124` | Quick start and keyboard shortcuts, as a wiki page. |
| #200 | `fbdc456` | Inventories: `sb.md` (112 units, 34 PART), `mpu401.md`, `gameport.md` (written by agents, citations spot-checked); the surface table caught up. Findings listed on #200 — the SB's unknown-command argument bytes, MPU-401 dropping SysEx, INT 15h/84h reporting unwired buttons as pressed. |
| #216 | `96f375b` | Tooling: `rigshot wcmd "<caption>" <id>` + `pbcmd.bat` (one Win16 menu command per launch, with its log). PBrush's menu: 701/702 = Zoom In/Out, and Ctrl+N/Ctrl+O are ASCII accelerators (so #215 made them reachable by keyboard). |
| #58 | `2e1d3f1` | `tools/doomoracle/wipecheck.py` judges a melt frame against TITLEPIC shifted per column; self-tested PASS/FAIL/NOT-A-MELT on synthetic frames. `capture.flag` takes `period delay`. |

## ⚠ Harness traps paid for tonight

- **Two dosdiff runs at once cross-contaminate PCem.** A foreground run while the background
  probe run was using PCem got *another probe's output* back (p_vgareg got p_xms's dump, then
  a `pcem-vesa` capture under `pcem`'s name) and cached it. Caught by the `#PROBE` name and by
  mode 7 entering mono (which `pcem` never does). ⇒ **One dosdiff at a time.** Every cache file
  from tonight was checked for its `#PROBE` name; the bad ones were deleted and re-run alone.
- **XP's COMMAND.COM as `target.txt` just runs and exits.** It is only a shell on a bare launch;
  `chain.bat … xp` copies the stub as `ntvdmex.com` and `start /wait`s that.
- **MOUSE.EXE is not a TSR test on this rig** (no mouse found → AH=4Ch).

## Found on the way

- **XMS 0Bh could reach host memory** (`24111d6`): `offset + len > size` wrapped at 32 bits and
  a conventional endpoint had no ceiling. Found by the XMS/EMS inventory; fixed and tested
  (`xms_test` +3), in the final gated build.
- `vesa.md` and `xms-ems.md` inventories (`193f99d`, `1016232`); their findings are on #200.

## Measurements (`runs/s84/gate/`, `gate.sh` = s83's ab.sh)

| Run | Skyroads n8 | Doom sounding / IRQ5 | ZAR sounding / IRQ5 | Win16 shelf |
|---|---|---|---|---|
| A `6b833b1f` | 0x21 | 50/59 · 5159 | 63/68 · 688 | — |
| B212 | 0x22 | 50/59 · 5147 | 64/68 · 693 | = A |
| A2 | 0x1d | 50/59 · 5154 | 66/70 · 711 | — |
| C `16915315` | 0x27 | 50/60 · 5161 | 66/70 · 709 | = A2 |
| A3 | 0x28 | 50/59 · 5123 | 66/70 · 712 | — |
| **E `af53da5c`** | **0x16** | **50/59 · 5157** | **66/70 · 710** | **= A3** |

Skyroads' n8 wanders 0x16–0x28 on the SAME approved build across the night: it is not a signal
at this hour (s83's note stands; `winvnc.exe` was running all night, no client connected).
**#219 live:** Skyroads windowed on the rig, focus to the watcher window and back —
`PAUSE: resumed after 7500 ms (pauses=1 parked-in-loop=367)`; it landed through the
cooperative park (Skyroads traps constantly), IRQ0 delivery resumed, the game kept drawing.

## Next

The morning checklist, then #202's order: #183 (cheaper waits first), #172. #216's follow-up
is USER cursor/icon loading (`0x16C`, `0x217` for non-accelerator kinds).

---

## Day, 2026-09-29: the emulation spec (`docs/EMULATION.md`, epic #236)

The user's morning checks (`runs/s84/morning/sweep.txt`): #219, #214, #215 and #188 confirmed and closed,
#217 confirmed, #167 parked by the user. New: DirectDraw fullscreen crashed on VESA → **#223**, a
fixed 640x480 staging surface overrun by 1024x768/800x600 frames (fixed); then the user asked for
sharp pixels on DirectDraw too, since smoothing should come only from scaler/filter. A hand-written
per-pixel stretch into VRAM was **unplayably slow** (rolled back at once); `StretchDIBits` into the
back buffer's DC (the GDI routine) is the fix.

Then the user's spec, *"review today's capabilities against the spec, and fill in the gaps. Get all
of it done."* Filed as #224–#235 under epic #236.

| # | Commit | What |
|---|---|---|
| #224 | `9359228` | The spec's CPU ladder; rungs above this PC's `~MHz` greyed (owner-drawn Settings list) |
| #225 | `8a01b11` | **Rungs calibrated on Doom 1.9s `-timedemo demo3`** against thandor.net / TU Wien: 386DX33 8.2 fps (real 7.47), 486DX2-66 34.2 (33.1), DX4-100 ~44, P133 ~80. Fps is linear in the share; E = 8.67 s |
| #229 | `bccc677` | Colour filters (palette recoloured: free in 8-bit modes) |
| #228 | `27fa29d` | Aspect Auto (VGA 4:3, VESA square), window follows the mode |
| #230 | `2638b0e` | VSync renamed Force VSync |
| #227 | `2df3519` | Scanlines/CRT on DirectDraw |
| #234 | `42a9029` | WinMM / DirectSound output |
| #231 | `1e3cf18` | SB Pro / 16 / AWE32 as distinct cards (DSP version, commands, BLASTER) |
| #235 | `68a211e` | GUS / MPU-401 resource settings |
| #232 | merge + `588409a` | Real OPL3 (worktree agent), default now OPL3 |
| #226 | merge + `0e7ec0d` | VBE gaps (worktree agent): 8-bit DAC on the ports, 4F07h BL=80h waits, 4F03h flags, 4F02h mode state |
| #233 | merge + `e0d54b8` | AWE32 EMU8000 (worktree agent), no GM ROM |
| #183 | `def6eb2`, `edf2263` | 3DAh clock from the TSC (no syscall per read); retrace-wait loops sleep 1 ms |

⚠ **Lessons:** a file knob that is silently range-checked by an OLD build reads as "the throttle
does nothing" (index 7 of a six-rung ladder); a 10-minute headless cap hid the slow rungs; ask the
host which build is in `bin\` before believing a calibration run. **#222 needs the user's decision**
(options on the issue).

---

## Evening, 2026-09-29: the CPU speed limit, by focused rig rounds (#225)

The user's rule from round 8 on: **one focused test per round**, `checks.txt` = one test, `report.txt`
empty. Rounds 8-15 are in `runs/s84/round*/`; the rig's unattended runs are in `runs/s84/coop/`,
`runs/s84/rm/`, `runs/s84/calib/`.

| Round | Found | Fixed |
|---|---|---|
| 9 | "Speeds degrade over time"; 100 -> 66 MHz nearly locked up | A #219 pause was billed as execution; a speed change re-priced the old rung's execution. Both rebaseline the window |
| 10 | 486DX2-66 got 2x its CPU share yet felt < 33 MHz: 13 retraces/s against 36 | A retrace that passes unseen during a hold is reported once (`vbl_owe_on`, only while throttled) |
| 11-12 | Slow at "busy" moments: IRQ0 91..170/s, holds up to 178 ms | Cooperative catch: the exec thread parks at its next re-entry when the throttle cannot suspend it |
| 13 | Pure-compute seconds capped IRQ0 at ~120/s | One run+hold cycle passes one tick; the run is shortened until a cycle is <= half the guest's timer period |
| 14-15 | Still ~2x slow in-game at 486DX2-66, while Doom was right | Real-mode programs get their own share: Doom's x2 (by ear) |

`fae9e45` + `860877d`. **User-confirmed:** Skyroads, Gothica and Doom "have the performance characteristics
I would expect from a 66 MHz CPU".

⚠ **Lessons:**
- **A game's feel needs the numbers from the run the user actually played.** Closing the window skipped
  STAGE2, so `CLOSE2:` now writes the throttle, the per-second timer and the retrace counts.
- **Per-second records beat totals.** `STAGE2: CTL` put the slow seconds on pure compute, which no run
  total could have done.
- **Three plausible fixes measured wrong** (defer holds while a tick is pending; hold only with VIF on;
  inject from the throttle thread). The third was UNSAFE: without `g_lock` it raced the PIT's own
  delivery and injected 12,170 ticks of 5,934 raised.
- **This host is not a fixed multiple of a 486.** Doom's 32-bit code and Skyroads' 16-bit real-mode code
  differ by ~2x at the same share, with both clocks verified honest.
- 3DBench V1.0 (the measured replacement for the x2) times itself on a 1 kHz timer we deliver ~44% of,
  even at Unlimited: **#238**.
- The rig's SAVED speed is Pentium III 1 GHz (set by the user in round 8), so un-forced harness runs are
  throttled. Force with `cfg\cpuspd.txt` and delete it afterwards.

---

## Night, 2026-09-29/30 (unattended after the user's go-ahead)

| # | Commit | What | Verified |
|---|---|---|---|
| #237 #229 #228 | `f9e1138` | Renderer names "GDI"/"DirectDraw"; View > Colour Filter; Sepia as washed-out colour (hue kept at 40%, warm cast, faded blacks); new **Stretch** aspect (Auto in a window, fills fullscreen/maximised) | present_test +10; rig guard |
| #183 | `0298db5` | rt_idle recognises LOOPZ/LOOPNZ retrace waits; STAGE2 `host cpu ms` | Rig A/B ×2: Skyroads guest thread CPU **16.0 s vs 29.6 s** per 30 s; retraces/IRQ0/guard unchanged |
| #139 | `c97c6c3` (worktree agent) | Snare, hi-hat and cymbal synthesised, clean-room (Nuked as black box only) | 1.0000 per drum vs the oracle; battery 2,085/0; rig Skyroads sounds 30/30 s |

**Stopped on purpose:**
- **#238** (1 kHz IRQ0 ~44%): the attempts find 3DBench with interrupts off 78% of the time. The fix is deliver-at-STI, and every route is closed (VIP fatal, courier refuted, STI patching hazardous). Measured and recorded; the 3DBench calibration stays blocked.
- **#180** (keyboard ACKs): the code warns the last two attempts went wrong, and headless can't judge keyboard feel.

⚠ **#239, found by the gate:** ZAR's sound fails intermittently **after the rig's reboot**. This morning's approved `af53da5c` fails 3/3 now (6/6 this morning), so it isn't code. In a failing run ZAR's IRQ self-test never gets IRQ5 delivered (no `vec=0x0d` attempt at all), so it switches sound off. Suspect: load on the rig (a VNC client?). Not solved.

⚠ **Lessons:**
- **A gate regression must be re-run on the BASELINE too, interleaved.** One silent ZAR run on the new build looked like a regression; the baseline then failed as well.
- `gate.sh` doesn't force a speed, and the rig's saved speed is P3 1 GHz. `runs/s84/night/gate.sh` forces `cfg\cpuspd.txt`=0.

Morning test (`checks.txt`): the FM drums in Skyroads' music.

---

## Morning, 2026-09-30: the user's round (one focused test at a time)

| Result | Commit |
|---|---|
| **#139 FM drums**: user heard no difference in Skyroads ("has sounded correct for a while anyway"). Skyroads uses them lightly; the oracle match is the evidence. Left OPEN for the user to decide. | `c97c6c3` |
| **#218 smart mouse**: "MUCH improved, the UX is excellent". One bug: focus via the TITLE BAR captured, but Windows' move loop left the cursor unclipped, so the pointer walked onto the desktop. Fixed: re-clip at WM_EXITSIZEMOVE + a UI-tick clip guard (CLOSE2 `clip_repairs`). **User-confirmed: "Mouse capture is working perfectly now."** | `d569f09`, `782e3e5` |
| Window title "Windows NT Virtual DOS Machine", Settings "Windows NT Virtual DOS Machine Settings" (user ask). ⚠ The harness finds the window BY TITLE: `scripts/bm/*.bat`, the rig's `debug\rig\*.bat`, `rigshot.c` (rebuilt + deployed) and `runs/s84/*/gate.sh`/`zarab.sh` all follow it. | `8f13b8a` |
| Status strip to the user's layout: `name │ 16/32-bit Real/Protected Mode │ 66 MHz / 1 GHz / Unlimited │ Press WIN to release mouse / Click video to capture mouse`. Each part sized to its text, left-aligned; the 4th part only for a mouse-using program. | `782e3e5` |
| Strip bug: the name was always COMMAND.COM (set once, from the first program loaded). EXEC now saves the parent's name per level and dos_terminate restores it. **User-confirmed: "status strip is good now."** | `4769ed8` |

**Where it stands:** rig `bin\` = **`24901cd7`** (= HEAD `4769ed8`'s host). `checks.txt` = the status-strip
name test (DOOM.EXE → back to COMMAND.COM at the prompt → MEM/EDIT follow). `report.txt` empty.
**15 commits are NOT pushed** (since `c0e54bb`); push only when the user asks.

**Still owed to the user, one test at a time, after the name test:** the display items (#228 Stretch,
#229 View > Colour Filter + sepia, #237 labels); #223 (DirectDraw fullscreen on VESA: ZAR/Duke3D/VESACUBE);
#234 (what exactly is "buggy" about DirectSound); #227 (scalers blur in DirectDraw fullscreen).
**Open, investigated:** #239 ZAR sound intermittent since the rig reboot (not code; its IRQ5 self-test
never gets an interrupt delivered); #238 1 kHz IRQ0 (needs deliver-at-STI; every route closed).

---

## Late morning, 2026-09-30: #172 re-read, one instrument

**#172's premise is refuted by its own run.** s81 filed Doom's quit-wait IRQ0 collapse (0x85/s → 0x15–0x22/s)
as ticks never *raised*, from the run-wide raises:delivered ratio. The same log's gap classifier says
`IRQ0WHY gen=0 del=0x9e`: every long gap held ≥2 raised, undelivered ticks. In those gaps the async arm made 0x1d4
attempts and 0xf4 bailed `why=20` (CPU thread in host code, i.e. in a 3DAh trap), which is expected. The
cooperative per-pass latch in the PM loop runs after every PM port trap and should catch up in microseconds.
It doesn't, and IRQ0-in-service is ruled out (`irq0_isr blocks=0`).

New `STAGE2: PMCOOP owed>=2 gate[...]`: per PM pass with ≥2 ticks owed, the first gate that refused
(observe-only). Host **`2de5e9b6`** deployed; rollback `debug\prev\ntvdmhost_24901cd7.exe`.
`checks.txt` = Doom quit by hand (user). Not posted to #172 until that run names the gate.

**Found (unattended, `runs/s84/day/`).** The owed-count latch fix (`8540f7ef`) moved PMCOOP's refusals from
`latch` to `decl`, and the census showed every `decl` was DOS/4GW start-up, not the quit. The quit window measured
directly (`qwin.py`, Y → text mode): the user's runs had 57–69 timer entries/s, but my F10 quits had 139/s. The user
quits through the MENU, which leaves the map mask multi-plane. `I_WaitVBL`'s 3DAh traps then land in
`modey_pm_run`, which ran the poll loop to MYPM_CAP (400,000 instructions, ~27 ms) holding g_lock (the user's
worst `hold_us` was 57 ms at that line; q1 had 87 cap stops against 2 headless). Fix: stop the interpreter when an
interrupt is waiting AND the run has gone MYPM_IDLE=4096 instructions without touching the aperture (a drawer
is never cut short). Menu-route A/B ×2 (`dq.sh`, keys Esc/Up/Enter/Y): **timer in the quit 59/s → 139/s;
REPLAYED_LOUD 79/90 → 45/46** (the F10 baseline level, i.e. the #57 race). Detector A/B (`modeypm_detect.flag`):
native multi-plane store sites identical. Gate ×2: Skyroads/Doom/Win16 shelf unchanged; ZAR silent on BOTH builds
at random (#239); a 9-run ZAR series (stopped at 7) had silent runs on baseline and fix alike.
**User (a07f0e88): "better but not perfect"; Doom sound ~98% on WinMM, remaining pops/static (#57);
DirectSound still buggy (#234).** Not isolated: whether the latch change is needed (the tested build has both).
