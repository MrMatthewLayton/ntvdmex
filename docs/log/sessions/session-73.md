# Session 73 — The share is laid out for release; Win16 comes back; Hexen and Doom run

> Session 73. Split out of `docs/STATE.md` on 2026-09-23.

> Verbatim from `docs/STATE.md`. Nothing has been edited or corrected,
> including conclusions a later session refuted — same rule as the rest of
> this archive. See [`README.md`](README.md).

## ★★★ SESSION 73 (2026-09-16, morning) — **THE SHARE IS LAID OUT FOR RELEASE: `bin\ dist\ cfg\ debug\ demo\`. THE HOST MOVED TO `bin\`; THE RIG RUNS `cfdd7211`, UNCONFIRMED.**

**Two days to the first test release (the 18th).** The user's plan: copy the share to a
USB — binary, games and apps together — so the share had to be coherent to copy AND to
keep working in. It was `bm\ cfg\ out\ games\ demos\ dist\` plus root litter. Now:

```
\ntvdmex\                        (= the SMB share; = the USB)
      bin\ntvdmhost.exe            THE working host. Nothing else in here.
      dist\ntvdmex-<date>-<sha>.zip the installable package(s) -- zips only
      cfg\                          everything the host READS (unchanged: FLOPPY.IMG, target.txt, knobs)
      debug\rig\                    the harness: rt.bat runwatch.bat controld rigshot vdmwatch vdmdump
                                    dosstub.com + the LIVE .bat runners + qbkeys_*.txt
      debug\tests\                  Probe\ Argtest\ Testcard\ selftest\ dos\ (the old bm\tests)
      debug\out\                    everything the host WRITES (the old out\ + notes.txt)
      debug\prev\                   rollback builds: ntvdmhost_prev.exe = a5cd764b (USER-CONFIRMED)
      demo\msdos\                   games + demos FLAT: Doom Duke3D HERETIC Hexen Lemmings Mario Skyroads
                                    skyxmas Wolf3D Wolfy Zar Bubbles chasmdem dkd-egas fusion_f radiance h7 qb45
      demo\win16\                   Win 3.11 apps (EMPTY -- README only; nothing was on the share)
      debug\ctl\                    the control channel: cmd.txt watcher.txt control.txt controld.txt rigshot.txt
```
**The root is exactly `bin cfg debug demo dist`.** For the USB: copy everything but `debug\`.
(The control files were at the root until the user asked; `controld.exe` and
`rigshot.exe` have the path compiled in, so both were rebuilt -- `controld_v2.exe` is
hot-swapped by `runwatch.bat` at its next start -- and `bmqueue.sh`, `bmwow.sh`,
`lemhpab.sh`, `launchmatrix.sh`, `dosdiff.py` etc. write `debug/ctl/cmd.txt`.)

**What had to change for `bm\` → `bin\`, and it was not a rename:**
* **The host** derived its root from the exe's directory being literally `bm`
     (`ntvdmex_root()`). It now accepts `bin` **or** `bm` (an installed s72 zip keeps
     working) and writes to **`debug\out\`** instead of `out\` (`NTVDMEX_OUT`; `debug\`
     is created first — `CreateDirectoryA` makes one level). That is a **5th unseen host
     change** on top of s72's four; the user chose "rebuild + deploy".
* **`vdmwatch.c`** logged to a compiled-in `\out\` — repointed. controld/rigshot write
     to the root and are unchanged.
* **`rt.bat`**: `BIN RIG TESTS DEMO CFG OUT` variables; a target resolves to
     `demo\msdos\<Name>` first, `debug\tests\<Name>` second, so `bmqueue.sh Probe P_DRV.COM`
     and `bmqueue.sh Doom DOOM.EXE` are unchanged. `setup` points IFEO at `bin\`. `clean`
     `rd`s the empty s61 dirs. `runwatch.bat` runs `debug\rig\rt.bat`; its window title now
     carries `[debug\rig]` so an old and a new watcher can be told apart.
* **`package.sh` + `package\*.bat` + README**: `bin\ntvdmhost.exe`, `debug\out\ntvdmhost.log`.
* **Mac side**: `bmqueue.sh` (results in `debug/out/`), `paritysweep.sh`, `lemhpab.sh`,
     `wowtriage.sh`, `bmwow.sh`, `bmsxs.sh`, `bmstockdump.sh`, `doomstack.py`.
* ★ **NEW `scripts/bmstage.sh`** — lays `debug\rig\` out FROM THE REPO (CRLF, md5 both
     sides). **It does not touch the host unless `--host`**, and `--host` copies the old
     `bin\` exe to `debug\prev\ntvdmhost_prev.exe` first. `--check` diffs without writing.

**⚠ WHAT WAS DROPPED FROM THE SHARE, AND WHY.** `doomrun menushot setshot stageall
stockdump sxs zarargs zarcmp zarlong zarout zarplay gamedir` — every one is **pre-s61**:
they `md C:\ntvdmex`, copy the host there, and `reg add` the IFEO Debugger to
`C:\ntvdmex\ntvdmhost.exe`. Running any of them recreates the litter the user wiped the
box over, then points the interception at a binary that does not exist — *"no DOS app
runs"*. They stay in `scripts/bm/` for reading; `bmstage.sh` stages only the LIVE set.
`rt_stock.bat` (the stock oracle) was the one live-era script still copying to
`C:\test` — ported to run in place from `debug\tests\dos\`.

**The cutover, in the order that keeps the box alive:** (1) `restore.bat` via controld
→ IFEO = `bin\ntvdmhost.exe` **before** anything moved out of `bm\`; (2) `exec` the new
`runwatch.bat` → new Startup entry + new controld; (3) `reboot` → only the new watcher
comes back; (4) delete `bm\`. ⚠ **The queued `reboot` was consumed and never fired**
(both watchers kept beating; `rigshot list` showed both windows) — the old watcher was
killed by its exact window title through controld instead, `bm\` then deleted cleanly,
so the box has NOT been rebooted on the new layout yet. The new `runwatch.bat` did
install its Startup entry; the first reboot will tell. The same kill-by-title move
was used a second time for the `debug\ctl\` change (title now
`[debug\rig, ctl=debug\ctl]`); a queued selftest then ran 8/8 through the new channel.

**Verified on the rig, in this order:** `setup` → IFEO = `"…\bin\ntvdmhost.exe"` and
the layout listing · **selftest 8/8 through `bin\ntvdmhost.exe`** (`STAGE0: root` =
the share, program under `debug\tests\selftest\`, log in `debug\out\`) · the new
package **`dist/ntvdmex-20260916-970f9f1.zip`** by `pkgtest.bat` from a fresh folder:
`/status` (saw the rig's key as another program's) → `/install` → **8/8 through the
package's own `bin\`** → `/uninstall` restored the rig's key · **Skyroads headless from
`demo\msdos\`: `n8=0x65 max_ms=0x14`** (baseline `≈0x6c/0x15`), guards intact.
`dist\` on the share now holds ONLY that zip (the two unpacked test folders and the
`bm\`-layout `9ad5eff` zip are gone). `debug\out\` was 53 files incl. two vdmwatch
cores (12 MB) and an 86 MB `result_Wolfy.log` — left as-is, they are the user's call.

### ⛔⛔ s73, LATER: **WIN16 DOES NOT RUN ON THE RIG, AND HAS NOT SINCE THE s61 WIPE.**

Found while laying `demo\win16\` out (17 apps from `guest/win16/`, one folder each, via
**`scripts/w16demo.sh`**; `debug\rig\w16launch.bat <app> [EXE] [keep]` starts one through
the IFEO hook and lists the desktop; `w16watch.bat` watches the process second by second).
Notepad through the hook → nothing on screen. Three stacked causes, two fixed:

1. **The Win16 half is FOUR `cfg\` files and the wipe took all of them**: `wowtry.flag`
      (absent = every Win16 launch REFUSED with a "16-bit Windows not supported" box),
      `wowsched.txt`, `wowcall.txt` (existence-gated), `wowidle.txt`=`0` (absent = a task in
      GetMessage is quit after ~6 s). Filed as s38–s43 "experiments", never promoted, and
      **the shipped zip did not include them while its README promised Notepad and Paint.**
      FIXED: on the rig, and `package.sh` now ships all four.
2. **The log destroyed itself on the Win16 path** (`src/host/log.h`, an s71 regression):
      `log_append` cached the caller's path POINTER, which since s71 is one of sixteen ring
      slots — so a stale slot compared equal to a different file and the main log's lines
      went into `ldtprobe.log`. Separately, three callers pass a bad `[buf,end)` (one NULL
      buffer, two with a LENGTH where `end` belongs); `(DWORD)(end-buf)` wrapped, tripped
      the 256 MB cap on one call, and the file held **66 bytes: the cap marker and nothing
      else**. The confirmed `a5cd764b` did exactly the same (A/B from a temporary `bm\`), so
      this is NOT the s72/s73 host changes. FIXED: the cache keys on a copy; a bad range is
      reported in the file and dropped, and the run keeps logging. The three bad callers are
      still to be found — the log now prints their pointers.
3. ~~OPEN~~ **CLOSED at midday, see the next block.** krnl386 reached protected mode and the VDM died silently at `PMHB steps=0x85`
      — after `FUNC 0xc0` and `0xbe` were STEPPED OVER as unimplemented, a run of
      `INT31h AX=0002` (segment→selector for 0x40/0xF000/0xA000…0xE000), two `0703`
      paging no-ops and a `04F2` commit. `wow32{ok=5 decl=4 unimpl=2}`. Log
      `runs/`-worthy: `debug\out\result_w16watch*.log` + `ntvdmhost.log` from 10:29.
      ⚠ Also still true: **the WOW path takes its program from `cfg\target.txt`** (the CSRSS
      first fetch is FALSE on `-w`), so a double-click on a fresh machine has NO program
      name. `w16launch.bat` writes it, as `wowlive.bat` always did; the product gap stands.

**Rig host is now `4c502027`** (cfdd7211 + the log.h fix; archived as
`debug\prev\ntvdmhost_cfdd7211.exe`). `bmstage.sh --host` archives the displaced exe
BY HASH and never touches `ntvdmhost_prev.exe` — promotion to "confirmed" is by hand.

### ★★★★★ s73, MIDDAY: **WIN16 IS BACK — 15 OF 17 DEMOS PUT THEIR WINDOW UP, FROM A REAL LAUNCH, WITH NO `target.txt`.**

Item 3 above is closed, and it was never a WOW-layer defect. Three host fixes, in the
order the log revealed them (each one uncovered the next):

1. **`e595c91` (s68) killed Win16 and nobody noticed for five sessions.** "The report
      stops eating itself" turned every `report` flush into a `base` flush mechanically,
      including the one at the WOW selector stage (`main.c` ~21566) — 1,500 lines
      **before** `base = p` is executed. So `log_append(NULL, p)`, then `p = NULL`, then
      every STAGE1 line was `zput` from **address 0 = the guest's IVT and BDA** in a VDM
      process. krnl386 ran on a trashed interrupt table and died at `PMHB 0x85`. The
      flush is gone (nothing to flush: the probes log via `ldtprobe.log`). ⚠ The lesson
      is the standing one about a fix measured on one guest: that commit was verified on
      Lemmings, and Win16 was never launched again after the wipe.
2. **krnl386 sees 8.3 names only.** With the log alive the kernel put up "Cannot find
      file …\notepad\notepad.EXE (or one of its components)" for a file that was there:
      its loader opens through INT 21h. New `wow_shorten()` (`wow32.h`) runs
      `GetShortPathNameA` on every path handed to the Win16 side — the launch command and
      `ResolveModulePath`'s answer. The old `C:\WIN16\` never needed it.
3. **The Win16 program now comes from CSRSS, as stock does — the s50 gap is closed.**
      On `-w` the first fetch is FALSE/0x57, so the name came only from `cfg\target.txt`.
      Measured shapes (every one DONT_WAIT, all logged as `STAGE1: WOW command fetch [...]`):
      `WOW|FIRST` alone → `FALSE err=0x490` with either task id;
      **`GET_FIRST_COMMAND|WOW` → TRUE (junk AppName, real CurDir) = the handshake, THEN
      `WOW|FIRST_TASK` → TRUE with the AppName already in 8.3** — the same two-step the DOS
      path learned in s72. A name is believed only if drive-qualified or UNC (TRUE with
      capture-buffer junk is not a program — the first cut launched the junk). The image
      is read into `filebuf` like the target.txt path or the V86 stage builds for the
      embedded stub and krnl386 dies in its own heap init ("Unable to initialize heap").
      **Negative control passed:** `target.txt` naming Terminal, Paint launched → Paint.
      Then `target.txt` deleted from the box, whole shelf launched → 15/17.

| up (window title) | not up |
|---|---|
| Notepad, Paintbrush, Solitaire, Minesweeper, Character Map, Calculator, Write, Cardfile, Clock, System Configuration Editor, Task List, Recorder, Sound Recorder, Object Packager, Terminal (+ its port dialog) | **PROGMAN** (host alive, no window — was "frame up" in s55), **MPLAYER** (host gone — was "launches" in s53) |

`scripts/bm/w16launch.bat <app> [EXE] [keep]` is the launcher (kills the shared WOW
VDM first, re-asserts IFEO, lists the desktop, keeps the app up with `keep`).
Rig host **`28ec97b2`**; offvm 1361/0; selftest 8/8; Skyroads `n8=0x63 max_ms=0x14`.
Package NOT yet rebuilt with this — do that before the by-hand pass.

### ★★★★ s73, AFTERNOON: **THE SCREEN UPDATE IS RAISED BY THE GUEST'S FRAME ("Auto") — BOUNCEBX MEASURED 1:1, WHERE IT WAS CHOPPY.**

User: *"BOUNCEBX in stock NTVDM is virtually butter smooth. On NTVDMEX it's choppy. Why?"*
Measured (30 s headless, mode 12h): the guest drew **1798 frames at 59.9 Hz** and the
host presented **~1200 of them** — the present was a 15 ms `WM_TIMER` sampling a ~2 ms
phase window, so it missed a third of the frames at an irregular cadence. Stock has
ONE clock for the retrace and the repaint; we had two, beating.

**Now:** `video_state.present_hook` fires from the guest's own `0x3DA` poll, once per
frame — on the **first poll after a gap ≥ 400 µs** (the guest went away to draw and
is back to wait: frame complete, 14 ms to spare), else in the old window, else at the
edge — and `WM_APP_PRESENT` runs the frame body at once. The timer is a fallback only
(Auto floor = 90% of the mode's frame period; presents only if the hook has been quiet
two frames). Settings › Timing: **Screen update = Auto (recommended) / 5 / 10 / 15 /
20 ms** (combo, index-valued, registry `UiTickMode`; `cfg\uitick.txt` 0 = Auto).

Three things the numbers caught on the way, each a session-saver:
* **Firing in the window (2 ms before retrace) lost one frame in twenty** — the render
     under the lock blocked the guest's polls across the retrace it was waiting for
     (edges 1798 → 1697, dtmax 2.4 → 13.5 ms). Hence the gap trigger.
* **`WaitForVerticalBlank` is a BUSY LOOP on XP** and, at 60 presents/s, stole a whole
     core from the guest. Replaced by sleep-to-just-before-the-blank + a bounded look.
     ⚠ "Sleep(1) and look again" sailed past the 1.4 ms blank half the time (1002 presents
     for 1788 frames).
* Even the polite wait delays the NEXT frame's render to a random phase (−3%). So
     **"Wait for monitor VSync" now defaults OFF** (stock does not vsync its blit either)
     and is labelled "can drop frames". Result: `presents{hook=1800 timer=~20}` for 1801
     fires, guest 59.0–59.4 Hz, dtmax 3.1 ms.

**Skyroads guard:** `pacer_prio=0 joy_thread=0 pit_split=1`; IRQ0 **1 anomalous gap
in 5398 ticks (was 76 this morning)**; V86 stretches now `n8=0 max_ms=6` vs the
`≈0x6c/0x15` baseline — evenly interrupted rather than in long runs. Feel is the
user's call. Rig host **`e1a56ef1`**. offvm 1361/0.

### ★★★★★ s73, EVENING — **HEXEN AND DOOM RUN: DOS/4GW COPIES argv[0] INTO A 64-BYTE BUFFER.** (HEAD `23bd9ae`, rig host `877eb238`)

~~▶▶ NEXT JOB: GET HERETIC WORKING.~~ **Done in s74 — see the s74 block.** What was known is in the block
below. Doom, Hexen, Zar and Wolf3d are **USER-CONFIRMED WORKING** on this host.

**The find.** DOS/4GW re-opens `argv[0]` to load its protected-mode half and copies
that name into a **64-byte buffer with no bound**. Measured from this one share:

| game | argv[0] length | result |
|---|---|---|
| `doom\DOOM.EXE`       | 62 | loads and plays |
| `hexen\HEXEN.EXE`     | **64** | `fatal error (1007): can't find file ...\HEXEN.EXE<` — no room for the NUL, so it reads one byte of garbage |
| `heretic\HERETIC.EXE` | 68 | truncated at 64: `...\HERETICD` |

Copying `HEXEN.EXE` alone to a 61-char path fixed it outright — that pinned it.

⚠⚠ **"HEXEN WORKED BEFORE AND DOES NOT NOW" WAS NOT A CODE REGRESSION — IT WAS THE
FOLDER RENAME.** s73 moved the games from `games\Hexen\` (59 chars) to
`demo\msdos\hexen\` (64) and crossed the limit. A layout change broke a game, and
no code was involved. ▶ **Path length is a compatibility surface. Keep `demo\msdos\`
shallow, and never lengthen it without re-running a DOS/4GW guest.**

**The fix** (`23bd9ae`), both at the one place `argv[0]` is built:
* shorten to 8.3 (`GetShortPathNameA`). A by-hand launch already worked because CSRSS
     hands over the short name; only the `target.txt` path passed the long one — which is
     why this read as a *"headless-only DOS/4GW blocker"* for sessions. **It was never
     headless-only: it is PATH-specific**, and any user whose games sit under a path with
     spaces had the same broken launch.
* if it is STILL over 62 chars, hand over the **bare filename** — the guest's cwd is
     the program's own directory (it is how these games find their WAD), so the extender
     opens the same file and an 8.3 name can never approach 64.

★ **THE HEADLESS RIG CAN NOW RUN DOS/4GW GAMES.** That is the multiplier: Doom renders
in-game headless (HUD, 113 colours) and Hexen too (ettins, weapon, HUD; log 461 KB →
7.9 MB). A DOS/4GW guest no longer costs a by-hand test to judge.

### ✅ HERETIC — WHERE IT WAS AND WHAT WAS KNOWN AT s73 CLOSE (closed in s74: the VESA 4F00 overrun, hypothesis 3)

Heretic is **further than it has ever been**: past the loader, through `V_Init`,
`M_LoadDefaults`, `Z_Init` (`DPMI memory: 0x0, 0x800000 allocated for zone`) and
`W_Init: Init WADfiles.` — then dies on screen with:

```
I_AllocLow: DOS alloc of 1024 failed, 256 free
```

Which is our `INT31h AX=0100 BX=0x40` (64 paras) answered `ENOMEM max=0x10`.

**THE CAUSE IS A BROKEN MCB CHAIN, AND THE CHAIN DUMP PROVES IT.** A new diagnostic
(in `23bd9ae`, the `0x0100` ENOMEM arm) walks and prints the chain on any failed DOS
allocation. At Heretic's failure it reads:

```
0x0005f own=0x0100 sz=0x010     0x00070 own=0x0008 sz=0x08e
0x000ff own=0x0100 sz=0x1236    0x01336 own=0x0100 sz=0x200
0x01537 own=0x0100 sz=0x000     0x01538 own=0x0100 sz=0x040
0x01579 own=0x0100 sz=0x080     0x015fa own=0x0100 sz=0x006
0x01601 own=0x0100 sz=0x040     0x01642 own=0x0100 sz=0xfa0
0x025e3 FREE sz=0x010      <-- THE WALK STOPS HERE
```

**There is no terminating 'Z' block and nothing above `0x25e3`.** The MCB at
**`0x25f4`** has an invalid signature, so the chain walk stops and the allocator
cannot see the **~480 KB** between `0x25f4` and the CDS/SFT at `~0x9d58`. `max=0x10`
is exactly the 16-para hole Heretic had just freed. `dos_free` is behaving correctly:
it refuses to coalesce into a neighbour that is not a valid MCB.

**The sequence that gets there** (all `INT31h AX=0100/0101`):
1. `BX=0xfa0` (4000 paras) → seg `0x1643`  — splits, tail free MCB lands at `0x25e3`
2. `BX=0x010` (16 paras)   → seg `0x25e4`  — splits again, tail free MCB at **`0x25f4`**
3. `AX=0101` frees selector `0x347` (that 16-para block) — coalesce forward **fails**
4. `BX=0x040` (64 paras)   → **ENOMEM max=0x10**

So **`0x25f4` is corrupted between step 2 and step 3.** Doom does the same shape of
allocations (a 16000-para block, then 64 paras) and does **not** corrupt, so it is not
simply "our split is wrong for every case".

**Three hypotheses, none yet tested:**
1. **The guest overran its own 16-para block** (256 bytes) and smashed the MCB header
      at `0x25f4`. If so the bytes there are Heretic's data, and the question becomes why
      it writes past an allocation it asked for — possibly our **LDT limit for the DPMI
      DOS block is wrong** (we set `limit = (want<<4)-1`, so 0xFF here; a guest writing
      through a *different*, flatter selector would not be stopped).
2. **`dos_alloc`'s split wrote the tail MCB wrongly** for this size/position. Read the
      split arm in `src/dos/dos_mcb.h`; it looked correct on inspection but was not
      instrumented.
3. Something of **ours** wrote there (the usual suspect list — a patcher, a probe).

▶ **THE NEXT MEASUREMENT, AND IT IS CHEAP:** dump the **16 bytes at `0x25f4`** at the
moment of failure (is it guest data, zeros, or a mangled header?), and print the chain
**immediately after step 2 and again after step 3** to bracket exactly when it breaks.
That distinguishes all three hypotheses in one run. `./scripts/bmqueue.sh heretic
heretic.EXE` is the whole loop — no by-hand test needed.

⛔ **DO NOT "FIX" IT BY MAKING `dos_free` COALESCE ACROSS AN INVALID MCB.** That hides
a memory corruption behind a plausible-looking chain, which is the exact shape this
project keeps paying for.

### ★★★★ s73, 1–2 pm: **"QBASIC CANNOT BUILD EXEs" — CLOSED. Two host defects, one missing variable, and the launcher's environment now reaches every DOS guest.**

**Package `dist\ntvdmex-20260916-09e101f.zip` is on the share — the FIRST zip whose
Win16 half is proven from a fresh folder:** `pkgtest.bat` 8/8 through the package's own
`bin\`, then new **`pkgw16.bat`** (installs the package host, `w16launch.bat` with
`BIN`/`OUT` pointed at the package, rig's key restored) put **Notepad AND Paintbrush up
from the package folder** with `STAGE0: root` = the package. Earlier zips never shipped
the four WOW `cfg\` files. `dist\` holds only the zip. ⚠ It predates the env change below.

**The QB bug, reproduced the user's way** (`scripts/bm/qbmake.bat`: `cli host|stock`
= BC then LINK from a cmd line, redirected; `qb` = QB's own Run > Make EXE by key script
with the DOS trace on — the Run menu here is **Easy Menus**, four items, so Down×3).
The user's leftovers said it first: `CAVE.OBJ` compiled `/O` (default library BCOM45),
`CAVE.EXE` **3,772 bytes with 0 relocations** (no runtime = linked WITHOUT the
library), `~QBLNK.TMP` not cleaned up. Then, in order of discovery:
1. **A second DOS command in the same cmd window never ran.** `BC` then `LINK`: BC's
      host was handed LINK by CSRSS and relaunched it in a fresh host (s72's design) —
      which logged **`REFUSED: another ntvdmhost is LIVE (owns the single-instance
      mutex)`** and quit, because the relaunching host still owned the mutex while it
      waited on the child. cmd saw rc=0, nothing was written. **Fixed:** the mutex (and
      `host_panic_release()`'s system-wide things) are handed over before the relaunch.
2. **The guest got a FIXED four-variable environment** and the launcher's block CSRSS
      hands over (`envlen=0x746`) was discarded — so `set LIB=…` then `LINK` found no
      `BCOM45.LIB` under us while the same two lines under stock did. **Fixed, to stock's
      MEASURED rules** (new `tools/dostest/p_env.com` dumps PSP:2C as text; run in the
      same cmd window under both, `scripts/bm/envprobe.bat stock|host`): COMSPEC first;
      names upper-cased, values verbatim; `ALLUSERSPROFILE APPDATA COMMONPROGRAMFILES
      PROGRAMFILES USERPROFILE` and each `PATH` element to 8.3; `windir` dropped;
      **`TEMP`/`TMP` of ≥12 characters → `%windir%\TEMP`** (eleven data points: length is
      the only predictor — `C:\WINDOWS`, `C:\windows`, `C:\NOSUCH`, `C:\A\B` kept;
      `C:\ABCDEFGHI`, `C:\WINDOWS\system32`, the user's `LOCALS~1\Temp` replaced; rule
      not understood, recorded); BLASTER last. Ours now differs from stock's dump in
      exactly two DELIBERATE lines (`COMSPEC=C:\COMMAND.COM`; our card's BLASTER without
      `P330`). The block lives in its own **PSP-owned MCB at the top of memory** beside
      the CDS and SFT (one `dos_mcb_reserve_top`, carved three ways), PSP:2C and PSP+2
      point at it; the 256-byte `0x60` block stays as the fallback for a launch with no
      environment; `dosenv.txt` still appends. Cost: the env's size (0x67 paras here,
      stock 0x6E) off the top of the program's block.
3. **QB's Make EXE needs `LIB` in the environment** — on a real DOS box QB's SETUP
      wrote `SET LIB=C:\QB45\LIB` into AUTOEXEC.BAT. QB.INI's Set Paths (`C:\LIB` on the
      user's copy) is used by QB's own probe and is NOT handed to LINK. **Stock fails
      identically without it** (3,772-byte EXE). With `set LIB=<8.3 path>\qb45\LIB` in the
      launching batch, **QB builds a 46,846-byte stand-alone `CAVE.EXE` that runs**
      (mode 13h, clean exit). ▶ Tell the user: set `LIB` (system env var, 8.3 form) or
      launch QB from a batch that sets it.

**Then (`726da7a`): the relaunched child inherits the launcher's redirect.** The next
command's StdIn/Out/Err come back from the report call as handles CSRSS placed in the
parent; they are dup'd inheritable and — the part that mattered — set as the parent's
OWN std handles (`SetStdHandle` = the PEB fields the child's `stdio_from_parent` reads;
`STARTUPINFO` never reaches a BaseSrv-created child, measured). `LINK … > file` now
fills the file as stock does.
⚠ **Relaunch-shape caveat, unchanged:** the launcher is released by `ExitVDM` *before*
the relaunched command finishes (a batch's next line runs early — its own `dir` did not
see the EXE that appeared a second later). Stock's shape — stay resident, run the next
command in-process, report each exit code in turn — is the right one and is a WinMain
restructuring, not a day's work. Documented in `package/README.txt` KNOWN LIMITS.
* `package/README.txt`: the stale "no `> file` yet" line replaced by an ENVIRONMENT
     VARIABLES section (LIB for QB, 8.3 paths) and the Win16 list of 16.
* **`package/demo/qb45/QB45.BAT`** (also on the share): sets LIB/INCLUDE to the folder's
     own 8.3 paths and starts QB — the USB-shape fix for Make EXE. (`QB.BAT` would lose to
     `QB.EXE` in PATHEXT order.)
* **PROGMAN IS UP** (`runs/s73_qbmake/progman.png`): title bar, `File Options Window
     Help`, empty grey client — correct with no `.GRP`s. This morning's "no window" read
     XP's own desktop caption ("Program Manager") as the tally. **16 of 17.** MPLAYER is a
     real guest GPF at `0001:3983` right after `RegQueryValue` (SHELL) with a stepped-over
     USER call earlier in its log — MCI/MMSYSTEM territory, not chased.

**Verified on host `8733e095` (= HEAD `137f673`, THE RIG'S HOST NOW):** selftest 8/8 ·
Notepad up · Skyroads `V86STR n8=0 max_ms=7`, IRQ0 `anom_n=0`, guards intact · offvm
**1361/0** · parity: 12 probes clean before the sweep was cut short (it was ~4 min/probe
— see the SMB note below), then the memory-map subset `p_mcb p_ovl p_psp p_sysvar p_tsr
p_umb` **35/35 comparable rows agree** (`p_child`/`p_tsrc` are companions, always NO
ROWS) · `dosenv.txt` still appends · **package `dist\ntvdmex-20260916-137f673.zip`
verified from a fresh folder: 8/8 + Paintbrush up** — the only zip in `dist\`.
Evidence in `runs/s73_qbmake/`.

★ **THE SWEEP WAS SLOW FOR ONE REASON, AND IT WAS NOT QEMU.** The 6.22 oracle run is
**3 s**. The rest of each ~4 min was macOS's SMB attribute cache taking minutes to show
an mtime change on the existing `result_Probe.log` — the same lag that gave `bmqueue.sh`
a false TIMEOUT today. A file that APPEARS is seen in seconds, so `dosdiff.py` and
`bmqueue.sh` now delete the old result and wait for a fresh one (also retiring the
stale-result hazard). Six probes then took ~2 min in total. Oracle answers are cached
under `build/dosdiff-cache/` by the probe's sha1 (`DOSDIFF_NOCACHE=1` bypasses).

▶ **The by-hand pass owed from s72 is still owed, now for EIGHT changes** (SFT, HMA,
`AH=3Dh`, guard logging, the `bin\`/`debug\out\` paths, Auto screen update, **the
environment block, the relaunch/redirect handover**). Asks: Doom E1M1 kill, QBasic
Open → run, **`QB45.BAT` → Run > Make EXE File → run the EXE**, one memory-tight guest
(Duke3D matters doubly: the env block took ~1.6 KB). Roll back by copying
`debug\prev\ntvdmhost_prev.exe` (`a5cd764b`) over `bin\ntvdmhost.exe` — and note a
rolled-back host would write to `out\` again and expect `bm\`; **`a5cd764b` cannot run
from `bin\`** (it derives its root from a folder named `bm`). A rollback therefore
means `mkdir bm`, put it there, `reg add` IFEO to `bm\`. That asymmetry is the price of
the rename; `bmstage.sh --host` handles the forward direction only.
