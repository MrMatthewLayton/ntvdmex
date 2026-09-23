# Session 75 — Doom's low detail is genuinely broken; `detaillevel 1` cost a day; the zip in the field

> Session 75. Split out of `docs/STATE.md` on 2026-09-23.

> Verbatim from `docs/STATE.md`. Nothing has been edited or corrected,
> including conclusions a later session refuted — same rule as the rest of
> this archive. See [`README.md`](README.md).

### ⛔ s75 (22nd, late) — **AND THE OTHER HALF: DOOM'S LOW DETAIL IS GENUINELY BROKEN.**

**User, after testing both via the in-game menu: *"High detail works. Low detail is
broken!"*** The `detaillevel` finding below explained WHICH picture was on screen; it
did not explain that one of those pictures is rendered wrongly **by us**.

**Measured** (odd-column-boundary changes, 320×200 guest framebuffer):

| frame | 3D view | **status bar** |
|---|---|---|
| HIGH 01 / 02 | 8030 / 12088 | **3667 / 3613** |
| LOW 01 / 02 | 352 / 16059 | **1263 / 941** |

▶ **Doom draws the status bar at FULL resolution whatever the detail level**, so that
column must match in both. It loses ~70% of its detail — we are degrading pixels the
game never asked us to degrade. The 3D view is inconsistent too (352 = properly
doubled on one frame, 16059 = noisier than HIGH on the next): corruption, not doubling.

**THE CAUSE, MEASURED** — `STAGE2: modeY mapmask hist` on the low-detail run:

```
0x01 x 16964   0x02 x 16884   0x04 x 16829   0x08 x 16821    <- one plane
0x03 x 143490  0x0c x 143514                                 <- TWO PLANES AT ONCE
```

The two-plane masks dominate by ~8.5×. That is Doom's low-detail column drawer
writing one byte into a PAIR of planes so every pixel is double-width. **Our mode-Y
support cannot serve it:** `ymap_select()` maps the A0000 window to a SINGLE plane and
`modey_flush()` falls back to a snapshot that `vdd_video.h` itself calls *"approximate
for a program that interleaves planes mid-scan"* — which is exactly this case.

▶ **This is the concrete instance of what `docs/inventory/vga.md` predicted**: we
approximate mode Y instead of modelling the address generator, so an idiom nobody
implemented against renders wrong. **Mario's open mode-Y artefacts are a candidate for
the same cause.** The fix belongs in VGA steps 3–4 (derive addressing from
CR17/CR14/GR5/GR6/SR4), not another special case.

⚠ **Not yet measured:** the HIGH-detail contrast histogram (that run printed no
`modeY` block — it is gated on `g_yremap` and the run ended without the video
summary). Expectation is single-plane masks only; **verify, do not assume.**

### ⛔⛔⛔ s75 (22nd, evening) — **"DOOM IS BROKEN" WAS `detaillevel 1`, AGAIN. TWO INSTALLS HID IT FOR A WHOLE DAY.**

**Resolved by one change:** `detaillevel 1 -> 0` in
`demo\msdos\doom\default.cfg` on the SHARE copy. User: *"What is running on the
screen RIGHT NOW is perfect!"* **No NTVDMEX defect was involved.**

Low detail draws every column double-width and makes the status-bar labels
illegible — exactly the reported *"raycaster columns too wide and flickery, and the
status bar is broken"*. Measured, same binary, one variable:

| share `default.cfg` | odd-column-boundary changes (3D view) |
|---|---|
| `detaillevel 1` | **0** — every column pair identical |
| `detaillevel 0` | **8030**, **12088** (s74's good figure: 8279) |

**⛔ WHY IT WAS UNFINDABLE — THREE TRAPS:**

**1. TWO COMPLETE NTVDMEX INSTALLATIONS, DIFFERENT GAME CONFIGS.**
`C:\…\All Users\Documents\ntvdmex\` (the SMB share — **`detaillevel 1`**, and what
every `live doom` launched) and `C:\…\Matthew\Desktop\ntvdmex\` (the user's own, with
their savegame — **`detaillevel 0`**). The user said *"I have Doom playing on high
detail"* — true of theirs. I said the config was low detail — true of the one on
screen. **Both correct, describing different installations, for hours.**
▶ `dir /s /b C:\ntvdmhost.exe` finds every install. **Confirm the copy you launch is
the copy they are looking at.**

**2. `runs/s74_doom_regression/good/` WAS MISLABELLED** — it holds low-detail
captures (0/267/0 vs 8030+ for real high detail). Every comparison said "matches the
good reference", which was true and worthless. Renamed to
`MISLABELLED_was_good_actually_lowdetail/` with a `READ_THIS_FIRST.txt`.

**3. A GAME CONFIG SURVIVES EVERYTHING** — binary swaps, two reverts, a machine
reboot, and a full wipe-and-rebuild from the 17th tag, because none of those touch
`default.cfg`. Doom rewrites it on exit, so an in-game F5 persists forever.

**▶ THE RULE: `findstr /I "detaillevel screenblocks" default.cfg` BEFORE touching
code.** s74 resolved the identical symptom the same way; this is the third time.

**Collateral from the hunt, all recorded:** today's VGA commits `db4c059` (mode-set
register file + CR11 write-protect) and `ec7c3e9` (integer scaling) were reverted
while chasing this and are **not** implicated in anything.

⚠ **REASSESSED 23rd — `db4c059` IS REVERTED ON AN UNPROVEN REGRESSION AND MUST BE
RE-TESTED.** The by-hand report that condemned it was *"columns too wide, flickery,
status bar"* — and two of those three are the **low-detail signature**: doubled
pixels *are* columns too wide, and the status bar losing detail is the measured
low-detail collapse (3613 → 941). That install was at `detaillevel 1`. The note
written at the time — *"the code says it cannot have; read-back is its only
guest-visible effect, so an assumption is still wrong"* — has a simpler reading:
the wrong assumption was not in the code, it was that the baseline was controlled.
User, 23rd: *"I have never tested Doom in low-res mode, so the fault may have been
there from the beginning … the work you did yesterday may have been correct."*
**Action: un-revert on a branch and re-A/B with `detaillevel 0` pinned and the
odd-column measurement.** "Flickery" is the one symptom detail level does not
explain, so this is a re-test, not an assumption in the other direction. Running stock ntvdm on a
graphics target to get a reference **crashed the rig's `nv4_disp` driver** and forced
a reboot. `/uninstall` REFUSES when the IFEO value points at a third binary, which
locked the user out of uninstalling — the displaced-value restore had pointed the key
at a throwaway `dist\` package. Both are real defects worth fixing.

**Rig now:** ONE host on the machine — `6e28e178`, rebuilt from tag
`release-20260917b` after deleting every `ntvdmhost.exe` on C: — installed and
verified; share Doom at `detaillevel 0`; `cfg\` as it was on the 17th.

### ★★★★ s75 (22nd) — **THE ZIP IN THE FIELD: TWO MORE MACHINES. ONE INSTALL DEFECT FOUND AND CLOSED; ONE FIRST-SESSION FAILURE UNEXPLAINED; DUKE3D TOOK NO KEYBOARD ON ONE BOX.**

> **Rig `bin\ntvdmhost.exe` = `4fc852aa` (this session). Stable zip UNCHANGED at
> `4847355` / `9448cf27` (`debug\prev\ntvdmhost_prev.exe`). Guards on `4fc852aa`:
> selftest PASS, Notepad launches and closes via X, Skyroads `n8=0 max_ms=6`.
> Log rotation seen working on the rig (`ntvdmhost-1.log` appeared on the second
> direct launch). ⚠ `rt.bat` deletes `ntvdmhost.log` before every run, so the
> harness never rotates — only a direct launch does.**

**The report (user, 22nd; runs on the 18th).** The `4847355` zip installed on a
friend's XP SP3 box (built for Win98: BIOS strips hardware, PS/2 keyboard + mouse,
Quadro FX 3450 → passive HDMI → 2560×1440) and on the user's own second XP box
(older Win98-era hardware, triple-boot). **The user's box: everything fine, first
run.** The friend's box, **first session, straight after install.bat**: Doom, Duke3D,
Heretic, Hexen all crawled — the DOS/4GW banner printed like a typewriter, never past
the PM text; Skyroads playable with slight lag; **Notepad/Paint/WinMine/Solitaire
drew under STOCK ntvdm**. **After a reboot** (during which the BIOS was also changed:
hardware removed, optimised defaults): Doom/Heretic/Hexen fine, Skyroads fine bar
minor keyboard lag, Win16 under NTVDMEX — and **Duke3D ran but took no keyboard
input** (it does on the rig and on both of the user's boxes).

**1. Win16 under stock — explained and closed.** The tester had run the Win16 apps
under stock *first*, to compare. XP keeps ONE shared WOW VDM resident after a Win16
program exits, and the IFEO `Debugger` value is only consulted when a NEW `ntvdm.exe`
is created — so a resident stock VDM keeps every Win16 launch until it dies or the box
reboots, and `/install` never said so. Now it does: `install_resident_vdms()` counts
`ntvdm.exe` in the process list (ours is `ntvdmhost.exe`, never mistaken for one) and
both `/install` and `/status` print *"N copies of Windows' own ntvdm.exe are still
running … close every MS-DOS and 16-bit Windows program (or reboot)"*. README step 3
says the same. `/status` on the rig with nothing running prints nothing extra.

**2. The slow first session — unexplained, and not separable.** Not `[0x714]` (written
since `d6f8014`), not the 3-strikes key (a reboot does not re-add it, and install was
not re-run). The reboot and the BIOS change happened together, the machine is
unreachable, and the only log it keeps is the last run's. So: **the host now rotates
`ntvdmhost.log` → `ntvdmhost-1.log` … `-5.log` on the first truncate of each process**
(`log_rotate_once`, `src/host/log.h`) — a field box holds its last six runs for
someone to copy back by hand. That is the whole of what can be done about it from
here.

**4. ★★★★★ THE DIRECTION CHANGED (user, 22nd): BUILD FROM THE SPECS, THEN TEST THE
APPS.** *"Gaps implemented by what we're building, not what's asking for them."* The
example given: Doom, Wolf3D, Mario and Skyroads all use mode 13h differently
(chain-4 off + CRTC page flips, map-mask layouts, plain linear) and real hardware
copes because it IS the registers; we built a 13h renderer and then a fix per game.
The inventory (every surface, its spec, every function/register, coverage measured
from the code) is the next deliverable, with the VGA register model at its head —
PCem with a real ET4000 ROM is the oracle. Recorded in memory as a standing rule.

**5. Windows 2000 loads (`ffebdab`, zip `dist\ntvdmex-20260922-ffebdab.zip`, host
`e1f4b4ea`).** The host refused to start there: `AddVectoredExceptionHandler` missing.
The import table (417 entries) has exactly four XP-only imports (VEH, `AttachConsole`,
`RegisterRawInputDevices`, `GetRawInputData`); all four are now bound at run time with
fallbacks (the unhandled filter runs the VEH's arms; no SEH frames exist in this
CRT-less host). `STAGE0: os=` logs what bound. **Loads-on-2000 only**: the
`NtVdmControl`/`VDM_TIB`/`VdmInitialize` contract is XP's and unmeasured on 2000; the
Win16 half is pinned to XP's `krnl386`. **The user tests by hand on the triple-boot
box and reports; no 2000 rig.** Windows 7 (32-bit only — x64 has no NTVDM) added to
the list; no machine for it yet. XP unchanged: selftest PASS, Doom to `ST_Init`,
Notepad, on `e1f4b4ea`. Stable zip untouched.

**6. ⛔⛔⛔ `install.bat` WAS ANNOUNCING A SUCCESS IT HAD NOT PERFORMED (`6449645`).**
The user's 2000 report — *"Installed, apparently (this failed before). But smoke does
not run, and no logs are produced"* — is reproduced exactly by a defect in our own
ordering, found while building a diagnostic for it. The single-instance guard
(`main.c`, "ONE HOST AT A TIME") returns **0 — success, silently, no output** — when
another host owns the mutex, and **the install-verb block sat BELOW it**. So
`/install` or `/status` issued while any guest was on screen printed nothing, wrote
nothing to the registry and exited 0; `install.bat` branches on the exit code alone
and announced *"Installed. Every MS-DOS and 16-bit Windows program now runs under
NTVDMEX"*. `smoke.bat`'s `/status` gate passed the same way and then failed with *"no
log was written"*. **A verb is a command-line utility invocation, not a VDM launch,
and must not be subject to a guard about how many VDMs are running.** Verbs now run
first; `install.bat` re-asks `/status` instead of trusting the exit code. ⚠ This is a
MECHANISM that fits the report, not a confirmed diagnosis of the 2000 box — it
requires a live/zombie `ntvdmhost.exe` at install time, which is plausible there
(a first launch that wedged) and unproven.

**7. `diag.bat` — one pass that names which failure you have.** Ships in the zip.
Separates *Windows never launched us* (section 6 counts the resident stock
`ntvdm.exe` — the check added in `de98e60`) from *we died before logging* from
*`debug\out\` is not writable*, using nothing absent on Windows 2000 (no `reg.exe`,
no `tasklist.exe`, neither of which ships there). Validated end-to-end on the rig by
installing the package host and running it exactly as a user would — **and its
section 6 is what proves the verb fix: before it, `/status` beside a live guest
printed nothing at all.** ⚠ Rig lesson: `start` on a missing path raises a MODAL box
that blocks the batch forever (it blocked this very run; `rigshot shot` found it on
screen in seconds — look at the screen before theorising).

**3. Duke3D, no keyboard, one machine.** Keys go WM_KEYDOWN → scancode FIFO → IRQ1
(`main.c:9977`); the low-level hook is off by default; PS/2 and USB are identical at
that layer. Both keyboard symptoms on that box (Skyroads lag, Duke3D dead) sit on the
IRQ1 path, whose only machine-specific inputs are CPU/BIOS — which changed. **Nothing
to act on without its log**; the ask is Duke3D again, ten seconds of keys, quit, send
`debug\out\`.
