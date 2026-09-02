# Session 39 — WOWEXEC opens two windows, and krnl386 opens a real Win16 application

- **Branch:** `m9/completeness`
- **Issue:** [#128](https://github.com/MrMatthewLayton/ntvdmex/issues/128)
- **Predecessor:** [session 38](session-38.md) — the `0001:229C` wall came down and
  `RegisterClass "WOWExecClass"` was serviced.

---

## ★★★★ THE HEADLINE

`CreateWindow` is answered, and WOWEXEC runs a long way past it. In one run it now:

1. registers `WOWExecClass` and **creates a window from it** — `"WOWExec"`, style
   `0x02CF0000`;
2. registers a **second** class, `WOWFaxClass`, and creates a **second** window;
3. runs on through `SYSTEM.INI`'s `[drivers]` section, looking for `MMSYSTEM.DLL` and
   `WFWNET.DRV`;
4. and arrives at **its message loop** — `wowexec seg1:0x0798` — where it now spins.

The ~155× task-relaunch loop of session 38 is **gone**: `RegisterClass` and
`CreateWindow` each appear exactly **twice** in the whole run, which is once per class
and once per window, not once per relaunch. What fills the log now is the message pump
turning over with nothing to pump.

**Nothing has drawn a pixel.** A window here is an object, not an image — see below.

---

## ★★★ THE INSTRUMENT THAT MADE THIS QUICK: A CALL SITE CAN NAME ITSELF

`nedis.py` prints every imported call in an NE as

```
08b2  9affff0000    lcall 0, 0xffff
```

which is honest — that really is what is in the file — and useless. An unlinked NE
stores a **chain** in the operand words, and `0:0xffff` is the *end* of one, not a
target. So the disassembly of a program that is almost entirely API calls names none of
them, and the only method anyone had was to read the pushes and recognise the shape.
**That is inference, and this project has twice written up an inferred name that was
wrong.**

The answer was in the file the whole time. Each relocation record carries
*(module, ordinal)*, and the chain it heads lists **every site that takes that import**.
Walk the chains once and every `lcall` in the module has a name.

▶ `tools/ne/neimports.py` — new. It named the entire frontier in one command:

```
0x08b2  USER.41 CREATEWINDOW
0x079c  KERNEL.262 WOWWAITFORMSGANDEVENT
0x07b2  USER.109 PEEKMESSAGE
0x07cd  USER.113 TRANSLATEMESSAGE
0x07d7  USER.114 DISPATCHMESSAGE
```

⚠ **The relocation points at the OPERAND, not the instruction** — a `9a` far call at
`0x08b2` is relocated at `0x08b3` — so a reader coming from a disassembly and a table
keyed on the relocation are off by one from each other. The tool prints the *call*
address and accepts either on lookup, because being strict there makes it useless at
exactly the moment it is needed.

★ **And it is a second, independent method, not a replacement for the first.**
`seg1:0x08b2` resolves to `CREATEWINDOW` by relocation; the WOW32 stub it reaches
declares id `0x29` and 30 argument bytes by its own `push`es; `wow-user-surface.md`
says id `0x29` is `CREATEWINDOW`. Three routes, one answer. That is why the layout below
is trusted rather than assumed.

---

## ★★★ CreateWindow — USER id `0x29`, 30 argument bytes

### The argument block is REVERSED, and the parameter order does not tell you that

The block base (`bp+16`) is the **lowest** address, so it holds the **last** word
pushed. Pascal pushes left to right, so the parameter list appears backwards — and a
DWORD's **high** word sits at the **lower** offset, because it is pushed first. Reading
"lpClassName is the first parameter, so it is at offset 0" gets every field wrong.

WOWEXEC's fifteen pushes ahead of `seg1:0x08b2` come to exactly the `0x1e` bytes the
USER stub declares:

| offset | pushed | parameter |
|---|---|---|
| +28 / +26 | `push ds` / `push 0xae` | `lpClassName` |
| +24 / +22 | `push [0x16]` / `push [0x14]` | `lpWindowName` |
| +20 / +18 | `push 0x2cf` / `push 0` | `dwStyle` |
| +16 / +14 / +12 / +10 | four × `push 0x8000` | `x`, `y`, `nWidth`, `nHeight` |
| +8 / +6 / +4 | `push 0` / `push 0` / `push [0x206]` | `hWndParent`, `hMenu`, `hInstance` |
| +2 / +0 | `push 0` / `push 0` | `lpParam` |

### ★ And the DATA cross-validates the layout, four times over

This is the part that makes it a measurement rather than a reading:

- `+26/+28` → `ds:0x00ae` decodes to **`"WOWExecClass"`** — the class WOWEXEC has just
  registered. (It is a *second copy* of the literal; the `WNDCLASS` used `ds:0x0082`.
  Both decode to the same name, which is why the mismatch between the two pointers is
  not a defect.)
- `+18/+20` → **`0x02CF0000`** = `WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN`. Read the other
  way round it is `0x000002CF`, which is not a window style at all.
- `+10..+16` → four copies of **`0x8000` = `CW_USEDEFAULT`**, exactly where `x/y/w/h` are.
- `+4` → **`[0x206]`**, the same word WOWEXEC stored into `WNDCLASS.hInstance` at
  `seg1:0x0803`.

A wrong offset assignment produces **none** of those agreements.

### The shape: an object, deliberately not a real HWND

Session 38's handoff asked for this decision before any code, and the answer it
suggested is the one taken. A real host window would drag in a real message queue, a
real `WM_CREATE` and the 16:16 thunk back into the class's window procedure — all of
which would be half-built, and **lying**, by the time the first `CreateWindow` returned.

What the guest can actually observe at this point is a handle that is non-zero, stable
and answers questions about itself. So that is exactly what `g_wu_win[]` holds: the
class, the rectangle, the style, the text. WOWEXEC's own window is the WOW shell's and
is normally hidden, so for this guest there is nothing to draw in any case.

⇒ When windows get pixels, that struct is the thing that grows a host window handle.
Nothing above it has to move.

⚠ **The handle space is synthetic and says so.** A real Win16 `HWND` is an offset into
USER's local heap; ours is a counter (`0x0100`, step `0x20`). Nothing may infer memory
from it.

⚠ **`CW_USEDEFAULT` is resolved once, here**, because every later reader wants numbers.
The size it resolves to (`WOWUSER_DESK_CX/CY`) is **nominal** — there is no desktop, so
there is no honest answer — and it is a named constant so that the day there is one, it
is a one-line change rather than a hunt.

### Two things done because they are cheap now and expensive later

- **An unregistered class FAILS**, as it does on real Windows. A host that made a window
  for any name at all would hide a broken `RegisterClass` behind a working
  `CreateWindow` — the "runs but lies" class this project treats as the most expensive.
- **`lpClassName` may be an ATOM** — a null selector with the atom in the offset. That is
  how a program written around `RegisterClass`'s own return value passes it, so it is
  handled now rather than found later as a wrong answer. WOWEXEC happens to pass a
  string.

---

## ▶ THE NEW FRONTIER: THE MESSAGE LOOP

`wowexec seg1:0x0798`, with every call named from the import table:

```
0798  push [0x10]              ; the hWnd we returned -- 0x0100
079c  lcall  KERNEL.262 WOWWAITFORMSGANDEVENT
07a1  or ax,ax / jne 0x0798    ; non-zero -> wait again
07a5  lea ax,[bp-0x14] / push ss / push ax / push 0 / push 0 / push 0 / push 1
07b2  lcall  USER.109 PEEKMESSAGE          ; PeekMessage(&msg, 0, 0, 0, PM_REMOVE)
07b7  or ax,ax / je 0x0798     ; no message -> back to the wait
07bb  cmp [bp-0x12],0x401      ; msg.message
07c2  cmp [bp-0x12],0x12       ; WM_QUIT
07cd  lcall  USER.113 TRANSLATEMESSAGE
07d7  lcall  USER.114 DISPATCHMESSAGE
07dc  jmp 0x0798
```

Both `WowWaitForMsgAndEvent` (krnl386 id `0x83`, and the log confirms it is handed
**our `hwnd` 0x0100**) and `PeekMessage` (USER id `0x6d`, 12 argument bytes — matching
`wow-user-surface.md`) are unimplemented, so both answer the harness sentinel `0`: the
wait returns instantly and the peek says "no message", forever. 364,966 WOW32 calls in
one run, all in task `0x03b7`, all that pair.

**This is the honest shape of the wall, not a defect.** There is no input, no timer and
no queue, so there is genuinely nothing to return. The next piece of work is a message
queue with something in it.

### What to do next, in order

1. **A message queue.** `PeekMessage`/`GetMessage` need a source. The smallest honest
   first cut is a queue the host can post to, seeded with what a window's creation
   already implies (`WM_CREATE`, `WM_SIZE`, `WM_PAINT`) — but note that dispatching any
   of them means `DispatchMessage` must **call back into the 16-bit window procedure**,
   which `g_wu_win[].wndproc` has been holding since `CreateWindow` and which nothing
   has ever done. That callback is the real next wall, and it is bigger than a table
   entry.
2. **`WowWaitForMsgAndEvent` must be able to block**, or the pump is a spin regardless.
   It is krnl386 id `0x83` and it is handed the `hWnd`.
3. **`ShowWindow` (`0x2a`) / `UpdateWindow` (`0x7c`)** are cheap against `g_wu_win[]`
   and are what a normal program calls next. WOWEXEC does not, so they are not on the
   critical path for *this* guest — they will be for Notepad.

---

## ⚠ A DEFECT SEEN IN PASSING, NOT CHASED: THE MODULE SEARCH PATH

Past the windows, krnl386 goes looking for the `[drivers]` entries and asks for

```
INT21h AH=3d open "C:\Documents and Settings\Matthew\MMSYSTEM.DLL" -> FAILED gle=2
```

It is resolving a bare module name against the **current directory**, which our
`GetCurrentDirectory` reports as `Documents and Settings\Matthew`. Windows searches the
Windows and system directories for this. Filed here rather than fixed because it is
downstream of the frontier and fixing it would only load more drivers into a VDM that
cannot yet show a window — but it is a real wrong answer and it will bite the moment a
real application looks for a DLL.

---

## ⚠ ONE CORRECTION TO SESSION 38'S HANDOFF

It said `CreateWindow` returning 0 made WOWEXEC "take its error path at
`wowexec:0x0849`". `0x0849` is the failure path of the call at `0x0840`, which is
`RegisterClass` — and the string it pushes, `ds:0x8f`, decodes to
**`"WOWEXEC: RegisterClass failed\n"`**, which settles it outright. `CreateWindow`'s
failure path is `0x08bc je 0x0852`, which returns 0 from the init function without a
message. It changes nothing that was done, but the two are different paths and a reader
breakpointing `0x0849` to watch `CreateWindow` fail would have waited forever.

---

## The baseline, re-confirmed

The scheduler is still **opt-in** (`wowsched.txt`), and the committed no-scheduler
behaviour must not move. Re-run after the change, twice, and compared against session
38's archived run:

| | session 38 | session 39 |
|---|---|---|
| WOW32 calls | 265 | 265 |
| serviced | 42 | 42 |
| task timeline | 9 / 222 / 27 | 9 / 222 / 27 |
| `0001:229C` fault | present | present |

Different `md5`s, same size, same numbers — two genuinely distinct runs, not one stale
artefact read twice. The baseline has not moved.

---

## ★★★★ PART 2: THE MESSAGE LOOP WAS THE WRONG FRONTIER

The plan above said "build a message queue". The **run** said something else, and it
said it in one line. WOWEXEC asks `WowGetNextVDMCommand` (WOW32 `0x70`) exactly **once**,
we answered the harness sentinel `0`, and the very next call in the log is:

```
FUNC=0x84 WowMsgBox
  ★ arg[2] = "Can't run 16-bit Windows program"
  ★ arg[4] = "Insufficient memory to run this application. Quit one or more
              Windows applications and then try again."
```

**`WowGetNextVDMCommand` is "which 16-bit program do I run?"** — and the program it
wanted is the one this VDM was launched for. `wowrun.bat` writes
`C:\WINDOWS\SYSTEM32\SYSEDIT.EXE` into `target.txt`; the host had it in `progpath` the
whole time and never offered it. The message-loop spin is simply **what WOWEXEC does
after it has given up**.

*A histogram of one run answered a question that a plan had got wrong.*

### ★ AND `0` IS A HARD ERROR, NOT "NOTHING TO DO"

The caller distinguishes them, and we were sending the wrong one:

| answer | what WOWEXEC does |
|---|---|
| `ret == 0` | error box — `seg1:0x0bf8` |
| `ret != 0`, `cbCmdLine == 0` | **quiet** cleanup — `seg1:0x0c1a / je 0x0c05` |

So the sentinel was making WOWEXEC report a failure that had not happened. "No command"
is `1` with a zero length, and it is silent.

### The structure, off WOWEXEC's own frame (`seg1:0x0b20`)

0x20 bytes at `ss:bp-0x34a`: three 0x10d-byte buffers, a `GlobalAlloc`'d environment,
their lengths, a 0-based drive, and `nCmdShow`. Every field is either written by the
caller before the call or read by it after — none is inferred. Full table in
[`wow32.h`](../../../src/wow/wow32.h).

★ **`+0x04` is the module name, and that is measured.** The success path reaches
`seg1:0x01c0`, which pushes that far pointer and a `LOADPARMS` block into
`KERNEL.LoadModule` — named from the relocation chain, not from the shape of the call.
`wEnvSeg` comes from `+0x0a` and `nCmdShow` from `+0x1e`, which is what identifies the
rest of the fields.

⚠⚠ **The command line is a Pascal tail and the `-2` is the whole puzzle.**
`seg1:0x0173` does `lstrlen(lpCmdLine)`, **subtracts 2**, stores that as the count byte
of the DOS command tail, then `lstrcpy`s our buffer to the byte *after* it. So the
delivered string must be exactly two bytes longer than the tail text it represents, and
the only shape that works in every case is **`<tail text> CR LF`**. With no arguments
we deliver `"\r\n"`: `lstrlen` 2, count byte 0, tail `<0><CR><LF>` — correct. Deliver an
empty string instead and `0 - 2` makes the count byte **0xFE**, and the program reads
254 bytes of somebody's stack as its arguments.

⚠ **Deliver once.** The caller loops on this while `[0x18]` is set (`seg1:0x0791`), so a
host that answered every time would relaunch the program forever.

---

## ★★★★ PART 3: AND THEN THE PATCHER CORRUPTED krnl386 — THE DOOM BUG, AGAIN

With the launch answered, the run died differently:

> `WOWEXEC caused a General Protection Fault in module KRNL386.EXE at 0001:2053.`

The fatal address **moved** from `0001:229C`, which is progress, and the new one was
ours. Found by the method that found Doom's, in three steps and no runs:

1. **Diff the bytes there against the file on disk.** Memory `c4 50 0b c9 …`, file
   `75 50 0b c9 …`. **Exactly one byte.**
2. **Rule out a legitimate difference.** Walk krnl386's own relocation records —
   including chains — over `0x2040..0x2060`: **none**. So it was a *write*.
3. **Ask the log who wrote it.** `DPMI: code region 0x0001dd00..0x000295bf -> patched
   0000000c INT sites … at +0x000013a3 +0x00002052 …` — **the offset was already in the
   log**, in a line this session had read once and dismissed.

krnl386 seg1 at `0x2051`:

```
2051  3a cd     cmp cl,ch
2053  75 50     jne +0x50
```

The `cd 75` spanning them is not an instruction. We turned it into `c4 c4`, so the `jne`
became `les dx,[bx+si+0x0b]` — the exact fault at the exact address.

### ★★★ The rule was right for its premises. The premises changed in session 34.

`x86len.h` rejected a candidate only when it could name the owning instruction **and
that owner was a relative branch**, because refusing a *real* site left a raw `CD nn` in
protected mode, and that was fatal too — both halves measured (Doom's
`R_InitTextureMapping` death; DOS/4GW's version check dying 54,000 lines earlier).
"Reject anything a confirmed instruction covers" had been tried and reverted for exactly
that reason.

**Session 34 killed the second half.** A `#GP` whose error code has the IDT bit set *is*
that interrupt: the host takes the vector from the error code, confirms it against the
two bytes at the faulting address, services it, and patches the site — with **no
heuristic at all**, because the CPU has just executed those bytes *as* an interrupt.
`main.c`'s own comment already said it is *"strictly better than a scan"*.

⇒ The costs have **inverted**:

| | cost, today |
|---|---|
| false accept | silent code corruption, fatal, hard to find |
| false reject | one extra `#GP`, serviced, then patched correctly and for good |

So the rule follows the premise: **when in doubt, reject.** An owner is enough. Its
*class* never could have carried the separation anyway — krnl386's owner is a `cmp`
(`3a cd`) and DOS/4GW's is a `xor` (`30 cd`), structurally identical byte-pair forms.

The test keeps **both** fixtures: the DOS/4GW assertion is inverted on purpose with the
reason attached, and krnl386's real bytes are added, so the regression is pinned from
both sides.

### Measured

| | before | after |
|---|---|---|
| `0001:2053` GP | present, fatal | **gone** (0 occurrences, no error box) |
| krnl386 seg1 rejects | 0 / 8 | 1 / 16 |
| krnl386 seg1 patched | 2 / 12 | 1 / 4 |
| baseline (scheduler off) | 265 / 42 / `9·222·27` / `229C` | **identical** |

### ★★★★ AND THEN DOOM WAS RE-MEASURED — IT DOES NOT REGRESS IT, IT **FIXES** IT

The section above originally ended "⚠ NOT re-measured: Doom and the DOS extenders", on
the standing rule that *a fix measured on one guest is a fix for none*. It has now been
measured, and the result inverts the caveat.

**First, two runs that turned out to prove nothing.** `dpmitest.com` passes every one of
its five own checkpoints (`0300 simulate-real-mode-int OK!`, `0301 real-mode far-call
OK (sentinel BEEF)!`, `0303 real-mode callback OK`, the nested `INT 31h`), and
`pm32flat.com` completes. Both are worth having — the DPMI host still works — but
**neither is a regression check for this change**, because neither logs a single
`code region … -> patched` line: they never declare a code region, so
`dpmi_patch_code_region` never runs and the changed decision is never taken. Reporting
those two as "DPMI: green" would have been precisely the instrument-that-lies shape this
project keeps falling into.

**Doom does exercise it** — 19 code regions, 196 rejected byte pairs — so Doom is the
measurement. A/B on the single file, everything else identical, `result_doom.log`
deleted before each run and the deployed binary checksummed:

| | `x86len.h` at `1c0d215` | `x86len.h` at HEAD |
|---|---|---|
| log | **268 MB** (the runaway cap) | **3.5 MB** |
| Doom startup stages reached | **none** | **all eleven**, `V_Init` → `ST_Init` |
| code regions scanned | 2 | 19 |
| sites rejected | 16 | 196 |
| raw INTs serviced from `#GP` | 0 | 2 |
| end state | `#GP` loop, `ds=0000{NO DESCRIPTOR}` | clean, `STAGE2: complete` |

Reproduced: the post-fix run was repeated after restoring the file and rebuilt from
scratch, giving 3,506,020 bytes against the first run's 3,508,409 — same nineteen
regions, same 196 rejects, same two serviced raw INTs, same eleven stages.

★★★ **And the prediction landed on the exact address it was made about.** The argument
for the change was that DOS/4GW's version check — the *one* real site the old rule was
shaped around keeping — would now be rejected, left raw, fault once, and be serviced
from the `#GP`. That is what the log says, at the address `x86len.h`'s own comment
names:

```
EXC: #GP(IDT) is a RAW INT 0x21 at 0x0097:0x00002c65 -- servicing + patching
```

`0x2c65` is DOS/4GW's `mov ah,30h / int 21h`. The mechanism the argument depended on is
observed doing exactly the job it was claimed to do.

⇒ **The flagged risk is closed, and it was pointing the wrong way.** Doom was broken
*before* this change and completes its startup after it — so the old rule was costing
the guest it was written to protect, and nobody had re-measured. (⚠ "Completes its
startup" is not "playable": per the headless-rig rule, feel and picture need a human at
the box. What is measured here is that it initialises fully and does not fault.)

⚠ One thing still unclosed: the `#GP(IDT)` arm guards on a non-zero selector base, so a
**base-0 code region would not be covered**. Such regions are not scanned as a range
either, so nothing is made worse — but that is the gap to close if a guest is ever seen
dying on a raw INT.

---

## ★★★ WHERE IT ACTUALLY GETS TO NOW

With both fixes, in one run:

```
-> SERVICED, returned 0x00000001 -- LAUNCH [C:\WINDOWS\SYSTEM32\SYSEDIT.EXE]
...
INT21h AH=43 attributes "C:\WINDOWS\SYSTEM32\SYSEDIT.EXE" -> AX=0x20 CF=0
INT21h AH=3d open       "C:\WINDOWS\SYSTEM32\SYSEDIT.EXE" al=0x80 -> AX=0x06
INT21h AH=3F read 0x40 h=6 -> first= 4d 5a ae 01 03 00 00 00      ★ "MZ"
```

**krnl386 opens a real Win16 application and reads its header.** It then seeks to
`0x400`, and the load does not complete: the last call before the verdict is krnl386
id **`0x82`** (4 argument bytes, one far pointer, from `seg1:0x53c0`), unimplemented,
after which `WowFailedExec` and the same *"Insufficient memory"* box. No third task is
ever created.

⚠ **A defect seen in passing, still not chased** (it is on this path now): krnl386
resolves bare module names against the **current directory** —
`"C:\Documents and Settings\Matthew\MMSYSTEM.DLL"` — instead of the Windows and system
directories. `SYSEDIT.EXE` was given as a full path, so it is unaffected; the
`[drivers]` modules are not.

---

## ★★★★★ PART 5: SYSEDIT.EXE's WIN16 TASK RUNS

With `0xc5` answered, the launch gets all the way through: krnl386 binds SYSEDIT's
imports (`SHELL.DLL` now **opens**), creates its task database, and reaches the task
launch —

```
FUNC=0x74 ... task=0x00000adf ... (wExpWinVer 0x030a)
WOWSCHED: task 0x00000adf parked at its launch; creator sent home through
          epilogue mode 25 with LoadModule result 0x00000a96 (TDB+0x1c)
```

**A real Win16 application has a task id, a task database and a launch frame.** And then
it sat there, because nothing resumed it.

### The missing moment: a task that waits is a task that yields

Moment (C) resumes a parked task when the creator **retires**. WOWEXEC never retires — it
opens its windows and settles into its message loop, so `[0x228]` never reaches 0.
`WowWaitForMsgAndEvent` is the Win16 *"I have nothing to do"* primitive and the only
thing that pump calls when the queue is empty, so **a task blocking on it is a task
offering the CPU**. That is moment (D), and it needs no new lever: the frame is already
a WOW32 frame with a mode word and a return hole.

★ **The wait returns 0, and that is read rather than chosen**: `wowexec seg1:0x07a1` is
`or ax,ax / jne 0x0798` — non-zero loops back to wait again, zero falls through to
`PeekMessage`. Zero is "carry on and look", which is what a task just handed its turn
back should do.

### ★★ `[0x228]` IS PART OF THE CONTEXT, NOT A LEVER

The register file is not the whole of a switch. The launch sequence sets krnl386's
current-task word (`seg1:0x97ee mov [0x228],es`) **before** the `0x74` BOP we park at,
and mode 25's epilogue pops it back to the creator. Restoring registers alone resumes the
new task's code with krnl386 still believing somebody else is current.

⚠ **This is the opposite of the ruled-out "write `[0x228]` to yield".** That used the word
as a *lever* to make krnl386 switch, and it does not work — `seg1:0x2c05`'s branch is a
re-entrancy guard whose incoming task is the caller's own. Here the host has already
switched, and this makes the guest's bookkeeping agree with the context it is about to
run, using the value the word held when that frame was parked. Moment (C) had the same
hole and is fixed with it.

⚠ **Two tasks only, and the code says so.** `g_ws_task` means *"the task that is not
running"* — complete for two, wrong the moment there is a third. A third needs a real run
queue, and krnl386 already maintains one (every task linked at `seg1:0x99ed` by the signed
priority at `TDB+0x08`): walk that rather than invent an order.

### Measured

```
WOWSCHED: task 0x000003b7 waited for a message -- YIELDING to parked task 0x00000adf
```

and **SYSEDIT's task executes** — on its own stack (`SS:SP=0x0a97:0x24a8`), calling into
krnl386. The 268 MB message-loop spin is **gone**: the run is 735 KB and ends at a `#GP`
in code at `0x0abf:0x09f0`, `cmp byte es:[di],0`, with a **null `ES`**.

⚠⚠ **That fault is NOT caused by the `0xd2` call in front of it, and I nearly filed it
that way.** `0xd2` (seg2 table, 0 args, from `seg1:0x99be`) is a **poll loop** — `0` means
done, `>0` means call again, `<0` means handle-then-done — and its routine pops `ES` back
off the stack at `seg1:0x99d1`. So the null `ES` predates the call. *A measurement that
something happens is not a measurement of what it returns*, one more time.

---

## ▶ RESUME HERE

**Where it stands:** the whole chain works. WOWEXEC is told what to run, krnl386 resolves
and opens SYSEDIT's modules, builds its task database, launches it — and **SYSEDIT.EXE's
Win16 task executes on its own stack.** It faults early, and nothing has drawn a pixel.

### 1. ★★★ The environment a task inherits — `PSP+0x2c`

**Part 6 traced and half-fixed this.** The fault bytes were matched against every guest
module at that offset: an exact hit in **`sysedit.exe` seg1**, identical to the file, so
nothing had corrupted it. The code is

```
09e1  mov es,[0x1ae]        ; its PSP
09e5  mov cx,es:[0x2c]      ; the ENVIRONMENT segment
09ea  jcxz done             ; 0 is handled...
09ec  mov es,cx             ; ...1 is not
09f0  cmp byte es:[di],0    ; #GP -- selector index 0 is the null descriptor
```

so the whole fault is one field. **`INT 21h AH=55h` (create PSP) had no PM arm** —
measured, exactly two calls per run, one per task — so neither task ever got a PSP and
`+0x2c` held whatever was in that memory: **0** for WOWEXEC (whose launcher then read
`lstrlen(0000:0000)` and took a reflected `#GP` inside krnl386) and **1** for SYSEDIT,
which walks past the `jcxz` guard. One cause, two symptoms.

`AH=55h`/`26h` were already implemented for V86 and merely unreachable, and the V86 code
could not be reused: **`DX` is a selector here, not a paragraph**, and **`+0x2c` must be
a selector too**, because the guest's next instruction is `mov es,` that word.

★ **Fixed and confirmed: the `krnl386 seg1:0x259f` fault is gone** — WOWEXEC's own
environment walk works.

⚠ **SYSEDIT's `0x09f0` fault survives — and it was WATCHED, not reasoned about.** Every
PSP the host builds is now recorded, and `+0x2c` is sampled at **every WOW32 BOP and
every PM `INT 21h`**. The sampling interval *is* the resolution: sampling only at WOW32
calls put the whole of WOWEXEC's launcher, `LoadModule` included, inside one window,
which names a suspect rather than a writer. Three lines then replaced a page of
speculation:

```
sel 0x03bf (WOWEXEC) +0x2c 0x03c7 -> 0x0aff   its launcher installs its new env
sel 0x0adf (SYSEDIT) +0x2c 0x03c7 -> 0x0001   ★ clobbered
sel 0x03bf (WOWEXEC) +0x2c 0x0aff -> 0x03c7   its launcher restores its own
```

⇒ **WOWEXEC's environment build works** — `0x0aff` is a real selector and the
save/restore around `LoadModule` is visible in the log. The **child's** field is
separately written to `1`, in a tight window: between our `AH=55h` returning and the next
WOW32 call (`0x8a`), i.e. **inside krnl386's own caller**. `1` is `0|1`, so some source
of it is zero.

⚠⚠ **AND THE INSTRUMENT HAD TO WATCH THE SELECTOR, NOT A LINEAR ADDRESS.** The first cut
froze `dpmi_sel_base()` at `AH=55h` time — and the log three lines earlier says why that
is wrong: krnl386 **re-bases** these selectors (`INT31h 04F2` moved `0x0adf` from
`0x297c0` to `0x299e0` within three lines). A frozen address stops pointing at the PSP the
moment it moves, and the instrument would then report whatever now lives at the old
address as if it were the field — the same mechanism that once had krnl386 reading a
staged image at a stale address while it walked relocations at the new one. It resolves
per sample now and logs a `PSPENV REBASED` line. ★ With that hardening the suspicion was
**refuted**: no rebase happens after `AH=55h`, so the reading is sound.

⚠ **Eliminated, so nobody re-chases it:** the `or al,1` at `wowexec seg1:0x0500` is *not*
the source. Its operand `[bp-0x1c]` is the `GlobalAlloc` handle, and a zero there is
guarded at `seg1:0x045b` and aborts the launch with the error box we no longer see.

⚠ Also live, and not yet looked at: a `#NP` at `0x0b47:0x00b2` with `err=0x0b34` — a
demand-load fault for one of SYSEDIT's own segments — and a `#GP` at `krnl386
seg1:0xc5f0`.

### 2. `0xd1` and `0xd2` — krnl386 seg2's id space, still undispatched

`0xd1` is answered only by the `wow32ret.txt` **experiment** and must not be mistaken for
a service. It only has to be non-zero (see Part 4), but "non-zero" is a measurement of
the abort, not of what the value means: the wrapper hands it to `f(AX, hModule)` whose
result becomes a DWORD the TDB creator keeps. `0xd2` is a poll loop whose `0` we answer,
which is at worst premature.
▶ Both need **a dispatcher for krnl386 seg2's table** — 121 stubs, 50 already named by
the export table, listed with
`tools/ne/wowmap.py guest/ne/krnl386.exe "--table=seg2 -> imported thunk"`.

### 3. The second CWD-relative path site

`0xc5` fixed the module lookups, but two opens still compose against the current
directory — `MMSYSTEM.DLL` and `TIMER.DRV`, both with `al=0x40` rather than `0x80`, i.e.
a different open site that does not go through `0xc5`.

### 4. Still true, and still the big one

`DispatchMessage` means the host must **call** 16-bit code. `g_wu_win[].wndproc` has held
a 16:16 far pointer since `CreateWindow` and nothing has ever used it.

### 0. ★★★★★ PART 4: SYSEDIT GETS A TASK DATABASE, AND THE REAL BLOCKER IS `0xc5`

`0xd1` was run as an **experiment**, not implemented — `wow32ret.txt` with `d1 00000001`,
which the log stamps `** an EXPERIMENT, not a service **` on every use. (⚠ id-space
checked first: `0xd1` does not exist in krnl386's seg1 table, so the override cannot
collide.) Prediction written before the run: *if the abort is the only obstacle, the TDB
creator proceeds and a third task id appears.*

**Half right, and the half that failed is the more useful half.** No third task appeared
— but krnl386 went a long way further:

```
INT31h 0007 setbase ×3                       SYSEDIT's segment descriptors
0x7d AcceptTaskSelector -> 0x0bbf            ★ A TASK DATABASE SELECTOR FOR SYSEDIT
INT21h AH=55h  (PM thunk TODO)               ★ create-PSP, unimplemented in our PM thunk
GetProfileInt("ModuleCompatibility","SYSEDIT")  ★ krnl386 KNOWS THE MODULE BY NAME
```

So `0xd1`'s only job, as far as this path is concerned, is *not returning zero*: with any
non-zero answer krnl386 builds SYSEDIT's task database, approves its selector, and
registers the module. **What stops it next is not a missing WOW32 id at all.**

#### ★★★ It is the module search path — the defect filed two sections ago as "not chased"

krnl386 goes on to resolve SYSEDIT's imports. SYSEDIT imports `SHELL.DLL`, and:

```
0xc5 (krnl)  arg[2] = "SHELL.DLL"            -> UNIMPLEMENTED, answered 0
GetCurrentDirectory -> "Documents and Settings\Matthew"
AH=43 attributes "C:\Documents and Settings\Matthew\SHELL.DLL" -> CF=1
AH=3d open       "C:\Documents and Settings\Matthew\SHELL.DLL" -> FAILED gle=2
```

⇒ **`0xc5` is the module-path resolver, and answering it `0` is what makes krnl386 fall
back to composing the name against the current directory.** The defect recorded earlier
as cosmetic is the launch blocker.

#### The semantics are pinned by the two call sites, and by their symmetry

```
0848  … push src.seg / push src.off / push ss / push bx     ; bx = [bp-0x1e]
0856  lcall 0xc5
085b  mov [bp-0x20],ax / cmp ax,0 / je 0x0875               ; 0 -> fall back
0864  push [bp-0x1c] / push [bp-0x1e] / push ss / push si / push ax / lcall  ; RESOLVED
0875  push [bp+0x0c] / push [bp+0x0a] / push ss / push si / push ax / lcall  ; ORIGINAL
```

★ **The two tails are the same five-word call with one substitution**: the resolved far
pointer in place of the original one. That is what proves `dst` receives a **16:16 far
pointer to a path string**, not a string copied into a buffer — a much stronger argument
than reading the pushes alone.

★ And the **second** call site (`seg2:0x08ba`) calls `0xc5(dst, NULL)`, gated on the
first call having returned non-zero, and ignores the result. So the pair is
**resolve / release**, and a host implementation owns the storage in between.

⚠⚠ **THIS IS WHY `0xc5` IS NOT A ONE-LINER.** The answer has to be a far pointer the
guest can dereference *in protected mode*, so the host needs guest-visible scratch with
a **selector**. `g_pm_xfer_seg` is a V86 paragraph, not a selector, and is reused by
every PM→V86 `INT 21h` — it cannot hold a string across the resolve/release window. The
next step is therefore a small **WOW scratch selector** allocated at bring-up, with
`0xc5` writing the path into it and returning `sel:0`, and the release call a no-op
while only one is outstanding (which the guest's own pairing guarantees). ⚠ The LDT
machinery is delicate — see the standing notes about reserved indices and force-typing.

⇒ **Resolution itself is not a guess**: "find this module the way Windows does" is the
Windows and system directories then the path, which is `SearchPathA`. The only open
question is where to put the answer.

---

### 1. ★★★ krnl386 **seg2** id `0xd1` — the call that must not return zero

⚠ **`0x82` IS REFUTED, AND IT WAS MY OWN HANDOFF THAT NAMED IT.** Part 3 above said the
frontier was `0x82`, "the last call before the verdict", with a warning attached that
this is where to *start* rather than proof. The warning was the right one:

- `0x82` is **`INT 21h AH=3Bh`** — `chdir`, the twin of `0xc9 GetCurrentDirectory`.
  Named the cheap way: the WOWBOP line records `ax=0x3b1a` at the call, and `AH` names
  the DOS function outright. No disassembly needed.
- And it is **already succeeding**. Its call site's fall-through (`seg1:0x53c5`) is
  `mov al,0 / jmp 0x5577`, and `0x5577` is `pop ds / popf / **clc** / ret` — *carry
  clear*, the DOS success convention. The `je` that our sentinel does not take leads to
  the chain-to-DOS path. So answering `0` already makes krnl386 report the chdir as
  done, and implementing it would have changed nothing.

**The real abort is one call earlier**, and it is in a table the host does not dispatch
at all. Tracing the whole load rather than its last line:

```
AH=3d open  "…\SYSEDIT.EXE"          -> handle 6
AH=3F read 0x40 @0                   -> 4d 5a …            "MZ"
AH=42 seek 0x400
AH=3F read 0x40 @0x400               -> 4e 45 05 14 …      "NE"
AH=3F read 0x1c3  -> sel 0xbcf:0x60                        the NE tables
AH=42 seek 0x6c0
   0x97 read 0x1580 -> sel 0xbc7:0    (serviced)           ★ the first CODE SEGMENT
   0xd1  from seg2:0x2c84             UNIMPLEMENTED -> 0   ★ and here it stops
AH=3E close h=6
AH=3E close h=6                      (was NOT open)        -- a double close
   0x9d WowFailedExec  ->  WowMsgBox "Insufficient memory…"
```

krnl386 reads SYSEDIT's **first code segment** and then asks `0xd1`. The call site says
exactly what a zero means:

```
2c66  test byte [0x46c],1 / je            ; a gate: set -> return 0 without asking
2c72  push [0x293] / push [bp+6] / push [bp+4] / push [bp+8]   ; 8 arg bytes
2c81  call <0xd1 stub>
2c84  or ax,ax
2c86  je 0x2c93
2c93  mov ax,0xffff                       ; ★ 0 from us -> this function returns 0xFFFF
```

Measured arguments: `(0xbcf, 0x3d7:0x1ce4, 0x13f)` — the module's own selector, a far
pointer that is **not** a C string (the decoder printed nothing for it, so it is a
structure), and the global `[0x293]`.

⚠⚠ **`0xd1` IS IN A THIRD ID SPACE, AND THE HOST HAS NO DISPATCHER FOR IT.** The stub
lives in krnl386's **seg2** (`stubseg=0x1d7`), not seg1 (`0x1cf`), so `f.krnl` — which
is `stubseg == CS` — is false, `wow32_call()` returns 0 immediately, and the log prints
`[?'s table -- a DIFFERENT id space]`. Session 38's table already counted it: *krnl386
seg2 → imported thunk, **121 stubs***. `0xc5` (session 39, the path canonicaliser) is in
the same table. Nothing in this host answers any of them.

▶ So `0xd1` needs **a dispatcher for krnl386 seg2's table**, which does not exist.
`tools/ne/nedis.py guest/ne/krnl386.exe 2 <off>` reads those call sites, and
`tools/ne/wowmap.py guest/ne/krnl386.exe "--table=seg2 -> imported thunk"` lists all 119
ids with 50 of them already named by the export table (`0xd1` is not one of them).
`[0x46c]` gates at least `0xc5` and `0xd1`, so it is worth knowing what sets it.

★ **Its one caller names what it is for.** `seg2:0x29a5`, twenty-one bytes into the
task-database creator (`seg2:0x2984`), and it is skipped entirely when its far-pointer
argument is null (`cmp si,[bp+0xc] / je`). Arguments, measured:
`(hModule = 0xbcf, lpParameterBlock = 0x3d7:0x1ce4, [0x293] = 0x13f)` — and the creator
dereferences that same pointer at `+2` (`les di,es:[di+2]`), which is Win16 `LOADPARMS`'s
`lpCmdLine`. So `0xd1` is *"32-bit side, take a record of this new task"*, returning a
handle the wrapper converts to a DWORD.

⚠ **But it is NOT the frontier — see Part 4 above.** Any non-zero answer gets past it,
and what stops the run next is `0xc5`. Implement `0xd1` for real only after the module
path is fixed, or the next run measures the same wall through a different id.

### 2. The message loop (was #1 last time, still real, now second)

`wowexec seg1:0x0798` — `WowWaitForMsgAndEvent` / `PeekMessage` / `TranslateMessage` /
`DispatchMessage`, all four named from the import table. Both the wait and the peek are
unimplemented, so the pump spins and fills the log to its 268 MB cap. Behind it is the
first work in this epic that is not *answering* the guest: **`DispatchMessage` means the
host must CALL 16-bit code**, and `g_wu_win[].wndproc` has been holding a 16:16 far
pointer since `CreateWindow`.

### 3. Module search path

Bare module names resolve against the current directory. Real, filed, and now on the
launch path.

### 4. ~~Re-measure Doom and a DOS extender~~ — **DONE, and it fixed Doom**

See Part 3. A/B on the single file: pre-fix, Doom reaches **no** startup stage and loops
in a `#GP` to a 268 MB log; post-fix it completes **all eleven** stages in 3.5 MB, twice.
⚠ `dpmitest.com`/`pm32flat.com` pass but are **not** evidence here — they never declare a
code region, so they never take the changed decision. Doom does (19 regions, 196 rejects).
⚠ Still open: the `#GP(IDT)` arm does not cover a **base-0** code selector.

### How to drive it

```bash
ARCHIVE=build/wowruns ./scripts/bmwow.sh              # deploy, run, collect
ARCHIVE=build/wowruns ./scripts/bmwow.sh --no-deploy  # re-run what is on the box
```
- The scheduler is **opt-in**: `touch /private/tmp/xpshare/wowsched.txt` to arm it.
  **Without it you are measuring the baseline, not the frontier.**
- ⚠ SMB writes to `/private/tmp/xpshare` need the sandbox disabled.
- ⚠⚠ **Always re-run with the scheduler off and confirm 277 / 44 / `9 · 222 · 39` /
  `0001:229C`.** ⚠ That is the baseline **as of `0xc5`** — it was 265 / 42 / `9·222·27`
  before, and it moved because `0xc5` resolves module paths with the scheduler off too,
  carrying WOWEXEC twelve calls further into the same wall. A baseline that moves for a
  reason you can name is fine; one that moves silently is not. Unmoved as of this session, across the patcher change.
- ⚠ The spinning pump fills the log to its 268 MB cap in seconds. `grep` it; do not open
  it. `grep -c 'CreateWindow "'` is 2 in a healthy run.
- ★ `grep -m1 'LAUNCH \['` says whether the program was handed over, and which.

### How to drive it

```bash
ARCHIVE=build/wowruns ./scripts/bmwow.sh              # deploy, run, collect
ARCHIVE=build/wowruns ./scripts/bmwow.sh --no-deploy  # re-run what is on the box
```
- The scheduler is **opt-in**: `touch /private/tmp/xpshare/wowsched.txt` to arm it.
  **Without it you are measuring the baseline, not the frontier.**
- ⚠ SMB writes to `/private/tmp/xpshare` need the sandbox disabled.
- ⚠⚠ **Always re-run with the scheduler off and confirm 277 / 44 / `9 · 222 · 39` /
  `0001:229C`.** ⚠ That is the baseline **as of `0xc5`** — it was 265 / 42 / `9·222·27`
  before, and it moved because `0xc5` resolves module paths with the scheduler off too,
  carrying WOWEXEC twelve calls further into the same wall. A baseline that moves for a
  reason you can name is fine; one that moves silently is not.
- ⚠ The spinning message pump fills the log to its 268 MB cap in seconds. `grep` it;
  do not open it. `grep -c "CreateWindow \""` is 2 in a healthy run — if it climbs, the
  task-relaunch loop is back.

### Ruled out — do not re-try

Everything in [session 38's list](session-38.md#ruled-out--do-not-re-try-all-by-measurement)
still holds, plus:

- **Reading an imported call's target out of the disassembly.** `lcall 0, 0xffff` is a
  chain terminator. Use `tools/ne/neimports.py`.
- **Breakpointing `wowexec:0x0849` to catch a `CreateWindow` failure.** That is
  `RegisterClass`'s error path — see the correction above.

### Tools

`tools/ne/neimports.py` (**new** — names every imported call site from the relocation
chains), `tools/ne/nedis.py`, `tools/ne/wowmap.py`, `tools/ne/wowdecline.py`,
`scripts/bmwow.sh`.
