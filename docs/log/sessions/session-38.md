# Session 38 — the `0001:229C` wall is down: WOWEXEC registers a window class

**Date:** 2026-08-31 · **Branch:** `m9/completeness` · **Issue:** [#128](https://github.com/MrMatthewLayton/ntvdmex/issues/128)

---

## ★★★★ THE HEADLINE: WOWEXEC RUNS PAST `LoadCursor`, AND WE ARE THE SCHEDULER

The fault that ended session 37 — *"WOWEXEC caused a General Protection Fault in module
KRNL386.EXE at 0001:229C"* — **is gone**, and it was never a value that needed fixing. It was
an **ordering** defect: krnl386 has no scheduler, so the task it launched never gave control
back, its own boot task never returned from `LoadModule`, and the bring-up record that
`GetExePtr(NULL)` matches was never unlinked. With a ~70-line cooperative scheduler in the
host, on real hardware:

```
task histogram   9 task=0x00000000     before the bring-up record exists
                222 task=0x000001ef    krnl386's boot task
                  1 task=0x000003b7    the launch BOP
                  3 task=0x000001ef    ★ the creator, resumed -- LoadModule returns
                 28 task=0x000003b7    ★ WOWEXEC, restarted after the creator retires
```

and WOWEXEC's own code says where it got to:

```
0812  push 0 / push 0 / push 0x7f00
0819  lcall LoadCursor      ; ★★ THE OLD WALL -- it RETURNS now
081e  mov [bp-0xc],ax       ;    and the cursor handle is stored
0823  lcall <stock object>  ;    WOW32 0x57, the last call of the run
0828  mov [bp-0xa],ax
082b  mov word [bp-4],0x82  ; ★ style
0830  mov [bp-2],ds         ; ★ hInstance
083b  lea ax,[bp-0x1a] / push ss / push ax   ; ★ &WNDCLASS
```

**WOWEXEC is filling in a `WNDCLASS`.** No Win16 fault, no fatal dialog, 263 WOW32 calls.

⚠ **The scheduler is OPT-IN** (`wowsched.txt` on the share) and a run without it is
byte-for-byte the committed behaviour — 258 calls, the same histogram, the same GP fault,
re-measured after every change. DOS batteries 209/209.

★ **And then two host defects fell** (below): a read-only string literal handed to
`GetProfileIntA`, which was killing the run at `0xc0000005` in `ntdll`, and — much more
serious — **dispatching WOW32 calls on the id alone when the id space is per module**, which
had us answering WOWEXEC's `RegisterClass` with `GetProfileIntA`. With both fixed WOWEXEC
makes **1112 calls** and reaches `RegisterClass` honestly.

⚠ **The new frontier is USER's own thunk table.** `RegisterClass` returns 0 because it is
not implemented, WOWEXEC takes its error path at `wowexec:0x0849` (`push 0x8f`), the task
exits, and krnl386 relaunches it — ~155 times until the log cap. That loop is the correct
shape of "the window class cannot be registered yet".

### ★★★ AND THEN TWO HOST DEFECTS, BOTH OF THEM OURS

**1. We handed Windows a read-only string literal.** With the scheduler on the run died
host-side, deterministically, at `0xc0000005` in `ntdll` — `mov [ecx+eax],bl`. A frame walk
added to the fatal dump gave `ourbase=0x0f000000`, which turned two "random" numbers into
addresses in our own image: the caller at `+0xc3bf` and, in `EAX`, the *target of the write*.
`objdump` at that address:

```
f00c39f:  mov  $0xf09a9f6,%eax      ; a string literal
f00c3aa:  cmove %eax,%ecx           ; hk ? key : ""
f00c3af:  cmovne %esi,%eax          ; ha ? app : ""
f00c3b9:  call *...                 ; GetProfileIntA(app, key, def)
```

XP's profile code **writes to the name buffers it is given**, and `""` lives in a read-only
page. It survived ten calls in every earlier run because all of them had both names
non-empty; the scheduler simply let the guest get as far as asking with one missing. Both
`0x39` and `0x80` now pass writable buffers. ⇒ WOWEXEC went from **28 calls to 1112**.

⚠ And the log could not name the crashing call: the `WOWBOP` line is accumulated and only
flushed *with its result*, so a crash inside a service loses the header too and the log ends
at the last call that **succeeded**. My first reading blamed `0x57`, which was fine. The fatal
dump now records the last id entered, and says that it may have completed.

**2. ★★★ THE ID SPACE IS PER MODULE, AND WE WERE DISPATCHING ON THE ID ALONE.**

WOWEXEC's `RegisterClass(&WNDCLASS)` arrives as **id `0x39`** — `retstub=0x0c25`, **4** argument
bytes, stub in USER's segment. krnl386's `0x39` is `GetProfileInt` — `retstub=0xb537`, **10**
argument bytes. We serviced the first with `GetProfileIntA` and handed WOWEXEC the answer:
**a function answered by an unrelated function**, the "runs but lies" class this project
treats as the most expensive kind. Every id in `wow32.h` was read out of krnl386; USER, GDI
and the drivers have their own tables reaching the same BOP with their own numbering.

The discriminator is exact and needs nothing external: **the BOP lives inside krnl386's own
common thunk (`seg1:0x2bf1`), so the executing `CS` at a BOP *is* krnl386's code segment, and
a stub in that same segment is krnl386's stub.** Every service, every decline and the
scheduler hooks are gated on it, and the log prints `[krnl]` or
`[FOREIGN TABLE, id is NOT krnl386's] stub=0x....` instead of putting krnl386's name on
something it has not identified.

⚠ The first attempt gated on `g_wow_pmbase[]`, which is filled by a descriptor-limit match
that has not fired when the earliest calls arrive — **the baseline collapsed from 258 calls to
3**. Measured, not reasoned about, and that is the only reason it was caught.

★ **The gate changes nothing that worked**: baseline 42 serviced / 114 declined / 97
unimplemented, before and after, identical. All 45 foreign calls in a baseline run were
already unimplemented. It only stops the wrong answer.

### How the scheduler works — and why it is small

`src/wow/wowsched.h`. Three moments, and it never invents a frame:

| | |
|---|---|
| **(A) at WOW32 `0x74`** | the launch. Save the guest context, answer with the new task's instance handle, and set the epilogue mode to **25** — so the creator goes home through `seg1:0x9827` and the new task is parked at its launch instruction. |
| **(C) at `[0x228] == 0`** | the creator has retired (`seg1:0xcd41`) and the first code to touch the current task takes a null-selector `#GP`. That fault is the cue: restore the parked context with mode **0**, and krnl386 runs the `seg1:0x985c` continuation and `iret`s into the task exactly as it always did. |

The context is a `0x40`-byte copy of the TIB register block (`VTIB_GS`..`VTIB_SS`); krnl386's
own thunk epilogue pops `di, si, ax, dx, ds, gs, fs, cx, es, bx` back off the frame, and its
re-entrancy guard (`seg1:0x2c05`) notices `[0x228]` is 0 and makes the resumed task current
by itself. **The host chooses; krnl386 does the work.**

### ⚠ Two things measured the hard way

- **(B) does not exist, and the first cut assumed it did.** The obvious ordering — let the new
  task run first, hand the creator back at the task's `WaitEvent` — **faulted in
  `SwitchToTask`**, because *the launch frame's memory is the new task's own stack*. The
  epilogue pops it, SP moves up, and the task pushes straight back over it; by `WaitEvent` the
  frame is gone. Switching at `0x74` is the only ordering that works — and it is also the one
  that leaves the new task's stack **frozen and untouched** while the creator runs, which is
  what makes resuming it sound rather than lucky.
- **The launch result is read, not guessed.** `TDB+0x1c` already holds `0x03d6` at the `0x74`
  call, so the host reads it. There is a fallback (`SS & ~1`, Win16's `SS == DS` invariant,
  which the stock oracle shows twice and our own task a third time) and it is **verified at
  the resume and logged**, because an earlier probe hardcoded `0x03d6` and that is exactly the
  kind of thing that stops reproducing.
- ⚠ **It truncates the creator.** At (C) the boot task had retired but was still freeing
  selectors; we take the machine away and those leak. The honest fix is to park the creator
  too and give it the rest of its turn. Logged as the truncation it is.

---

**The rest of this document is how the wall was understood, in the order it happened.**

**In one paragraph.** Two things. First, **WOW32 `0x7d` is pinned and shipped**: the
furthest point this project has reached had been depending on a line in a text file
(`wow32ret.txt` had to contain `7d 00000001`), and that line is now a service, derived from
krnl386's own code rather than guessed. Second, **session 37's fork is resolved, and the
branch everyone would have taken is the wrong one.** `TDB+0x1c` — the instance handle whose
being zero makes `GetExePtr(NULL)` match krnl386's own bring-up record and return `0xFFFF`
into `ES` — has **exactly one writer in the whole of krnl386**, and it is inside `InitTask`,
which the bring-up record never goes through. So its `hInstance` is zero on real Windows
too, "it should be `0x001e`" cannot be right, and there is nothing to fix there. What is
wrong is that the record is still **in the list**: the boot task must resume and unlink
itself (`seg1:0xcd36`) before WOWEXEC reaches `LoadCursor`, and it never gets the chance.

---

## ★★★ WOW32 `0x7d`: approve the selector that becomes a task database

Not named by the export table, so it comes from its call sites — both of which are inside
**one** function, `seg2:0x2984`, and that function is krnl386's TDB creator:

```
2984  enter 4,0                      ; the whole of it
29b5  mov si,0x100                   ; room for a PSP
29d5  add si,0x223 / and si,0xfff0   ; -> 0x320
29e6  lcall seg1:0x4e81              ; GlobalAlloc(that many bytes)
29f6  push ax / lcall 0xb397         ; 0x7d: "is THIS one acceptable?"
29fc  or ax,ax / je 0x2a04           ; no -> the retry loop
2c02  mov word ptr [0xfa],0x4454     ; "TD", the TDB signature
```

`0x320` is exactly limit+1 of every task database in the stock panel.

### The retry loop is what pins the semantics

On a `0` the caller does **not** go and allocate different memory. It calls `seg1:0x574d`,
which is `AllocSelector`: `lsl ecx,<sel>` for the limit, take LDT entries, copy the source
**descriptor** onto them, `or si,7`. Every retry therefore offers an **alias of the same
bytes**, and the only thing that differs between one attempt and the next is the *numeric
value of the selector*. The question can only be "may this selector value be a task
handle?", and the rejects are freed again (`seg1:0x5a53`) as soon as one is accepted.

### And the answer is the selector, not a boolean

`seg2:0x29fe` merely tests it, but `seg2:0x2a22` does `mov si,ax` and si goes on to **be**
the TDB selector (`mov es,ax`, then the block is zeroed through it). A 32-bit companion
cannot conjure an LDT selector, so the only value it can return is one it was offered:
**echo the argument**. The `1` the experiment answered was right only because the first call
is accepted and the second path is never taken; it would install `0x0001` as a task's
selector if it were.

### It names itself in the run

```
FUNC=0x0000007d AcceptTaskSelector? ... from=0x000001d7:0x000029fc (000003b7)
  -> SERVICED, returned 0x000003b7
INT31h AX=0x000004f1 BX=0x000003b7 CX=0x00000001 -> private-alloc 00000001 sel 0x000003bf
  LDTSYNC idx 0x00000077 <- guest wrote base=0x0002df00 limit=0x0000031f acc=0xf3
```

`limit = 0x31f`. The selector `0x7d` approved is the `0x320`-byte task database, and the
stack at the call carries `0x320` and `0x32` (its paragraph count). `wow32ret.txt` now
ships **empty**, and `bmwow.sh` printing `0 override(s)` is the correct state.

⚠ The endpoint is unchanged — WOWEXEC loads, runs, and krnl386 reports the same GP fault —
at **258** WOW32 calls against the **265** of the last recorded run. There is no archived
log of that run to diff, so the difference is reported rather than explained.

---

## ★★★ THE FORK IS RESOLVED, AND BRANCH (b) IS REFUTED

Session 37 left two readings of why `GetExePtr(NULL)` returns `0xFFFF`:

> **(a)** real `LoadModule` returns before the new task runs — an ordering bug of ours, or
> **(b)** it also switches, and the bring-up record's `hInstance` must be non-zero by then
> (expected `0x001e`).

### `TDB+0x1c` has exactly one writer in the entire binary

A scan of every segment for a word store to displacement `0x001c` (`a3 1c 00`, every
`89 /r 1c 00`, every `8c /r 1c 00`, `c7 06 1c 00`) finds **one** site: `seg2:0x2e02`, and it
is inside `InitTask`:

```
2dd9  mov ds,[0x22a]              ; the task being started
2ddd  cmp word ptr [0xfa],0x4454  ; "TD" or bail
2ded  mov [4],dx                  ; TDB+0x04 = SS
2df7  mov [2],ax                  ; TDB+0x02 = SP
2dfa  push [bp+0xc] / call 0x1645 ; the module handle -> its DGROUP
2e02  mov [0x1c],ax               ; ★ TDB+0x1c = hInstance
```

The bring-up record is not built by that path — it is installed wholesale at `seg1:0xc51c`,
where `[0x228]` (current task) and `[0x226]` (list head) are both set to it directly. So
**nothing in krnl386 would ever write its `hInstance`**, on our machine or on a real one.
Branch (b) is not a thing we can fix; it is not a defect.

★ And session 37's expected value does not survive either. It came from the stock pattern
`hInstance == SS` bar the low bits (`SS=0x16bf / hInst=0x16be`, `SS=0x03af / hInst=0x03ae`),
which holds because Win16 tasks have `SS == DS` seen through two aliases. The bring-up
record's `SS = 0x001f` is **our host's entry stack**, not a Win16 DGROUP, so `0x001e` is a
number with nothing behind it. Do not aim a fix at it.

### What is actually in the task list — measured, because the dump could not say

The fault dump leaves `ES` on the boot record with `next = 0`, which is equally consistent
with a **one**-element list and with the boot record being the **second** of two. Those are
different defects. Two repeating breakpoints settle it:

| site | what it is |
|---|---|
| `seg1:0x2225` | `cmp ax, es:[0x1c]` — the compare inside `GetExePtr`'s walk. `ECX` is the cursor, so its first value per call is the **head**. |
| `seg1:0x9a16` | `mov [0x226], es` — the only writer of the head other than the boot install at `seg1:0xc51c`. |

```
before the launch   0x2225  ECX=0x000001ef     <- head is the boot record
                    0x9a16  fires once          <- a task is linked
after the launch    0x2225  ECX=0x000003b7     <- head is WOWEXEC's TDB
```

So the list is `0x03b7 -> 0x01ef -> 0`. **WOWEXEC's task is linked, at the head**, by the
priority-ordered insert at `seg1:0x99ed` (the key is the signed byte at `TDB+0x08`).
`GetExePtr(NULL)` skips it — WOWEXEC's `+0x1c` is `0x03d7`, its real instance handle — and
matches the record behind it.

⚠ I had inferred from the fault dump that WOWEXEC's TDB was **never linked**, and built
this instrument to confirm it. It refuted it. The dump was never evidence either way.

### Therefore: the boot record has to leave, which means the boot task has to run

krnl386 unlinks its own boot record at `seg1:0xcd30/0xcd36`, after `LoadModule` returns and
after it reads `[boot] 386GRABBER`. On our machine `LoadModule` never returns, because the
new task runs to its fault inside it. That is branch (a), and it is the only branch left.

---

## ★★ The task launch is 16-bit, and it is now mapped end to end

This matters because it says the switch is **krnl386's**, not something WOW32 performs for
it — so it is ours to observe rather than ours to invent.

```
seg1:0x97c2  push bp
       97c3  mov di,ss / mov cx,sp        ; park the CREATING task's stack in registers
       97e9  mov ss,[bp+8] / mov sp,si    ; ★ switch to the new task's stack
       97ee  mov [0x228],es               ; current task = the new TDB
       97f9  mov es,es:[0x1e]             ; the module handle out of the TDB
       97fe  mov ax,es:[0x3e]             ; its expected Windows version
       980b  push es,es,es:[0x26],es,es:[0xa],dx,ax
       981e  push cs / push 0x985c        ; the return address
       9822  jmp 0xb1d0                   ; ★ WOW32 id 0x74
       985c  add sp,0xc / xchg shuffling
       986e  pop dx,bx,es,cx,ax,di,si,ds  ; ★ the Win16 entry frame
       9876  call 0x99b6                  ; -> WOW32 id 0xd2
       9879  iret                         ; ★ ENTER THE TASK
       9827  mov ss,di / mov sp,cx        ; the switch back
```

The `pop` block at `0x986e` is exactly the entry frame session 37 measured on WOWEXEC
(`DI = hInstance`, `SI = hPrev`, `ES = PSP`, `BX = 0x2000`, `CX = 0x0800`), and the `iret`
is the transfer. Two IDs get readings out of it, from their arguments rather than a guess:

| id | reading | evidence |
|---|---|---|
| `0x74` | per-task-launch notification carrying `wExpWinVer` | the run logs `(0000030a 00000000)`, and `0x030a` is Windows **3.10**, read out of the module's `ne_expver` at `es:[0x3e]` two instructions earlier |
| `0xd2` | "about to enter the task" | called from `seg1:0x99c3`, three instructions before the `iret` |

Neither is implemented; both are stepped over and answered `0`, and the launch proceeds, so
neither is load-bearing for *reaching* WOWEXEC.

---

## `GetExePtr` in full, since the wall is inside it

`seg1:0x2200`, five strategies in order:

```
2207  test ax,1 / cmp es:[0] == 'NE'     ; already a module handle?
2221  walk the task list, match TDB+0x1c ; ★ a NULL argument matches here
2233  return TDB+0x1e                    ; the module handle -> 0xFFFF
223a  call 0x48b4 -> 'NE'?               ; the block's owner
224d  call 0x6436 -> 'NE'?
2265  walk the task list, match TDB+0x60 ; never reached with ax = 0
```

and the caller, `GetExpWinVer` at `seg1:0x228a`, does `or ax,ax / je 0x22ab` — so a correct
`0` is handled, and only a non-zero non-selector is fatal:

```
229c  mov es,ax        ; ax = 0xFFFF  ->  #GP, err=0xfffc
229e  mov ax,es:[0x3e]
```

`bytes@fault = 8e c0 26 a1 3e 00` and `eax = 0x0000ffff` in the fault record: the
disassembly and the hardware agree to the byte.

---

## ★★★ krnl386 HAS NO SCHEDULER. WE ARE THE SCHEDULER.

This is the finding that says what the remaining work *is*. Every Win16 scheduling
primitive is a **pure exported pass-through** to WOW32 — a stub that BOPs and returns, with
no 16-bit body and **no near call, near jmp, short jmp, far call or far jmp to it anywhere
in the binary**:

| export | id | stub | reached from krnl386 |
|---|---|---|---|
| `Yield` | `0x1d` | `seg1:0xb503` | nothing |
| `OldYield` | `0x75` | `seg1:0xb510` | nothing |
| `DirectedYield` | `0x96` | `seg1:0xb592` | nothing |
| `WaitEvent` | `0x1e` | `seg1:0xb578` | nothing |
| `PostEvent` | `0x1f` | `seg1:0xb56b` | nothing |
| `SetPriority` | `0x20` | `seg1:0xb585` | nothing |
| `LockCurrentTask` | `0x21` | `seg1:0xb59f` | nothing |

krnl386 keeps the *state* — the task list, and each parked task's `SS:SP` at `TDB+0x02/+0x04`
— and hands every scheduling **decision** to the 32-bit side. On real WOW that side runs each
16-bit task on its own Win32 thread and blocks it. We answer all seven with the harness
sentinel and return immediately, which is why a task, once entered, never gives control back.

★ **WOWEXEC's startup names the handshake.** Its entry is the textbook Win16 `__astart`:

```
11b7  xor bp,bp / push bp / lcall InitTask
11c9  save cx,si,di,bx,es,dx into [0xe0..0xea]   ; the entry frame
11e1  xor ax,ax / push ax / lcall WaitEvent(0)   ; ★ from=0x03cf:0x11e9 in the log
11e9  push [0xe4] / lcall InitApp(hInstance)
11f6  push hInstance,hPrev,PSP,...,nCmdShow / call the WinMain wrapper
```

`WaitEvent(0)` sits exactly between `InitTask` and `InitApp`, which is where a new task hands
control back to its creator.

### ⚠⚠ THE SAME MISTAKE THREE TIMES: A SEARCH THAT CANNOT EXPRESS ITS TARGET RETURNS ZERO

The third is further down — I dumped **sixteen** entries of a **thirty-eight** entry jump
table and concluded the switch-back "is not in it". Every one of these produced a confident
negative finding, and two of them reached a commit before I caught them. **When a scan comes
back empty, the first question is what encodings it can see — not what the guest does.**

- **`nedis.py --callers` counted `call` only.** It reported "0 caller(s)" for WOW32 `0x74`,
  which reads as *nothing calls this* — and `0x74` **is** called, from `seg1:0x9822 jmp
  0xb1d0`, preceded by `push cs / push 0x985c`: a hand-built far call whose return address is
  deliberately not the next instruction. That shape is normal here, not an oddity. The scan
  now takes `e9`, `eb` and `ea` as well, and the zeros in the table above were re-measured
  with the fixed one.
- **And then the SAME MISTAKE AGAIN, twenty minutes later.** I wrote up "the creating task
  resumes at `seg1:0x9827`" as **refuted**, on the strength of a scan of every segment for
  the *word* `0x9827` that came back empty. A near jump encodes a **relative displacement**,
  not the target — so the scan could not have found it however many times I ran it. It is
  reached, from `seg1:0x2c61`, and the section below is what it does. Twice in one session,
  the same shape: *a search that cannot express the thing it is looking for reports zero, and
  zero reads like an answer.*

---

## ⚠⚠ THE THUNK'S TASK CHECK IS A RE-ENTRANCY GUARD, NOT A SCHEDULING REQUEST

**Read this before the section below it, which was written first and is wrong where it says
so.** I read the thunk's `cmp ax,[0x228] / jne` as *"the 32-bit side may change the current
task and krnl386 will switch"*, and wrote it up as the host's scheduling lever — one word to
write, no context save needed. Re-reading `seg1:0x98ab` with the argument in hand inverts it:

```
2c04  pop ax          ; ax = the PRE-BOP [0x228] -- i.e. THE CALLER'S OWN TASK
2c05  cmp ax,[0x228]
2c09  jne 0x2c4b  ->  jmp 0x98ab      ; entered with AX = the caller
...
98bf  mov es,[0x228]                  ; the POST-BOP value is the OUTGOING task
98cb  mov es:[4],[0x6a4]              ; park ITS stack
98dc  pop ds / mov di,ds              ; ds = di = AX = the caller  -> the INCOMING task
9902  mov es:[0x228],di               ; ★ current task = THE CALLER, again
```

The incoming task is **the caller**. So the sequence is not "you asked for task B, here it
is" — it is *"someone else became current while you were in the 32-bit side; park them and
put me back."* That is a re-entrancy guard for a world where another task really can run
during a WOW32 call, and `[0x6a4]/[0x6a6]` hold whichever thunk frame parked itself last.

⇒ **Writing `[0x228]` from the host does not yield.** It would park the wrong task's stack
into the wrong TDB and then undo itself. Do not build on it.

⇒ And it pushes the evidence back towards **tasks being threads** on real WOW: the guard only
earns its keep if a *concurrent* task can leave itself current. Which means the host's part
is a context save/restore after all — the thing the section below claims is unnecessary.

⚠ **And the switch-back is not reached from that path either.** The epilogue is chosen by
`2c0d cmp bx,0 / 2c10 jne 0x2c32` and then `2c3f add bx,bx / 2c41 jmp word ptr cs:[bx+0x2a36]`
— a word table which reads

```
bx= 0 -> 0x2c12   1 -> 0x2c46   2 -> 0x2fba   3 -> 0x2c6f   4 -> 0x2c86 ...
```

and **`0x2c4e` — the entry that jumps to `0x9827` — is not in it.** `bx` is the word pushed
by `2bc7 push 0`; `seg1:0x98ab` reads that same word (`9932 mov bx,sp / 9934 cmp ss:[bx],1`)
but never changes it, and returns to `2c0b jmp` where it is popped as `0`. So on the
task-changed path `bx` is zero, the table is skipped, and control goes to the ordinary
epilogue at `0x2c12`.

⇒ **How `seg1:0x2c4e` is ever entered is unknown.** It is not a table entry, nothing jumps or
calls to it, and the byte before it (`0x2c48 cc`) is an `int3` in the middle of straight-line
code — which smells like a site krnl386 patches at run time. **Answer that before writing any
scheduler code**; it is the only route to the switch-back, and the switch-back is the only
thing in the binary that puts a creating task back on its own stack.

---

## ~~AND krnl386 SAYS HOW TO SCHEDULE IT: CHANGE `[0x228]`~~ — SUPERSEDED, see above

The common WOW32 thunk, `seg1:0x2bb6`, is not a plain call gate. It brackets every BOP with
task bookkeeping:

```
2bc9  mov ds,cs:[0x30]         ; ★ DS is krnl386's DGROUP at EVERY WOW32 BOP
2bce  push [0x228]             ; ★ remember which task is making this call
2bd2  mov di,ss
2bd4  mov [0x6a4],di           ; ★ park this frame's SS
2bd8  mov [0x6a6],bp           ;    and BP
2bf1  C4 C4 51                 ; the BOP
2c04  pop ax
2c05  cmp ax,[0x228]           ; ★★ DID THE CURRENT TASK CHANGE ACROSS THE CALL?
2c09  jne 0x2c4b               ; ★★ yes -> switch
2c0b  ...                      ; no  -> the ordinary epilogue
```

`seg1:0x2c4b jmp 0x98ab` is `SwitchToTask`, and it does exactly what the name implies:

```
98ab  inc [0x24b]                    ; switch depth
98bf  es = [0x228]                   ; the outgoing task
98cb  ax=[0x6a4] / es:[4]=ax         ; ★ TDB+0x04 = its SS, from the parked word
98d2  ax=[0x6a6] / sub ax,0x10
98d8  es:[2]=ax                      ; ★ TDB+0x02 = its SP
98e7  cmp es:[0xfa],0x4454           ; "TD" -- only for a real task database
9902  es:[0x228] = di                ; ★ current task = the incoming one
9923  ...                            ; per-task bookkeeping
9964  jmp 0x2c0b                     ; back into the thunk's epilogue
```

and the epilogue's jump table (`2c41 jmp word ptr cs:[bx+0x2a36]`) reaches `seg1:0x2c4e`,
which pops the saved registers, **zeroes the return-value hole at `[bp-0x18]`**, and

```
2c61  jmp 0x9827                     ; -> mov ss,di / mov sp,cx
```

— the launcher's switch-back, with `DI:CX` still holding the creating task's `SS:SP` because
the thunk saved and restored them across the BOP. That is what `seg1:0x9827` is for, and why
`0x9825/0x9826` are NOPs: it is the tail of a routine that is *jumped into*, not fallen into.

### The one part of that which SURVIVES

~~We do not need to save or restore CPU contexts.~~ — refuted above.

What does survive, and is worth keeping, is the **addressing**: `seg1:0x2bc9` sets `DS` to
DGROUP two instructions before the BOP, so at **every** WOW32 BOP the guest's own `DS`
selects krnl386's data segment (the run logs `ds=0x000001e7` on every one of them). So
`[0x228]` — the current task — and `[0x6a4]/[0x6a6]` — the last parked frame — are readable
by the host at every call with no new plumbing and no dependence on a base that moves.
**Logging `[0x228]` per BOP turns the whole run into a task timeline**, and that is the
cheap next instrument regardless of which scheduling reading turns out to be right.

---

## ★★★ THE GATE IS ANSWERED: THE THUNK HAS 38 RETURN PATHS, AND WE PICK ONE

`seg1:0x2c4e` **is** in the epilogue table — at index **25**. I had dumped sixteen entries of
a thirty-eight entry table and written "not in it". *(Third time this session; see the method
note below.)* The full picture:

```
2bc7  push 0                        ; ★ THE MODE WORD, at bp-24
2bf1  C4 C4 51                      ; the BOP
2c0b  pop bx                        ; ★ read it back
2c0d  cmp bx,0 / jne 0x2c32         ; 0 = the ordinary epilogue
2c41  jmp word ptr cs:[bx+0x2a36]   ; ★ 38 entries, 0x2c12 .. 0x2f43
```

★ **krnl386 itself never sets it.** A scan of every segment for a store to `[bp-0x18]` finds
one site inside the thunk — `seg1:0x2c5c`, which *clears* it. Thirty-seven epilogues the guest
can never select are not dead code; **they are a menu for the other side.**

★★ **Mode 25 is the task switch-back, and it pairs with the launcher instruction for
instruction.** `seg1:0x97be` (the launcher's real entry — `--callers 0x97c2` found nothing
because the function starts four bytes earlier):

```
97be  push [0x228] / push bp     ; on the CREATOR's stack
97c3  mov di,ss / mov cx,sp      ; its stack, kept in registers
97e9  mov ss,[bp+8] / mov sp,si  ; switch to the new task
9822  jmp 0xb1d0                 ; WOW32 0x74, through the thunk -- which pushes
                                 ;   DI and CX into its frame on the NEW stack
```

and mode 25 lands at `seg1:0x2c4e`, which pops that frame (DI and CX come back) and jumps to

```
9827  mov ss,di / mov sp,cx / pop bp / pop [0x228]
```

— the creator back on its own stack, its `BP` restored, **and current again**. The two are a
matched pair: `push [0x228] / push bp` at one end, `pop bp / pop [0x228]` at the other.

### MEASURED ON HARDWARE, in two runs

A new knob, `wowmode.txt` (`<hex id> <hex mode>`), returns one id through a chosen epilogue and
logs every use as an experiment. With `74 19`:

| | result |
|---|---|
| **mode 25, answer `0` (the sentinel)** | control returns to task `0x01ef` — **the creator, on its own stack** — WOWEXEC never runs, and the `0001:229C` GP fault is **gone**. krnl386 then says, in its own words, *"NTVDM KERNEL: Missing 16-bit system module … WOWEXEC.EXE"* and calls `ExitKernelThunk(1)`. ⇒ **the return value at that call is the launch result**, and `0` means failure. |
| **mode 25, answer `0x03d6`** (WOWEXEC's measured hInstance) | the boot task **runs on**, into two sites no run of this project had ever reached: `seg1:0xcd0b GetProfileInt("KERNEL","EnableEMSDebug")` and `seg1:0xcd30 GetPrivateProfileString("SYSTEM.INI", …, "386GRABBER")` — **the read that sits immediately before the unlink at `seg1:0xcd36`.** |

The checkpoint set earlier in this document is therefore **met**: the boot task can be given
control back, and when it has it, it goes exactly where the disassembly said it would.

### ★★★ AND THE FAULT AT THE END OF IT IS NOT A BUG — IT IS THE BOOT TASK RETIRING

The second run ends in a `#GP` loop, and I assumed that was the fabricated hInstance catching
up with us. It is not. There are **no further WOW32 calls** after the `386GRABBER` read, so
the boot task died inside the very code that read follows — and that code says exactly what
it is doing:

```
cd36  call 0x9a3e             ; ★ THE UNLINK -- take this task out of the list
cd3c  mov es:[0xfa],dx        ; ★ zero the "TD" signature: no longer a task database
cd41  mov [0x228],dx          ; ★★ CURRENT TASK = 0
cd66  mov ss,cs:[0x30]        ; ★ a private kernel stack
cd6b  mov sp,0x210
```

**krnl386's boot task ends itself.** It unlinks its record, unsigns it, sets the current task
to **zero**, and moves onto a fixed kernel stack. From that instruction the machine belongs to
the scheduler — and there isn't one, so the next code to touch the current task,

```
321f  mov es,[0x228]          ; = 0
3223  test word ptr es:[0x18],2
```

dereferences a **null selector**. `#GP` with `err=0x00000000`, which is precisely the error
code in every record of the loop, and krnl386's own handler resumes at the faulting
instruction, hence the **268 MB**.

So the run did not fail early — it ran the boot task **to completion** and stopped at the one
place this host has never had anything to put. That is the frontier stated as a single fact:

> **`[0x228] == 0` means "no task is current; schedule one", and nothing does.**

⚠ The probe is still a probe: WOWEXEC never ran, so its `hInstance` was fabricated and
everything the boot task did with it after `LoadModule` is suspect. What the run establishes
is the *shape* — creator resumes, runs to its own retirement, and the machine falls idle with
a runnable task nobody starts.
★ And the host already logs `[0x228]` on every call, so the cue needs no new instrument.

⚠ **The log cap does not bound a fault loop.** Second time this has produced a
quarter-gigabyte file. Budget for it before setting a knob.

---

## ▶ RESUME HERE

**The scheduler is built and the wall is down** (see the headline). What is left:

1. ~~The host-side crash.~~ **FIXED** — a read-only `""` literal passed to `GetProfileIntA`.
2. **USER's OWN THUNK TABLE.** This is the frontier now. `RegisterClass` is id `0x39` *in
   USER's numbering* (`stub=0x0327`, 4 arg bytes, a far `&WNDCLASS`), and everything in
   `wow32.h` is krnl386's numbering, so none of it applies. The work is to map USER's table
   the way `wowthunks.py`/`wowmap.py` mapped krnl386's — **against `guest/ne/user.exe`, not
   krnl386** — and then implement `RegisterClass`, `CreateWindow` and the rest against it.
   The log already separates the two (`[krnl]` vs `[FOREIGN TABLE] stub=0x....`).
3. **Park the creator instead of truncating it.** At (C) the boot task had retired but was
   still freeing selectors when the host took the machine away. Saving its context there and
   giving it the rest of its turn is a small change and removes a known leak.
4. **A second task.** Everything so far is one launch. `WOW32_WAITEVENT`/`Yield` still return
   immediately, which is right while only one task can run and wrong the moment two can.

### Ruled out — do not re-try (all by measurement)

- **Writing the bring-up record's `hInstance`.** One writer, in `InitTask`, which that record
  never enters. `0x001e` was pattern-matching on a stack selector that is ours.
- **"WOWEXEC's TDB is never linked."** It is linked, at the head, and measured to be.
- **Writing `[0x228]` to yield.** `seg1:0x2c05`'s branch is a re-entrancy guard; its incoming
  task is the caller's own.
- **Letting the new task run first and switching at its `WaitEvent`.** Built, run, faulted:
  the launch frame's memory *is* that task's stack and is gone by then.
- **Looking for a 16-bit scheduler inside krnl386.** There is not one — but there IS a 16-bit
  task *switcher* (`seg1:0x98ab`), driven by the epilogue mode.
- `wow32ret.txt` as a load-bearing file. It ships empty and must stay that way.

### How to drive it

```bash
ARCHIVE=build/wowruns ./scripts/bmwow.sh              # deploy, run, collect
ARCHIVE=build/wowruns ./scripts/bmwow.sh --no-deploy  # re-run what is on the box
```
Knobs on the share, none of them deployed by the script except `wow32ret.txt`:
`wowsched.txt` (presence = scheduler ON), `wowmode.txt` (⚠ the most dangerous file in the
tree), `pmbp.txt`, `pmchg.txt`.
⚠ SMB writes to `/private/tmp/xpshare` need the sandbox disabled.
⚠ **Always re-run with the scheduler OFF** and confirm 258 calls / the `0001:229C` fault /
`9-222-27`: that is the committed baseline and it must not move.

### The instrument log, for the record

1. ~~Instrument first.~~ **DONE — every WOW32 call line now carries `task=0x....`**, read
   from `[0x228]` through the guest's own `DS` (DGROUP there by construction,
   `seg1:0x2bc9`). The whole run collapses to three numbers:

   ```
     9 task=0x00000000     ; before seg1:0xc51c installs the bring-up record
   222 task=0x000001ef     ; krnl386's boot task
    27 task=0x000003b7     ; WOWEXEC -- and never back
   ```

   and the transition is exactly `FUNC=0x74`, which already reports `task=0x03b7` because the
   launcher sets `[0x228]` at `seg1:0x97ee` before calling it. One `uniq -c` now says what
   took this session to establish: **control enters WOWEXEC and never returns to the boot
   task.** Anything about who was running is a grep away from here on.
2. ~~Settle the epilogue question.~~ **DONE, and measured on hardware** — the mode word at
   `bp-0x18`, 38 epilogues, mode 25 = the switch-back. `mode=` is on every log line and reads
   `0` on all 258 calls of a stock run, so the day something sets one, the log says so.
3. **Then the interleave**, per the plan above. `wowmode.txt` is the knob to prototype it
   with; ⚠ it is the most dangerous file in the tree, because a mode selects a code path that
   pops a specific stack shape.
4. **The policy can be read out of the guest** either way: krnl386 links every task at
   `seg1:0x99ed` keyed on the signed priority byte at `TDB+0x08`, so a scheduler can walk the
   list it already maintains rather than invent an order.

The first checkpoint is small and specific: with `WaitEvent(0)` yielding to the creator,
krnl386's boot task should return from `LoadModule`, take `seg1:0xcc6b (cmp ax,0x20)`, read
`[boot] 386GRABBER`, and **unlink its own record at `seg1:0xcd36`** — none of which fires
today. Breakpoint those three and the fix is measurable before WOWEXEC is asked to draw
anything.

⚠ **The return-value hole is zeroed on the switch path** (`seg1:0x2c5c`,
`mov word ptr [bp-0x18],0`), so a switching call's return value is not the host's to choose —
do not try to answer *and* switch on the same call and expect both to land.

### Ruled out this session — do not re-try

- **Writing the bring-up record's `hInstance`.** One writer, in `InitTask`, which that
  record never enters. `0x001e` was pattern-matching on a stack selector that is ours.
- **"WOWEXEC's TDB is never linked."** It is linked, at the head, and measured to be.
- **Looking for a 16-bit scheduler inside krnl386.** There is not one — but there IS a
  16-bit task *switcher* (`seg1:0x98ab`), and it is driven by `[0x228]`.
- ~~"The creating task resumes at `seg1:0x9827`."~~ **This "refutation" was wrong** — see the
  correction above. It resumes there, from `seg1:0x2c61`.
- `wow32ret.txt` as a load-bearing file. It ships empty and must stay that way.

### How to drive it

```bash
PMBP=1 ARCHIVE=build/wowruns ./scripts/bmwow.sh              # deploy, run, collect
PMBP=1 ARCHIVE=build/wowruns ./scripts/bmwow.sh --no-deploy  # re-run what is on the box
```
⚠ SMB writes to `/private/tmp/xpshare` need the sandbox disabled.
