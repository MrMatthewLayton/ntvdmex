# Session 33 — 2026-08-28 — the stock oracle answers, and krnl386 loads segments 2 and 3

Continues [session 32](session-32.md). GH #128. Branch `m9/completeness`.

**Where it started:** session 32's handoff. `LoadSegment(segment 2)` returns 0 inside our
WOW bootstrap, `ExitKernelThunk(1)`. The cause was known to be a loader-contract gap — we
give segment 1 a real `GlobalAlloc` and segments 2-4 a size-0 placeholder that nothing ever
fills — but *what a correct loader puts there* was not known, and six leads had been ruled
out guessing at it. The handoff's plan was to stop guessing and **ask stock ntvdm**, which
implements this exact contract, against this exact `krnl386.exe`, on this exact box.

**Where it ended:** stock was asked and answered, and the answer is a difference we can
act on:

> **Stock keeps krnl386's WHOLE FILE IMAGE resident in conventional memory** — from the
> NE header on, `0x16440` bytes at linear `0x899f0`, with a 64KB selector (`0x01ef`) over
> it. **We copy the first `0x4000` bytes and cut off the rest** (`WOW_HDRIMG_PARAS`), and
> the part we cut off is where every segment's image lives.

⚠️ **The first version of this write-up said something else, and it was wrong** — see
part 7. It read our segment bases off the *selector stage*'s log, which is not the stage
that runs. The project memory carries that exact warning ("the executing krnl386 is NOT at
the address the bind stage logs") and I walked into it anyway. The corrected comparison is
part 7; part 6 is left as it was written.

---

## Part 1 — the instrument: `vdmdump.exe`

A VDM maps the guest's V86 address space at process linear 0 (which is why
`src/host/main.c` can write `g_in.bda = (uint8_t *)0x400`). Stock ntvdm is an ordinary
Win32 process with the same property, so a second process with `PROCESS_VM_READ` can read
a *live* VDM from outside — no debugger, no ntsd, nothing to wedge.

`tools/vdmdump/vdmdump.c` (built by `scripts/build-vdmdump.sh`, XP-safe: no CRT, console
subsystem, kernel32-only imports) attaches to a named running process and records:

| # | what | why |
|---|---|---|
| 1 | region map of the whole user address space (`VirtualQueryEx`) | to see where the 16-bit heaps are at all |
| 2 | linear `0x00000000..0x00110000`, raw → `.bin` | the guest's low megabyte + HMA |
| 3 | byte-needle hits anywhere in committed memory | **a Win16 segment need not live under 1MB**, and assuming it did would have missed the answer |
| 4 | 64KB of context per hit + `--grab` ranges → `.blk` | to bring the high-memory copies home |
| 5 | the target's LDT | selector → base/limit/type, the other half of the layout |

The LDT read is `NtQueryInformationProcess(ProcessLdtInformation)` and **it works on XP for
another process** — 512 entries came back first try. That is a better instrument than
anything we have used on ourselves so far.

The needles come from `tools/ne/needles.py`, which cuts 32-byte runs out of each segment
image of the NE file **avoiding every byte a relocation rewrites** — expanding each chained
record's chain (the word at a site is the offset of the next, `0xffff` ends it) exactly as
session 32 part 1 established — and rejecting low-entropy runs, because a 32-byte run of
`0x00` matches half of memory and proves nothing.

## Part 2 — the run, and the bracket

`scripts/bm/stockdump.bat` + `scripts/bmstockdump.sh`: log the IFEO `Debugger` value, drop
it, launch `SYSEDIT.EXE` (so XP starts a *stock* WOW VDM), wait, dump `ntvdm.exe` from
outside, restore the key, prove it is back. The restore is on every exit path and the host
side **fails loudly unless the collected state file proves the key is restored**, because
the standing hazard on this box is that an absent IFEO key silently turns every later test
into a stock run whose logs look entirely plausible.

```bash
./scripts/bmstockdump.sh              # ~40s end to end; artefacts in build/stockdumps/<time>/
python3 tools/ne/dumpscan.py guest/ne/krnl386.exe build/stockdumps/<time>/stockdump
```

It worked on the first run. `IFEO Debugger verified RESTORED`.

## Part 3 — what stock's memory says

`tools/ne/dumpscan.py` reads the `.bin` and `.blk` as one sparse address space, proposes
candidate bases from unique windows taken across the *whole* segment image, and scores each
candidate against the file.

> ⚠️ The first cut probed only the first 0x2000 bytes of each image and **lost segment 1's
> other copy entirely** — a relocated copy differs most densely at its start (krnl386's
> segment 1 has 160 patched bytes in its first 0x100). Probe the whole image.

| krnl386 segment | live base | selector | limit | where |
|---|---|---|---|---|
| 1 (code, PRELOAD, fixed) | `0x00008aa0` | `0x01ff` code (+ `0x01af` data alias) | `0xb8bf` / `0xd7ff` | **low megabyte** |
| 4 (DGROUP, DATA) | `0x000162a0` | `0x0217` data | `0x149f` | **low megabyte** |
| 2 (MOVEABLE, DISCARDABLE) | `0x02960100` | `0x0207` code | `0x3eff` | high, private RWX `0x02950000` |
| 3 (MOVEABLE, DISCARDABLE) | `0x0295ee80` | `0x020f` code | `0x127f` | high, same region |

Two selectors over segment 1 at the same base — a **data alias with the full segment length
(`0xd7ff`) and a code selector with a shorter limit (`0xb8bf`)** — is the shape a loader
needs: you cannot write an image or apply relocations through a code selector.

Pristine, *unrelocated* images of segments 2, 3 and 4 also exist, in the low megabyte, at
**file-image-relative offsets from `0x000895f0`** (`0x895f0 + file_off` lands on each one to
the byte). The head of that staging area has since been reused by other allocations, which
is why segment 1's copy there scores only 0.62.

## Part 4 — the MCB walk: it is a DOS arena block called "krnl386"

Walking the MCB chain in the dumped low megabyte:

```
mcb 020a M owner=0008 size=  958 para  data 0x020b0..0x05c90
mcb 05c9 M owner=05ca size=  162 para  data 0x05ca0..0x066c0  COMMAND
mcb 0674 M owner=05ca size=   87 para  data 0x06750..0x06cc0
mcb 06cc M owner=0896 size=   74 para  data 0x06cd0..0x07170
mcb 0717 M owner=0718 size=  381 para  data 0x07180..0x08950  KB16
mcb 0895 Z owner=0896 size=38761 para  data 0x08960..0x9fff0  krnl386   <-- everything
```

Segment 1 (`0x8aa0`), DGROUP (`0x162a0`) and the staged file image (`0x895f0`) are **all
inside the last block**, owned by PSP paragraph `0x0896`, named `krnl386`. The PSP itself
has a selector: `0x018f`, base `0x00008960`, limit `0x100`.

So stock's krnl386 is loaded the way a DOS program is loaded — one arena block, a PSP, the
fixed image just above it — and the Win16 global heap above 1MB holds only what is moveable.

## Part 5 — the fixups prove which copies are live

At every byte where segment 2's copy at `0x02960100` differs from the file:

```
0x01ff x323     <- segment 1's code selector
0x020f x1       <- segment 3's selector
0x0217 x1       <- segment 4's selector
(6 other values, x1 or x2)
```

325 of 331 written words are exactly the selectors from the table in part 3. That is a
relocated segment and nothing else, and it confirms the selector assignment independently
of the LDT read.

## Part 6 — what this means for our loader ⚠️ REFUTED IN PART 7

Our bind stage (session 30-31) already builds selectors for all four KERNEL segments and
copies the images — but at bases `0x02950000 / 0x02990000 / 0x029a0000 / 0x029b0000`. All
four, high. Stock puts segment 1 and DGROUP in the conventional arena at paragraph-aligned
linear addresses.

That is a candidate root cause for the whole `LoadSegment` failure, and it fits the
evidence session 32 could not explain:

- `[bp+6]` into `LoadSegment` is a **selector into the arena**, and the file-open path is
  never taken — consistent with segment images being expected to be reachable *through the
  arena*, which is exactly where stock's staged image is.
- krnl386's own relocation pass converts **paragraphs → selectors**
  ([session 32](session-32.md)). A segment loaded at `0x02950000` has no paragraph.

## Part 7 — the correction, and the comparison that actually holds

Part 6 compared stock against `WOWTRY: selector stage` — `wow_probe_selectors()`, whose own
source comment says its segments get host memory "which is NOT where execution will want
them". **The stage that runs is `wow_place_v86()`**, and it has always put krnl386 in
conventional memory. From the same run's `wow_ldt.txt`:

```
WOWV86: seg 1 CODE len=0xd7fa alloc=0xd81c -> para 0x0643 (linear 0x00006430)
WOWV86: seg 2 CODE len=0x3ee2 alloc=0x3efc -> para 0x13c6 (linear 0x00013c60)
WOWV86: seg 3 CODE len=0x1278 alloc=0x1292 -> para 0x17b7 (linear 0x00017b70)
WOWV86: seg 4 DATA len=0x1ba2 alloc=0x1dbc -> para 0x18e2 (linear 0x00018e20)
WOWV86: NE header image at para 0x9000 = SS 0x8f00 + 0x1000 (0x4000 bytes from file offset 0x400)
WOWV86: krnl386 PSP/arena block at para 0x1abf size 0x7440 paras
```

So "we place all four above 1MB" is false. The corrected comparison:

| | stock | ours |
|---|---|---|
| PSP / arena block | para `0x0896`, data `0x8960..0x9fff0` (the top of conventional memory) | para `0x1abf`, `0x1acf0..0x8eff0` |
| segment 1 | `0x8aa0` — **inside** the arena block, `0x140` above the PSP | `0x6430` — its own `dos_alloc`, **below** the arena |
| DGROUP | `0x162a0` — inside the arena | `0x18e20` — below the arena |
| segments 2, 3 | above 1MB in the global heap | `0x13c60`, `0x17b70` — low |
| **staged file image** | **`0x899f0`, from the NE header on, `0x16440` bytes** | **`0x90000`, `0x4000` bytes — truncated by `WOW_HDRIMG_PARAS`** |
| selector over it | `0x01ef`, base `0x899f0`, limit `0xfffe` | krnl386 builds its own over `base(SS)+SP` |

Two of those rows are steady-state artefacts rather than loader decisions: segments 2 and 3
are `MOVEABLE|DISCARDABLE`, and moving them into the global heap is precisely what
krnl386's own `LoadSegment` does — the pass we fail in. The dump shows the destination, not
the starting layout.

The row that is a **loader decision, and is ours**, is the staged image. `hlen` is computed
as `img_len - hdr` = `0x16440` and then clamped to `WOW_HDRIMG_PARAS * 16 = 0x4000`.
Segment 1's image starts at file `0x2040` (inside what we keep) and **segments 2, 3 and 4
start at `0xf880`, `0x137a0` and `0x14a60` — all past the cut**. Stock keeps all of it, and
in stock the segment images are recoverable byte-for-byte at `0x895f0 + file_offset`.

That also explains, without any new mechanism, why `LoadSegment(2)` reads code as
relocation records while `LoadSegment(1)` succeeds.

⚠️ It also contradicts session 32 part 17's reasoning for putting the stack+window at the
top of memory ("a small arena forces krnl386 to use the high global heap"): stock's arena
is 620 KB — the whole top of conventional memory — and krnl386 *still* put segments 2 and 3
in the high heap. Arena size is not what decides that.

## Part 8 — the fix, and what it did not do

`wow_place_v86()` now stages the **whole** file image: `WOW_HDRIMG_PARAS` (a constant
`0x400`) is gone, replaced by a size computed from the file, and the stack+window block is
sized to hold it. It also logs, per segment, where that segment's image landed and whether
it is resident — because the old line printed the *already-clamped* length, so the
truncation was invisible in it. A number that cannot show the fault it exists to catch is
not an instrument.

```
WOWV86: NE header image at para 0x89bc = SS 0x88bc + 0x1000 (0x16440 bytes from file offset 0x400)
  staged 0x16440 of 0x16440 bytes = 0x1644 paras (WHOLE FILE)
WOWV86:   seg 1 file 0x02040 -> staged linear 0x0008b800 RESIDENT
WOWV86:   seg 2 file 0x0f880 -> staged linear 0x00099040 RESIDENT
WOWV86:   seg 3 file 0x137a0 -> staged linear 0x0009cf60 RESIDENT
WOWV86:   seg 4 file 0x14a60 -> staged linear 0x0009e220 RESIDENT
```

★ Those addresses land **within `0x1d0` of stock's** (`0x8b630 / 0x98e70 / 0x9cd90 /
0x9e050`), and the staged image ends at `0xa0000` exactly, as stock's does — two loaders
arriving at the same place from opposite directions. That is the strongest corroboration in
this session that the staging area is real and is where it belongs.

**And the wall did not move.** The run is behaviourally identical to the one before it:

| | before | after |
|---|---|---|
| `ExitKernelThunk` | `(1)` from `0x9880` | `(1)` from `0x9880` |
| wow32 counters | `ok=4 decl=4 unimpl=2` | `ok=4 decl=4 unimpl=2` |
| committed code selectors | `0x1ad00 limit 0xd7ff`, `0x03b10100 limit 0x3eff` | identical |
| last PM step | `0x51` | `0x4f` |

**So the truncation is not the cause**, and the reason is visible in that table: *segment 2
already had a block and a selector with limit `0x3eff` — segment 2's exact size, and stock's
exact limit for it — before this change.* krnl386 is getting segment 2 allocated and
described; the failure is after that, not for want of the image.

The change is kept: it fixes a real defect (three of four segment images were absent from
the staging area), it is non-regressive here and on the DOS guest class
(`selftest.com`, clean; 209 NE checks, 0 failed), and anything that reads the staged image
depends on it. It is not a fix for `ExitKernelThunk(1)`.

⚠ It leaves one landmine, logged loudly rather than quietly fixed: `CX` still declares
`0xf880` bytes of scratch above the header, and that region is now staged image. The run
does not touch it today, so it was left alone — one change per run — and the log says
`⚠ OVERLAPS THE STAGED IMAGE` every time.

## Part 9 — the module database: 10 bytes per entry, and krnl386 builds it

Reading LoadSegment (`seg1:0x90d9`) rather than instrumenting it:

```
90fa  mov es,[bp+0xa]     ; hModule -- the module database block
90fd  mov si,[bp+8]       ; segment number
9100  dec si
910b  shl si,1 / mov bx,si / shl si,1 / shl si,1 / add si,bx    ; ★ si * 10
9115  add si,es:[0x22]    ; + ne_segtab
9145  mov ax,es:[si+8]    ; ★ the HANDLE -- two bytes the file's entry does not have
```

**Stride ten.** The file's segment table has 8-byte entries; the in-memory one has ten,
the extra word being the segment's handle. Stock's dump proves it outright — its KERNEL
module database is at linear `0x196c0`, a `0xa60`-byte block with its own selector
(`0x01f7`), and read with stride 10 it reproduces krnl386's file segment table exactly:

| | sector | length | `[+8]` |
|---|---|---|---|
| seg 1 | `0204` | `d7fa` | **`01ff`** |
| seg 2 | `0f88` | `3ee2` | **`0206`** |
| seg 3 | `137a` | `1278` | **`020e`** |
| seg 4 | `14a6` | `1ba2` | **`0217`** |

Sector and length match the file to the byte, and `[+8]` is exactly the four selectors
stock's LDT holds for krnl386's segments (part 3). It also *explains the symptom
perfectly*: with 8-byte entries, index 0 is at offset 0 whatever the stride — so segment 1
would load and every later segment would be read at `segtab + 10n` out of a stride-8 table
and get a nonsense sector and length. Segment 1 loads, segment 2 does not.

⚠️ **So I widened the staged table to 10 bytes per entry, and it was wrong.** One run said
so, through the selector limit of the block krnl386 allocates for its module database:

| | module block |
|---|---|
| ours, before | `base=0x0001ad00 limit=0x00000a5f` |
| **stock** | `base=0x000196c0 limit=0x00000a5f` ★ |
| ours, with the table widened | `base=0x0001ad00 limit=0x00000a7f` |

Our module block was **already byte-identical in size to stock's**. krnl386 allocates that
block, copies the header we stage into it, and does the widening *itself* — so pre-widening
made it widen an already-widened table. The run died at PM step `0x32` against `0x4f`, with
no error message at all: further from an answer and quieter about it.

Reverted, and the baseline reproduced exactly (`0xa5f`, step `0x4e`, `ExitKernelThunk(1)`).
The `⚠ DO NOT WIDEN` note is in the source at the place someone would next try it.

Same lesson as session 32's relocation finding, in a second place: **the loader's job is to
stage the file's header, not to pre-chew it.** krnl386 is its own loader.

## Part 10 — where the failure actually is

Two corrections to session 32's bracket, both from reading rather than measuring:

1. **`ExitKernelThunk(1)` does not imply `call 0x8cb6` returned 0.** `0x92b7` (the read
   failed) does `xor ax,ax` and jumps *past* that call to the same test. Several paths
   reach the same failure label `0x9318`.
2. **The read never happens at all.** LoadSegment reads a segment with `mov ah,0x3f` +
   `call 0x4ff2` (its INT 21h thunk) at `seg1:0x9272`, and **no `INT21h AH=3f` appears in
   any run's log** — not once, in any of the four this session. So LoadSegment bails
   before it ever asks for the bytes, which also puts the staged image out of the frame a
   second time.

Walking forward from there, the decision point is `seg1:0x91ca`:

```
91c0  push es / push es / push si / push [bp+8] / push ax / push [bp+4]
91ca  call 0x937e          ; <-- LoadSegment's outcome
91d3  or ax,ax / je 0x91fc  ; -> 0x9303, the not-loaded path
91d7  inc ax / jne 0x91ff   ; -> the normal path, which reaches the read
```

and inside `0x937e` the size handed to the allocator comes from the segment table entry:

```
93c0  mov bx,es:[si+6]      ; minalloc
93c6  cmp bx,1 / adc dx,dx  ; 32-bit size, 0 meaning 64K
93cb  add bx,2 / adc dx,0
93d9  call 0x4658           ; GlobalAlloc/GlobalReAlloc(handle=[bp-2], dx:bx, 0)
93dd  cmp [bp-2],ax / je 0x93f7   ; same handle back = success
```

That is krnl386's **own global-heap code**, in its own segment 1 — no WOW32 call, nothing
of ours to implement. What we control is the heap it is allocating from: the arena, `CX`,
and `ES` at entry — the geometry session 32 part 17 tuned by hand and part 7 above shows
stock does differently (its arena is a separate high block; ours is the window above the
header, and the staged image now overlaps what `CX` declares).

⚠️ Also corrected: session 32 read `[bp+6]` into LoadSegment as "the arena selector". At
the first call site (`seg1:0xc2f4`) it is simply `push cs`; at the segment-2..4 loop
(`seg1:0xc4f6`) it is whatever `call 0x48b4` returned. It is an argument, not an identity.

## Part 11 — ★★★ THE WALL IS DOWN: segments 2 and 3 load

Following LoadSegment forward from part 10 rather than guessing, with three new
instruments built along the way (all of which paid for themselves in one run each):

- **seg1-relative breakpoints.** krnl386's PM copy of segment 1 lands at a different
  linear address every run, so a breakpoint list of absolute addresses is wrong by the
  next run. `pmbp.txt` mode bit 1 now means "this is an offset in krnl386's segment 1",
  resolved when that selector is committed. Every reading below used it.
- **`@es:si` and `@ds:di` in the hit report.** It dumped only `ds:si` and `es:di` — the
  string-move pairing — and the code under investigation walks structures with
  `les si,[bp+x]`. The pair that was missing was the one on the screen.
- **A DS-relative dump column** (mode bit 2), because the variables worth watching are
  krnl386's DGROUP variables and DGROUP moves between runs.

### The reading that broke it open

Breakpoint at `seg1:0x947f`, where LoadSegment has just done `mov ds,bx` and is about to
`rep movsd` the segment image in from `DS:0000`:

```
DPMI-BP HIT ... cs:eip=0x01cf:0x0000947f ECX=0x00003ee2      <- segment 2's length
  dsbase=0x00089bc0
```

`0x89bc0` is **our staged NE header** (`WOWV86: NE header image at para 0x89bc`). krnl386
was copying `0x3ee2` bytes of *header and tables* and calling it segment 2. There is no
file read anywhere in this path — which is why `AH=3f` never appeared in any log.

### Why the source never advanced

The staged block is walked by *reclaiming* what has been consumed:

```
c4a3  sub ax,[0x5a6]     ; ax = the size krnl386 computes, minus what WE declared in CX
c4a7  mov [0x5a4],ax     ; the GAP, in paragraphs
c4dd  call 0xcf9e        ; rep movsd everything above the gap DOWN to offset 0
c4e5  push [0x59e] ...   ; then LoadSegment copies the segment from offset 0
```

Read out of krnl386's own DGROUP at the loop head (`dump@ds:0x598`):

```
[0x59e] = 0x01b6   the staged block's handle
[0x5a4] = 0x0000   ★ the gap
[0x5a6] = 0x0f88   what we declare -- CX >> 4
```

**Session 32 set that gap to zero on purpose**, to stop the reclaim's `rep movsd` from
running over krnl386's own code, and recorded the reasoning at length. It fixed the crash
and it broke the load: with the gap zero the block never advances, so *every* segment is
copied from offset 0 of the staged image. The hazard it was avoiding is also gone —
that `rep movsd` spanned the whole arena (`base 0x1bbe0 limit 0x8441f`) when the block
was the arena; now the block is the staged image (`0x89bc0 + 0xf880`) and krnl386's code
copy at `0x1ad00` is outside it.

### The fix, and the two-step measurement that landed it

`CX` now declares the window **minus the image that precedes the first segment the loop
loads**, so the first reclaim discards exactly that:

| gap | what happened |
|---|---|
| `0` (session 32) | source pinned at the header; `LoadSegment(2)` fails; `ExitKernelThunk(1)`; step `0x4e` |
| `0x1c40` — the header and tables | source advanced by one step too few: `@ds:si` was **segment 1's** image. Two copies, no error, step `0x56` |
| `0xf480` — header **+ segment 1** | ★ `@ds:si` = `20 77 61 73 20 64 65 73 69 67 6e 65 64 …` — `" was designed for a previous ver"`, **segment 2's own image at +0x4a, byte for byte** |

The middle row is the interesting one: segment 1 is loaded by `seg1:0xc2f4`, *before* the
loop and without its compaction step, so it never consumes from the block. The loop's own
per-iteration arithmetic (`0xc4b8..0xc4d8`) handles every segment after that.

### Where it is now

```
[CODE sel 0x01cf base=0x0001ad00 limit=0x0000d7ff]   segment 1
[CODE sel 0x01d7 base=0x03b10100 limit=0x00003eff]   segment 2   ★ new
[CODE sel 0x01df base=0x03b0ee80 limit=0x0000127f]   segment 3   ★ new
```

against stock's `0x02960100 / 0x3eff` and `0x0295ee80 / 0x127f`: **the same limits and the
same offsets within the heap block** — `+0x0100` and `-0x1180` — arrived at independently
by krnl386's own allocator from our inputs. `ExitKernelThunk` is **gone from the run
entirely**, and the furthest PM step went `0x4e → 0x5f`.

The new frontier is a different and much better-shaped problem: at `seg1:0xc5ae`, while
setting interrupt vectors (`AX=0x2510`), krnl386 issues **native BOP `0x1f`**, which we do
not implement and step over. That is a named, unimplemented call, not a mystery.

Regressions: 209 NE checks 0 failed, `selftest.com` clean on the rig, imports clean.

## Part 12 — the "unimplemented BOP" was a swallowed `INT 21h`

Part 11's frontier was `native BOP 0x1f at seg1:0xc5ae`. **There is no BOP `0x1f` in
krnl386** — a scan of all four segments finds thirteen BOP sites and they are `0x51`,
`0x53`, `0x56` and `0xfe`. The file has `cd 21 1f 07` at that offset: an **`INT 21h`**,
and the `0x1f` is the `pop ds` after it.

Our patcher rewrites `CD nn` (two bytes) as `C4 C4` (two bytes) — there is nowhere to put
the vector, the instruction is the same length — and keeps it in a side map keyed by
**linear address**. That is sound until the guest *moves* the patched bytes, and krnl386
copies each of its segments to a block of its own. The `C4 C4` travels; the map entry
stays behind. The handler then reads `bb[2]` — the next instruction's first byte — as a
BOP code.

Two in one run, and they are the same pair:

```
WOWBOP 0xb8 at seg1:0xc59b   AX=0x3510   file: cd 21     get INT vector 10h
WOWBOP 0x1f at seg1:0xc5ae   AX=0x2510   file: cd 21     set INT vector 10h
```

`0xb8` was `mov ax,`; `0x1f` was `pop ds`. Both INT 21h calls were being silently swallowed
("UNIMPLEMENTED, STEPPED OVER — the call did NOT happen"), so krnl386's INT 10h handler was
never installed.

**The fix recovers the vector from the module's own file image** — the one copy of those
bytes nothing has rewritten — and it is self-verifying, firing only when the memory really
is `C4 C4`, the address really is inside a segment whose base we recorded, and the file
really has `CD` there. A genuine BOP site has `C4 C4` in the file too, so it declines
itself; the log line for the real `0x51` thunk now reads `[file seg1+0x2bf1 = c4 c4]`.
The recovered vector is written into the map, so it costs one lookup once.

After it:

```
INT21h AH=35 (PM) get PM vector 0x00000010 -> 0x0000014f:0x00000030
INT21h AH=25 (PM) set PM vector 0x00000010 =  0x000001cf:0x00004fe4
```

krnl386 has installed its own INT 10h handler. PM step `0x5d → 0x63`, a fifth WOW32 call
serviced (`GetCurrentDirectory`), and **zero** remaining BOP misreads.

⚠️ The patcher's own count is worth keeping an eye on: it reports `patched 2 INT sites at
+0x2b2 +0x8c6` for the whole of segment 1, which is why the offsets are now logged
alongside the count. Two in 55 KB of KERNEL code is either correct or a broken scan, and a
count alone cannot tell you which — the offsets can, because they are checkable against a
disassembly of the same bytes.

## Part 13 — where it stands: krnl386 is alive

```
---- at end of run ----
Image Name                   PID Session Name     Session#    Mem Usage
ntvdmhost.exe               1656 Console                 0     24,616 K
```

**The host is still running when the run ends** — the first time in this investigation.
Every previous run ended with `No tasks running`. krnl386 is not dying; it is waiting.

The last PM activity is `seg1:0xc5ee`, and the code there says exactly what it is waiting
for:

```
c5d8  mov bl,6 / mov ax,0x202 / int 31h     ; DPMI get exception handler, exception 6
c5e7  mov bl,6 / mov ax,0x203 / int 31h     ; DPMI set exception handler -> cs:0xc5f2
c5f0  0f ff                                 ; ★ UD0 -- a deliberate invalid opcode
c5f2  push bp / mov bp,sp ...               ; the handler it just installed
```

Both INT 31h calls are serviced (`setEXC 0x06 = 0x01cf:0xc5f2`). Then krnl386 **executes an
invalid opcode on purpose** to check that the handler it just registered is actually
reached — and we never deliver it, so the guest sits there.

⇒ The next task is **DPMI exception reflection**: a fault in the client has to be delivered
to the handler registered through `0203`, with the DPMI exception frame, so the handler can
return past the faulting instruction. That is a specified interface (DPMI 0.9 §exception
handling), not a guess, and krnl386 is asking for it in the simplest possible way — one
invalid opcode with a handler that just resumes.

Regressions after all of it: 209 NE checks 0 failed, `selftest.com` clean on the rig,
imports clean.

## What is NOT proven

- ~~That the truncated staging image is THE cause.~~ **TESTED AND REFUTED — part 8.** It
  was a real defect in the right place and it explains the symptom perfectly; it is not
  what `ExitKernelThunk(1)` is about. The dump is one settled steady state, not a trace of
  the load: it shows where things ended up, not how they got there.
- **That stock's staged image was placed by the loader** rather than read in by krnl386
  itself. The file-relative arithmetic from `0x895f0` is exact; the causality is not. Stock's
  module database is only `0xa60` bytes and is a *separate block* from that staging area, so
  the two are not the same thing.
- **Which path inside `LoadSegment` is actually taken.** Part 10 narrows it to `call 0x937e`
  at `0x91ca` by elimination (the `AH=3f` read never happens) and by reading the code, not by
  a breakpoint. The breakpoint is the next run to make, not a conclusion already drawn.
- **What overwrote the head of the staging image**, or whether staging is what
  `LoadSegment` reads from. The file-relative arithmetic is exact, but that is a
  correlation.
- **Nothing about the ordering** of allocation, relocation and selector creation.
- Our `0x01B7` and stock's `0x01b7` (base `0x02920000`, limit `0x2607f`) are almost
  certainly unrelated — the same ordinal selector in two different processes.

## Files added

| file | what |
|---|---|
| `tools/vdmdump/vdmdump.c` | read a live VDM from outside: regions, low 1MB, needles, blocks, LDT |
| `scripts/build-vdmdump.sh` | XP-safe build (no CRT, console subsystem, kernel32-only) |
| `tools/ne/needles.py` | cut relocation-free, high-entropy needles from an NE's segments |
| `tools/ne/dumpscan.py` | locate an NE's segments in a dump; score and diff each copy |
| `scripts/bm/stockdump.bat` | rig side: IFEO bracket, launch SYSEDIT, dump, restore, prove |
| `scripts/bmstockdump.sh` | host side: build, deploy, drive, collect, verify the key is back |

`tools/ne/nedis.py` also got the fix session 32 asked for: **branch targets are masked to 16
bits**. capstone computes `target = address + rel` without wrapping, so disassembling from a
high offset printed `call 0x11493` for what the CPU executes as `call 0x1493` — a
non-existent address, in a tool whose whole job is to name addresses.

---

## ▶ RESUME HERE — session 33 handoff

### Where it is

**krnl386 is alive.** `ntvdmhost.exe` is still running when the run ends (24 MB, PID in
`wow_alive.txt`) — every run before this session ended with `No tasks running`. It is not
dying, it is **waiting**, at `seg1:0xc5f0`:

```
c5d8  mov bl,6 / mov ax,0x202 / int 31h   ; DPMI get exception handler, exception 6
c5e7  mov bl,6 / mov ax,0x203 / int 31h   ; set it -> cs:0xc5f2   (we service both)
c5f0  0f ff                               ; ★ UD0 -- a deliberate invalid opcode
c5f2  push bp / mov bp,sp ...             ; the handler it just registered
```

krnl386 registers a #UD handler and then executes an invalid opcode **on purpose** to check
it is reached. We never deliver it. PM step `0x63`, `wow32{ok=5 decl=4 unimpl=1}`, and
`ExitKernelThunk` no longer appears in the run at all.

Segments 1, 2 and 3 load and get code selectors at the same heap offsets stock uses; the
DOS bar is untouched (`selftest.com` clean, 209 NE checks 0 failed).

### The next run

**Deliver DPMI exceptions to the handler registered through INT 31h `0203`.** A fault in
the client has to reach that handler with the DPMI exception frame, so the handler can
return past the faulting instruction. It is a specified interface (DPMI 0.9, exception
handling) rather than a guess, and krnl386 is asking for it in the simplest possible way:
one invalid opcode, a handler that just resumes. `0202`/`0203` are already serviced — the
log shows `setEXC 0x06 = 0x01cf:0xc5f2` — so what is missing is only the delivery.

Then: **segment 4 / DGROUP**. Segments 2 and 3 get selectors; 4 should follow the same way.

### What changed in the tree (UNCOMMITTED)

| file | change |
|---|---|
| `src/host/main.c` | stage the WHOLE file image, not `0x4000` (part 8); **the arena gap** — `CX` declares the window minus everything before segment 2, which is what walks the staged image (part 11); recover a lost INT vector from the module's file image (part 12); record every segment's PM base; seg1-relative breakpoints; DS-relative dump column; `@es:si`/`@ds:di` in the hit report; the INT patcher logs the offsets it patched |
| `tools/ne/nedis.py` | branch targets masked to 16 bits (`call 0x11493` → `call 0x1493`) |
| `tools/vdmdump/`, `scripts/build-vdmdump.sh`, `scripts/bmstockdump.sh`, `scripts/bm/stockdump.bat`, `tools/ne/needles.py`, `tools/ne/dumpscan.py` | new — the stock-oracle toolchain (parts 1-2) |
| `docs/log/sessions/session-33.md`, `README.md` | this file, and index rows for 31-33 |

There is a `⚠ DO NOT WIDEN` note and a `⚠ TRIED, MEASURED, REFUTED` note in `main.c` at the
two places someone would next try the refuted things.

### Ruled out this session — do not re-try

| lead | verdict |
|---|---|
| Staged image truncated to `0x4000` | real defect, **fixed** — but the run was byte-identical afterwards, so it was not the cause of anything |
| Widen the staged segment table to 10 bytes/entry | **refuted** — krnl386 widens it itself; our module block was already `limit=0x0a5f`, stock's exact size, and pre-widening made the run die *earlier* (`0x32` vs `0x4f`) |
| `[bp+6]` into LoadSegment is "the arena selector" | it is an argument — `push cs` at one call site, `call 0x48b4`'s result at the other |
| WOW32 `0xc0` is the blocker | its caller (`seg1:0x30da`) does `pop ds / retf`; **the result is never tested** |
| `AH=3f` reads the segment from the file | **it never happens** — LoadSegment `rep movsd`s the image in from `DS:0000` |

### The loop

```bash
./scripts/build.sh
ARCHIVE=build/wowruns ./scripts/bmwow.sh              # a WOW round
PMBP=1 ARCHIVE=build/wowruns ./scripts/bmwow.sh       # ...keeping pmbp.txt armed
./scripts/bmstockdump.sh                              # re-take the stock dump (~40s)
python3 tools/ne/dumpscan.py guest/ne/krnl386.exe build/stockdumps/<time>/stockdump
```

Regression, every time:

```bash
./tools/dostest/run.sh                          # 209 NE checks + batteries, off-VM
./scripts/check-imports.sh build/ntvdmhost.exe  # XP-safe imports
TIMEOUT=200 ./scripts/bmqueue.sh selftest.com   # the OTHER guest class, real hardware
```

### The instruments — use them, they are why this session moved

- **`pmbp.txt` mode is a bit field.** bit 0 (1) = one-byte site; **bit 1 (2) = the address is
  an OFFSET IN krnl386's PM segment 1**, resolved per run — krnl386's copy moves every run,
  so never write an absolute krnl386 address down again; **bit 2 (4) = the dump column is
  DS-relative**. Columns are `<addr> <dump> <skip> <mode> <rep>`, CRLF, `rep=1` to stay
  armed. Example that produced part 11: `947f 0 0 2 1`.
- The hit report resolves **all four** segment/index pairings (`ds:si`, `es:di`, `es:si`,
  `ds:di`) and prints 64 bytes of stack.
- An unrecognised BOP prints what the **module's file image** has at that offset, which is
  how part 12's swallowed `INT 21h` was identified in one run.
- `tools/ne/nedis.py <file> 1 <start> <count>` (count, not end); `--callers 0xNNNN`;
  `--wowfunc 0xNN`.
- `./scripts/bmstockdump.sh` brackets the IFEO key and **fails loudly unless the restore is
  proven**. `build/stockdumps/130913/` is the dump this session was built on — keep it.

### Method notes earned this session

- **Read `WOWV86:` lines, never `WOWTRY: selector stage`.** The selector stage is not the
  stage that runs; reading its bases cost this session a wrong headline (parts 6-7).
- **A count is not a location.** "patched 2 INT sites" could not distinguish a correct scan
  from a broken one; the offsets can. Same for "staged 0x4000 bytes", which printed the
  already-clamped length and so could never show the truncation it was reporting.
- **When krnl386 does something itself, do not do it for it.** Relocation (session 32), the
  module database (part 9) — twice now, the loader's job was to stage, not to pre-chew.

### Rig left as

IFEO `Debugger` **set and verified**, `wowtry.flag` present, `pmbp.txt` **removed**,
`dostrace.flag` absent, latest build deployed (`md5` in the last `bmwow.sh` output).
