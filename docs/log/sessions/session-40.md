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
3. And with both answered, **`SYSEDIT.EXE` shows its window, updates it, and opens
   `C:\CONFIG.SYS` and `C:\AUTOEXEC.BAT`** — the thing the program exists to do.

And then the other half of the same mechanism went in — `SendMessage` and the default
window procedure a system class needs — and **SYSEDIT builds its entire user interface**:

```
WM_MDICREATE "mpchild" "C:\WINDOWS\SYSTEM.INI" in client 0x0160 -> hwnd=0x0180
  -> WOWCALL into mpchild's own procedure, which creates
     CreateWindow "EDIT" style=0x513000c4 -> hwnd=0x01a0
... four times: SYSTEM.INI, WIN.INI, CONFIG.SYS, AUTOEXEC.BAT
```

**Four MDI children, each with its own EDIT control, each child's window procedure run by
this host.** It stops at `EM_SETHANDLE` (`0x040D`) — the message that hands the edit
control the text — because an EDIT control's behaviour is the 32-bit side's, and the
32-bit side is us. See [▶ RESUME HERE](#-resume-here).

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

## ★★★★ WHERE IT ACTUALLY GETS TO NOW

With `wowsched.txt` **and** `wowcall.txt` armed, one run:

| | |
|---|---|
| WOW32 BOPs | **596** (baseline 270) |
| serviced / declined / unimplemented | **129 / 290 / 159** |
| task histogram | `9 · 225 · 203 · 159` — **four**, the last being SYSEDIT's |
| 16-bit calls made by the host | **7**, all returned |
| windows created | **8** — 2 WOWEXEC, `mpframe`, `MDICLIENT`, and 4 `EDIT` controls |
| MDI children | **4** |
| ends at | `★ ExitKernelThunk(0x00000000)`, 885 KB |

and in the middle of it, `SYSEDIT.EXE`:

1. creates its frame window and **receives `WM_CREATE` in its own procedure**;
2. creates its **MDI client**;
3. loads its **accelerator table**;
4. `ShowWindow(0x0140, ...)` and `UpdateWindow(0x0140)` — **it shows its main window**;
5. asks where Windows is, and builds `C:\WINDOWS\SYSTEM.INI`;
6. sends `WM_MDICREATE` to the MDI client, which **makes an `mpchild` window and runs its
   window procedure**, which **creates an `EDIT` control** and stores its handle in the
   child's own extra bytes;
7. opens the file, sizes it, and sends the control `EM_SETHANDLE` (`0x040D`);
8. gets 0 from our default procedure, and says
   `"C:\WINDOWS\SYSTEM.INI\nCannot open this file."`;
9. **does all of that four times** — `SYSTEM.INI`, `WIN.INI`, `CONFIG.SYS`,
   `AUTOEXEC.BAT` — and exits.

Step 8 is the program being **right**, one whole layer deeper than it was this morning.
Nothing has drawn a pixel.

---

## ▶ RESUME HERE

### 1. ★★★★★ THE FRONTIER: THE `EDIT` CONTROL'S OWN BEHAVIOUR

```
-> SERVICED (USER), returned 0x00000000 -- default procedure: msg 0x040d not implemented
```

`0x040D` is `WM_USER + 13`, sent by SYSEDIT to the `EDIT` control it made, immediately
after it has opened the file and sized it, and with a local memory handle as its
parameter. It is the message that hands an edit control its text. Answered 0, SYSEDIT
reports *"Cannot open this file."*, which is correct — it has been told the control did
not take the text.

**And this is a different KIND of frontier from everything before it.** Every wall this
session was a *call* that was missing. This one is a **control**: `EDIT` under WOW is the
32-bit side's, so implementing it means implementing an editable text control — storage,
selection, the `EM_*` message surface — and, before very long, **pixels**, which is the
one thing this project still has none of.

What to do first, in order, and none of it is a guess:

1. **Read what SYSEDIT actually sends.** `sysedit seg3` is the file-loading routine and
   it is short: `OPENFILE` at `0x00cf`, two `_LLSEEK`s, `SENDMESSAGE` at `0x011b`,
   `LOCALREALLOC`, `_LCLOSE`, `LOCALLOCK`, `_LREAD`, `LOCALUNLOCK`. That sequence is
   `EM_GETHANDLE`/`EM_SETHANDLE` around a local block the program grows and fills itself
   — so the control's *text storage* is a Win16 local handle the guest owns, not
   something the host has to allocate. Read the exact messages off those call sites
   before implementing anything.
2. **Decide what an `EDIT` control IS in this host**, the way `wowuser_win_t` decided what
   a window is: an object with real content and a synthetic handle. That is a decision
   worth writing down before code, because it is the first host-side *control*.
3. ⚠ **`lParam` for `WM_CREATE` stops being optional soon.** An MDI child reads its
   `MDICREATESTRUCT` back out of `CREATESTRUCT.lpCreateParams`; SYSEDIT's does not, but
   the next application will.

### 2. Closed this session

- **The host cannot call 16-bit code.** It can. `src/wow/wowcall.h`, 7 calls in a run.
- **`MDICLIENT` / `EDIT` do not exist.** They do, as system classes.
- **`LoadAccelerators` fails.** `NotifyWow` (USER `0x217`) is answered.
- **`SendMessage` is unimplemented.** It is a real service, both halves.
- **`WM_MDICREATE` has nowhere to go.** It makes the child and runs its procedure.
- **Window extra bytes.** `Get/SetWindowWord`, bounded by the guest's own `cbWndExtra`.
- **krnl386 `0xd0` is unnamed.** It is `GetWindowsDirectory`, and answering it fixed four
  wrong window titles.

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
- ⚠ SMB writes to `/private/tmp/xpshare` need the sandbox disabled.
- ⚠⚠ **Always re-run with both switches OFF and confirm the baseline**, which is
  unchanged from session 39:

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
- ★ In a healthy **frontier** run: `grep -c WOWCALL` is **15** (the ON line + seven
  call/return pairs), `grep -c 'CreateWindow "'` is **8**, `grep -c WM_MDICREATE` is
  **4**, `grep -c NotifyWow` is **1**, and the run ends on
  `★ ExitKernelThunk(0x00000000)` at ~885 KB.
- ⚠ A fault loop still fills the log to its 268 MB cap in seconds. `grep` it; do not open
  it.

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
