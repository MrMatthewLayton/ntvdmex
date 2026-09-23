# Project state — start here

> **This is the canonical resume point.** If you have never seen this project before, read
> this file top to bottom and you will know where it is, what works, what does not, and
> what to do next.

- **Last updated:** 2026-09-23 (session 76 — ★★★★★ **THE SPEC-FIRST PROGRAMME IS THE WORK NOW, AND `59fac7d` IS THE RECORDED CHECKPOINT.** `git diff a5dd042..HEAD -- src/` is **EMPTY** — every `src/` byte matches the tree that built `2565bffe`, the last build the user confirmed by hand; the three change/revert pairs cancelled exactly. **No tag: the first tag will be `0.0.1` at the first beta release** (user directive). The rollback point is the hash. **Scope rule set: a device is in the inventory because it is in the period-correct hardware contract, NOT because a guest asked for it** — *"We don't not implement it because only one app asked for it."* GUS is in scope on that basis. **Win16 is NOT exempt from spec-first**: the Win3.1 SDK specifies the API, Wine and ReactOS implement it in the open, and only the WOW32 thunk ABI is genuinely unspecified — deterministic Win16 tests are owed. Two documents per surface from now on: `docs/ref/<surface>.md` (what the hardware does, our own words, cited, wiki-ready) and `docs/inventory/<surface>.md` (what we do, marked from the code). ⚠ **`db4c059` was reverted on an UNPROVEN regression — re-test it.** Order: **VGA to completion first.**)
- Previously: 2026-09-22 22:20 (session 75 — ⛔⛔⛔ **A WHOLE DAY LOST TO `detaillevel 1` IN DOOM'S OWN `default.cfg` — THE THIRD TIME.** User: *"What is running on the screen RIGHT NOW is perfect!"* after the single change `detaillevel 1 -> 0` on the SHARE copy. Nothing in NTVDMEX was wrong. **WHY IT HID: the rig had TWO COMPLETE NTVDMEX INSTALLATIONS with DIFFERENT game configs** — the share (`All Users\Documents\ntvdmex`, `detaillevel 1`, the one every `live doom` launched) and the user's own `Matthew\Desktop\ntvdmex` (`detaillevel 0`, holds their savegame). The user said "I play on high detail" (true of theirs), I said the config was low detail (true of the one on screen) — **both right, different installs.** Also `runs/s74_doom_regression/good/` was MISLABELLED low-detail captures, so every measurement "matched good" and proved nothing; it is renamed with a `READ_THIS_FIRST.txt`. A game config survived every binary swap, two reverts, a reboot and a full wipe-and-rebuild from the 17th tag. **Rig: one host, `6e28e178`, rebuilt from tag `release-20260917b`, installed + verified.** Today's VGA work (`db4c059`, `ec7c3e9`) was reverted during the hunt and is NOT the cause of anything. See the s75 block.)
- Previously: 2026-09-22 13:40 (session 75 — ★★★★★ **THE DIRECTION CHANGED AND THE FIRST STEP IS IN: BUILD FROM THE SPECS, THEN TEST THE APPS.** `docs/inventory/` is the method + the VGA measurement (41 of 71 registers modelled; every absent one describes geometry, addressing or panning). **Step 1 shipped (`a5dd042`): a real register file — every SEQ/CRTC/GC/AC index captured with a write count, the five unclaimed external ports claimed, `STAGE2: VGAREG` on both exit paths. It immediately proved the thesis: Doom programs CRTC `14`, CRTC `17` and GC `06` once each — the address generator — and we drop all three; Skyroads programs NOTHING. Same mode 13h.** Rig `2565bffe`, offvm 1426/0, selftest PASS, Doom to `ST_Init`, Skyroads `n8=0 max_ms=7`. ⚠ **BY-HAND SHELF CHECK OWED.** Win2000 PARKED at the user's request. **Stable zip still `4847355`/`9448cf27`.**)
- Previously: 2026-09-22 12:45 (session 75 — ⛔⛔⛔ **`install.bat` COULD ANNOUNCE AN INSTALL IT HAD NOT PERFORMED**: the single-instance guard returns 0 silently and the install verbs sat below it, so `/install` beside any live guest wrote nothing and exited 0. Fixed (`6449645`); `install.bat` now re-asks `/status`. That mechanism reproduces the user's Windows 2000 report exactly (installed-apparently, smoke fails, no logs) though it is not confirmed on that box. New: **`diag.bat` in the zip** names which of three failures you have. Trial zip for 2000 = `dist\ntvdmex-20260922-c477a13.zip`, host `97e37bbe`; rig `bin\` = same, selftest ALL PASSED. **Stable zip still `4847355`/`9448cf27`.**)
- Previously: 2026-09-22 10:50 (session 75 — ★★★★ **THE ZIP WENT TO TWO MORE MACHINES.** User's second XP box: everything fine first run. A friend's Win98-era box: first session broken (DOS/4GW games crawled; Win16 drew under STOCK — the tester had run them under stock first, and the resident shared WOW VDM kept them; `/install` and `/status` now detect and say so), fine after a reboot that also changed the BIOS, and Duke3D took no keyboard there. Host now keeps the last six logs (`ntvdmhost-1..5.log`). Rig `bin\` = `4fc852aa`; guards green; **stable zip UNCHANGED at `4847355`/`9448cf27`**. See the s75 block.)
- Previously: 2026-09-17 21:05 (session 74c — ★★★★★ **RELEASE CUT: `dist\ntvdmex-20260917-4847355.zip`, host `9448cf27`, git tag `release-20260917b`. USER-CONFIRMED BY HAND: Duke3D + its Setup, ZAR VESA modes + mouse buttons, on top of the 17:10 set.** `pkgtest` 8/8 + `pkgw16` from the package's own `bin\`. Old `f3c349d` zip → `debug\prev\`; `debug\prev\ntvdmhost_prev.exe` = `9448cf27`. **THE NEW STABLE ANCHOR — the zip is IMMUTABLE until the next confirmed build.** The user is copying the whole share to USB to install on a friend's machine.)
- Previously: 2026-09-17 19:15 (session 74c — ★★★★★ **DUKE3D RUNS + ITS SETUP RUNS; ZAR's VESA MODES RENDER AND ITS MOUSE BUTTONS ARRIVE** — `4012e1a` `d15a26a` `dbcf44f` `70fc097`; rig `bin\` = `9448cf27` = HEAD. ⛔⛔ **A shared path ring CREATED `cfg\pmnoirq.flag` at 18:59 and silently killed every PM timer until 19:08 — fixed (per-thread), flag deleted; if a DPMI guest has no time, `dir cfg\` FIRST.** Stable zip UNCHANGED at `f3c349d`/`a988c6e6`; **by-hand OWED: Duke3D + SETUP, ZAR VESA1/VESA2 + clicks, then Doom/Heretic/Hexen/Skyroads/heaven7/Notepad.** See the s74c block.)
- Previously: 2026-09-17 18:30 (session 74c — Duke3D runs, ZAR VESA renders; `4012e1a`, `d15a26a`, rig `61093e09`)
- Previously: 2026-09-17 17:10 (session 74b — ★ **RELEASE CUT: `dist\ntvdmex-20260917-f3c349d.zip`, host `a988c6e6`, USER-CONFIRMED BY HAND across all major apps and games incl. Hexen's loader; pkgtest 8/8 + pkgw16 from the package's own bin\. This is the new stable anchor; the old `eb466c56` zip is in `debug\prev\`.**)
- Previously: 2026-09-17 16:00 (session 74b — **HEXEN'S HI-RES LOADER RENDERS** (`2df5651`, rig `a988c6e6`); VESACUBE demo deployed (`demo\msdos\vesacube`, solid/culled/shaded, vsync, page flip); see the s74b block)
- Previously: 2026-09-17 14:50 (session 74b — PCem boots UNATTENDED (~60 s; the stall was AMI's 'D: drive failure — Press F1', not sync); p_vesa/p_vesapm/p_plan12/p_lpt clean vs real BIOSes; rig `474f7b2e`; see the s74b block)
- Previously: 2026-09-17 14:30 (session 74b — **PCem IS THE VESA ORACLE**: `p_vesa` 128/128 vs a real Tseng ET4000/W32p ROM + Bochs, rig `b3a3cf33`; see the s74b block)
- Previously: 2026-09-17 10:35 (session 74b — VESA/VBE runtime + text + 1280x1024 + PM + DDC done, rig `7d883a85`; PCem BOOTS (needs eyes); see the s74b block)
- Previously: 2026-09-17 09:20 (session 74b — USER-CONFIRMED BY HAND on `e92ce8ab`: Doom, Heretic, Hexen, Zar, Wolf3D, Skyroads; heaven7 geometry closed (`6fce192`, rig `36c872e9`); heaven7 music = GUS-only, no GUS model yet)
- **Score: 86.9%** (`./tools/score/score.py` — run it, do not quote this line).
  Session 53 moved it 72.4 → 79.6; session 54 → 80.2; session 55 → 83.1;
  session 56 → 85.4; s57, **s58, s59 and s60 moved it not at all** — s57 built the modal
  dialog loop, s58 spent the day on ZAR (#23). `guests` only counts a guest a
  human has confirmed, and `guest-zar` is BINARY: playable or not. Four real
  defects were fixed in s58 and the number did not move by one point. That is
  the model working, not the model failing — see the session 58 block.
- ⚠ **The rig was shut down at the end of session 54.** Re-mount the share
  (`mount_smbfs -N //guest@192.168.1.29/ntvdmex /tmp/xpshare`) and **check
  `ntvdmhost.exe /status` before believing any run** — see the hazard below.
  ⚠⚠ **Session 55 found the IFEO value ABSENT TWICE IN ONE DAY** — once on the
  freshly booted rig before a single run, and once mid-session after a batch of
  `taskkill`s tripped GH #132's recovery. Both scripts that lacked the re-add
  now have it (`wowlive.bat`, `wowrun.bat`), and `bmwow.sh` now FAILS LOUDLY
  instead of reporting `0 / 0 / 0`.
- **Branch:** `m9/completeness`
- **Tracker:** [140+ issues](https://github.com/MrMatthewLayton/ntvdmex/issues) — reconciled against the repo on 2026-08-26 (`tools/gh/backfill.py` is the manifest)
- **Knowledge base:** the [wiki](https://github.com/MrMatthewLayton/ntvdmex/wiki)
- **Day-by-day history:** [`docs/log/sessions/`](log/sessions/)

---

## What this is

**NTVDMEX is a replacement for `ntvdm.exe` on 32-bit Windows XP SP3.** It runs DOS
programs by executing 16-bit code on the **real CPU** in Virtual-8086 mode — reusing the
NT kernel's own VDM machinery through the undocumented `NtVdmControl` syscall — not by
emulating a CPU. It is not DOSBox and it is deliberately not a fork of anything.

**The goal:** install it so that every MS-DOS *and* Win16 launch routes to NTVDMEX and
stock `ntvdm` lies dormant. Not by overwriting `System32\ntvdm.exe` — that is Windows File
Protection territory — but by an Image File Execution Options `Debugger` value on
`ntvdm.exe`, which achieves the same routing and is reversible with one `reg delete`.

**The bar it was built against:** run real DOS games well — Doom, Skyroads, ZAR — with
flawless sound. **That bar is met** (Doom is fully playable, sound and mouse included).

### ★ The north star now: **run MS Paint and Notepad from Windows 3.x**

The DOS half works, so the goal moved to the Win16 half:

> **Run MS Paint (`PBRUSH.EXE`) and Notepad (`NOTEPAD.EXE`) from Windows 3.x under
> NTVDMEX.**

A good bar for the same reasons Doom was: small, iconic, and impossible to fake. Between
them they exercise the whole stack — NE loading, the KERNEL 16→32 boundary, USER windows
and menus, GDI drawing, mouse and keyboard. Paint in particular has to actually paint.

**Status: ★★★★★ BOTH NORTH-STAR PROGRAMS RUN, AND MS PAINT IS A PAINT PROGRAM
THAT SAVES FILES — USER-CONFIRMED (session 49).**
`PBRUSH.EXE` from Windows 3.11 draws **in colour** with the box, rounded-box,
ellipse and brush tools, its **flood fill stops at the border it should**,
everything **survives a minimise and restore** (so it is in Paint's own image,
not merely on the screen), and **`File > Save As` writes a valid 24-bit `.BMP`
to the directory you choose** — 4,909,014 bytes, 1680×974, headers verified on
disk, and confirmed by hand by the user. `NOTEPAD.EXE` has been a working text
editor since session 44 (open, type, save, menus, Help > About).

★★ Paint's whole UI is **pixel-identical to stock ntvdm**, measured child by
child: the toolbox with its real colour tool icons, the line-size box, the colour
palette bar, the canvas and its scrollbars. Its menu bar opens, and **mouse and
keyboard input reach it**.

★★★ The user's three defects from session 45 are closed, and two of them were
**one call**: `GetProfileString` (krnl386 id `0x3a`) was unimplemented, so Paint's
`GetProfileString("Paintbrush", "clear", "COLOR", …)` returned nothing, and eleven
instructions later it selected its **black-and-white** palette and a 1bpp canvas.
⚠ Session 45's note that WIN.INI has no colour key was *correct and led away from
the answer* — **the default is `COLOR`, and an unimplemented call cannot return a
default.** The third defect ("fill does not work") was `ExtFloodFill` plus
**`CreatePen`**, both of which `neneeds.py` reports as *free*. See
[session 46's resume block](log/sessions/session-46.md#-resume-here).

Below is how the bootstrap got there, kept because it is still the reference for the
loader and the scheduler. As of session 39 it runs
a long way: krnl386 loads three of its four segments, installs its interrupt handlers,
**takes and returns from its own DPMI exceptions**, and loads **all eight** of the 16-bit
system modules — `SYSTEM.DRV`, `KEYBOARD.DRV`, `MOUSE.DRV`, `VGA.DRV`, `SOUND.DRV`,
`COMM.DRV`, `USER.EXE` and `GDI.EXE` — completes its bootstrap, reads `[boot] WOWSHELL` out of
`SYSTEM.INI`, finds and opens `C:\WINDOWS\SYSTEM32\WOWEXEC.EXE`, loads it and **runs it**.
As of session 38 the `0001:229C` general-protection fault is
**gone** — it was an *ordering* defect, not a value one, and behind it was the fact that
**krnl386 has no scheduler and we are it**. With a ~70-line cooperative scheduler in the host
(`src/wow/wowsched.h`, opt-in), krnl386's boot task returns from `LoadModule`, retires itself,
and WOWEXEC restarts and runs on past `LoadCursor` into **filling in a `WNDCLASS`** and **registering it: `RegisterClass "WOWExecClass"`**, the first time this project has read
what a Win16 program is putting on screen. Two host defects fell behind that: a **read-only string literal** handed
to `GetProfileIntA` (a deterministic `0xc0000005` in `ntdll`), and — the serious one —
**dispatching WOW32 calls on the id alone when the id space is PER MODULE**, which had us
answering WOWEXEC's `RegisterClass` with `GetProfileIntA`. USER's table is now mapped
(`docs/research/wow-user-surface.md`, 441 ids, 385 named) and has its own dispatcher.

★★ **As of session 39 `CreateWindow` is answered**, so WOWEXEC creates a window
(`"WOWExec"`, `WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN`), registers a *second* class,
creates a second window, reads `SYSTEM.INI`'s `[drivers]`, and arrives at **its own
message loop** — `WowWaitForMsgAndEvent` / `PeekMessage` / `TranslateMessage` /
`DispatchMessage`, every one named out of the import table rather than inferred.

★★★ **And then it is told what to run.** `WowGetNextVDMCommand` is *"which 16-bit
program do I run?"*; we were answering the harness sentinel, which WOWEXEC reads as a
hard error and reports as *"Can't run 16-bit Windows program"*. Answered, **krnl386
opens `C:\WINDOWS\SYSTEM32\SYSEDIT.EXE` and reads its `MZ` header** — a real Win16
application being loaded, not a shell with nothing to do. Behind that, one more wall
fell and it was ours: **our own INT-site patcher had corrupted krnl386's code**, turning
a `jne` into a `les` and killing every launch at `0001:2053` — and fixing it turned out
to **fix Doom too**, which the old rule had been breaking.

★★★★ **And then the whole chain closed.** `0xc5` — the *module path resolver* — was
answering 0, so krnl386 composed every module name against the current directory;
answered, `SHELL.DLL` and `MMSYSTEM.DLL` open from `system32`, SYSEDIT's imports bind,
and krnl386 **creates its task database and launches it**. The last missing piece was a
scheduler moment: WOWEXEC never retires, so a task parked at its launch waited forever.
`WowWaitForMsgAndEvent` is the Win16 *"I have nothing to do"* primitive, so a task
blocking on it is a task yielding — and with that, **SYSEDIT.EXE's Win16 task executes on
its own stack.**

★★★★★ **And then it runs its whole startup.** The last thing in its way was its
*environment*: `PSP+0x2c` held `1`, which turned out to be **our own `wow32ret.txt`
experiment value** for krnl386 seg2 id `0xd1` — the call that *creates a launched task's
environment*, whose answer krnl386 stores into the child's PSP at `seg2:0x2b55`. Answered
properly (a **copy**, because the parent frees the block it lends the child),
`SYSEDIT.EXE` registers **its own** two window classes, `mpframe` and `mpchild`, and
creates its main window — **"System Configuration Editor"**.

★★★★★ **And as of session 40 the host CALLS 16-BIT CODE — the direction this project
had never gone.** A Win16 window procedure is an ordinary FAR PASCAL function, and three
of the four things needed to call one already existed (a saveable context, the
application's own stack, and an entry convention read off SYSEDIT's own prologue). The
fourth is new and it is three bytes: `C4 C4 57` in guest memory with a 16-bit **code**
selector over it, pushed as the far return address, so the procedure's own `retf` lands
on a BOP. `CreateWindow` now sends `WM_CREATE` to the window's own procedure and waits
for it. Three walls fell behind it — **the SYSTEM window classes belong to the 32-bit
side, which under WOW is us** (`MDICLIENT`); **`USER` id `0x217` is `NotifyWow`**, which
does not return a handle but permission, and answering it makes `LoadAccelerators`
succeed — and with them **`SYSEDIT.EXE` shows and updates its main window and opens
`C:\CONFIG.SYS` and `C:\AUTOEXEC.BAT`**, which is the thing the program exists to do. It
then correctly reports `"C:\CONFIG.SYS\nCannot open this file."`, because it asked its
MDI client for a child window (`SendMessage(WM_MDICREATE)`) and **the MDI client's window
procedure is the 32-bit side's, i.e. ours, and does not exist yet**. Nothing has drawn a
pixel. See #128 below.

★★★★★ **And as of session 41 the MESSAGE LOOP TURNS, on a real keystroke.** SYSEDIT's
`GetMessage` used to get the harness sentinel, which its own `or ax,ax / jne` reads as
`WM_QUIT` — so the application was not failing, it was being dismissed. There is now a
**message queue**, and the host's own keyboard feeds it: a key pressed on the rig becomes
`WM_KEYDOWN` (the virtual key from `MapVirtualKey`, i.e. the OS, not a table), is taken by
`GetMessage`, survives `TranslateMDISysAccel` / `TranslateAccelerator` /
`TranslateMessage`, and is handed to **`DispatchMessage`, which calls the window procedure
of the window the guest itself gave the focus to**. Twelve messages delivered and
dispatched in a run. Four ids the export table could not name were named by the run —
`0x71` TranslateMessage, `0x72` DispatchMessage, `0xb2` TranslateAccelerator, `0x1c3`
TranslateMDISysAccel. And a defect session 40 located but deliberately left unfixed is
fixed: our protected-mode DOS services returned `CF` in **live EFLAGS**, and the three-byte
handler stub the guest returns through (`C4 C4 CF` — the BOP, then an **IRET**) restored
the caller's flags over the top of it, so `SYSEDIT` was told two 0-byte files could not be
read. `SYSEDIT.EXE` now **reads all four of its files and shows no message box**. Still no
pixel. ⇒ **the frontier is pixels.**

---

## How far along is this, honestly

⚠ A single percentage is a judgement, not a measurement, so here are three against three
different bars, with the basis stated. (The last recorded figure, *~40% of the full vision*,
is from the **2026-08-05** review — it predates working sound, working DPMI and the entire
Win16 push, and should not be quoted.)

| Bar | Where it is | Est. |
|---|---|---|
| **The original DOS games bar** — Doom / Skyroads / ZAR, flawless sound | Two of three fully playable and confirmed by hand. ZAR is the gap, and **the gap is NOT VESA** (s58): it ships `VGA_320x200` and dies long before any video call. It now banners and reaches `Game loading...`, then stalls. | **~85%** |
| **★ The north star** — MS Paint + Notepad from Windows 3.x | **BOTH RUN AND BOTH DO THEIR JOB, USER-CONFIRMED.** **NOTEPAD IS A WORKING TEXT EDITOR**: opens through the real XP file dialog, you type into it, File > Save writes the text (verified byte for byte). **MS PAINT DRAWS IN COLOUR, KEEPS WHAT IT DRAWS, AND SAVES IT** — every shape tool, the flood fill, persistence across a repaint, and `File > Save As` writing a valid 1680×974 24-bit `.BMP` to the chosen directory; UI pixel-identical to stock ntvdm. **Paint's GDI surface is 67/76 and its USER surface 88/92; PBRUSH.DLL is 16/16.** ⚠ Not yet exercised: the Text tool, the cutout tools, Edit > Paste, printing. **The Alt-menu defect is CLOSED (session 53)** — Paint holds the mouse capture and USER32 will not open a menu while one is held. | **~90%** |
| **The full vision** — an `ntvdm` superset on XP-32 | Everything above, plus the host UI, minus the standing DOS defects and M7/M8. **This is the bar `tools/score/score.py` measures, and it is the only one of the three with a model behind it rather than a judgement.** | **79.6%** |

**Read the north-star number carefully.** The hard *unknowns* are largely behind us — what is
left is mostly known work, but there is a lot of it, and **M6 is one line on the roadmap and
probably the largest single body of work remaining**: 16:16↔flat thunking, USER/GDI object
mapping, and message bridging. *Calling into 16-bit code* was on that list until session 40
and is now a working mechanism (`src/wow/wowcall.h`); *message bridging* was on it until
session 41 and is now `src/wow/wowmsg.h`, with the host's own keyboard on one end of it and
a Win16 window procedure on the other.

⚠ And the standing DOS defects below are small individually but sit in the **"runs but lies"**
class this project treats as the most expensive kind. **`MEM.EXE`'s main report is now correct
and matches the 6.22 oracle row for row (session 53)** — but `MEM /C` still reports MSDOS as
1,028K, contradicting its own summary in the same report, so the class is not closed.

---

## Where it actually is

### ✅ Working, and confirmed by hand on real hardware

| | Status |
|---|---|
| **Doom** | **Fully playable** — 3D rendering, status bar, menus, PCM + MIDI, keyboard **and mouse**. Runs its own 32-bit code through DOS/4GW on real silicon. |
| **Skyroads** | **Fully playable** — menus, Controls, level select, in-game. |
| **MS-DOS 6.22 `COMMAND.COM`** | Runs as a guest: prompt, line editing, internals (VER/VOL/CLS/ECHO/SET/TYPE/COPY/DIR/EXIT), and an external program EXEC'd and **returned from**. |
| **DOS API** | 103 INT 21h functions. XMS 3.0, EMS (LIM 4.0). |
| **Video** | Text (authentic IBM ROM font), mode 13h, mode 12h planar, VESA banked. Windowed GDI + exclusive-fullscreen DirectDraw, Luna-themed. All ten QuickBASIC demos render with **zero pixel defects**. |
| **Sound** | SB16 PCM at **99.999%** delivery, clean-room OPL2/OPL3 FM (MIT, Nuked used only as a black-box oracle), MPU-401 MIDI, PC speaker. |
| **DPMI** | 0.9 host running unmodified third-party clients; real-CPU protected mode; 32-bit DOS/4GW. |
| **Host UI** | Menu bar, status strip (program \| 16/32-bit \| Real/Protected mode \| capture state), six-tab Settings dialog backed by `HKCU\Software\NTVDMEX`. |

### ❌ Not working — and these are the honest blockers

| | Why it matters |
|---|---|
| **Win16 / WOW — two applications work, the other seventeen on the shelf are untested** | ⚠ **This row's detail below is the 2026-08 bootstrap history, kept for the loader and the scheduler; it predates sessions 42-49 and its "nothing draws" framing is long out of date.** Notepad edits and saves text; MS Paint draws in colour and saves bitmaps. **What is unproven is everything else** — the shelf has been *measured* (Solitaire needs 9 services, Minesweeper 15, Media Player 20) but not one of them has been launched. Until they have, "WOW works" is a claim about two programs. And installing NTVDMEX permanently still routes *every* 16-bit Windows launch through us. `ntvdm.exe` is *also* the host for every 16-bit **Windows** program. The NE loader loads, relocates and binds the **whole** XP WOW module set on real hardware, **krnl386 executes** — protected mode, its own segments, its interrupt handlers, its own DPMI exceptions, and all eight 16-bit system modules — and it then finds, loads and **runs `WOWEXEC.EXE`** — which, since session 38's host-side task scheduler and session 39's `CreateWindow`, registers two window classes, **creates two windows** and sits in **its message loop** — and, since session 39's `WowGetNextVDMCommand` + `0xc5` + the yield point, **launches `SYSEDIT.EXE`, which runs its startup, registers its own classes and creates its own main window** — and, since session 40, **receives `WM_CREATE` in its own window procedure**, because the host can now call 16-bit code. It goes on to create its MDI client, load its accelerators, show and update its main window, and build **four MDI children, each with its own `EDIT` control**, titled `C:\WINDOWS\SYSTEM.INI`, `WIN.INI`, `C:\CONFIG.SYS` and `C:\AUTOEXEC.BAT`, and — since session 41's CF fix — **reads all four into memory**. Since session 41 it also **runs its message loop**: a key pressed on the host reaches the window procedure of the window the guest gave the focus to. But a window here is a host-side *object* — a handle, a class, a rectangle — **with no pixels behind it**, so nothing draws, `WM_PAINT` has nowhere honest to come from, GDI's id space is not dispatched at all, and there is no 16:16↔flat thunking. Since interception is an IFEO key on `ntvdm.exe`, and Win16 launches go through `ntvdm.exe` too, **installing NTVDMEX permanently would break every 16-bit Windows app today**. → [#128](https://github.com/MrMatthewLayton/ntvdmex/issues/128) |
| **Console/stdio integration** | DOS output is buffered and flushed to `CONOUT$` at exit, so shell redirection and piping are bypassed and every DOS program pops a window. Blocks non-interactive use. |
| ~~In-guest redirection~~ | **DONE** (#133, session 52) — oracle, rig and stock ntvdm agree on all six cases. This row was stale. |
| ~~No INT 13h / INT 25h / 26h~~ | **DONE** (#44) — `disk-int13` scores 100%. This row was stale. |
| ~~No TSRs~~ | **DONE** (#49) — `tsr` scores 100%. This row was stale. |
| **`MEM /C` reports wrong figures** | The main report is correct and matches the 6.22 oracle row for row (#47, s53). `MEM /C` still says MSDOS is 1,028K, contradicting its own summary. |
| **ZAR** | ⚠ **NOT a VESA gap — that entry was wrong and is corrected in the session 58 block.** ZAR's own `USER1.CFG` ships `VGA_320x200` (mode 13h, supported since M3) and it never calls `INT 10h` at all. As of s58 it banners and reaches `Game loading...`, then stops after ~4.8 MB of loading. Four defects were fixed to get there, all ours. |
| **24 of 47 settings** | Stored in the registry, honoured by nothing (was 40 of 46 before session 53). The three `settings_apply*` functions in `src/host/main.c` are the honest list of what actually works, and the comment above them now names *why* each remaining one is not there. |

---

## Next actions, in order

> ⚠️ **The plan changed on 2026-08-26.** #129 was going to make "leave it installed"
> safe by handing Win16 launches back to stock ntvdm. **That is impossible** — measured
> three ways (see #129). Windows validates the VDM image's identity, so a renamed copy is
> refused outright, and the real name re-enters us through the IFEO hook. So there is no
> safe install story until WOW exists, and #128 moved onto the critical path.

1. **[#128] WOW / Win16 — IN PROGRESS. ★★★★★ THE NORTH STAR IS MET: NOTEPAD
   EDITS AND SAVES TEXT, AND MS PAINT DRAWS IN COLOUR AND SAVES A BITMAP.**
   ⚠ **And that is a claim about TWO programs** the host has been shaped around
   for eight sessions. The shelf is measured and unlaunched; a third guest is
   the next step, and it is also the honest correction for *a fix measured on
   one guest is a fix for none*.

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

   ### ★★★★★ s74c (17th, 17:15–18:30) — **DUKE NUKEM 3D RUNS. ZAR'S VESA MODES RENDER. TWO DPMI DEFECTS, BOTH "A 32-BIT CLIENT IS NOT A 16-BIT CLIENT".**

   > **Rig `bin\ntvdmhost.exe` = `61093e09` = HEAD `d15a26a`. Stable zip on the share is
   > STILL `f3c349d` / `a988c6e6` (`debug\prev\ntvdmhost_prev.exe`) — unchanged, per the
   > rule. `debug\prev\ntvdmhost_a988c6e6.exe` is the displaced release host.**
   > **By-hand OWED on `61093e09`: (1) Duke3D from `demo\msdos\duke3d` — expect the
   > setup/title, demo, sound; (2) ZAR → options → a VESA1 and a VESA2 mode (the by-hand
   > crash); (3) the DPMI regression set: Doom, Heretic, Hexen, ZAR VGA, Skyroads,
   > heaven7, Notepad — all headless-green here, none eyeballed.** `cfg\` clean, ZAR's
   > `USER1.CFG` restored to `VGA_320x200`.

   **1. Duke3D "doesn't load at all" was its own exit 0 (`4012e1a`).** The morning's
   corpus log already held the answer: the game writes *"You don't have enough memory to
   run Duke Nukem 3D … 'Total memory free'"* to stdout and terminates. It sizes itself
   from **INT 31h 0500** called from flat 32-bit code with the 30h-byte block on its
   32-bit stack (`ES:EDI = 0x2f7:0x045d53xx`); the handler did `esb + (EDI & 0xFFFF)`,
   wrote our 64 MB answer at linear `0x53xx`, and the game read its own uninitialised
   buffer. `dpmi_rmcs_ptr` had learned this rule from Doom's 0300 mouse RMCS in s6x; it
   is now `dpmi_caller_off()` (the caller's CS D/B bit decides 16 vs 32) and **0500 and
   0303 (handler DS:ESI, RMCS ES:EDI) use it**. 0500 refuses an unreadable block with
   8021 instead of writing it. **Headless: CONs compile, art/palette load, mode 13h,
   DEMO1 plays with SB DMA — `runs/s74c_duke/shot01.png` is E1M1.** Duke3D is a bound
   DOS/4GW *Professional*; no `DOS4GW.EXE` needed.

   **2. ZAR crashed on selecting a VESA mode (`d15a26a`) — reproduced headless by
   editing `USER1.CFG` `graphics VideoMode` to `VESA2_640x480` / `VESA1_640x480`.**
   4F02 `0x4101` accepted, 0800 mapped the LFB, then **`#GP` on the first `rep stosd`
   through `es=0x3af`**. The log had the cause three lines up: `DPMI-LDT: install
   REJECTED … 0xC000011A`. ZAR builds the selector with **0009 CX=8092 — DPL 0, G=1** —
   then 0007 base, 0008 limit `0x4afff` bytes. NT's `PspIsDescriptorValid` rejects any
   non-null LDT entry whose DPL≠3, and G=1 over a raw `0x4afff` field is a 1.2 GB segment
   it rejects again. DPL 0 is legal on real DOS only because DOS/4GW runs the client at
   ring 0; a ring-3 host takes the client's CPL (the spec says so). **`dpmi_install`
   forces DPL 3 on PRESENT descriptors** (first attempt forced it on freed/null ones too
   → 2375 rejections — a null descriptor is the one non-DPL-3 entry NT takes) **and 0008
   clears a client-set G flag** (its limit is bytes; the host chooses G). Both paths
   in-game at 640×480: `runs/s74c_zar/zar_vesa2_01.png` (LFB) and `zar_vesa1_01.png`
   (banked, 4,735 `4F05` calls).

   **3. Guards on `61093e09`, all headless, `runs/s74c_guards/`:** Doom/Heretic/Hexen
   heartbeats 672/344/646 vs the morning's confirmed 674/359/656, same SB block counts,
   same Hexen watchdog wind-down · ZAR VGA 579 vs 591, 0 faults · Skyroads `n8=0
   max_ms=7` · heaven7 `0x4170` LFB, 16,389 `4F07` · Notepad (Win16) launches and
   X-closes through the package host (`w16close_notepad.txt`) · **offvm 1426/0**.

   **4. The other "never worked" guests, read not run:**
   - **Chasm, Radiance:** *"Runtime error 200 at xxxx:0091"* — Turbo Pascal's CRT-unit
     `Delay()` calibration overflow on any CPU > ~200 MHz. Identical on real hardware
     and stock ntvdm; the fix is the well-known EXE patch (TPPATCH/ctbppat), not ours.
   - **egasikio:** its readme says *"requires an EGA card … doesn't work on VGA-cards"*;
     it detects VGA and exits 0. Correct.
   - **Fusion (Russian demo, own DPMI stub): DIAGNOSED, NOT FIXED.** It installs its own
     PM handler for **INT 33h** (`0205 → 0x187:0x55b`) and then issues `INT 33h AX=D021`
     as its *own* syscall; our patched site services vector 33h as the mouse and the
     demo aborts cleanly. The dispatcher chains only vector 21h to a client handler
     (`main.c` ~18376); 10h/16h/1Ah/33h are host-serviced regardless. **The right fix is
     "a client-installed PM handler owns the vector", but DOS/4GW installs passthrough
     handlers on all four for every game on the shelf** (`sel:0x40/0x58/0x68/0xcc`), so
     that change reroutes the whole shelf's BIOS/mouse traffic through DOS/4GW's
     reflector — not for deadline day. Candidate narrow rule: chain when the handler's
     selector is a 32-bit code selector the extender did not create.

   **5. Second pass (18:35–19:15, `dbcf44f`, `70fc097`) — the user's next two: "duke3d's
   setup program crashes, and mouse clicks still don't work in zar".**
   - **ZAR's buttons (`dbcf44f`).** ZAR installs an INT 33h **0Ch event handler from flat
     code** (mask `7e` = buttons only, handler `0x347:0x045d2240`) and polls motion with
     0Bh — so aiming worked and no click could: `mouse_cb_try()` served V86 guests only
     and **dropped a PM client's queue** ("not this path (yet)", counted `cb_pm`). New
     `dpmi_inject_pm_mousecb()` = `dpmi_inject_pm_irq()`'s mechanics with a **RETF
     frame** (CS:EIP, what the handler pops) and AX/BX/CX/DX loaded; 0Ch/14h store a
     full-width EDX from a 32-bit caller. Headless with `keys.txt` `m0`/`m1` (needs
     `qimode.txt`=`20`): **10 injected, 10 returned**, both buttons, press and release.
     The click's *effect* is by-hand: headless capture refuses ZAR's 800×600 window
     snapshot (instrument gap, unchanged).
   - **Duke3D SETUP (`70fc097`) was four things**, each named by the log in turn:
     (a) *"DOS/16M error: [8] cannot open file ''"* — SETUP EXECs `.\setmain.exe` with
     its **own env segment** and we handed it over as-is; DOS **always** copies and
     appends `0001` + the program name, which is where DOS/4G finds its LE payload.
     (b) Then it sat at `0x042d16f1` until the **watchdog** killed it — a flat client
     that hooks the keyboard but not the timer never bumps `g_dpmi_iter`; "frozen 3 s"
     read as wedged. The watchdog's own live sample showed the EIP moving; **client code
     at a new EIP now resets the streak** (same idea as the Win16 GetMessage exemption).
     (c) The async timer path refused IRQ0 (`why=6`, app not hooked) and bumped nothing,
     so a Watcom `delay()` had no clock; it now does the BIOS's `0040:006C` bookkeeping
     there, billed to the owed count.
     (d) ⛔⛔ **Then Duke3D itself regressed — *"Playback failed, possibly due to an
     invalid or conflicting IRQ"* — and it was NOT code: `cfg\pmnoirq.flag` had appeared
     at 18:59 containing the watchdog's first log line.** `ntvdmex_path()` used a 16-slot
     ring shared across threads; the watchdog's path pointer was overwritten with the PM
     loop's `pmnoirq.flag` probe before its `CreateFile`, so the watchdog CREATED the
     knob that suppresses every PM IRQ. Two guard runs "passed" under it (their checks
     were too weak to notice no timer). Per-thread ring now (Win32 TLS — `__thread`
     wants libgcc's emutls). **Rule: a DPMI guest with no time → `dir cfg\` first.**
     SETUP's main menu is drawn (`runs/s74c_duke/shot02.txt`) and it lives the whole run.
   - Guards on `9448cf27`: Duke3D plays (0x1574 ticks) · SETUP alive · Doom `STAGE2:
     complete` · ZAR VGA 579 HB / 0 faults · heaven7 LFB · Notepad opens/X-closes ·
     QBasic V86 click 2/2 · offvm 1426/0.

   **Lessons.** *A guest that "doesn't load" usually said why — read its stdout out of
   the log before the DPMI trace* (Duke3D's message was in the morning's log at 08:31).
   *A regression need not be code — `dir cfg\` before bisecting* (a flag file with log
   text in it). *"Wedged" needs a moving/not-moving test, not a trap count.*
   *`& 0xFFFF` on an (E)SI/(E)DI/(E)DX from a client is a 16-bit assumption; grep them
   all when one bites* (s6x fixed the RMCS one and left 0500/0303). *A cfg file is a
   headless menu* — ZAR's by-hand crash became a 90-second loop.

   ### ★★★★★ s74b (17th, 07:30–08:50) — **HEAVEN7 RUNS WITH NO FLAG. THE PATCHER KNOT IS UNTIED. VESA'S RUNTIME HALF (4F04–4F09) IS DONE, SPEC-TESTED.**

   > **Rig `bin\ntvdmhost.exe` = `e92ce8ab` = HEAD `9938226`. Stable zip on the share is
   > STILL `eb466c56` — unchanged, per the user's rule. Rollbacks in `debug\prev\`:
   > `f3468d65` (this morning's start), `0d85af53`, `6ba7524a`, `772d8911`.**
   > `cfg\` is clean (no diagnostic flags). **Nothing needs the user until they can test
   > by hand: heaven7, then Doom/Heretic/Hexen/ZAR, whose INT path changed.**

   **1. heaven7 — the knot (`b45137b`).** All seven "INT sites" the eager scan wrote
   `C4 C4` over are in `h7.EXE` at object+0x2d8: the LE code object is a PACKED payload
   (s74's "runtime-generated" was wrong). Reproduced offline with the real `x86len.h`:
   the six false sites score **44–48 votes of 48** — x86 self-synchronisation converges
   on random bytes as well as on code; the vote measures convergence, not code-ness.
   Entropy separates only at 1 KB windows by 0.3 bits over 281 real sites; rejected.
   **Fix: `d32` regions are scanned and logged (`32-bit, NOT WRITTEN: would patch N`)
   but never written**; the `#GP(IDT)` arm services on the CPU's evidence and patches on
   the way past. 16-bit paths unchanged.
   **That exposed a second defect:** the lazy arm restored `VTIB_ESP = fr[6]` — the
   frame's **16-bit** SP — while its own comment claimed flat-SS clients were "declined
   above". Nothing declined them. heaven7 survived by luck; **Doom (SS flat, ESP
   `0x0443xxxx`) would have died on its first lazy INT.** The kernel saves full-width
   SS:ESP/EIP in the TIB (`fcs:feip` = SS:ESP, `fss3` = EIP — calibrated on three known
   faults); ESP now comes from the slot when SS is 32-bit (and only if its low half
   matches the frame), EIP for a flat CS from `sav3`, byte-checked, with the 0501-block
   reconstruction kept as a logged cross-check (`AGREE` on every one of 14 heaven7 +
   17 Doom sites).
   **Graded headless, `runs/s74b_lazy32/`:** heaven7 ×3 renders (22,524 colours) ·
   Doom ×3 `STAGE2: complete`, 17 lazy INTs, 0 declined · Heretic/Hexen wind-down
   identical to `eb466c56` · **ZAR A/B vs `f3468d65`: same frame, same wind-down
   address** (its forced-exit path prints no STAGE2 summary — grade it by capture) ·
   Skyroads `n8=0 max_ms=6–7`, guards intact · Notepad opens and closes ×2 · offvm
   1396/0.

   **2. VESA runtime half (`5be7cb6`, `9938226`) — tests from the PDF first, 31 new
   checks, 25 of which fail on the previous code.**
   - **4F06/4F07 were accepted and ignored** — the presenter never read the logical
     pitch or the display start, so a page-flipping guest saw page 1 forever with
     `004F` in hand. Now `vesa_stride` + `vesa_origin()` drive the 8bpp pointer, the
     direct-colour conversion and the written-extent probe. Failure codes per spec.
   - **4F05 read BL as set/get** (spec: BH set/get, BL window). "Get window A" was a
     SET to whatever DX held. Window B fails; LFB mode fails AH=03.
   - **4F08** next-lower width (10→8, 7→6), AH=03 in direct colour, reset to 6 on any
     mode set. **4F09** never called `pal_refresh()` (presenter kept the old palette);
     BL=02/03 → AH=02; DX+CX>256 → AH=02.
   - **`pal_refresh()` took the EGA attribute path in VESA 8bpp** (a 4F02 never sets
     `mkind`), so indices 6, 8–15 showed DAC 0x14, 0x38–0x3F. Identity now.
   - **4F04 did not exist; AH=1Ch reported 3 blocks and wrote 768 bytes** — the Heretic
     MCB overrun in another function. One 896-byte state block serves both; restore
     re-enters the mode with the don't-clear bit and refuses a foreign buffer.
   - **Corpus inventory (`runs/s74b_vesa_corpus/inventory.txt`, 20 targets):
     heaven7 is the ONLY guest on the shelf that calls VESA** — 4F00/01/02/07. Its
     15,900 `4F07` calls are all `(0,0)`, none refused: display start as a flip/vsync
     idiom on a single buffer. **So its 320×176 placement is the demo's own** (most
     likely its CPU-speed render-size pick); no VESA question remains there.
     `graphics\VS87.EXE` produced no summary (died/hung) — not looked at.

   **3. 09:00–09:20 — USER-CONFIRMED BY HAND on `e92ce8ab`: Doom, Heretic, Hexen, Zar, Wolf3D,
   Skyroads all still play.** heaven7 loaded "in a box". **Closed (`6fce192`, rig `36c872e9`):**
   read the guest — new `cfg\memdump.flag` (`<linear> <size>` → `debug\out\memdump.bin` at the
   headless deadline) dumped heaven7's UNPACKED image (`runs/s74b_lazy32/h7_memdump_04330000.bin`).
   Its help text is `-1 512x384 -2 640x480 -3 800x600 -a low -b average -t no text -n no sound
   -l looping`, and a render table `(320,176) (512,280) (640,352) (800,440)` — one letterboxed
   picture per screen height. **320x176 is the picture for its DEFAULT screen, 320x240 — an OEM
   mode we did not publish.** It fell back to 640x480 and drew the default picture at
   `y = 480-240+(240-176)/2 = 272`. Published 320x240x{8,15,16} as S3 `0x151/0x160/0x170`; the
   unmodified demo now picks `0x4170` and draws edge to edge (its bars are its own). `h7 -2` =
   640x352 in 640x480, also correct. Captures `shots_h7_320/`, `shots_h7_dash2/`.
   **Music: heaven7 is GUS-ONLY** — the image scans for `ULTRASND=`, programs base+offset, has no
   `BLASTER` string and no `0x2xx` port immediate. A Gravis Ultrasound model (GF1: 32 voices,
   1 MB DMA-loaded sample RAM, envelopes, timers, IRQ) is a new device — not a same-day job.
   `-n` silences it cleanly. By-hand owed on `36c872e9`: heaven7 default + `-2`.

   **4. 09:20–10:35 — the rest of the VBE list (`fac0349` `5f42907` `f8b3410`), rig `7d883a85`:**
   - **1024x768 + 1280x1024** (`0x105/0x116-118`, `0x107/0x119-11B`): `NTVDD_FRAME_MAXW/H`
     (1280x1024) in `ntvdd.h` now sizes the presenter's snapshots, its guard, the VESA list's
     cap and `VID_FB_MAX` from ONE number; VRAM 2 → 4 MB.
   - **132-column text modes `0x108-0x10C`**: 4F01 answers in characters (model 0, B800),
     4F02 goes through the standard mode-3 set then applies the geometry, 4F03 remembers the
     VESA number until a standard set clears it. Renderer needed nothing — it was already
     `cols x 8` / `rows x cell_h`. QB's 80x25 unchanged on the rig.
   - **4F10 VBE/PM** (report/set/get) and **4F15 VBE/DDC** (synthesised EDID 1.3, valid
     checksum, one block) — both from the published interface as Bochs/DOSBox answer; no PDF,
     no oracle, said so in the commits.
   - `4F0A` PM interface stays a clean decline BY DESIGN (guests fall back; a proper one is
     position-independent code the client copies that we then have to service).
   - video_test 197 → 219 checks; offvm **1418/0**. Guards on `7d883a85`: heaven7, Doom ×2 clean
     (the first post-deploy Doom took the watchdog wind-down — the standing rule, not chased).
   - **PCem BOOTS AND EMULATES** (`~/PCem/roms` was missing — PCem created `~/PCem/` itself in
     s71; CMOS now 32 MB + C-first). The guest chain after POST is unobserved: needs the user at
     the Mac once (approve lldb, or PCem's screenshot key). ⛔ **DO NOT `lldb -p` unattended** —
     the pending SecurityAgent prompt wedged every unsigned exec on the Mac for 20 minutes
     (memory `lldb-attach-wedges-the-mac`).
   - **heaven7 (`36c872e9`+) default = 320x240x16 LFB edge-to-edge; `-2` = 640x480. GUS-only
     audio; no GUS model.** By-hand owed on `7d883a85`: heaven7, then the six confirmed on
     `e92ce8ab` if you want the new stable to be this build.

   **5. 11:00–14:30 — PCem IS AN ORACLE (`42ab7f8` `2451aa8`), and it found two defects
   the spec audit had passed.** With the user's eyes for three minutes: DOS 6.22 boots to
   `C:\>` in PCem (AMI 486, real IBM VGA), `AUTOEXEC` has the `A:\RUN.BAT` hook.
   - `--load_drive_a` is a NO-OP in the wx build; A: is mounted via the config's `disc_a`
     line (rewritten per launch). Guest POST+boot ≈ 100–115 s; PCem writes the floppy image
     THROUGH while running, so the poll loop stands (default 240 s).
   - ⛔ **`enable_sync = 1` STALLS THE GUEST WHEN THE WINDOW IS NOT BEING DRAWN** (another
     Space, behind a fullscreen app): CPU spins at ~25 %, nothing progresses. Five "boot
     never finishes" runs were this; every run that worked was one somebody was looking at.
     Both configs now `enable_sync = 0`. `pcem/` is gitignored — the note lives in
     `scripts/pcemoracle.py` and memory `pcem-oracle-setup`.
   - New probe **`tools/dostest/p_vesa.asm`**: VbeInfoBlock + every ModeInfoBlock, BIOS
     pointers masked. dosdiff hosts **`pcem`** (IBM VGA) and **`pcem-vesa`** (Diamond
     Stealth 32 = Tseng ET4000/W32p, VESA 1.2 in ROM, `configs/NTVDMEX-VESA.cfg`); rule
     cases may be globs; a probe opts into extra oracles with `; ORACLE-ALSO: <host>`.
   - **Found by the oracle, fixed:** VbeInfoBlock **Capabilities D0 = 0** (we honour the
     8-bit DAC but told guests not to ask) · **NumberOfImagePages = 0** for every mode (the
     s74 audit fixed the offset and left "one page" in it) · YCharSize 8 in 200-line modes ·
     DirectColorModeInfo D1 for 5:5:5 · Lin/BnkNumberOfImagePages follow +29.
   - **Read and deliberately NOT copied**, each a recorded rule: card version/memory/OEM
     rev · Tseng's write-only-A/read-only-B window pair · 5:5:5 as "16 bpp" · TTY-in-graphics
     bit (we don't draw teletype into VESA modes, so we don't claim it) · 9-dot 80-column
     text cells (we render 8-dot and say 8 — fix by rendering 9, not by lying).
   - Bochs and the real ROM split on YCharSize/DC-D1; with both voting those rows are
     DISPUTED; the two modes only Bochs offers abstain it, reason written down.
   - **`paritysweep.sh p_vesa`: 131 rows, 128 comparable, 100 %, 3 abstained.**
   - `p_vesapm` / `p_lpt` / `p_plan12` — "PCem-blocked" since the programme began — are now
     tagged `ORACLE-ALSO` and being swept (result in the session log / memory).
   - ⛔ **NEVER `lldb -p` unattended** (memory `lldb-attach-wedges-the-mac`).
   - **14:20 CORRECTION — the "only boots when watched" stall was AMI's "D: drive failure —
     Press F1"** (the seeded CMOS described two disks; the user had been pressing F1 without
     saying so). `enable_sync` was NOT the cause; the earlier commit message is wrong on that.
     CMOS 0x12=F0/0x1A=0/0x24..2C=0, checksum redone; **unattended boot+run ≈ 60 s**. Lesson:
     when a run works only when someone is watching, ask what the watcher DID.
   - **`474f7b2e` (`1c8578b`+):** INT 10h AH=00 returns the video-mode FLAG in AL (20h/30h/3Fh —
     AMI ROM and SeaBIOS agree; we returned the mode); 4F0A and unknown 4Fxx return `AX=0100`
     (AL≠4Fh, "no such function" — both real BIOSes) not `014F`. **p_plan12 (PCem-blocked since
     GH #15), p_vesapm, p_lpt: clean vs real BIOSes. p_video: the `bda.crtc` row has the IBM VGA
     ROM's vote at last; 7 rows DISPUTED (IBM ROM vs SeaBIOS on cursor shape 0D0E/0607 and mode 7
     on a colour monitor — card facts, correctly ungraded).** Guards on `474f7b2e`: Doom, heaven7,
     Skyroads, QB, ZAR (capture A/B), Notepad — all clean. offvm 1424/0.

   **6. 15:00–16:00 — VESACUBE + HEXEN'S LOADER.**
   - **`demo\msdos\vesacube\VESACUBE.COM`** (`tools/vesacube/`, NASM real mode): menu of every
     banked VESA mode from 4F00/4F01; a SOLID cube — outward-wound faces, back-face culled on
     the rotated normal, 4-level flat shading, convex scan-line fill — page-flipped with
     `4F07 BL=80h` when two pages fit, `4F09` shade palette in 8 bpp, vsync on 3DAh with a tick
     fallback (rig: ~800 retrace edges / 930 flips in 12 s). Verified: QEMU screenshot, PCem
     ET4000 clean run, rig host-screenshot in 640x480x8 and 320x240x16. `VESACUBE 111` = 12 s
     headless. **By-hand owed: B, L, N, E, W.** (`scripts/bm/cubeshot.bat` drives the menu with
     rigshot; XP focus rules make it flaky.) My one bug: 4F03 returns the mode in BX, my record
     pointer. ⚠ The headless `capture.flag` shots came out BLACK for this guest while the
     framebuffer stats and a new unit check said the picture was there — instrument loose end.
   - **HEXEN'S HI-RES LOADER (`2df5651`):** `planar hi_water=0`. Hexen is a PM (DOS/4GW) guest,
     so no interpreter routes its stores; they land wherever A0000 is mapped. **chain4 was 1
     from reset and only the GUEST's SR4 write changed it** — a real BIOS writes SR4=06h for
     planar modes at the mode set; ours didn't, so every map-mask write took
     `mask_skip_chain4`, the window never left the linear section and the four planes were
     copied on top of each other. AND render_planar/the engine used `st->plane[]`, never the
     host sections. Fix: the mode set programs SR4/SR2 like the BIOS (chain-4 off planar / on
     13h) and repositions the window; `PL(st,p)` = the host's section when present, for the
     engine, BIOS pixel services, latches and renderer alike. **Hexen shows the logo, Raven/id
     marks and skull progress bar** (`runs/s74b_lazy32/shots_hexen/`). Guards on `a988c6e6`:
     Lemmings level screen (0Dh/10h via `lemhp.bat`), Doom, Skyroads, QB CAVE, Heretic (no 12h
     loader in this version). Rig `bin\` = `a988c6e6`; **zip still `eb466c56`.**

   **7. 17:10 — RELEASE.** User: *"tested all of the major apps and games with no significant
   regressions, and the hexen hires loader works"* on `a988c6e6`. `./scripts/package.sh` →
   `dist\ntvdmex-20260917-f3c349d.zip` (host md5 `a988c6e6`, commit `f3c349d`). On the rig from
   the package's OWN `bin\`: `pkgtest.bat` install → selftest **8/8** → uninstall → rig host
   restored; `pkgw16.bat` Notepad launches through the package host, rig host restored. Old
   `ntvdmex-20260916-08824e0.zip` (`eb466c56`) moved to `debug\prev\`; `debug\prev\
   ntvdmhost_prev.exe` = `a988c6e6` now; git tag `release-20260917`. **The zip is again
   IMMUTABLE until the next confirmed build.**

   **Score now:** see the s74b VBE table in the session log — **~82/100**, up from 60.
   Open on VESA: `4F0A` PM interface (clean decline; nobody on the shelf calls it),
   `4F15` DDC (decline), 1024×768 (presenter cap), and **still no oracle** (PCem needs
   its romsets found).

   ### ✅ s74 RESOLVED — **DOOM'S "4px COLUMNS" WAS `detaillevel 1` IN ITS OWN `default.cfg`. NOT OUR CODE.**

   > **It survived a rollback to `eb466c56` — which is the whole tell.** A defect that
   > outlives a binary swap is not in the binary. Doom's LOW DETAIL mode (F5 in-game)
   > halves horizontal resolution, drawing every column double-width; at 2x present
   > scale that is the ~4px stride the user saw. It had been written into
   > `demo\msdos\doom\default.cfg` when the game was quit, so it persisted across
   > every host build.

   **PROVEN, same binary `eb466c56`, only the guest's config changed:**

   | `default.cfg` | odd-column-boundary changes (3D view) |
   |---|---|
   | `detaillevel 1` (as found) | **0** — every column pair identical |
   | `detaillevel 0` (restored)  | **8279** — full 1px columns |

   Restored to `detaillevel 0` on the rig; original saved as
   `runs/s74_doom_regression/default.cfg.lowdetail.bak`. Status-bar labels
   ("AMMO/HEALTH/ARMS/ARMOR") are legible again, and were not before.

   ✅ **THE VESA BUILD WAS NEVER IMPLICATED.** Doom on `6d2f9af5` with detail restored:
   7756 odd-boundary changes — full 1px columns, same as the confirmed build.

   ⚠⚠ **MY OWN ERROR, RECORDED.** I declared a headless baseline "renders correctly"
   from a 320x200 thumbnail BY EYE. It was already low-detail; column doubling is
   invisible at that size. That false baseline is what made a phantom regression look
   real and sent me hunting my own VESA changes. **MEASURE THE PIXELS. An image small
   enough to eyeball is small enough to lie.** (Second wrong turn in the same hunt --
   see the mode-Y `fanouts` false lead below.)

   ▶ **h7 "did not work when I ran it" is EXPLAINED and still open:** heaven7 only gets
   past wall 2 with `cfg\nopmpatch.flag` present, and the headless harness deletes it
   after every run -- so a by-hand launch hits the eager INT-site patcher and dies.
   That knot is still untied.

   ### ⛔ s74 (superseded by the block above) — the investigation that got there

   > **The user ran Doom by hand on `6d2f9af5` and its raycaster drew ~4px-wide column
   > strides instead of 1px. Heretic and Hexen were fine. h7 also did not work by hand.**
   > Rig rolled back to the confirmed `eb466c56` immediately (standing rule: roll back
   > FIRST on "worse"), stable zip untouched, user's log in `runs/s74_doom_regression/`.
   > **The whole VESA branch (`96345f6`) is committed but MUST NOT go back on the rig
   > until this is understood.**

   **What is ruled OUT, by measurement:**
   - **The patcher.** Footprint is byte-identical between the good and broken runs —
     same regions, same counts (`0x17`, `3`, `0x24`, `0x45`), same rejects. Only the
     base addresses moved, because `video_state` grew.
   - **The EIP reconstruction (`bb5ae66`).** **Zero** reconstructions in Doom's log.
   - **Struct growth aliasing the mode-Y planes.** The plane views are independent
     `CreateFileMapping` sections, not buffers inside `video_state`.
   - **A stack blowup from the 2.09MB I added to `present_ddraw`.** Both instances
     (`main.c`, `present_demo`) are `static`. ⚠ But note the struct carries an explicit
     design constraint in its own comment — "stays something a caller can hold by
     value" — which 2.09MB violates in spirit. Worth undoing regardless.
   - **Headless Doom on the VESA build renders CORRECTLY** (`runs/s74_doom_regression/
     test/`), same launch shape as the good baseline (`good/`).

   ⚠⚠ **A FALSE LEAD, RECORDED SO NOBODY REPEATS IT.** The mode-Y counters looked
   damning — good run `fanouts=0x70` (112), broken `fanouts=0xdc61` (56,417), i.e.
   "more fanouts than swaps". **It is normal.** Re-running Doom on the CONFIRMED build
   gave `fanouts=0x44d44` (281,412) against `swaps=0x104e2`. The two logs being compared
   were different launch shapes and durations. ⇒ **An A/B between two runs you did not
   control is not an A/B.**

   ▶ **WHERE TO LOOK NEXT.** The screenshot path (`snap` -> BMP) is clean, so the defect
   is in the ON-SCREEN path that headless never exercises: `gdi_present` /
   `fs_stage`, which is exactly what changed to add the 32bpp snapshot. **Prime
   suspect:** `gdi_present` can now set `biBitCount = 32` (the `split || direct`
   branch). An 8bpp buffer described to `StretchDIBits` as 32bpp renders **exactly 4x
   too wide** — which is the reported signature precisely. NOT PROVEN: `direct` should
   be 0 for Doom (`snap_bpp` is assigned from `f->bpp` on every valid frame). Reproduce
   it before fixing it; a by-hand run with the per-present values logged would settle it.

   ### ★★★★★ s74 LATE — **HEAVEN7 RENDERS. VBE 2.0 DIRECT COLOUR + A LINEAR FRAMEBUFFER.** (HEAD `96345f6`, rig host `6d2f9af5` — UNCONFIRMED)

   **heaven7 draws its opening corridor ("we used to dream") and its "heaven seven"
   title, 640x480x24, 17,707 distinct colours, 874 presents over the full 30s
   deadline.** Captures in `runs/s74_vesa/shots/`. It was FOUR walls, three of them
   ours — see the blocks below for 1 and 2:

   3. **The lazy raw-INT path could not serve a flat 32-bit client** (`bb5ae66`) —
      NT's 16-bit exception frame truncates a flat client's EIP. Reconstructed from
      the client's own 0501 blocks, requiring a UNIQUE site holding `CD <vec>`.
      Serviced raw INTs 62 → 72; the demo reached its own code and printed its own
      `VESA error`.
   4. **VESA** (`96345f6`). The eleven VBE sub-functions were all present — what was
      missing was everything a guest FILTERS on: only three 8bpp modes, `MemoryModel`
      hardcoded to 4 (packed) for every mode, no RGB field layout, and **no linear
      framebuffer** (attribute bit 7 and `PhysBasePtr` both 0), with **DPMI 0800
      unimplemented** so the advertisement could not have been honoured anyway.
      Now: twelve modes incl. 15/16/24bpp, a correct ModeInfoBlock, the LFB aperture
      at `VID_VESA_LFB_PHYS`, DPMI 0800/0801, and direct colour converted to ARGB for
      a presenter that gained a 32bpp snapshot beside its 8bpp one.
      **The 8bpp path is byte-for-byte unchanged** — Doom/Heretic/Hexen/ZAR run through it.
      Measured: `BX=0x4112` (640x480x24, LFB) ACCEPTED, aperture `0xE0000000` size
      `0xE1000` = exactly 640*480*3.

   ⚠ **NOT DONE — the geometry.** The picture is a **320x176 rectangle at (160,272)**
   inside an otherwise correct 640x480 frame. x=160 is *exactly* `(640-320)/2`, so the
   demo is centring horizontally; y=272 is NOT `(480-176)/2`=152. It calls neither
   4F06 nor 4F07, so it is not panning. Unexplained — start here.

   ⚠ **heaven7 still needs `cfg\nopmpatch.flag`** to get past wall 2 (our eager
   INT-site patcher corrupting its generated tables). That knot is NOT untied.

   ⛔ **HEXEN'S LOADER SCREEN IS NOT VESA** — measured, zero `INT 10h AX=4Fxx` calls in
   an 89,636-line run from startup into gameplay. VBE work will not touch it; it needs
   its own investigation.

   ### ▶ START HERE: **SESSION 74 (below) — HERETIC RUNS, USER-CONFIRMED BY HAND. `eb466c56` IS THE CONFIRMED BUILD.** Then s73 evening, 72, 71, 70, 69, 68, 61, 60, 59.

   > **s74 close (2026-09-16, late). Rig host `eb466c56` (= `00c780e`'s binary) is
   > USER-CONFIRMED BY HAND: Doom, Heretic and Hexen all play.** It is now the rollback
   > copy (`debug\prev\ntvdmhost_eb466c56.exe`); the previous ones are
   > `ntvdmhost_877eb238.exe` and `ntvdmhost_prev.exe` = `a5cd764b`. The package on
   > the share is re-cut from this binary (`dist\ntvdmex-20260916-<sha>.zip`, VERSION.txt
   > names host md5 `eb466c56`). Deadline is END OF THE 17th.
   >
   > The user also reports Doom's wave audio is now **flawless** where it was slightly
   > glitchy before. ⚠ **Nothing on the audio path changed** — `23bd9ae..HEAD` touches
   > only `vdd_video.c` (VESA 4F00) and one log line in `main.c`; the only timing-adjacent
   > commit in the last 60 is `0d19439` (IRQ0 held in service, the 15th), which governs
   > the real-mode arm while Doom's DMX ISRs run under the PM arm. Unexplained; not
   > claimed. If it ever matters, an INTERLEAVED A/B against `debug\prev\ntvdmhost_prev.exe`
   > is the only honest measurement.

   ### ▶ s74 later — **HEAVEN7: TWO OF OUR BUGS BACK TO BACK — EXEC'S ENV COPY, THEN THE INT-SITE PATCHER.** (HEAD `a7e9de0`, rig host `90f41fb9` — UNCONFIRMED)

   The user asked why the heaven7 demo fails. Two things were wrong:

   1. **The share's `demo\msdos\heaven7\` held only `h7.EXE`** — no `DOS4GW.EXE`.
      h7.EXE is a DOS/4GW *stub* that loads an external extender; the folder was
      incomplete. Copied in the standard `DOS4GW.EXE` (the same one ZAR ships).
   2. **A real host bug, oracle-verified.** INT 21h AH=4Bh with env=0 ("inherit")
      must give the child a **COPY** of the parent's environment. We handed over
      the parent's block itself, and then `dos_psp_build` zeroed its first three
      bytes — so `COMSPEC=` became `PEC=C:\COMMAND.COM`, exactly where DOS appends
      the program name. DOS/4GW reads that slot to find what to load and died with
      `DOS/16M error [8]: cannot open file 'PEC=C:\COMMAND.COM'`. **Doom, Heretic,
      Hexen and ZAR never showed it because their extenders are bound into the game
      EXE — no EXEC, no copy.** Fixed `2a39000`: `dos_psp_build` no longer touches
      the env block; `exec_begin` allocates the child a copy (owned by its PSP,
      freed on exit); `exec_name` is captured verbatim from DS:DX. New relations in
      `p_child`/`p_exec` (copy, count word 0001, name tail/shape, parent intact):
      **5 rows, all MISMATCH before, all AGREE with 6.22 after.** offvm 1366/0.

   **Wall 2, found when the user tried it by hand and it still failed (`3d451cf`):
   OUR INT-SITE PATCHER, FOR THE FIFTH TIME.** The by-hand log died at exactly the
   same instruction as the headless one — a file close that succeeds, then silence,
   no STAGE2. The patcher had rewritten **7 "INT sites" inside the demo's own 0x3a000
   LE allocation**, vectors `0x41 0x11 0x1a 0x11 0x33 0x08 0x31` preceded by
   high-entropy bytes. **Nobody executes `int 08h` (IRQ0) or `int 41h`, least of all
   from 32-bit PM code** — it is DATA. heaven7 generates its tables at runtime and
   1 byte in 256 of noise is `0xCD`. The proof is in the same run: in DOS/4GW's own
   region every patched site is a real `b4 09 cd 21` / `b8 02 09 cd 31`. And the
   demo's region was scanned FIVE times — the first four found **zero** sites,
   because the content had not arrived yet.

   ⇒ **The patcher now logs the 16 bytes around every site it patches, before it
   patches them.** For five sessions the one thing it never recorded was what IT
   clobbered — and for a guest that GENERATES code they are not in the binary at all
   (the file has `0b c3` where we patched). `nopmpatch.flag` skips scanning; its
   contents are an optional hex minimum region SIZE, so DOS/4GW's ~0x5000 of dense
   real INT sites keep their patches while a 0x3a000 code+data+generated allocation
   is left alone. **A DIAGNOSTIC, NOT A FIX.**

   **MEASURED A/B:** with `nopmpatch.flag`=`20000` heaven7 **no longer dies silently
   — it reaches `STAGE2: complete`.** That confirms the patcher was killing it.

   **Wall 3 — ROOT-CAUSED, NOT FIXED (`a7e9de0`). THE TWO WALLS MEET, AND THAT IS THE
   WHOLE PROBLEM.** With the eager scan excluded from the demo's region, its own
   `int 31h` **sixteen bytes past the LE entry point** raises `#GP(IDT)` err=`0x018a`
   (= IDT | vector 0x31) and is reflected to DOS/4GW's `#GP` handler instead of being
   serviced — so the demo never gets its first DPMI call answered and never sets a
   mode. (The "cannot make transparent segment" string on the fault stack was a red
   herring: stack residue, not the cause.)

   The host has a **lazy** `#GP(IDT) → RAW INT → service + patch` arm, and it is the
   architecturally right mechanism: it patches only bytes the CPU **actually executed
   as an interrupt**, so it has *no false positives by construction*. It serviced
   **62** real INTs in the same run. It could not serve heaven7, for two stacked
   reasons:
   1. Its guard read `if (gcb && ...)` where `gcb = dpmi_sel_base()` → `g_ldt[].base`,
      which is **0 for exactly the descriptor a flat 32-bit client runs on** (base 0,
      limit 4 GB, D/B=1). It was declining those faults *by accident*, reading a
      legitimate base as "no selector".
   2. ⚠ **But fixing that predicate alone is WORSE than the bug** — measured before it
      shipped. **NT hands us a 16-bit exception frame whatever the client is**, so a
      flat client's EIP (which *is* its linear address, the base being 0) arrives
      **truncated to 16 bits**: heaven7 reports `0x231c` for an instruction that lives
      at `~0x0433231c`. `gcb + fr[3]` then names **low memory**, and a chance `CD nn`
      match there would write `C4 C4` into an innocent page — the patcher's own
      failure mode, relocated. **That build was thrown away, not shipped.**

   ⇒ `a7e9de0` declines on the **real** criterion (the frame is trustworthy only for a
   16-bit faulting CS, which is what all 62 serviced faults were) and **logs the reason
   once, naming the limitation**. A 16-bit CS with base 0 is now serviced correctly.

   **So heaven7 needs the eager scan for its flat 32-bit `int 31h` sites — and the
   eager scan is exactly what corrupts its generated tables.** That is the knot.
   Either the heuristic gets smarter, or the lazy path gets a frame wide enough to
   locate a flat client's instruction. **heaven7 moved two walls in one session and is
   still not playable.**

   ⚠ **The patcher heuristic itself was NOT changed** — Doom/Heretic/Hexen/ZAR are
   confirmed on it and the release is imminent. The tightening (e.g. never patching
   the hardware IRQ vectors 0x08-0x0F, which no application executes) is written
   down, not done.

   (`dosdiff`'s rig adapter was fixed to the s73 layout in passing — it had gone
   stale and only said so by "no disputes" off one host.)

   ⚠ **RIG IS ON `f5b86403`** (= `ddedb494` + the patcher's byte logging and the
   inert `nopmpatch.flag`), **displacing the confirmed `eb466c56`** (rollback copy
   `debug\prev\ntvdmhost_eb466c56.exe`). The EXEC-env change is a strict superset —
   it only touches the inherit-the-env path, which Doom/Heretic/Hexen/ZAR do not use —
   and it is oracle-clean + offvm-green, but it is **not user-confirmed by hand.**
   The package/USB is still `eb466c56` until it is.

   ### ★★★★★ s74 — **HERETIC RUNS: OUR VESA 4F00 HANDLER WROTE PAST THE CALLER'S 256-BYTE BLOCK AND OVER THE NEXT MCB.** (HEAD `00c780e`, rig host `eb466c56`)

   **The find took one log read, not a run.** The s73 chain dump said the MCB at
   `0x25f4` was corrupt between "alloc 16 paras" and "free it". The DPMI trace shows
   what sits between those two lines — exactly one thing:

   ```
   INT31h AX=0100 BX=0010 -> DOSmem seg=0x25e4        (256 bytes)
   RMCS 0300 int=0x10 ... eax=0x00004f00               (VBE: get controller info, ES:DI = that block)
   INT31h AX=0101 BX=0347 -> DOSfree
   ```

   `vdd_video.c`'s 4F00 arm wrote the OEM string at **+0x100** and the mode list at
   **+0x120** of the caller's buffer, unconditionally. `0x25e4:0x100` **is** `0x25f4:0`
   — the MCB. Its signature became `'N'` (of `"NTVDMEX VESA"`), the walk stopped, and
   the 480 KB above went invisible. VBE 2.0 §4.3: the block is **256 bytes unless the
   caller preset `"VBE2"`**, and the OEM string / mode list belong in the reserved area
   at +34. Hypothesis 3 from the s73 block ("something of ours wrote there") — and it
   was not a patcher or a probe, it was a BIOS service answering with more than it was
   asked for. Doom never probes VESA; Heretic does; that is the whole difference.

   **Fixed (`00c780e`):** OEM string at +0x22, mode list at +0x40, 512-byte form and
   2.0-only fields only when `"VBE2"` was preset. `video_test` now poisons 256..511 and
   checks nothing lands there (fails 3 checks on the old handler). The ENOMEM chain
   dump now also prints the 16 bytes at the MCB where the walk stopped.

   **Measured on the rig, headless (`runs/s74_heretic/`):** the 64-para allocation
   succeeds at `0x25e4`; Heretic sets mode 13h, shows the title screen and **plays its
   E1M1 demo** — HUD, weapon, "ethereal arrows" pickup message, SB streaming (`sb_blocks=
   0xaa4`), ~2000 INT 33h polls — until the headless deadline (`heretic_shot01/03.png`).
   Doom on the same host: mode 13h, SB streaming, deadline — unchanged. offvm **1366/0**.

   ~~▶ **OWED: a by-hand Heretic run**~~ **Done — the user ran Doom, Heretic and Hexen by
   hand on `eb466c56` and all three play.** (The Hexen glance was owed because its VESA
   probe, if any, goes through the same handler; it passed.)

   > **s73 close (2026-09-16, ~19:00). HEAD `23bd9ae`, rig host `877eb238` = HEAD.
   > Package `dist\ntvdmex-20260916-23bd9ae.zip` on the share, fresh-folder verified 8/8.
   > USER-CONFIRMED BY HAND TODAY: Doom, Hexen, Zar, Wolf3d all play; QB builds a
   > runnable standalone EXE (`MKEXE.BAT`). Deadline is END OF THE 17th, not the 18th.**
   >
   > **Open, in the user's priority order:** (1) **HERETIC** — see the evening block;
   > everything needed is written down and the loop is headless. (2) Win16 polish: the X
   > does not close WinMine/Charmap (app-specific — Notepad closes cleanly; `w16close.bat`
   > tests it headlessly), Calc/Charmap/Clock draw incorrectly. (3) Bubbles palette,
   > Matrix_1 slow. MPLAYER GPFs at `0001:3983` (MCI), not chased.

   ---

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

   ## ★★★★★ SESSION 72 (2026-09-15, afternoon) — **THE BY-HAND PASS: THE PACKAGE WORKS ON A FRESH FOLDER, AND DOOM'S E1M1 CRASH IS NOW REPRODUCIBLE WITH THREE SUSPECTS DEAD.**

   **HEAD `9ad5eff`, pushed. Battery 1316/0. RIG = `70351a20…` = USER-CONFIRMED, and
   it is now also `bm\ntvdmhost_prev.exe` (the rollback); `bm\ntvdmhost_lemok.exe`
   keeps `bf9534a9` (f79d954, Lemmings). Package `dist/ntvdmex-20260915-9ad5eff.zip`.**

   ### ✅ USER-CONFIRMED BY HAND
   * **THE 18th DELIVERABLE WORKS.** From a fresh Desktop folder: `status` → 2
     (another program owns the VDM), `install.bat`, `status` → 0, `smoke.bat` →
     **eight PASS lines and ALL TESTS PASSED**, `uninstall.bat`, the rig's own host
     back in charge. This had **never been run by a human** before today.
   * QBasic: welcome box and "Untitled" banner now draw, **mouse clicks navigate the
     cursor**, Alt/File menus open by mouse, pointer is an inverted cell.
   * Mouse capture: exclusive to the guest, **Win** releases it, clicking recaptures.
     (One bad capture on the first launch after a deploy, **not reproduced** — the
     known "a first run after a deploy is not evidence" pattern.)
   * **Mario now PLAYS** (it used to die on the intro). ⚠ Cause UNKNOWN — the s72
     simInt guard did **not** do it (`simint_rm=0`, and Mario makes no DPMI calls).
     Graphics leave artefacts behind moving objects: a mode-Y planar issue, open.
   * `DATE$`/`TIME$` correct in a guest — the INT 1Ah RTC half landing.

   ### ✅ BOTH BUGS THE PASS FOUND ARE NOW CLOSED (user-confirmed)
   * **QB's Open dialog navigates by mouse, lists files, opens and runs programs.**
     `INT 21h AH=29h` stored `*` literally where DOS expands it into `?`s: QB parses
     the pattern into an FCB and matches every directory entry against it, so a literal
     star matched nothing -- empty Files pane, correct Dirs pane. Oracle-pinned, fixed
     in `fcb_put_name` (`0dbe737`). ⛔ I guessed the FCB SEARCH twice; QB enumerates
     with `AH=4Eh/4Fh`, which one trace line showed.
   * **Dialog labels stopped losing letters.** `Files`->`iles` was QB's accelerator
     characters BLINKING: Blink Enable is attribute-controller register 0x10 bit 3 and
     we kept a private flag only the BIOS call could move (`48d7a67`).
   * ★★ **The instrument that ended it: `cfg\textdump.flag`** -- the text screen as the
     GUEST wrote it, beside each screenshot. It showed the pane empty IN THE BUFFER,
     clearing the search and the renderer at once, after three wrong guesses from
     pixels. ▶ When something is missing from a text screen, dump the CELLS first.
   * ⛔ **NEW, user-reported, not investigated:** making EXEs from QBasic (the
     BC.EXE/LINK.EXE path) does not work properly. Filed at the user's request.

   ### ⛔ THE OPEN BUG THE PASS FOUND
   * **QB File > Open lists NO FILES** (Dirs/Drives is correct). QB parses `*.BAS`
     with AH=29h into an FCB and searches with **AH=11h/12h**, not AH=4Eh. ⚠ I saw
     the empty pane in my own headless screenshots this morning and explained it away
     as a Tab-order quirk — it is a real bug.
   * ~~**Doom dies ~1s after killing the imp on the ledge / the far zombie** on E1M1~~
     — ✅ **SOLVED AND USER-CONFIRMED, s72 evening.** It was **our own INT-site
     patcher corrupting a jump table it mistook for code.** See the block below;
     everything under "in what is now known rather than guessed" is superseded as a
     *conclusion* but kept because the eliminations were all correct.

   ### The Doom crash, in what is now known rather than guessed
   No `HOSTFAULT`, `veh{any=0 fatal=0}`, **no Application Error in XP's event log and
   no Dr Watson log**, and **not one line of the shutdown path**. The process is
   terminated outright — NT killing a VDM whose state it will not accept.
   Eliminated **by measurement**, one user run each:
   | suspect | how | result |
   |---|---|---|
   | sound / SB / IRQ5 | `cfg\nosb.flag` | crashed, **0** IRQ5 deliveries (was 521) |
   | mouse + DPMI real-mode simulation | `cfg\nomouse.flag` | crashed, **0** `simInt 0x33` (was ~1389) |
   | async injection racing the mode switch | `g_simint_busy` guard | **`simint_rm=0`** — never hit |

   ⛔⛔ **A METHOD TRAP, MINE:** three crashes ended on the same two `simInt 0x33`
   lines and I built a theory on it. It was **coincidence** — `simInt` was simply the
   most frequent line in the log. ▶ Before calling "it always ends on X" a
   fingerprint, ask what share of all lines X is.
   ⛔ **AND I ASKED FOR A RUN THAT COULD NOT ANSWER:** the counter I needed was
   printed only in the exit report, which a killed guest never writes. Counters that
   bear on a crash now ride the PM heartbeat.
   ▶ ~~**NEXT:** the DPMI/PM IRQ0 arm~~ — **wrong suspect; see below.** The IRQ0 arm
   still auto-EOIs and selector `0x317` (base `0x041A0000`, limit `0x19`) is still
   **refused by NT twice and never installed in the LDT**; both remain open defects
   on their own account, but neither was this crash.

   ### ✅ THE ANSWER (s72 evening, `6a2174a`) — A JUMP TABLE IS DATA, EVEN INSIDE A CODE OBJECT

   **The instrument that ended it: `bm\vdmwatch.exe`** (`scripts/bm/vdmwatch.c`, built by
   `scripts/build-vdmwatch.sh`) — a tiny attach-and-log debugger. Nothing we own runs
   after the kernel gives up on a process, but `KiDispatchException` forwards every
   exception to the **debug port first**, before the user-stack write that fails here.
   It answers everything `DBG_EXCEPTION_NOT_HANDLED` (the no-debugger path) and logs the
   code, address, register file, CS/SS descriptors, code+stack bytes and the exit code.

   It caught the kill exactly: **`ACCESS_VIOLATION` whose fault address IS the EIP** —
   `cs:eip=02bf:04c4c4fa`, an unmapped page — first chance, second chance, exit
   `0xC0000005`, `veh{any=0}` to the end. `scripts/doomstack.py` then walked the core it
   dumps: the guest `jmp`'d through a **near-pointer table at Doom obj1+`0x2cc6c`** and
   landed on entry[1] = `0x04c4c4fa`.

   **That value is `0x0416cdfa` with its middle two bytes overwritten by `C4 C4`** — the
   BOP our INT-site patcher writes. Three of the table's five entries point into
   obj1+`0x2cdXX`; little-endian that is `XX cd 16 04`, so the middle pair reads as
   `CD 16` = INT 16h, a **serviced** vector, and `x86_int_site_is_real()` passes because a
   table of code pointers decodes into plausible instruction streams. The guest writes the
   table **after** the first scan, so the **second** scan corrupts it — DOS/4GW re-declares
   its code selector on every file load (`AH=0009`/`000c`), re-running
   `dpmi_patch_code_region` over the same range. `pmap_get` stops a site being patched
   twice but **not a new candidate that only appeared once data was written.**

   This is the **fourth** time this patcher has rewritten non-code — ZAR's call
   displacement, `R_InitTextureMapping`'s `jle` displacement and the FP range 34h..3Fh are
   the other three, all documented at the call site — and the first found by measurement
   rather than a hunt.

   **The fix:** a `cd nn` candidate lying inside an **aligned dword that points back into
   the region being scanned** is a jump/call table entry, not two instructions — skip it.
   Safe by the same argument as every arm beside it: a genuinely raw INT so aligned is
   still serviced out of the `#GP`.

   **★★★★★ USER-CONFIRMED: "Doom survived the crash!"** on build `a5cd764b`, which is now
   the rig baseline (`bm\ntvdmhost_prev.exe` = the previous confirmed `c91b521e`).
   Regression-clean: **ZAR** (the patcher's heaviest user) shows byte-identical patch
   counts with the guard firing **0** times; **Skyroads** `max_ms=0x14`.

   ⚠ **Doom's repro is BY-HAND ONLY** — headless can't even load the WAD (the target path
   is mangled to `GAMESDOOME`, the known path-specific DOS/4GW blocker), so it never
   reaches the second scan.

   ⛔ **AND THE GUARD WAS SILENT IN THE RUN THAT PROVED IT** (`f036ba5`): its log line
   shared the `rej++ < 16` cap, and that scan rejects **334** byte pairs before reaching
   the table. What actually proved it was differencing two runs' counters —
   `patched 3, rejected 0x14e` became `patched 0, rejected 0x151`, and `0x14e + 3 = 0x151`.
   The guard now has its own counter, always printed in the scan line. *An absence in the
   report means nothing unless the report says what it left out* — a fresh instance of
   this project's most repeated lesson.

   ---

   ## ⏸⏸ SESSION 72 CLOSE (2026-09-15 evening) — **READ THIS BEFORE ANYTHING ELSE**

   ### ⚠ A BY-HAND PASS IS OWED AND THE DEADLINE IS THE 17th (i.e. TOMORROW)

   **The package is built and headless-verified; it has not been in front of a human.**

   * **`dist/ntvdmex-20260915-5a72811.zip`** — host `ffdea07a`, HEAD `5a72811`.
     Verified by `bm\pkgtest.bat` from a fresh folder: `/status` correctly saw another
     program's Debugger value → `/install` → **selftest 8/8 THROUGH the package host**
     → `/uninstall` → the rig's own routing restored. The previous package was
     **23 commits stale** — it predates the Doom fix AND the confirmed QB fixes.
   * ⛔ **The rig still runs `a5cd764b`** (the user-confirmed Doom build), so **four
     host changes have never been seen by a human**:

     | change | commit | risk |
     |---|---|---|
     | **The DOS SFT** | `9ca0437` | **Moves conventional memory −7.4 KB for EVERY DOS guest.** Biggest risk. |
     | The HMA | `7bf65e8` | New guest-visible memory at `FFFF:0010`. |
     | `AH=3Dh` error codes | `b580c4d` | Programs branch on these. |
     | Jump-table guard logging | `f036ba5` | Cosmetic, rides along. |

   * ▶ **Ask the user for three things (~10 min):** (1) **Doom E1M1, kill the distant
     imp** — the regression check that matters most, because the SFT and HMA both moved
     memory under the fix confirmed this afternoon; (2) **QBasic File > Open, navigate,
     run** — the memory map moved beneath it; (3) **one memory-tight guest** (Duke3D is
     the known one), which is where 7.4 KB would show.
   * Rollback: ~~`bm\ntvdmhost_prev.exe` = `c91b521e`~~ **s73: `debug\prev\ntvdmhost_prev.exe`
     = `a5cd764b` (the confirmed build); `c91b521e` is `debug\prev\ntvdmhost_c91b521e.exe`.**

   ### ⚠ RIG STATE THAT MUST NOT BE LOST
   `cfg\FLOPPY.IMG` is now on the share and **must stay** — without it `p_disk` goes
   back to measuring an absent drive. Rebuild with `./scripts/mkfloppy.sh`. It backs
   INT 13h/25h only; DOS file I/O on A: still goes through Win32.

   ### The three scores, all MEASURED (do not quote, re-run)
   ```
   ./scripts/paritysweep.sh   36 probes · 532 rows · PARITY 97.9% (459/469 comparable)
                              99.6% excluding the 8 rows with NO valid oracle
                              ⇒ only 2 gradeable rows remain open
   ./scripts/offvm.sh         30 tests · 1361 checks · 0 failed
   tools/score/score.py       86.9%  (DOS 89.7 / WOW 86.3 / product 76.8)
   ```
   ⚠ The overall score did **not** move today, and that is correct: it scores attested
   capability items, not probe rows. Nudging one because something looks done is how it
   becomes a vanity metric — its own header says so.

   ### What closed today
   1. ★★★★★ **Doom's E1M1 kill crash — USER-CONFIRMED** (`412624a`). Six sessions of
      silent VDM deaths were **our own INT-site patcher corrupting a jump table on a
      re-scan**. Found by attaching a debugger and reading the dead guest's core.
   2. `AH=3Dh` answered "file not found" for **every** failure (`b580c4d`).
   3. A DOS guest had **no SFT** — and `SysVars+4 = 0` is an SFT at *segment 0*, so a
      walker reads the IVT (`9ca0437`).
   4. The **HMA** — NT had mapped it all along; we were refusing it (`7bf65e8`).
   5. **INT 13h** — never unimplemented, it simply had no disk (`a662c58`).

   ### New tooling, all reusable
   | tool | what |
   |---|---|
   | `bm\vdmwatch.exe` | Attach-and-log debugger: sees the exception the kernel refuses to deliver, dumps a core. **The thing that cracked Doom.** |
   | `scripts/doomstack.py` | Walks a vdmwatch core against the LE object map. |
   | `scripts/offvm.sh` | The whole off-VM battery in one command (was 30 hand-compiles). |
   | `scripts/paritysweep.sh` | The whole oracle tier in one command. |
   | `scripts/mkfloppy.sh` | Builds `cfg\FLOPPY.IMG` for INT 13h. |

   ### Still open
   * ⛔ **QBasic cannot build EXEs** (BC/LINK) — user-reported, uninvestigated. Today
     **eliminated the obvious suspect**: `p_exec` and `p_ovl` are clean, so EXEC,
     load-without-execute and overlay relocation all match DOS. It is elsewhere.
   * 2 gradeable parity rows: `p_tsr` paras-still-held; `xms.08` **BH** (undefined by
     the XMS spec — left red and undecided rather than matched by invention).
   * 8 rows **blocked on PCem** (`p_lpt`, `p_plan12`, `p_vesapm`): SeaBIOS is a rewrite
     and there is no DOS ground truth for a VESA BIOS. **Do not fix toward them.**
   * Older: Mario mode-Y artefacts, Heretic load screen, Hexen hi-res, Duke3D memory.

   ### Method lessons worth more than the fixes
   * ⛔ **A one-host run is not a pass** — `p_drv` printed "no disputes" from an
     oracle-only run and was really 27 mismatches.
   * ⛔ **Before calling a surface unimplemented, check it has something to work on**
     (INT 13h).
   * ⛔ **An all-AGREE probe is not a verified surface** — several emit fewer functions
     than their headers claim, and many rows compare only `CF`.
   * ⛔ **A guard's own log line must never share a budget with what it guards against**
     — the jump-table guard logged nothing in the very run that proved it works.
   * ⛔ **Five false positives**, two within a commit of "fixing" correct code: a probe
     naming *"the default drive"* or **Z:** compares two **machines**, not two
     implementations. Fix it by giving the probe the **relation**, not just an abstention.
   * ★ `ERROR_INVALID_ADDRESS` means *"occupied"*, not *"denied"* — **query before you
     allocate** (the HMA).

   ---

   ## ★★★ SESSION 72 (2026-09-15, morning) — **QBASIC'S "FILE SYSTEM PROBLEMS" WERE THE DRIVE LIST: A DRIVE WITH NO MEDIA MUST STILL BE SELECTABLE, AND CHDIR NEVER MOVES THE CURRENT DRIVE.**

   **HEAD `a9b6c51`. Battery green (1302/0). Candidate `build/ntvdmhost.exe` = `86bb80b1…`,
   verified headless on the rig and then TAKEN OFF AGAIN: the rig is at the mouse-confirmed
   `7e9080bb…` (`c896300`), rollback `bm\ntvdmhost_prev.exe` = `bf9534a9…`. Deadline: the 17th.**

   The user's round-5 verdict on QB.EXE was "lots of problems, mostly around the file
   system". Driven headless (`scripts/bm/qbopen.bat`, `dostrace.flag`, screenshots), the
   Open dialog listed the directory correctly and `[-C-]` alone under Dirs/Drives, on a
   machine with A:, C:, D: and Z:. QB sizes that list by the classic probe -- for each
   letter `0Eh`, `19h`, compare, `0Eh` back -- and our `0Eh` selected a drive only when
   `SetCurrentDirectoryA("X:")` succeeded, which an empty floppy or CD-ROM drive refuses
   with NOT READY. Asked the oracle first (`tools/dostest/p_drv.asm`, MS-DOS 6.22):

   * `0Eh` selects any letter with a device behind it from the CDS, without touching the
     media -- the phantom B: on a one-floppy machine reads back through `19h`. Only a
     letter with no device (D: under LASTDRIVE=E, Z:) is refused, silently.
   * `3Bh C:\ZZDRV` issued from A: leaves `19h` at A:, and `47h` for C: then answers
     `ZZDRV`: every drive keeps its own directory. Ours moved the process (and so the
     current drive) to C:. A bare `3Bh "C:"` is path-not-found (3).
   * ⚠ `47h` or `3Bh` on the phantom drive PROMPTS "Insert diskette for drive B:" and
     hangs the oracle run. Two probe revisions learned that.

   Fix (`src/dos/dos_int21.c`): `m->vdrive` holds a drive that exists but cannot be
   entered; `dos_cur_drive()` answers `19h`/`47h`/`36h`/`1Bh`/FCB/IOCTL for it, and
   `v86_path()` prefixes every relative path with it so an access fails ON that drive
   the way DOS's would. `3Bh` on another drive only sets that drive's `=X:` variable
   (what `"X:"` resolves through; `SetCurrentDirectory` does not maintain it, so
   `C:` → `D:` → `C:` now also returns to the directory it left, not the root).

   **Rig, headless, on the candidate:** `p_drv` selects A: / D: / Z:, refuses B:, keeps
   the drive across a cross-drive chdir. QB.EXE: Dirs/Drives shows `[-A-] [-C-] [-D-]
   [-Z-]`; `PERSONAL`↵ enters and relists; `BLIT.BAS`↵ opens (its `.MAK` miss is QB
   looking for a make file -- normal, and the "CMAK" of the old log); `D:\`↵ on the
   empty CD-ROM is error 3, Esc, clean exit; Save As writes the file. Skyroads on
   baseline (`n8=0x66 max_ms=0x13`).

   **▶ NEXT (needs the user):** deploy the candidate on their go (`build/ntvdmhost.exe`;
   the confirmed build is already the rollback plan) and re-run the QB
   pass by hand: File > Open shows four drives, a `.BAS` opens, Save As saves. Then
   edit.com. ⚠ The Doom regression the user reported on the 14th is still deferred, and
   its log is gone -- my QB runs delete `out\ntvdmhost.log`; copy a user's log to `runs/`
   before queueing anything.

   ### ★★★★★ s72, afternoon — **THE PACKAGE SMOKE TEST WAS RUNNING THE STUB, NOT THE SELF-TEST: A DOS LAUNCH FROM cmd/BATCH NOW RUNS THE REAL PROGRAM.** (HEAD `11e4a13`, host `caff9e08…`.)

   Running the fresh-folder package install on the rig (`scripts/bm/pkgtest.bat`,
   what the friend's machine does on the 18th) exposed that `smoke.bat`'s
   `selftest.com` reported "File I/O FAIL=21" -- on a host that had just opened the
   file it was launched from. Two bugs, both on the path that has always mattered
   for the 17th and was never exercised, because every rig run goes through
   `dosstub.com` + `target.txt`:

   1. **We ran the wrong program.** The first `GetNextVDMCommand`
      (`VDM_GET_FIRST_COMMAND`) fills only the console Title and CurDirectory.
      Explorer puts the program's path in the Title -- the sole reason a
      double-click has ever worked -- but a launch from `cmd.exe`, a batch file, or
      `smoke.bat` gives `title=[]` (via `start`) or the typed command WITH its
      arguments (direct). So we loaded the embedded 4-byte `mov ah,4Ch/int 21h` stub
      and reported a clean exit; the packaged smoke test "passed" having run nothing.
      Stock ntvdm consumes the real command in its exec-BOP path with a SECOND fetch,
      `VDM_FLAG_DOS` (`reverse/ntvdm.exe` 0xf04ed86 / 0xf00ac1e). We now do the same:
      AppName comes back as the program's full path, CmdLine as its tail, and that
      wins over the title heuristics and target.txt. The harness `dosstub.com` is
      recognised by name so target.txt still names the game there; a WOW launch
      (first fetch FALSE, err 0x57) is untouched.
   2. **We never told CSRSS the task ended, so a second program in the same window
      never ran.** On exit we now report the errorlevel (`GetNextVDMCommand` with
      ExitCode) and `ExitVDM`. That report blocks for the console's next command
      (stock ntvdm's resident idle state), so it runs on a helper thread while the
      main thread ExitVDMs; and because that call is itself handed the queued
      follow-up command, we relaunch it (`CreateProcess`, same console) in a fresh
      host. Measured: `selftest.com` run twice in one `cmd` window now runs twice.

   `selftest.asm`: the File I/O test named `C:\ntvdmex\ST$.TMP` -- the first rig
   layout, on nobody's machine now -- made relative and deleted after.

   **Rig, headless (candidate `caff9e08…`), all verified, then TAKEN BACK OFF:**
   `selftest` 8/8 under a bare `selftest.com`, a redirected run, a `start /wait`, and
   a second run in the same window; the fresh-folder package install (differently
   named dir) → smoke 8/8 → uninstall restores → the rig's own host back; the
   target.txt game harness still loads Skyroads (timing baseline `max_ms=0x13`).
   `smoke.bat` now fails on a `FAIL=` line or a missing `ALL TESTS PASSED`, not only
   on a crash. Package `dist/ntvdmex-20260915-11e4a13.zip` carries the fix. **Rig is
   the mouse-confirmed `7e9080bb…`; candidate `build/ntvdmhost.exe` = `caff9e08…`
   holds the QB drive fix + this, awaiting one by-hand deploy.**

   ---

   ## ★★★★ SESSION 71 (2026-09-14, morning) — **THE TEXT-MODE APPLICATION CLASS: QBASIC'S THREE SYMPTOMS WERE SIX HOST DEFECTS, ALL OFF-VM TESTABLE.**

   **HEAD `b7ccd6e`, NOT pushed. Battery green (input 60, video 162; full suite green).
   ★ DEPLOYED with the user's go: rig = `836e9440…`; rollback `bm\ntvdmhost_prev.exe` =
   the confirmed `f79d954` (`bf9534a9…`). Awaiting the user's QB re-test. Deadline: the
   17th — three days.**

   **Midday addendum — the user's "still not working" was measured on the OLD build
   (never deployed), and its log named two more defects, now fixed in `b7ccd6e`:**
   7. **IRQ1 stayed in service after the first key** (`keyirq=1` across ~19 presses). QB's
      hook EOIs only the keys it swallows and chains to the BIOS for the rest, leaving the
      EOI to the BIOS handler; our INT 09h BOP arm never sent one. It does now, exactly as
      the INT 08h arm EOIs IRQ0.
   8. **The INT 33h event handler (0Ch) is now CALLED** (`mouse_cb_try`; return stub BOP
      0x35 at `DOS_HDLR_SEG:005C`; context saved host-side; one in flight; 2 s timeout;
      PM handlers counted as `cb_pm`, not called). QB called 03h twenty times in a whole
      run — Microsoft's text UIs take all their mouse input through the callback.
   ⚠ Also seen: old-build QB detected CGA (zero BDA video bytes) and sat in its
   snow-avoidance `cli`/`3DA`-per-character loop. ⚠ `irq0_inj=` counts only exec-loop
   deliveries; the async path delivered the rest (`gap_ms[]` showed a healthy 18 Hz) —
   do not read a low `irq0=` as a stalled timer.

   The user's report on QB.EXE 4.5 (`demos/qb45` on the share): a graphical mouse
   pointer drawn over a text screen, no menu opened, and nothing could be typed. None
   of it was QBasic's. Each was read from the code (and the guest binary) and pinned as
   a battery check before the fix:

   1. **Typing (`vdd_input.c`)** — QB's INT 09h hook (`1DDB1h`) does `in al,60h`, looks
      at the byte, and for every ordinary key chains to the BIOS via `int 0EFh` (the
      saved vector). On an 8042 the BIOS's own `in al,60h` reads the SAME byte again;
      our FIFO had popped it on the hook's read, so the BIOS arm found the FIFO empty,
      translated nothing, and the ring at 0040:001E stayed empty. Now the BIOS arm
      serves that byte once from `sc_last` (`sc_bios_owed`; STAGE2 `owed=`), superseded
      by any newer byte. Every INT 09h hook that peeks the port and chains — Turbo
      Pascal's CRT unit, most TSRs — had the same gap. Input battery T9.
   2. **Alt menus (`vdd_input.c`)** — the translation never consulted Alt: Alt+F arrived
      as `AH=21 AL='f'` where the BIOS stores `2100h`, so every editor's accelerator
      typed a letter. Replaced the two ASCII columns with the IBM four-column table
      (plain/Shift/Ctrl/Alt, F-keys incl. F11/F12, Ctrl+arrows `7300h` etc., enhanced
      Alt+grey codes); CapsLock inverts Shift for letters only, NumLock for the keypad
      only. Input battery T10. Also 0040:0096 bit 4 (enhanced keyboard present) is set.
   3. **The pointer (`main.c` present path + `vdd_video_text_cursor`)** — in a text mode
      the driver has no pixels; it rewrites the ATTRIBUTE of the cell under the pointer
      (INT 33h 0Ah masks, defaults `77FFh`/`7700h`). The host stamped its 16x16 arrow into
      the text frame regardless. Now text modes redraw the cell through the masks (0Ah
      BX=0 stores them; BX=1 falls back to the default and is counted).
   4. **Mouse menus (`main.c` INT 33h)** — the driver's text screen is 640x200 whatever
      the font, so apps do `row = DX/8`. We returned the 400-line frame row: every row
      doubled, a menu-bar click landed two rows down. `i33_vy/i33_py` scale Y in text
      modes (03h/04h/05h/06h/08h).
   5. **Text rendering (`vdd_video.c`)** — attribute bit 7 was always masked off (no
      bright backgrounds after `1003h BL=0`, no blink with it on); `1112h` — THE 50-line
      call — cleared the user font and changed nothing else (cell height is now
      per-state: 8/14/16, rows = 400/cell_h, cursor emulation and INT 43h answer follow
      it; `1111h`/`1114h`/`1x` user fonts likewise); CRTC `0A/0B/0E/0F` (cursor
      shape/address, how every CRT unit moves the cursor) fell into `default:`; the BDA
      display fields 0449..0489 were never written (rows-1 at 0040:0084 read 0 — a
      one-row screen to anything that sizes itself from it); 40-column text was drawn
      at stride 640 into a 320-wide frame. Video battery T21 a–f.
   6. **Alt held forever (`main.c`)** — Windows delivers a key's UP to whichever window
      has focus when it is released, so Alt+Tab away left the guest with Alt down.
      `WM_KILLFOCUS` now releases the modifiers we pushed.

   **Evening addendum — THE 17th IS A PORTABLE ZIP (see `docs/PLAN-17th.md`).**
   Done today: `NTVDMEX_DIR` is derived from the host's own path (parent of `bm\`;
   rig-verified: `STAGE0: root=[...\ntvdmex\]`), `package/` holds install/uninstall/
   status/smoke `.bat` + README, `scripts/package.sh` builds `dist/ntvdmex-<date>-<sha>.zip`
   (host + selftest.com + empty cfg\ out\; NO Win16 system files -- the WOW half uses
   XP's own from system32). A copy sits on the share at `dist\` for the by-hand
   fresh-folder install test. Also today: mouse callbacks delivered at our stubs, the
   return stub moved 0x5C->0x12 (0x5C was `DPMI_RAW2PM_OFF`, planted later -- "DOS
   terminate on the first mouse move"), 8042 transfer hold (900 us), Win key never
   forwarded, LASTDRIVE=26 + CDS in a top-of-chain block + real AH=0Eh/19h. PCem 17
   (macOS app + source) is under `./pcem` (gitignored) for the BIOS/VGA oracle.
   Rig = `b85a8a53…`; rollback = `bf9534a9…` (f79d954). Branch pushed.

   **▶ NEXT (needs the user):** deploy on their go, then by hand in QB.EXE: type in the
   edit window; Alt then F opens File; click File in the menu bar; the pointer is an
   inverted cell; Options > Display for 50 lines (`1112h`). Then edit.com (XP's
   `system32\edit.com` — a different code base, 43/50-line and mouse paths both used).
   ⚠ Re-run Skyroads after (shared keyboard path, standing rule). Then the same
   read-the-binary pass over the next text-mode guest rather than another game.

   ---

   ## ★★★★★ SESSION 70 (2026-09-13, evening) — **LEMMINGS #3 CLOSED FOR REAL: THE TIMER RESTARTS PER THE DATASHEET, AND IRQ0 IS HELD IN SERVICE.**

   **HEAD `f6a2080`, committed, NOT pushed. Battery 1238/0 (pit 35). Rig binary = HEAD
   (`09a42b5a…`). Deadline: the 17th.**

   The s69 "fader-vs-timer coupling" was never a fader problem. Read from the guest
   binary + the 8254 datasheet, then measured: Lemmings' HP-mode timer ISR (`CS:17C7`)
   does `sti`, spins on `0x3DA` for the retrace, then `out 43h,36h` + the calibrated
   count. Its main loop runs one game frame per 5 ticks and steps the level fade one
   step per game frame — it never waits on the retrace in HP mode (`0x31E`). So the
   design is a 70 Hz retrace-locked tick, and two host bugs hid each other:

   1. **PIT (`vdd_pit.c`)**: read-back was the free-running phase → random calibration
      reload (the slow music). And the datasheet says Control Word + count **restarts**
      the period in modes 2/3 too ("synchronized by software"); only a *bare* count
      write waits for the period's end. s69's two models were each wrong one way. Now
      `cw_armed`/`next_pending` + count-from-load. Tests T11–T15 from the datasheet
      quotes, verified failing on the old model.
   2. **PIC / host**: IRQ0 was auto-EOI'd on delivery (ba927ac). With a correct 70 Hz
      tick, an IRQ0 raised during the ISR's `sti` spin **re-entered the handler**, and
      every tick then nested one level deeper forever — no ISR body (no music), no main
      loop (black screen = "stuck fade"), stack overrun (the es=0xD000 blitter AV). The
      tell was `irq0/s == flips/s == 70` with `p3da` at 3.3M reads/s. Now IRQ0 is held in
      service until the guest EOIs (or our INT 08h BOP EOIs, as the BIOS does), PIC
      ISR/IRR updates are atomic, and a 250 ms timeout ×3 falls back to auto-EOI with a
      log line. STAGE2 `irq0_isr[strict,auto,blocks,timeouts,fallback]`.

   **Measured (rig, headless, keyed to gameplay):** Lemmings IRQ0 70.0/s == vbl edges,
   level fade 2.5–3 s (real ≈2.3 s), `0x3DA` share 24% (the spin), 0 blocked / 0
   timeouts, no faults. Skyroads `n8=0x67 max_ms=0x13 ui_gap=0x8d23` — the baseline
   exactly; keyed play run's frozen-at-`0110:3B40` ending is identical on the previous
   binary (A/B). ⚠ The DPMI (protected-mode) delivery arm still auto-EOIs IRQ0 — Doom's
   path, by hand only.

   **Also (user-specified, same evening): capture RULE 6 — released means released.**
   Raw input followed focus, not capture, so after the Windows key gave the pointer
   back, dragging on the desktop still mouse-looked in the game while our window was
   foreground, and right-clicks over the picture still reached it. Now a mouse-using
   guest that is not captured gets no deltas, no position, no buttons; release reports
   any held button as let go. `mouse_goes_to_guest()` is the one decision point.
   Skyroads on baseline, Lemmings' scripted click still lands while captured.
   **USER-CONFIRMED by hand in Doom and Lemmings.**

   **User's by-hand result (21:03): screen no longer blank, fade completes — but the
   palette still flickers and "it stalled when the trapdoors opened".** The heartbeat
   named the stall: every sample in the ISR's retrace spin, `irq0 == edges == 71/s`,
   `p3da` 3.1M/s, and the new counters `blocks=796 timeouts=1`. Not nesting (the guard
   held) but a **LOCKSTEP**: one long tick (>250 ms in service, the trapdoor moment)
   left a tick queued behind the handler; the queued tick re-entered at the EOI, that
   instance spun to the next retrace and re-armed the PIT, and its own tick then
   fired during the next instance's spin — queued again, forever. A real 8259 would
   do the same after such a stall; the game's design is fragile, a 386 never stalled.
   **Fix (rig-clean, by-hand unproven): a PIT restart (CW+count) from INSIDE the IRQ0
   handler drops the tick queued behind it** (`host_pit_resync_check`, both port
   paths; counted as `resync_drop`). Guests that program the timer once are untouched.
   Plus `IRQ0-ISR-LONG` lines (≤8/run) naming the guest cs:ip during a long episode.
   ⚠ One headless run died silently at `russell.dat` (first run after a deploy, the
   s69 shape, `runs/lemref/s70_headless_death1.log`); 4 later runs were clean.

   **★★★★★ USER-CONFIRMED (22:xx): "everything worked! Lemmings is now playable. I've
   just completed two levels."** On the `3026254` build (`0908cce5…`). The briefing
   screen no longer flickers; a small part of the GAME screen still flickered — that
   is the raster split below.

   **The remaining flicker, root-caused and fixed off-VM (not yet deployed):** Lemmings
   keeps TWO palettes for DAC 16–23 — `ds:2668` pushed by the timer tick, which is
   calibrated to land at row 160 (the toolbar's top), and `ds:2650` pushed after the
   retrace. Level in one, toolbar in the other, separated by the beam. Our presenter
   applied one palette per frame, so the snapshot's phase decided which half was wrong.
   ⚠ The real-DOS oracle under QEMU shows ONE set everywhere (its default 0x3DA makes
   the two writes land back to back) — for raster effects the oracle is not truth; the
   game's tables and timing are. Now: `pal_base`/`pal_split`/`pal_split_row` per entry
   in the video VDD (`pal_split_note`, keyed on the same beam model as 0x3DA), resolved
   per row by the presenter (all three paths) and the capture (24bpp when split), pinned
   by video_test T-SPLIT with the fake clock (8 checks, incl. phase independence and
   expiry). Battery green.

   **The rest of the evening, in order (all on the rig now, `f79d954`):**
   * Deploying the raster split over the confirmed build made it WORSE ("flicker at
     the bottom") — rolled back; rule written: a user-confirmed build is the rollback
     copy and is not replaced without an explicit go.
   * Instrumented runs by the user proved the mechanism: HP mode (1,2) writes two
     palettes per frame with the tick landing on rows 160–169 (reload `0x3192`);
     normal-PC mode (1,1) writes none during play and is fine.
   * Why the split looked worse: the calibration landed the tick on a different row
     every run (306–383 lines), and our IRQ jitter moved it ±5 rows frame to frame.
     Root cause of the run-to-run spread: the interpreter's port path (`iio_out`) did
     not sync the PIT clock before 0x40–0x43, unlike the reflected path — load and
     latch each read a clock stale by up to a pacer round (`19dd3b8`). Spread went
     ±14 → +2..+10 lines. Sticky split boundary (12 rows) + 2-frame expiry absorb
     the jitter (`40112ac`). The multi-line "blank debt" repayment was tried twice
     and measured wrong off-VM; s69's one-blank rule stays, bounded to recent polls
     (`f79d954`). Stall-aware IRQ0 timeout (capture stalls are not the guest's).
   * Headless: reload 322/322/330 lines, 0 blocks/timeouts/drops, Skyroads baseline.
   * **USER VERDICT (23:05): "Clean. There is some slight flicker … Lemmings is
     playable, and flicker is minimal."** Session paused here at the user's request.

   **▶ NEXT:** (a) the user wants to judge the residual flicker against the oracle
   themselves — note the QEMU oracle shows ONE palette everywhere (wrong for raster
   effects); a real-hardware reference or the game's own tables are the oracle;
   (b) the standing ask: **oracle-backed regression gates** (`tools/lemgate`: log
   invariants — reload 320–330 lines, IRQ0 == retrace ±2/s, fade 2–4 s, blocks/
   timeouts/drops 0, no faults, run completes; per-region palette on split frames;
   Skyroads bands) so none of this regresses silently; (c) the silent first-run-after-
   deploy death at `russell.dat` (3 of ~14 headless runs); (d) #4 click→skill; (e)
   strict IRQ0 on the PM arm; (f) the residual +2..+10-line calibration excess (a host
   stall inside the count is repaid one line).

   ---

   ## ★★★★★ SESSION 69 (2026-09-13) — **LEMMINGS: MUSIC-FIX REVERTED (IT BLANKED THE SCREEN); CAPTURE REFINED.**

   **HEAD `0097dd1`, committed, NOT pushed. Battery 1291/0. Deadline: the 17th.**

   The arc: I fixed Lemmings' "music too slow" (#3) by correcting the PIT calibration
   reload, it regressed into a hang, I fixed the hang, then it turned out the *corrected
   timing broke the palette fade* (blank gameplay screen). User chose REVERT for the
   deadline. Net: Lemmings is visible/playable again (music slow again), plus a durable
   crash guard and two capture refinements.

   1. **`59ee731`+`9eec369` (REVERTED in `e084638`): the PIT count-from-load timing fix.**
      Lemmings' "High Performance PC" calibrates its tick from a 320-scanline count on
      0x3DA; `pit_current_count` read the free-running phase not the load instant, giving
      a random reload (0x4bb9 = 61.5 Hz by hand). Count-from-load fixed the tempo — but
      it is COUPLED to the palette fader (also timer-driven), and the corrected reload
      (0x2faa) left the fade stuck near-black. ⛔ **A "correct" timing fix broke a
      timing-dependent renderer.** Reverted to the s68 free-running behaviour the user
      confirmed visible. **#3 is OPEN again**; fixing it for real needs the fader
      (`ds:0x2668` scaled from raw `ds:0x25F0` by a timer-driven level; find the level +
      its driver — guest `int 60h`/`int 61h`, `CS:3e6f`/`CS:5827`).
   2. **`7b59ae0`: a stray guest pointer must not jam the machine.** The blank-screen
      runs also hit an intermittent AV — the planar interpreter read guest linear
      0xd4013 (es diverged to 0xd000 in Lemmings' sprite blitter) in an unmapped UMB
      hole. `imem_r8/w8` now VirtualQuery-cache each page → unmapped reads return 0xFF,
      writes drop, instead of taking the whole host down. Root cause of the es
      divergence not yet found (needs `IMEM-OOR`/interp-regs from a by-hand crash run).
   3. **`0097dd1`: capture policy — focus re-captures + WIN releases without the Start
      menu.** WM_ACTIVATE now re-captures (was click-in-video only); WIN release injects
      one tagged Ctrl tap via SendInput so Explorer sees WIN+Ctrl not lone WIN (the safe
      preventDefault — the literal WH_KEYBOARD_LL hook stays off, it jammed the rig).
      ⚠ **DESKTOP-ONLY, user tests by hand.** Auto-close-on-exit confirmed already done.
   4. **Diagnostics kept** (all cheap/gated): `IMEM-OOR` log + interp regs in the fatal
      dump; exit `level-palette signature` scan + fade dump; heartbeat `pal2668=`/`1f7c=`
      (survives a by-hand death); `cfg\pitlatch.flag` gates the 0x3DA poll ring.

   **▶ NEXT:** (a) user tests the capture refinements by hand; (b) #3 needs the
   fader-vs-timer coupling solved before the reload can move again; (c) Hexen/Heretic
   hi-res loaders + VESA hi-res are **by-hand only** (DOS/4GW `1007` headless blocker)
   and our VESA is banked-only, no LFB — not quick wins. Method scars this session:
   a test written from belief (not the datasheet) certified a hang; a repro harness
   "survived" without ever running the game; leaving a hang running + `livehb.flag` let
   the host's own crash dump name the fault.

   ---

   ## ★★★★★ SESSION 68 (2026-09-12/13) — **THE RIG WAS SILENTLY STOCK; LEMMINGS REACHES GAMEPLAY BY HAND.**

   **HEAD `b4791aa`, committed, not pushed.** Battery 1200/0, interp 170/170.

   1. **"No DOS app runs" = two silent mechanisms stacked** (`d6f8014`). `[0x714]`
      (`FIXED_NTVDMSTATE`) was never written and inherits the machine's real-mode boot
      garbage, so it changed after the Sep-12 reboot to a value with bit 0 set → the
      kernel raised VIP → the first `STI` was a raw #GP → XP tore the VDM down in <1 s
      with no exception and a 2.6 KB log. Then GH #132's counter, which cleared only on
      a clean exit, counted the X-button (TerminateProcess) and guest crashes as failed
      starts, and the fourth launch **removed the IFEO key** — every later launch ran
      stock ntvdm. Now: `[0x714]` written to 0 after `VdmInitialize` (logged as
      `STAGE1: FIXED_NTVDMSTATE inherited=… -> set …`, override `cfg\vdmstate.txt`), and
      the counter clears the moment the window is up. `rt.bat setup` restores the key;
      `ifeochk.bat` is READ-ONLY now (it used to `reg add` a deleted path).
   2. **Lemmings** (`0e48ff7` `e595c91` `5225cb5`): the un-erased trail, the trapdoor
      flicker and lemmings falling through the ledges were **one cause** — in a planar
      mode the host interpreter is the CPU and A0000 is unprotected in V86; an opcode it
      did not model (`repne scasb`, `jmp far [m]`, `CBW`, `XLAT`, `LES`) bailed to V86
      **until the next event**, and every VRAM access in that stretch was invisible to
      `st->plane[]`. Modelled them all (+ `POP r/m16`, `WAIT`, `LAHF/SAHF` from Bubbles'
      1.17M bails). **Every report now prints `STAGE2: P12 non-BOP bail sites=` — read it
      FIRST for any planar-mode guest.** Headless: OUT 0→10 on the reference's clock.
      **User, by hand: lemmings fall into the first chamber correctly.**
   3. **Settings → Display → Start fullscreen** Always / Graphics only / Never
      (`8ca0c5b`, registry `StartFullscreen`). User: "worked great".
   4. Instruments: `cfg\planedump.flag` (planes beside each capture), HB line carries
      `crtc=/flips=/ofs=` (and its buffer was 384 for a 392-byte line since s62),
      `cfg\livehb.flag`, `scripts/bm/lemlive.bat`, rigshot `key` now sends scancodes.

   **▶ NEXT — the user's four remaining Lemmings symptoms, measured in his own 49 s run,
   saved as `runs/lemref/user_s68/ntvdmhost_byhand.log`:** (1) briefing screen flickers,
   (2) game screen flickers **between right and wrong COLOURS** (`dacw=52528`, ~18 DAC
   writes/frame — a palette-tear/blank-during-flip hypothesis, unmeasured), (3) music
   too slow — **by hand `pit_reload=0x4bb9` = 61.5 Hz where every headless run had 0**,
   delivered 59 IRQ0/s, so the s67 "not slower" verdict does not cover this path,
   (4) an in-game click does not assign a skill (`MOUSEI33`: only AX=3 level polls;
   `captured=1`; check the X doubling of `i33_xshift` against where the crosshair is
   drawn). Then: sweep the other guests' bail sites; the user's `claude.txt` list.

   ---

   ## ★★★★★★ SESSION 61 (CONCLUSION) — **SKYROADS PERFECT: THE CRYSTAL WAS ON THE WRONG LOCK.**

   **Date:** 2026-09-10. **COMMITTED (this session, on `m9/completeness`).** Skyroads is
   **user-confirmed "absolutely perfect" while keys are held** — better than its Aug-19
   best. The wobble was never the keyboard yield or delivery (those were the 14%); **86%
   of stalls were the clock never GENERATING the tick**, because `host_pit_sync` took the
   device lock `g_lock` (shared with the renderer, mixer, ~68k port-traps/s).

   **THE FIX (baked, no knob needed):** the PIT is split into
   - `host_pit_generate()` — advance the 8254 from QPC + latch IRQ0, under a NEW
     micro-lock `g_pit_cs`; port handlers 0x40–0x43 take it via a `guard` hook in
     `pit_state`. The crystal can no longer be blocked by the renderer. `gen` 86%→0.
   - `host_pit_deliver()` — the async attempt still needs `g_lock` (never-suspend-a-
     lock-holder), but takes it with `HOST_LOCK_TRY`: a busy lock SKIPS, the tick is
     already latched, cooperative delivery places it. `pit_skip`~50k/session, harmless.
   - **pacer priority HIGHEST→NORMAL** (baked): once generation stopped needing to win
     the lock, HIGHEST starved the present thread on the single core (stepped fades,
     dropped frames). NORMAL = perfect. ⚠ mechanism is a strong guess — `ui_gap` moved
     the WRONG way, see [[timing-fidelity-frontier]].

   ⚠ **FIVE arbitration "arms" (keyirq 0/1/2/3, courier) were all REFUTED in-game first**
   — they fixed the 14%. All left in as `keyirq`/`courier` knobs, defaults are the
   winners (`keyirq=1`, `courier=0`). Also this session: machine-jamming LL keyboard hook
   OFF by default, single-instance mutex, `host_panic_release` on close, capture
   watchdog, GH#132 three-strikes cleared per run, one-folder rig relayout, quoted
   `target.txt` path parse. Det-test 30/30, imports clean, Skyroads headless `gen=0` with
   ZERO knob files.

   ### ▶ NEXT ACTIONS, IN ORDER

   0. **★★★ GAMES NEED SHORT 8.3 PATHS.** Doom (& likely others) won't LOAD from
      `games\<Name>\` — the long "Documents and Settings" path corrupts an INT 21h
      filename (`DOOM.EXE`→`DOOM.ETX`→`DOOME`). Hand the guest a `GetShortPathName` 8.3
      path. ⚠ The user is doing a one-by-one game pass and will report what works; this
      blocks every game that reads its own files by full path.
   0b. **THE CLOSE-ZOMBIE may recur** (host holding its binary/log after window close);
      `controld` `kill` recovers it without a reset. Watch for it during the game pass.

   ### ▶ SUPERSEDED SESSION-61 ACTIONS (kept for the trail; the crystal fix replaced them)

   1. **★★★★★ N REPEATED RUNS PER ARM — THE FIX IS BUILT AND UNPROVEN.** The tick
      courier (`courier.txt = 1`, ships **OFF**) is implemented and its dangerous half
      is validated, but a single 45 s run **cannot** resolve its effect: five runs of
      nominally the same configuration gave anomalous-gap counts of **50, 75, 81, 85,
      240**. Any A/B must be N runs per arm with the **run shape checked** — Skyroads
      sometimes plays a ~10 s intro at the BIOS 18.2 Hz and sometimes goes straight to
      180 Hz, and `IRQ0TL persec`'s first entry says which (`0x14` = intro, `0xb6` =
      no intro). Do not compare across shapes; I did once and it cost a round.
   2. **★★★★ THE REAL LEVER IS THE PORT TRAP (2.33 us), AND THIS SESSION IS THE FIRST
      DIRECT EVIDENCE FOR IT ON THE WOBBLE.** IRQ0 and IRQ1 compete for one scarce
      thing — a moment when the guest is in exec with interrupts on. Every arbitration
      tried just moves the loss: yield on → the clock waits; yield off → **half the
      keystrokes go past 64 ms**; courier on → the clock gains a little and keys got
      worse. Arbitration cannot create windows. The trap cost is what destroys them
      (Skyroads' OPL helper alone runs ~68,000 port traps/s), and it is also handoff
      item #2 from s60. **Own session.**
   3. `guests` (w15, 63%) is still the heaviest score lever; **CALC and WRITE remain
      unjudged** on the rig since s59.

   ### ★★★★★ WHAT THE RUNS ACTUALLY SAID

   The s60 handoff pinned the wobble on **async delivery starving** — "75% of IRQ0
   attempts bail `not_in_exec`" — and prescribed a lock-free courier. The first half of
   that is measured and true; the inference from it is **wrong**, and two new
   instruments say so:

   ```
   anomalous gap = one spanning >= 2 of the period THE GUEST ITSELF programmed
   STAGE2: IRQ0WHY gen=.. del=.. anom[raise,att,nie,yld]=..
   ```

   | in the gaps where Skyroads missed a tick | run A | run B | run C | run D | run E |
   |---|---|---|---|---|---|
   | IRQ0s the 8254 generated | 110 | 192 | 85 | 119 | 362 |
   | injection attempts made | 53 | 76 | 85 | 81 | 253 |
   | **attempts never made** | **57** | **116** | **0** | **38** | **109** |
   | **keyboard yields** | **57** | **116** | **0** | **38** | **109** |
   | bailed `not_in_exec` | 4 | 1 | 0 | 0 | 4 |

   **`raises − attempts == yields`, exactly, in every run.** Every tick that lost its
   injection attempt lost it to `host_irq_sink` handing the timer's one async
   opportunity per raise to a pending key (`g_keyirq_retry`, ON by default,
   `KEYIRQ_MAX_YIELD` = 3 in a row = 16.7 ms at 180 Hz). The worst gap measured reads
   `raise=3, yld=2, att=1` → **20 ms**. That is the wobble in four numbers, and it is
   an **identity within each run**, not a comparison between runs — which is why it
   survives the noise that sinks everything else here.

   ⚠ **`not_in_exec` IS THE HEALTHY BASELINE, NOT THE FAULT.** It runs at **76% during
   the ~6,000 gaps that are keeping perfect time** and 0–4 events total inside the
   anomalous ones. A statistic that is *higher* when the clock is working cannot be
   what breaks it; the cooperative path absorbs those bails routinely.

   ⚠ **AND THE YIELD MUST NOT SIMPLY BE REMOVED.** `keyirq.txt = 0`, measured:
   **51 of 102 keystrokes past 64 ms, worst 1864 ms** (against 0–1 and 5–250 ms with
   it on). That is the "loses keys" result this file already records twice.

   ### ★★★ TWO DEFECTS IN MY OWN INSTRUMENT, BOTH FOUND BY RUNNING IT

   1. **THE "LONG GAP" THRESHOLD WAS A FIXED 8 ms AND THE GUEST CHANGES THE RATE UNDER
      IT.** Skyroads' intro runs at the BIOS 18.2 Hz (55 ms), so **187 of the first
      run's 319 "big gaps" were the intro ticking correctly** — 187 measured against
      ~182 predicted from the timeline. The threshold is now **relative to the period
      the guest programmed**, which is the only frame that means anything.
   2. **I/O PER *GAP* IS CONFOUNDED BY GAP LENGTH.** A gap ten times longer collects
      ten times the I/O whatever the guest is doing. ⚠ **The s60 note records the
      music-overrun hypothesis as REFUTED on this number** (`big gaps 5.3 io/gap vs
      360`); the same raw ratio came out **25x the other way** on this session's runs.
      Neither figure means anything. As a **rate** the answer is clean and does
      confirm the s60 conclusion: **0 port ops/ms during anomalous gaps against 169/ms
      during normal ones.** The guest is not hammering the OPL when the clock slips.

   ### ★★★ THE TICK COURIER — BUILT, DEFAULT **OFF**, HARD PART VALIDATED

   `tick_courier_thread`: a thread that retries a still-pending IRQ0 **outside
   `g_lock`**, woken by the raise site, one tick per wake, bounded by
   `COURIER_BUDGET_US`. An immediate second attempt in the sink cannot work — injecting
   IRQ1 leaves the guest entering INT 09h with interrupts off — so the retry has to
   wait microseconds for the handler to IRET, which nothing in the old structure could
   do. **V86 only (`!g_dpmi_pm`), so it cannot regress Doom.**

   ★★ **THE `g_lock` INTERLOCK PROBLEM IS SOLVED, AND THE SOLUTION IS MEASURED.**
   `host_irq_sink`'s note names the prerequisite for ever moving a suspend outside the
   lock: *"a separate suspend-safe handshake (the exec thread marking itself
   un-suspendable while it holds g_lock)"*. That handshake already shipped — the CPU
   throttle's — and it is now in `async_inject_irq`: re-read `g_in_exec` **after** the
   suspend has landed (`GetThreadContext` is what makes "landed" true) and resume
   instantly on 0. **`left_exec` fired 4 times in 45 s**: four real races where the
   guest left `v86_run` between the pre-check and the suspend. Without it those are
   four suspends of a possibly-lock-holding thread. Plus `g_async_ctxwr`, a
   single-context-writer interlock (**`ctx_busy` = 52**), because two threads building
   IRET frames from the same context would collide on the guest's stack —
   previously safe only *by accident*, since every caller ran under `g_lock`.
   `vdd_pic_ack_autoeoi()` replaces the ack-then-eoi pair for auto-EOI'd lines: the
   pair's transient set/clear of the shared ISR byte is not safe without the device
   lock, and losing IRQ1's in-service bit is the "press a key and everything hangs"
   fault.

   ⚠ **IT SHIPS OFF BECAUSE THE DATA DOES NOT SUPPORT TURNING IT ON.** Its one run cut
   delivery-caused gaps (30 → 19) while generation-caused gaps rose, and key latency
   was worse — the same competition running the other way. Against a 5x run-to-run
   spread, none of that is a result. See next action 1.

   ---

   ## ★★★★ SESSION 60 — **CPU SPEED, HONESTLY. THEN THE SKYROADS WOBBLE, ROOT-CAUSED.**

   **Date:** 2026-09-09. **Score: 86.9%, unmoved** — everything this session is a row
   already at 1.0 (`host-ui`) or accuracy on a counted row (`settings-live`). The 8
   points to 95% live in `guests` (w15 at 63%). **Tree: UNCOMMITTED (13 files).** Rig
   clean. Deterministic CPU test 30/30; full build clean.

   ### ▶ NEXT ACTIONS, IN ORDER

   1. **★★★★★ FIX THE SKYROADS WOBBLE — ROOT CAUSE IS PINNED, USER DEFERRED THE FIX TO
      A FRESH HEAD.** The lever: **deliver the timer tick to a SPINNING guest without
      waiting on `g_lock`.** Skyroads spin-waits at `0110:3b40` (IF set) for its 180 Hz
      tick; during a spin it never traps, so only ASYNC delivery works, and async is
      starved (75% bail `not_in_exec`; residual 8–20 ms gaps are `g_lock` contention
      with the audio mixer — which is why timer AND music wobble together). During a
      pure spin the guest holds no lock, so suspend-to-inject is safe WITHOUT taking
      `g_lock`. ⚠⚠ `g_lock` is the load-bearing interlock (never suspend a lock-holder);
      hardest area of the codebase, needs care. Full chain + instruments in
      [[skyroads-playable-input-stack]]. ⚠ **Skyroads wants UNLIMITED, not a throttle**
      — the throttle STARVES its timer (measured: 84/180, 1 s gaps).
   2. **THE PORT TRAP IS THE REAL CEILING (2.33 us).** Below every speed label; the only
      lever that also speeds Doom/Skyroads at Unlimited (and would cut the OPL trap
      volume that starves async delivery in #1). Own session.
   3. `guests` (w15, 63%) is still the heaviest lever; **CALC and WRITE STILL unjudged**
      on the rig since s59.
   4. **THE MOUSE + MENUS NEED YOUR EYES** (popup contents, Edit-greying-in-graphics) —
      `scripts/bm/menushot.bat` / `setshot.bat` put a live host up. ⚠ Do NOT drive the
      desktop while the user is on the box (I did once; use headless `dlgcheck.flag`).

   ### ★★★ DONE THIS SESSION (all in the uncommitted tree)

   - **CPU throttle rewritten as a CLOSED-LOOP INVARIANT + a DETERMINISTIC TEST.** The
     open-loop debt carry (leaked twice) → `hold = max(0, E/duty − T)`, one pure
     function `cpuspeed_step`, driven against a virtual clock in `cpuspeed_test.c`
     (30/30, bit-identical). Rig, host-measured `delivered_bp` (non-circular): whole
     ladder within ~12% of label (100→88, 66→61, 33→30, 16→15, 8→7). Ref reverted
     **5900→3704** (native ALU) — 5900 was a fit to hide the broken mechanism. Two
     real-world fixes the det-test can't see: torn exec-clock read (clamp `dexec` to
     run wall) and per-period catch overhead (1 ms Sleep-able run floor above 32 bp).
     See [[acceptance-test-is-the-calibration]].
   - **SETTINGS RESTRUCTURED (user's asks):** "CPU"→"Processor" tab (one honest "Limit
     speed" control + a live CPU-name line; Type/Core/Cycles/FPU/Turbo/slider/affinity
     removed as dead or clutter), Memory split to its own tab, PIT/UI-tick moved to a
     new Advanced tab. Speed ladder trimmed 18→6. `Help▸About`→system ShellAbout. All 8
     pages verified building via `dlgcheck.flag` (headless).
   - **MENU BAR** `File Edit View Tools Help` (photographed); **mouse auto-capture** on
     INT 33h use-calls (Doom captures, Skyroads doesn't).
   - **SKYROADS ROOT-CAUSED** (see #1) — and the music-overrun hypothesis REFUTED with
     data (big gaps 5 io/gap vs normal 360).

   ### ★★★ THE ACCEPTANCE TEST WAS THE CALIBRATION WORKLOAD (early-session, superseded above)

   `CPUSPEED_REF_MHZ` is calibrated by `cpubench.com`, whose header says outright *"NO
   REGISTER IN THE LOOP TOUCHES MEMORY, and that is deliberate"*. `cpuswp.bat` then
   re-runs **that same program** at each index and checks the reported MHz tracks the
   label. **It passes however wrong the constant is for every other kind of code**,
   because the ratio it measures is the ratio it was built from.

   `tools/dostest/mixbench.asm` is the instrument that can fail: five shapes of work,
   four of them 12 cycles on a 486 *by construction* so they are directly comparable
   with no arithmetic in between. On the rig at **index 11, where the menu says 66 MHz**:

   ```
   ALU 68    MEM 209    VID13 92    VID12 23    PORT ~1      (MHz apparent)
   ```

   A **200x spread at one setting**. The ALU figure is right because ALU code is what
   the constant was made from.

   ### ★★ FOUR DEFECTS, ALL OURS

   1. **`cpuspd.txt` PARSED A SINGLE DIGIT.** `c[0] - '0'` cannot express an index above
      9, and the ladder went to 17 in s54. So 75, 66, 50, 33, 25, 16, 12 and 8 MHz --
      **every period-hardware setting** -- were unreachable from the file knob, and
      `echo 13` ran as index **1 = 3300 MHz** while reporting that it had. The range
      check *hid* it: `c[0] < '0' + CPUSPEED_COUNT` with COUNT=18 accepts up to `'A'`.
      ⇒ The rig sweep this knob exists to drive **never tested the slow half of the
      ladder even once**, and `cpuswp.bat` only ever swept 0-6.
      ⚠ File knob only. The menu and dialog set the index directly, so this is a
      testability defect and **not** the cause of any speed a user has seen.
   2. **THE THROTTLE BILLED THE GUEST FOR OUR OWN OVERHEAD.** `ran_us` was wall clock
      from resume to suspend, and a DOS guest spends much of that window not executing
      but trapped inside us. Every port write is an IOPL-0 #GP costing **2.33 us**
      (iobench case 3: 117,920 accesses / 5 ticks = 429,400/s). At a 1.8% duty the
      guest is held 54x as long as it "ran", so every mis-attributed microsecond costs
      it 54 more -- hardware-touching code penalised in proportion to how slow *we* are.
      Now charged on guest **execution** time (`g_exec_us_acc`, bracketed by
      `g_in_exec`, gated off entirely at Unlimited).
   3. **THE EXECUTION BASELINE MOVED ON FAILED RETRIES**, discarding execution that was
      never charged. The throttle reported `delivered_bp=100` -- a 1.00% duty, exactly
      as asked -- while mixbench measured the guest getting **2.6%**. `missed=594` is
      where it went.
   4. **`delivered_mhz` REPORTED THE REQUEST, NOT THE DELIVERY.** At index 15 it logged
      16 MHz while the guest measured **34** -- and index 15 was *faster than index 13*,
      so **the ladder is non-monotonic at the slow end** and this field could not say
      so. `owed_ms=1326` of unpayable arrears was sitting three fields away saying the
      opposite. Now `requested_mhz` beside a measured `delivered_bp`, with `wall_us`
      next to `ran_us` so the overhead is visible.

   ### WHAT IT BOUGHT, MEASURED

   | index 11 ("66 MHz") | before | after |
   |---|---|---|
   | ALU | 68 | 76 |
   | MEM | 209 | 76 |
   | VID13 | 92 | 54 |
   | VID12 (interpreter) | 23 | 23 |
   | **PORT** | **89,600 outs/s** | **220,400 outs/s** |

   Spread across the real-CPU cases **9.1x -> 3.3x**; port throughput **2.46x**;
   Unlimited unchanged (3781 / 3926 / 4012 / 27).

   ⚠ **`CPUSPEED_REF_MHZ_DEFAULT` IS NOW 5900 AND ITS UNITS CHANGED.** It is MHz of
   guest *execution* time, not of wall clock. **`cpubench` still reports the wall-clock
   number** and will read ~3700 on this rig for ever, which looks like a disagreement
   and is not -- putting 3661 back would silently restore the old behaviour. 5900 is a
   **fit across two points** (1.44x at index 11, 1.85x at 13), not a derivation: one
   constant cannot satisfy both because the residual is per-workload escape.
   ⚠ **STILL OPEN:** the throttle is workload-dependent and still not monotonic -- MEM
   reads 80 MHz at index 13 against ALU's 26, and higher than its own index-11 figure.

   ### ★★★ AND NO CALIBRATION CAN FIX THE REAL PROBLEM

   A port trap is **6.9 MHz apparent** against a 486's 16-cycle `OUT`. That is **below
   every label on the menu**. Doom does ~43,000 port writes a second while drawing;
   Skyroads' AdLib helper spends 43 port accesses per OPL register. So the dropdown is
   an approximation for **compute**, and `cpuspeed.h` has always said so -- it now says
   by how much. Making the trap faster is the only route to an honest 66 MHz, and it is
   the one change that would also speed the games up at Unlimited.

   ### ★★ SKYROADS: THE TIMER IS FLAT. DO NOT RE-INSTRUMENT IT.

   User report: "the weird slow down/speed up timing issue". The last recorded run was
   `idx=0 duty_bp=10000` -- **unthrottled** -- so the CPU throttle is not involved.
   New instrument (`STAGE2: IRQ0TL` / `IRQ0GAP`, both delivery paths) over 45 s:

   ```
   IRQ0TL persec = 183,181,179,180,180,180,180,181,180,...,177,179,183,180,179,181,177
   IRQ0GAP ms[<1,1,2,4,8,16,32,64+] = 79,59,416,7375,171,1,0,0  n=8101  max_ms=16
   ```

   **180 ticks/second flat, range 177-183 (+-1.7%).** 91% of gaps in the 4-8 ms bucket
   (180 Hz = 5.56 ms), **one** gap over 16 ms, worst case 16 ms. 8101 delivered against
   the session-21 baseline of ~4485 -- delivery is better than it has ever been.
   ⚠ **BUT THIS IS A HEADLESS ATTRACT-MODE RUN**, and the memory warns twice that
   Skyroads' attract loop fakes success and that in-game uses a different input path.
   ⇒ The tick rate is cleared **for this run shape**. Next suspects are frame
   presentation and OPL pacing, not the timer.

   ### ★ MOUSE EXCLUSIVITY IS THE GUEST'S OWN DECLARATION

   A program that wants the mouse says so through INT 33h; one that does not never
   calls it. Capture now keys on the **use** functions (01/03/05/06/0B) and explicitly
   **not** on the detection probes -- validated against real logs before it was written:

   - **Doom:** `0000 x1  0015 x1  53c1 x1  0003 x1338  000b x1338` -- probes once, then
     polls every frame. Confirmed live: `autocap_want=1 fired=1 captured=1`.
   - **Skyroads:** *no `MOUSEI33` lines at all.* Never calls INT 33h, so its mouse stays
     on the Windows desktop. Exactly the split the user described.

   ★ **AND IT CLOSED AN OPEN QUESTION IN THE CODE.** The `i33oth=1079` note listed two
   hypotheses for high AX values -- unimplemented functions, or a mis-patched `CD 33`.
   It is neither: **`AX=53c1` is Logitech CyberMan SWIFT detection**, made once, and
   Doom prints `CyberMan: Wrong mouse driver - no SWIFT support (AX=53c1)` in the same
   log. The bucket was the defect, not the thing bucketed.

   Latched once per program so a polling guest cannot drag the pointer back after
   Win+F10; requires foreground so a background VDM cannot steal it. **Show Host Cursor
   and Ctrl+F8 are gone** -- pointer visibility is what exclusive mode looks like, not a
   knob of its own. `ShowHostCursor` survives, narrowed to "when not captured".

   ### ⚠ TWO HARNESS TRAPS I WALKED INTO TODAY

   - **TWO CONCURRENT `build.sh` RUNS INTO ONE `build/`.** A backgrounded job re-ran the
     build while I ran it in the foreground; the binary came out with *some* edits and
     not others, and I spent a detour concluding a field "was missing".
   - **`grep`/`strings` ON THE PE DOES NOT FIND ITS STRING LITERALS.** `site_ovf=` is
     demonstrably printed by the running binary and matches **zero** times in the file.
     Two wrong conclusions came from that before I stopped trusting it. **The only
     trustworthy check that a build contains a change is to RUN IT.**
   - Also: a stray background job racing the foreground over `cpuspd.txt` produced an
     interleaved, contaminated sweep -- *stale artefact worse than missing*, again.

   ---

   ## ★★★★★ SESSION 59 — **ZAR RENDERS.** ITS ATTRACT DEMO IS ON SCREEN, IN COLOUR.

   > **▶ NEXT SESSION, IN ORDER. (15 commits, `10a62cf`..`543858c`, NOT PUSHED.**
   > **Rig clean: no knobs set, watcher alive, staged binary = the build.)**
   >
   > 1. **IS ZAR PLAYABLE?** The user saw it render ("I saw it run. Good progress")
   >    but has not yet driven it. **`scripts/bm/zarplay.bat` is staged and ready** —
   >    it asserts the IFEO key, clears the GH #132 counter, silences the trace for
   >    speed and leaves the game up. Questions: does the demo run smoothly; does
   >    ESC/SPACE/ENTER reach a MENU (`MAIN MENU`/`SELECT PLAYER` exist in the
   >    binary); do arrows/mouse respond and how does it FEEL. Expect NO sound.
   >    ⚠⚠ **DELETE `wowquiet.txt` AFTERWARDS** — see hazard (5).
   > 2. **ZAR'S AUDIO** — one named gap, and it needs its own session because the
   >    right answer touches an explicit "never do this". See the block below and
   >    [[zar-dos16m-frontier]]. Do NOT start it at the end of a long session.
   > 3. **REMOVE THE `host_irq_sink` PM THROTTLE.** It defends against a phantom
   >    16 kHz burst that the 8254 fix proved never existed. Separate, measured.
   > 4. **CALC and WRITE** are still unjudged on the rig (carried from s58);
   >    `guests` remains the heaviest score lever at +0.26 each.
   > 5. ⚠⚠⚠ **TWO RIG HAZARDS BIT THIS SESSION — READ BEFORE TRUSTING A RESULT:**
   >    `zarplay.bat` leaves `wowquiet.txt` behind (a TIMING change for every later
   >    run; a 6 KB `result_*.log` is the tell), and `doomrun.bat` exists TWICE with
   >    `rt.bat` calling the ROOT copy by absolute path. Also: the watcher dies with
   >    a host teardown and needed restarting three times.

   ### ▶ THE ROOT CAUSE OF #23, AND IT WAS OURS

   **DPMI `0300` (simulate real-mode interrupt) implemented ONLY `INT 21h` and `INT 33h`
   — and then cleared CF unconditionally, so every other vector was told "done".**

   A Watcom/DOS4GW program does not write `int 10h`. It calls `int86()`, and `int86`
   under an extender is `INT 31h AX=0300, BL=10h`. So ZAR's `SetMode(0x13)` returned
   SUCCESS having done nothing, and the game ran its demo — frames advancing, data
   streaming — into a screen it had never been allowed to open. Same 45 s run:

   | | before | after |
   |---|---|---|
   | `STAGE2: mode sets` | `none` | **`mode=0x13/kind=02/320x200`** |
   | `STAGE2: video now` | `mkind=00` (text) | **`mkind=02 gw=0x140 gh=0xc8`** |
   | `linear_bar_nonzero` | `0/0x2800` | **`0x2800/0x2800`** (framebuffer full) |
   | unhandled `simInt` | *never printed* | `int2fh x3 int66h x3` (the game's own probes) |

   ✅ **SCREENSHOT: 3D terrain, the `Z.A.R. - DEMO` banner, a vehicle — the attract demo
   playing in our VDM window.** ⚠ That is RENDERING, not "playable": input, sound and a
   real game session are unproven, and `done` means USER-CONFIRMED.

   **RE-GATED** (the video path is shared; same day, same box, 45 s):

   | | Doom | Skyroads |
   |---|---|---|
   | `simInt UNHANDLED` | **0** (all serviced) | **0** (all serviced) |
   | `mode sets` | `0x03` **and** `0x13` | `0x13` |
   | delivery | raises 6196, TOTAL 6197 (**100%**), owed_max 31 | `pit_reload=0x19e4` (180 Hz) |
   | `irq0_inj` | — | **5412** vs 5415/5413 baseline |

   ⇒ Neither guest affected; ZAR gains a picture.

   ★★★ **THIS IS THE SIXTH INSTANCE OF THE PROJECT'S MOST EXPENSIVE BUG SHAPE** — a
   service that does nothing and reports success (see [[stepped-over-call-answers-at-random]]).
   The `0300` arm's own comment says *"widen this when the evidence names an
   interrupt"*, which is right — but it still cleared CF for vectors it did not
   service, so nothing ever failed loudly.
   ⚠⚠ **AND THE HOST HAD BEEN COUNTING THEM FOR FOUR SESSIONS.** `g_simint_unhandled`
   and `g_simint_vec[]` recorded ZAR's seven unhandled `INT 10h` calls every run — into
   the periodic KEYLOG block, **which a headless run never reaches**. Every "ZAR never
   calls INT 10h" note in sessions 58 and 59 (including the ones above, written before
   this was found) rested on a grep that could not see them. The counters are in STAGE2
   now. ▶ **If a service dispatches on a sub-function, its unimplemented arms must be
   reported where a HEADLESS run prints.**

   ### ▶ ★★★★ AND SOUND IS THE SAME BUG, ONE GAP FURTHER DOWN

   **DPMI `0300` is specified to invoke the GUEST'S OWN real-mode handler from the IVT.**
   Ours serviced three vectors host-side (21h, 33h, and now 10h) and silently did nothing
   for the rest. ZAR installs its own real-mode INT 66h handler (`0201 setRMvec int 0x66
   = 0x34d3:0x01d1`, an AIL-style private API), calls it three times with AX=0300/0301/
   0304, and restores the vector. **We dropped all three.** Consequences, measured:
   the Miles driver it loads — `SOUND\SBLASTER.DIG`, opened, seeked, read — is **never
   once called**, all **2,386** real-mode calls in a run go to our own DOS handler at
   `0050:0000`, and no SB port is ever touched (`sb_dspwr=0`, `opl writes=0`).

   ✅ **WITH REFLECTION ON, ZAR PROGRAMS THE SOUND BLASTER FOR THE FIRST TIME:**
   `SNDIO out 0x22c`, `sb{blocks=1 left=0x10 len=0x10 rate=0x56ce}` — 0x56ce = 22222 Hz,
   exactly the `sound SamplingRate 22222` in `USER1.CFG`.
   ⚠ **AND THEN IT WEDGES.** The guest spins in REAL MODE at `0x34d3:0x06b1` with that
   DMA block queued and never draining (`irq0=1 intpend=1`) until the watchdog kills it.
   The driver is polling for a completion that never arrives: **the nested V86 loop that
   `0301/0302` runs a real-mode procedure in does not appear to deliver the SB's IRQ**,
   so `v86_run` never returns and the poll never ends. ▶ THAT is the next gap.

   ⇒ Committed behind **`simintrefl.flag`, OFF by default** — a wedge is strictly worse
   than "renders, silent", and this is the call `wowquiet.txt` / `pitinj.txt` already
   make. Turn it on to work the audio thread; leave it off to play.

   ### ▶ ★★★ AND ONE MORE STEP DOWN THE SOUND PATH (always on, Doom re-gated)

   **The SB mixer answered "no IRQ, no DMA".** Registers `0x80` (IRQ select) and `0x81`
   (DMA select) fell through to the plain mixer RAM, which is zero — and on real
   hardware zero there does not mean "default", it means **NO IRQ SELECTED / NO DMA
   CHANNEL SELECTED**. A driver that autodetects the card instead of trusting `BLASTER`
   learns it is unconfigured. ZAR's Miles driver does exactly that:
   `out 224<-80 / in 225->00`, `out 224<-81 / in 225->00`, then `40 D3` (22222 Hz) and
   `14 0F 00` — an 8-bit **single-cycle 16-byte** transfer, the classic init-time
   DMA/IRQ **self-test** — and waits for a completion interrupt it cannot receive.
   ► Now answered from `st->irq`/`st->dma8`/`st->dma16`, **derived not stored**, so it
   cannot drift from the numbers that go into `BLASTER`.

   ✅ It moves the fault exactly one step, and names the next gap precisely:
   `in 225 -> 0x02` (IRQ 5) and `0x22` (DMA1|DMA5); **`sb{left}` 0x10 → 0x00 — the block
   now DRAINS**; and **`ASYNC-EARLY bail irq=05 why=0x14`** — **IRQ 5 IS RAISED** and
   refused, `why=0x14` being *"the CPU thread was in HOST code"*.
   ⇒ **An IRQ raised while the guest is inside a nested real-mode call cannot be
   delivered**: the async injector only places one when it finds the thread executing
   GUEST code, and single-cycle means one IRQ and no second chance.
   ### ▶ ...AND THAT COOPERATIVE FIX WAS TRIED. IT IS RIGHT, AND IT IS NOT ENOUGH.

   The gate was inline in the main exec loop, so a guest inside a nested real-mode call
   never reached it. Factored into `v86_deliver_dev_irq()` (extracted verbatim, not
   copied — two copies of an interrupt-delivery gate is how they drift) and called from
   the nested `0301/0302` loop too. **A real gap, closed.**
   ⚠ **But it does NOT fix ZAR's audio, and the measurement says why.** Its Miles driver
   waits on a **memory flag its ISR will set, not on a port** — the last `SNDIO` in the
   run is the `14 0F 00` command itself, with no polling after. A guest that spins
   without trapping never returns from `v86_run`, so the nested loop gets no further
   turn: **zero `IRQN-REFUSE` lines**, i.e. the gate never once saw a pending device IRQ
   while the driver waited. A cooperative fix structurally cannot reach this.

   ### ▶ THE ASYNC RETRY WAS ALSO TRIED. IT HELPS DOOM AND CANNOT HELP ZAR.

   A device IRQ got exactly ONE async attempt, at the raise instant; if the CPU thread
   was in host code that microsecond it was lost for ever. Now retried **once per PIT
   sync**, first pending hooked line only, nothing at all when none is outstanding
   (session 22's disaster was ~800 unbounded `SuspendThread` round trips *per sync*;
   this is ~2 **a second**). Done in `host_pit_sync`, which already holds `g_lock` —
   and holding it is what guarantees the thread we suspend is not holding it.
   ✅ **Doom: `try=0x5f ok=0x10` and `try=0x6f ok=0x10` — 16 SB block-completion
   interrupts a run that were previously LOST are now delivered**, reproducibly.
   ⚠ `REPLAYED_LOUD` deserved care (a late IRQ is a plausible late-refill mechanism).
   Six runs settle it as variance: **without** 112/120/152, **with** 157/79 — the retry
   runs bracket the others on both sides. `idle` at its lowest, everything else flat.

   ### ▶ ★★★★★ AND NOW THE ZAR AUDIO BLOCKER IS FULLY EXPLAINED

   With `simintrefl.flag` on, the **heartbeat** (the only instrument that survives a
   wedge — STAGE2 never prints when the watchdog kills the run, so the counters were
   added there) reads:

   ```
   r5=0x1   rtry=0x65f/0x0   why=0x14
   ```

   IRQ 5 raised **exactly once**, the retry offers it **1631 times**, **every** offer
   refused with `why=0x14` = *"the CPU thread was in HOST code"*. And the log's last
   line is the **third** reflected INT 66h call (`AX=0x0304`) with **no matching
   `0301 -> RM proc returned`** — the previous two both returned.

   ⇒ **That call entered the guest's real-mode handler, programmed the SB, and never
   came back. The host thread is blocked inside `v86_run`**, so `GetThreadContext`
   reports the syscall frame rather than the V86 guest: the injector can neither SEE nor
   REACH it. Cooperative delivery cannot help either — the loop gets no further turn
   (zero `IRQN-REFUSE` lines). **Neither delivery path can reach a real-mode procedure
   that spins without trapping.**

   ▶ The architecturally right answer is the kernel's own interrupt assist via the
   `FIXED_NTVDMSTATE` pending bits. ⚠⚠ But **"Never poke `[0x714]|=1`"** is an explicit
   prior finding (VME/VIF gating) — so that wants its own session, deliberately, not the
   tail of this one. ZAR renders and is silent; that is a good place to stand.

   ⚠ Doom re-gated and the change is **provably inert** for it: Doom only ever selects
   mixer index `0x82` (10 times a run), never `0x80`/`0x81`, so no path reaches it.
   `sb_blocks 0xec5→0xecc`, `midi_msgs 0x309→0x30b`, `idle 0x6202→0x5a02` (less inserted
   silence), `mix82 ANSWERED_NO 3→3`. `REPLAYED_LOUD 0x78→0x98` is run-to-run variance
   on a metric with no route to this code — not a regression.
   ⚠ Only reflects when the GUEST owns the vector (IVT segment != `DOS_HDLR_SEG`).
   Vectors still pointing at our own stubs keep today's behaviour and stay visible in
   `STAGE2: simInt (DPMI 0300) UNHANDLED`, which is where the next one will be found.

   ### ▶ ⚠⚠⚠ A REPORTED DOOM REGRESSION, AND THE TWO RIG HAZARDS BEHIND IT

   The user reported Doom's status bar pixelated and messages leaving pixels behind —
   the symptoms `8648f41` fixed in session 25. **It did not reproduce, and the video
   path measures perfect:**

   - **`TITLEPIC vs shot01/shot02 : 0 of 64000 compared pixels differ (0.000%)`** —
     planes → compose → present → capture, bit-perfect over a full screen, judged
     against the IWAD.
   - `planejudge`: planes **71.2/70.8/72.4/69.5%** vs STBAR, against the **70/69/71/68%**
     recorded when it was fixed (**34/71/30/28%** when broken). Plane-to-plane identity
     18–30%; the broken state was 66.8% (one plane smeared over the rest).
   - Nothing this session touches Doom's video: **zero `simInt 0x10` calls**,
     byte-identical video counters, mixer `0x80/0x81` never selected by Doom.

   ⚠⚠⚠ **BUT TWO RIG HAZARDS MADE THE EVIDENCE UNTRUSTWORTHY, AND ONE WAS MINE.**
   1. **`zarplay.bat` leaves `wowquiet.txt` behind** and cannot clean it up (it exits
      while the game runs). That silences `log_append` for every LATER run — **which is
      a TIMING change, not just a quiet log**: per-event logging under the device lock
      is what cost Skyroads 24% of its ticks. Measured: created 22:43, and a Doom run at
      23:19 came back **6,702 bytes** with the `WOWQUIET` banner — no STAGE2, no
      counters, and different timing from the shipping configuration. A "bad, then fine"
      report straddling that moment is explained with no code change at all — and it
      cuts both ways, since "fine" under a silenced trace is not "fine" as shipped.
      ▶ **`ls` the share for `wowquiet.txt` before believing any measurement or report.**
      A 6 KB `result_*.log` where megabytes are expected is the tell.
   2. **`doomrun.bat` exists TWICE on the share** and `rt.bat`'s `:doomrun` calls the
      **ROOT** copy by absolute path. I staged a fix to `bm\` only; it did nothing and
      read as "the fix does not work". Same trap as session 58's root-`.bat` breakage.

   ✅ Re-baselined with the trace RESTORED (6.3 MB log): identical video counters.
   ▶ **And it is now catchable**: `capture.flag` → `doomrun` collects `shot*.bmp` (it
   never did — the `doom` arm skips `:collect`) → `doomref.py cmp TITLEPIC` is a
   0/64000 pass-fail on the whole video path.

   ## ★ SESSION 59 (earlier) — HOW THE PROBLEM WAS NARROWED

   ### ▶ ★★★★★ THE ONE FACT THAT CHANGES THE PROBLEM

   Session 58 handed over *"loads ~4.8 MB of a data file and stops progressing"* and
   set the next step as *"instrument the TRANSITION where the reads stop"*. **There is
   no transition, because the reads never stop.** Given seven minutes instead of
   forty-five seconds, ZAR's file-read sequence is **PERIODIC**: period **1073 reads**,
   6.2% mismatch, ~175 s per cycle, measured by autocorrelating the `INT21 AH=3F`
   trace of a 420 s run. 2,553 reads, 1,148 of them distinct, the tail set hit 10-11
   times each. The "stall at `pos=0x49d454`" was a 45-second window onto a cycle.

   ★ **AND THE BINARY NAMES THE CYCLE.** ZAR's state machine switches on a dword at
   obj1 link `0x4c878`, and each state loads a caption from the obj3 string pool:

   | state | caption | | state | caption |
   |---|---|---|---|---|
   | 1 | `WAIT...` | | 6 | **`LOADING DEMO...`** |
   | 2 | `LOADING...` | | 7 | `LOADING INTRO...` |
   | 5 | `LOADING BATTLE...` | | 9 | `QUITING...` |

   A load cycle that repeats every ~175 s **is the attract/demo loop**. The outer loop
   at obj1+0x1289 is a plain `wait for the tick counter to advance, then run that many
   frames` — the thing session 58 filed as "its idle is a wait-for-next-tick loop" is
   ZAR'S MAIN LOOP, and it is turning. Nothing fails: the binary carries
   `Can't load game data`, `Library reading error`, `** NOT ENOUGH MEMORY: **` and
   `Error loading sound effects.`, and **not one of them is ever printed**.

   ⇒ **The blocker is not loading and never was. ZAR runs its game logic and never
   initialises video** — `INT 10h` is called ZERO times in a seven-minute run.

   ### ▶ HOW TO READ THE GUEST'S ADDRESSES (this cost an hour; it need not again)

   - `ZAR.EXE` is MZ + **LE at 0x2a50**, three objects; the object page map is LINEAR,
     so `obj1 + off` is at file `datapages + off` (`datapages` = LE header `+0x80`).
   - The load base is IN THE LOG: `INT31h AX=0501 BX=0x0b CX=0x7000 [LE CODE OBJECT]
     -> mem 0x03f70000` is obj1 (0xb7000 = its 183 pages). obj3 is the next 0x621000.
     ⚠ **It moves run to run** — 0x03f70000 one run, 0x03b70000 the next. Take it from
     the log, never from a previous session's note.
   - The file image is already relocated against the LINK bases (**obj1 @ 0x10000,
     obj3 @ 0xe0000**), so an absolute operand in a disassembly is a link address and
     `guest = link - link_base + load_base`. Confirmed: obj1+0xae71c (the LE entry
     point) is the `WATCOM C/C++32 Run-Time` banner.
   - The string pool at obj3+0x100.. is the game's whole vocabulary — states, menus,
     every error message. Dump it FIRST on any new guest; it is an hour of call-graph
     work for free.

   ### ▶ AND SWITCHES NOTHING ON THE RIG COULD REACH

   `ZAR.EXE` obj3+0x1b5 is its own `-Help` text:
   `-NoSound` (disables the sound system), **`-NoVESA2` (disables VESA 2.0 linear
   frame buffer modes)**, `-Join <address> <port#>`, `-Psw <password>`. Every runner
   here hard-coded a bare `ZAR.EXE`. `scripts/bm/zarargs.bat` passes a guest command
   line (on the REAL command line — when CSRSS names the program the host does not
   consult `target.txt`); `-Help` is the smoke test that arguments reach the guest.
   ⚠ Note this against session 58's *"do not plan the VESA work around ZAR"*: that
   conclusion still holds for the CONFIGURED mode (`USER1.CFG` ships `VGA_320x200`),
   but the binary plainly has a VESA 2.0 path, a `VIDEO MODES` menu, and VBE `4F06`/
   `4F07`/PM-interface call sites at obj1+0x97dca..0x980c9.

   ### ▶ TWO DEFECTS FIXED, BOTH FOUND BY LOOKING AT WHAT THE HOST WAS DOING

   **1. ★★★★ THE REFLECTED-INTERRUPT TRACE WAS A FIREHOSE A GUEST COULD DRIVE.**
   `dpmi_dispatch_to_pm_handler` wrote two `log_append` lines (~350 bytes, plus two
   `host_readable` probes) on EVERY reflected INT, unconditionally. ZAR polls its own
   `INT 21h` hook for `AH=2Ch` at ~3,800/s, so a 45 s run was ~170,000 dispatches,
   ~340,000 `WriteFile` calls and **53 MB of log** — a histogram of which is 100% one
   line, one vector, one AX value. The 501 file reads that mattered were 0.1% of it.
   ► Now bounded **per (vector, AH)**: the first 24 of a pair always print, then at
   most one per 100 ms, with the total for every pair reported at STAGE2 and a line
   saying when a pair crossed into the limited regime. **A seven-minute run is 9 MB
   instead of ~500 MB** — which is the only reason the periodicity above was visible.
   ⚠ Per-pair and RATE-based, both deliberately: a global count cap silences a rare
   vector because a common one spent the budget, and a pure count cap goes dark exactly
   where a long run's evidence is. `AH=3Fh` at 6/s stays fully traced.
   ★ This is the THIRD time this project has paid for a per-event log in a hot path
   (Skyroads lost 24% of its ticks to one; `wowquiet.txt` argues it again).

   **2. ★★★★ THE 8254 APPLIED HALF A COUNT.** `pit_out` read-modify-wrote `reload` on
   every byte, so between a guest's two `out 40h` instructions the divisor was
   (old MSB | new LSB). ZAR programs **0x8002** (36.41 Hz, ~2x the BIOS rate) as
   lo=0x02 then hi=0x80, from a standing 0 (65536) — so the LSB write alone left
   **`reload = 2` = 596,591 Hz** for the whole gap between the two writes, and that gap
   is not microseconds for us, it is two traps out of protected mode and back.
   The run's own counters had said so all along and nobody had read them:
   `raises=29657` in 45 s against a programmed 36.4 Hz, `owed_max=64` (saturated),
   `PIT-RELOAD 0x2 (hz=0x91a6f)` sitting in the log with nothing else out of range.
   ~29,000 interrupts the 8254 never generated, each a `SuspendThread` round trip under
   the device lock. ► The real 8254 buffers the LSB and loads the count register on the
   MSB write. LSB-only / MSB-only zeroing the other half is the same mistake in a second
   dress and is fixed too, but **on datasheet grounds, not on ZAR's evidence** — ZAR
   uses lo/hi and never takes that path. Three new checks in
   `tools/dostest/pit_test.c`, **verified failing on the old code**, 26/26 passing now.
   ⚠⚠ The PIT is the most shared path here — so BOTH were re-gated, same day, same box,
   `bmqueue.sh` at the standard 45 s cap, the "before" run using the session-58 binary:

   | | Doom before | Doom after | Skyroads before | Skyroads after |
   |---|---|---|---|---|
   | `pit_reload` | 0x214a (140 Hz) | 0x214a | 0x19e4 (180 Hz) | 0x19e4 |
   | `raises` | 6624 | **6189** | 8242 | **8109** (=180.0 Hz exactly) |
   | delivered / raises | 93% | **100%** | — | — |
   | `owed_max` | 64 (**saturated**) | **27** | — | — |
   | owed buckets 32-63 / 64 | 28 / 26 | **0 / 0** | — | — |
   | `irq0_inj` (V86) | — | — | 5415 | **5413** (0.04%) |

   ⇒ **Doom strictly better, Skyroads unaffected**, and in both the drop in `raises` is
   exactly the phantom burst disappearing. That is the shape a correctness fix should
   have: the guest's programmed rate is untouched, only the invented interrupts go.

   ★★★ **AND IT REFUTES A BELIEF THIS HOST IS BUILT ON.** `host_irq_sink`'s throttle is
   justified in a long comment by *"Doom's music driver programs the 8254 at 16 kHz
   (reload 0x4a = 16 kHz, measured)"*, which makes a 50 ms catch-up gap EIGHT HUNDRED
   raises. **There is no 16 kHz timer.** `0x4a` is the LOW BYTE of `0x214a`, and the
   "measurement" was this bug's transient being read as the guest's intent. Doom
   programs 140 Hz, once. ▶ So the whole "one attempt per sync" PM throttle was built to
   defend against a burst WE CREATED — and it is the thing that later cost Skyroads a
   fifth of its clock until it was scoped to PM clients. It is now defending against
   nothing. **Removing it is a real lever and a separate, measured change** — do not
   slip it in with this one, but it is the first thing to try for any PM guest that
   looks tick-starved.

   ### ▶ ★★★★★ AND THE BIG ONE, FOUND BY TRYING TO PASS `-Help`:
   ### **NO DOS PROGRAM COULD BE GIVEN A COMMAND-LINE ARGUMENT. AT ALL.**

   `target.txt` has split `path [args]` since M2.5. **The CSRSS path never did** — and
   that is EVERY REAL LAUNCH, because the IFEO hook is how a program reaches us on the
   user's machine. So `ZAR.EXE -Help` opened a file literally called
   `C:\game\ZAR.EXE -Help`:

   ```
   STAGE2: loaded 0x00000000 from C:\game\ZAR.EXE -Help
   STAGE2: loaded 0x00000000 from C:\game\C:\game\ZAR.EXE -Help
   STAGE2: cmdtail len=0x02 [20 5c 0d ...]          <- " \" -- junk from CSRSS's cmd= field
   ==> DOS terminate (AH=4Ch), exit code AL=0x00000000     (78 ms)
   ```

   Zero bytes read, fall through to the four-byte `mov ah,4Ch / int 21h` embedded stub,
   clean exit having done nothing. **`EDIT FOO.TXT`, `DOOM -warp 1 1`, `PKUNZIP x.zip`
   — none of them could ever have worked**, and each would have looked exactly like
   "the program runs and does nothing". Same symptom GH #131 chased for a session.
   ► Fixed by `csrss_open_split()`: try the whole string first (so a real path
   CONTAINING a space still works), then split left to right and take **the first split
   that names a file which actually exists** — the file system arbitrates instead of a
   guess. Applied to both the absolute-title and the joined-relative arms.
   ✅ **MEASURED:** `zarargs.bat 30 -Help` now prints ZAR's own options block through
   our host, `cmdtail len=0x06 [20 2d 48 65 6c 70 0d]` = `" -Help\r"`.

   ⚠⚠ **AND IT WAS NEVER "NO ARGUMENTS" — IT WAS ALWAYS A WRONG ONE.** With the title's
   args unparsed the code fell back to CSRSS's `CmdLine`, which arrives as junk on this
   path (`cmd=[\]`), so **every ZAR run this project has ever done handed the guest a
   command tail of `" \"`** — a spurious argument, on a program whose argument parser
   selects network and video behaviour. That is a live suspect for the whole #23
   investigation and it has been under every measurement since session 55.

   ### ▶ ⚠⚠ AND ONE SELF-INFLICTED WOUND WORTH REMEMBERING

   ⚠⚠ **AND THE HOST UNINSTALLS ITSELF WHEN OUR OWN METHOD LOOKS LIKE CRASHES.**
   GH #132 counts consecutive failed starts in `C:\ntvdmex\startfail.txt`, incremented
   at every start and cleared only by a CLEAN GUEST EXIT — but `zarlong.bat` and
   friends deliberately LEAVE THE HOST RUNNING for a human to look at, and the next run
   `taskkill`s it. That is a "failed start" every time. **Three in a row and the host
   removes its own IFEO Debugger key**, after which every later run silently measures
   STOCK ntvdm. Measured this session: a run came back `start mode was UNINSTALL`, the
   guest died in 31 ms, and the evidence read as *"-Help makes ZAR exit instantly"*.
   The mechanism is correct and stays; the runners now `del C:\ntvdmex\startfail.txt`,
   because a runner that kills the host ON PURPOSE has no business feeding that counter.
   ▶ This is [[stock-ntvdm-doom-oracle]]'s hazard from a NEW direction: not "somebody
   forgot the key" but "we took it out ourselves". `bm\ifeochk.bat` answers it in
   seconds — run it whenever a result surprises you.

   Adding that `from CS:EIP` field **killed the host outright** on the next run:
   `DPMI FATAL: exception code=0xc0000005 ... bytes@fault: 0f b6 01 c0 e8 04`. The
   entry line is built in ONE pass into a `char lb[256]` with nothing counting
   characters, and it was already **247 characters** long — vector, handler sel:off,
   AX, DS:EDX, a linear address, a 16-byte `zdump` (48 chars by itself), SS/ESP/CS with
   D/B annotations, h32. Nine bytes of headroom. The new field is 42, so it overflowed
   the frame by 33 and the host died several calls later inside `zdump`'s own
   nibble-to-hex lookup, running on a pointer the overflow had wrecked.
   ► **A fixed log buffer in this file is a silent budget nobody is tracking.** The
   fault report was excellent and named the formatter, but the formatter was the
   VICTIM. Before adding a field to any of these lines, add up the one that is already
   there. `lb` is 512 now, with the arithmetic written down beside it.

   ### ▶ REFUTED THIS SESSION — DO NOT RE-TRY

   - **"It is just slow / it is the log."** Ten minutes with `wowquiet.txt` on (trace
     silenced, 8.6 KB of log instead of 500 MB): still `Game loading...`. The trace
     was a real defect and worth fixing on its own merits; it is not the blocker.
   - **"It stalls at 87% of ZARN0.SFS."** It reaches the same byte at 45 s and at 7
     minutes because that byte is the END OF A CYCLE. See above.
   - **The timer is being starved.** It is not: `delivered ~36/s` against a programmed
     36.41 Hz. The 29,657 `raises` were the PIT bug's phantoms, not real demand.

   ### ▶ ★★★★★ AND THEN THE STATE VARIABLE ITSELF — IT IS **PINNED AT 4**

   `pmwatch.txt` now takes `+<hex>` = **an offset from the guest's LE code-object load
   base**, resolved lazily (the base is not known when the file is parsed). That matters
   because an extended guest IS NOT LOADED AT A FIXED ADDRESS — ZAR came up at
   `0x03f70000` one run and `0x03b70000` the next, so an absolute address copied out of
   one log silently watches a neighbouring allocation on the next run. `+12c878` is
   copy-pasteable from a disassembly and stays right.

   With `pmwatch.txt = +4c878 +12c878 +12c8d0 +141310`, the state dword goes
   **0 → 1 → … → 4 and then never changes again** for the rest of the run. It is NOT
   cycling an attract loop. It is stuck in state 4 — and **4 is the one value the
   caption switch at obj1+0x11f9 has no name for** (it handles 1, 2, 3, 5, 6, 7, 9).
   The main loop then does exactly what the disassembly says it will: state != 0 and
   != 5, so fall into the tick-wait at obj1+0x1317, for ever.
   ⚠ ALSO SETTLED BY THE SAME RUN: the file image's absolute operands are **object
   offsets, not link addresses** — `[0x4c878]` is `obj3 + 0x4c878`, i.e.
   `codebase + 0x12c878`. The obj1-relative reading (`+4c878`) reads a constant
   `0x9504`, which is code. Two candidate interpretations, one run, no guessing.
   ▶ **NEXT SESSION STARTS HERE.** Every write of a constant to the state lives in one
   jump table at obj1+0x1824 (`jmp [eax*4+0x1628]`), plus obj1+0x1191 (→7),
   obj1+0x170b (→1), obj1+0x8b15 (→0) and obj1+0x13b77 (→4). **Find who selects arm 4
   and what state 4 is waiting for.** The table index comes from `eax`; a `pmbp.txt`
   breakpoint at obj1+0x1824 gives it directly.

   ### ▶ AND THE PIT FIX TRANSFORMED ZAR'S OWN TIMER

   Same 45 s run, before → after: `raises` **29,657 → 1,604** (= 35.6 Hz against the
   36.41 Hz it programs), `delivered/raises` **5% → 100%**, `owed_max` 64 (saturated)
   → 44. The "timer starvation" visible in every previous ZAR log was our phantom
   interrupts, not real demand. **It still loops**, so this was not the blocker either
   — but every future ZAR measurement is now taken on a guest whose clock is right.

   ### ▶ ★★★★★ STATE 4 IS **THE DEMO**, AND IT IS RUNNING. THE PICTURE NEVER LEAVES.

   Following the chain out of the state machine, all measured or read off the image:

   - The caption switch keys on the **request code**, not the state: request 6 →
     `LOADING DEMO...` → jump-table index 5 (`lea eax,[ebx-1]`, table at obj1+0x1628) →
     **state 4**. `ebx` is the return of the scene run loop **obj1+0x111a**, the function
     that contains the tick-wait at obj1+0x1317. So ZAR asked for the demo, entered the
     demo scene, and never leaves it.
   - **A VIDEO DRIVER IS INSTALLED AND IT IS THE RIGHT ONE.** `[0x5ea320]` (the driver
     pointer) = `0x046377e8` from the first tick, and the object there begins
     `0x5f414756` = ASCII **`"VGA_"`** — it is the `VGA_320x200` descriptor from
     `USER1.CFG`. Its put slot `[obj+0x80]` = `0x03ffc024` = **obj1+0x8c024**, the real
     mode-13h blitter (`mov ecx,0xfa00 / mov edi,0xa0000 / rep movsd`). Nothing here is
     a stub: mode SELECTION ran and chose correctly. (The descriptor lives at
     obj3+0x5e77e8, past the initialised region, so it was BUILT at runtime.)
   - **THE FRAME LOOP TURNS.** The per-tick body at obj1+0x1343 increments a frame
     counter `[0x4c870]`, and it climbs **monotonically and never resets**.
     ⚠ Its RATE varies wildly run to run — 0x34d (845) in one 45 s run against 0x3a (58)
     in another at a similar tick count — so do NOT quote a frame rate from it; the
     claim it supports is only "the loop is turning".
   - ⇒ The periodic 1073-read cycle is the demo **streaming**, not restarting.
   - **THE MODE IS NEVER SET.** The mode-set is obj1+0x98b0a (`eax=0x14`, then `0x13`),
     called from obj1+0x8cdf2. It forks on `[0x4b82c]`: non-zero → `call ptr [0x4b714]`
     (an alternate/external video dispatch), zero → the built-in path that issues
     `INT 10h`. **Measured at runtime: `[0x4b82c]=0` and `[0x4b714]=0`** — the fork is
     NOT taken, so the built-in path would have issued `INT 10h`. It never does.
     ⇒ **obj1+0x98b0a is never CALLED.** Nothing is ever written to 0xA0000 either.

   ▶ **NEXT SESSION STARTS HERE, and it is now a narrow question:** is obj1+0x8cdf2
   (the function containing the mode-13h set) ever entered, and if so what does its gate
   return? The gate is `mov eax,0x5e7724 / call 0x98de7 / cmp eax,1 / jne -> return 0`,
   and with `[0x4b82c]=0` that resolves to **obj1+0x9cf2b**. A breakpoint at obj1+0x8cdf2
   and obj1+0x9cf2b answers it in one run.
   ⚠ **`pmbp.txt` still takes ABSOLUTE addresses and the load base moves every run** —
   it needs the same `+<hex>` treatment `pmwatch.txt` just got, but it ARMS breakpoints
   (patches the guest) rather than reading passively, and arming happens at several
   points, so it is real work rather than a two-line change. Do that first.

   ### ▶ NEXT

   ▶ **Find what state 4 is waiting for** (above). `INT 10h` is still called ZERO times
   and `mode sets: none`, so video init is not merely failing — it is never entered.
   ⚠ **A LEAD WORTH ONE MEASUREMENT FIRST:** `p3da_reads=481,618` against
   `vbl_edges=94` in 45 s. The guest hammers the VGA status register ~10,700 times a
   second and completes a retrace wait only **twice** a second, where real hardware
   gives 70. Either an `in al,0x3DA` costs us ~90 µs, or the polls arrive in bursts and
   the edge counter is fine. **Measure which before acting** — a counter's layout is a
   claim, and this one has two very different readings.
   ⚠ Cheap and now possible: `zarargs.bat 180 -NoVESA2` / `-NoSound`. Neither has been
   run — the first attempt failed because `bmqueue.sh skyroads` had `rmdir`'d `C:\game`
   out from under it (rt.bat's `:game` arm wipes the directory; re-run `bmqueue.sh zar`
   to restore it).
   ⚠ The reflected-dispatch line now carries `from <CS>:<EIP> lin=<linear>` — session
   58's open question. For ZAR it names the Watcom CRT (`obj1+0xAEC87` read,
   `obj1+0xB5C2C` clock), not game code, so the game's own frame needs a stack walk.
   ▶ And the new STAGE2 line answers "what is this guest DOING?" in one place:
   `21/2c=533,808` — ZAR asks its own clock hook **11,862 times a second**.

   ---

   ## ★ SESSION 58 — 86.9% (unchanged, and four real defects were fixed anyway)

   ### ▶ ★★★★★ RESUME HERE: ZAR BANNERS AND LOADS. IT DOES NOT RUN.

   ```
   DOS/4GW Protected Mode Run-time  Version 1.97
   Copyright (c) Rational Systems, Inc. 1990-1994

   Z.A.R.  Demo Version  v1.02 (net:v1.00)  Apr 06 1998 02:47:03
   Copyright (C) 1997,1998 Maddox Games Ltd., Auric Vision Ltd.

   Game loading...
   ```

   That is on the XP desktop, in our VDM window. It was a 47 MB `#GP` loop this
   morning. **It still does not run** — see "where it stops" below.

   ⚠ **THE DAY'S GOAL WAS >95% AND THE SCORE DID NOT MOVE AT ALL.** That is the
   honest outcome and it is worth understanding rather than explaining away:
   `guest-zar` is BINARY (playable or not), everything else touched was already at
   1.0, and the user's own framing is the right one — *"if ZAR doesn't work then
   NTVDMEX doesn't work"*, because the IFEO key routes EVERY DOS launch through us,
   so a program stock runs and we refuse is a REGRESSION ON THE USER'S MACHINE, not
   a missing feature. ▶ **The score model under-prices this**: `guest-zar` sits at
   w3 in the DOS section, priced like a third game, when it is really evidence for
   or against the superset claim the whole project rests on. Worth a deliberate
   model change; it is the user's call, not one to slip in.

   ### ▶ FOUR DEFECTS FIXED, IN THE ORDER THEY WERE FOUND

   **1. A 32-bit DPMI client got a 16-bit exception frame.** The rule was already
   written in our own source for INTERRUPT frames (*"the frame width follows the
   CLIENT'S MODE, not the handler selector's D bit"*) and had never been applied to
   the EXCEPTION frame. Confirmed against the binary: DOS4GW's `#GP` handler reads
   `[bp+0x12]`/`[bp+0x16]` = frame `+0x0C`/`+0x10`, the DPMI **32-bit** EIP and CS
   slots, and leaves by `66 cb` (RETFD, eight bytes). Our 16-byte frame had nothing
   at `+0x10`, so it loaded DS=0 and faulted on its own first memory read, for ever.
   47,089,105 bytes of log → 252,718.

   **2. ★★★★★ OUR INT-SITE PATCHER CORRUPTED A CALL — THIRD INSTANCE.**
   `dpmi_patch_code_region` rewrote a call's DISPLACEMENT:
   ```
   file  cs[0x5682] = e8 cd e4   call 0x3b52   (rel16 = 0xe4cd)
   guest cs[0x5682] = e8 c4 c4   <- `cd e4` read as INT E4h, made a BOP
   ```
   The x86len vote passed it correctly on its own terms — everything in front is
   DATA (zeros), and an odd-aligned stream decodes `00 e8` as `add al,ch` and lands
   exactly there. **The vote is a heuristic about where instructions START; it
   cannot know a region is not code at all.** The corrupted call landed at `0x1b49`,
   MID-INSTRUCTION, decoded as `mov ah,al / les ax,[di]`, RESYNCHRONISED at
   `0x1b4f`, and so skipped both the `[0x34]` test and the `int 15h` the real
   routine begins with — which is why neither ever appeared in the log and why two
   earlier readings of this bug were wrong.
   ⇒ **THE 64K SCANNER ALREADY HAD THE ANSWER AND THIS SCANNER NEVER GOT IT**:
   *evidence only — patch a vector only with a guest that provably needs it and a
   service arm to receive it.* Now restricted to the vectors `dpmi_service_pm_int`
   actually implements: `{08,10,11,15,16,1A,21,2F,31,33,41}`. Not patching is SAFE
   (a raw INT in PM is serviced out of the `#GP`, session 34) — patching is an
   OPTIMISATION and it has now cost three guests (Doom s21, CALC/TASKMAN/CARDFILE
   s55, ZAR s58).

   **3. `INT 31h AX=0200h/0201h` — get/set REAL-MODE interrupt vector.** DPMI 0.9
   core, and we had NEITHER half while the protected-mode twins `0204`/`0205` worked
   all around them. DOS/16M reads all 256 and installs 39; every one was refused.
   The IVT is the store, not a shadow table — V86 `int nn`, `INT 21h AH=35h` and
   this call must not disagree. Doom exercises it (99 get, 2 set).

   **4. `INT 21h AX=FF80h` — "lock this region".** DOS/16M requires it and reads
   CF=1 as fatal (`jae ok / push 0x22 / call fatal`; 0x22 is message 34 = `DOS/16M
   error: [34] DPMI host error (cannot lock stack)`). Answered CF=0, for the reason
   our own `INT 31h AX=0600` arm already gives: guest memory here is never paged, so
   a lock is already true. Scoped to FF80h exactly.

   **5. ★★★★★ THE SECOND PM RUN LOOP HAD NO FAULT ARM — this is the one that got
   the banner.** `dpmi_dispatch_to_pm_handler` runs the client's own INT handler to
   completion in a NESTED protected-mode loop. It handles PMRET, event 3 and I/O,
   then hands everything else to `dpmi_service_pm_int` — and never had an arm for
   THAT HANDLER FAULTING. The kernel reflects such a fault onto our fault-site stubs
   as usual, but here the BOP was read as an interrupt: `dpmi_service_pm_int` saw the
   stub's own `C4 C4 57` and dispatched it to the WOW handler, where **0x57 is the
   WOW callback id — a genuine collision with `DPMI_FAULT_BOP`**. Verdict
   "UNIMPLEMENTED, STEPPED OVER", exception never delivered, guest re-executed the
   site for ever. Measured: `cs=0x017f == g_dpmi_flt_code_sel`, `eip=0x694 ==
   DPMI_FAULT_SITE(13)` — a `#GP` inside DOS/16M's INT 21h handler while ZAR asked
   the DOS version. **That is why ZAR printed nothing**: the call that would have
   printed was the one being stepped over. Now handed back to the main loop, which
   owns the whole delivery path. ⚠ Cannot affect WOW: a real WOW trampoline lives in
   a guest 16-bit code selector, never in `g_dpmi_flt_code_sel`. Doom fires the new
   arm ZERO times.

   ### ▶ AND ONE FEATURE: `dosenv.txt`

   The guest environment was a hardcoded four (COMSPEC/PATH/PROMPT/BLASTER), so a
   DOS program configured through its environment **could not be configured at
   all**. One `NAME=VALUE` per line, `#` comments. MEASURED, not asserted — the host
   dumps the block back OUT OF GUEST MEMORY after building it. ⚠ It does NOT fix
   ZAR (DOS/4GW faults before reading `DOS4GVM`); kept because the gap is real.
   ⚠ Not applied on the WOW path: that block is krnl386's and it finds its own
   executable by scanning to the double NUL.

   ### ▶ WHERE ZAR STOPS NOW — THE NEXT THREAD

   It loads **~4.8 MB of a data file and then stops progressing**: at 45 s the last
   read is `pos=0x49d454` (854 reads); at 130 s `pos=0x454c28` (1118 reads).
   - **It never calls `INT 10h` AT ALL** — no video init is ever attempted.
   - It settles into polling **its own** `INT 21h` hook (`0x027f:0x0084`) for
     `AH=2Ch` with NO `0302` round trip — ~156,000 iterations against two buffers
     (`0x04250030`, `0x0425185a`).
   - Its idle is a **wait-for-next-tick** loop at `0x03b71317`, which spins only
     while `elapsed == 0`. NOT a hang: `pmwatch` shows the counter advancing one per
     ISR (`0x86→0x87 … 0x680→0x681`), `done=1` every time. **Our timer has the
     effect the guest is waiting for.**

   ▶ **NEXT STEP:** instrument the TRANSITION where the reads stop, not the steady
   state. Catch the LAST `AH=3F` read's caller and follow it forward — that is a
   different dig from anything tried today, and the steady-state sampling has
   nothing left to give.

   ⚠⚠ **REFUTED IN SESSION 58 — DO NOT RE-TRY.**
   - **CPU speed.** A 1998 game calibrating against 3.3 GHz is the classic failure.
     `cpuspd.txt=6` (233 MHz, Pentium MMX): identical behaviour.
   - **The COM port.** The banner says `(net:v1.00)`, ZAR.CFG configures a modem on
     0x3F8 with an `"ATZ"` string, and s56 made our serial VDD claim that port and
     compute the equipment word from it — we now advertise TWO serial ports where we
     advertised none, and a modem init awaiting a reply would hang exactly like
     this. `net ComPortNum -1`: identical. (ZAR.CFG restored, share and box.)
   - **Our `AH=2Ch`.** `GetLocalTime`, genuinely advancing — and the guest does not
     even ask us; it services `AH=2Ch` in its own hook.
   - **`DOS4GVM=@ZAR.VMC`** (the game's own launcher sets it): reaches the guest,
     verified in the env block; DOS/4GW faults before reading it.
   - **`INT 15h AH=88h`.** Chased TWICE and wrong both times. It is never called —
     DOS/16M installs its own PM INT 15h handler (its 33 `AX=0205` calls) and
     answers internally. ⚠ The `0x3C00` "matching the XMS pool" answer IS still a
     real double-count defect (a real HIMEM reports 0 once it owns the memory) —
     worth fixing deliberately, on its own merits, with Doom re-gated.
   - **VESA.** `guest-zar` is filed as "needs VBE 2.0 hi-colour + LFB". **It does
     not.** ZAR's own `USER1.CFG` ships `VGA_320x200` (mode 13h, supported since M3)
     and it dies long before any video call. Do not plan the VESA work around ZAR.
   - **MZ+LE page layout.** `DOS4GW.EXE` is a PURE MZ image (`e_lfanew` is garbage);
     the load module is CONTIGUOUS and `file = guest + 0x9B10` holds throughout. An
     earlier note here claiming otherwise was wrong — the real error was
     disassembling from an unaligned offset.

   ### ▶ INSTRUMENTS ADDED (all generic, all earned by getting something wrong)

   | knob / output | what it answers |
   |---|---|
   | `dsprobe.txt` | named DS offsets dumped at every `#GP` — a guest's branch state |
   | `csprobe.txt` | the same against CS — **guest code vs the file image**, which is what found the patcher corruption |
   | `@ss:sp` at a `#GP` | who CALLED the faulting routine, off the guest stack |
   | `@ds:0000`, `csbase`, `code[ip±0x20]` | locate a fault in a binary instead of guessing |
   | `pmap` line in the WOWBOP report | **is this BOP ours or the guest's own `C4 C4`** — works for ANY guest; the old file-image check needed `g_wow_nmod` |
   | `code@eip` on ASYNC-PM | what code an injected tick interrupted — turns "looping at X" into "polling Y" |
   | `scripts/bm/zarlong.bat` | a run with REAL wall-clock, `nolog` for long ones |
   | `scripts/bm/zarout.bat` | ZAR's stdout under BOTH hosts, in text |
   | `rigshot arrange <exe> left\|right` | move windows by OWNING PROCESS (ours and stock share captions) |
   | `scripts/bm/sxs.bat`, `scripts/bmsxs.sh` | a BATCH of guests up under both hosts at once, left running |

   ### ▶ TRAPS LEARNED THE HARD WAY THIS SESSION

   ⚠⚠ **THE SHARE ROOT IS PART OF THE HARNESS, NOT SCRATCH SPACE.** Tidying it
   695 → 32 entries broke the rig TWICE: `wowtry.flag` is the **WOW opt-in** (its
   absence made the whole shelf report *"16-bit Windows not supported"*), and the
   root-level `.bat` files are called BY PATH from `rt.bat` and repo scripts, so
   `bmqueue.sh doom` timed out with no result log. All 158 `.bat` restored; root is
   ~203 entries with the ~500 `.bmp`/`.log`/stale `.txt` still archived in
   `archive/2026-09-08-pre-s58/`. Check anything moved against `rt.bat`,
   `runwatch.bat` and every `%RES%\…` reference in `scripts/` — not just `src/`.

   ⚠⚠ **NEVER `start /wait` OUR HOST.** Without the `autoexit` marker it keeps its
   window open after the guest ends, so `/wait` never returns and the box wedges —
   it took a `controld kill` to clear, twice.

   ⚠ **THE HARNESS ITSELF STARTED LYING.** `rt.bat` caps a run at 45 s and the
   compare scripts shoot at ~30 s. Fine while ZAR died in half a second; once it got
   to "Game loading..." they cut it off mid-load and the evidence read *"no video
   mode set"* when the truth was *"not finished yet"* (`HEADLESS: deadline
   reached`). When a guest starts working, re-check the instrument's assumptions.

   ⚠ **AN INSTRUMENT THAT FAILS BY PRINTING A PLAUSIBLE WRONG ANSWER** is worse than
   one that fails loudly: the first env-block dump appended into the running report
   bounded by `p < base + 3800`, which was already passed, so every readable byte
   was dropped and it printed `[......]` — indistinguishable from an EMPTY
   ENVIRONMENT.

   ### ▶ REGRESSION BASELINES AT `4768150` (all re-run this session)

   - off-VM battery: **62/62 + 27 + 27 + 48 + 23 + 17, 0 failed** (8 new `dosenv` checks)
   - Doom: **all ten init markers, `STAGE2: complete`** — and it EXERCISES the new
     code (99 `getRMvec`, 2 `setRMvec`, 195 sites correctly left unpatched across 21
     regions), while firing the new fault arm ZERO times
   - WOW gate: **110 / 113 / 32 · 0001:229C**, the s56/s57 baseline exactly

   ⚠ **CALC and WRITE were left on the rig under BOTH hosts awaiting a verdict and
   never judged** — the day went to ZAR instead. `guests` is still the heaviest
   lever at +0.26 per confirmation, and the user's standing rule is side-by-side,
   which `bmsxs.sh` now does for a whole batch.

   ---

   ## ★ SESSION 57 — 85.4% (unchanged, and that is the honest number)

   **One feature: `DialogBox` finally does the thing that defines it — it does not
   return until `EndDialog`.** Session 56 diagnosed this and deliberately did not
   build it; this is the build. `src/wow/wowdlg.h` is the whole loop.

   ### ▶ ★★★★★ TASKMAN PUTS UP ITS TASK LIST, AND CANCEL CLOSES IT

   Measured on the rig, every step out of the guest's own log:

   ```
   DialogBox (MODAL) "Task List" ... ★ MODAL: the caller is PARKED here
   WOWDLG: MODAL 0x0140 -> WM_INITDIALOG -> 0x0b9f:0x00ba
   WOWDLG: MODAL 0x0140 -> hwnd=0x0140 msg=0x000f  (WM_PAINT)
   WOWDLG: MODAL 0x0140 -> hwnd=0x0140 msg=0x0008  (WM_KILLFOCUS)
   WOWDLG: MODAL 0x0140 -> hwnd=0x0140 msg=0x0111  (WM_COMMAND -- the click)
   EndDialog 0x0140 result 0x0000 -- ★ THIS ENDS A MODAL LOOP
   WOWDLG: MODAL 0x0140 ENDED -- DialogBox returns 0x0000 after 4 message(s)
   STAGE2: complete
   ```

   The dialog is **centred at (677,402), painted, six buttons, its own taskbar
   button**, and the task stays alive until Cancel is clicked — where s56
   measured *no window at all*, because `DialogBox` returned immediately and
   `WinMain` ended there.

   **THE MECHANISM NEEDED NO NEW MACHINERY.** It is the `EDITLOCK → EDITFILL →
   LocalUnlock` chain (s44) with a continuation rule: `wowcall_enter` parks the
   guest *exactly as it will be resumed*, so every turn of the loop restores and
   re-parks the same context, and when the loop runs out the guest is standing
   where `DialogBox` returns to. Nothing accumulates, no EIP is written by hand.

   ### ▶ ★★★★★ THE RUN NAMED THE BLOCKER, AND IT WAS `#32770`

   The loop was written, correct, and the dialog was **inert** — the click landed,
   the button redrew itself, and nothing arrived:

   ```
   WOWDLG/win32: msg=0x0201 hwnd=0x003900e4 -> win16 0x01c0
   ...  Win16 queued 0x00000000
   ```

   `#32770` was in `g_wu_sysclass[]`, and that list's rule is *"a system class is
   the OS's own, use it as-is"*. **That rule is right for four of the five and
   wrong for the fifth, and the difference is whether the OS's implementation can
   reach OUR code.** `MDICLIENT`/`EDIT`/`LISTBOX`/`COMBOBOX` implement the whole
   control and answer for themselves. `#32770` implements a **dialog manager**,
   and what it does with a click is call the window's `DWLP_DLGPROC` — a slot
   that cannot hold 16-bit code. So `wowwin_proc` never ran, no `WM_COMMAND` was
   ever relayed, and the dialog could not be dismissed. It is now our class.
   ⚠ The upgrade not taken, written down: a **32-bit** `DLGPROC` installed with
   `SetWindowLongPtr(DWLP_DLGPROC)` would keep the OS's dialog manager — tab
   order, mnemonics, ESC=IDCANCEL, the default button. That is real behaviour we
   are doing without, and it needs an undocumented structure this host will not
   guess at while a plain answer works.

   ### ▶ ★★ A MODAL DIALOG IS CREATED HIDDEN AND SHOWN AFTER `WM_INITDIALOG`

   A dialog procedure's first act is routinely to move itself — TASKMAN centres
   its Task List with `MoveWindow(..., bRepaint=FALSE)`. On a window that is not
   yet visible that costs nothing; on one we have already shown, Win32 documents
   it as *no repainting of any kind*, and the rig showed exactly that: the
   dialog's pixels **left behind at its old position**. Real USER creates, sends
   `WM_INITDIALOG`, then shows. So do we.
   ⚠ **AND A WARNING ABOUT READING SCREENSHOTS ON THIS RIG.** The desktop keeps
   stale pixels from earlier runs, so "the client area shows wallpaper" was read
   as a paint defect **twice** before a shot taken *after forcing a repaint*
   (`rigshot fg`, then shoot) showed a perfectly painted dialog. Force the
   repaint before believing any window's contents.

   ### ▶ THE LOOP CANNOT HANG, AND THAT IS A TESTED PROPERTY

   s56's reason for not building this was *"a half-built modal loop that never
   returns is worse than an honest immediate return"*. So the exit decision is a
   **pure function** (`wowconv_modal_exit`, in the file the battery already
   pins): four facts in, five verdicts out, and `wow_test.c` enumerates all
   sixteen combinations — exactly one keeps pumping, the other fifteen leave.
   The four exits are EndDialog, the window is gone, nothing to dispatch to, and
   the input wait expired (`wowidle.txt`, the same knob `GetMessage` honours).

   ### ▶ WHAT THIS IS WORTH, SAID PLAINLY: **+0.00**

   `guests` scores `partial` at 0.5 and `done` means USER-CONFIRMED, so the score
   does not move until a human looks at TASKMAN — and it should not, because
   **the Task List is EMPTY**: TASKMAN enumerates tasks with `GetWindow`, which
   answers 0. The feature also unblocks the sub-dialogs the user named in s53
   (Minesweeper's Preferences, Solitaire's Options/Deck) but those guests are
   already `done`, so that is quality, not score.
   ▶ **NEXT, in score order**: `breadth`'s 29 remaining services (+0.70,
   mechanical), `stdio`/#131 (+1.16, a located bug), `flat-thunks` (+1.96).
   TASKMAN itself needs window enumeration before it is worth confirming.

   **Regressions all clean at `0f6c52f`:** WOW gate `110/113/32 · 0001:229C`
   (s56 baseline exactly), Doom 3,501,081 bytes with all ten init markers and
   `STAGE2: complete`, the seven-guest shelf sweep unchanged (including
   TERMINAL's `Default Serial Port` dialog), off-VM battery 0 failures with 16
   new checks in `wow_test.c`.

   ---

   ## ★ SESSION 56 — 83.1% → 84.3%

   **Twenty-four commits, all rig-gated. Resumed from session 55's pause; the user chose
   the MIXED option, then asked for both halves of the follow-up.**

   ### ▶ ★★★★★ CALC IS A CALCULATOR THAT CALCULATES — AND s55 NAMED THE WRONG PASS

   Session 55 left one question open: which pass re-patched CALC's `INT 0x39`
   site, given the new scanner guard skipped it. **It was not a scanner.** It is
   the `#GP(IDT) is a RAW INT` arm (`src/host/main.c`), which believes the CPU's
   error code and rewrites the site to a BOP. It was in s55's own log all along:

   ```
   EXC: #GP(IDT) is a RAW INT 0x39 at 0x0b77:0x05c6 lin=0x03d15b66
        -- servicing + patching                              x120,673
   ```

   The error code proves the byte pair really is an `INT`. **It does not prove it
   is an INTERRUPT.** And the emulator was there the whole time — `WIN87EM.DLL`
   loads and installs its own PM handlers across the range, which the log states
   plainly and nobody had read:

   ```
   set PM vector 0x34..0x3b = 0x0b57:0x0dd5
   set PM vector 0x3c       = 0x0b57:0x0d95   (segment override)
   set PM vector 0x3d       = 0x0b57:0x0d8b   (FWAIT)
   ```

   We never called it. `0x34..0x3F` is now **REFLECTED** — not patched, not
   serviced — with a real interrupt frame on the guest's own stack.

   ⚠⚠ **THE OBVIOUS FIX IS WRONG AND THE CODE NOW SAYS SO.** Routing it through
   `dpmi_dispatch_to_pm_handler` looks right and is not: that resumes at a fixed
   `sEIP + 2`, and an FP handler **adjusts its own return address** to step over
   the x87 operand bytes that follow the `CD nn`. Coming back at `sEIP+2` drops
   the guest onto its own operands. Every host-side mechanism we have (BOP +
   trampoline, or a service arm that IRETs) throws that adjustment away.

   ★ **MEASURED**: 716 FP instructions reflected in one run across five vectors;
   log 137 MB → 2.5 MB (the 120,670-iteration heal loop is gone because nothing
   patches the site any more); and **clicking `2` then `sqrt` displays
   `1.414213562373`**, correct to every digit. Doom is **inert** for this path —
   0 reflects.

   ⚠ **TWO s55 CLAIMS REFUTED, both by measurement.** "TASKMAN and CARDFILE end
   the same way" was reasoned, not measured. TASKMAN: 0 FP reflects, was never an
   FP casualty. CARDFILE: 0 FP reflects, and a different bug entirely (below).

   ### ▶ ★★★★★ WE NEVER LET THE CPU FAULT FOR US — CARDFILE'S SILENT DEATH

   A Win16 code segment is **loaded on demand, and the demand is a fault**:
   krnl386 marks the selector not-present (`acc 0x7b`) and services the #NP by
   reading the segment in and committing it present (`0xfb`). This host reflects
   that exception correctly and always has — **eight succeeded in the very run
   that found this.** But `wowcall_enter` reached a 16-bit procedure by *writing
   CS:EIP into the VDM TIB*, and a TIB whose CS names a not-present selector is
   not a fault, it is **a VDM that does not come back**. CARDFILE registered its
   class, created its window, and died on the first instruction of its own
   `WM_CREATE`:

   ```
   INT31h AX=0x000c BX=0x0b7f  sel 0x0b7f <- desc ... acc 0x7b   (NOT present)
   WOWCALL: -> 0x0b7f:0x0000 (hwnd=0x0140 msg=0x0001) -- ENTERED, depth 1
   <log ends>
   ```

   The contrast was in the same log: `0x04a7` gets the **same** `0x7b` descriptor
   and is later committed present, and its WOWCALL returns normally. `0x0b7f`
   never is, because nothing ever faulted on it.

   ⇒ Enter on a **`RETF`** with the target pushed as a far address — one byte, at
   offset 4 of the callback paragraph we were using three bytes of. The RETF is a
   normal instruction in a segment that IS present, so its #NP is an ordinary
   restartable fault. Gated on the present bit; WRITE uses it **zero** times,
   which is how we know it is narrowly scoped.

   ### ▶ ★★★ "NOT ENOUGH DISK SPACE" ON A 243 GB DISK WAS A MISSING DOS CALL

   With the segment loading, CARDFILE got far enough to **diagnose itself in
   English** (the `implement MessageBox first` payoff, fifth time) and the log
   named it: `INT21h AH=0x5b (PM thunk TODO)`.

   `AH=5Bh` is `3Ch` with **CREATE_NEW**, and that difference is the whole point:
   the caller invents a name, tries it, retries on error 50h. **Unimplemented, it
   does not look like a missing call — it looks like a full disk.** WRITE hits the
   same call exactly once. s55 filed this under *runs but lies* and went hunting
   among krnl386's stepped-over thunks (`0xc8`, `0xc6`, `0x87`/IOCTL 4408h); it
   was none of them.

   Both guests now run: **`Cardfile - (Untitled)`** 430x323 with its Card View
   bar, navigation arrows and `1 Card` counter; **`Write - (Untitled)`** 1260x742
   with its full menu, caret and scrollbar.

   ### ▶ ★★★★ #9 CLOSED-BUT-FOR-PASSTHROUGH: A REAL UART, AND A STROBE

   The gap was never "INT 14h is missing" — it answered since GH #45. It is that
   **there was no UART**: `0x3F8..` was unclaimed (a guest driving the hardware
   read 0xFF from every register), receive returned TIMEOUT unconditionally
   because there was nothing to receive from, and **transmit wrote to `g_serial`
   — the host's own debug channel**, interleaving guest bytes with our log.

   `src/vdd/vdd_comm.c` is an 8250/16550A plus the parallel data/status/control
   at 0x378. INT 14h is claimed **by the VDD**, so the BIOS and the registers
   cannot disagree — which is the COMM.DRV failure shape.

   ★ **LOCAL LOOPBACK IS THE CENTRE OF IT**: MCR bit 4 needs no cable, no peer
   and no host device, so it is the one part of a UART establishable *completely*
   on a bare rig — and it is what every driver uses to decide a port exists.
   ★ The parallel byte leaves on the **RISING EDGE OF STROBE**, not on the data
   write; getting that wrong prints every byte twice.
   ⚠ The equipment word is now **computed** (`bios_equipment_word`): both INT 11h
   arms carried a comment reading *"one floppy, 80x25 colour, ONE SERIAL, one
   parallel"* over a constant whose serial bits were **0**. The comment had been
   wrong at both sites, and the 0040:0000 note quotes the same wrong reading.

   Rig: `EQUIP=4421 SER=2 / BDA COM1=03F8 / SCRATCH OK / LOOP TX=5A RX=5A OK /
   DTR>DSR RTS>CTS OUT1>RI OUT2>DCD all OK / BIOS14 TX=5A RX=5A OK / LPT1
   STATUS=D8 OK`, and both spools hold what the guest sent.
   ▶ **NEXT**: TERMINAL now puts up its main window AND its `Default Serial Port`
   dialog, but touches the UART **0 times** — it is waiting on that dialog.
   Driving it is the end-to-end test this item still lacks.

   ### ▶ ⚠⚠ TWO INSTRUMENTS WERE LYING, AND BOTH WERE CAUGHT BY A NUMBER

   * **`doomrun.bat`/`rt.bat` never deleted `ntvdmhost.log`, and the host APPENDS.**
     Every `result_*.log` was *whatever ran last* followed by this run. A Doom
     regression taken after a live CALC session came back **7,234,588 bytes**
     against a 3,503,139 baseline, and the "Doom" FP counts and PM vector table
     read out of it were **CALC's** — selector `0x0b57`, WIN87EM's handler, in a
     log for a guest that never loads WIN87EM. The doubled size was the ONLY
     hint; anything the same size would have passed silently.
     ⚠ Only runners that start a FRESH host get the delete. The interaction
     scripts (pbmin, minetest, pbtools, overlap, playtest, savetest, wowkeys)
     attach to a running host and the accumulated log is the point.
   * **`wow_test.c`'s `MAXDEF 512` silently dropped macros**, so 27 checks
     reported `macro CDIB_ARG_HDC is not defined` for a macro defined at
     `wowgdi.h:695`, in a file the scanner reads. s55 crossed the cap; the tail
     had been read as known-bad ever since. Cap raised and **overflow is now
     fatal**. Measured both ways: 27 FAILs at `49d939c`, 0 now. The whole off-VM
     battery is clean for the first time in a session.

   ### ▶ ★★★★ THE TRACE WAS THE PROBLEM — 158 MB → 3.3 MB

   `log.h`'s own note on `g_log_quiet` prescribed this in advance: *the answer is
   not to ship the silencer on, it is to stop writing a kilobyte per BOP.*
   TERMINAL made it unanswerable — **a grep over its log TIMED OUT**, which is
   the point at which an instrument has stopped being one.

   ★ **THE CLOCK IS NOT THE BUG**, checked before any code was written:
   `GetCurrentTime` returned **321 DISTINCT values stepping ~16 ms**, so it
   advances correctly and the guest polls it ~485×/tick. `FUNC=0x6d` turned out
   to be **PeekMessage** — TERMINAL is running an ordinary PeekMessage +
   GetCurrentTime idle loop. Nothing was wrong with the guest at all.

   ⚠⚠ **THE FIRST DESIGN WAS THE WRONG SHAPE, AND THE RUN SAID SO.** It folded
   *runs* of the same call back to back — which sounds like the same thing and is
   not: **the longest identical run in that log is 23.** The calls are
   interleaved into a repeating CYCLE, so the fold never fired once
   (`folded=0x0`) and the log came back **BIGGER**, 177 MB. The pattern is *one
   function called an enormous number of times*, not *one function repeated*.
   ⇒ Cap **per function**: full dump for 256 calls, then verdict-line-only, then
   at 4096 counted and not traced. Both stages announce themselves by id.

   ⚠ **KEEPING THE VERDICT LINE IS LOAD-BEARING.** `bmwow.sh`'s signature is a
   COUNT of those lines; folding whole blocks would have moved the gate with no
   behaviour change — a self-inflicted regression signal, the exact fault class
   the fold exists to fix. The hard cap sits 48× above the gate's traffic.

   ⚠ Wired into the WOW32 block's own flushes only, never `log_append`: the mute
   spans BOPs, and a global one would swallow a fault or a PM interrupt logged
   *between* them. Seven sites an over-wide edit had converted outside the block
   were reverted for that reason.

   ### ▶ ★★★★ krnl386 THOUGHT IT WAS ON DRIVE A: — WOW32 0xc8

   Named from its call site with the method this repo already prescribes,
   `nedis.py guest/wow/KRNL386.EXE --wowfunc 0xc8`. It is krnl386's **INT 21h
   AH=0Eh (select default drive)** arm, and `mov byte ptr [0x2a0], al` means
   **whatever we return becomes krnl386's answer to "what drive am I on".**
   Unimplemented it got the sentinel `0` — a perfectly good drive index — so
   krnl386 cached **A:** after every select while `AH=19h` went on saying C:.
   Two routes to one fact, disagreeing, no error anywhere.

   ⚠ **AND IT CORRECTS A RECORDED NOTE**: `[0x2a0]` is NOT the per-drive table
   and is not *"written from none"*. It is the cached CURRENT DRIVE, written from
   exactly two places (here, and the `AH=19h` arm at `seg1:0x533a` reading the
   real one through `[0x275]`) and invalidated with `0xFF` at `seg1:0x5717`. The
   per-drive table is the separate `[0x2a2 + bx]` read at `seg1:0x51ae`.

   ⚠ All three routes now read `DOS_CURRENT_DRIVE` (`dos_layout.h`). Returning
   the REQUESTED drive would claim a switch that did not happen — `AH=0Eh`
   accepts a select and ignores it.

   ★★ **THE GATE MOVED AND IS ACCOUNTED FOR TO THE CALL: 85/113/57 → 110/113/32.**
   +25 serviced / −25 unimpl is EXACTLY the 25 `SetCurrentDrive` calls the gate's
   guest makes; declines and `0001:229C` unchanged. `bmwow.sh`'s baseline is
   updated **with that accounting beside it**, so the next session reads a step
   rather than drift.

   ▶ **NEXT ID, NOT TAKEN**: `0xc6` (15 calls in TERMINAL, 18 in WRITE). Its call
   site is at `seg1:0x4792`, reached from a `test cl,1` branch whose `test cl,0x10`
   sibling calls `0x9c WowCursorIconOp`, and `0x7c` precedes both — all three take
   the same handle and `0x3e8`. That is not enough to name the RETURN, and s55
   already measured that 0 avoids its failure path, so it was left alone rather
   than guessed.

   ### ▶ ★★★★★ A GUEST THAT CANNOT START NOW SAYS SO — WowMsgBox (0x84)

   krnl386 has **its own error reporter** and we were stepping over it. A failed
   launch calls `WowFailedExec` (0x9d) → `WowMsgBox` (0x84) with the text →
   `ExitKernelThunk`. All three unimplemented, so a guest that could not start
   **simply vanished**, with the reason sitting in a string nobody displayed.
   [[wow-real-hwnd-frontier]] has said *implement MessageBox FIRST on any new
   guest* for four guests running; this is the one that covers every guest which
   dies **before** it can put up its own.

   ★ **WINFILE — THE LAST `todo` — IS DIAGNOSED, AND IT IS NOT A HOST DEFECT.**
   On screen: caption *"Can't run 16-bit Windows program"*, body *"Cannot find
   file C:\WIN16\WINFILE.EXE (or one of its components)…"*. Its module table is
   `[VER, KERNEL, GDI, USER, KEYBOARD, COMMDLG, SHELL, SCONFIG, COMMCTRL]` — the
   **Windows for Workgroups build**, statically importing `SCONFIG.DLL`, which is
   not on the box (`open "SCONFIG.DLL" -> CF=1 gle=2`, and the ONE module the
   resolver could not path, while VER/LZEXPAND/COMMDLG/COMMCTRL all resolved).
   **An asset gap.** ⚠ Its recorded blocker named ShellExecute and CreateWindowEx
   as *"both still unimplemented"* — both were implemented later in the SAME
   session that wrote the note (s55, `aba3783`). Ledger corrected.

   ⚠⚠ **TWO MISTAKES OF MINE, BOTH CAUGHT BY THE INSTRUMENT, BOTH WORTH KEEPING:**
   * **The first cut BLOCKED BEFORE IT RECORDED.** `MessageBoxA` does not return
     until a human clicks and the arm's log block is not flushed until it does —
     so the box appeared and the log held **nothing**, not even the harness's own
     arg lines. *An instrument that blocks before it records is not an
     instrument.* The line is written first now, with both candidate slots on it.
   * **And that line immediately refuted my slot guess.** I had reasoned "take
     whichever slot has text as the body"; one run printing both showed
     `arg4="Can't run 16-bit Windows program"` and `arg8="Cannot find file …"` —
     the SHORT one is the caption, the LONG one is the text. My version put the
     explanation in the title bar. Plausible, and backwards. Now pinned from
     data, with both slots still logged so it stays checkable.

   ⚠ `LOG_PATH` moved from `main.c` (line 70, **after** the headers that want it)
   into `log.h` beside `log_append`. One definition, not two.

   ### ▶ ★★★★★ USER-REPORTED: TRAY ICONS STACKING UP — AND THEY WERE LIVE HOSTS

   *"When a WoW16 window exits, it leaves its tray icon behind. They are stacking
   up in the tray."*

   ⚠⚠ **I ANSWERED "THEY ARE NOT GHOSTS, THEY ARE LIVE PROCESSES" AND THAT WAS
   WRONG.** The user refuted it with one observation — *"why do they all
   disappear when the mouse hovers over them?"* — which is the **textbook ghost
   signature**: Explorer reaps a tray icon only after its owner is dead, and only
   lazily, when the mouse crosses it. A live process's icon does not do that.
   Measured afterwards and it is not close: **5 icons in the tray, `tasklist`
   reporting 0 ntvdmhost.exe.**

   ★ **BOTH FACTS WERE REAL AND THEY COMPOSE — that is what I missed.** The
   lingering host is measured too (launch CALC, click its X, window gone, PID
   1488 still there). But a lingering host is what the NEXT
   `taskkill /f /im ntvdmhost.exe` kills — and every launch script runs one,
   against **all** instances. An externally terminated process cannot run
   `NIM_DELETE`, so each becomes a ghost. Reproduced end to end: 1 host + 6 icons
   → `taskkill` → **0 hosts, still 6 icons**. I ran that kill ~30 times today.
   ⇒ The icons were ghosts; the lingering hosts were what got ghosted; and one
   fix covers both, because a host that exits WITH its guest never needs killing.
   ▶ The launchers now ask before they kill (`rigshot close` on the host window,
   then `taskkill` as the fallback). MEASURED: three consecutive launches, icon
   count stable at 7 — it would have gone 7 → 8 → 9.

   ⚠ **MY FIRST MEASUREMENT PROVED NOTHING.** `rigshot` has no `close` verb, so
   the "test" never closed the guest and the host was still running for the most
   boring reason available — a run that *read as* a clean reproduction. Clicking
   the real X is what produced the evidence. ⚠⚠ And the tool HAD said so:
   `unknown verb` went to `rigshot.txt` as designed and the batch overwrote that
   file before anything read it. **Read the tool's log before trusting the tool's
   effect.**

   ### ▶ ★★★★★ THE REAL CAUSE: A WIN16 APP COULD NOT EXIT

   The lifecycle is `WM_CLOSE → DestroyWindow → `**`WM_DESTROY`**` →
   PostQuitMessage → GetMessage returns 0 → WinMain returns → task exits`.
   We relayed `WM_CLOSE` (wowwin.h) and implemented `DestroyWindow` (wowuser.h),
   and **dropped the middle link — `WM_DESTROY` was delivered by NOTHING,
   ANYWHERE.** So a guest destroyed its window and went straight back to
   `GetMessage` and blocked forever (`WOWMSG: blocked 0x7f23 ms` and climbing,
   window already gone). The task never ended ⇒ the exec loop never ended ⇒ the
   host never exited ⇒ the icon stayed.

   ⚠ **ORDER IS THE WHOLE DIFFICULTY.** The window record must OUTLIVE the window
   by exactly one message, because `DispatchMessage` resolves the window
   procedure THROUGH it (`wowuser_findwin`). Clearing `hwnd` in DestroyWindow —
   which the existing note rightly wants for a dead window — makes the message
   just posted undeliverable. It is marked `dying` and released the instant its
   `WM_DESTROY` is dispatched.
   ⚠ Real Windows **SENDS** WM_DESTROY; we post it. Same caveat and same reason
   as WM_SIZE/WM_SETFOCUS already carry.

   ★ **SECOND HALF: A WIN16 HOST MUST NOT OUTLIVE ITS GUEST.** The exec loop's
   tail said *"keep the window open so the guest's final screen stays visible"* —
   right for DOS, wrong here, because a Win16 guest has **no window and no final
   screen** (the `tray_add` note says so outright). Nothing to look at, nothing
   to close, and the wait never ended. A Win16 host now asks its UI thread to
   close and leaves through its OWN path, which is also what stops the OPL and
   Beep.sys — both of which have outlived a host before.
   ⚠ `tray_remove` now also runs before the VEH's `ExitProcess`, the headless
   `ExitProcess` and the watchdog's `TerminateProcess` — the paths that CANNOT
   unwind, and the only ones that could leave a genuine ghost.

   ★ The full chain, in one log: `DestroyWindow -> WM_DESTROY posted /
   DispatchMessage msg=0x0002 -> its own window procedure / PostQuitMessage 0 /
   GetMessage -> WM_QUIT -- the loop ends / ExitKernelThunk(0) / STAGE2: exec
   loop exited`, and `tasklist` after: **NONE**. Confirmed on NOTEPAD (classic
   wndproc), CALC (a dialog) and CARDFILE.

   ### ▶ ★★★★★ THE MODAL FLAG — s55's OPEN QUESTION, SETTLED, AND IT IS THE
   SUB-DIALOG GAP THE USER NAMED

   s55 left this written down and unresolved: *"Which of DialogBox/CreateDialog
   this id serves is NOT known. ▶ To settle it, disassemble USER.EXE at
   `0x03ff:0x4bdc`."* Done. The answer was **one push instruction** away:

   ```
   CreateDialog   seg1:0x4bd5   push 0    / lcall 0x047b:0x4c48
   DialogBox      seg1:0x4d0c   push 1    / lcall 0x047b:0x4d97
   ```

   The **same thunk**, and the last word pushed — arg offset 0 — is 0 for the
   modeless family and 1 for the modal one. s55 recorded that field as *"+0 still
   unexplained (0 in every run)"*: it was 0 in every run because every guest
   measured then (CALC, SOUNDREC, TERMINAL) calls **CreateDialog**.
   `USER.87 DIALOGBOX` and `USER.89 CREATEDIALOG` are thin argument shufflers
   onto `0x4c80` and `0x4b4a`, and **both return immediately after the thunk** —
   so the modal message loop is **not in USER's 16-bit code. It is ours, and we
   do not run one.**

   ★ **THAT IS WHY A MODAL DIALOG ENDS ITS PROGRAM.** DialogBox's whole contract
   is not returning until `EndDialog`. We create and return at once, so a caller
   whose WinMain is `DialogBox(...); return;` — **TASKMAN exactly** — exits.
   ⚠ This only became visible because the host leak was fixed earlier today: the
   window s55 saw was an ORPHAN.

   ★★ **AND IT IS THE SUB-DIALOG GAP THE USER NAMED IN s53** — Minesweeper's
   `Game > Preferences`, Solitaire's `Options`/`Deck`. **Five guests call
   DialogBox: NOTEPAD, PACKAGER, SYSEDIT, TASKMAN, WINMINE (3 calls).** One
   feature unblocks all of it.

   ▶ **WHAT IT NEEDS — the next task, specified.** Deferred completion of this
   BOP: park the guest inside the service, pump the dialog by calling its 16-bit
   dlgproc through the existing wowcall chain (`EDITLOCK → EDITFILL →
   LocalUnlock` is the same shape), and complete the original call with
   `EndDialog`'s result.
   ⚠ **NOT ATTEMPTED, deliberately.** A half-built modal loop that never returns
   is worse than an honest immediate return — it HANGS the guest instead of
   ending it. What landed is the diagnosis: arg 0 is read, and a modal call now
   NAMES ITSELF and its consequence in the trace.

   ### ▶ WHERE THE NUMBER WENT

   `guests` 55% → 61% (CALC/CARDFILE/WRITE), `serial-vdd` 0 → 85%,
   overall **83.1% → 84.3%**. ⚠ `0xc8` did NOT move `breadth`: it is a
   krnl386-internal id, in no guest's import list, so the probe cannot see it.
   The value there was correctness, not the number.

   ⚠ **All three guests are `partial`, NOT `done`** — `done` means USER-CONFIRMED
   and none has been typed into, saved and reloaded by a human. CALC's arithmetic
   is measured; the other two have only been launched.

   ▶ **REMAINING BOUNDED ITEMS**: `sdk` (#11, +1.15), `vesa-pm` (#53, +0.78),
   `net-vdd` (#8, +0.77), `guest-zar` (#23, +2.33). The heaviest lever is still
   `guests`: 12 partials at **+0.26 each** = +3.1, and it needs the user at the
   keyboard.

   ---

   ## ★ SESSION 55 — 80.2% → 83.1%  (paused mid-afternoon at the user's request)

   **Seven commits, all rig-gated. The user's goal for the day was >90%; it was
   not reached and the reason is measured rather than guessed — see the rate
   note at the end.**

   ### ▶ ⏸ WHERE WE STOPPED, AND THE QUESTION ON THE TABLE

   The session was paused so the user could restart the IDE. They will resume
   with *"Continue where we left off"* and asked to be **presented with the four
   options below again** before any more work is done.

   | option | what it means | rough value |
   |---|---|---|
   | **Chase the number** | the bounded not-started items: VDD/driver SDK (#11), VESA 4F0A PM bank switching (#53), serial VDD (#9), net VDD (#8), plus the last ~25 Win16 services | **+3.9**, predictable, → ~87%. Mostly checkbox items rather than visible capability |
   | **Make the guests work** | find which pass re-patches CALC's FP site (unblocks CALC + TASKMAN + CARDFILE), give PROGMAN its groups, get SYSEDIT saving | fewer points (**+0.26** per partial→done) but it is the north-star work |
   | **Mixed** | guests for ~2h while the user is around to confirm, then harvest the bounded items | — |
   | **DOS defects** | WRITE's "not enough disk space" on a 243 GB disk; `MEM /C` contradicting its own summary | small, but this is the *runs but lies* class |

   ### ▶ ★★★★★ WIN16 DIALOGS — THE UNLOCK OF THE DAY (USER thunk `0xEF`)

   `USER.EXE` implements `CreateDialog`/`DialogBox`/`DialogBoxParam` in its OWN
   16-bit code, so `neneeds.py` calls them `native16` and prices them at
   nothing. **They are not free**: that code does the
   FindResource/LoadResource/LockResource itself and then calls out to a 32-bit
   helper at **USER id `0xEF`, which no export maps to**. Stepped over, CALC
   produced no window at all.

   **The arguments are MEASURED**, from one CALC run — 22 bytes, and the WOWBOP
   line above the call settles the important one (`ax=cx=dx=0x0b47`, the
   selector USER had just had back from `LockResource`):

   ```
   +20 hInstance   +16 template FAR*   +14 parent   +10 dlgproc   +6 initparam
   +2  TEMPLATE LENGTH IN BYTES        +0  still unexplained (0 in every run)
   ```

   ★ **`+2` is proved on two different values**: CALC passes `0x0140` and its
   `SC` dialog is 320 bytes; SOUND RECORDER passes `0x0210` and its `DIALOG 1`
   is 528. ⚠ It was nearly read as a window handle because CALC's `0x0140` also
   happened to be the next handle our own allocator would issue — **a number
   matching something is not a number meaning it.**

   ★ The template reading was confirmed **offline before any host code existed**
   (`tools/ne/neres.py dialog`, new this session), which is the discipline
   session 43 held the menu decoder to: a wrong offset does not spell
   *'Calculator'*, *'SciCalc'*, and buttons reading Hex/Dec/Oct/Bin/Hyp/Inv.

   ⚠ **One thing is a JUDGEMENT and is marked as one in the code**: we show a
   dialog whose template omits `WS_VISIBLE`, because that is what the dialog
   manager does and we are its 32-bit half. Which of DialogBox/CreateDialog this
   id serves is **not known**. ▶ To settle it, disassemble `USER.EXE` at
   `0x03ff:0x4bdc`.

   ### ▶ ★★★ FIVE GUESTS MOVED OFF `todo` — the shelf sweep

   `scripts/bm/shelfsweep.bat` launches each never-run guest in turn. **Four of
   seven produced windows on the first sweep**, which is the shelf method
   working exactly as recorded: *the run names the blocker.*

   | | |
   |---|---|
   | **CALC** | real `Calculator`, own icon, Edit/View/Help, taskbar button; **15/15 `SciCalc` controls at the template's rectangles**, `rigshot tree` showing 13 hidden + 2 visible = EXACTLY the 13 `SW_HIDE` and 2 `SW_SHOW` CALC itself made. Runs its whole `WM_INITDIALOG`. |
   | **TASKMAN** | real `Task List` via the `#32770` fallback class, 8/8 controls |
   | **PROGMAN** | ★ **the Windows 3.1 SHELL** — 1260×742 `Program Manager`, own icon, File/Options/Window/Help, taskbar button. MDI client empty grey. |
   | **TERMINAL** | 1260×742, menu, scrollbars, and **a status bar it is drawing into — `Level: 1` and a live clock**. Third guest on the `0xEF` helper (`Default Serial Port` dialog with real static/listbox/OK). |
   | **SOUNDREC** | real `Sound Recorder` with its microphone icon, File/Edit/Effects/Help, panels, wave display, scrollbar, five transport buttons |
   | **PACKAGER** | real `Object Packager` with Appearance/Content panes and a working Description/Picture radio group |

   ★★ **PACKAGER WAS UNBLOCKED BY SOMETHING THAT WAS NOT ITS RECORDED BLOCKER.**
   Its note said `EnumTaskWindows`, still unimplemented. The real cause was that
   registering `BUTTON`/`STATIC`/`SCROLLBAR`/`COMBOBOX`/`#32770` as system
   classes for the dialog work also fixed its `CreateWindow "STATIC"`. ⇒ the run
   is the oracle, again.

   ### ▶ ★★★★★ INT 34h..3Fh IS FLOATING-POINT CODE, NOT AN INTERRUPT RANGE

   **The patcher's own comment was wrong and it cost three guests.** It rewrites
   `CD nn` to a BOP for any voted vector because "servicing is harmless for any
   vector: our default PM handlers cover all 256 and simply IRET". True of
   interrupts. **These are not interrupts**: Microsoft's FP emulator encodes
   x87 opcodes as `CD nn` in that range (0x34..0x3B = ESC D8..DF, 0x3C =
   segment override + ESC, **0x3D = FWAIT**), and 0x3E/0x3F are the far-call and
   overlay fixups. Rewriting one does not make it serviceable, it makes it stop
   being the instruction.

   CALC — **a calculator, which imports WIN87EM** — built its whole dialog, ran
   its whole init, then died at
   `bytes@eip-2 = 3d cb c4 c4 07 cd 3d cb cd 39` = FWAIT, RETF, our BOP, more FP.

   ★ **The instrument that named it is three lines and is now permanent**: the
   PM-stop line asks `pmap` whether the byte pair is one of OUR patches, because
   the log had always left the reader to guess between a real BOP, a patched INT
   we failed to resolve, and data being executed. First run with it:
   `pmap[eip]=INT 0x39 ★ THIS IS A SITE WE PATCHED`.

   ⚠ **DOOM WAS DOING IT TOO** — the guard fires 4× in Doom's own code.
   Regression clean: `V_Init..ST_Init`, 3,503,139 bytes.

   ⚠⚠ **IT DOES NOT YET UNBLOCK CALC AND THAT IS THE HONEST STATE.** The guard
   stops us corrupting NEW sites. CALC's site was patched by an EARLIER pass and
   `if (pmap_get(lin)) continue` skips it before the guard is reached — the
   region containing it reports "patched 0 INT sites" precisely because the bytes
   were already `C4 C4`. **Which pass writes it is not yet known, and that is the
   next step — not another repair.**

   ⚠⚠⚠ **AND THE OBVIOUS FIX WAS MEASURED WRONG.** Healing at the point of
   failure (restore `CD nn`, leave EIP, re-execute) ran **120,670 times on one
   address**: something re-patches it between heals, so "self-limiting because
   the map entry is cleared" was simply false. Reverted, with a scanner repair
   pass that never fired once. An infinite loop is worse than a clean stop.

   ### ▶ #28 CLOSED — the DOS version is configurable and THE GUEST SAYS SO

   Everything was already in the tree; what was missing was a run where the
   guest reports it. `scripts/bm/verver.bat`, three cases: `6.22` → `INT21.30
   major=06 minor=16 oem=FF serial=00:0000`, `3.31` → `03/1F` on both calls, no
   override → the registry default. Case 1 reproduces **every field** of the
   recorded 6.22 oracle except `flags`, which `dos-oracle.md` already excludes by
   name (DH bit 4 = DOS in HMA, a property of that image's CONFIG.SYS).
   ⚠ The first run of that gate reported the HOST side only — `findstr` matched
   `#PROBE`, a string the probe does not print, and the wait loop polled for a
   done-marker **the previous run had left on the share**. Delete both
   destinations from the driving side BEFORE dispatching.

   ### ▶ THE RIG SCRIPTS THAT EARNED THEIR KEEP (all fixed, all in the repo)

   * **`wowlive.bat` and `wowrun.bat` re-add the IFEO value before every launch.**
     GH #132 drops it after three unclean starts and every batch's `taskkill`
     manufactures them. It happened **twice today**.
   * **`bmwow.sh` moves `wowsched.txt`/`wowcall.txt` aside itself** (`SWITCHES=1`
     keeps them) and **prints the gate signature next to the baseline**. That
     number had been hand-counted out of the log every session it was ever
     quoted. Without the move-aside the gate reads **280/291/74** and looks like
     catastrophic drift; with it, `85 / 113 / 57 · 0001:229C`.
   * **`bmwow.sh` fails loudly on an empty log** rather than printing `0/0/0`,
     which means "no run happened", not "everything regressed".

   ### ▶ WHAT THE NUMBERS DID, AND THE RATE

   `guests` 39% → **55%** (5 done, 12 partial, 2 todo of 19) ·
   `breadth` 83% → **91%** (287 serviced / 27 to do) · `dos-version` 0 → **100%**.

   ⚠ **The day's rate was ~+1.05 points/hour**, and the last 50 minutes produced
   a real finding and **zero points**. That is the shape of what is left: +6.9 to
   reach 90%, over work that is harder than what has been done, not easier.

   ---

   ## ★ SESSION 54 — 79.6% → 80.2%

   **Product work, mostly. Everything below is committed and rig-gated.**

   - **AN INSTALLER (#13, the M10 gap).** `ntvdmhost.exe /install | /uninstall |
     /status`, and the same three on the File menu behind a confirmation. It is
     **reversible, not merely removable**: a `Debugger` value belonging to another
     program is SAVED before we displace it and RESTORED on uninstall; one we never
     installed is REFUSED rather than deleted. Verified by READING THE VALUE BACK,
     never by a return code. 21 off-VM checks on the decision (quoted/unquoted, case,
     trailing space, forward slashes, a path that merely *starts* with ours); six
     behavioural cases on the rig.
   - **CPU SPEED (#56)** — 18 speeds, Unlimited…8 MHz, on the Machine menu and the
     Settings CPU page. Duty cycle by suspending the exec thread.
   - **THE WHOLE DISPLAY PAGE IS ALSO THE VIEW MENU**, and menu changes are
     **session-only** (`g_set` = in force, `g_set_disk` = saved). Window size resizes
     live; aspect is a **lock** (None/4:3/16:9/16:10) with a 640×480 floor; scales
     that cannot fit the desktop are **greyed**, not silently substituted.
   - **The status strip is three real parts**, name-hugging divider, bitness+mode
     joined ("16-bit Real mode").
   - **The DOS text cursor is an underscore again** — see the cursor note below.

   ### ▶ ⚠⚠ THE THREE THINGS THAT COST ME TIME TODAY — READ BEFORE RUNNING ANYTHING

   1. **THE RIG SILENTLY STOPPED ROUTING TO US, TWICE.** GH #132's recovery drops the
      IFEO `Debugger` value after three consecutive unclean starts — and every rig
      batch begins with `taskkill`, which manufactures them. A guest then comes up
      under **stock ntvdm** and everything looks normal. I showed the user a Clock
      window that was stock's. ⇒ **`reg add` the key before EVERY case in a batch**,
      make the batch print a loud line when the host log is missing, and use
      **`ntvdmhost.exe /status`** — it diagnoses this in one command now.
   2. **A `copy /y` STRAIGHT AFTER `taskkill` FAILS SILENTLY.** Windows has not
      released the image handle yet; with output sent to `nul` the OLD binary stays
      and the run looks entirely normal. I gate-tested the previous build for a whole
      run. `wowlive.bat` already had this written down — **reuse the scripts.**
   3. **ONE SCREENSHOT IS NOT A MEASUREMENT of anything that repaints.** I reported
      Clock fixed from a single frame; it was one phase of a loop that is still
      running. Two shots minimum, and diff them.

   ### ▶ ★★★★★ THE CLOCK: A STEPPED-OVER CALL, FOR THE FIFTH TIME

   `CLOCK.EXE` asks DOS for the date the ordinary way — **`INT 21h AH=2Ah`** in
   protected mode. Inside a WOW VDM there is no DOS underneath, so **krnl386 owns that
   vector and thunks it out as seg1 `FUNC 0x86`**. Unimplemented ⇒ stepped over ⇒
   answered 0 ⇒ no date ⇒ blank face ⇒ invalidate ⇒ ask again. **10,317 times in
   fourteen seconds**, which is why its log came back at **234 MB**.

   Implemented (FAT packing: date high word, time low). **Clock now renders the correct
   time** — `20:55:04`, measured.

   ⚠⚠ **BUT IT IS NOT FIXED, AND I SAID IT WAS.** The repaint loop is STILL THERE:
   8,922 date calls served in ~20 s (≈450/s, where a clock needs 1/s), and consecutive
   frames differ by 28.5% of the client — the face is caught mid-erase. Answering
   `0x86` changed **what** it paints, not **how often**. `guests` stays 5 done + 5
   partial.

   ⚠ **It also corrected a recorded inference.** The old note said Clock's time "comes
   from krnl386's own 16-bit code, so there is no thunk to observe". The two *measured*
   hypotheses beside it were right; that one was a guess wearing the same clothes.

   **NEXT ON CLOCK, in order:** (a) find what drives the invalidate loop — the date
   service no longer returns a sentinel, so whatever remains is a second cause;
   (b) `20:55:04 PM` is 24-hour digits with a 12-hour suffix — `[intl]` `iTime` /
   `s1159` / `s2359` out of WIN.INI, a formatting bug, not the date service.

   ### ▶ THE OTHER TWO PARTIAL GUESTS (same sweep, same day)

   - **RECORDER** — titled window, **blank client, no menu bar**.
   - **MPLAYER** — **no window at all**. Its recorded blocker
     (`SPI_GETICONTITLELOGFONT`) has since been implemented, so that note is stale.
   - Both show the **same ~77 stepped-over startup calls** Clock did, so those are
     shared and survivable — **neither blocker is in that set.**

   ### ▶ THE CURSOR (user-reported, fixed)

   `ABC123-` instead of `ABC123_`. DOS asks for its cursor in scan lines of an
   **8-line** cell (underline 6-7, insert block 0-7); our cell is 16, so the underline
   landed halfway up. Our own default did it too (`cur_shape` init `0x0607`). Fixed
   with the VGA BIOS's own **cursor-emulation** rule (IBM/Bochs/SeaBIOS), reproduced
   exactly: 6-7 → 14-15, 0-7 → 1-15. Overwrite underline **confirmed at a real
   COMMAND.COM prompt**; the insert block is implemented and unit-tested but **not
   witnessed** — plain COMMAND.COM may never set `0x0007` (DOSKEY and full-screen
   editors do).

   ### ▶ #131 STDIO — LOCATED, NOT FIXED, AND THE OLD HYPOTHESIS IS DEAD

   Five routes to the console handle are now eliminated. The fifth
   (`VDM_COMMAND_INFO.StdIn/StdOut/StdErr`) returned non-handles — and **the
   instrument that showed it named the real defect**: the WHOLE struct is junk
   (`CreationFlags` 0x4d445674 and `CodePage` 0x78654e74 are ASCII and constant,
   `TaskId` 0), so **`GetNextVDMCommand` returns TRUE and populates nothing**. The
   command line says why: it arrives as `-f` with **no `-i<taskid>`**.
   ⇒ **#131 is the same defect as M2.5's "recover the real command line from CSRSS's
   multi-call protocol". Fixing one fixes both.**
   ⚠ **NOT a subsystem problem** — we are already CUI, pinned in CMakeLists.
   ► Target proven reachable: `hello.com > out.txt` writes **136 bytes under stock and
   0 under ours**, same box, same command.

   ### ▶ WHERE THE NEXT POINTS ARE (weight × remaining, measured)

   | item | now | full close | note |
   |---|---|---|---|
   | `guests` (w15) | 39% | **+5.97** | **+0.26 per guest** partial→done — the heaviest lever by far |
   | `breadth` (w12) | 83% | +1.33 | ~0.025 per service |
   | `guest-zar` (w3) | 0% | +2.33 | VBE hi-colour + LFB |
   | `flat-thunks` (w4) | 25% | +1.96 | architectural |
   | `stdio` (w3) | 50% | +1.16 | blocked on the CSRSS handshake above |
   | `dos-version` (w1) | 0% | +0.78 | the setting already exists and applies |

   ---

   ### ▶ Session 53's handoff: **[Session 53, afternoon](#session-53-afternoon--the-guest-shelf-751--796)**
   at the bottom of this file — it names the next job, the entry point for it,
   and the two hypotheses already killed. Sessions 51-53 are recorded here rather
   than in `log/sessions/`; session 50 is the last one with its own file.

   ---

   ### ▶ ★ WHERE IT IS, IN ONE BLOCK (read this before the session notes)

   **Working and confirmed by hand:** Notepad edits and saves text files. MS
   Paint draws in colour with every shape tool and the flood fill, keeps what it
   draws across a repaint, and **saves a valid `.BMP` where you tell it to**.
   Both have their own icons, menus, captions and taskbar buttons, and both are
   real `HWND`s on the XP desktop.

   **The service surface, measured** (`tools/ne/neneeds.py`, fixed in session 47
   so it sees through validating export wrappers — it under-reported by half
   before):

   | | needs us | serviced | left |
   |---|---|---|---|
   | MS Paint — GDI | 76 | 67 | 9 |
   | MS Paint — USER | 92 | 88 | 4 |
   | PBRUSH.DLL | 16 | 16 | **0** |
   | Notepad — GDI + USER | 56 | 52 | 4 |

   ### ▶ THE NEXT THREE THINGS, IN ORDER

   1. ⚠ **After mouse drags, the Alt-key menu route stops opening the menu.**
      On a freshly launched Paint, `Alt`,`F`,`A` opens Save As reliably — that is
      how the verified save was driven. After three `rigshot drag`s on the canvas
      the same sequence does nothing and `rigshot fg "Save As"` reports NOT
      FOUND. Drawing still works; it is the *menu* that stops responding.
      ★ **This is probably the user's standing "menu clicks crash the app"
      report seen from another angle**, and it is the last thing between Paint
      and "use it like a program". ▶ Test in order: mouse **capture** left set
      after a drag (check `SetCapture`/`ReleaseCapture` pair); focus parked on
      the `pbPaint` child so `WM_SYSKEYDOWN` never reaches the frame;
      `ClipCursor` accepted-but-not-applied.
   2. **Broaden to the shelf — the user's call, and the numbers back it.**
      Solitaire **9** services, Minesweeper **15** (6 are the optional SOUND
      driver), Clock 7, Charmap 16, Media Player **20**, Sound Recorder 26.
      ★★ **Media Player's MMSYSTEM imports all resolve to 16-bit code inside
      MMSYSTEM.DLL — none reach a WOW32 thunk**, so WinMM is not a wall in
      front of it; the work sits *below* MMSYSTEM.DLL and only a run can name
      it. ★★★ The guests overlap so heavily (`SetTimer`, `DrawText`,
      `FrameRect`, `GetParent`, `IsDialogMessage`, `DefDlgProc`, `ExtTextOut`,
      the menu trio) that **~35 distinct services cover all six**. ⚠ `SetTimer`
      is the only one that is not a pass-through: it needs `WM_TIMER` posted
      into the Win16 queue, and both games want it.
   3. **The 22 ids still unserviced**, all enumerated and named: GDI `Escape`,
      `EnumObjects`, `LineDDA`, `CreatePolygonRgn`, `GetCharABCWidths`,
      `GetPaletteEntries`, the metafile trio; USER `IsDialogMessage`,
      `SetDlgItemText`, `GetDlgItemInt`, the three dialog-button calls,
      `ModifyMenu`, `GetMenuState`, `TabbedTextOut`, `ScrollWindow`, the three
      clipboard calls. ⚠ `EnumObjects` and `LineDDA` take **16-bit callbacks**
      and need `wowcall`, not a pass-through.

   ### ▶ ⚠ STANDING HAZARDS (each of these has cost a session)

   * **The regression gate is `85 / 113 / 57 · 0001:229C`** (session 50; was
     `82 / 122 / 60`), and it must be run with `wowsched.txt` and `wowcall.txt`
     **moved aside** — `wowlive.bat` creates them, and a gate run that leaves
     them in place measures a guest running much further (`238/308/101`) and
     reads as catastrophic drift.
     ★ **The session-50 move is ONE cause and the arithmetic closes on it.**
     `0x7f GetPrivateProfileInt` is now serviced, so three calls moved
     stepped→serviced (60→57, 82→85), and nine redundant `0xc2 _lclose`
     declines stopped happening (122→113) because the profile machinery no
     longer re-opens the file after being told 0. Improvement, not drift.
   * ⚠⚠ **KILL EVERY `ntvdmhost.exe` BEFORE A GATE RUN.** All hosts append to
     the *same* `C:\ntvdmex\ntvdmhost.log`, and the gate copies that file — so a
     guest left running by `wowlive.bat`/`playtest.bat` writes into the gate's
     own artefact. Session 51 read **`6680 / 400 / 133`** off a 6.7 MB log and it
     was Solitaire and Minesweeper's output mixed into the gate's; killed first,
     the same binary measured `85 / 113 / 57` exactly. ⚠ A contaminated gate does
     not look like contamination — it looks like a guest running spectacularly
     further.
   * ⚠⚠ **`C:\ntvdmex\target.txt` IS AN UNCONDITIONAL OVERRIDE, AND IT DECIDES
     WHICH Win16 PROGRAM RUNS.** On a WOW launch the host takes the program name
     from that file (`main.c`, STAGE2) because Windows does not put it on the
     VDM's command line — so **every** Win16 launch on the box runs whatever it
     names, whatever was actually double-clicked. `start SOL.EXE` with
     target.txt still saying WINMINE starts a **second Minesweeper**, cheerfully
     and with no error (session 50). To run two guests, rewrite target.txt
     between the launches — `scripts/bm/playtest.bat` does. ⇒ This is also the
     reason a real install cannot work yet: without target.txt the WOW path has
     no program name at all.
   * **Doom is the other half of every DPMI change.** The known-good signature
     is **all eleven startup stages `V_Init`…`ST_Init` in ~3.5 MB**
     (`./scripts/bmqueue.sh doom DOOM.EXE`).
   * ⚠⚠⚠ **The guest loads XP's `system32` copies** of OLESVR, OLECLI, SHELL,
     MMSYSTEM, COMMDLG and VER — *not* the Windows 3.11 ones in `guest/win16/`.
     Same sizes, different md5, different code. Disassembling the wrong one
     wasted part of session 49. They belong in **`guest/wow/`**, which is
     `.gitignore`d like the rest of `guest/`, so **a fresh checkout has to fetch
     them off the rig**:
     ```bash
     RES='C:\Documents and Settings\All Users\Documents\ntvdmex'
     printf 'exec cmd /c "copy /y C:\\WINDOWS\\SYSTEM32\\OLESVR.DLL "%s\\wowdrv_olesvr.dll""\r\n' "$RES" > $SH/control.txt
     mkdir -p guest/wow && cp $SH/wowdrv_olesvr.dll guest/wow/OLESVR.DLL
     ```
     (same for `MMSYSTEM.DLL`, `SHELL.DLL`, `OLECLI.DLL`, `COMMDLG.DLL`, `VER.DLL`).
   * **`neneeds.py`'s "free (16-bit)" column is a lower bound, not a statement
     about work.** It was wrong for 40 services before session 47 fixed it, and
     `native16` still does not mean free where a module calls down on the
     program's behalf.
   * **A stepped-over call answers, and its answer is load-bearing.** Three of
     the last four bugs were a sentinel `0` that the call site reads as
     *success* — `GetProfileString`, `GetParent`, `SetCurrentDirectory`.

   ### ▶ HOW TO DRIVE IT (all through `controld` on the share)

   ```bash
   SH=/private/tmp/xpshare
   RES='C:\Documents and Settings\All Users\Documents\ntvdmex'
   cp build/ntvdmhost.exe $SH/bm/ntvdmhost.exe          # ⚠ md5 both ends

   printf 'exec cmd /c ""%s\\wowlive.bat" C:\\WIN16\\PBRUSH.EXE"\r\n' "$RES" > $SH/control.txt
   printf 'exec cmd /c ""%s\\savetest.bat""\r\n'   "$RES" > $SH/control.txt  # clean+launch+SaveAs+verify
   printf 'exec cmd /c ""%s\\pbtools.bat""\r\n'    "$RES" > $SH/control.txt  # box+fill+ellipse+stroke
   printf 'exec cmd /c ""%s\\pbmin.bat""\r\n'      "$RES" > $SH/control.txt  # minimise/restore = persistence
   printf 'exec cmd /c ""%s\\wowcompare.bat" C:\\WIN16\\NOTEPAD.EXE"\r\n' "$RES" > $SH/control.txt  # vs STOCK
   ./scripts/bmwow.sh            # the WOW gate  (switches moved aside first)
   ./scripts/bmqueue.sh doom DOOM.EXE

   python3 tools/ne/neneeds.py   guest/win16/PBRUSH.EXE --todo --stubs
   python3 tools/ne/neimports.py guest/win16/PBRUSH.EXE --seg 3
   ```
   ⚠ **A multi-step rig test must be ONE batch.** Driving clean/launch/keys/check
   as separate `exec`s raced and deleted the evidence in session 49;
   `savetest.bat` exists because of that.
   ⚠ `rigshot` logs to `rigshot.txt`, **not stdout** — redirecting it captures an
   empty file.

   ---

   > ⏹ **Everything below is the session-by-session record.** It is kept because
   > it is still the reference for the loader, the scheduler and the message
   > loop — but the newest blocks are at the top, and anything below session 45
   > describes a frontier that has since moved. Do not read an old
   > *"and the frontier is …"* heading as current.

   ### ▶ ★★★★★ MS PAINT SAVES A FILE (session 49)
   `File > Save As` writes a valid **4,909,014-byte, 1680×974, 24-bit .BMP** to
   the directory the user chose, and Paint stays running. Verified by reading the
   file's own headers off the rig. Two bugs, both the same shape — **a
   stepped-over call whose sentinel answer means "yes"**:
   ★ **`USER.46 GetParent`** unimplemented ⇒ OLESVR asked window **0** for its
   window long and dereferenced the zero (`OLESVR seg3:0x1548`,
   `cmp es:[bx+0xe]` with `ES:BX = 0:0`). The right answer was knowable before
   the run: window `0x200` is `WS_CHILD` of `0x140`, and OLESVR had stored its
   server object on `0x140` thirty log lines earlier.
   ★ **krnl386 `0x82 SetCurrentDirectory`** unimplemented ⇒ the whole .BMP was
   written correctly *to the wrong directory*. ⚠⚠ Its call site reads our
   sentinel 0 as **success**, so krnl386 told the app the directory had changed.
   ⚠ Not declinable — `wowdecline.py` already listed it; declining was tried
   twice and only moves the fault.
   ⚠⚠ **AND THE WRONG BINARY WAS BEING DISASSEMBLED**: the guest loads XP's
   `system32` copies of OLESVR/OLECLI/SHELL/MMSYSTEM/COMMDLG, not the Win3.11
   ones in `guest/win16/`. Same size, different md5. They now live in `guest/wow/`.
   ⚠ **NEW, unexplained**: after mouse drags the **Alt-key menu route stops
   opening the menu** (fresh instance: reliable). Probably the user's "menu
   clicks" report from another angle — see the resume block for the three
   hypotheses to test.

   ### ▶ Session 48's handoff (background): [session 48](log/sessions/session-48.md#-resume-here)

   ### ▶ ★★★★★ TWO ALLOCATORS, ONE LDT — AND IT BLOCKED EVERY GUEST (session 48)
   `File > Save As` crashed MS Paint **and Notepad** with a #GP in
   `KRNL386.EXE at 0001:5349`. krnl386 caches the DOS structures at boot and
   turns their segment into a selector with **DPMI Segment-to-Descriptor**; that
   selector has to live for the life of the VDM, and **krnl386 kept its own idea
   of which LDT entries were free and repointed ours** (`DPMI 000C`, and direct
   descriptor-shadow writes). Our counter started at 6 and grew up; krnl386's
   arena starts at `0x30`; **they grew into each other.**
   ⇒ A **host-private LDT pool at `0x09..0x2b`**, and the client-facing counter
   now starts above it. ★★★ **The pool is MEASURED, from logs already on disk**:
   348 distinct indices the guest touches, dense from `0x30` up, largest
   untouched run 35 entries, against 11 host selectors a run.
   ⇒ **Paint's save now reads its whole canvas** (`GetDIBits … -> 03ce scan
   lines`); the next wall is elsewhere — `OLESVR.DLL at 0003:1548`.
   ⚠ Verified three ways because it is a shared path: **gate unchanged
   `82/122/60`**, **Doom's eleven startup stages at 3.51 MB**, pool never spilled.
   ⚠⚠ **Two earlier explanations were wrong** (declining `0x82`/`0xc1`; "freed
   and recycled" — refuted by `grep -c recycled` = 0). Both were reasoning from a
   mechanism that fit rather than from the log.

   ### ▶ Session 47's handoff (background): [session 47](log/sessions/session-47.md#-resume-here)

   ### ▶ ★★★★★ THE ENUMERATOR WAS UNDER-REPORTING THE JOB BY HALF (session 47)
   `tools/ne/neneeds.py` knew two export shapes and GDI/USER have four, so
   every **validating wrapper** — an export that checks an argument before its
   tail jump — was reported *free*. Fixed (the scan is bounded by the export's
   own pushed `retf`), Paint's GDI surface went **41 → 76** and USER **52 → 92**,
   and **40 services went in**: `SetDIBits`/`GetDIBits`/`StretchDIBits`,
   `TextOut`/`GetTextMetrics`/`CreateFontIndirect`, `CreateDC`, `DefWindowProc`,
   `SetClassWord`, `GlobalAddAtom`, the caret, and the rest. **PBRUSH.DLL is now
   100% serviced.** ⚠ Gate unchanged at `82/122/60`.
   ★★ **`0x99` is `CreateIC` (ord 153), NOT `CreateDC` (ord 53 = `0x35`)** — a
   recorded fact corrected; the id tracked the ordinal all along.
   ★★ **Both icon defects fixed and confirmed against stock pixel by pixel.**
   Paint's `GROUP_ICON` is the NAMED resource `"PBRUSH"` and `0xad` refused
   named resources — ⚠ **the third time this same gap has been found** (menus in
   session 45); its seven cursors are named too. Notepad's *taskbar* icon
   measured **0 cyan pixels against stock's 59** because a class with no
   `hIconSm` makes Windows derive one — now `WNDCLASSEXA` with a real 16×16.
   ⚠ **`File > Save As` is NOT fixed and is diagnosed to the instruction**: a
   DPMI selector minted for krnl386's SysVars cache is **recycled** and
   redefined 64 bytes long, so `krnl386 seg1:0x5349` faults. The fix is in the
   DPMI allocator, which is Doom's shared path — see the resume block.

   ### ▶ ★★★★★ MS PAINT DRAWS, IN COLOUR, AND IT STAYS DRAWN (session 46)
   A red-outlined box, a flood fill bounded by that border, a green ellipse and
   a brush stroke — then Paint minimised (its window's pixels genuinely
   destroyed) and restored, and the whole picture repaints from **Paint's own
   image**. All three of the user's defects are closed.
   ★★★ **The palette and the persistence were ONE CALL.** `GetProfileString`
   (krnl386 `0x3a`) was unimplemented, so
   `GetProfileString("Paintbrush", "clear", "COLOR", buf, 9)` returned 0
   characters, `cmp [bp-8],2 / jbe` took the short arm, and `seg2:0x08de`
   pointed Paint at the **28 greys** at DGROUP `0x09a2` instead of the 28
   colours at `0x0932` — and a black-and-white image gets a 1bpp canvas, which
   is why a correctly-blitted stroke vanished on the next repaint.
   ⚠⚠ **The default is `COLOR`.** Session 45 checked WIN.INI, found no colour
   key, and ruled the profile out — correctly and fatally. **An unimplemented
   call cannot return a default**, so "the key is absent" became "the key says
   something that is not COLOR".
   ★★ **And the fill was `ExtFloodFill` + `CreatePen`, both reported as
   *free*.** GDI's ordinal-372 export validates the fill type before its
   tail-jump, so the stub scanner cannot see it; the shape tools' 48 successful
   `Ellipse` calls per drag were all `R2_XORPEN` rubber band, and the commit
   asked for a `PS_INSIDEFRAME` pen, got 0, and declined to draw. ⇒ **the
   static TO-DO list is not the definition of what is missing.**

   ### ▶ Session 45's handoff (background): [session 45](log/sessions/session-45.md#-resume-here)

   ### ▶ Session 44's handoff (background): [session 44](log/sessions/session-44.md#-resume-here)

   ### ▶ Session 43's handoff (background): [session 43](log/sessions/session-43.md#-resume-here)

   ### ⏹ ---- FROM HERE DOWN IS SESSION 45 AND OLDER: BACKGROUND ONLY ----
   Every *"the frontier is …"* heading below was true when written and is not
   now. Paint's frontier went pixels → GDI → colour → save → the menu route.

   ### ▶ ★★★★★ MS PAINT RUNS, HAS ITS MENU, AND PAINTS (session 45)
   **Both north-star programs now run.** `PBRUSH.EXE` is a real sized, titled
   window on the XP desktop — its own icon, its own taskbar button, its **real
   menu bar** (File/Edit/View/Text/Pick/Options/Help, 62 items from its own
   resource) which **opens on Alt-F** — and it **paints**: it answers its own
   `WM_PAINT` with `MoveTo`/`LineTo`/`PatBlt`. It registers itself as an OLE
   server, reads WIN.INI, creates all five of its windows, takes and releases
   real DCs, loads its toolbox bitmaps, and its canvas has working scrollbars.
   ⚠ **Not yet correct** — the toolbox, line-size box and palette are ~1.35×
   too large so two of them fall below the bottom of the window, and the tool
   icons are not blitted. See the resume block.

   ### ▶ ★★★ THE PLAN ON RECORD PREDICTED NONE OF THE SIX WALLS
   Session 44 said Paint's next step was GDI's remaining calls **or** the
   sent-vs-posted split. Neither was the blocker. With `MessageBox` already in,
   Paint named its own walls: *"Failed to register server"* (SHELL's `Reg*` —
   and the anchor), *"Not enough memory to perform this operation"* (`GetDC`
   = 0), a GP fault (null `CREATESTRUCT`), *"Not enough memory to edit image"*
   (`LoadBitmap`), another GP fault (`GetObject` = 0), and finally the layout
   (`GetClientRect`). **Implement `MessageBox` first on any new guest** paid for
   itself a third session running.

   ### ▶ ★★★★★ AN ANCHOR MUST BE THE WHOLE STUB TABLE
   SHELL was anchored on `ShellAbout` **alone**. Paint never calls it, so SHELL
   was never identified at all and every `Reg*` — plus `DragAcceptFiles`, which
   had been **implemented since session 44** — was logged as *"?'s table"* and
   answered by nobody. ⚠⚠ **In a log, "nobody wrote this service" and "nobody
   identified this module" are the same line.** Anchors are now generated from
   the binary: `tools/ne/wowthunks.py --anchor` → `src/wow/wowanchors.h`.

   ### ▶ ★★★ STOCK ntvdm ON THE XP BOX IS THE ORACLE — NOT A Win3.1 INSTALL
   `wowcompare.bat` runs the **same** `PBRUSH.EXE` under ours and under stock at
   the same time on the same desktop, and `rigshot tree` (new verb) prints every
   matching window **and its children** with exact rectangles. That turned "it
   paints wrong" into a table, and the table named `GetClientRect`: Paint asked
   how big it was, got nothing, and laid itself out from WIN.INI's 1680×974 in a
   1252×688 client. **Nothing was wrong with the drawing** — it drew the right
   picture at the wrong size in a window it could not measure.

   ### ▶ ★★★ THE ORACLE IS STOCK ntvdm, AND THE MOUSE NOW REACHES A GUEST
   `wowcompare.bat` runs the **same** `PBRUSH.EXE` under ours and under stock at
   once on the same desktop, and `rigshot tree` prints every matching window AND
   its children with exact rectangles — that turned "it paints wrong" into a
   table. ★ Two of the smallest calls in USER (`IsWindow`, `IsWindowVisible`,
   both answered 0) were why Paint never *re*-computed its layout; answering them
   made every child match stock to the pixel. ★★ And `wowwin_proc` relayed no
   mouse messages at all, which is why a paint program could be looked at but not
   used — now relayed, with `WM_MOUSEMOVE` **coalescing** so the ring cannot
   flood.

   ### ▶ ⚠ THE GATE IS **`85 / 113 / 57 · 0001:229C`** (session 50)
   Was `81/122/61` (session 45), and `64/122/78` before that. Declined unchanged
   at 122, total identical at 264; the session-46 delta is **exactly one call and
   it has a name** — `GetProfileString(…, "NwcsInstalled", …)`, which the
   bootstrap asks during its NetWare-shim probe. Improvement, not drift.
   ⚠ **Run it with `wowsched.txt` and `wowcall.txt` MOVED ASIDE.** `wowlive.bat`
   creates them, and a gate run that leaves them in place measures a guest that
   runs much further (`238/308/101` over 663 BOPs) — which reads exactly like
   catastrophic drift and is a different configuration.

   ### ▶ ★★★★★ NOTEPAD IS A WORKING TEXT EDITOR (session 44)
   It **opens a file through the real XP file dialog, you type into it, and
   File > Save writes the text you typed** — verified byte for byte on disk.
   Help > About opens (`SHELL.22 ShellAbout`, its own icon). Its menus grey
   and check their own items. **32 of the 34 imports NOTEPAD.EXE makes into
   the 32-bit side are serviced**; the two left are Find and printing.

   ### ▶ ★★★ THE METHOD CHANGED — ENUMERATE WHAT ONE BINARY CALLS
   `tools/ne/neneeds.py` reads a program's import table and resolves each
   ordinal through the exporting module's own entry table to the bytes it
   lands on: a WOW32 stub is **our job**, anything else is the module's own
   16-bit code and is **free**. That turns *"what is next?"* into *"what is
   left?"* — a list with an end. ⚠⚠ An export does **not** point at its stub
   (COMMDLG prefixes a far call, USER/GDI **tail-jump**), and ⚠⚠ `native16`
   does **not** mean free: `MessageBox`, `LoadIcon`, `EnableMenuItem` and
   `CheckMenuItem` are wrappers reaching stubs the tool cannot see. **The run
   still finds those.** ★ The whole 19-guest shelf is ~**158** services against
   the ~1000 thunked entry points those modules define — which is why you
   enumerate per PROGRAM, not per API. ⚠ This also retires the *"GDI is 367
   stubs"* figure: **MS Paint needs 41.**

   ### ▶ ★★★★★ IMPLEMENT `MessageBox` FIRST ON ANY NEW GUEST
   It is how a Win16 program tells you what is wrong. Implemented, Notepad
   diagnosed its own failures in English three times in one session — *"Cannot
   open the … file"*, *"This file is empty and will be deleted"*, *"too large
   for Notepad"* (about a 59-byte file). All three had presented for hours as
   "nothing happens". Two sessions of guesswork ended on the first sentence.

   ### ▶ AND THE NEXT WALL IS ARCHITECTURAL: **SENT vs POSTED MESSAGES**
   Win32 **sends** `WM_INITMENUPOPUP` and `WM_PAINT` and expects an answer
   before it proceeds; this host can only **post**. ⚠ Measured consequence:
   the menu-state calls are correct and do **not** take effect while a menu is
   open, because Win32 runs the menu's modal loop **nested on the exec thread**
   and the guest cannot run until it returns. ⚠⚠ **`WM_PAINT` is a sent
   message, so this blocks GDI and therefore Paint** — implementing more GDI
   calls first will hit the same wall the moment Paint tries to paint.

   ### ▶ ★★★★★ NOTEPAD FROM WINDOWS 3.11 RUNS ON THE XP DESKTOP (session 43)
   One of the two north-star applications, **with its own menu bar (File / Edit /
   Search / Help), its own icon on the taskbar, the caption its program gives it
   ("Notepad - (Untitled)"), and its own message loop** — and it stays running
   until it is closed. Three services made it possible, each named by a call site
   rather than guessed: `0xad` *"build me a predefined cursor or icon"* (Notepad's
   init returns 0 if `LoadCursor` does), `0x76` `RegisterWindowMessage` (it
   registers the two `commdlg_*` names and abandons its init if either fails), and
   `0x91` `RegisterClipboardFormat`. ★ The **menu and the icon come out of the
   application's own NE file** (`src/wow/wowres.h`), and both layouts were decoded
   by an offline tool (`tools/ne/neres.py`) and confirmed against the data before
   any host code existed — a wrong offset does not spell *"&About Notepad..."*.
   ★ **Twelve Windows 3.11 guests are on the rig** (`C:\WIN16`, extracted with
   `tools/fat12.py`) and `scripts/wowtriage.sh` prints how far each one gets:
   TERMINAL also shows a window, PBRUSH builds ten classes, and every failure names
   itself. ⚠ Still missing: `MoveWindow`, so Notepad's edit control does not follow
   the window; `WM_COMMAND`, so the menu does not *do* anything yet.

   ### ▶ AND THE FRONTIER IS GDI
   A frame titled *"System Configuration Editor"*, four cascaded MDI children titled
   `C:\WINDOWS\SYSTEM.INI`, `WIN.INI`, `C:\CONFIG.SYS` and `C:\AUTOEXEC.BAT`, each
   with a real `EDIT` control holding the file's text, a taskbar button, and **no VDM
   window** — an NTVDMEX icon in the tray instead. Every window there is a real Win32
   `HWND`, because that is what WOW *is*: `wow32.dll` gives every Win16 window one, and
   that is why a 16-bit app on XP gets a real title bar, real focus and real clipping.
   ⚠ **The first attempt drew a Windows 3.x desktop INSIDE the NTVDMEX window and was
   thrown away** — see session 42 Part 0. If an answer involves inventing a desktop, a
   caption bar or a font for chrome, it is the DOSBox-shaped answer and it is wrong.
   ⇒ SYSEDIT works because almost nothing it shows is its own drawing. **MS Paint is not
   like that**: it paints its own client area, which needs `WM_PAINT` forwarded to the
   guest, `BeginPaint`/`EndPaint` on the real window, and **GDI's id space — 367 stubs,
   dispatched nowhere at all today**.

   ### ▶ Session 41's handoff: [session 41](log/sessions/session-41.md#-resume-here)
   That block is the live handoff — where it is, the leads already **ruled out**
   (do not re-try them), the next run, the instruments, and the standing hazards.
   Everything below it is background.

   ### ▶ AND THE FRONTIER IS PIXELS
   The loop turns, so the two things it exists for are the only things missing, and they
   are the same piece of work. A window here is a class, a rectangle, a style and a text
   behind a handle that says it is synthetic — **deliberately**, since session 39, because
   a half-built window that claimed pixels would lie about every question asked of it.
   Giving it a real host window is what makes `WM_PAINT` honest (a window that has been
   shown and never painted has an update region; one with no pixels does not), and
   `WM_PAINT` is what drags in `BeginPaint`/`EndPaint` and therefore **GDI's id space**,
   which this host does not dispatch at all. The nearest concrete steps: `ShowWindow`
   (`0x2a`) and `UpdateWindow` (`0x7c`), both called by SYSEDIT and both unimplemented;
   then `DefFrameProc` (`0x1bd`), where everything SYSEDIT does not handle goes.
   ⚠ Do **not** synthesise a `WM_PAINT` before there is something to paint on — a window
   that reports an update region it does not have is the same lie one level down.

   **Session 41 in one paragraph.** Two things, and the first was staged by session 40 as
   *"the next experiment, one instruction wide"*. A PM breakpoint at `krnl386
   seg1:0x4549` — the `jae` in `_lread`'s tail whose other arm is `mov ax,0xffff` — gave
   an **A/B inside one run**: `SYSTEM.INI` (0xe7 bytes) and `WIN.INI` (0x1dd) reach it
   with `efl=0x...206`, and the two **0-byte** files with `efl=0x...207`, i.e. **CF set**,
   while our `AH=3Fh` had answered `AX=0 CF=0` for all four. So the CF we return had
   *never* reached the guest, and the reason the other two worked is that `_lread`'s
   buffer probe (skipped on a zero-length read by `seg1:0x3d96 jcxz`) clears CF for the
   guest's own reasons. Where it went is three bytes: our default protected-mode handler
   for all 256 vectors is **`C4 C4 CF`** — the BOP, and then an **IRET** — so the very
   next instruction after a serviced call restores the flags krnl386 pushed at
   `seg1:0x5238` and discards ours. The answer now goes where a real `INT 21h` handler
   puts it, the caller's own flags image, which the **V86** arm has always done and the
   PM arm never did. ⚠ **CF only**: the other status flags in live EFLAGS are the guest's
   leftovers, and copying them would be inventing an answer in the one place a wrong bit
   cannot be seen. ⇒ *"Cannot read this file."* 2 → 0. ★★★★★ **Then the frontier.**
   SYSEDIT's loop is six calls and every one is named from its own relocation chain;
   `GetMessage` returning 0 is `WM_QUIT`, so the application was being *dismissed*, not
   failing. `src/wow/wowmsg.h` is the queue, and the host's own keyboard is what fills it
   — hung off `host_key_scancode`, the single choke point a scripted probe and a human
   press already share, with the **virtual key from `MapVirtualKey`**, i.e. the OS's own
   answer rather than a table written from memory. ★ **Four ids the export table could
   not name were named by the run**: with `GetMessage` answered they arrive as ordinary
   BOPs and their call sites name them — `0x71` TranslateMessage, `0x72` DispatchMessage,
   `0xb2` TranslateAccelerator, `0x1c3` TranslateMDISysAccel — and the `from` address is
   the *application's* rather than USER's because USER's exports reach their stubs by
   **tail-jump, not by call**. So the host dispatches too, and `DispatchMessage` is
   `wowcall_enter` with no new machinery under it. ★ The **MSG is 18 bytes** and both
   sides say so (`sysedit seg1:0x0102 lea ax,[bp-0x12]`, `user seg1:0x1c43 mov bx,0x12`).
   ⇒ **12 messages delivered and dispatched**, into the window procedure of the window
   the guest itself gave the focus to (`SetFocus`, four times, its own decision).
   ⚠ `TranslateMessage` returns **0** and that is the true answer, not a stub: a `WM_CHAR`
   needs keyboard state nothing here keeps, and guessing it would put *wrong characters*
   into an edit control. ⚠ **One queue, not one per task**, and the file says why.
   ⚠ `GetMessage`'s block is **bounded at 6 s** because a harness run has to end, and when
   it expires the log says *the wait expired* so that is never confused with a quit.
   ★ Measured three ways: frontier `661/186/308/150` ending on `ExitKernelThunk(0)`;
   **baseline exactly unchanged at `270/45/122/97 · 9·222·39 · 0001:229C`**; Doom's eleven
   startup stages, 3.51 MB, with the CF fix firing 0 times in it — predicted before the
   run, because DOS/4GW's `INT 21h` are patched sites in its own code.

   **Session 40 in one paragraph.** The host now **calls 16-bit code**, and the mechanism
   is small because three of its four pieces already existed: a saveable context (the
   0x40-byte VDM TIB block that `wowsched.h` established), a stack (the application's own
   — at a USER BOP the chain is app → USER stub → krnl386's thunk → BOP, all on it), and
   an entry convention **read out of the guest rather than a header**. The fourth is new
   and it is three bytes: **`C4 C4 57`** in guest memory with a 16-bit **code** selector
   over it, pushed as the far return address, so a window procedure's own `retf 0x0a`
   lands on a BOP. ⚠ It is dispatched by **linear address**, not by the code byte — our
   own INT-site patcher writes `C4 C4` too. ⚠ **`DS` on entry is the contract, not a
   detail**: sysedit.exe is `MULTIPLEDATA`, so the loader does not rewrite its
   `push ds / pop ax` prologue into `mov ax,<DGROUP>`, and the procedure takes its data
   segment from its caller — the host enters with `DS = AX = the window's own hInstance`.
   ⚠ The context is parked **after** EIP has been advanced past the BOP and the answer
   written; a context saved *at* the BOP is a loop, not a call. Contexts are a **stack**
   (depth 8) because re-entrancy is the normal case — the first thing SYSEDIT's
   `WM_CREATE` handler does is call `CreateWindow` again. ★ Three walls fell behind it.
   **The SYSTEM window classes belong to the 32-bit side**, because under WOW `USER.EXE`
   is a thunk module — and the run proves it without new measurement: four `RegisterClass`
   calls in a whole launch, all four a program's own. `MDICLIENT` is registered
   host-side (⚠ *only* what a run asked for — seeding `BUTTON`/`EDIT`/… would be answering
   questions nothing has asked). **`USER` id `0x217` is `NOTIFYWOW`**, named by USER's
   export table and pinned by its only call site, which is the whole of
   `LoadAccelerators`; ★ it does **not** return a handle — `seg1:0x3e37` hands the
   application `[bp-4]`, krnl386's own — it returns *permission*, so the answer is `1` and
   deliberately not something that looks like a handle. ⚠⚠ Its `lpResource` is stale one
   instruction later (`GlobalUnlock` at `seg1:0x3e23`), so it is **logged, not kept**.
   ★★ **Then the other half of the same mechanism went in and SYSEDIT built its whole
   interface.** `SendMessage` (USER `0x6f`) is **two mechanisms, not two cases of one** —
   to a window with a 16-bit procedure it *is* the call, and ★ the procedure's return
   value IS `SendMessage`'s (a second return mode in `wowcall.h`, `RESULT` vs `KEEP`;
   conflating them would be silent); to a **system-class** window the procedure is ours.
   The `MDICREATESTRUCT` was read off SYSEDIT's own stores at `seg3:0x0046`, and the
   reading confirms itself from outside the code: `ds:0x004a` in its DGROUP — read from
   the file — is `"mpchild"`, the class it registered two calls earlier. Measured: **four
   MDI children, each with its own `EDIT` control, each child's window procedure run by
   this host.** ★ `EDIT` went in because **the guest binary named it** (`seg1:0x0281`
   pushes `ds:0x003a`, which is `"edit"` in segment 6 on disk) — reading the guest binary
   is stronger evidence than waiting for the run line, not weaker. Two "runs but lies"
   defects only the new reach could expose: **the window extra bytes** (`Get/SetWindowWord`,
   bounded by the guest's own `cbWndExtra` — `mpchild` declares 8, exactly the four words
   its `WM_CREATE` writes; without them SYSEDIT sent `EM_SETHANDLE` to handle **zero**,
   having been told to forget its own control), and **krnl386 `0xd0` = `GetWindowsDirectory`**
   (unimplemented, the buffer kept another module's leftover string and SYSEDIT titled a
   window `"REGISTERPENAPP\SYSTEM.INI"` — not a failure, a *wrong name*). ⚠ **`WM_CREATE`'s
   `lParam` is 0 and the log says so on every line** — a `CREATESTRUCT` has never been
   built from measurement, and inventing one is how a wrong layout becomes a fact. ⚠ One
   more instrument was lying quietly: every USER call printed `[?'s table]` because
   `wow_module_of_sel` is bind-stage-only and cannot name a runtime selector — about a
   segment the dispatcher had identified and was routing on.
   ★★★ **And then the host called 16-bit code for its OWN reasons.** `0x040D` is
   **`EM_GETHANDLE`**, not `EM_SETHANDLE` (`0x040C` is), so the run was stopping on the
   *first* of a pair — and an edit control's text is a handle in the **application's own
   local heap**, which is not an assumption about Windows but what the program
   demonstrably requires: it hands the answer straight to `LocalReAlloc`/`LocalLock` with
   DS = its own DGROUP. The host cannot make such a handle and the guest's KERNEL can, so
   the host asks it: **`KERNEL.5 LocalAlloc` is entry-table `FIXED, segment 1, offset
   0x3ddb`, and krnl386's segment 1 is the segment every WOW32 BOP executes in** — its
   runtime address is `<the BOP's CS>:0x3ddb`, with no resolution machinery at all
   (`retf 4` and `test ax,0xf08d` on `[bp+8]` confirm the signature). That needed three
   generalisations of `wowcall.h`: an **argument list** instead of a message, a **sink**
   for a result that only exists after the service that asked for it has finished, and
   ⚠ a declared **return width** — `LocalAlloc` came back `0x00422502`, whose high word
   was *the flags we had pushed*, an instrument lying about a call the host itself made.
   ⇒ **`C:\WINDOWS\SYSTEM.INI` and `WIN.INI` are read into memory**, in blocks the
   application allocated, grew, filled and owns. ★★ **And the oracle then located a host defect that had always been there.** The other
   two files are **0 bytes** and get *"Cannot read this file."* — so `SYSEDIT` was run
   under **stock ntvdm on the same box**, and it opens all four with **no message box**
   (`docs/research/evidence/stock-sysedit-four-files.png`). ⇒ the message is **ours**.
   krnl386's own `_lread` says how: `jcxz` sends a zero-length read *past* the buffer
   probe at `seg1:0x4114` — whose `or` is what **clears CF** — straight to the
   `pushf / push cs / call` at `0x4530`, and `0x4549 jae` turns a set CF into `-1`. Every
   other read had CF cleared for the guest's own reasons, which is why a defect that was
   always there needed an empty file to expose it. ⚠ **Located, not fixed** — the fix has
   to know which flags image the guest restores from, and inventing that is how a host
   corrupts a stack. The next experiment is **one instruction wide**: a PM breakpoint at
   `krnl386 seg1:0x4549` to read CF at the `jae` that decides. ★ **The baseline moved by
   exactly one call and it has a name**: **270 / 45 / 122 / 97 · `9·222·39` · `0001:229C`**
   against session 39's 270/44/122/98 — `GetWindowsDirectory`, at line 615 of both logs,
   which krnl386 asks during its own bootstrap. Everything else is unchanged.

   **Session 39 in one paragraph.** `CreateWindow` is `USER.41`, id `0x29`, 30 argument
   bytes — and it was named without a single inference, because **a call site can name
   itself**. `nedis.py` prints every imported call as `lcall 0, 0xffff`, which is what
   is genuinely in the file: an unlinked NE stores a **chain** in the operand words and
   `0:0xffff` is the *end* of one. So the disassembly of a program that is almost
   entirely API calls names none of them, and the only method available was to read the
   pushes and recognise the shape — inference, which this project has twice written up
   wrongly. But each relocation record carries *(module, ordinal)* and the chain it
   heads lists **every site that takes that import**, so one walk names every `lcall` in
   the module (`tools/ne/neimports.py`, new). ⚠ **The relocation points at the OPERAND,
   not the instruction.** The argument block then had to be read the other way round
   from the parameter list — the base (`bp+16`) is the **lowest** address and holds the
   **last** word pushed, so a DWORD's high word is at the *lower* offset — and the data
   cross-validates that four times: `ds:0x00ae` decodes to `"WOWExecClass"`, `+18` reads
   `0x02CF0000` (`WS_OVERLAPPEDWINDOW|WS_CLIPCHILDREN`, where the other reading is not a
   style at all), `+10..+16` hold four copies of `CW_USEDEFAULT`, and `+4` is the same
   word that went into `WNDCLASS.hInstance`. A wrong assignment produces none of them.
   The window is **deliberately not a real `HWND`**: a host window would drag in a real
   queue, a real `WM_CREATE` and the thunk back into the window procedure, all of them
   half-built and lying by the time the call returned, so what exists is an object —
   class, rectangle, style, text — and a synthetic handle that says it is synthetic. An
   unregistered class still **fails**, because a host that made a window for any name
   would hide a broken `RegisterClass` behind a working `CreateWindow`. Measured: two
   classes, two windows, `[drivers]` read, and WOWEXEC in **its message loop**, spinning
   because neither `WowWaitForMsgAndEvent` nor `PeekMessage` has anything to return.
   One correction to session 38: `wowexec:0x0849` is `RegisterClass`'s error path, not
   `CreateWindow`'s — the string it pushes says so. One defect filed and not chased:
   krnl386 resolves bare module names against the **current directory**
   (`"C:\Documents and Settings\Matthew\MMSYSTEM.DLL"`), not the Windows/system
   directories.

   ★★ **Then the frontier moved off the message loop entirely, because the run said so.**
   WOWEXEC asks `WowGetNextVDMCommand` (`0x70`) exactly once — *"which 16-bit program do
   I run?"* — and the very next call after our sentinel `0` was
   `WowMsgBox("Can't run 16-bit Windows program", "Insufficient memory…")`. The message
   pump is what it does **after giving up**, and the program it wanted was in
   `target.txt` all along. ⚠ `0` is a HARD ERROR, not "nothing to do": `ret != 0` with
   `cbCmdLine == 0` is the quiet answer, so the sentinel was making WOWEXEC report a
   failure that had not happened. The command structure was read off WOWEXEC's own
   frame, and `+0x04` is the module name **because the success path pushes it into
   `KERNEL.LoadModule`** — named from the relocation chain. ⚠⚠ The command line is a
   **Pascal tail**, and the guest's `lstrlen - 2` means the delivered string must be
   `<tail text> CR LF`; an empty string makes the count byte `0xFE` and the program
   reads 254 bytes of stack as its arguments. ★★★ Behind that, one more wall, and ours:
   **the INT-site patcher had corrupted krnl386's code.** At `seg1:0x2051` the bytes are
   `3a cd 75 50` (`cmp cl,ch` / `jne`); the spanning `cd 75` was patched to `c4 c4`, the
   `jne` became `les dx,[bx+si+0xb]`, and every launch died at `0001:2053`. Found by
   diffing memory against the file (**one byte**), ruling out relocations, and then
   finding **the offset already printed in our own patch log** — a line read once and
   dismissed, because it rendered *candidates* under the words "patched N INT sites"
   (fixed). `x86len.h`'s narrow rule was right for its premises and **session 34 inverted
   them**: a raw `INT nn` in PM is now serviced from the `#GP`, with no heuristic at all,
   so a false reject costs one fault and a false accept still costs silent corruption.
   ⇒ **when in doubt, REJECT.** With both fixed, krnl386 opens `SYSEDIT.EXE` and reads
   `4d 5a`. The frontier is krnl386 id **`0x82`**, the last call before the load gives
   up. ★★★ **And Doom was re-measured — the change does not regress it, it FIXES it.**
   A/B on that one file, everything else identical: pre-fix Doom reaches **no** startup
   stage and loops in a `#GP` to a 268 MB log; post-fix it completes **all eleven**
   (`V_Init` → `ST_Init`) in 3.5 MB, reproduced twice. The prediction landed on the exact
   address it was made about — `#GP(IDT) is a RAW INT 0x21 at 0x0097:0x2c65`, DOS/4GW's
   version check, the one real site the old rule was shaped around keeping, now rejected
   by the scanner and serviced from the fault. ⚠ `dpmitest.com`/`pm32flat.com` pass but
   are **not** evidence: they never declare a code region, so they never take the changed
   decision — reporting them as "DPMI green" would have been another instrument that
   lies. ⚠ Still open: the `#GP(IDT)` arm does not cover a **base-0** code selector.
   ⚠ Also refuted this session: **`0x82` was NOT the blocker** — it is `INT 21h AH=3Bh`
   (`chdir`) and its call site already returns success (`clc`) on our sentinel. The real
   abort is krnl386 **seg2** id `0xd1` (`seg2:0x2c84`), read right after SYSEDIT's first
   code segment loads — and seg2 is a **third id space** (121 stubs) that this host does
   not dispatch at all.
   ★★★★ **And an experiment then took it much further.** Answering `0xd1` non-zero
   through `wow32ret.txt` (logged as an EXPERIMENT, never committed) makes krnl386 set up
   SYSEDIT's segment descriptors, **approve a task-database selector for it**
   (`0x7d -> 0x0bbf`) and look up `GetProfileInt("ModuleCompatibility", "SYSEDIT")` — it
   knows the module by name. ⇒ `0xd1` only has to *not be zero*, and **it is not the
   frontier.** What stops it next is the **module search path**: SYSEDIT imports
   `SHELL.DLL`, and `0xc5` — *the module-path resolver* — is unimplemented, so krnl386
   falls back to composing the name against the current directory and opens
   `C:\Documents and Settings\Matthew\SHELL.DLL`, which does not exist. The defect filed
   two sessions ago as cosmetic **is the launch blocker.** `0xc5`'s semantics are pinned
   by its two call sites: `(dst, src)` resolves and `(dst, NULL)` releases, and the
   success and fallback tails are *the same five-word call with the resolved far pointer
   substituted for the original* — which proves `dst` receives a **16:16 far pointer**,
   not a copied string. ⚠ That is why it is not a one-liner: the answer must be
   dereferenceable in protected mode, so it needs a **WOW scratch selector**
   (`g_pm_xfer_seg` is a V86 paragraph and is reused by every PM→V86 `INT 21h`).
   Resolution itself is not a guess — Windows/system directories then the path, i.e.
   `SearchPathA`.

   **Session 37 in one paragraph.** `GDI.EXE` was never rejected — we could not **open**
   it. `seg2:0x218a` has exactly two instructions that return `0x0B` over its whole
   `0x570` bytes, a breakpoint on each named `0x2242` (validation), and the first `0x40`-byte
   read there came back `AX = 6` — **ERROR_INVALID_HANDLE**, not six bytes — from file handle
   `2`. Handle 2 was never a handle: the open had failed and `2` is the DOS code the host
   returned for *every* open failure, so `-> AX=0x0002` in the trace meant two opposite
   things and the wall had been read the wrong way round off that one line. **`AL` in a DOS
   open is a bit field, not a number** — bits 0-2 access, bits 4-6 the SHARE.EXE sharing
   mode, bit 7 no-inherit — and the protected-mode arm compared the whole byte against 0 and
   1, so krnl386's `al = 0x80` fell through to "anything else" and asked Windows for
   `GENERIC_READ | GENERIC_WRITE`. USER.EXE imports GDI, so `GDI.EXE` was **already open and
   never closed**; a second open asking for WRITE against a `FILE_SHARE_READ` handle is
   `ERROR_SHARING_VIOLATION` (`gle=0x20`, measured), and GDI was the only module ever open
   twice at once. Access now comes from `AL & 7` on both arms, **we stopped enforcing a
   SHARE.EXE we do not emulate** (both arms share read *and* write, because bare DOS locks
   nothing), and a failed open reports the failure it actually suffered instead of "file not
   found". Two instruments had to be fixed first: `dpmi_bp_load()` read `char buf[1024]` and
   **silently dropped six of eleven breakpoints** behind a header comment, showing five
   confident arms and 42 hits while answering nothing; and the open trace printed AX without
   `AL`, `CF` or the Win32 error, while `AH=3E close` printed nothing at all. WOW32 `0x88
   GetDriveType` was also implemented — 26 stepped-over calls sweeping A: to Z:, whose only
   caller does `cmp al,2` — and it is a **closed gap, not a moved wall**.
   **Then WOW32 `0x080` turned out to be `GetPrivateProfileString`**, named not by inference
   but by its own arguments, which point at the DGROUP strings `"BOOT"`, `"WOWSHELL"`,
   `"WOWEXEC.EXE"` and `"SYSTEM.INI"`; thirty bytes later krnl386 hands that buffer to
   `LoadModule`. With it and `0x039 GetProfileInt` answered, krnl386 **completes its
   bootstrap and names the program it wants**. Three more walls then fell. `LoadModule`
   was failing on **one wrong byte** — `0x0e` in krnl386's per-drive flag table at DGROUP
   `0x2a2`, which routes every `AH=47h` for C: through a pre-handler that forces `CF` on
   every path — and that byte was written because our protected-mode `AH=44h` whitelist
   admitted `AL=00/06/07` and sent the rest to the TODO arm *"because everything else takes a
   DS:DX buffer"*: true of most of `AH=44h` and **false of exactly the three that classify a
   drive** (`08h` removable? `09h` remote? `0Eh` drive map?), which are register-only and
   which the V86 side answered with an `else { OKCF(); }` — *success*, with the caller's own
   registers as the answer. With them answered **`WOWEXEC.EXE` opens**, and WOW32 calls go
   237 → 2116. That run died in a **1884-iteration retry loop** (`seg2:0x2a08`: allocate, ask
   WOW32 `0x7d` whether the result is acceptable, on `0` allocate another — leaking two bytes
   of stack per iteration). `0x7d` is one of the 53 unnamed ids, so instead of guessing it a
   **knob** was built (`wow32ret.txt`, which changes an unimplemented answer for one run and
   logs every use as an experiment); with `0x7d → 1` the loop runs **once** and
   **`WOWEXEC.EXE` loads and executes**. It then takes a GP fault and krnl386 says so in
   Windows' own words — visible only because the string decoder, which had been gated on
   krnl386's own MessageBox id, now decodes *any* call's string arguments: the next message
   came through a different id from a different module. Two instruments were built on the
   way: **`pmchg.txt`** (which byte changed, and at which PM event — it named the drive-table
   write in one run) and **`wow32ret.txt`**.
   ~~⚠ The furthest point depends on an EXPERIMENT (`wow32ret.txt` must contain
   `7d 00000001`).~~ **CLOSED in session 38** — `0x7d` is a service now. Both its call sites
   are inside krnl386's task-database creator (`seg2:0x2984`, which allocates `0x320` bytes
   and stamps `"TD"` into them); the retry loop offers **aliases of the same memory**, so the
   question can only be about the selector's numeric value, and `seg2:0x2a22` uses the return
   **as** the selector — so the answer is to **echo the argument**. Measured: the one call is
   asked about `0x03b7` and the next LDT write gives that selector `limit=0x31f`.
   `wow32ret.txt` ships **empty** and `0 override(s)` is the correct deploy line.
   **What kills WOWEXEC is `LoadCursor(NULL, IDC_ARROW)`** — a NULL instance is the
   documented way to ask for a system cursor. USER passes it to `GetExpWinVer`, krnl386's
   `GetExePtr(0)` walks its task list and **matches krnl386's own bring-up record, whose
   instance handle is `0`**, then returns that record's module-handle field, `0xFFFF`, which
   USER loads into `ES` — `mov es,ax` at `seg1:0x229c`, `#GP` with `err=0xfffc`.
   **Session 38 resolved the fork, and the obvious branch is refuted.** `TDB+0x1c` has
   **exactly one writer in the whole of krnl386** — `seg2:0x2e02`, inside `InitTask`, from
   the module's own DGROUP — and the bring-up record never goes through `InitTask`. So its
   `hInstance` is zero on real Windows too and there is nothing there to fix; session 37's
   expected `0x001e` came from the stock pattern `hInst == SS` bar the low bits, and the
   bring-up record's `SS = 0x001f` is **our host's entry stack**, not a Win16 DGROUP.
   What is wrong is that the record is still **in the list**: krnl386 unlinks it itself at
   `seg1:0xcd36`, after `LoadModule` returns — and `LoadModule` never returns, because the
   new task runs to its fault inside it. ⇒ **an ordering defect, not a value defect.**
   Measured with two repeating breakpoints (`seg1:0x2225`, `seg1:0x9a16`): the list is
   `0x03b7 -> 0x01ef -> 0`, i.e. **WOWEXEC's task IS linked, at the head**, and `GetExePtr`
   skips it (`+0x1c = 0x03d7`) and matches the record behind it. The fault dump alone could
   not tell that from a one-element list, and an inference drawn from it was wrong.
   The task launch is also mapped end to end now — `seg1:0x97c2` parks the creating task's
   `SS:SP` in `DI:CX`, switches `SS:SP` to the new stack, calls WOW32 `0x74` (which carries
   `wExpWinVer = 0x030a`, read out of `ne_expver`), pops the Win16 entry frame and **`iret`s
   into the task at `seg1:0x9879`**.
   ★★ **And the next piece of work has a name: krnl386 HAS NO SCHEDULER — we are the
   scheduler.** All seven Win16 scheduling primitives — `Yield`, `OldYield`, `DirectedYield`,
   `WaitEvent`, `PostEvent`, `SetPriority`, `LockCurrentTask` — are **pure exported
   pass-throughs** to WOW32 with no 16-bit body and *no call or jump to them anywhere in the
   binary*. krnl386 keeps the state (the task list, each parked task's `SS:SP` at
   `TDB+0x02/+0x04`) and hands every decision to the 32-bit side, which on real WOW runs each
   task on its own thread and blocks it. We answer all seven with the harness sentinel, so a
   task that has been entered never gives control back. WOWEXEC's entry is the textbook Win16
   `__astart` and its `WaitEvent(0)` sits exactly between `InitTask` and `InitApp`.
   The common thunk (`seg1:0x2bb6`) pushes `[0x228]` before the BOP, parks the frame's
   `SS`/`BP` in `[0x6a4]`/`[0x6a6]`, and afterwards does `cmp ax,[0x228] / jne` into
   `seg1:0x98ab`. ⚠ **That is a RE-ENTRANCY GUARD, not a scheduling lever** — `0x98ab`'s
   *incoming* task is `AX`, the caller's own, so the sequence means *"someone else became
   current while I was in the 32-bit side; park them and put me back."* Writing `[0x228]`
   from the host does **not** yield; it parks the wrong stack in the wrong TDB and undoes
   itself. (This entry said the opposite for one commit — the correction is in the session
   log, with the instruction that settles it.) What survives is the **addressing**:
   `seg1:0x2bc9` makes the guest's `DS` krnl386's DGROUP at every WOW32 BOP, so `[0x228]` is
   readable for free — every call line now carries `task=0x....` and a run is a task timeline
   (`9 task=0 | 222 task=0x01ef | 27 task=0x03b7`, and never back).
   ★★★ **THE REAL LEVER IS THE EPILOGUE MODE.** The thunk does not have one return path, it
   has **38**: a mode word at `bp-0x18`, `push 0`ed at `seg1:0x2bc7`, popped at `seg1:0x2c0b`
   and dispatched through the table at `cs:0x2a36`. **krnl386 never sets it** (the only other
   access clears it), so 37 epilogues exist for the 32-bit side. **Mode 25 is the task
   switch-back**, pairing instruction-for-instruction with the launcher: `seg1:0x97be push
   [0x228] / push bp / mov di,ss / mov cx,sp` … `seg1:0x9827 mov ss,di / mov sp,cx / pop bp /
   pop [0x228]`. ★ **MEASURED**: returning `0x74` through mode 25 puts the creating task back
   on its own stack, current again — the `0001:229C` GP fault disappears — and with a non-zero
   launch result the boot task runs on to `seg1:0xcd0b` and `seg1:0xcd30`
   (`[boot] 386GRABBER`), **the read immediately before the unlink**, which no run had ever
   reached — and then **runs the unlink itself**: `seg1:0xcd36` unlink, `es:[0xfa]=0` (unsign
   the record), **`[0x228] = 0`**, `SS=DGROUP / SP=0x210`. **krnl386's boot task ENDS ITSELF**,
   and the `#GP` loop that follows is not a bug: `seg1:0x321f mov es,[0x228] / test es:[0x18],2`
   dereferences a **null selector** (`err=0`, exactly as logged) because nothing scheduled the
   next task. ⇒ the frontier is one fact: **`[0x228] == 0` means "no task is current; schedule
   one", and nothing does.** The remaining work is the **order** — return the creator first,
   resume the recorded `0x74` frame with mode 0 on that cue; the thunk frame *is* the context.
   New knob `wowmode.txt` (⚠ the most dangerous file in the tree — and a fault loop still
   makes a 268 MB log). See [`session-38.md`](log/sessions/session-38.md).

   **Session 36 in one paragraph.** The frontier moved from an address to a **module
   name**. Session 35's `WOW32_UNIMPL_RET = 0` — written but never run — turned out to be
   the largest single step of this epic: krnl386 goes from loading **one** system module
   to **six** (`SYSTEM.DRV`, `KEYBOARD.DRV`, `MOUSE.DRV`, `VGA.DRV`, `SOUND.DRV`, then
   `COMM.DRV`). Behind it were two walls, both ours and both *instruments* rather than
   mechanisms. First, **protected-mode `INT 15h` had no arm at all**: a COMM.DRV segment
   runs `mov ah,0C0h / int 15h`, every other BIOS vector krnl386 uses had a PM twin, and
   the raw `CD 15` was correctly identified as a raw INT, handed to a function with no arm
   for it, and fell out of the bottom as *"unexpected PM stop event=0x4"* — the run died
   two bytes into a driver, naming an address rather than a cause. It now answers exactly
   what the V86 arm answers, and `AH=C0h` is deliberately **refused** rather than stubbed,
   because the caller's next instructions read a **model byte** out of a table we would
   have to invent. Second, the WOW32 MessageBox decoder accepted only `0x20..0x7E`, so it
   printed the caption (identical for every module) and rejected the body —
   `"Please re-install the following module…\r\n\t\tCOMM.DRV"` — as "not a C string": **it
   named the class of failure and withheld the instance**. With both fixed, one repeating
   breakpoint at `seg1:0xcca4` reads the loader's return code per module, and `COMM.DRV`
   comes back **`AX = 0`** — not `2`/`4`/`0x0B`/`0x0F`, so "file not found / bad EXE / too
   many handles" are excluded by measurement. `COMM.DRV` is also the **only** one of the
   six that takes a `#NP` demand-load fault, and all ten of its segment-2 imports **exist**
   in krnl386/SYSTEM.DRV — checked, so that lead is closed before it was chased. A third
   defect cost a run on the way: **a one-shot breakpoint was the only kind that re-planted
   itself under a standing guest**, firing 512 times with byte-identical registers and then
   retiring before the pass it existed to observe. krnl386's own `/B` boot log was tried
   and **removed** — it self-disables silently, and so does the `[0x12b0]` poke.
   **Part 2 of the same session** narrowed it further: `LoadModule` (`seg2:0x051d`) returns
   **0**, which in Win16 is *"out of memory"*, not "not found" — and AX is already 0 at the
   earliest instrumented checkpoint, so the origin is upstream of `seg2:0x0e0b`. The
   relocation pass, the segment loads and COMM.DRV's own `LibMain` are all **closed by
   measurement**, and the `#NP` everyone would chase turns out to be krnl386 calling
   **`WEP`** — teardown, *after* the verdict. What is left is structural and visible in the
   files. **Part 3 closed it.** The bisect ran down through `LoadModule` — five stages, then
   an untested sixth, then the entry-point call — to `seg2:0x2da6 or ax,ax`, where AX is
   **the DLL entry point's return value**: `1` for all five that load, **`0` for COMM.DRV**,
   whose module handle was perfectly good. And COMM.DRV's own `LibMain` ends by returning
   **the word at `0040:0008`** — LPT1's base address in the BIOS data area. Nothing had ever
   written that table, **while our INT 11h equipment word (`0x4021`) declares one parallel
   port**: our own BIOS contradicting itself. Writing only what the equipment word already
   claims — LPT1 at `0x0378`, no serial ports, rather than inventing hardware nothing answers
   for — makes **COMM.DRV load, and `USER.EXE` behind it**. ⚠ One lead was **wrongly closed**
   on the way: "LibMain runs and takes this branch" was written up as "LibMain is not the
   cause", and it was the cause. *A measurement that something happens is not a measurement
   of what it returns.*

   **Session 35 in one paragraph.** No new wall — this one bought *understanding*, and
   corrected the plan. The harness logged an unimplemented WOW32 call as "registers
   untouched — the call did NOT happen", which is true of the registers and **false of the
   result**: the thunk does `sub sp,4` before the BOP and `pop ax / pop dx` after it, so a
   stepped-over call hands krnl386 a **stack hole nobody wrote**, and it branches on the
   litter. Printing that value settled two questions in a single run. `0xc6` handed back
   `0x01b7`, and its caller does `or ax,ax / jne <failure>` — so that failure was **ours**,
   not a decision. `0x2d` handed back `0x2714`, which is `>= 0x21`, so `LoadModule` took its
   **success** path into `les si,[bp+6] / mov es:[si+2],di` with a NULL parameter block —
   which *is* the terminal `#GP` session 34 deduced and warned against chasing, now measured.
   ⇒ **`WowLoadModule` is not the frontier**: it is only ever called because `LoadModule`
   already failed with `AX = 0x17` (the path is exact — `cmp ax,0x17` at `seg2:0x0f26`, and
   `0x0f28` then *overwrites* `lpModuleName` with `FFFF:FFFF`, which is why the call carries
   no module name), so implementing `0x2d` first would have been writing the handler for a
   failure we cause. The enclosing function meanwhile **names itself**: its own
   `"LoadStart = "` / `"LoadSuccess = "` / `"LoadFail = "` strings make `seg2:0x051c` Win16
   **`LoadModule`**, `retf 8`, `lpModuleName` at `[bp+0xc]:[bp+0xa]` and `lpParameterBlock`
   at `[bp+8]:[bp+6]` — and its narration is switched off only by a zero at `ds:[0x12b0]`,
   which is the cheapest way to find where `0x17` is really generated. Two lies were also
   fixed in `nedis.py`: capstone **stops dead at the first undecodable byte**, so a
   misaligned start produced a *silent empty window* (seg2 opens with a string), and
   `--wowfunc` scanned segment 1 only — it reported **`0 caller(s)`** for `0x2d`, the very
   call the run stops on, when there are **two**, the second a `WINOLDAP.MOD` fallback.

   **Session 34 in one paragraph.** DPMI exception delivery works, and nine walls behind
   it fell — every one of them ours. The first was never a missing frame: **NT builds the
   DPMI 0.9 16-bit exception frame itself** and leaves only the return `CS:IP` zero for the
   host to fill, measured against krnl386's deliberate `UD0` (an exception whose every
   field was known in advance) and confirmed independently by its own handler's writes to
   `[bp+8]`/`[bp+0xa]`. **The kernel's fault table is indexed by the x86 exception vector,
   not an NT "class"** — #UD arrives at 6 and #GP at `0x0d`, refuting session 19 and
   showing the 8-entry table could never reach a #GP at all. Then, in order: `INT 21h
   AH=52h` had no PM thunk (so `ES` was the null selector — that *was* the #GP);
   `SysVars+4`, the SFT chain head, was zero, and **a zero head is not an empty chain**, so
   krnl386 read the IVT as an SFT header and cycled forever (117 MB in one run); it counts
   file handles and **refuses 64** (it wants 100 or 127, both literals in its code), so the
   real table is 128; growing the SFT then starved the 256-vector PM handler table out of
   the host pool, silently, exactly as that function's own comment warned; WOW32 `0x98` is
   the file **seek** and was unimplemented, so every read after the first landed at the
   wrong file offset; `wowdecline.py` was **under-reporting** declinable sites because it
   only understood `je`, not the `jne` fall-through; **declining turned out to be a property
   of the CALL SITE, not the ID** — `0x97` has one site that chains to DOS and one that
   returns the failure to the app, and we were declining at both; and finally **a reserved
   LDT index is not a read-only one**: `INT 31h 04F2` discarded krnl386's re-base of
   selector `0x17`, so an image it staged there was read to a stale address while it walked
   the relocations at the new one. Finally, **a raw `INT nn` in protected mode is retired as
   a class**: krnl386 re-bases the initial CS over a block it fills *after* declaring it, so
   no commit-time scan can ever patch it — but a `#GP` whose error code has the IDT bit set
   IS that interrupt, and servicing it there (vector from the error code, confirmed against
   the `CD nn` bytes) turns the project's oldest silent VDM killer into an ordinary serviced
   call. PM step `0x63` → **`0xd9`**; "Missing 16-bit system module" cleared; SYSTEM.DRV
   loads. **Next: WOW32 `0x2d` WowLoadModule** — krnl386 handing a module to the 32-bit
   half, which is the 16→32 boundary itself rather than another one-line gap.

   ### Session 33 in one paragraph
   Stock ntvdm was used as an oracle for the first time *from the outside*: `tools/vdmdump` reads a live VDM's memory and its whole LDT
   (`ProcessLdtInformation` works on XP against another process), and
   `tools/ne/dumpscan.py` locates an NE's segments in the dump. That settled the
   layout — and then two hypotheses drawn from it were **tested and refuted**, which is
   how the real cause surfaced: `LoadSegment` never reads the file, it `rep movsd`s each
   segment in from a **staged image block** that is walked by *reclaiming* what has been
   consumed. Session 32 had set that reclaim's gap to zero on purpose (to stop it
   overwriting live code), so every segment was copied from offset 0 — the NE header.
   With the gap restored, segments 2 and 3 load, at the same heap offsets stock uses.
   The next wall, an "unimplemented native BOP", was a **swallowed `INT 21h`**: our
   patch map is keyed by linear address and krnl386 *copies* its patched code, so the
   `C4 C4` travels and the vector is lost — recovered now from the module's own file
   image. krnl386 then installs its INT 10h handler, registers a DPMI exception-6
   handler, and executes `0F FF` (UD0) **on purpose** to check it is reached. It waits
   there. **Next: DPMI exception reflection.**

   ### The bootstrap (session 30, unchanged and still true)
   On real hardware the **entire XP WOW module set loads, gets LDT selectors and
   binds**: krnl386 + system/keyboard/mouse/sound/comm drivers + gdi + user + shell
   + toolhelp + wowexec. Every import resolves; 27 descriptors installed and
   confirmed by `LAR` readback. Site counts match the off-VM battery to the digit
   (KERNEL 495, GDI 781, USER 1269, WOWEXEC 144). `src/wow/ne.h` + a 209-check
   battery over all 15 real binaries.
   ⚠️ **krnl386 is a LIBRARY, not a program** — no stack of its own, and its `CS:IP`
   is a DLL *init* entry. Bootstrap: init krnl386 → user + gdi → run **wowexec.exe**
   (the PROGRAM) → wowexec launches the app.
   ⚠️ **Load every module, assign every selector, then relocate ONCE.** Relocation is
   not idempotent. Entry indicator **`0xFE` is a CONSTANT**, and **ADDITIVE adds**.
   ⚠️ **Its init entry demands `AX == 0x4B4F`**, runs in **V86** (not PM), and turns
   itself into a 16-bit DPMI client. **LDT indices below `DPMI_LDT_RESERVED` are
   force-typed to data.** **A PM guest cannot reach the IVT**, so any `INT nn` absent
   from the patcher's list stays a raw `CD nn` and **silently terminates the VDM**.
   The `INT 2Fh 168A` vendor API is **REQUIRED**; our LDT is not user-mapped, so
   krnl386 gets a **descriptor-table shadow** reconciled on entry to any PM interrupt
   service. `04F2` = "commit CX descriptors from selector BX"; `04F1` = the private
   twin of `0000`.

   ### ★ The WOW32 half (session 31) — the interface is PINNED and 5 functions run
   The 16↔32 boundary lives in **exactly one module**: only krnl386 has these stubs,
   user/gdi/drivers funnel through KERNEL. `0x51` is the generic gateway and the
   whole interface is **82 integer function IDs**, now with **29 of them NAMED by
   krnl386's own export table** — no inference at all. See
   [`wow32-call-surface.md`](research/wow32-call-surface.md) for the frame diagram,
   the argument convention and the work list, and `src/wow/wow32.h` for the code.

   ⚠️ **Arguments are at `bp+16`, not `bp+12`.** Session 30's "VirtualAlloc's argument
   order is not pinned down, two readings possible" was an **instrument that lied** —
   the trace read four bytes low and printed the caller's far return address as the
   first two arguments. There was only ever one reading.
   ⚠️ **The return value is NOT a register.** The thunk does `sub sp,4` before the BOP
   and `pop ax / pop dx` after it. It must be written into that stack hole at
   `[bp-16]`. Getting this wrong is silent.
   ⚠️ **`SysVars+0x6A` was zero, and krnl386 WRITES through what it finds there.** Its
   init builds six far pointers into DOS's data area from a table named by that word;
   with SysVars zeroed those became offsets into `DOS_HDLR_SEG` — our own INT 21h BOP
   stub and DPMI entry points. `dos_wow_publish()` plants the table now, shaped like
   the one `lolprobe` measured off stock. **Clearing "error #2: Unable to initialize
   heap" needed this, not just the allocator.**
   ★ **Implemented:** `0xb8` VirtualAlloc (krnl386 services **DPMI 0501** with it),
   `0xb9` VirtualFree, `0xbc` GlobalMemoryStatus, `0xcf` GetSystemDefaultLangID,
   `0x78` (record the DOS data area).
   ★ **DECLINING IS A REAL ANSWER.** krnl386 hooks INT 21h in PM and chains to
   `cs:[0x3c]` — real DOS, i.e. **our own working layer** — when the 32-bit side
   returns `0xFFFF`. Seven file functions are declined and krnl386 now **opens a real
   file and gets handle 5 back**, which then appears as the argument to its
   subsequent get-date and close calls. ⚠️ Only where the call site says so:
   `tools/ne/wowdecline.py` finds three IDs where `0xFFFF` is a plain error.

   ### ★★ THE MEMORY MODEL (session 31) — krnl386 carves from `ES + 0x10`
   Its DPMI bring-up (`seg1:0xd688`) does `push es / int 2Fh 1687 / pop ax /
   add ax,0x10`, puts the DPMI host's private data there and grows **every** later
   allocation upward — **without a single INT 21h `AH=48h`**. So whatever sits above
   `ES + 0x10` is memory krnl386 believes is its own.
   ⚠️ Entered with `ES = DOS_PSP_SEG` it carved from `0x110`, where `dos_alloc` had
   already put **its own four code segments**. It now gets a real PSP block covering
   all remaining conventional memory, allocated last.
   ⚠️ **On the WOW path host memory comes from `wow_host_alloc()`, not `dos_alloc()`.**
   A `dos_alloc()` after `wow_place_v86` finds nothing, and the failure looks like the
   guest's fault — it cost two regressions in one sitting (the 168A vendor stub →
   "Inadequate DPMI Server"; the default PM handler table → `AH=35h` reporting vector
   0x21 as `0000:0000`).

   ### ★★ ERROR #3 CLEARED — krnl386 finds its own executable
   At `seg1:0xc257` it reads `PSP+0x2Ch` (the environment segment), scans past the
   strings to the **double NUL**, reads the count WORD and takes what follows as the
   program's full pathname — the MS-DOS 3.0+ convention, and **the only channel it
   uses**. `dos_psp_build` zeroes the first three bytes of the env block, which is
   right for a fresh PSP and destructive here; `wow_place_v86` rebuilds it with
   **krnl386's own path** (the `-a` argument).
   ★ Measured: `INT21h AH=3D open "C:\WINDOWS\SYSTEM32\KRNL386.EXE"`.
   **Two of krnl386's five errors are now cleared.**
   ★ `0xc9` = `GetCurrentDirectory` (INT 21h `AH=47h`; `AX=0x4717` at the BOP names
   it). One of the three that may **not** be declined. Unimplemented BOPs in a run:
   **zero**.

   ### ★★ THE NE HEADER IS PLACED — and krnl386 reaches LoadSegment
   Nothing in krnl386's bring-up reads its own NE header from disk (`seg1:0x1812` is
   `OpenFile(..., OF_EXIST)`, the `0xd02b` chain is structure-building, and the
   handle→selector path at `seg1:0xcf9f` never runs — all measured). It parses whatever
   is at `[0x5a0]`, the selector it builds over `base(SS) + SP` at `seg1:0xc17e`.
   ★ **That base is FIXED** — `SS:SP` is `0x1f:0x0FFE` at `seg1:0xc0d6`, `0xc123` **and**
   `0xc164`, the entry SP unchanged. So the loader can place the header there, and does:
   the header + tables are copied **immediately above krnl386's stack**, in the SAME DOS
   block (allocated separately they come out one paragraph apart — DOS puts an MCB header
   between allocations), and it enters with `SP` at the very top rather than top-2.
   ⚠️ Copy from the **NE header**, not the start of the file: every table offset in an NE
   is relative to the header, so it must be at offset 0 of that selector.
   ⇒ `ne_cseg` reads **4**, the copy loop runs four times instead of 65536, and the run
   goes from PM step `0x31` to **`0x3a`** with **12** WOW32 calls instead of 9.

   ### ⏹ SUPERSEDED (sessions 33-34) — the LoadSegment wall, kept for the method
   > This section describes where the run stopped in session 32. It is **no longer where
   > it stops**: segments 1-3 load and krnl386 now fails much later, on the *other*
   > system modules. Kept because the reasoning below is how the arena was understood,
   > and because two of its conclusions were later refuted.
   krnl386 builds its module database entry, then calls `seg1:0x90d9` for segment 1 of
   itself. It returns 0, so krnl386 takes `mov al,1 / call 0x987a` → WOW32 `0x02`
   **ExitKernelThunk(1)** → `int3`: a deliberate, traceable exit rather than a silent
   teardown. `0x90d9`'s early checks pass (`ne_cseg` = 4; it indexes `ne_segtab` at
   `es:[0x22]` with **ten-byte** records, exactly what `0xd45a`'s copy loop builds), so
   read on from `seg1:0x911d`.
   ★ **The failure is krnl386's RELOCATION pass, traced to the instruction.** Every
   step of `seg1:0x90d9` LoadSegment succeeds — `ne_cseg` check, self-load test,
   `flags=0xc142` (loaded), `handle=0x0207`, `call 0x937e` returns the handle, the
   relocation count is read off the segment — and then `call 0x8cb6` (**apply
   relocations**) returns **0** and `0x92b5` jumps to the failure tail. Both are
   breakpoint hits. ⇒ **This IS "two loaders, two copies"**, one step later than it
   looked: our NE loader already relocated the image that is *executing*, and krnl386
   loads its own segments a second time and relocates that copy itself.
   ★ **The relocation records were never copied into memory.** An NE segment with
   `NE_SEG_RELOCS` is followed *in the file* by a count word and 8-byte records; a
   conventional loader applies and discards them, but krnl386 re-reads them out of the
   LOADED segment (`seg1:0x921b`, then `0x8d54`). With only `length` bytes copied it was
   decoding whatever followed as records and taking them for **imported** fixups.
   `wow_place_v86` copies them now, and the walk measurably changes branch — `seg1:0x8dc7`
   (INTERNALREF) instead of `0x8d6a` (imported), with `@ds:si` byte-for-byte the file's
   records.
   ⚠️ It does **not** yet clear the wall: `0x8dc7` is entered once rather than four times,
   so record 1's fixup still does not complete.
   ▸ Next: read `seg1:0x8e0e` onward — handle `0x0207`, `test al,1`, the patch via
   `0x8e3f`.

   ★ **The module database it builds is WELL-FORMED** — dumped at `seg1:0x9145`
   (`esbase=0x1fca0`): starts `"NE"`, `ne_cseg=4`, `ne_segtab=0x40`, and all four ten-byte
   records carry both the loaded bit and a real handle (`0207/020e/0216/021f`), with
   seg4's minalloc grown to `0x1ba2+0xe00` by krnl386 itself. So the header placement is
   doing its job, `0x9068` is **never called**, and this is NOT the "two loaders" problem
   it looked like. Every check inside LoadSegment passes as far as `seg1:0x9183`; read on
   from there.
   ▸ `seg1:0x1493` is the segment-load **notification** (the source of the two `BOP 0x56`
   calls); `seg1:0x22b2` is **GlobalAlloc**.
   ▸ **CX at entry is a kept-but-UNPROVEN hypothesis**: `seg1:0xc164` turns CX into a
   paragraph count for the `[0x5a0]` arena and it measured 0 from entry, so it is now
   handed the window size. It changed nothing observable.
   ▸ Also decoded: **BOP `0x56` is the per-call 16→32 gateway**, sub-function on the stack
   at `SS:SP`, `add sp,6` after it. At `seg1:0x14cc` its return is not tested — a
   notification, harmlessly stepped over.

   ### ⚠️ Instrument hazards that cost this session, all now fixed
   ⚠️ **A 2-byte BOP over a 1-byte instruction eats its neighbour.** Breakpoints on
   `c3`/`1f`/`c9` silently changed what the guest did — the `c3` at `seg1:0x662f`
   ate the first byte of the instruction at `0x6630`, which sets AX for krnl386's
   first INT 31h, so it asked for `0x0000` instead of `0x000A` and died at PM step
   1. `dpmi_bp_arm()` now measures the instruction with `x86len.h` and **REFUSES**.
   *This also refuted an earlier conclusion in this same session* — "the breakpoints
   never fired, therefore that code is never reached" — when the run had died forty
   entries earlier.
   ⚠️ **THE EXECUTING krnl386 IS AT LINEAR `0x1410` (segment `0141`), NOT at the
   base the bind stage logs.** Breakpoints armed at `0x02950000+off` report
   themselves ARMED with the right displaced bytes and never fire — a dead copy.
   `csbase=` is printed on every PM heartbeat now.
   ⚠️ **`target.txt` leaked between DOS and WOW runs.** `rt.bat` writes it for every
   DOS test; `wowrun.bat` never set its own, so **every** WOW run of session 31 was
   told to load `C:\test\selftest.com` — a DOS `.COM` — as its Win16 program.
   `wowrun.bat` now establishes its input. No measurement taken while that was true
   can be trusted.
   ⚠️ **The watchdog thread logs ONE sample per WOW run and then stops**, for reasons
   not yet found; it is not a usable instrument here. The `PMHB` heartbeat comes
   from the main loop, which is provably alive, and `DPMI-BP HIT` now resolves DS/ES
   and dumps `@ds:si` / `@es:di` — a debugger that makes you guess where a selector
   points is most of the way to being no debugger.
   ⚠️ Before consulting any other NTVDM project, read
   [`reference-projects.md`](reference-projects.md).
   Still unknown: **`INT 31h 04F3`**, and what four of the six `SysVars+0x6A` pointers
   mean (two are pinned: LASTDRIVE and the current-drive byte).

   ### ▶ How to drive it
   `ARCHIVE=build/wowruns ./scripts/bmwow.sh` deploys and runs a WOW round on the rig
   (add `PMBP=1` to keep `pmbp.txt` armed). ⚠ **Two opt-in switches, and the frontier
   needs BOTH** — `touch /private/tmp/xpshare/wowsched.txt` (the Win16 task scheduler)
   and `touch /private/tmp/xpshare/wowcall.txt` (calling 16-bit code). Without them you
   are measuring the baseline, which is what they exist to preserve. ⚠️ **SMB writes to `/private/tmp/xpshare`
   need the sandbox disabled.** `dostrace.flag` turns on the INT 21h trace — it is
   **opt-in**, so its absence is not evidence. **Breakpoint addresses are LINEAR =
   `csbase + offset`, and `csbase` moves whenever an allocation size changes** — derive
   it per run from a `cs:eip=0x0000000f:` line, and **always check the armed line's
   `displaced <bytes>` against `nedis.py`**. Full operational detail, including the
   immediate next breakpoints, is in
   [`session-31.md`](log/sessions/session-31.md#-resume-here--the-operational-detail-a-fresh-context-needs).

   ### Tools for this work
   `tools/ne/neimports.py` (**names every imported call site** from the relocation
   chains — the only non-inferential way to say what an `lcall 0, 0xffff` calls),
   `tools/ne/nedis.py` (16-bit disassembly with the WOW32 stubs named inline;
   `--wowfunc <id>` gives the stub, its callers and the argument-building code),
   `tools/ne/wowmap.py` (names the surface from the export table),
   `tools/ne/wowdecline.py` (which calls may be declined), `scripts/bmwow.sh` (drive a
   WOW run on the rig through controld, which the watcher path cannot do).

2. **[#131] Console/stdio integration.** Independent of WOW and needed regardless:
   anything script-driven behaves differently under NTVDMEX than under stock.
3. **[#130] Installation & routing.** Blocked on #128 — an installer is not useful while
   installing breaks every 16-bit Windows program.
4. **Known DOS defects** — #133 redirection, #134 the `$p` prompt, #47 MEM.EXE lying.

---

## Getting started on a machine that has never seen this

```bash
# Build (macOS/Linux cross-compile to XP-32; needs mingw-w64 i686)
./scripts/build.sh                 # -> build/ntvdmhost.exe

# Fast test loop -- no VM and no rig needed. Builds the batteries, then runs them.
./tools/dostest/run.sh                 # 18 batteries, 839 checks, ~10s, non-zero on failure
```

- The build is **no-CRT on purpose**: the toolchain is UCRT-default and UCRT is absent on
  XP, so a CRT-linked binary will not load there. `src/runtime.c` supplies the entry point
  and `mem*` primitives. Verify with `./scripts/check-imports.sh`.
- **`build/ntvdmhost.exe` is the host.** `build/ntvdmex.exe` is a small separate launcher.
  Deploying the wrong one has cost more than one session — checksum what you deploy.

For the bare-metal rig, the oracles, and how to run anything against real hardware, see
the wiki's testing pages. **Do not skip them**: the single most reliable way to waste a
day on this project is to measure stock `ntvdm` by accident and believe the result.

---

## How to read the rest of the docs

| Path | What it is |
|---|---|
| [`docs/log/sessions/`](log/sessions/) | The day-by-day archive, verbatim, refutations included. |
| [`docs/decisions/`](decisions/) | Architecture decision records. |
| [`docs/research/`](research/) | Raw findings — disassembly, kernel RE, oracle disagreements, measurement runs. |
| [`docs/research/wow32-call-surface.md`](research/wow32-call-surface.md) | **The 82 WOW32 functions krnl386 needs**, with argument sizes. The #128 work list. |
| [`docs/research/evidence/`](research/evidence/) | Screen captures that back specific claims. |
| [`docs/ROADMAP.md`](ROADMAP.md) | Milestones. |
| [`docs/GLOSSARY.md`](GLOSSARY.md) | VDM, VDD, DPMI, WOW, thunk, BOP, IFEO… |
| [`docs/risks.md`](risks.md) | Standing risks. |
| [`docs/reference-projects.md`](reference-projects.md) | **Read before consulting any other NTVDM project.** What we may and may not read, and why. |

---

## The one thing to internalise

This project has repeatedly been wrong in the same way, and it is always the same shape:
**an instrument that lies**. A counter whose layout implies a claim it cannot support. A
video metric that moved the wrong way when a bug was fixed. A test that silently ran
against stock `ntvdm`. A dialog checker that reported 47 problems of which 45 were its
own. A "before" and "after" run that analysed the same stale file.

The habits that actually work, learned the expensive way:

- **Build ground truth from something that is not us** — the game's own WAD, a real MS-DOS
  under QEMU, stock `ntvdm`, Nuked-OPL. Oracles vote on truth; NTVDMEX does not vote.
- **Read the guest binary.** Disassembling `DOOM.EXE` fixed in an hour what twenty runs of
  host instruments could not.
- **When a guest dies at an address, diff the bytes there against the file on disk.** That
  found a five-session bug that was ours all along.
- **A trace that prints the request but not the answer is half an instrument.**
- **Predict the number before the run.**
- A fix measured on one guest is a fix for none — re-run the other class (V86 vs DPMI).

---

## Session 52 (2026-09-05) — the completeness push, 61.7 → 72

**Score 61.7 → 72.0** (`./tools/score/score.py`). Suite **893 → 1036** checks.

### Closed, each rig-gated against the MS-DOS 6.22 oracle
| | |
|---|---|
| **#133** `echo x > file` | oracle, rig AND stock ntvdm agree on all six cases |
| **#50** EXEC `AL=01`/`AL=03` | probe builds its own relocatable `.EXE`; `SP = e_sp-2`, measured |
| **#45** INT 14h/17h | ports now agree with the equipment word; printed bytes read back as `Hi` |
| **#49** TSR residency | child hooks INT 60h, TSRs, parent calls it: `AX=BEEF` |
| **#52** user fonts | `AH=11h AL=x0` glyphs are actually drawn |
| **#44** INT 13h/25h/26h | real disk images; every oracle field matches |
| **#132** recovery | 3 failed starts and the host **removes its own IFEO key** — proven on hardware |
| **#15** | **REFUTED**: all seven ModRM store forms already work |

### ★ #47's root cause was ours, and it was a layout collision
MEM.EXE does not ask the XMS driver how much extended memory exists — it reads
`SysVars+0x45`. `DOS_SDA_OFF` was `0xD4` = `SysVars+0x44`, so **`DOS_INDOS_OFF`
WAS `SysVars+0x45`**: MEM read the InDOS flag, saw zero, and skipped the whole
report. Seven driver-side hypotheses died before the binary was read.

**Still open**: `Total` is off by exactly the phantom `Upper 1,663K`. MEM's MCB
walk is at image `0x1759`; the segment map is cracked (**CS = load segment, so
IP == image offset**) — see [[mem-exe-segment-map]].

### ⚠ Hazards this session paid for
* **The rig cannot be recovered remotely from a modal dialog.** Wedged twice:
  `GetDiskFreeSpaceA` on an empty floppy, and stock ntvdm's "cannot find the
  file" box. `kill`, `reboot`, `shutdown` all failed; only `exec runwatch.bat`.
* **A wedged stock run leaves the IFEO Debugger key REMOVED**, after which every
  later run silently measures stock ntvdm. Check `stock_state.txt` + `reg query`.
* **`copy` preserves mtime**, so a stale result defeats even an mtime check —
  `rt_stock.bat` reported one wedged run's output as five different rows' answers.
* Two host-log lines written at startup never survived: a later
  `log_write(LOG_PATH,…)` **truncates**. Report at exit.

---

## Session 53 (2026-09-06) — settings, #47, the menu defect, and the guest shelf: 72.4 → 79.6

**Score 72.4 → 79.6** in one day. Suite **1036 → 1086** checks, all green, all rig-gated.
Morning: settings, #47, the PC speaker. Afternoon: the menu defect and the guest shelf.

### ★ The PC speaker made no sound at all

`vdd_speaker.c` has modelled port 0x61 and reported PIT channel 2's tone since M3,
and **nothing ever turned that into a sample** — while the score line read
"SB16 PCM, OPL2/3 FM, MPU-401, speaker" throughout. It is a square wave gated by
two bits, so it is a dozen lines in the mixer: phase as a 16-bit fraction of a
cycle (the top bit *is* the half-cycle), amplitude well below full scale, and
tones past 20 kHz refused rather than aliased. Measured back out of the mix at the
frequency the PIT was programmed to.

**The same line also claimed OPL3 and never had one** — `vdd_opl` is a 9-channel
OPL2. Corrected in `tools/score/model.json` rather than left flattering, and the
`Opl` combo is deliberately **not** wired for the same reason.

### Sixteen more settings do something (7 → 23 of 47 rows)

| Page | Now live |
|---|---|
| Audio | `MasterVolume`, `Mute`, `SampleRate`, `PcSpeaker`, `SbAddress`, `SbIrq`, `SbDma` |
| Display | `WindowSize`, `Scaler`, `AspectRatio`, `Filtering`, `VSync`, `FrameSkip` |
| Memory | `Xms`, `Ems` |
| Drives | `FloppyAImage` |

⚠ **`BLASTER` had to move with the card.** The moment the Audio page can change the
Sound Blaster's port, a hard-coded `BLASTER=A220 I5 D1 T3` is a **lie told to every
guest that reads it** — a driver that believes the string masks the line it was told
about and waits for an interrupt that arrives elsewhere. `dos_env.h` builds the string
from a `dos_sbcfg`, `main.c` configures `vdd_sb` from the same struct, and with no card
supplied the output is byte-for-byte the literal it replaced.

⚠ **The `SbDma` list contains a channel that is not an 8-bit channel.** "5" is the
*sixteen*-bit channel on an SB16; selecting it moves `H` and leaves `D` alone.

⚠ **Recorded, not silently fixed:** the string says `T3` (an SB 2.0) while the DSP
reports 4.05 (an SB16, i.e. `T6`). That predates this change, Doom's audio is
user-confirmed against the string as it stands, and correcting it is a measurement
to make on the rig — not a tidy-up.

### ★ The apply function had to split three ways, and the reason is a real trap

The mixer and the presenter **zero their own struct when they initialise**, so a
setting pushed into them at the top of `WinMain` is written into a struct that is
about to be wiped. But `settings_apply()` cannot simply move later — every knob in it
also has a text file on the test share and **the file wins**, which only works because
it runs *before* the file-knob block. So:

- `settings_apply()` — before anything is built, before the file knobs.
- `settings_apply_devices()` — after the mixer exists.
- `settings_apply_present()` — on the UI thread, after `present_ddraw_init()`.

None of the late ones has a file-knob twin, so the precedence rule costs nothing.

### ⚠⚠ THE RIG WAS UP THE WHOLE TIME AND I FILED IT AS DEAD

The first commit of the day says *"not rig-gated — the bare-metal box did not
answer"*. It answered fine: **LAN access from the build machine needs
`dangerouslyDisableSandbox`**, and the first probe ran inside the sandbox, so a
silent connection failure read as a dead box. Half a session's worth of work was
planned around a constraint that did not exist. ▶ **Before concluding the rig is
down, re-probe with the sandbox off.**

### What the rig then found, and what it proved

**Found:** `WindowSize` defaulted to index 1 = **2x**, harmless for as long as it did
nothing and a 1280×800 client area the moment it became live. The rule in
`settings.h` is that *the defaults are the shipped behaviour*. Fixed to 1x, plus a
work-area clamp so 3x on a small desktop steps down instead of hanging its status bar
off the bottom edge.

**Proved** (each run's own log confirms the settings it ran under):

| | |
|---|---|
| `selftest.com` | 8/8 with the new build |
| `p_disk.com` | every oracle field unchanged; the floppy path is a variable now and still resolves to the harness fallback |
| `Xms=0` | guest reports **XMS FAIL @1** — `INT 2Fh 4300` does not answer, like a box with no `HIMEM.SYS`. Not an entry point that then refuses every call |
| `Ems=0` | **EMS FAIL @1** — no `INT 67h` vector *and* no `EMMXXXX0` name |
| card moved | `A240 I7 D3`, selftest still 8/8 |
| `p_mcb.com` | MCB chain byte-identical — the rebuilt `BLASTER` is the same length, so the env block did not move |
| `AspectRatio=1` | black bars of **exactly 54 px** each side, 532 px of picture between them; `present_fit` predicts 53/533 |
| `Scaler=Scanlines` | alternate **physical** rows exactly 0: `0.0, 156.5, 0.0, 164.7, 0.0, 119.5 …` |

Both registry knobs were restored afterwards and **proven restored by `reg query`** —
the same shape as `rt_stock.bat` leaving the IFEO key absent.

Evidence: `docs/research/evidence/session53-display-{aspect,scanlines}.png`.

### ★ #47 CLOSED — and it was the SAME BUG SHAPE AS ITS OWN ROOT CAUSE

Session 52 found that MEM reads `SysVars+0x45` for extended memory and our SDA sat
on it. The **remaining** half — the phantom `Upper 1,663K` — was a *second*
absolute-offset read, and the giveaway was in the report all along: `Largest free
upper memory block 548K (561,264 bytes)` was **our own free conventional block**,
one MCB header away from its real size.

**MEM asks `AH=52h` for SysVars, keeps the SEGMENT, throws the OFFSET away, and
reads `<SysVars segment>:0x008C`** (mem.exe image `0F88`/`0F8E`/`0F95`). That word
is the conventional/upper line; every block in the walk is bucketed by
`segment >= it` (`0x1304`, `0x31DB`). We left it **zero**, so every segment
compared `>= 0` and the entire chain was filed as upper memory.

MS-DOS 6.22 has **`0xFFFF`** there — and it was in a dump this project had already
taken: `lolprobe-msdos622.txt`, offset `0x8C` = `FF FF`, with `0x0253` at `0x8E`
which is *also* what 6.22 reports at `SysVars-2`. So `0x8C`/`0x8E` are "first
UMB" / "first MCB", and our MCB head already landed on `0x8E` by luck.

| | before | after (rig) |
|---|---|---|
| Conventional | 639K = 639K + **0K** | 639K = 4K + **635K** |
| Upper | **1,663K** = 1,028K + 635K | **0K** |
| Extended (XMS) | 14,721K, used `4,192,6` | 16,384K = 0K + 16,384K |
| Largest executable | **0K** (4,294,967,280 bytes) | **635K** (650,240 bytes) |

Every row is now self-consistent, and the Extended row **corrected itself** — MEM
derives one row from the grand total, so the phantom had been stealing 1,663K from
it. ★ `./scripts/oracle.sh --batch MEM` on genuine 6.22 prints the **same seven
rows with the same labels**; the numbers differ honestly (our resident DOS is 4K
where 6.22's is 19K).

`DOS_SYSVARS_OFF` moved from `main.c` into `dos_layout.h`: **two fixed addresses in
that segment are load-bearing for MEM**, and the constant they are measured against
belongs beside them. Six off-VM checks assert the relationships, including a guard
that InDOS has not drifted back onto `SysVars+0x45`.

▶ **The general lesson, now paid for twice:** *a guest can read a fixed address in a
segment we handed it, with no call to intercept and nothing in any log.* When a
guest reports a wrong number and the trace is clean, **disassemble it and look for
absolute offsets in our own segments.**

### ★ The PC speaker, end to end — and the question that found the real gap

The synthesis was measured off-VM and rig-gated, and the user then asked the
question no counter had: **which speaker?** It was the sound card — a square wave
summed into `waveOut` — and that was a choice made silently. On a box with
nothing plugged into line out, that is inaudible and *indistinguishable from a
broken emulator in every counter the host had.*

`PcSpeaker` is now **Off / Sound card / Real PC speaker / Both** (the old checkbox
values migrate for free: 0 and 1 were already those first two).

⚠ **The real speaker is NOT driven with `Beep()`.** It blocks the calling thread
and wants the duration up front; a guest opens the port-0x61 gate and closes it
whenever it likes, so no `(frequency, duration)` pair expresses "sound this until
I say stop" — and 1-bit sample playback through the speaker would be
unrepresentable. The driver underneath *does* have that shape:
`IOCTL_BEEP_SET` with `Duration = 0xFFFFFFFF` sounds until told otherwise and
returns immediately.

⚠⚠ **AND THERE IS NO `\\.\Beep`.** The obvious spelling returns
`ERROR_FILE_NOT_FOUND` on a box where `sc query beep` reports the driver
**RUNNING**: Beep.sys creates `\Device\Beep` and publishes **no `\DosDevices`
symlink**, so the `\\.\` prefix has nothing to resolve — `Beep()` itself opens the
native path. `\\?\GLOBALROOT\Device\Beep` is the way in.

⚠ Stopped on **every** exit path, including the headless one that never sees
`WM_DESTROY`: the driver keeps sounding after the process that started it dies.

**USER-CONFIRMED BY EAR, both paths** — the scale and arpeggio play correctly out
of the motherboard speaker *and* out of the sound card. The original "nothing at
all" was environmental (muted or unplugged at the time), not a defect: every
counter in between read healthy throughout, which is exactly why the counters were
added rather than trusted.

### ⚠ OPEN: ~21,700 I/O TRAPS PER INSTRUCTION, INTERMITTENTLY

Some runs of `spktest.com` report `io_events` of **1.87 million** for a program
that issues **86**, and those runs take **8.7 s** where the clean ones take 4.6 s.
The `IO-SITE` log shows the **same six CS:IP sites** in both cases, all inside the
guest — so the exec loop is re-servicing an I/O event the guest already retired,
about 21,700 times per instruction, and burning ~4 s doing it. The guest still
completes correctly (12 dots, right note durations), which is why nothing ever
caught it.

▶ The instrument for it already exists: `spktest.asm` prints one dot per note, so
"the loop is spinning" and "that counter is not counting what its name says" are
distinguishable in a single run — which no host-side counter can do alone.

### Still open, and honest about it

- **The PC speaker's sound has not been listened to.** The synthesis is measured;
  the audibility is not. That needs ears, not a rig.
- The **CPU page** needs a duty-cycle throttle on the exec loop — there are no cycles
  to count on a real CPU. This is the user's standing "approximate CPU speed"
  request (33/66/100/200 MHz) and it is the largest remaining piece of `settings-live`.
- `ConventionalKB`/`Umb` need `DOS_MEM_TOP` to stop being a compile-time constant (#47).
- `Renderer` needs a windowed DirectDraw blit that does not exist.

---

## Session 53, afternoon — the guest shelf, 75.1 → 79.6

### ★ The menu defect (#136) — closed, and the fix is what stock DOES

MS Paint takes the mouse capture on button-down and **never gives it back** —
nine `SetCapture`, **zero** `ReleaseCapture` in one measured session, and it
*re-takes* it after every button-up. That is legal Win16. But **USER32 refuses
`SC_KEYMENU` while the calling thread holds a capture** (menus deliberately do
not open mid-drag), so `DefWindowProc` did nothing with Alt and the menu bar was
dead for the rest of the session.

The fix is not an invention. A new `rigshot capture` verb reads another thread's
capture cross-process (`GetGUIThreadInfo` — `GetCapture` is per-THREAD and would
have answered "nobody" from another process no matter what):

| after three drags | ours (before) | stock |
|---|---|---|
| `hwndCapture` | canvas | canvas — **same** |
| then Alt | unchanged, no menu | moves to the top-level, `GUI_INMENUMODE` |

⚠ **Stock gets it free because its thread reports `GUI_16BITTASK` (flags 0x20)
and ours reports 0.** USER32 gives its own WOW threads special menu handling and
that flag is not settable from outside. Reproducing the *behaviour* is the route.

### ★★ The shelf: four service batches, ~80 services, four new guests

The loop that worked, four times: **`neneeds.py` prices a guest by name and
ordinal → implement a batch chosen for OVERLAP across guests → launch → THE RUN
NAMES THE REAL BLOCKER.**

| | |
|---|---|
| **done** (user-confirmed by hand) | NOTEPAD, PBRUSH, **SOL**, **WINMINE**, **CHARMAP** |
| **partial** (launches, job unproven) | SYSEDIT, **CLOCK**, **MPLAYER**, **RECORDER**, WOWEXEC |
| guests 21% → 39%, breadth 61% → 83% | |

⚠⚠ **`neneeds` SAID CALC WAS 0 TO DO AND THE RUN SAID OTHERWISE.** It classifies
`CREATEDIALOG`/`DIALOGBOXPARAM`/`CREATEDIALOGPARAM` as **`native16`** — USER
implements them in its own 16-bit code — and **native16 is not free**: that code
calls out to a 32-bit helper at **USER thunk id `0xEF`, an id NO EXPORT MAPS TO**
(USER's wow32 id space skips exactly the native16 gaps). ⇒ **The run is the
oracle; neneeds is a hint.**

▶ **DIALOGS ARE TOMORROW'S FIRST JOB and the entry point is pinned.** USER has
already done `FindResource`/`LoadResource`/`LockResource` by the time it calls us,
so the locked `DLGTEMPLATE` is handed over. Args at thunk `0xEF` (22 bytes):
`+16/18` template far pointer, `+10/12` dialog proc, `+14` parent, `+6/8`
init param, `+20` hInstance. It unlocks **TASKMAN, CALC, Solitaire's Options and
Deck, and Minesweeper's preferences** — the user named the last two unprompted.

### ★★★ THE SENTINEL-IS-AN-ANSWER SHAPE, TWICE MORE

* **`SPI_GETICONTITLELOGFONT`** — MPLAYER produced no window. I had
  `SystemParametersInfo` answer FALSE and deliberately *not* touch the buffer;
  MPLAYER called `CreateFontIndirect` **on that buffer anyway**, building a font
  from stack litter. *The return value was not what it acted on; the buffer was.*
  ⚠ A Win16 `LOGFONT` is **50 bytes, not 60** — every metric a WORD, face name at
  18 not 28.
* **The dialog create returning 0** — CALC's log then reads `ShowWindow 0x0000`
  and every `CheckDlgButton`/`SetDlgItemText` on window `0`, forever.

### ⚠ THREE USER-REPORTED DEFECTS, SETTLED BY RUNNING THE SAME PROGRAM UNDER STOCK

1. **"Our Win16 chrome is flat, stock is bevelled"** — **refuted**. Character Map
   diffed against stock: font row **0** of 13,440 pixels differ, character grid
   **0** of 78,400, same Luna caption. The flat look is what a Win16 app gets from
   *both* hosts; a bevelled one ships `CTL3D.DLL`.
2. **"Recorder's window is massive and blank"** — **refuted**. Stock gives it the
   identical 1260×742. It sizes itself from the screen; on a 640×480 display that
   was the small window we remember.
3. **"Alt stops opening the menu"** — **real**, and fixed above.

⚠ And on (1): the title-bar *region* diffed 15.7% and that was **desktop
wallpaper at the rounded corners**, not chrome. A percentage over a region that
includes the background is not a claim about the foreground.

### Still open, in the order I would take them

1. **Dialogs** (thunk `0xEF`) — 2 guests + 2 confirmed sub-dialogs. Entry pinned.
2. **CLOCK's blank face** — a paint→invalidate→paint loop at ~555/s (19,929
   `WM_PAINT`, 9,964 `InvalidateRect`, one per paint, 50 `WM_TIMER`). It never
   calls `GetCurrentTime`, and **the BDA tick rate is correct** (816 ticks over
   ~45 s of guest time = 18.1 Hz), so both obvious hypotheses are dead.
3. **`MEM /C` reports MSDOS as 1,028K** — a per-module row that contradicts the
   summary in the same report, and 1,028K is 1024K + 4K: the same phantom-megabyte
   signature as the Upper bug fixed this morning.
4. `CallWindowProc`, `CreateWindowEx`, `ShellExecute`, `EnumTaskWindows` — one
   each blocks CARDFILE, WINFILE and PACKAGER.
