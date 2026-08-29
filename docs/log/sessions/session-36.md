# Session 36 — the bootstrap loads six system modules, and names the seventh

**Date:** 2026-08-29 · **Branch:** `m9/completeness` · **Issue:** [#128](https://github.com/MrMatthewLayton/ntvdmex/issues/128)

**In one paragraph.** The frontier moved from an address to a **module name**. Session 35's
deterministic `WOW32_UNIMPL_RET = 0` — written but never run — turned out to be the largest
single step this epic has taken: krnl386 goes from loading **one** system module to **six**.
Behind it were two walls that were both ours and both instruments rather than mechanisms: a
protected-mode `INT 15h` with no arm at all, and a fatal-message decoder that stopped at the
first `\r` and so printed the one part of krnl386's complaint that is identical for every
module. With those cleared, krnl386 says in its own words that **`COMM.DRV`** is the module it
cannot load, and one repeating breakpoint reads back the loader's return code for all six.
A third instrument defect was found on the way and cost a run: **a one-shot breakpoint was the
only kind that re-planted itself under a standing guest.**

---

## ★★ Where the run now stops: `COMM.DRV`, loader returns 0

`seg1:0xcc8e` loads one system module (`ret 2`, the name's DGROUP offset in DI). At `0xcc9f` it
`lcall`s the loader and at `0xcca4` it tests the result:

```
cca4  3d 0200    cmp ax,2      je error     ; file not found
ccab  3d 0b00    cmp ax,0xb    je error     ; invalid EXE
ccb2  3d 0f00    cmp ax,0xf    je error     ; invalid app
ccb9  3d 0400    cmp ax,4      je error     ; too many open files
ccc0  3d 2000    cmp ax,0x20   jae ok       ; anything below 0x20 is an error too
ccc5  ...        call 0xc9ff   ; the reporter
ccca  b8 0100    mov ax,1 / call 0x987a     ; ExitKernelThunk(1)
```

One breakpoint at `seg1:0xcca4`, mode 2 (offset into seg1's PM copy), **rep 1**:

| DI | module | AX |
|---|---|---|
| `0x1641` | `SYSTEM.DRV` | `0x01b7` ✓ |
| `0x164c` | `KEYBOARD.DRV` | `0x0277` ✓ |
| `0x1659` | `MOUSE.DRV` | `0x0297` ✓ |
| `0x166f` | `VGA.DRV` | `0x02b6` ✓ |
| `0x1677` | `SOUND.DRV` | `0x02df` ✓ |
| `0x1681` | **`COMM.DRV`** | **`0x0000`** ✗ |

`0` is **not one of krnl386's named codes** — the loader returned plain failure, so the
"file not found / bad EXE / too many handles" readings are all excluded by measurement.

### The module table, read out of krnl386's DGROUP
`mov es, cs:[0x30]` before the `repne scasb` means DI indexes the **automatic data segment**
(segment 4), not seg1. At `seg4:0x1641`:

```
SYSTEM.DRV  KEYBOARD.DRV  MOUSE.DRV  WIFEMAN.DLL  VGA.DRV  SOUND.DRV  COMM.DRV
USER.EXE    GDI.EXE       WINNLS.DLL WOWEXEC.EXE  WOWSHELL KEYBOARD  #5  #6  KERNEL
```

`WIFEMAN.DLL` and `WINNLS.DLL` are **correctly skipped**: `seg1:0xcb74` calls WOW32 `0xcf` for
the language ID and loads them only for `0x411`/`0x412`/`0x404`/`0x804`/`0xc04` — the Far
Eastern locales. Answering `0` is the right answer on an English box, so this is the bootstrap
working, not a gap.

### ★ What is different about `COMM.DRV`, and it is the lead
It is the **only one of the six that takes a `#NP` demand-load fault**:

```
GH#18: PM-FAULT REFLECTED class=0000000b ...
  EXC: -> client handler for 0x0b at 0x1cf:0x36c5 frame{err=0x02fc cs:ip=0x1d7:0x04b2 ...}
GH#128: EXC RETURN -> resume 0x1cf:0x37c7
```

`err=0x02fc` → LDT index `0x5f` → selector `0x2ff`. krnl386's own handler runs, allocates
`0x2ff` over a `0x13df`-byte block, and reads COMM.DRV's **segment 2** (`0x13cf` bytes from file
`0x1320`) followed by its **12 relocation records** (`0x60` bytes) — and *then* the load fails.

Segment 2's fixups, decoded from `guest/ne/comm.drv`:

| kind | at | target |
|---|---|---|
| OFFSET | `0x136d` | KERNEL.193 |
| SEGMENT | `0x057c` | INTERNALREF seg 2 |
| SEGMENT | `0x0b3d` | INTERNALREF seg 3 |
| FAR_ADDR | `0x0386` | KERNEL.509 |
| FAR_ADDR | `0x12c3` | SYSTEM.6 |
| FAR_ADDR | `0x0c85` | KERNEL.127 |
| FAR_ADDR | `0x0bf9` | KERNEL.128 |
| FAR_ADDR | `0x0793` | KERNEL.47 |
| FAR_ADDR | `0x079f` | KERNEL.50 |
| FAR_ADDR | `0x0229` | KERNEL.186 |
| OFFSET | `0x06fb` | KERNEL.178 |
| FAR_ADDR | `0x0929` | KERNEL.354 |

⚠ **Every one of those exports EXISTS** — checked against the entry tables of
`guest/ne/krnl386.exe` (399 entries) and `guest/ne/system.drv` (11). So this is **not** a
missing export, and that lead is closed before it was chased. `193` and `178` are the
`ABSOLUTE` magic selectors (`__0040H`, `__WINFLAGS`), and the run does build the whole set
(`INT31h AX=0002 BX=0040/f000/a000/b000/b800/c000/d000/e000`).

▶ **Next bisect:** inside the loader, after the `#NP` path. The question is binary — is the
demand-load itself wrong, or the relocation pass over the demand-loaded segment? A breakpoint
either side of the fixup walk answers it.

▸ Unexplained in the same window, and worth a look:
`LDTSYNC idx 0x34 <- guest wrote base=0x00000000 limit=0x00000310 acc=0x0f INSTALL FAILED
(g_ldt NOT updated) st=0xc000011a`. `acc=0x0f` has P=0 and the "limit" is a selector value —
this is krnl386's **free-selector list link**, stored in the descriptor itself. NT refuses it
(`STATUS_LDT_DESCRIPTOR`) and **our shadow `g_ldt` is then not updated either**, so a read-back
returns stale data. Five per run; four during early free-list construction, one inside the
COMM.DRV window.

---

## Walls cleared, all three ours

### 1. ★★ Protected-mode `INT 15h` had no arm at all
A COMM.DRV segment runs `b4 c0 / cd 15` — `INT 15h AH=C0h`, *get system configuration table* —
in protected mode. Every other BIOS vector krnl386 uses had a PM twin in
`dpmi_service_pm_int()` (`0x10`, `0x11`, `0x16`, `0x1A`, `0x08`, `0x2F`, `0x31`, `0x33`,
`0x41`); `0x15` did not. So the raw `CD 15` reached the `#GP` reflect, was **correctly
identified as a raw INT and patched**, was handed to a function with no arm for it, and fell
out of the bottom as:

```
DPMI: unexpected PM stop event=0x00000004 CS:EIP=0x000002f7:0x00000005
```

The run died two bytes into a driver, naming an address rather than a cause.

The arm now answers **exactly what the V86 arm answers**, for the same reason the `INT 11h`
twin shares its constant: a guest that gets a different machine depending on which mode it
asked from is a guest we cannot reason about.

- `AH=88h` → `0x3C00` KB extended, matching the XMS pool
- `AH=86h` → CF=0 (the PIT already paces us)
- everything else, **`C0h` included** → `AH=86h`, CF=1

⚠ **`AH=C0h` is deliberately refused, not stubbed with a table.** The caller's next
instructions are `jc +0x1d` and, on the no-carry path, `cmp byte es:[bx+2],0xf8` — it reads a
**model byte** out of the table we would have to invent. CF=1 sends it down the path a real
PC/AT without the call takes; a fabricated table sends it down a path chosen by a number we
made up. If a later run shows a driver needs the table, build it from the oracle.

### 2. ★★ The fatal-message decoder stopped at the first `\r`
krnl386's last act before `ExitKernelThunk` is WOW32 `0xc4` — the fatal error box — and the
harness already tried every adjacent word pair in the frame as a far pointer to a string. But
the scan accepted only `0x20..0x7E`, and the **one** string in the frame that names the actual
fault is the formatted body:

```
Please re-install the following module to your system32 directory:\r\n\t\tCOMM.DRV
```

The walk stopped at the `\r` at offset 66, `s[n2] != 0` rejected it as "not a C string", and
the log printed only the **caption** — `"NTVDM KERNEL: Missing 16-bit system module"` — which
is the one part of the message that is the same for every missing module. **The run named the
class of failure and withheld the instance, and the instance is the whole question.**

Now accepts `\t`, `\r`, `\n` in the scan and escapes them on output (a raw CRLF would split one
log line into three and make the message read as unrelated records), with a bound so escaping
cannot overflow `report[2048]`.

### 3. ★★ A one-shot breakpoint was the only kind that re-planted itself
`dpmi_bp_arm()` runs before every PM entry and re-plants anything not currently armed. Its
"never re-plant a site the guest is standing on" guard keys off `g_bp_pending` — and
`g_bp_pending` was set **only** for *repeating* and *skip* breakpoints. So the plain one-shot,
which is the kind you reach for first, had no protection at all.

Measured: a one-shot at `seg1:0xcca4` fired **512 times with byte-identical registers and one
millisecond on the clock**, hit `DPMI_BP_ARM_MAX`, and was then never re-planted — so the
*seventh* time the guest reached that site, which was the COMM.DRV pass the breakpoint existed
to observe, **there was no breakpoint there**. The log showed 512 confident hits and answered
nothing.

`pending` ("the guest is standing on this footprint") and *"should fire again"* were the same
flag. They are now separate: every hit sets `pending`, and a non-repeating, non-skip hit also
sets `g_bp_done[]`, which `dpmi_bp_arm()` honours. Re-measured: **6 hits, one per module.**

> **Method note.** When a PM breakpoint reports many hits, check whether the **registers differ
> between them** before believing the guest is looping. Identical dumps with no elapsed time
> mean the debugger is holding the guest in place. And for a site reached once per iteration of
> a guest loop, set the `rep` column — a one-shot fires on iteration 1, which is almost never
> the interesting one.

---

## ⏹ Tried and removed: krnl386's own `/B` boot log

krnl386 has the stock `WIN /B` switch. Its command-line parser at `seg1:0xc941`
(`mov ds,[0x220] / mov si,0x80` — the PSP command tail) calls `seg1:0xb148` on `/b`, which
builds a path from the Windows directory cached at `[0x504]:[0x50c]`, creates `BOOTLOG.TXT`,
and sets `[0x12b0] = 1`; the printer at `seg1:0xb0d9` then appends every `LoadStart = ` /
`LoadSuccess = ` / `LoadFail = ` line, **failure code included**. `dos_psp_build` leaves the
tail empty, so the parser had always exited at its first `or al,al`.

**Measured, and it does not work here.** `/B` was placed in the tail — confirmed by
`WOWV86: /B in krnl386's command tail` in `wow_ldt.txt` — and **no `BOOTLOG.TXT` appeared** at
any of four candidate paths. The likely reason is that `[0x504]:[0x50c]` is not filled this
early, so `0xb148`'s open fails; and its failure path runs `mov word [0x12b0],0` at
`seg1:0xb12d`, which switches krnl386's own logging off for the rest of the run, **silently**.
One failed open, then nothing — indistinguishable from "the flag did not work".

★ **Do not re-try the obvious `[0x12b0]` poke either.** The printer opens the filename at
`ds:0x12be`, which only `0xb148` ever fills, so a hand-set flag opens an empty name and takes
that same self-disabling path.

The code is removed; the finding is kept as a comment beside `WOWTRY_FLAG` and here. The
`seg1:0xcca4` breakpoint answers the same question, per module, and **does** work.

---

## ⚠ Hazards this session

- **The session started with UNCOMMITTED, UNTESTED work in the tree.** `WOW32_UNIMPL_RET = 0`
  had been written and built but its effect was never read; it turned out to be the biggest
  single advance of the epic (1 driver → 6). Commit before the context ends.
- **A truncated `grep | head -n 20` produced a wrong conclusion** — "`/B` never reached the
  command tail" — from a log where the line was at position 98. Two runs were spent on it.
  If a grep is filtered, say so; if it is truncated, do not conclude from absence.
- The rig's `wowrun.bat` still deletes and collects `BOOTLOG.TXT` (it lives on the share, not
  in git). Harmless, and it makes an absent file distinguishable from an uncollected one.

---

## ★★ Part 2 — COMM.DRV is the first module with a NON-PRELOAD segment

After the first handoff was written the bisect continued, and it has narrowed the failure
to a structural difference that is visible in the files themselves.

### `LoadModule` returns 0, which is "out of memory", not "not found"
`seg1:0xcc9f` is `lcall seg2:0x190a`, a thunk that tails into **`LoadModule`** at
`seg2:0x051d` (`jmp 0x051d` after pushing the `retf 8` at `seg2:0x193b`) — confirming
session 35's identification. `lpParameterBlock` is NULL here (the caller pushes
`xor ax,ax` twice), so the `or bx,cx / je` at `0x1926` skips the parameter-block work.
**AX = 0** then walks the error triage at `seg2:0x0f16` — `cmp ax,0x20 / jb`, `cmp ax,0x15`,
`cmp ax,0x17`, `cmp ax,0xf / jae`, `cmp ax,0xa / jbe` — and falls to the give-up exit
**without** the `WowLoadModule` retry, which is why WOW32 `0x2d` is never called in this
run (session 35 saw it only because stack litter made AX look like `0x17`).

### Where the 0 first appears
krnl386 carries the result in AX and re-tests it with `cmp ax,0x20` at each stage. Six
verified checkpoints, all repeating, in one run:

| checkpoint | SYSTEM | KEYBOARD | MOUSE | VGA | SOUND | **COMM** |
|---|---|---|---|---|---|---|
| `seg2:0x0e25` | 0x01b7 | 0x0277 | 0x0297 | 0x02b6 | 0x02df | **0x0000** |
| `seg2:0x0e55` | " | " | " | " | " | **0x0000** |
| `seg2:0x0e88` | " | " | " | " | " | **0x0000** |
| `seg2:0x0f16` | " | " | " | " | " | **0x0000** |
| `seg1:0xcca4` | " | " | " | " | " | **0x0000** |

`seg2:0x0e11` never fires — `cmp [bp-0x24],0 / je 0x0e24` skips it. **So AX arrives at
`0x0e25` already 0**, and the origin is upstream of `seg2:0x0e0b`.

★ At every one of those checkpoints **`DI = 2` for all five successes and `DI = 4` for
COMM.DRV** — which turns out to be the segment count.

### ★ The structural difference, from the files
| module | segs | segment flags |
|---|---|---|
| SYSTEM.DRV | 2 | CODE,PRELOAD,RELOCS + DATA,PRELOAD |
| KEYBOARD.DRV | 2 | CODE,PRELOAD,RELOCS + DATA,PRELOAD |
| MOUSE.DRV | 2 | CODE,MOVEABLE,PRELOAD,DISCARDABLE + DATA,PRELOAD |
| VGA.DRV | 2 | (same shape) |
| SOUND.DRV | 2 | CODE,PRELOAD,RELOCS + DATA,PRELOAD |
| **COMM.DRV** | **4** | seg1 CODE,MOVEABLE,**PRELOAD**,RELOCS,DISCARDABLE · **seg2 CODE,MOVEABLE,RELOCS,DISCARDABLE — NOT PRELOAD** · seg3 CODE,PRELOAD,RELOCS · seg4 DATA,PRELOAD,RELOCS |

**COMM.DRV's segment 2 is the only non-PRELOAD segment in the entire set.** Every module
that loads has nothing but PRELOAD segments; the first module with a demand-loaded segment
is the first module that fails. That is not a coincidence worth ignoring.

Consistent with it, the LoadSegment trace shows krnl386 loading COMM.DRV's segments
**1, 3 and 4 only** — segment 2 is correctly left as a not-present placeholder — and each
one's relocation pass returns AX=1:

```
seg1:0x90d9 LoadSegment(DI=1) -> seg1:0x929f AX=1     (relocations applied)
seg1:0x90d9 LoadSegment(DI=3) -> seg1:0x929f AX=1
seg1:0x90d9 LoadSegment(DI=4) -> seg1:0x929f AX=1
```

⇒ **Neither the segment loads nor the relocation pass fails.** `seg1:0x8cb6` (apply
relocations) returns success every time, for every module.

### Leads closed by measurement this part
- **The relocation pass.** AX=1 at `seg1:0x929f` on every call, COMM.DRV included.
- **COMM.DRV's own `LibMain` failing.** Its entry point is `seg1:0x002a` (the NE header's
  CS:IP) and it does run — the raw `INT 15h` at `0x2f7:0x0005` and `INT 2Fh` at
  `0x2f7:0x006b` are exactly its offsets 0x0005 and 0x006b. It takes the
  "no Virtual COMM Device, and not standard mode" path: `INT 2Fh AX=1684h BX=3` correctly
  answers ES:DI=0, and the `jne` at `0x0090` is taken — proved by the **absence** of a raw
  `INT 31h` at `0x2f7:0x0095`, which the other branch would have executed two instructions
  later. ⚠ `WF_ENHANCED` is `0x20` and `WF_STANDARD` is `0x10`, not the other way round;
  `seg1:0xc152` (`or byte [0x464],0x21`) is the enhanced-mode arm and it is the
  **fall-through** — `cmp al,3` at `seg1:0xc14b` has no branch after it.
- **`seg2:0x04b2 lcall [bp-8]`**, which is where the `#NP` demand-load of segment 2 fires,
  is krnl386 calling **`WEP`** — the string `57 45 50 00` is on the stack at the call. That
  is the *unload* path, so the `#NP` and the segment-2 read are part of **teardown**, not
  of the load. It returns AX=1 and its wrapper discards it (`popaw` then `xor ax,ax`).
- **A missing export.** All ten of COMM.DRV segment 2's imports exist, including
  `KERNEL.509 = WOWCLOSECOMPORT`, which confirms this is the WOW-aware COMM.DRV.
- **krnl386's own narration.** `seg2:0x0ed3` was armed at the right bytes and never fired:
  the block is guarded by `cmp word [0x12b0],0 / je` at `seg2:0x0e9f`, the same `/B` flag
  that could not be turned on. Independent confirmation that the boot-log route is dead.

### ⚠ Two more instrument defects, both found the hard way
- **A byte-pattern search is not a disassembly.** Sweeping seg2 for `3d 20 00` found 15
  sites, and `0x0aa1` was the middle of an instruction (`5f`, a one-byte `pop di`). The
  arm-time length check **refused** it — which is the only reason that run merely died
  instead of corrupting krnl386 invisibly. Only boundaries seen in an *aligned* listing
  are addresses. Related: disassembling from `seg2:0x0ed0` lands mid-instruction and
  invents a `push cs`; `0x0ec4` and `0x0ed3` are real.
- **★ `dpmi_bp_arm()` armed unresolved segment-relative addresses as linear ones.** Mode
  bit 1 means "offset into a krnl386 segment", resolved when that selector is committed —
  and until then the field holds a bare offset like `0x0e11`, which this loop happily
  planted a BOP at. Sixteen of them landed in **conventional memory**, over our own DOS
  kernel and krnl386's V86 image, and the guest died in its own bring-up with zero
  breakpoint hits. It had gone unnoticed because only one such breakpoint had ever been
  used at a time, and that one happened to land where the `b[0]==0 && b[1]==0` guard
  skipped it. Now: not armed until resolved.
- Also fixed so the above could be measured at all: `dpmi_bp_resolve_seg()` handles **every**
  krnl386 segment, not just segment 1 (mode bits 4..7 name it — `0x22` is segment 2), and
  the segment-identification window was widened from `[len-1, len+16)` to
  `[len-1, len+0x100)`. **Segment 2 rounds to a 0x100 boundary** (`0x3ee2 -> 0x3eff`, i.e.
  len+0x1d) where segments 1 and 3 round to a paragraph, so seg 2 — the one the whole
  module-load path lives in — had never been identified. The delta is logged now.

## ▶ RESUME HERE — session 36 handoff

**State:** krnl386 loads `SYSTEM.DRV`, `KEYBOARD.DRV`, `MOUSE.DRV`, `VGA.DRV`, `SOUND.DRV` and
fails on `COMM.DRV` with loader `AX = 0`. No Win16 application has run.

**The next question:** `LoadModule` returns **0** ("out of memory"), and AX is already 0 by
`seg2:0x0e25`, so the origin is **upstream of `seg2:0x0e0b`**. Bisect earlier in
`LoadModule` (`seg2:0x051d` onwards) the same way — repeating breakpoints on verified
`cmp ax,0x20` boundaries, comparing COMM.DRV against SOUND.DRV.

**The hypothesis to test first, and it is well supported:** COMM.DRV is the first module
with a **non-PRELOAD segment**. All five that load have two PRELOAD segments and nothing
else; COMM.DRV has four, and segment 2 is `CODE,MOVEABLE,RELOCS,DISCARDABLE` with no
PRELOAD bit. krnl386 correctly loads only segments 1, 3 and 4 and leaves segment 2 as a
not-present placeholder — so look at the **placeholder/allocation** path for a
demand-loaded segment, not at the loading of the three that do load.

**Ruled out — do not re-try (all by measurement, see Part 2):**
- The relocation pass. `seg1:0x8cb6` returns AX=1 for every segment of every module.
- The segment loads. LoadSegment(1), (3), (4) all succeed for COMM.DRV.
- COMM.DRV's own `LibMain` (`seg1:0x002a`). It runs, takes the "no VCD, not standard mode"
  branch, and the `INT 31h` on the other branch is provably never executed.
- The `#NP` demand-load of segment 2 — that is `WEP`, i.e. **teardown**, after the verdict.
- A missing export. All ten of segment 2's imports exist, `WOWCLOSECOMPORT` included.
- Error 2 / 4 / 0x0B / 0x0F, and the `WowLoadModule` retry. AX=0 skips the retry entirely.
- `USER.EXE`/`GDI.EXE` being absent. The run never reaches them; COMM.DRV is earlier.
- `WIFEMAN.DLL`/`WINNLS.DLL` being skipped. That is correct for an English locale.
- krnl386's `/B` boot log, the `[0x12b0]` poke, and the `seg2:0x0ed3` narration branch.
  All three are the same self-disabling flag.

**How to drive it:**
```bash
PMBP=1 ARCHIVE=build/wowruns ./scripts/bmwow.sh      # deploy, run, collect
```
⚠ SMB writes to `/private/tmp/xpshare` need the sandbox disabled. `pmbp.txt` columns are
`<addr> [dump] [skip] [mode] [rep]`; **mode bit 1 = the address is an offset into a krnl386
segment** (bits 4..7 name it: `2` = seg 1, `0x22` = seg 2), which moves every run, and
**rep 1 for anything reached more than once**. ⚠ **Only use addresses you have seen as
instruction boundaries in an aligned disassembly** — a byte search will hand you the middle
of an instruction. The working list:

```
# addr   dump  skip  mode  rep
0e25     0     0     22    1
0e55     0     0     22    1
0e88     0     0     22    1
0f16     0     0     22    1
cca4     0     0     2     1
```
