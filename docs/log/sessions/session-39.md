# Session 39 — WOWEXEC opens two windows and reaches its message loop

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

## ▶ RESUME HERE

**Where it stands:** WOWEXEC is fully initialised. It has two window classes, two
windows, and it is sitting in its message loop asking for messages that do not exist.
Every call in that loop is named. Nothing draws.

**The next wall is `DispatchMessage` calling 16-bit code**, and it is a different kind
of work from everything in sessions 34–39: so far the host has only ever *answered* the
guest, and a window procedure means the host must *call* it. `g_wu_win[].wndproc` is a
16:16 far pointer that has been sitting there since `CreateWindow`, unused.

### How to drive it

```bash
ARCHIVE=build/wowruns ./scripts/bmwow.sh              # deploy, run, collect
ARCHIVE=build/wowruns ./scripts/bmwow.sh --no-deploy  # re-run what is on the box
```
- The scheduler is **opt-in**: `touch /private/tmp/xpshare/wowsched.txt` to arm it.
  **Without it you are measuring the baseline, not the frontier.**
- ⚠ SMB writes to `/private/tmp/xpshare` need the sandbox disabled.
- ⚠⚠ **Always re-run with the scheduler off and confirm 265 / 42 / `9 · 222 · 27` /
  `0001:229C`.**
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
