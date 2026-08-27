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
