# Session 88 (2026-10-02/03) — Win16 gaps, and one tray icon for everything

Picked up after session 87's fifteen (all user-confirmed and closed at the start of this
session). User's next ask: the Win16 API gaps — dead menu items, X buttons that don't
close, Recorder blank, Sound Recorder half-working, Clock flicker. Then a design change:
one tray icon for every running program.

## Where it stands (handoff)
- **Branch** `m9/completeness`, HEAD = this note's commit, on top of `5d099c8`.
  **Pushed up to `62e0194` only** — the 5 code commits of this session are LOCAL; push
  only when the user asks.
- **Rig `bin\`**: `ntvdmhost.exe` = **`3c67faee`**, `ntvdmex.exe` (the manager) =
  **`6a5666e0`**. Both built from `5d099c8`. Last gated-and-pushed build: `56d212e7`
  (`runs/s87_final/B_new.exe`); before that `978589de` (`runs/s87_final/A_978589de.exe`).
- **User-confirmed this session:** the tray manager (#281, closed) — one icon, per-program
  Show / Close Program, global Settings / About / Exit All, no machine window for Win16,
  icon gone after the last program.
- **Owed by hand (in `checks.txt` on the share, `report.txt` empty):** Clock (flicker,
  date) and the X on Clock, Charmap, Minesweeper, Solitaire, Task List — all fixed and
  measured headless, not yet seen by the user.
- **Not done this session:** a full regression gate (Skyroads / Doom+ZAR audio / Win16
  shelf A/B) on the s88 builds. The Win16 shelf was run (below); the DOS/audio gate was
  not — the changes are Win16-, tray- and paint-path only, but run `runs/s85/gate.sh`
  (or `runs/s87_final/final.sh`'s gate part) before the next zip.

## What landed (all `#162` unless noted)
| Commit | What | Evidence |
|---|---|---|
| `6c40e63` | **BeginPaint validates** the real window and absorbs WM_PAINT16s already queued for it; **GetDateTime** packing read off krnl386's reader | Clock: 36,609 paints → 2; date `?5/00/2074` → `02/10/2026` |
| `b13c08b` | **DefWindowProc forwards again**: fill USER's forward table at `NotifyWow(4)`; `(0x217, 6, 0x12ea)` anchors USER | DefWindowProc had reached the host 0× in 16 apps; Clock closes |
| `a79cf8a` | **DefDlgProc calls the DLGPROC first** (callback + `WOWCALL_ACT_DLGDEFAULT`); modal loop applies the WM_CLOSE default; **GetWindowText** (USER 0x24) | Charmap, Task List close; Sound Recorder labels appear |
| `9b69fe1` | **NTVDMEX manager** `ntvdmex.exe` — one tray icon for every DOS + Win16 program (#281) | `scripts/bm/mgrtest.bat`: manager appears, shared, exits 15 s after last |
| `5d099c8` | Win16 hosts **never** show their machine window; no per-program Settings (#281) | user-confirmed |

Shelf X-close (16 apps, `scripts/w16shelf.sh`): **9 → 14**. Remaining: Terminal
(expected — its first-run modal "Default Serial Port" dialog is up; see #279) and MPLAYER
(never opens a window).

## Root causes worth remembering
- **USER's DefWindowProc gate** (`seg1:0x013c`): forwards to WOW32 0x6b only if the
  message's bit is set in a bitmap at USER `cs:0x00d0` (max at `cs:[0x137]`), **both zero
  in the file**; WOW32 fills them when USER's init calls `NotifyWow(wKind 4, &block)`
  (`seg1:0x3cf9`; block +0x0e max ptr, +0x12 bitmap ptr, +0x16 byte count). We stepped
  that over because USER was not yet anchored. **Only WM_CLOSE is enabled** — our 0x6b
  handler passes parameters raw; pointer/handle messages join as they are translated.
  `NotifyWow(4)` returns **0** deliberately: non-zero makes USER hot-patch itself
  (`seg1:0x6947`), which has never run here.
- **The s82 reading was wrong**: USER's filter `seg1:0x38fe` forwards EVERYTHING (default
  arm `0x3b5d`); its list is "which messages need lParam conversion". Memory note
  `wow-posted-vs-sent-messages` corrected.
- **Clock's paint storm**: it calls `InvalidateRect(hwnd,NULL,TRUE)` inside WM_PAINT, and
  the exec loop pumps the real windows between guest calls (`wowwin_pump`), so the real
  WM_PAINT is relayed (erasing) before the guest's BeginPaint.
- **Dialog-as-main-window** (Charmap): class proc = DefDlgProc, real DLGPROC passed to
  CreateDialog — DefDlgProc is the only route to the program. WM_DESTROY is dispatched
  after the record is freed; the last freed window's DLGPROC is kept (`g_wu_gone`).

## The manager (#281) — design, as built
- `ntvdmex.exe` (the CMake target that used to build the retired milestone-0 preview;
  `src/main.c`, `console.c`, `console.h` removed). Source `src/manager/manager.c`;
  protocol `src/host/mgrproto.h`.
- Hosts re-announce every 2 s from a `THREAD_PRIORITY_LOWEST` thread (WM_COPYDATA:
  pid, kind, name, command hwnd, show hwnd); the manager upserts by pid, drops on process
  handle signal, exits 4 s after empty (15 s if never spoken to). Commands back via the
  registered message `NTVDMEX_ManagerCommand`.
- Without `ntvdmex.exe` beside the host, behaviour is the old one.
- NT allows one V86 machine per process ⇒ DOS programs stay one process each. Sharing
  one process between Win16 programs is **#280, deferred** until a program needs it
  (Task List seeing others, OLE/DDE between Write and Paintbrush).
- `package.sh` / `bmstage.sh` ship/deploy it and refuse a stale M0 `ntvdmex.exe`.

## Lessons
- **Sessions ended four more times.** Detached (`nohup … & disown`) rig jobs survive;
  agents and in-session background shells do not. Commit after every step.
- ⛔ **A harness run force-kills every host** (`w16close.bat` / `mgrtest.bat` start with
  `taskkill /f /im ntvdmhost.exe`). One run this session closed the user's open programs.
  Run `scripts/bm/hostcheck.bat` (writes `debug\out\procs_now.txt`) FIRST.
- `controld exec` mangles nested quotes — wrap anything non-trivial in a `.bat`.
- `w16close.bat` now saves a desktop screenshot before the X (`debug\out\w16close_<app>.bmp`);
  "does it draw right" is not in the log.

## Issues filed / updated
#278 Sound Recorder (owner-drawn buttons show raw text; MMSYSTEM `LoadLibraryEx32W` /
generic thunks unimplemented) · #279 modal dialog doesn't disable its owner · #280 shared
Win16 host (deferred) · #281 manager (closed) · #220 closed (superseded) · #162 progress
comment. **Next by the user's list:** dead menu items (#216 — USER `0x16C`
LookupIconIdFromDirectoryEx + `NotifyWow` for cursor/icon kinds), then #278.
