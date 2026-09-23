# Session 56 — Calc calculates; we never let the CPU fault for us; the trace was the problem

> Session 56. Split out of `docs/STATE.md` on 2026-09-23.

> Verbatim from `docs/STATE.md`. Nothing has been edited or corrected,
> including conclusions a later session refuted — same rule as the rest of
> this archive. See [`README.md`](README.md).

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
