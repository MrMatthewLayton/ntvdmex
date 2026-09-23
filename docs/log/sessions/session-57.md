# Session 57 — TASKMAN puts up its task list, and Cancel closes it

> Session 57. Split out of `docs/STATE.md` on 2026-09-23.

> Verbatim from `docs/STATE.md`. Nothing has been edited or corrected,
> including conclusions a later session refuted — same rule as the rest of
> this archive. See [`README.md`](README.md).

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
