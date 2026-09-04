# Session 50 — the third and fourth guests, and a scoreboard that computes itself

**Branch:** `m9/completeness` · **Epic:** [#128](https://github.com/MrMatthewLayton/ntvdmex/issues/128)

> ★★★★★ **SOLITAIRE AND MINESWEEPER RUN.** Solitaire deals a full card table with
> real card faces and its own menus; Minesweeper lays out from its own defaults
> and **cascades a reveal when you click it**. Until today "WOW works" was a claim
> about two programs the host had been shaped around for eight sessions. It is now
> a claim about four, two of which nobody designed for.

---

## What moved

| | Before | After |
|---|---|---|
| Guests that run | Notepad, Paint | **+ Solitaire, + Minesweeper** |
| Shelf service breadth (measured) | 177 serviced / 137 to do — 56.4% | **192 / 122 — 61.1%** |
| WOW regression gate | `82 / 122 / 60` | **`85 / 113 / 57`** — improvement, arithmetic closes |
| Off-VM batteries | 842 / 842 | 842 / 842 |
| Overall score | 60.8% | **61.7%** |
| — of which WoW16 | 65.7% | **67.7%** |

## The bugs, and three of the five were the same shape again

**★★★ `krnl386 0x7f GetPrivateProfileInt` was unimplemented, and the sentinel `0`
is a legitimate value.** WINMINE.EXE asks it 37 times in one startup — `Height`,
`Width`, `Mines`, `Xpos`, `Ypos` — and `nDefault` is *right there in the argument
block* (`8`, for `Height`). We were throwing it away and answering 0, so
Minesweeper laid itself out with every stored value zero and created its window at
**(-2, -48)**: its caption and menu bar off the top of the screen. Nothing in the
log looked like an error. **Fifth session running that this exact shape has been
the bug.**

**★★★ `LoadBitmap` refused the `BITMAPCOREHEADER`.** The handler accepted only
`biSize == 40` and declined anything else — deliberately, and the refusal was
right at the time, because guessing at a format is worse than declining it. But
every card face in SOL.EXE is the 12-byte Windows 3.0 / OS-2 header, so Solitaire
got 0 for all of them and put up **"Out of memory"**. The two headers differ in
more than length (`bcWidth`/`bcHeight` are *unsigned 16-bit*, and the colour table
is `RGBTRIPLE`, three bytes, not `RGBQUAD`), so this converts field by field
rather than casting.

**★★ `SetTimer` with a real `TIMERPROC`.** Solitaire arms one at 250 ms and reads
the result; refusing it *also* produces "Out of memory", because Win16 timers were
a scarce system-wide resource and that is genuinely how a program of this era
reports failing to get one. The trap was calling the guest's procedure from inside
a Win32 timer callback — the nested run this host has not built. It is not needed:
**Win16 does not call a TIMERPROC from the timer either.** It posts `WM_TIMER` with
the procedure in `lParam`, and **`DispatchMessage`** calls it — a place this host
already calls 16-bit code from, on the guest's own thread, with its own stack. The
faithful implementation and the safe one turned out to be the same one. Confirmed
firing: **42 dispatches into Solitaire's own code in one run.**

**★★ `InvalidateRect` has been invalidating the whole client area since it was
written.** `InvertRect` redefined `IR_ARG_RECT` from 2 to 0, and the preprocessor
takes the last definition before the use — so *both* handlers read offset 0 and
`InvalidateRect` fetched its `lpRect` out of `bErase`, got a junk far pointer, and
fell into the NULL path. It never looked broken because over-invalidating still
repaints correctly; it just repaints everything, every time. The only evidence was
a `warning: 'IR_ARG_RECT' redefined` that had been in the build output all along.
Found because `SetMenu` collided with `SendMessage`'s `SM_ARG_HWND` the same way —
**a prefix here is a namespace, and a collision in it is a silent wrong answer.**

## New services

18 ids: USER `SetTimer`, `KillTimer`, `GetCurrentTime`, `FindWindow`, `FrameRect`,
`DrawText`, `SetDlgItemText`, `GetDlgItemInt`, `CheckRadioButton`,
`CheckDlgButton`, `IsDlgButtonChecked`, `AdjustWindowRect`, `SetMenu`,
`GetLastActivePopup`; GDI `CreateDIBitmap`, `SetDIBitsToDevice`; krnl386
`GetPrivateProfileInt`. Solitaire's USER surface is **48/48** and Minesweeper's
**41/41**; Minesweeper's GDI is **15/15**.

`KillTimer` is not in either import list — it resolves to 16-bit code in USER which
calls *down* to this id, invisible to `neneeds.py`. Implemented anyway, because
arming a timer with no way to disarm it is a leak per game. **`native16` still does
not mean free.**

## Wrong turns, recorded

- **The first Minesweeper click test proved nothing and looked like it passed.** It
  drove an already-running guest and compared before/after: both shots came back
  byte-identical **and already showing a played board**, because someone had played
  it between the launch and the test. ⚠ **The rig is not necessarily unattended, so
  a test that does not create the state it measures is measuring somebody else's.**
  `minetest.bat` now launches the guest itself and deletes `winmine.ini` first so
  the window lands at the default position every run.
- Chasing Minesweeper's missing menu through `0x13a` — that call is in **WOWEXEC's**
  task (`0x048f`), not Minesweeper's (`0x0baf`). Filter by task before reading a
  call as the guest's.

## Open

1. **Minesweeper has no menu bar.** Its class names none (`(no menu named)` — where
   Solitaire's says `MENU=#0001`), it never calls `LoadMenu`, and it asks
   `CheckMenuItem`/`EnableMenuItem` with `hMenu = 0` *before* `SetMenu(hwnd, 0)`.
   So it gets its menu somewhere this host has not found yet. Without it there is
   no new game and no difficulty.
2. **Solitaire is not yet driven interactively** — it deals correctly and its timer
   runs, but no card has been dragged. Marked `partial`, not `done`.
3. `LineDDA` (GDI `0x064`) is Solitaire's last unserviced import; it takes a 16-bit
   callback per point, so it needs `wowcall`, not a pass-through.
4. The **Alt-menu-after-drag** defect from session 49 is untouched.
