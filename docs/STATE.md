# Project state — start here

> **This is the canonical resume point.** If you have never seen this project before, read
> this file top to bottom and you will know where it is, what works, what does not, and
> what to do next.

- **Last updated:** 2026-09-04 (session 49)
- **Sessions 46-49 are committed and pushed** on this branch — the whole WOW service
  batch, the LDT pool and the `tools/ne/neneeds.py` fix.
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
| **The original DOS games bar** — Doom / Skyroads / ZAR, flawless sound | Two of three fully playable and confirmed by hand. ZAR is the gap (VBE 2.0 hi-colour + linear framebuffer). | **~85%** |
| **★ The north star** — MS Paint + Notepad from Windows 3.x | **BOTH RUN AND BOTH DO THEIR JOB, USER-CONFIRMED.** **NOTEPAD IS A WORKING TEXT EDITOR**: opens through the real XP file dialog, you type into it, File > Save writes the text (verified byte for byte). **MS PAINT DRAWS IN COLOUR, KEEPS WHAT IT DRAWS, AND SAVES IT** — every shape tool, the flood fill, persistence across a repaint, and `File > Save As` writing a valid 1680×974 24-bit `.BMP` to the chosen directory; UI pixel-identical to stock ntvdm. **Paint's GDI surface is 67/76 and its USER surface 88/92; PBRUSH.DLL is 16/16.** ⚠ Not yet exercised: the Text tool, the cutout tools, Edit > Paste, printing. ⚠ And one live defect: after mouse drags the Alt-key menu route stops responding. | **~90%** |
| **The full vision** — an `ntvdm` superset on XP-32 | Everything above, plus the host UI, minus the standing DOS defects and M7/M8. | **~60%** |

**Read the north-star number carefully.** The hard *unknowns* are largely behind us — what is
left is mostly known work, but there is a lot of it, and **M6 is one line on the roadmap and
probably the largest single body of work remaining**: 16:16↔flat thunking, USER/GDI object
mapping, and message bridging. *Calling into 16-bit code* was on that list until session 40
and is now a working mechanism (`src/wow/wowcall.h`); *message bridging* was on it until
session 41 and is now `src/wow/wowmsg.h`, with the host's own keyboard on one end of it and
a Win16 window procedure on the other.

⚠ And the standing DOS defects below are small individually but sit in the **"runs but lies"**
class this project treats as the most expensive kind — `MEM.EXE` reporting wrong figures
silently is worth more attention than its size suggests.

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
| **In-guest redirection** | `echo x > file` writes to the screen and leaves the file 0 bytes. Three fixes attempted, all at the wrong end. |
| **No INT 13h / INT 25h / 26h** | No direct disk access. |
| **No TSRs** | AH=31h is handled but residency is explicitly *not* honoured (and says so, rather than pretending). |
| **`MEM.EXE` reports wrong figures silently** | The "runs but lies" class — the most expensive kind. |
| **ZAR** | Needs VBE 2.0 hi-colour + linear framebuffer. |
| **40 of 46 settings** | Stored in the registry, honoured by nothing. `settings_apply()` in `src/host/main.c` is the honest list of what actually works. |

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

   ### ▶ START HERE: [session 49](log/sessions/session-49.md#-resume-here)

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

   * **The regression gate is `82 / 122 / 60 · 0001:229C`**, and it must be run
     with `wowsched.txt` and `wowcall.txt` **moved aside** — `wowlive.bat`
     creates them, and a gate run that leaves them in place measures a guest
     running much further (`238/308/101`) and reads as catastrophic drift.
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

   ### ▶ ⚠ THE GATE IS **`82 / 122 / 60 · 0001:229C`**
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
