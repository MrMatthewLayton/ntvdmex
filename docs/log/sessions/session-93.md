# Session 93 — 2026-10-05: the user's eight-program report, and the overnight run toward 99%

The user tested all eight remaining partial Win16 guests (checks.txt, CRLF) and reported:
Cardfile and Media Player work (TONE.WAV and MIDI) — **done**, with Task List (background
confirmed). Score 95.2 → 96.0%. The rest: Program Manager (empty start, "unknown stack fault"
on New Program Item), Terminal ("entirely messed up UI"), Write (draws the desktop, Backspace
types a square), Sysedit (Save disabled), Recorder (closes when clicked away), Packager
(Import does nothing, same drawing problem). Target set for the night: 99%.

*(Draft — completed at the end of the session.)*

## Fixed (rig-verified, against stock where stock answers)

| What the user saw | Cause | Verified |
|---|---|---|
| Terminal: UI "captures the desktop" | the modal loop called another window's procedure with the DIALOG's hwnd -- Terminal's WM_PAINT went to its own code as the dialog; its client was never painted | Terminal client grey, dialog where stock puts it |
| Sysedit: Save disabled | WM_MDIACTIVATE never reached MDI children (dropped during WM_MDICREATE; not relayed at all) -- Sysedit keeps "the active file" from it | File > Save enabled after typing |
| Write: nothing typed appears | Win32's default WM_ACTIVATE gave the focus back to the frame after Write's own SetFocus(document) | "abc" appears |
| Write: Backspace types a square | every Win32 WM_CHAR was relayed; Win16 only makes one when the program calls TranslateMessage -- Write handles Backspace in WM_KEYDOWN | "abcd"+BS = "abc" (the mark after it is Write's own end-of-document mark) |
| Program Manager dialogs at the screen's top-left | a popup dialog's template position is relative to its OWNER's client area (no DS_ABSALIGN) | (190,257) vs stock (193,286), then frame-corrected |
| Program Item Properties: first keys lost, Tab dead | a dialog window kept the focus itself (DefWindowProc); DefDlgProc passes it to a control | in test |
| Recorder: SetTimer(NULL, 0, ms, proc) refused | windowless timers: Win32 thread timers relayed as Win16 WM_TIMER hwnd 0 + TIMERPROC | in test |

## DOS rows re-measured

- **MEM / MEM /C / MEM /D vs the 6.22 oracle** on the merged tree (runs/s93/mem_vs_622.txt):
  #207's negative row is gone in MEM itself; same device chain, same totals. mem-accuracy
  0.93 → 0.97.
- **SysVars**: s92's device chain / DPB / +45h already verified against all three oracles;
  the row still described them as gaps. 0.85 → 0.95. Score 96.0 → 96.1%.

## Traps

- ⚠ A "square" in Write after Backspace was Write's own END MARK -- check the fresh window
  before calling a glyph an artefact.
- ⚠ Terminal's first-run dialog only appears until a port is saved in the SHARED WIN.INI;
  ours and stock share it, so a run on one host changes what the other shows.
- ⚠ `wcmd` matches the window title exactly: "Object Packager" never reached "Object Packager
  - Package", and both hosts "agreed" because neither got the command.
