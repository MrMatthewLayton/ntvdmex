# Session 38 — the boot task's `hInstance` cannot be fixed; the boot task has to leave

**Date:** 2026-08-31 · **Branch:** `m9/completeness` · **Issue:** [#128](https://github.com/MrMatthewLayton/ntvdmex/issues/128)

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

### ⚠ An instrument that lied, and one reading refuted

- **`nedis.py --callers` counted `call` only.** It reported "0 caller(s)" for WOW32 `0x74`,
  which reads as *nothing calls this* — and `0x74` **is** called, from `seg1:0x9822 jmp
  0xb1d0`, preceded by `push cs / push 0x985c`: a hand-built far call whose return address is
  deliberately not the next instruction. That shape is normal here, not an oddity. The scan
  now takes `e9`, `eb` and `ea` as well, and the zeros in the table above were re-measured
  with the fixed one.
- **REFUTED: "the creating task resumes at `seg1:0x9827`."** The switch-back
  (`mov ss,di / mov sp,cx`, with `DI:CX` holding the creating task's parked stack) is
  unreachable by any control flow the binary shows, and the two `nop`s at `0x9825/0x9826`
  looked like a landing pad. A scan of every segment for the word `0x9827` — as a call, a
  jump, or a datum — finds **nothing**: WOW32 is never told that address. The NOPs are
  padding.

---

## ▶ RESUME HERE

**The next piece of work is a scheduler, in the host.** That is not a guess about scope: the
seven scheduling exports above have no 16-bit body and no internal caller, so there is no
other place for the decision to live. The concrete shape:

1. **Suspend at `0x74`.** `seg1:0x9822` is the one call made with the new task's `SS:SP`
   already installed and the creating task's parked in `DI:CX`. Save the guest context there
   as the creating task's, then let the flow continue into the `iret` as it does today.
2. **Switch at the yielding calls** — `WaitEvent`, `Yield`, `OldYield`, `DirectedYield` —
   by restoring another task's saved context instead of returning. `PostEvent` and
   `SetPriority` feed the choice; `LockCurrentTask` forbids it.
3. **The list is already right**: krnl386 links every task at `seg1:0x99ed` keyed on the
   signed priority at `TDB+0x08`, and `[0x228]` names the current task, so a host scheduler
   can read its policy out of the guest rather than invent one.

The first checkpoint is small and specific: with `WaitEvent(0)` yielding back, krnl386's boot
task should return from `LoadModule`, take `seg1:0xcc6b (cmp ax,0x20)`, read
`[boot] 386GRABBER`, and **unlink its own record at `seg1:0xcd36`** — none of which fires
today. Breakpoint those three and the fix is measurable before WOWEXEC is asked to draw
anything.

⚠ Do not skip step 1. Without a saved context for the creating task there is nothing to
switch *to*: `seg1:0x97c2` parks its `SS:SP` in `DI:CX` and then clobbers both at
`seg1:0x9871/0x9873`, and `TDB+0x02/+0x04` alone gives a stack but no `CS:IP`.

### Ruled out this session — do not re-try

- **Writing the bring-up record's `hInstance`.** One writer, in `InitTask`, which that
  record never enters. `0x001e` was pattern-matching on a stack selector that is ours.
- **"WOWEXEC's TDB is never linked."** It is linked, at the head, and measured to be.
- **"The creating task resumes at `seg1:0x9827`."** The address is referenced nowhere in the
  binary, in any form.
- **Looking for a 16-bit scheduler inside krnl386.** There is not one.
- `wow32ret.txt` as a load-bearing file. It ships empty and must stay that way.

### How to drive it

```bash
PMBP=1 ARCHIVE=build/wowruns ./scripts/bmwow.sh              # deploy, run, collect
PMBP=1 ARCHIVE=build/wowruns ./scripts/bmwow.sh --no-deploy  # re-run what is on the box
```
⚠ SMB writes to `/private/tmp/xpshare` need the sandbox disabled.
