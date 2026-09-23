# Session archive

Every working session's notes. This is the project's **history**; it is not where you find
out where things stand — that is [`docs/STATE.md`](../../STATE.md).

**The content is verbatim. Nothing has been edited or corrected**, including conclusions
that a later session refuted. That is deliberate: on this project the refutations have been
worth more than the conclusions, and a note that says *"session 22's cause for each was
wrong"* is only useful if session 22 is still there to be wrong.

## ⚠ Why this directory exists, twice over

This archive was created on 2026-08-26 by splitting `return-ntvdm.md`, a single rolling
handoff file that had grown to 4,000 lines of reverse-chronological narrative — *"a fine
scratchpad and an impossible thing to hand to anyone."*

**It then happened again.** By 2026-09-23 `STATE.md` had itself grown to **5,155 lines**, of
which ~240 were state and ~4,500 were session blocks for s54–s75 nested *inside a list item*
— 156 headings indented three spaces, invisible to every heading-level tool. Sessions 54–75
below were split back out by `tools/docs/split_state.py`.

> **So: when STATE.md starts accumulating session blocks, split them out.** The pull toward
> one rolling file is strong, it has won twice, and the cost is paid by whoever reads next.

| Where to look | For |
|---|---|
| [`docs/STATE.md`](../../STATE.md) | Where the project is **now**, and what to do next |
| [the wiki](https://github.com/MrMatthewLayton/ntvdmex/wiki) | How something works, and why |
| below | What happened on a given day |

## The archive

| Session | Date | Headline |
|---|---|---|
| [75](session-75.md) | 2026-09-22 | Doom's low detail is genuinely broken; `detaillevel 1` cost a day; the zip in the field |
| [74](session-74.md) | 2026-09-17 | Duke3D runs, ZAR's VESA modes render, heaven7 renders, Heretic runs |
| [73](session-73.md) | 2026-09-16 | The share is laid out for release; Win16 comes back; Hexen and Doom run |
| [72](session-72.md) | 2026-09-15 | The by-hand pass; Doom's E1M1 crash is ours; QBasic's drive list |
| [71](session-71.md) | 2026-09-14 | The text-mode application class -- QBasic's three symptoms were six host defects |
| [70](session-70.md) | 2026-09-13 | Lemmings closed for real: the timer restarts per the datasheet, IRQ0 held in service |
| [69](session-69.md) | 2026-09-13 | Lemmings: music fix reverted (it blanked the screen); capture refined |
| [68](session-68.md) | 2026-09-12/13 | The rig was silently stock; Lemmings reaches gameplay by hand |
| [61](session-61.md) | 2026-09-10 | Skyroads perfect: the crystal was on the wrong lock |
| [60](session-60.md) | 2026-09-09 | CPU speed, honestly -- then the Skyroads wobble, root-caused |
| [59](session-59.md) | — | ZAR renders: its attract demo is on screen, in colour |
| [58](session-58.md) | — | 86.9% unchanged, and four real defects were fixed anyway |
| [57](session-57.md) | — | TASKMAN puts up its task list, and Cancel closes it |
| [56](session-56.md) | — | Calc calculates; we never let the CPU fault for us; the trace was the problem |
| [55](session-55.md) | — | Win16 dialogs -- the unlock of the day (USER thunk 0xEF) |
| [54](session-54.md) | — | The clock: a stepped-over call, for the fifth time |
| [50](session-50.md) | — | the third and fourth guests, and a scoreboard that computes itself |
| [49](session-49.md) | — | MS Paint saves a file |
| [48](session-48.md) | — | two allocators, one LDT |
| [47](session-47.md) | — | the enumerator was lying, and it was hiding 40 services |
| [46](session-46.md) | — | MS Paint is a working paint program |
| [45](session-45.md) | — | MS Paint runs, has its menu, and paints |
| [44](session-44.md) | — | Notepad is a usable text editor, and the method changed |
| [43](session-43.md) | — | Notepad from Windows 3.11, with its menu and its icon |
| [42](session-42.md) | — | SYSEDIT.EXE on the Windows XP desktop |
| [41](session-41.md) | — | the message loop turns, on a real keystroke |
| [40](session-40.md) | — | the host CALLS 16-bit code, and SYSEDIT builds its whole MDI window |
| [39](session-39.md) | — | WOWEXEC opens two windows, and krnl386 opens a real Win16 application |
| [38](session-38.md) | — | the `0001:229C` wall is down: WOWEXEC registers a window class |
| [37](session-37.md) | — | GDI.EXE was never rejected; we could not open it |
| [36](session-36.md) | — | seven system modules load, and the wall was our own BIOS data area |
| [35](session-35.md) | 2026-08-28 | the stepped-over call is answering, and `LoadModule` names itself |
| [34](session-34.md) | 2026-08-28 | 2026-08-28 — DPMI exceptions are delivered, and krnl386 tells us what is wrong |
| [33](session-33.md) | 2026-08-28 | 2026-08-28 — the stock oracle answers, and krnl386 loads segments 2 and 3 |
| [32](session-32.md) | 2026-08-27 | 2026-08-27 — krnl386 relocates itself, and we were destroying the chains |
| [31](session-31.md) | 2026-08-27 | 2026-08-27 — the WOW32 interface is pinned, and krnl386 opens a file |
| [30](session-30.md) | 2026-08-26 | 2026-08-26 |
| [29](session-29.md) | 2026-08-26 | 2026-08-26 |
| [28](session-28.md) | 2026-08-26 | 2026-08-26 |
| [27](session-27.md) | 2026-08-26 | 2026-08-26 |
| [26](session-26.md) | 2026-08-25 | 2026-08-25 |
| [25](session-25.md) | 2026-08-25 | 2026-08-25 |
| [24](session-24.md) | 2026-08-24 | 2026-08-24 |
| [23](session-23.md) | 2026-08-24 | 2026-08-24 |
| [22](session-22.md) | 2026-08-24 | 2026-08-24 |
| [21](session-21.md) | 2026-08-24 | 2026-08-24 |
| [20](session-20.md) | 2026-08-24 | 2026-08-24 |
| [19](session-19.md) | 2026-08-23/24 | 2026-08-23/24 |
| [13](session-13.md) | 2026-08-20 | 2026-08-20 |
| [12](session-12.md) | 2026-08-19 | 2026-08-19 |
| [11](session-11.md) | 2026-08-19 | 2026-08-19 |
| [7](session-07.md) | 2026-08-18 | 2026-08-18 |
| [~15–17](standing-reference.md) | — | Standing reference |
| [≤53](session-53-and-older.md) | — | Session 53 and older -- handoff pointers and the WOW/krnl386 reference blocks |
| [2026-06-09](2026-06-09.md) | 2026-06-09 | 2026-06-09 — mode-12h tiered interpreter, the per-pixel wall, INKEY$/XCHG fixes |
| [2026-06-08](2026-06-08.md) | 2026-06-08 | daily log entry |
| [2026-06-07](2026-06-07.md) | 2026-06-07 | daily log entry |
| [2026-06-06](2026-06-06.md) | 2026-06-06 | **Committed the M2.4 spike (`4aa6f44`).** The +252-line uncommitted diff in |
| [2026-06-05](2026-06-05.md) | 2026-06-05 | daily log entry |
| [2026-06-02](2026-06-02.md) | 2026-06-02 | Chose and wired the build toolchain: **CMake + mingw-w64 `i686-w64-mingw32`** cross-compiler, |
| [2026-06-01](2026-06-01.md) | 2026-06-01 | Answered the gating question "is this even possible / is NTVDM replaceable?" → yes |

> **Gaps are real, and dates are only asserted where a source states them.** Sessions 1–6,
> 8–10, 14–18, 51–52 and 62–67 have no surviving block: some were pruned as stale restart
> snapshots at the time, some were folded into a neighbouring session, and the earliest work
> predates this archive. Their record is the commit history, `docs/research/`, and the June
> daily entries above.
