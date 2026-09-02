# Session 40 — the host CALLS 16-bit code, and SYSEDIT builds its whole MDI window

- **Branch:** `m9/completeness`
- **Issue:** [#128](https://github.com/MrMatthewLayton/ntvdmex/issues/128)
- **Predecessor:** [session 39](session-39.md) — `SYSEDIT.EXE` ran its startup and created
  its own main window, then exited because nothing had ever called a 16-bit window
  procedure.

---

## ★★★★★ THE HEADLINE

**A Win16 window procedure has been called by this host, and it returned.**

Session 39 ended with the frontier compressed into one sentence — *the host must CALL
16-bit code* — and named to the instruction: `sysedit seg2:0x0114` gives up because
`[0x22]` is zero, and `[0x22]` has exactly one writer, at `sysedit seg1:0x01cf`, inside
the frame's **own window procedure**, while it handles `WM_CREATE`.

That direction now exists. In one run:

```
WOWCALL: -> 0x0ac7:0x0131(hwnd=0x0140, msg=0x0001, wp=0x0000, lp=0x00000000)
         ds=0x0a9e ss=0x0a9f:0x2356 ret=0x0c07:0000 -- ENTERED, depth 1
   ... 12 WOW32 calls made by the procedure ...
WOWCALL: <- returned 0x00000000 from 0x0ac7:0x0131 (hwnd=0x0140 msg=0x0001, depth now 0)
```

`0x0131` is the address the disassembly said it would be, before the run: `sysedit
seg1:0x0131`, the Win16 exported prologue `push ds / pop ax / nop / inc bp / push bp`,
ending `retf 0x0a` at `seg1:0x0222`.

Three walls fell behind it, each one named by the run that hit it rather than guessed:

1. **`MDICLIENT` was not a class**, because under WOW the SYSTEM classes belong to the
   32-bit side, and the 32-bit side is us.
2. **`LoadAccelerators` needs `USER` id `0x217` `NotifyWow`** — and that call does not
   return a handle, it returns permission.
3. And with both answered, **`SYSEDIT.EXE` shows its window and opens `C:\CONFIG.SYS`
   and `C:\AUTOEXEC.BAT`** — the thing the program exists to do.

And then the other half of the same mechanism went in — `SendMessage` and the default
window procedure a system class needs — and **SYSEDIT builds its entire user interface**:

```
WM_MDICREATE "mpchild" "C:\WINDOWS\SYSTEM.INI" in client 0x0160 -> hwnd=0x0180
  -> WOWCALL into mpchild's own procedure, which creates
     CreateWindow "EDIT" style=0x513000c4 -> hwnd=0x01a0
... four times: SYSTEM.INI, WIN.INI, CONFIG.SYS, AUTOEXEC.BAT
```

**Four MDI children, each with its own EDIT control, each child's window procedure run by
this host.** And then the last piece: an edit control's text is a handle in the
**application's own local heap**, which the host cannot make and the guest's KERNEL can —
so the host **calls `KERNEL.5 LocalAlloc` in the application's DGROUP**, and

```
C:\WINDOWS\SYSTEM.INI  0x00e7 bytes -> loaded
C:\WINDOWS\WIN.INI     0x01dd bytes -> loaded
```

**two of the four files are in memory**, in blocks the application allocated, grew, filled
and owns. SYSEDIT then sits in `GetMessage`, and we tell it `WM_QUIT`. See
[▶ RESUME HERE](#-resume-here).

⚠ **The baseline moved by exactly one call, and it has a name.** With both switches off
the same build gives **270 / 45 / 122 / 97 · `9 · 222 · 39` · `0001:229C`** against
session 39's 270 / 44 / 122 / 98. The BOP count, the declines, the task histogram and the
GP box are unchanged; one call moved from *unimplemented* to *serviced*, and it is
`GetWindowsDirectory` at **line 615 of both logs** — krnl386's own bootstrap call from
`seg1:0xc91a`, which happens on every run including a default one (Part 5). A baseline
that moves for a reason you can point at in the diff is fine; one that moves silently is
not. **270 / 45 / 122 / 97 is the figure to compare against from here.**

---

## ★★★★★ PART 1: HOW A HOST CALLS A 16-BIT WINDOW PROCEDURE

The mechanism is in [`src/wow/wowcall.h`](../../../src/wow/wowcall.h), and it is small
because three of its four pieces already existed.

| What a call needs | Where it came from |
|---|---|
| A context we can put back | `wowsched.h` — the guest register file is one 0x40-byte block in the VDM TIB |
| A stack | The one we are standing on. At a USER BOP the chain is app → USER stub → krnl386's thunk → BOP, **all on the application's own task stack** |
| An entry convention | Read off SYSEDIT's own prologue and body — see below |
| **A way back** | ★ The only new thing: **three bytes of guest memory holding `C4 C4 57`** and a 16-bit **code** selector over them |

### The entry convention, read out of the guest and not out of a header

```
0131  1e        push ds        ; ★ the UNPATCHED Win16 export prologue
0132  58        pop  ax
0133  90        nop
0134  45        inc  bp
0135  55        push bp
0136  8bec      mov  bp,sp
0138  1e        push ds
0139  8ed8      mov  ds,ax     ; ⇒ DS on entry IS the contract
...
0140  8b760e    mov si,[bp+0x0e]   ; hwnd  -- and si is later pushed as the MDI
0143  8b7e0a    mov di,[bp+0x0a]   ; wParam    client's PARENT, so it can only be
0146  8b460c    mov ax,[bp+0x0c]   ; msg       the window handle
0149  48/7442   dec ax / je        ; ⇒ 1 == WM_CREATE
...
0222  ca0a00    retf 0x0a          ; FAR, and it cleans its own 10 argument bytes
```

★ **`push ds / pop ax` is load-bearing.** `nedump` says sysedit.exe is
**`DGROUP=MULTIPLEDATA`**, so the loader does *not* rewrite those three bytes into
`mov ax,<DGROUP>` the way it does for a single-data DLL. The procedure therefore takes
its data segment **from whoever called it**. Enter it with the wrong DS and it runs its
whole body against another module's data — silently. So the host enters with
`DS = AX = the window's own hInstance`, which is the guest's own statement about its
data segment (it is the word the program put in `WNDCLASS.hInstance` and passed again to
`CreateWindow`). Setting AX to the same value satisfies a `MakeProcInstance`-style
prologue at no cost; one assignment cannot be right for one form and wrong for the other,
because both forms read the same register.

### The way back

```
C4 C4 57
```

pushed as the far return address, with a 16-bit **code** descriptor (`access 0xFA`,
limit one paragraph) over its paragraph. The procedure's own `retf 0x0a` lands on it and
the BOP arrives at the host like any other.

★ **It is dispatched by LINEAR ADDRESS, not by the code byte.** The byte is for the
reader. Our own INT-site patcher also writes `C4 C4` over `CD nn`, so a third byte is a
guess about a namespace we do not own; the address is ours by construction and cannot
collide. `dpmi_seg_to_desc` could not be reused — it builds a **data** descriptor and
these three bytes have to be *executed*.

### Ordering, and why it is the only one that works

The service has already written its answer into the `sub sp,4` return hole, and EIP has
already been advanced past the BOP, **before** the context is parked. So what is saved is
the guest exactly as it will be resumed. A context saved *at* the BOP would execute the
BOP a second time on return, which is a loop, not a call.

★ **`WM_CREATE` may refuse.** Returning `-1` is the documented way for a window procedure
to abort its own creation, and the host honours it: the return hole's linear address is
recorded with the parked frame, so the `CreateWindow` that sent the message can still be
made to answer 0 after the fact. The hole is guest memory and outlives the switch, so
this is a two-byte write rather than a special case.

### Re-entrancy is the normal case

The first thing SYSEDIT's `WM_CREATE` handler does is call `CreateWindow` again. So the
saved contexts are a **stack** (depth 8), and exceeding it **refuses** rather than
truncating — a refused callback leaves the guest with a correct "no client window", which
it handles; a truncated one leaves it running on somebody else's stack.

### ⚠ Named gaps, said now rather than discovered later

- **`lParam` is 0.** `WM_CREATE`'s `lParam` should be an `LPCREATESTRUCT`, and this host
  has never built one and does not know the Win16 layout **from measurement**. SYSEDIT's
  frame procedure never reads it; an MDI child will. The log line says `[lParam=0: no
  CREATESTRUCT yet]` on every call, so the zero cannot be mistaken for a measurement.
- There is no message **queue**, so this is a `SendMessage` and never a `PostMessage`.

---

## ★★★ PART 2: THE SYSTEM CLASSES BELONG TO THE 32-BIT SIDE, WHICH MEANS THEY ARE OURS

The first run with the callback armed produced exactly the predicted line:

```
-> SERVICED (USER), returned 0x00000000 -- CreateWindow: no such class
```

`[0x22]` stayed zero, for a **new** reason one step further on: SYSEDIT's `WM_CREATE`
handler calls `CreateWindow("MDICLIENT", ...)` at `sysedit seg1:0x01ca`, and nothing had
ever registered that class.

★ **And nothing ever would have.** Under WOW, `USER.EXE` is a **thunk module** — every
export funnels to the 32-bit half, which is why `RegisterClass` reaches this host at all.
The run says so without any new measurement: in a whole SYSEDIT launch there are exactly
**four** `RegisterClass` calls, and all four are a program's own (`WOWExecClass`,
`WOWFaxClass`, `mpframe`, `mpchild`). USER never registers one, because in this
architecture it cannot. So the classes Windows itself provides have nowhere else to come
from.

⚠ **The list is what the run asked for, not a list of system classes.** Windows also
provides `BUTTON`, `EDIT`, `STATIC`, `LISTBOX`, `COMBOBOX`, `SCROLLBAR` and the numbered
dialog/menu classes. Seeding all of them would be answering questions nothing has asked,
and every one would be a class that exists and does nothing — the "runs but lies" shape.
They go in when a run names them.

Two smaller things went in with it, both because the alternative is a host that hides its
own state:

- **`RegisterClass` REFUSES a system class.** The existing code re-registers over an
  existing class on purpose (WOWEXEC does it once per relaunch), and that same path would
  let a program point `MDICLIENT` at its own procedure and take the class away from every
  other window made from it.
- **A window made from a system class says so** (`[system class: no wndproc]`), because
  otherwise the ABSENCE of the `WOWCALL` line under it reads like a defect.

### ⚠ And an instrument that named the class of failure and withheld the instance

`"CreateWindow: no such class"` said nothing about *which* class — the exact shape this
project has been caught by before (session 36's MessageBox decoder printed the caption
and rejected the body). It prints the name now, or the atom when the lookup was made from
an atom, because a lookup that failed on an atom did not have a name to fail on.

---

## ★★★ PART 3: `NotifyWow` DOES NOT RETURN A HANDLE, IT RETURNS PERMISSION

With `MDICLIENT` registered, `[0x22]` came back `0x0160` and **`sysedit seg2:0x0114` —
the instruction session 39 named as the frontier — was passed.** The next stop was six
instructions later, and the run named it:

```
FUNC=0x0217 stub=0x0327  args=6b  from=0x0327:0x3e1c  (0x2382 0x0a9f 0x0003)
-> UNIMPLEMENTED, STEPPED OVER
```

`wowmap.py` names id `0x217` from USER's own export table: **`NOTIFYWOW`**, 6 argument
bytes, stub `seg1:0x12dd`. Its one call site is USER's entire implementation of
`LoadAccelerators`, and it reads end to end:

```
3dcc  push 0 / push 9        ★ lpType = 9 = RT_ACCELERATOR -- this IS LoadAccelerators
3dce  lcall FindResource     -> [bp-2]
3dde  lcall LoadResource     -> [bp-4], and 0 fails the whole call
3df4  lcall LockResource     -> [bp-0xc] : the bytes, 16:16
3e05  lcall SizeofResource   -> [bp-8]
3e10  push 3 / lea ax,[bp-0x10] / push ss / push ax
3e17  lcall <the 0x217 stub>          ★ THIS CALL
3e1c  or dx,ax / je 0x3e2a            -> 0 frees the resource and returns NULL
3e37  mov ax,[bp-4] / retf 6          ★ AND THE HANDLE IT RETURNS IS ITS OWN
```

★ **So the return value is not a handle.** Whichever way this call goes, the application
receives `[bp-4]` — krnl386's own global handle for the resource. All `NotifyWow` decides
is whether `LoadAccelerators` **succeeds**. Returning a fabricated handle here would have
been inventing a value nobody reads; the honest answer is "noted", which is what the
function's name says. It answers `1`, deliberately not a number that looks like a handle.

The 12-byte block at `ss:[bp-0x10]`, every field from a store in the window above:

| off | field | source |
|---|---|---|
| `+0x00` | `hInstance` | `[bp+0x0a]`, the caller's module |
| `+0x02` | `hResData` | `LoadResource`'s handle |
| `+0x04` | `lpResource` | `LockResource`'s 16:16 |
| `+0x08` | `cbResource` | `SizeofResource` |

Measured on the rig: `hInst=0x0a9e hRes=0x0a8e at 0x0a8f:0x0000 cb=0x00000040` — a
64-byte accelerator table, which is what SYSEDIT's is.

⚠⚠ **And `lpResource` is stale the moment we return.** `seg1:0x3e23` calls `GlobalUnlock`
on the very next instruction. A host that recorded that pointer for a later
`TranslateAccelerator` would be keeping an address the guest has already released — an
instrument that lies later, this project's most expensive shape. It is **logged, not
kept**. When accelerators are implemented the bytes must be **copied while they are
locked**, or asked for again through `FindResource`/`LoadResource`.

⚠ **Only `wKind == 3` is answered.** That is the only kind any run has ever taken;
anything else falls through to the honest "unimplemented", and the log says which kind.

---

## ⚠ ONE MORE INSTRUMENT THAT WAS LYING QUIETLY

Every USER call printed as

```
FUNC=0x00000217 [?'s table -- a DIFFERENT id space] stub=0x00000327
```

and the `?` cost a reading in this very session: `0x327` **is** USER's segment, learned
from its own stub at line 2717 of the same log and used by the dispatcher on every call.
`wow_module_of_sel()` is a **bind-stage** table and cannot name a selector krnl386
allocated at run time, so it answered `?` about something the host knew perfectly well.
The two tables that identify themselves now say their names.

---

## ★★★★★ PART 4: `SendMessage`, AND A DEFAULT WINDOW PROCEDURE THAT IS OURS

`SendMessage` is USER id `0x6f` (10 argument bytes, named from SYSEDIT's own relocation
chain at `seg3:0x007e`), and it is **two mechanisms, not two cases of one**:

- **to a window with a 16-bit procedure** — this *is* the call. Hand it to `wowcall.h`
  with the caller's own arguments, and ★ **the procedure's return value IS SendMessage's**.
  That needed a second return mode in `wowcall.h`: `KEEP` (the service wrote the answer;
  the procedure only gets a `WM_CREATE == -1` veto) versus `RESULT` (the procedure's DX:AX
  goes into the hole verbatim). Conflating them would have been silent — *"whatever the
  window procedure returned"* is the entire definition of `SendMessage`.
- **to a SYSTEM-class window** — the procedure is **ours**, for the same reason the class
  is. `wowuser_defproc` is deliberately tiny: the one message a run has ever sent, and a
  named 0 for everything else.

⚠ The whole service is gated on callbacks being armed. A `WM_MDICREATE` that made a child
window whose `WM_CREATE` never ran would be a half-built object the guest would then use —
worse than a missing answer — and the gate is also what keeps a default run identical.

### The MDICREATESTRUCT, read off SYSEDIT's own stores

`sysedit seg3:0x0046` builds one at `ss:[bp-0x1e]`; every offset below is a store in that
window, so none of it comes from a header:

| off | field | the store |
|---|---|---|
| `+0x00` | `szClass` | `mov [bp-0x1e],0x4a` / `mov [bp-0x1c],ds` |
| `+0x04` | `szTitle` | `mov [bp-0x1a],ax` / `mov [bp-0x18],ds` |
| `+0x08` | `hOwner` | `mov ax,[0x2e0]` |
| `+0x0a`…`+0x10` | `x, y, cx, cy` | four stores of `0x8000` (`CW_USEDEFAULT`) |
| `+0x12` | `style` (DWORD) | `mov ax,[0x28] / mov dx,[0x2a]` |

★ **And the reading is confirmed from outside the code.** `ds:0x004a` in SYSEDIT's DGROUP
— segment 6, read out of the file on disk — is the string `"mpchild"`, the class SYSEDIT
registered two calls earlier. A wrong offset for `szClass` does not decode to a class this
program has registered. The same read gives `ds:0x0030 = "mdiclient"` (what the frame
procedure creates, which a run had already asked for) and `ds:0x003a = "edit"`.

⚠ **The struct ends at `+0x16`.** `[bp-8]` is the SendMessage result, not a field, so the
slot where a `lParam` member would sit is never written by this program and is not read.

### `EDIT` went in because the guest binary named it, not because it is on a list

`sysedit seg1:0x0281` — inside `mpchild`'s own `WM_CREATE` handler — pushes `ds:0x003a` as
a `CreateWindow` class name, and that offset is `"edit"` in the file. **Reading the guest
binary is stronger evidence than waiting for the run line, not weaker**, and the run then
agreed: four `CreateWindow "EDIT" style=0x513000c4` in a row, one per MDI child.

---

## ★★★ PART 5: TWO DEFECTS THE NEW REACH EXPOSED, BOTH "RUNS BUT LIES"

Getting this far is what made both visible; neither could have been seen before.

### `SendMessage: no such window 0x0000 msg 0x040d` — the extra bytes were missing

`mpchild`'s `WM_CREATE` creates its EDIT control and then stores its handle **in the
window's own extra bytes** (`SetWindowWord`), and reads it back later to address the
control. With no storage behind them every read answered 0, so the program sent
`EM_SETHANDLE` to window handle **zero** — it had been told to forget its own control.

`Get/SetWindowWord` are USER `0x85`/`0x86`, and the bound is **the guest's own
declaration**: `sysedit seg2:0x0091 mov word [bp-0x14],8` is `+0x08` from the WNDCLASS base
at `bp-0x1c`, i.e. **`cbWndExtra = 8`** — exactly the four words at indices 0/2/4/6 that
its `WM_CREATE` writes. That same read puts `"mpchild"` at `+0x16`, so `wowuser.h`'s
WNDCLASS layout is confirmed a second time by a second program's own code.

⚠ **A negative index is a standard field (`GWW_*`) and this host does not answer those.**
The constants would be written from memory, which is the one thing this project has a
cardinal rule against. A run that needs one names the index in the log, and then it can be
read off the guest that asked.

### The window titles were wrong, and nothing had said so

The first run with MDI children titled them `"REGISTERPENAPP\SYSTEM.INI"` — a path built
by `lstrcat`ing `\SYSTEM.INI` onto **another module's leftover string**, because
`GetWindowsDirectory` was unimplemented and the buffer kept whatever was in it. Not a
failure; a **wrong name**, silently.

**krnl386 id `0xd0` is `GetWindowsDirectory`**, and two independent readings agree:

- krnl386's own call site says what shape it is, to the byte —
  `seg1:0xc910 push ds / push di / push 0x80`, then `or ax,ax / je`, then `repne scasb` to
  measure the string it wrote, then `mov [0x506],ds / [0x504],0x624 / [0x50c],cx` to cache
  the pointer **and its length**. That is a `Get<something>Directory` and nothing else.
- SYSEDIT says *which* directory, and it is a count rather than a guess: it imports
  `KERNEL.134 GETWINDOWSDIRECTORY` and **not** `KERNEL.135 GETSYSTEMDIRECTORY`, from
  exactly two sites, and its task makes exactly **two** calls to this id.

Answered, the titles read `C:\WINDOWS\SYSTEM.INI`, `C:\WINDOWS\WIN.INI`, `C:\CONFIG.SYS`,
`C:\AUTOEXEC.BAT`. ⚠ This is also the one call that moves the baseline, because krnl386
asks it during its own bootstrap.

---

## ★★★★★ PART 6: THE HOST ASKS THE GUEST'S KERNEL FOR MEMORY — AND FILES LOAD

`0x040D` was written up in Part 5 as `EM_SETHANDLE`. **It is `EM_GETHANDLE`**, and the
correction matters, because the run was stopping on the *first* of a pair, not the second.
SYSEDIT's file loader (`seg3`) reads end to end, every call named from the relocation
chain:

```
00cf  OPENFILE                          ; < 0 -> "Cannot open this file"
00f8  _LLSEEK(h, 0, 2)  -> [bp-4]       ; ★ the file's SIZE
010a  _LLSEEK(h, 0, 0)                  ; back to the start
011b  SendMessage(hEdit, 0x40D, 0, 0)   ; ★ takes NO parameters...
0124  push ax / push [bp-4]+1 / push 0x42
012c  LOCALREALLOC                      ; ★ ...and RETURNS A LOCAL HANDLE
0148  LOCALLOCK                         ; a far pointer to the bytes
015a  _LREAD(h, that, size)             ; the file goes straight in
017b  mov byte es:[bx+si],0             ; the guest NUL-terminates it itself
0183  LOCALUNLOCK
018b  SendMessage(hEdit, 0x40C, hMem,0) ; ★ 0x40C TAKES the handle
```

### ★★ And the handle has to be one the GUEST'S KERNEL made

Not an assumption about how Windows implements edit controls — **what this program
demonstrably requires**. It hands the answer straight to `LocalReAlloc` and `LocalLock`,
which operate on the local heap of the **current DS**, and DS throughout that routine is
SYSEDIT's own DGROUP (its window procedure's prologue put it there). A handle this host
invented would be a number `LocalReAlloc` rejects — and rejecting it is exactly what
produced *"Cannot open this file."*

So the host asks: **`KERNEL.5 LOCALALLOC`**, which krnl386's own entry table places at
`FIXED, segment 1, offset 0x3ddb` — and krnl386's segment 1 is *the segment every WOW32
BOP executes in*, so its runtime address is `<the BOP's CS>:0x3ddb` with **no resolution
machinery at all**. The disassembly there confirms the signature to the byte:
`test ax,0xf08d` against `[bp+8]` (LocalAlloc's own flag validation) and `retf 4`.

```
WOWCALL: -> 0x01cf:0x3ddb(0042 0020) ds=0x0a9e [hwnd=0x01a0 msg=0x040d] -- ENTERED
WOWCALL: <- returned 0x00422502 ... -- ★ the caller returns 0x00002502
        -> EM_SETHANDLE 0x2502 -- the control now holds the file's text
```

★ **This is the first time the host has called 16-bit code for its own reasons** rather
than to deliver a message, and it needed three small generalisations, each of which is a
statement about the interface rather than plumbing:

1. **An argument list, not a message.** `wowcall_enter` takes N words in *declared* order;
   a window procedure's 5 and `LocalAlloc`'s 2 are the same mechanism.
2. **A sink.** A call the host makes for its own reasons has a result the host must keep,
   and the result only exists when the guest returns — long after the service that asked
   for it finished. One pointer into the static object it belongs to, no completion queue.
3. ⚠ **A declared RETURN WIDTH, and this one was a defect caught by its own log.**
   `LocalAlloc` came back `0x00422502`, whose low word is the handle and whose high word
   is **the `0x42` we had pushed as its flags**. The guest reads AX and was unharmed, but
   the log printed a 32-bit number that was not a value — an instrument lying about a call
   the host itself made. A Win16 function returning a WORD does not set DX, so the width
   is now declared (`WOWCALL_RET_RESULTW`) and the log says when DX was discarded.

### Measured: two of the four files load, and the other two are 0 bytes

```
C:\WINDOWS\SYSTEM.INI  seek-to-end -> 0x00e7   loads, no message
C:\WINDOWS\WIN.INI     seek-to-end -> 0x01dd   loads, no message
C:\CONFIG.SYS          seek-to-end -> 0x0000   "Cannot read this file."
C:\AUTOEXEC.BAT        seek-to-end -> 0x0000   "Cannot read this file."
```

⚠ **A perfect correlation on four samples is a lead, not a cause, and it is recorded as
one.** *"Cannot open"* has become *"Cannot read"*, which is a different message from a
different site (`seg3:0x0164`, error code 2) reached from `cmp ax,[bp-4] / jne` after
`_LREAD` — so `_lread` returned something other than the 0 that a 0-byte file's size
implies. Our own `AH=3Fh` arm returns `AX = 0` with CF clear for a zero-length read
(read, not assumed), so the discrepancy is somewhere between that and `_lread`'s return.
**It is not yet attributed, and the test that settles it is the oracle**: run `SYSEDIT`
under *stock* ntvdm on the same box and see whether it says the same thing about the same
two empty files. Empty `CONFIG.SYS`/`AUTOEXEC.BAT` are the normal state on XP, so "the
application is right" is a live possibility and must be excluded rather than assumed.

---

## ★★★★ PART 7: THE ORACLE ANSWERED, AND THE DEFECT IS OURS

Part 6 left *"Cannot read this file."* about the two 0-byte files as a **lead, not a
cause**, with the test named: run `SYSEDIT` under stock `ntvdm` on the same box. Done
(`scripts/bm/stocksysedit.bat`, and ⚠ it restores the IFEO Debugger key on every exit
path). The answer is a picture —
[`stock-sysedit-four-files.png`](../../research/evidence/stock-sysedit-four-files.png):

> **"System Configuration Editor" with FOUR MDI children** — `C:\WINDOWS\SYSTEM.INI`,
> `C:\WINDOWS\WIN.INI`, `C:\CONFIG.SYS`, `C:\AUTOEXEC.BAT` — SYSTEM.INI's text visible in
> the leftmost, `AUTOEXEC.BAT` on top and correctly **empty**, and **no message box
> anywhere**.

⇒ **Stock opens a 0-byte file without complaint. The message is ours.** The same run
printed the sizes from the box's own directory listing — `system.ini 231`, `win.ini 477`,
`CONFIG.SYS 0`, `AUTOEXEC.BAT 0` — which matches our seeks exactly (`0xe7`, `0x1dd`, `0`,
`0`), so the file sizes were never in question; only the verdict was.

### ★★★ And krnl386's own code says exactly how a zero-length read goes wrong

`_lread` is `KERNEL.82`, entry-table `FIXED, segment 1, offset 0x3d7e`:

```
3d93  mov cx,[bp+8]        ; wBytes
3d96  jcxz 0x3da4          ; ★★ ZERO -> skip the buffer probe entirely
3d98  mov ax,[bp+0xa] / mov cx,[bp+0xc] / mov bx,1
3da1  call 0x4114          ;    the probe: `or byte es:[bx],0` -- ★ CLEARS CF
...
3da4  pop dx / pop bp / pop es / jmp 0x4530
4530  mov cl,0x3f / ... / pushf / push cs / call 0x4ff2   ; ★ pushf captures CF HERE
4548  pop ds
4549  jae 0x454e           ; ★★★ CF CLEAR -> keep AX
454b  mov ax,0xffff        ; ★★★ CF SET   -> -1
454e  retf 8
```

⇒ **`_lread` returns `-1` if CF is set on return**, and the zero-length path is the one
that skips `0x4114` — the instruction that would otherwise have cleared CF. SYSEDIT then
does `cmp ax,[bp-4]`, `0xFFFF != 0`, and reports the failure. Every piece of the symptom
is accounted for: **two of four files, exactly the two that are empty.**

★ **So the host's CF is not reaching the guest.** Our PM `AH=3Fh` arm sets `AX = 0` and
clears CF in the live EFLAGS (verified by reading it, and the log now prints both — see
below), so the loss is downstream, in how the flags a DOS service returns are restored
through krnl386's own `pushf / push cs / call` dispatcher. On every other path the guest
had *already* cleared CF for its own reasons, which is why a defect that has been there
all along took a zero-byte file to expose.

⚠ **Not fixed in this session, and deliberately not guessed at.** The fix has to know
*which* flags image the guest restores from, and inventing an answer to that is how a host
corrupts a stack. **The next experiment is one instruction wide**: a PM breakpoint at
`krnl386 seg1:0x4549` reads CF and AX at the `jae` that decides, and says whether CF is
set on arrival (our flags are lost) or clear (and the fault is elsewhere).

### ⚠ One instrument fixed on the way, by this project's own rule

`INT21h AH=3F` printed the byte count and **not the answer** — half an instrument, when
the entire question was what `_lread` came back with. It now prints the requested count,
`AX` and `CF`. `rd` and `AX` are the same number today, and printing only one of them was
a claim that they always would be.

---

## ★★★★ WHERE IT ACTUALLY GETS TO NOW

With `wowsched.txt` **and** `wowcall.txt` armed, one run:

| | |
|---|---|
| WOW32 BOPs | **606** (baseline 270) |
| serviced / declined / unimplemented | **133 / 298 / 157** |
| task histogram | `9 · 225 · 203 · 169` — **four**, the last being SYSEDIT's |
| 16-bit calls made by the host | **11**, all returned — 7 window procedures and 4 `LocalAlloc` |
| windows created | **8** — 2 WOWEXEC, `mpframe`, `MDICLIENT`, and 4 `EDIT` controls |
| MDI children | **4**, of which **2 hold a file's text** |
| ends at | `★ ExitKernelThunk(0x00000000)`, 888 KB |

and in the middle of it, `SYSEDIT.EXE`:

1. creates its frame window and **receives `WM_CREATE` in its own procedure**;
2. creates its **MDI client**;
3. loads its **accelerator table**;
4. `ShowWindow(0x0140, ...)` and `UpdateWindow(0x0140)` — **it shows its main window**;
5. asks where Windows is, and builds `C:\WINDOWS\SYSTEM.INI`;
6. sends `WM_MDICREATE` to the MDI client, which **makes an `mpchild` window and runs its
   window procedure**, which **creates an `EDIT` control** and stores its handle in the
   child's own extra bytes;
7. opens the file, sizes it, asks the control for its text handle (`EM_GETHANDLE`) — which
   **the host gets by calling the guest's own `KERNEL.5 LocalAlloc`** — grows it,
   **reads the file into it**, and hands it back with `EM_SETHANDLE`;
8. **does all of that four times** — `SYSTEM.INI`, `WIN.INI`, `CONFIG.SYS`,
   `AUTOEXEC.BAT` — and then sits in `GetMessage`;
9. is told 0, which is `WM_QUIT`, and exits.

**Two of the four files are actually in memory**, in blocks the application allocated,
grew, filled and now owns. Nothing has drawn a pixel.

---

## ▶ RESUME HERE

### 1. ★★★★★ THE FRONTIER: THE MESSAGE QUEUE

SYSEDIT's interface is built and its files are loaded. The last thing it does is

```
FUNC=0x6c [USER] args=10b from=0x0ac7:0x0112     -- USER.108 GETMESSAGE
```

at `sysedit seg1:0x010d`, whose very next instructions are `or ax,ax / jne 0x00c6` —
**non-zero keeps the loop, 0 is `WM_QUIT`**. We answer the harness sentinel, so the
application is told to quit and does, cleanly. It is not failing; it is being dismissed.

⇒ **The frontier is a message queue**, and it is the last structural piece of the USER
surface before drawing. What it needs:

1. **A queue per task**, and the two ways in: `SendMessage` (built — it calls the window
   procedure directly and is *not* queued) and `PostMessage` (USER `0x6e`, seen in
   `sysedit seg4:0x014e`).
2. **`GetMessage` / `PeekMessage`** to take from it, and ⚠ `GetMessage` returning 0 must
   keep meaning `WM_QUIT` — an empty queue is *block and yield*, which is what
   `wowsched.h`'s `WowWaitForMsgAndEvent` moment already does.
3. **`DispatchMessage`** — which is `wowcall_enter` into the window's procedure, i.e.
   already built. That is the point of having done the callback first.
4. **Input has to come from somewhere.** There is no keyboard or mouse feeding a Win16
   queue yet, and the host window that would supply it does not exist.

⚠ Do not fabricate a message to keep the loop alive. The 0 is currently *correct* for an
empty queue with no `WM_QUIT` posted — what is missing is the queue, not the answer.

### 1b. ★★ A HOST DEFECT, LOCATED BUT NOT FIXED: CF DOES NOT REACH THE GUEST

Part 7. The oracle has ruled: **stock SYSEDIT opens both 0-byte files with no message
box**, so *"Cannot read this file."* is ours. And krnl386's own `_lread` says how:
`jcxz` sends a zero-length read past `0x4114` — the buffer probe whose `or` **clears CF**
— straight to the `pushf / push cs / call` at `0x4530`, and `0x4549 jae` turns a set CF
into `-1`. Every other read had CF cleared for the guest's own reasons, which is why a
defect that was always there needed an empty file to show itself.

▶ **The next experiment is one instruction wide.** Arm a PM breakpoint at
`krnl386 seg1:0x4549` (linear = krnl386 seg1 csbase + 0x4549) and read CF and AX at the
`jae` that decides:
- **CF set** ⇒ the flags our DOS service returns are being discarded on the way back
  through krnl386's dispatcher, and the fix is to write CF into the image the guest
  actually restores from — ⚠ which must be *found*, not assumed; inventing a frame shape
  is how a host corrupts a stack.
- **CF clear** ⇒ the fault is elsewhere and `_lread` is not the culprit.

⚠ Do not "fix" this by making a zero-length read return something else. The guest's logic
is correct at every step.

### 1c. ⚠ `lParam` for `WM_CREATE` stops being optional soon

An MDI child reads its `MDICREATESTRUCT` back out of `CREATESTRUCT.lpCreateParams`.
SYSEDIT's does not, but the next application will.

### 2. Closed this session

- **The host cannot call 16-bit code.** It can. `src/wow/wowcall.h`, **11 calls in a
  run** — seven window procedures and four `LocalAlloc`s.
- **`MDICLIENT` / `EDIT` do not exist.** They do, as system classes.
- **`LoadAccelerators` fails.** `NotifyWow` (USER `0x217`) is answered.
- **`SendMessage` is unimplemented.** It is a real service, both halves.
- **`WM_MDICREATE` has nowhere to go.** It makes the child and runs its procedure.
- **Window extra bytes.** `Get/SetWindowWord`, bounded by the guest's own `cbWndExtra`.
- **krnl386 `0xd0` is unnamed.** It is `GetWindowsDirectory`, and answering it fixed four
  wrong window titles.
- **The host can only call 16-bit code to deliver a message.** It can call any far
  function, with an argument list, a result sink and a declared return width -- and it
  calls `KERNEL.5 LocalAlloc` for the edit controls' text.
- **`0x040D` is EM_SETHANDLE.** It is **EM_GETHANDLE**; `0x040C` is EM_SETHANDLE. The
  correction is in Part 6 and it moved the wall by one message.

### How to drive it

```bash
ARCHIVE=build/wowruns ./scripts/bmwow.sh              # deploy, run, collect
ARCHIVE=build/wowruns ./scripts/bmwow.sh --no-deploy  # re-run what is on the box
```

- **Two switches, both opt-in, and the frontier needs BOTH:**
  ```bash
  touch /private/tmp/xpshare/wowsched.txt   # the Win16 task scheduler (session 38)
  touch /private/tmp/xpshare/wowcall.txt    # ★ calling 16-bit code   (session 40)
  ```
  Without them you are measuring the baseline, not the frontier.
- ⚠ **The share is not mounted in a fresh session.** Nothing above works until
  `mkdir -p /tmp/xpshare && mount_smbfs -N //guest@192.168.1.29/ntvdmex /tmp/xpshare`,
  and both that and every later write to it need the sandbox disabled. A missing mount
  looks like a missing rig.
- ⚠ SMB writes to `/private/tmp/xpshare` need the sandbox disabled.
- ⚠⚠ **Always re-run with both switches OFF and confirm the baseline.** It MOVED this
  session, by exactly one call with a name (see the headline), so compare against these
  and not against session 39's:

  ```bash
  L=build/wowruns/<the run>.log
  grep -c "WOWBOP 0x51" $L                              # 270
  grep -c SERVICED $L                                   #  45  <- was 44
  grep -c DECLINED $L                                   # 122
  grep -c "UNIMPLEMENTED, STEPPED OVER" $L              #  97  <- was 98
  grep -o "task=0x[0-9a-f]*" $L | sort | uniq -c        # 9 · 222 · 39
  grep -c 0001:229C $L                                  # the WOWEXEC GP box
  ```
  The one-call change is `GetWindowsDirectory` and it is at **line 615** of both logs;
  anything else moving is a regression.
- ★ In a healthy **frontier** run: `grep -c WOWCALL` is **23** (the ON line + eleven
  call/return pairs), `grep -c 'CreateWindow "'` is **8**, `grep -c WM_MDICREATE` is
  **4**, `grep -c EM_SETHANDLE` is **4**, `grep -c NotifyWow` is **1**, and the run ends
  on `★ ExitKernelThunk(0x00000000)` at ~888 KB.
- ⚠ A fault loop still fills the log to its 268 MB cap in seconds. `grep` it; do not open
  it.
- ★ **The stock oracle for a Win16 GUI app** is `scripts/bm/stocksysedit.bat` (copy it to
  the share, then `printf 'exec cmd /c "<RES>\\stocksysedit.bat"\r\n' > control.txt`).
  It drops the IFEO Debugger key, runs SYSEDIT under stock ntvdm, screenshots the desktop
  with `rigshot`, and **restores the key on every exit path** — check
  `stocksysedit_state.txt` says `IFEO Debugger RESTORED` before trusting any later run.
  ⚠ `rigshot list` wrote an empty file (it is a GUI image, so its stdout does not
  redirect); the screenshot is the evidence, not the window list.

### Ruled out — do not re-try

Everything in [session 39's list](session-39.md#ruled-out--do-not-re-try) still holds,
plus:

- **Making `[0x22]` non-zero.** Retired by measurement rather than by argument: the field
  is written by the guest's own procedure now, and it holds a window handle this host
  created.
- **Reusing `dpmi_seg_to_desc` for the callback stub.** It builds a *data* descriptor and
  those three bytes are executed.
- **Recording `NotifyWow`'s `lpResource` for later.** `GlobalUnlock` is the next
  instruction.
- **Treating a `?` in the id-space label as "not USER".** It meant
  "`wow_module_of_sel` could not name this selector", which is true of every runtime
  selector.
- **Inventing a handle for `EM_GETHANDLE`.** The guest hands it straight to
  `LocalReAlloc`/`LocalLock` against its OWN local heap, so only the guest's KERNEL can
  make one. Part 6.
- **Reading a WORD-returning Win16 function's result as DX:AX.** DX is litter; LocalAlloc
  proved it by returning the flags we had pushed in the high word.
