# Session 32 — 2026-08-27 — krnl386 relocates itself, and we were destroying the chains

Continues [session 31](session-31.md). GH #128. Branch `m9/completeness`.

**Where it started:** krnl386 loaded its own segment 1 and failed the relocation pass —
`seg1:0x929c call 0x8cb6` returned 0, `0x92b5` took the failure jump, `ExitKernelThunk(1)`,
`int3`. Error #1 of its five-message table.

**Where it ended:** that wall is down, and the reason it was there turns out to be a
mistake in our loader that is worth more than the wall: **our NE loader must not relocate
krnl386 at all.** krnl386 is its own relocator, and it is the pass that converts real-mode
paragraphs into protected-mode selectors. Measured both ways.

⚠️ The first fix of the day cleared the wall and was **wrong**. Both readings are kept
below, because the run that went *further* was the one that was *more* wrong.

---

## Part 1 — the chain, and why a fixup can only be applied once

An NE relocation record that is not `ADDITIVE` names **one** site; the word *at* that site
is the offset of the next site, `0xFFFF` ending it. Segment 1's four records expand to 80
sites that way:

```
rec1: SEGMENT -> seg 1,  3 sites   0x576d -> 0x46de -> 0x30d3 -> 0xffff
rec2: SEGMENT -> seg 2, 58 sites   0xb06c -> 0xa4f5 -> 0x99c1 -> ...
rec3: SEGMENT -> seg 3,  8 sites   0xb024 -> 0x5777 -> ...
rec4: SEGMENT -> seg 4, 11 sites   0xaf7c -> 0xaf88 -> ...
```

Applying a chain **writes the target over the link**. So a second pass is not wasteful, it
is impossible — and `ne_apply_relocs` in `wow_place_v86` was doing the first pass.

krnl386 then walks the same chains at `seg1:0x8ec7`:

```
or    cx,0xffff
mov   bx,dx            ; the segment value to store
xchg  [edi],bx         ; store it; bx = "next offset"
movzx edi,bx / add edi,base
inc   bx               ; 0xffff+1 -> ZF, the chain terminator
loopne
```

reading back **our** value as the next offset. The loop falls out on CX rather than ZF and
lands at `seg1:0x8f41` — `xor ax,ax` — which is exactly the measured `call 0x8cb6 -> 0`,
and exactly why session 31 saw `0x8dc7` entered **once** for four records: record 1 never
finishes.


## Part 2 — ⚠️ the easy fix: right symptom, wrong cause

The gate is one instruction and it is unconditional:

```
seg1:0x9206  test bx,0x100   ; the IN-MEMORY segment flags
seg1:0x920a  jne  0x920f     ; ...relocate
seg1:0x920c  jmp  0x92bb     ; ...skip straight to the success tail
```

so clearing `NE_SEG_RELOCS` in the header image we place makes LoadSegment return the
handle without relocating. It worked: **`ExitKernelThunk` disappeared entirely** and the run
went from PM step `0x3d` to `0x47`, into extended-memory setup.

★ **And it was wrong, and the run itself said so.** At step `0x44`, code executing from
selector `0x0207` pushed its own far pointers as arguments to WOW32 `0xc0`:

```
0x0643:0x4ff2   0x18e2:0x024a   0x18e2:0x0228   ...
```

`0x0643` and `0x18e2` are **our paragraphs** — segment 1's and DGROUP's conventional-memory
addresses — being used as selectors in protected mode. The image had been moved into PM
with real-mode fixups baked in, and all 58 far calls to segment 2 pointed at a paragraph.

*A change that moves the run further while making it more wrong is the failure mode this
project keeps hitting, so it is recorded rather than quietly replaced.*


## Part 3 — ★★★ the loader contract: krnl386 relocates itself, to SELECTORS

The fixup at `seg1:0x8e0e` takes the target's value from `es:[bx+8]` — the in-memory
segment table's **handle**, i.e. a protected-mode selector. We enter krnl386 in V86 where a
segment is a paragraph; it switches itself to PM, moves segment 1 to linear `0x20760`,
builds a code selector (`0x0207`) over it, and **LoadSegment is the pass that rewrites every
far reference to match**. That is not duplicated work — it is the only relocation that was
ever supposed to happen.

⇒ So the loader's job is to **load, not to relocate**: segment bytes and relocation records
go into memory verbatim, the header describes them, and krnl386 does the one pass that is
legal to do.

**Measured, and it is the whole proof.** The same WOW32 `0xc0` arguments, before and after:

| argument | pre-relocated by us | krnl386 relocates itself |
|---|---|---|
| seg 1 code pointer | `0x0643`:0x4ff2 (paragraph) | **`0x0207`**:0x4ff2 (its PM selector) |
| DGROUP pointers ×5 | `0x18e2` (paragraph) | **`0x021f`** (DGROUP's PM selector) |

Nothing in the entry path depends on this: entry `CS`/`DS`/`SS` are handed over directly
and are not fixups. Confirmed empirically — the V86 bring-up needs no fixup at all, and
none of segment 1's 80 sites lies below `seg1:0xc2f4`, where it calls LoadSegment on itself.

⚠️ `ne_apply_relocs` is **unchanged** and still used by the selector-stage load and all 209
NE checks. Only the V86 entry stage must not run it.


## Part 4 — where it stops now, and what that line does and does not mean

Both corrected runs reach PM step `0x46`–`0x47` and stop with **no `ExitKernelThunk`, no
error, no fault**. The last events are krnl386 committing two descriptors over extended
memory (`base=0x03b14000 limit=0x1bfff`, `base=0x03b24000 limit=0xbfff`).

That memory is genuinely backed: WOW32 `0xb8` **VirtualAlloc** is serviced, and returned
`0x03a70000` (`0x88080` bytes, krnl386's global heap) and `0x03b00000` (`0x30000` bytes).
Both descriptors lie inside the second block.

★ **The last `PMHB` line is the CS:EIP we HANDED to `dpmi_enter_pm` and never came back
from — not where the guest stopped.** Those are different claims and only the first is
measured. The guest went into PM at `0x0207:0x662f` (the `ret` in krnl386's own INT 31h
wrapper) and did not return, i.e. it is **spinning in PM**, where it generates no event the
host can see.

The call chain out of that point is known from the logged stack:
`0x662f ret` → `seg1:0x5cf8 pop ds / ret` → `seg1:0x5a42 or si,7` (a selector allocator,
`ret 8`) → its caller.


## Part 5 — ★★ the run is 282 MILLISECONDS long, and that changes the question

The `ASYNC-EARLY bail` lines carry a `ms=` stamp, and reading them is the most useful thing
that happened after the relocation fix:

```
ms=0564a436  0564a475  0564a4a4  0564a4e2  0564a511 ... 0564a550
deltas             63        47        62        47        63   ms
```

⇒ **The entire logged run spans 282 ms** — first line to last. `wowrun.bat` then waits out
its remaining ~74 seconds and copies a log that has not been written to since.

So "krnl386 loads and then stops" was never a long run that stalls; it is a **fifth of a
second** of work followed by silence. Every earlier reading of the log's shape — including
"the watchdog logs one sample and stops" — has to be re-read against that: the watchdog
samples every 250 ms, so **wd[0] at ~250 ms is the only sample the run is long enough to
contain**. Session 31 filed that as an instrument defect. It is not one.

## Part 6 — ★ `04F2` installed code selectors without patching their INT sites

A real hole, found by following the stall and worth closing regardless of whether it caused
this one. `dpmi_patch_code_region` is called from INT 31h **0009** and **000C** on the rule
"the client naming a region CODE is the only notice we get that something it just loaded is
about to be executed". The **04F2** vendor-window commit — which is how krnl386 actually
installs descriptors — did not.

That matters because krnl386 **loads its own segments**: it copies segment 1 to linear
`0x20760`, relocates it, and commits a code descriptor over it (`acc=0xfb`). Our INT→BOP
scan ran over the image in conventional memory and has never seen that copy, so any raw
`CD nn` in it would silently kill the VDM — a PM guest cannot reach the IVT. (WOW BOPs keep
working in any copy, because `C4 C4 nn` is literal in the binary, which is why such a run
looks healthy right up to the instant it stops.)

Fixed: 04F2 now applies the same rule, and the log says so —

```
DPMI: code region 0x00020760..0x0002df5f -> patched 00000000 INT sites, rejected 00000000 ...
INT31h AX=0x04f2 ... (idx 0x40 base=0x00020760 acc=0xfb) [CODE sel 0x0207 ... INT sites patched]
```

⚠️ **And it patched ZERO sites, so it is not this stall's cause.** krnl386 copied segment 1
out of our *already patched* image, where all 108 `CD nn` had been rewritten to `C4 C4 nn`
before it ever ran. The hole is real and now closed — it will bite the moment krnl386 loads
a module from **disk** — but it is recorded here as a fix that did **not** move the wall,
not as one that did.

## Part 7 — ⚠️ the watchdog is still one-sample, and the ruling-out is not finished

`dpmi_watchdog` logs `wd[0]` and nothing more, on every WOW run. This was investigated and
**not solved**:

- It is **not** the `n < 12` logging gate — that permits twelve samples.
- It is **not** `log_append` starvation — that opens with `FILE_SHARE_READ|FILE_SHARE_WRITE`
  and appends, so concurrent writes from the two threads are fine.
- It is **not** `dpmi_sel_base` faulting — that is a bounds-checked array lookup.
- The difference between sample 0 and sample 1 is that sample 1 is the first with
  `frozen != 0`, so it is the first to enter the frozen branch. Session 31 guarded the
  dereference in that branch; the symptom survived the guard.

Two changes were made anyway, and both are sound on their own terms:

1. **A live guest sample.** When frozen, suspend `g_hcpu` (the exec thread — a properly
   duplicated handle, the same one the async IRQ injector uses), `GetThreadContext`, resume,
   and *then* log `LIVE cs:eip / ss:esp / efl` plus the bytes there. Nothing is logged while
   the guest is held. This is the only way to see a spinning PM guest, which never leaves PM.
2. **No 3-second guillotine on a WOW run.** `frozen >= 12` → `TerminateProcess` is right for
   a DOS client that should never stall and exactly wrong for krnl386, which is watched
   *because* it stops. Now `600` samples (150 s) when `g_wow_nmod` is set; unchanged at 12
   otherwise. `wowrun.bat` already bounds the run at 75 s.

Two more were tried, and the second is decisive about what is *not* wrong:

3. **`THREAD_PRIORITY_HIGHEST`.** The thread that runs the guest is raised to
   `ABOVE_NORMAL` (`HIGHEST` at `execprio>=2`) and the rig is a **single-core** box, so this
   thread was the lowest-priority runnable thread in the process at exactly the moment it
   exists for. Raising it is correct on its own terms — an instrument must be able to
   preempt what it instruments — and it is safe, since the thread sleeps 250 ms out of
   every 250 ms.
4. **An unconditional `wdtick <n>` marker**, printed after `Sleep` and before anything else,
   for the first 24 iterations. This separates "woke up but the sample failed" from "never
   woke up", which no previous log could.

**Result: `wdtick 0`, `wd[0]`, and no `wdtick 1`** — at HIGHEST priority, over 75 seconds of
wall clock. So it is not the enrichment (the basic line is now written first), not the
logging gate, not `serial_out` (removed from this loop; `FlushFileBuffers` on a comm handle
has no timeout, unlike `WriteFile`), and not priority. **The thread does not return from
`Sleep`.**

⚠️ **One claim I made too strongly, corrected.** I read the absence of later `ASYNC-EARLY`
lines as "the other auxiliary thread fell silent too, so the whole process stops". That does
not follow: `why=0x14` is the bail for *"the guest is not currently executing"*, so once it
is, that thread stops **bailing** and takes the other path — which logs nothing on success.
Its silence is consistent with it running normally. The watchdog tick is the only solid
evidence here, and it says only that **the watchdog thread** does not run.


## Part 8 — ★★★ CLOSED: the process is KILLED, and the watchdog was never broken

Two more measurements settled it, and the second is the one that should have been taken
five sessions ago.

**The PM heartbeat now carries `GetTickCount()`.** All 71 steps of the run report the *same
tick*: krnl386's entire logged execution — PM entry, its own segment load, relocation,
descriptor commits — happens in **under one ~16 ms tick**. It was never a long run that
stalls.

**The watchdog was given its own log file** (`WDLOG_PATH`, collected as `wow_wd.txt`), so a
shared-resource explanation could be excluded rather than merely doubted. It contains, in
full:

```
STAGE3-DPMI: watchdog started at THREAD_PRIORITY_HIGHEST; ...
  wdtick 00000000
  wd[00000000] iter=00000001 advancing enter=0000000f:0000d6be ...
```

**And `wowrun.bat` now asks the rig whether the host is still there.** Four seconds into the
run, and again at the end:

```
INFO: No tasks running with the specified criteria.
```

⇒ **`ntvdmhost.exe` is already gone.** The host process is *terminated*, silently, with no
fault line, no teardown line and no exit path taken. That single fact explains every symptom
at once: the log ends mid-flow, the 71 steps take no measurable time, and the watchdog
records `wd[0]` at 250 ms because **there is no process left to record `wd[1]`**.

★ So "the watchdog logs one sample and stops", on the books since session 31 and carried into
this session as *the blocker*, is not a defect at all. It is the instrument reporting the
process's death correctly, at the only resolution it has. **Closed.** The work spent on it —
priority, the split line, dropping `serial_out`, its own file — is all defensible hardening,
but none of it was ever going to produce a second sample.

## Part 9 — ⚠️ it is not the fault-reflect path either

`g_flt_tbl` is 8 fault classes at stride 0x10 and **only class 6 (#GP) was filled**. A class
left zero is a class the kernel has nowhere to send, so it terminates the VDM — which made
#GP the only fault we could ever see, while a not-present selector load, a stack fault or a
page fault killed us invisibly. krnl386 has just started writing its own descriptors, which
is exactly the code that produces those.

All eight classes now point at the trampoline. It is a diagnostic widening, not a claim we
can service them: the handler logs the reflect and dumps the TIB window, it does not resume.

**Result: no `PM-FAULT REFLECTED` line, and the process still dies.** So the kill does not go
through the VDM fault-reflect path at all. Kept anyway — a zeroed class is a silent kill
waiting to happen, and `selftest.com` still passes 8/8 on real hardware with it in place —
but recorded as another fix that did **not** move the wall.

Also checked and cleared: the residual-`CD nn` shortlist for the scanned region lists 31
byte pairs, and the histogram is all implausible vectors (`00h`, `01h`, `02h`, `74h`, `8bh`
…) with **no `21h` and no `31h`**, so the plausible interrupts were all patched.

## Part 10 — ★ and it is not a host crash either: the rig's own event log says so

Free, and it should be the first thing asked of any process that vanishes. Driving
`eventquery.vbs` on the rig through `controld` (`scripts/`-side one-shot, results in
`wow_evt.txt`):

```
error  1000  8/25/2026 10:19:19 PM  Application Error  ... Faulting application ntvdmhost.exe
error  1000  8/25/2026  9:51:34 PM  Application Error  ... Faulting application ntvdmhost.exe
   ... twelve of them, all 8/19 - 8/25
```

**The most recent is two days old.** Today's WOW runs produced **no Application Error event
at all** — so the host is not crashing with an unhandled exception, which would log event
1000, and it is not dying through Windows' error-reporting path.

⇒ Combined with Part 9, the remaining shape is: **the kernel's VDM support terminates the
process deliberately**, silently, leaving no crash record and taking no fault-reflect. That
is exactly the documented "silent VDM teardown", and it is now bounded by three independent
exclusions rather than assumed.


## Part 11 — ★★ the breakpoint instrument, repaired, and then used

With every host-side instrument dead at the moment of interest, one thing still runs: **the
guest**. Breakpoints are planted in guest memory and hit by guest execution, so they need no
host thread. They were also silently useless.

**Why they never armed.** `dpmi_bp_arm()` skips a site that still reads `00 00` — correct,
since arming into memory the client has not loaded was an earlier session's silent no-op —
and then only retries when a *code region is patched*. That sufficed while the client's
extender declared its modules. It does not now: krnl386 loads its own segments, and the copy
at `0x20760` lands after the last patch pass, so a breakpoint inside it is skipped at setup
and never looked at again. Two breakpoints produced **no "armed" line and no hit**, which
reads exactly like "the guest never got there" and means nothing of the sort.

**And why re-arming naively was worse.** Arming before every PM entry bypassed the
`g_bp_pending` protocol — a hit removes the BOP and re-arms only once EIP has moved off the
site — so the BOP was re-planted under a guest that had not executed the instruction yet.
Result: `seg1:0x5a42` hit **340,808 times** with byte-identical registers on a single
millisecond, and a **268 MB** log. The guest was not looping; the debugger was holding it in
place. Fixed by honouring `pending` inside `dpmi_bp_arm()` itself (so it is safe to call from
anywhere) plus a hard `DPMI_BP_ARM_MAX` ceiling, because that cost a run and a quarter of a
gigabyte before anything noticed.

**Then it worked, and the guest walked.** Five hops, each measured, each `displaced` field
checked against `nedis.py`:

```
seg1:0x5a42 (or si,7)        hit   -- the selector allocator's epilogue
seg1:0x5a50 (ret 8)          hit   -- returns selector 0x022f over base 0x03b14000
   -> stack at SS:SP gives the caller: 48 46
seg1:0x7ed4 ... 0x7f11       hit   -- and `call 0x63b4` RETURNS
   -> stack gives the next caller: 0x4648
seg1:0x4648 mov es,di        hit   -- di = 0
seg1:0x464a mov fs,di        hit   -- ES now 0x0000; FS=NULL SURVIVES (a real suspect, cleared)
seg1:0x464c call 0x67b9      hit
seg1:0x67b9 / 0x67bf         hit TWICE
```

## Part 12 — ★★★ there ARE raw `INT nn` in the code krnl386 executes

`seg1:0x67b9` is:

```
67b9  dec  word [0x1e]
67bd  jne  0x67ce
67bf  test word [0x44], 1
67c5  je   0x67ce
67c7  and  word [0x44], 0xfffe
67cc  cd 02          ★ INT 2
67ce  ret
```

A breakpoint on that `INT 2` **armed, reporting `displaced cd 02`**. That is proof, not
inference: a patched site is an INT site, `dpmi_bp_arm()` refuses those outright
(`if (pmap_get(lin)) continue`), so it could not have armed at all. **The site is raw.**

⇒ The boundary vote in the INT→BOP scan produces **false negatives on real instructions**.
Session 21 fixed the opposite error (the patcher rewriting a `jle` displacement it mistook
for `CD nn`); this is the same decision failing the other way, and in protected mode the
cost is a silent VDM teardown the moment the guest takes that branch.

On this run it did not: `test word [0x44],1` fell through both times, so the `INT 2` was not
executed and the run still ends at the same place. **So it is a proven defect and not yet
proven to be *the* killer.**

The residual scan now prints **addresses**, not just a histogram — a histogram names a
suspect without saying where, which still means reading the binary by hand:

```
DPMI: residual CD nn SITES (linear, first 24): 0x77d3=01 0x8482=75 0x9218=99 0x96b7=30
  0x9c6d=01 0x9eb0=c7 0x9ff0=3b 0xa267=02 0xadb0=8b 0xb729=03 0xcb84=2a 0xcbfc=02 ...
```

`0xcbfc` is `0x6430 + 0x67cc` — the very site, predicted before the run and confirmed by it.
`0xa267=02`, `0xb729=03`, `0x96b7=30`, `0xcb84=2a` are the other plausible ones.

## Part 13 — ★ the 64 KB scratch window was 48 KB of somebody else's memory

Found while walking, and true regardless of the killer. krnl386 builds a **64 KB** selector
over `base(SS) + SP` and treats everything above the header image as its scratch arena — we
hand it the size in CX (`0xFFF0 - 0x4000 = 0xBFF0`). The block was allocated as stack +
header image only:

```
SS                    para 0x1abf
header selector base  para 0x1bbf   (64 KB window -> para 0x2bbf)
our block ENDS        para 0x1fbf
krnl386's PSP/arena   para 0x1fc0   ★ inside the window
```

So for 48 KB of its length the "scratch arena" we promised was krnl386's **own PSP and arena
block**, which it carves from independently — two owners of one region, the exact failure the
session-31 memory-model note was written about, reappearing one allocation later.

Fixed by allocating the whole window (`WOW_WINDOW_PARAS = 0x1000`), which pushed the PSP block
from para `0x1fc0` to `0x2bc0` — exactly 64 KB higher, no overlap. ⚠️ It did **not** move the
wall: the run still ends in the same place. Recorded as a real defect closed, not as progress.

⚠️ And it moved every address: krnl386's segment-1 PM copy went `0x20760` → `0x2c760`, so the
armed breakpoints silently stopped hitting. *Derive the base per run from the `acc=0xfb` commit
line; never carry it between runs.* The resume block says this and it still cost a round.

## Part 14 — ★★★ THE KILLER: krnl386 overwrites the code it is executing

The walk ended at `seg1:0xc4d8`, with the next armed site `0xc4e5` never reached. Exactly one
call sits between them — `call 0xcf9e` at `seg1:0xc4dd` — and bisecting inside it puts the
last hit on `seg1:0xcfe4`:

```
cfa1  mov  ax,[0x5a0]         ; the arena selector
cfab  call 0x63f3             ; -> eax
cfae  mov  edx,[eax+0xc]      ; its size
cfb3  shr  edx,4              ;    ...in paragraphs
cfc2  xchg [0x5a4],bx         ; bx = accumulated gap, and zero it
cfca  sub  dx,bx
cfd0  movzx esi,bx / shl esi,4 ; src = gap
cfd8  xor  edi,edi             ; dst = 0
cfdb  movzx ecx,dx / shl ecx,2
cfe4  rep movsd                ★ ARENA COMPACTION
```

Registers at the fatal hit, which is the first time this session has had the faulting
instruction *and* its operands:

```
ECX=0x000202e0  -> 0x202e0 dwords = 0x80B80 bytes = 514 KB
ESI=0x000038a0  EDI=0x00000000  DS=ES=0x01b7
```

Selector `0x01b7` is `base=0x0001bbe0 limit=0x0008441f` — it spans `0x1bbe0 .. 0xA0000`,
sized deliberately to the 640 KB line. So **the copy is entirely in bounds**:

```
dst 0x1bbe0..0x9c760      src 0x1f480..0xa0000      both legal
```

★★★ **And krnl386's own segment-1 PM copy — the code it is executing — is at CS base
`0x2c760`, which lies inside that destination range.** The `rep movsd` overwrites itself
mid-instruction. That kills it instantly, with no fault to reflect, no crash record and no
surviving thread, which is every symptom this session has been chasing, in one instruction.

The gap being closed is `bx = 0x38a` paragraphs (14 KB), accumulated at `seg1:0xc4a3`
(`sub ax,[0x5a6]`) and `0xc4d8`. So the question for next time is **why our layout leaves a
14 KB hole below live code**: real WOW must either compact before anything live is in the
arena, or never accumulate that gap. `[0x5a6]` is the arena size we hand over in CX, so the
CX/`[0x5a6]` contract and the order in which krnl386's segments are placed relative to the
arena are the two things to examine.

## Part 15 — the arena contract, measured — and why tuning CX cannot fix it

`[0x5a4]`, the compaction delta, is `[0x148e] - [0x5a6]` plus a selector-base correction.
All three were read straight out of DGROUP with a breakpoint dump:

| CX handed over | `[0x5a6]` (declared) | `[0x148e]` (its own) | gap `[0x5a4]` | `rep movsd` |
|---|---|---|---|---|
| `0xbff0` (window - header) | `0x0bff` | `0x0f88` | `0x0389` | 514 KB down by **14 KB** |
| `0xfff0` (whole window) | `0x0fff` | `0x0f88` | `0xff89` (**-0x77**) | `ESI=0xFF890` — **outside the selector** |
| `0xf880` (matched) | `0x0f88` | `0x0f88` | `0x0000` | 542 KB down by **16 bytes** |

★ `[0x148e]` is `0x0f88` in **every** run — it is computed from krnl386's own arena, not
from CX. So the old value was declaring `0x389` paragraphs (our 16 KB header image) as dead
space for krnl386 to reclaim, and a negative gap is not a smaller bug than a positive one,
it is a wilder one.

`0xF880` ships: it is the value that makes the declared arena match what krnl386 believes it
has, and it shrinks the move from 14 KB to 16 bytes. ⚠️ **It does not clear the wall.** The
residual paragraph comes from `add [0x5a4],ax` at `seg1:0xc4d8`, where `ax` is the difference
between our header-selector base (`0x1bbf0`) and the arena selector krnl386 reallocates
(`0x1bbe0`) — 16 bytes. A 16-byte move of 542 KB is still a move of live code.

⇒ **So the fix is not in CX.** The destination range is bounded by the *selector*, and
krnl386 has grown `[0x5a0]` to `base=0x1bbe0 limit=0x8441f` — everything from just under our
header image to the 640 KB line. Its own segment-1 PM copy sits inside that at `0x2c760`.

▶ **The question for next time is placement, not size:** krnl386 VirtualAlloc'd a 0x88080-byte
global heap at `0x03a70000` through WOW32 `0xb8`, and *also* a 0x30000 block at `0x03b00000` —
extended memory, outside any conventional arena. **Why does it put its own code segments in
the conventional arena instead?** If segment 1 lived in the extended heap it would be outside
the compaction range entirely and this whole class of failure disappears. That is where to
look: what LoadSegment uses to choose the target block.

## Part 16 — ⚠️ capping the conventional arena: tried, no effect, reverted

The obvious follow-on from part 15 was that krnl386 only puts its code in conventional
memory because we hand it **all** of it — `dos_alloc(0xFFFF)` gives it `0x7440` paragraphs
(476 KB) — so capping that should push its segments into the `0x88080` extended heap it
already VirtualAlloc'd at `0x03a70000`, outside any compaction range.

Capped to `0x1000` paragraphs (64 KB). Measured:

```
capping krnl386's conventional arena 0x00007440 -> 0x00001000 paras
PSP/arena block at para 0x00002bc0 size 0x00001000
segment-1 PM copy still at base=0x0002c760 acc=0xfb      <- unchanged
arena selector 0x1b7: base=0x0001bbe0 limit=0x0008441f   <- unchanged
```

★ **The arena selector still reaches `0xA0000`** even though the PSP's top-of-memory field
now says `0x3bc0`. So krnl386 does not derive that selector from the PSP block we build for
it — it uses the 640 KB line directly. The cap changes nothing it can observe, and the
segment still lands at `0x2c760`.

Reverted. A change with a plausible mechanism and no measured effect does not stay in,
especially one that walks back session 31's "claim ALL remaining conventional memory", which
was itself the fix for krnl386 carving over our allocations.

⇒ Which sharpens the question again: the block at `0x2c760` is chosen by krnl386's own
allocator, and neither the arena size (part 15) nor the PSP block size (here) moves it. The
next thing to read is **the allocator itself** — `seg1:0x461f`, called from LoadSegment at
`seg1:0x9257`, is what picks the block; and `seg1:0x63f3`, called from the compaction, is
what tells it the block's size. Those two decide the range, and neither has been read yet.

## Part 17 — the two routines that decide the compaction range

Read, not run — the thread to pull next session.

**`seg1:0x461f` is krnl386's global allocator**, and its tail *is* the `0x4648` block this
session walked through: `push bp / ... / call 0x456c / call 0x7e95 / mov es,di / mov fs,di /
call 0x67b9 / pop edi / pop esi / pop ds / leave / retf 6`. LoadSegment calls it at
`seg1:0x9257`. So the whole walk — `0x5a42 -> 0x7f13 -> 0x4648 -> 0x67b9 -> retf 6 -> 0xc48d`
— was one allocation returning, and the compaction at `0xc4dd` happens immediately after it
in the `0xc4a3..0xc50a` loop.

**`seg1:0x63f3` is selector -> block descriptor**, and it is where the compaction's size
comes from:

```
bx = [bp+4]              ; a selector
and bl,0xf8 / shr bx,1   ; -> (sel>>3)*4, a DWORD index
cmp bx, es:[0x22e]       ; against the table's entry count
add ebx, es:[0x230]      ; + the table base (a 32-bit pointer)
eax = [ebx]              ; -> the block's descriptor
```

then the caller takes `[eax+0xc]` as the block size — the `0x84420` that becomes the 542 KB
copy. So `es:[0x230]` is a table of **32-bit** pointers indexed by selector — krnl386's
global-heap arena table — and the compaction range is whatever that table says the block is.

▶ That is the thread: the range is not derived from CX (part 15) and not from the PSP block
(part 16); it comes from this table. Reading how the entry for selector `0x01b7` gets its
`base` and `+0xc` size is what will explain why the block spans `0x1bbe0..0xA0000` and why
segment 1's copy is placed inside it.

## Part 18 — ★★★ THE WALL MOVES: put the window HIGH and the compaction cannot reach the code

Parts 15 and 16 eliminated the two inputs I thought controlled the compaction range. What
was left is the only one we actually own: **where `base(SS) + SP` is**.

krnl386's conventional arena is the region from `base(SS) + SP` to the 640 KB line. We were
allocating the stack + window LOW, so that arena was 542 KB — big enough for segment 1
(`0xd7fa`), which is exactly why krnl386 put its code copy at `0x2c760`, inside the block it
then compacted across.

Allocate the block HIGH instead — DOS has no "allocate high" here, so do it the way a DOS
program would: take a filler that leaves exactly this block at the top, allocate, free the
filler; the PSP block below then takes the freed region. (⚠️ the filler must be
`fmax - want - 1`: without the paragraph for DOS's MCB header the second allocation fails
outright, which is what the first attempt did.)

Every prediction landed:

| | before | after |
|---|---|---|
| SS / header image | para `0x1bbf` | para **`0x9000`** |
| arena selector `0x1b7` | `base=0x1bbe0 limit=0x8441f` (542 KB) | **`base=0x90000 limit=0xffff`** (64 KB) |
| segment 1's PM copy | `0x2c760` — *inside* the copy | `0x0001ad00`, and a second at **`0x03b10100`** (extended) |
| PSP/arena block | para `0x2bc0` | para `0x1abf` (the freed low region) |
| last PM step | `0x46` | **`0x4e`** |

★ `0x1ad00` is **not** inside `0x90000..0x9ffff`, so the compaction cannot touch the executing
code — and krnl386 has started using the extended heap it VirtualAlloc'd (`0x03b10100`) as
well. The mechanism is confirmed by the addresses, not inferred.

★★ **And the silent teardown is gone.** The run no longer vanishes: it ends with a
*deliberate* `ExitKernelThunk(1)` — `FUNC=0x2 ... from=0x00009880 (00000001)` — i.e. krnl386
reporting a LoadSegment failure for one of segments 2–4, out of the loop at `seg1:0xc4f6`
(`or ax,ax / jne 0xc501 / pop es / jmp 0xc9db`). A reported error is a different universe from
a process that disappears.

▶ **Next: which of segments 2/3/4 fails, and why.** The conventional arena is now deliberately
small, so an allocation failure is the obvious first suspect — LoadSegment's allocator is
`seg1:0x461f`, already mapped in part 17, and the breakpoint walk is repeatable at
`base + offset` with the base derived per run from the `acc=0xfb` commit line.

## Part 19 — ★★ the new wall: krnl386 believes the loader already placed segments 2-4

With the compaction survived, the run reaches the loop at `seg1:0xc4a3` and fails there.
Breakpoints on the loop name it exactly:

```
seg1:0xc4dd  compaction        ESI=2
seg1:0xc4f6  call 0x90d9       ESI=2      -- LoadSegment(segment 2)
seg1:0xc4f9  or ax,ax          EAX=0      -- ★ IT FAILED
```

and inside LoadSegment, breakpoints on the allocate path (all three armed, verified by their
`displaced` bytes):

```
seg1:0x913d  call 0x9068   -- ALLOCATE      not hit
seg1:0x9141  or ax,ax                       not hit
seg1:0x9145  mov ax,es:[si+8]               HIT, with EBX = 0x0001d152
```

★ `0x9068` — the allocator — **is never called**. `seg1:0x9135 test bl,2 / jne 0x9145` is
taken, because segment 2's **in-memory** flags are `0xd152` and bit 1 is set. The file says
`0x1d50`, so krnl386 set that bit itself while building its module database. It does the same
to segment 1 (file `0x0d40` → memory `0xc142`); the transform is consistent — bits 10/11 shift
to 14/15 and **bit 1 is added** — and all four segments are marked `PRELOAD` (bit 6) in the
file.

⇒ **krnl386 marks a PRELOAD segment as already resident and expects the LOADER to have put a
handle in the in-memory segment table at `+8`.** For segment 1 that handle is real (`0x0207`,
later `0x01c7`) and LoadSegment succeeds. For segment 2 it is not, so LoadSegment returns 0,
`seg1:0xc4f9` takes the failure jump and the exit stub reports error #1.

That is a *loader contract* gap, and it is the same shape as the two already found this
session (the header must be placed; the relocations must be left alone): **we copy all four
segments into conventional memory and never tell krnl386 where three of them are.**

▶ Two ways out, and they are worth weighing before coding:
   1. **Fill the handle** — give each segment a selector at load time and write it into the
      in-memory table's `+8`. Closest to what a real Win16 loader does for PRELOAD segments.
   2. **Clear PRELOAD** in the placed header, so krnl386 does not believe they are resident,
      calls `0x9068`, and loads them from `KRNL386.EXE` itself — it already has the file open
      (session 31) and LoadSegment has the LSEEK+READ path at `seg1:0x9227`.
   (2) is less code and hands the work to the component that knows how; (1) is more faithful.
   Either way the answer is measurable in one run.

⚠️ **Operational: the layout is not stable between runs.** `ES=0x01bf` resolved to base
`0x0001ad00` in one run and `0x0002aec0` in the next, so a `dump` address computed from a
previous log points at nothing. The HIT line now prints `dsbase=`/`esbase=` for exactly this
reason — use those, and derive every address per run.

## Part 20 — ⚠️ PRELOAD is not the bit, and `0x9145` is not the failure

Two corrections to part 19, both from one run.

**Clearing `NE_SEG_PRELOAD` on segments 2-4 does not change the outcome.** It propagates
(segment 2's in-memory flags go `0xd152` → `0xd112`, exactly bit 6) but **bit 1 is still set**,
so `seg1:0x9135 test bl,2 / jne 0x9145` is still taken and the allocator at `seg1:0x9068` is
still never called. So bit 1 is not derived from PRELOAD; krnl386 sets it regardless. The
change was reverted — no measured effect.

**And `seg1:0x9145` was never the failure.** With `bl=0xd112`, `test bl,4 / je 0x9183` falls
straight through: reading the handle at `+8` is the normal path, not an error path. Part 19
read a branch as a wall because it was the last breakpoint that hit, which is the same mistake
as reading the last `PMHB` line as where the guest stopped. **The furthest hit is a floor, not
a location.**

## Part 21 — ★★★ segment 2 fails the SAME WAY segment 1 did on the first morning

A spread of eight breakpoints across LoadSegment's tail names it without ambiguity:

```
seg1:0x9183  EAX=0x01ce      the branch
seg1:0x91c3  EAX=0x01b7      call 0x937e -- and it reports the segment IS loaded
seg1:0x9202  EAX=0x01d0      flags & 0x100 -- it has relocation records
seg1:0x929c  EAX=0x8b26      call 0x8cb6 -- APPLY RELOCATIONS      EBX=0x03b10000
seg1:0x92b5  EAX=0x00000000  ★ THE FAILURE JUMP
```

`call 0x8cb6` returns 0. **That is precisely the failure this session opened with** — the one
that produced `ExitKernelThunk(1)` for segment 1, whose cause was that the relocation records
were not in memory after the segment (session 31 part 21).

And the mechanism is the same. `[bp-2]` is `0xFFFF` (no file handle: `seg1:0x9191`, which opens
the module file, was **not** hit), so `seg1:0x9215 inc bx / jne 0x9227` is not taken and control
reaches `seg1:0x921b` — `mov es,dx / mov si,cx / lodsw` — which reads the record **count off
the end of the loaded segment**. Whoever placed the segment has to have put the records there.

★ `EBX=0x03b10000` at the fatal call: segment 2 is in the **extended heap**, not in our
conventional copy. `wow_place_v86` copies each segment's records immediately after its bytes,
but that is at *our* address; segment 2's copy at `0x03b10000` needs its records at
`0x03b10000 + 0x3ee2 = 0x03b13ee2`.

▶ **So the next question is who moved segment 2 into extended memory and whether the records
came with it.** `seg1:0x937e` reported it loaded, so a handle already existed — the same
"loader was here" assumption as part 19, one level down. Worth checking first, and cheap:
dump `0x03b13ee2` and compare against the file's record block for segment 2 (`04 00` count
then 8-byte records, at file offset `0xf880 + 0x3ee2`). If it is not there, that is the whole
answer and the fix is the same shape as session 31 part 21.

## Part 22 — ★★★ proven end to end: segment 2's address is our header window

`seg1:0x8d56` is where the record walker has `DS:SI` on record 1, and the HIT line resolves
and dumps it. For segment 2:

```
krnl386 reads:  16 0c 00 81 e2 00 20 5f 5e 1f c9 ca 02 00 55 8b   dsbase=0x00090000
the file says:  02 00 df 3e 01 00 00 00
```

Two things settle it at once.

★ **Those bytes are code, not records.** `5f 5e 1f c9 ca 02 00 55 8b ec` disassembles as
`pop di / pop si / pop ds / leave / retf 2 / push bp / mov bp,sp` — a function epilogue
followed by the next function's prologue. krnl386 is walking machine code as if it were an
8-byte relocation table, which is why `call 0x8cb6` returns 0 and `seg1:0x92b5` fires.

★★ **And `dsbase=0x00090000` names the place.** That is our 64 KB header/scratch window
(part 18 moved it there), not segment 2's image. `DX:CX` at `seg1:0x921b` comes from
`call 0x937e` — "where is this segment" — so the address krnl386 holds for segment 2 points
at the window.

⇒ This is part 19's gap, proven from the other end and unified with part 21: **segments 2-4
have no valid handle, so every consumer of that handle reads whatever the stale selector
happens to cover.** The relocation failure is a symptom, not a second bug — the same shape as
the header placement and the relocation chains, and the third instance this session of "we
did the work and never told krnl386 where the result is".

▶ **So it has to be option 1 after all: supply a real address for segments 2-4.** Option 2
(clear PRELOAD) is already refuted in part 20 — bit 1 is not derived from it. The open
question is *where* krnl386 takes that address from, since it builds the in-memory segment
table itself from our placed header and the file's 8-byte entries carry no handle field.
Segment 1 has a real one (`0x0207`, later `0x01c7`), so **finding what populated segment 1's
`+8` is the whole task** — it is the one worked example, and whatever wrote it is what needs
to run for 2-4 as well. `seg1:0x937e` (address-of-segment) and `seg1:0x9068` (allocate) are
the two routines already mapped that touch it.

## Part 23 — ★★★ found it: `seg1:0xd5e0` gives segment 1 memory and the rest a PLACEHOLDER

Part 22 said the task was "find what populated segment 1's `+8`". Byte-scanning krnl386 for
stores of the ALLOCATED bit finds three sites, all in the bring-up region — `seg1:0xd60c`,
`0xd63b`, `0xd674` — and they are one routine:

```
d5e7  mov ds,[bp+4]          ; the module database
d5ea  mov si,[0x22]          ; -> its segment table
d5ee  mov di,[si+6]          ; segment 1's MINALLOC
d5f3  mov bx,0x1200
d5f6  push bx / push 0 / push di
d5fb  call 0x1461f           ; ★ GlobalAlloc(flags=0x1200, size = 0:di)
d5fe  or ax,ax / je 0xd67f   ; failed -> bail out
d605  mov [si+8],ax          ; ★★ STORE THE HANDLE
d608  and byte [si+4],0xfb   ;    clear bit 2
d60c  or  byte [si+4],2      ; ★  set bit 1 = ALLOCATED
d615  add si,0xa             ;    next segment (10-byte stride)
...
d624  mov bh,0x23 / mov bl,0x0a
d628  push bx / push ax / push ax      ; ax = 0
d62d  call 0x1461f           ; ★ GlobalAlloc(flags=0x230a, size = 0:0)
d634  mov [si+8],ax          ;    same store...
d63b  or  byte [si+4],2      ;    ...and the same ALLOCATED bit
```

★ **Segment 1 gets a real allocation of `[si+6]` bytes. Every other segment gets
`GlobalAlloc(size = 0)`** — a placeholder handle with no memory behind it, flags `0x230a`
instead of `0x1200`. Both paths then set bit 1, which is why part 20's PRELOAD experiment
was a dead end and why `seg1:0x9068` is never called: krnl386 *has* allocated something for
every segment, just nothing of any size for segments 2-4.

⇒ **That is the whole chain, and it closes on itself.** The placeholder handle is why
`seg1:0x937e` hands back an address that lands in our header window (part 22), why the record
walker reads code instead of relocation records, why `call 0x8cb6` returns 0, and why
`seg1:0x92b5` fires. One cause, four measured symptoms.

▶ **Next: the deferred fill.** A placeholder is meant to be filled later, and LoadSegment has
exactly one path that does it — the file path at `seg1:0x9191` (`call 0x8b3f`, open the module)
feeding the LSEEK+READ at `seg1:0x9227`. It is not taken: `seg1:0x9183` tests `[bp+6]`, which
the caller sets from `call 0x48b4([0x59e])` at `seg1:0xc4e9` — a *selector*, not `0xFFFF` — so
the open is skipped and control reaches the in-memory path at `seg1:0x921b` instead.

So the question is now narrow and concrete: **what has to be true for `[bp+6]` to arrive as
`0xFFFF`** (making krnl386 read its own segments from `KRNL386.EXE`, which it has open), **or
alternatively what is supposed to be staged at `[0x59e]`'s selector** — the same window the
compaction reclaims, which would explain why that loop compacts once per segment. Those are
the two readings; `seg1:0x48b4` and `[0x59e]` decide between them.

## Part 24 — the deferred fill resizes the block but never fills it

Three routines close the loop from part 23.

**`[0x59e]` is the module handle.** `seg1:0xc233` calls `0xd02b` — the database builder —
with the PSP's **top-of-memory** (`es:[2]`), `[0x5cc]`, `[0x5a0]` (the window selector) and
`0x200`; on failure it prints from `0xb9ce` and does `INT 21h AX=4CFF`. The handle it returns
is stored at `seg1:0xc24a`.

**`seg1:0x93bd`, inside `0x937e`, is the deferred fill** — and it only fixes the size:

```
93c0  bx = es:[si+6]            ; the segment's MINALLOC
93c6  cmp bx,1 / adc dx,dx      ; dx:bx = size
93cb  add bx,2 / adc dx,0
93d1  push [bp-2]               ; the PLACEHOLDER handle from part 23
93d9  call 0x4658               ; ★ GlobalReAlloc(handle, size)
93dd  cmp [bp-2],ax / je 0x93f7 ; same handle back -> success
```

So the zero-size placeholder is grown to the segment's real size. The block then exists, is
correctly sized, and is **empty** — which is exactly what the record walker reads (part 22).

**Nothing copies the bytes.** For segment 1 that copy is `call 0x647a` at `seg1:0xd612`,
issued immediately after its handle is stored. Segments 2-4 have no equivalent, so the only
remaining source is LoadSegment's file read at `seg1:0x9227` — gated on `[bp-2] != 0xFFFF`,
which requires `seg1:0x9191` (`call 0x8b3f`, open the module file), which is reached only when
`[bp+6] == 0xFFFF` at `seg1:0x9183`.

★ And it never is. Both call sites pass `[bp+4] = 0xFFFF` but neither passes `[bp+6] = 0xFFFF`:
segment 1's caller (`seg1:0xc2f2`) passes `CS`, and the loop (`seg1:0xc4ef`) passes
`call 0x48b4([0x59e])` — the module-database selector, measured as `0x01ce`. A valid selector
there means "the data is already available", so the open is skipped and the empty block is
relocated.

▶ **Two readings, and they are distinguishable.** Either `0x48b4([0x59e])` is *supposed* to
fail here (returning `0xFFFF`, sending krnl386 to its own file — it has `KRNL386.EXE` open
already), in which case the module handle or its selector state is wrong; or the loader is
expected to have staged each segment's image where `0x937e` points, in which case the
compaction that runs once per loop iteration is the mechanism for reclaiming each one after
use. `seg1:0x8b3f` (open module) and `seg1:0x647a` (the segment-1 copy) are the two routines
that decide it, and neither has been read.

## Part 25 — ⚠️ correction: `seg1:0x647a` is SetOwner, not a byte copy

Part 24 said segment 1's byte copy is `call 0x647a` at `seg1:0xd612`. Reading it says
otherwise:

```
647a  push bp / mov bp,sp / push ds/es/esi/edi
6483  call 0x67a8               ; enter the critical section
648a  push [bp+6] / call 0x63f3 ; selector -> block descriptor, in EAX
6490  push [bp+4]
6493  pop word [eax+0x12]       ; ★ store [bp+4] into the descriptor at +0x12
649d  call 0x67b9               ; leave it
64a7  ret 4
```

`seg1:0xd610` pushes `ax` (the handle just allocated) then `ds` (the module database), so the
call is `SetOwner(block, module)` — it writes the **owner** field, and copies nothing. There is
no byte copy in `0xd5e0` for *any* segment, including segment 1. The inference in part 24 was
built on an unread routine and it was wrong; the question "what fills the block" is still open,
for all four segments.

## Part 26 — `seg1:0x8b3f` is a module→file-handle CACHE, and it works

The other unread routine, and it is in good order:

```
8b5e  cx = [0x5da]        ; entry count
8b62  di = 0x5dc          ; the table, 4 bytes per entry {handle, module}
8b65  bx = [di+2]         ; this entry's module
8b68  cmp ax,bx / je      ; already open for this module -> reuse it
8b6e  or bx,bx / ...      ; else remember the first free slot
8b7a  add di,4 / loop
...
8b8d  cx = [0x5d6]        ; round-robin victim pointer, bounded by [0x5d8]
8ba5  cmp bx,[bp+6] / je  ; never evict the module we are opening for
8bae  mov ah,0x3E / call 0x4ff2   ; ★ INT 21h CLOSE the victim
8bb5  [bp-2] = di         ; reuse that slot
```

So krnl386 keeps a small ring of open file handles keyed by module, closes the least recently
used when it needs a slot, and refuses to evict the caller's own. That is exactly the
machinery a loader needs to page segments in from disk on demand — and session 31 already
proved the DOS side of it works (krnl386 opens `KRNL386.EXE` and gets handle 5 through our
layer).

⇒ **The capability is present and functional; it is simply never invoked for segments 2-4**,
because `[bp+6]` at `seg1:0x9183` is a valid selector (`0x01ce`) rather than `0xFFFF`. Every
piece of the on-demand load path exists except the one input that would trigger it.

▶ So the remaining question is sharper than before and is about **one word**: what makes
`call 0x48b4([0x59e])` at `seg1:0xc4e9` yield `0xFFFF`. `0x48b4` validates its argument with
`lar ss:[bx+2]`, tests the AVL/high bit and falls through to `call 0x63f3`; its failure exits
are `seg1:0x48e8` / `0x48eb`. Reading what it returns on each, against what `[0x59e]`'s
selector actually looks like in our run, is the next measurement — and it is a static read plus
one breakpoint, not a redesign.

## Part 27 — ⚠️ option (a) is dead: `0x48b4` cannot return `0xFFFF`

Read in full, `seg1:0x48b4` is not "handle → selector" — it is the inverse, and it never
produces the value the file path needs:

```
48b4  bx = sp
48ba  lar ax, ss:[bx+2]        ; validate the ARGUMENT as a selector
48bf  jne 0x48e8               ; invalid -> return with AX = 0
48c1  test ah,0x80             ; the descriptor's present bit
48c4  je  0x48eb
48cc  ds = [0x21a]             ; the arena-info selector
48d0  push ss:[bx+2] / call 0x63f3   ; selector -> block descriptor, in EAX
48d7  or eax,eax / jne 0x48df
48dc  pop ds / jmp 0x48e8      ; no descriptor -> return with AX = 0
48df  ax = [eax+0x10]          ; the descriptor's +0x10
48e6  or al,1                  ; make it odd: a HANDLE
48e8  ret 2
```

`AX` is zeroed at `seg1:0x48b6` and every failure exit returns it unchanged, so the result is
either a valid odd handle or **0** — never `0xFFFF`.

⇒ `seg1:0x9183`'s `inc ax / je 0x9191` therefore **cannot fire from this call site**, so
LoadSegment in the `seg1:0xc4a3` loop is *never intended* to open the module file. Part 24
offered two readings and this eliminates the first: the segment image is expected to be **in
memory already**, reachable through the block `0x937e` resizes.

## Part 28 — ★★ and the reload is a WOW32 call we do not implement: id `0x7c`

`seg1:0x93bd` calls `0x4658` (GlobalReAlloc) to grow the placeholder. Reading `0x4658`:

```
4683  call 0x5fd2              ; the real reallocation
4686  or ch,ch / je 0x469a
468a  test cl,1 / je 0x469a    ; ...on a particular result
468f  push [bp+0xc]
4692  push 0x3e9
4697  call 0xb38a              ; ★ a WOW32 stub
```

and `seg1:0xb38a` is the thunk for **WOW32 id `0x7c`** (4 argument bytes):

```
b38a  push 4 / push 0 / push 0x7c / push cs / call 0x12bb6
```

`0x7c` is **not implemented** by our WOW32 layer and is not among the ids seen live so far
(`0x78`, `0x89`, `0xb8`, `0xc0`-`0xc2`, `0xc7`, `0xcf`). It is called from inside the memory
manager, with a constant `0x3E9` (1001) and the reallocation's flags — the shape of a
notification to the 32-bit side that a block moved or needs backing.

⇒ That is a concrete, checkable lead and it sits exactly where the missing step must be: the
block gets resized, `0x7c` is invoked and stepped over as unimplemented, and the segment's
bytes never arrive. **Implementing or at least tracing `0x7c` is the next move** — and unlike
everything in parts 15-18, it does not require guessing at a memory layout.

▶ First measurement: breakpoint `seg1:0x4697` and confirm `0x7c` is reached during segment 2's
`0x937e`, then read what our dispatcher does with it. `tools/ne/nedis.py --wowfunc 0x7c` gives
the callers and the argument-building code.

## Regression

- `selftest.com` **8/8 PASS on real hardware** — the other guest class, run because the
  watchdog change is on the shared DPMI path and *a fix measured on one guest is a fix for
  none*.
- 209/209 NE checks and every `dostest` battery pass off-VM.
- Imports still XP-safe.


## Next actions

1. **Find what terminates the process.** This is now the whole question, and it is a much
   better one than "where does the guest spin": the host is killed while executing guest PM
   code, within one 16 ms tick, and it is *not* the fault-reflect path. Ruled out so far:
   every fault class (all eight now reflect, none fires), unpatched `INT nn` in krnl386's
   own copy at `0x20760` (patched — zero sites, it was copied from our patched image), and
   residual `CD nn` in the scanned region (no `21h`, no `31h`).
   The cheap next measurements, in order:
   - **Log immediately after `dpmi_enter_pm` returns** (rc + event), so "died inside
     `NtVdmControl`" is distinguished from "died in our code just after it".
   - **Arm a breakpoint** at `seg1:0x662f` and the return chain below it
     (`0x5cf8`, `0x5a42`) with `rep=1`, linear = `0x20760 + offset`. Breakpoints are hit by
     the *guest*, so they need no host thread and survive what killed every other
     instrument. This is the one diagnostic left standing.
   - Check the XP **Application event log** on the rig for a matching entry — free, and the
     kernel may well be naming the reason we cannot see from inside.
2. **Implement WOW32 `0xc0`** — the only unimplemented call reached (`wow32{unimpl=1}`).
   28 arg bytes = seven far pointers: krnl386's INT 21h thunk (`seg1:0x4ff2`), five DGROUP
   variables (`0x024a`, `0x0228`, `0x06e2`, `0x022c`, `0x0297`) and the PSP (`0x013f:0`).
   It is a registration of krnl386's core data with the 32-bit side, called once from
   `seg1:0x30dd`, and `wowdecline.py` does not list it as declinable.
3. Then the spin itself, from `seg1:0x5a42`'s caller outward.
4. Then: user/gdi's KERNEL-funnelled path, then `wowexec`, then an app.

⚠️ **`tools/ne/nedis.py` does not mask branch targets to 16 bits** — `call 0x11493` at
`seg1:0xc2dd` is really `call 0x1493`. Cost a few minutes; worth fixing in the tool.

The operational detail (the rig loop, breakpoints, tracing, reading the guest) is unchanged:
see [`▶ RESUME HERE` in session 31](session-31.md#-resume-here--the-operational-detail-a-fresh-context-needs).
