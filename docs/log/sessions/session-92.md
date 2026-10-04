# Session 92 — 2026-10-04: painting, the run queue, and the board toward 98% (unattended)

The user confirmed the Win16 Task List lists tasks and switches to them, reported two
painting gaps (Packager leaves trails on a live resize; the Task List draws no background)
and asked for an unattended run toward **~98%**, closing as many of the 83 open issues as
possible. Every change below was verified on the rig, against stock NTVDM wherever stock
can answer (`scripts/w16stockdrive.sh` and `tools/wintest/gate.sh` are new for that).

**Result: score 95.1 → 95.2% (re-run, `tools/score/score.py`).** Branch `m9/completeness`, last commit pushed. The rest of the way to ~98% is mostly the WOW `guests` row (w15, 71%: 9 done + 9 partial of 19), which only by-hand confirmations move -- see "For the user" below.

## Closed

| # | What | Verified by |
|---|---|---|
| #289 | Packager: WM_PAINT SENT inside the move/size loop + one full repaint at WM_EXITSIZEMOVE | mid-drag screenshot (`rigshot cornerhold`); "white headers" = stock's own pixels |
| #284 | Cardfile: one nested WM_SIZE sent (the card hides its scroll bars inside WM_SIZE) | launch + resize = stock (`w16stockdrive`) |
| #287 | Calc: uncovered area stays grey | `rigshot cover`, = stock |
| #282 | Calc: LCD box position | pixel scan, (45,9)-(242,42) both |
| #314 | Media Player: LoadBitmap(NULL, OBM_*); WM_CTLCOLOR default brush; a raw INT in a nested run (task killed) | = stock; TONE.WAV loads |
| #295 | EnumMetaFile / PlayMetaFileRecord | `w_mfenum` 33/33 = stock |
| #279 #286 #277 | already fixed earlier, verified again | see each issue |
| #306 | Calc → Help opens WinHelp = stock; closing both ends the host | w16stockdrive window lists; gate13 "No tasks running" |
| #210 | LFN API, p_lfn 36/37 = stock (71A7h BH an intended divergence) | dospair, gate12 |
| #244 #274 | keyboard BIOS (INT 15h 4Fh, Pause, Alt+keypad, ring bounds) | p_kbd3 = oracles (INT 05h → #317) |
| #265 #266 | INT 33h extras; INT 10h after #252 | p_mouse3, p_vid266 = oracles |
| (comment) #264 #136 #285 | graphics cursor / settings wired / Notepad Help works | need by-eye or Search re-test |
| new #318 | WinHelp paints no background while its box is up; Calc stays above it | stock screenshot |

## Found and fixed along the way

- **Task List background**: the modal loop gave DefDlgProc's default only to WM_CLOSE;
  WM_PAINT/WM_ERASEBKGND answered FALSE were never erased.
- **Program Manager's X left a windowless host**: DefFrameProc's WM_CLOSE went to Win32,
  which destroyed only the real window. Now ours (WM_DESTROY reaches the program).
- **WM_DROPFILES** (#305 M12): a Win16 HDROP is a real krnl386 global block (SHELL.DLL's
  own DragQueryPoint/DragFinish read and free it); Notepad opens a dropped file = stock.
- **A raw `INT nn` reflected as #GP inside a nested run** was serviced and then resumed on
  our fault site, whose `C4 C4 57` the WOW dispatcher stepped over (0x57 is also a WOW id):
  krnl386 killed the task. Media Player's WM_INITDIALOG hit it.

## WinHelp from Calc and Notepad (#306 / #285) -- five layers, one per run

1. **Launch-first (F).** WinExec must not return before the new task has run to its
   first yield; USER's WinHelp() looks for `MS_WINHELP` straight away. The parent is
   parked at its next BOP (runnable, at its callback depth) and the child runs.
2. **NotifyWow kind 6** = "find the window of this class" (USER seg1:0x6d97); stepped
   over it answered 0 → "Not enough memory available". FindWindow by class must also
   try our `NTVDMEX16.` prefix.
3. **One ring, per-task takes.** Calc's GetMessage took the message WinHelp posted to
   its own window; DispatchMessage ran WinHelp's procedure on Calc's stack (SS≠DS, a
   near pointer to a local read garbage, #GP). Windows now record their creating
   task; GetMessage/PeekMessage take only the caller's; PostQuitMessage is per task.
   A task idle in an empty GetMessage is parked **waiting for messages** and becomes
   runnable when one arrives for it (a click on its window wakes it out of another
   task's wait). Parked at its own top level (no host frame held) it may resume at
   any callback depth and is re-based there.
4. **EnumTaskWindows ignored hTask** (and GetWindowTask answered the current task):
   WinHelp, inside Calc's SendMessage, enumerated "its own" windows, got Notepad's
   main window and sent it a WM_COMMAND -- "You have not entered any text to be saved".
5. **Resources came from the command-line program.** WinHelp's menu #0fa0 was looked
   for in CALC.EXE. The running task's file is now read from krnl386 (TDB+0x1E
   hModule → NE +0x0A → OFSTRUCT path), and the image cache holds several files.

Calc → Help now = stock (`w16stockdrive`): WinHelp's window plus its own
"Cannot open Help file." box (there is no CALC.HLP; XP ships calc.chm).

## Traps

- ⛔ **An agent's reading of a call site is a hypothesis.** #298's research said KERNEL
  0xc6's `jne` is the free path (true) and that 0 therefore leaks (false): answering 1
  killed every Win16 launch ("Missing 16-bit system module: KEYBOARD.DRV"). The shelf gate
  caught it before commit.
- A probe can be wrong in a way only stock notices: `w_mfenum`'s callback popped 14 bytes
  instead of 16; our host restores its own frame and never noticed, stock crashed.

## Gates on the final tree

- Combined (Q_77f67673 = batch 1 + WinHelp + batch 2): Skyroads n8 0x14/0x15 max_ms 0xb/0xa
  (within A..I's spread on this headless harness), Doom 50/60 s sounding ×2, **ZAR 66/70 s
  sounding ×2** (no silent start), Win16 probes all = stock (w_cwd handle row pre-existing),
  off-VM battery 3271/3271.
- S_1984a094 (head): Win16 shelf 15/16 (Terminal's close, as every build), Calc/Notepad Help
  = stock, closing WinHelp + Calc ends the host.
- The DPMI branch (#267/#268, `worktree-agent-a6c5e4ef575f343b7`) stays OUT: with it ZAR was
  silent 1 run in 2 and `cb.es_di_is_rmcs` is unexplained.

## For the user

- By-hand confirmations that move the score: TASKMAN (background now drawn), PACKAGER (live
  resize clean), CARDFILE, MPLAYER, WRITE, SYSEDIT, RECORDER, PROGMAN, TERMINAL; and Calc/
  Notepad → Help (WinHelp's "Cannot open Help file." is stock's own answer -- no .HLP files).
- #311 still needs your decision. The `linear` MCP server needs authorising (not possible
  from an unattended session).
