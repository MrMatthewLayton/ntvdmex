# Session 31 — 2026-08-27 — the WOW32 interface is pinned, and krnl386 opens a file

Continues [session 30](session-30.md). GH #128. Branch `m9/completeness`.

**Where it started:** krnl386 halted at message #2 of its own five-message error table,
`NTVDM KERNEL: Unable to initialize heap`, having named three WOW32 functions it wanted.
82 function IDs were enumerated as bare integers and none were implemented.

**Where it ended:** the heap error is gone, krnl386 **opens a real file through our own
DOS layer and gets handle 5 back**, the WOW32 calling convention is pinned to the byte and
confirmed against the rig, and the next stop is **traced instruction by instruction and
fully explained** — it dies in an NE segment-table copy loop whose header is our own PM
stub table.

⚠️ Three of the day's instruments were wrong and are corrected below, one of them twice.
The corrections are the most useful part of this log.

---

## Part 1 — 29 of the 82 functions name themselves

The surface was a list of integers. It did not have to be.

`tools/ne/wowmap.py` walks krnl386's **entry (export) table** and asks, for each export,
whether its target *is* one of the WOW32 stubs. 28 of them are — so the export's name in
the resident/non-resident name table **is** the function's name, with no inference at all:
`GETVDMPOINTER32W`, `LOADLIBRARYEX32W`, `GETPROCADDRESS32W`, `WOWLOADMODULE`,
`WOWGETNEXTVDMCOMMAND`, `YIELD`, `GETDRIVETYPE`, `GETSHORTPATHNAME`, `WOWMSGBOX`… A second
pass finds exports whose body is a thin wrapper around exactly one stub (2 more), labelled
`WRAPPER` because that *is* an inference.

⚠️ Both name tables, not just the resident one. krnl386 keeps 312 non-resident names and
several of the WOW32 exports live only there.

★ **It was cross-checked before being believed.** `0xcf` had already been worked out from
its call site alone — the caller compares the result against `0x411`, `0x412`, `0x404`,
`0x804`, `0x0c04`, which are the Far-East LANGIDs, i.e. it is asking "am I on a DBCS
system". The export table then said `GETSYSTEMDEFAULTLANGID`. **Two methods, one answer**,
which is what makes either trustworthy.

## Part 2 — ★ the frame, and an instrument that had already lied about it

Session 30 recorded VirtualAlloc's argument **order** as "not pinned down, two readings
possible". There was only ever one reading.

The trace read the arguments at `bp+12`. They are at `bp+16`, so it printed the **caller's
far return address** as the first two argument words — and `65ed 000f` is exactly the
instruction after the call site plus krnl386's CS. The proof is krnl386's own return path
at `seg1:0x2c1d`:

```
mov bx,[bp+10] / shl bx,2 / add bx,0x2ab6 / jmp bx
```

which lands in a table of `pop bx / pop bp / add sp,0xA / retf N` stubs, one per argument
size. `add sp,0xA` skips `bp+2..bp+10`, the `retf` consumes the far return at
`bp+12/+14`, and `retf N` discards N bytes above it. The arguments are those N bytes.

★ **And the return value is not a register.** The thunk does `sub sp,4` *before* the BOP
and `pop ax / pop dx` unconditionally after it, so anything left in AX/DX is overwritten
before the caller ever sees it. The 32-bit side must write the DWORD into that stack hole
at `[bp-16]`. Confirmed on hardware: in the rig's own `@ss:sp` dump those two words held
stale stack (`0x0047`, `0x0000`) at the BOP — an uninitialised return slot. **Getting this
wrong is silent**: the guest reads garbage and blames itself.

★ **Argument order is PASCAL** (pushed left to right, first declared argument highest),
agreed by three independent call sites — VirtualAlloc's `MEM_COMMIT|MEM_RESERVE` +
`PAGE_EXECUTE_READWRITE`, VirtualFree's `MEM_RELEASE`, and GlobalMemoryStatus's 32-byte
buffer with `dwLength` pre-set to `0x20` followed by `dwAvailPhys + dwAvailPageFile`
compared against `dwAvailVirtual`.

`0xb8`'s call site is guarded by `cmp ax, 0x501` and afterwards loads BX:CX and SI:DI from
the result — so **krnl386 services DPMI 0501 with VirtualAlloc** rather than passing it to
the DPMI host.

## Part 3 — ⚠️ `SysVars+0x6A` was zero, and krnl386 writes through what it finds there

Before anything else, krnl386's init does `INT 21h AH=52h` and then
`mov di, es:[bx+0x6a]`, and builds six far pointers into DOS's data area from the table
that word names. Our SysVars was zeroed apart from the MCB head, so those became offsets
`0x00`, `0x0c`, `0x10`… into `DOS_HDLR_SEG` — **our own INT 21h BOP stub and DPMI entry
points** — and krnl386 does not only read through them (`seg1:0x52b5` stores a word). The
host was being scribbled on.

`dos_wow_publish()` plants the table now, shaped like the one `lolprobe` measured off
stock ntvdm last session, plus the LASTDRIVE byte one of the pointers points at. Two of
the six are pinned (LASTDRIVE; and the current-drive byte, which `seg1:0x5343` returns as
INT 21h AH=19h's answer); the other four are private scratch, labelled as such.

**Clearing error #2 needed this, not just the allocator.**

## Part 4 — ★ krnl386 opens a real file, because DECLINING is a real answer

With `0xb8/0xb9/0xbc/0xcf/0x78` implemented the heap error vanished and krnl386 went
straight into its **DOS file layer**, asking for four functions we did not have.

Reading the call sites turned that from a wall into nothing at all. krnl386 hooks INT 21h
in protected mode and offers some functions to its 32-bit companion first; when the
companion returns `0xFFFF` it chains to `cs:[0x3c]`, **the vector it saved before
hooking** — which in this host is our own DOS layer, the one COMMAND.COM and Doom already
use.

```
5507  cmp ax, 0xffff
550a  je  0x55a1   ->  pop ax/bx/dx  ->  jmp 0x56c8  ->  lcall cs:[0x3c]
```

So a sentinel return is not a stub: it hands file I/O to working code instead of to a
parallel Win32 handle table that would disagree with every call that chains anyway.

⚠️ **Only where the call site says so.** `tools/ne/wowdecline.py` checks each site and
finds three (`0x82`, `0xc9`, `0x71`) where `0xFFFF` is a plain **error** and krnl386
reports failure to the app rather than chaining. Declining there would turn "not
implemented" into "the file does not exist" — a **wrong** answer instead of a missing one,
which is the more expensive kind. Verdicts spot-checked against hand disassembly before
the tool was trusted; declining `0x6f` and `0x97` makes krnl386 re-issue a plain
`AH=40h` / `AH=3Fh`, visible in its own code at `0x56c6` / `0x55a7`.

**Measured on the rig:** krnl386 asks for attributes, opens the file, and the subsequent
get-date and close calls arrive carrying `0x0005` — the handle **our DOS layer** returned.

▸ The honest trade: real WOW routes these to Win32, so a Win16 app gets NT file semantics
(sharing modes, long names). Declining gives it our DOS semantics. For loading and running
a program that is the same thing, and it is one line to change later — but it *is* a
difference, so it is written down rather than discovered.

## Part 5 — ⚠️ three runs of breakpoints armed on a corpse

The run now stops without an error message. Localising it cost four instrument fixes, and
the last one is the lesson.

- The **watchdog thread logs exactly one sample per WOW run and then stops**, for reasons
  still not found. Twelve were asked for. It is the component whose job is to answer
  "where is the guest stuck", so the answer was silence that looked like nothing being
  wrong. A `PMHB` heartbeat was added to the **main PM loop** instead, which is provably
  alive because everything else in the log comes from it.
- Its first sparse rate (every 4096 steps) printed **nothing at all** — a WOW run wedges
  after ~0x31 PM entries. A heartbeat coarser than the thing it measures is not a
  heartbeat.
- Two hazards fixed on the way, **neither of which was the cause** (the rig showed no
  change from either, and that is stated rather than implied): the watchdog dereferenced
  guest memory unguarded on its first FROZEN sample, and `serial_out` could block the
  process it instruments — no flow-control fields were cleared and no comm timeouts were
  ever set, so on real hardware with no cable a write waits for a peer that does not exist.
- A real bug in `wow_shadow_sync`: it diffed the shadow against `g_ldt[]`, the host's
  record of the **real** LDT, so when the kernel *rejected* a descriptor (four per run —
  krnl386's free-list links carry access byte `0x0F`) it updated `g_ldt[]` anyway, purely
  to stop retrying. From that moment `g_ldt[]` described what the guest **wanted** rather
  than what the CPU **had**, and `dpmi_sel_base()` — which every pointer translation in
  the host uses, including the WOW32 far-pointer arguments added this session — would have
  answered from it. It diffs against a host-private `seen` copy now. *"What did the shadow
  last look like" and "what is in the real LDT" are different questions, and this is the
  second time conflating them has produced a bug.*

★ **AND THEN THE ONE THAT MATTERS.** Breakpoints planted at `0x02950000 + offset` — the
base the bind stage logs for krnl386 — reported themselves **`armed`, with exactly the
right displaced bytes**, and never fired. Three runs. That base is a **second, dead copy**;
the executing krnl386 is at linear **`0x1410`** (segment `0141`). Session 30's own log
knew it — its `04431` breakpoint is `0x1410 + 0x3021` — and nothing in the tooling said
so. `csbase=` is printed on every heartbeat now.

⚠️ **An instrument that reports success against the wrong copy of the thing is worse than
one that fails.** Same shape as the stale-artefact and wrong-bytes findings before it.

## Part 6 — ⚠️ REFUTED TWICE: the breakpoints were corrupting the guest

Re-armed at the live base, the breakpoints at `0x662f`, `0x5cf8`, `0x5a42` and `0x5a4f`
still never fired, and a first draft of this log concluded from that:

> *"Breakpoints prove it never reaches the addresses the stack frame there appears to
> name, so that frame is stale and the naive reading of it is wrong."*

Comparing the step counter across the day's runs refuted that: **planting a PM breakpoint
inside krnl386's live image killed the guest at PM step `0x01`**, forty entries before the
watched code. So "it never fired" said nothing about whether that code executes.

★ **And then the cause, which makes the second conclusion wrong as well.** A breakpoint is
a two-byte `C4 C4`. Three of the four sat on **one-byte instructions** — `c3` (ret),
`1f` (pop ds), `c9` (leave) — so each ate the first byte of its *neighbour*. The `c3` at
`seg1:0x662f` ate the leading `2e` of

```
6630  2e 83 3e 32 00 00   cmp word ptr cs:[0x32], 0
```

which is on the path that sets AX for krnl386's **first** INT 31h. The guest asked for
`AX=0x0000` (allocate descriptors) instead of `AX=0x000A` (create alias) — visible in one
line of every log, twelve runs to one — and died immediately.

⚠️ The code already warned about this from session 17 and concluded *"we cannot tell where
instructions start, so we cannot prevent this in general."* That stopped being true in
session 21, when `x86len.h` was written for the INT-site patcher's identical disease.
`dpmi_bp_arm()` now measures the instruction and **REFUSES**, naming the byte.

⇒ **With working breakpoints, the original reading of the stack frame was right all
along**, and the whole route is confirmed by hits rather than inference:

```
0x662f → 0x5cf8 → 0x5a42 → ret 8 → 0x7ed4 → 0x63b4 → 0x7f11 → 0x4648
       → retf 6 → 0xd4c0 → 0xd4db
```

The `pop es` / `pop ds` suspects were cleared the same way: they execute fine.

## Part 7 — ★ where it actually dies, and why

`seg1:0xd4db` hits; `seg1:0xd4f5` does not. Between them is the **NE segment-table copy
loop**:

```
d4db  mov cx, es:[0x1c]      ; ne_cseg, from the NE header just copied in
d4e5  movsw / movsw / lodsw / and ax,0x11f9 / or ax,0xc000 / stosw / movsw / stosw
d4f3  loop 0xd4e5
```

To see what `ne_cseg` really was, the debugger had to stop making the reader guess where a
selector points — `pmbp.txt`'s dump column takes a fixed **linear** address, and two runs
were spent dumping `0x1100` on the strength of an old `04F2 installed base=0x1100` line.
`DPMI-BP HIT` now resolves DS/ES itself and dumps `@ds:si` and `@es:di`. The answer
arrived on the next run:

```
dsbase=0x0001696e  esbase=0x00001100
@ds:si=c4 cf c4 c4 cf c4 c4 cf c4 c4 cf c4 c4 cf c4 c4
```

**The "NE header" krnl386 copied is our own default PM handler stub table** (`C4 C4 CF`
repeated). So `ne_cseg` is nonsense, the loop's `stosw`/`movsw` walk off the segment, and
an unreflected PM #GP tears the VDM down exactly as observed: no VEH, no watchdog line, no
last log entry.

⇒ The open question is now **"why is the module image not where krnl386 thinks it is"**,
not "where does it die". Two facts bear on it: krnl386 **never reads the file it opens**
(zero INT 21h `AH=3Fh` in a whole run — it only stats it), and it **is** using our
`VirtualAlloc` block as its heap (`INT 31h 0007 setbase 0x03a70000`, `0008 setlimit
0x8807f`). Our NE loader puts module images in HOST memory at `0x0295xxxx` while krnl386
does its own loading — two loaders, two copies, and that is the thing to reconcile.

## Part 8 — ⚠️ the rig was feeding it a DOS `.COM`

`STAGE2: target.txt loaded 0x5d3 from C:\test\selftest.com` appears in **every** WOW run
of the session. `rt.bat` writes `target.txt` for each DOS test and `wowrun.bat` never set
its own, so the WOW bootstrap was told to load whatever DOS program ran last — and
krnl386 dutifully tried to parse a `.COM` file as a Win16 module.

`wowrun.bat` now establishes its input instead of inheriting it. Pointing it at
`SYSEDIT.EXE` does **not** by itself clear the wedge (the header krnl386 reads is still
our stub table, so the fault is upstream of the filename), but no measurement taken while
that was true can be trusted.

## Part 9 — and one self-inflicted host crash, named by its own fault dump

The REFUSED message added in part 6 is ~180 bytes; the log buffer in `dpmi_bp_arm` was
128. The overflow corrupted the function's stack and took the **host** down with an access
violation before the guest ran. The dump named it outright:

```
DPMI FATAL: exception code=0xc0000005 ... EDX=0x33746e69
```

`0x33746e69` is the ASCII `"int3"` — from that very string. `zput`/`zhex` are
caller-sized and check nothing, so the buffer has to fit the **longest** line, not the
usual one.

## Part 10 — ★ the real blocker: krnl386's PM DOS calls were never answered

Turning on the INT 21h trace (`dostrace.flag` — it is **opt-in**, which is why the log
had never shown ordinary DOS calls) named the problem in one line each:

```
INT21h AH=34 (PM thunk TODO)      INT21h AH=0e (PM thunk TODO)
INT21h AH=dc (PM thunk TODO)      INT21h AH=43 (PM thunk TODO)
INT21h AH=57 (PM thunk TODO)
```

⚠️ **This also refutes part 7's "krnl386 never reads the file it opens — zero INT 21h
`AH=3Fh` in a whole run".** That was not evidence of anything. The log did not contain
ordinary DOS calls because nothing was printing them. *An absent line in a log that does
not print that line is not a measurement.*

★ **Why the pointer-taking ones could not simply be whitelisted.** `dos_int21.c` resolves
a guest pointer as `(DS << 4) + DX` — exactly right for V86, meaningless for a selector.
DOS/4GW never exposed this because it services its own DOS calls internally; **krnl386 is
the first guest to chain them to us.** `pm_int21_xfer()` bridges it through a
conventional-memory transfer buffer — the same shape as DPMI's own translation buffer and
for the same reason. `AH=34h` is special-cased because it returns a far pointer, which in
PM must be a *selector*, not a paragraph.

⚠️ The buffer is allocated **only on the WOW path** and every arm is gated on its presence,
so a DOS or DOS/4GW run takes byte-identical paths to before.

**Measured on the rig — the whole chain now works:**

```
FUNC=0xc7 -> DECLINED -> INT21h AH=43 name="C:\WINDOWS\SYSTEM32\SYSEDIT.EXE"
FUNC=0xc1 -> DECLINED -> INT21h AH=3D open  "...SYSEDIT.EXE" -> AX=5
FUNC=0x89 -> DECLINED -> INT21h AH=57 on handle 5
FUNC=0xc2 -> DECLINED -> close
```

krnl386 asks its 32-bit companion, is declined, chains to real DOS, and our thunk turns its
protected-mode pointer into a filename DOS can open. **"PM thunk TODO" is now zero for a
whole run.** `seg1:0x1812` turns out to be `OpenFile(name, &ofstruct, OF_EXIST)`, which is
why that sequence opens and closes without reading.

The run still ends in the same segment-table loop: the NE header krnl386 parses comes from
a buffer (`[0x5a0]`, a selector it allocates itself) that something else is supposed to
fill, and finding what fills it is the next thread.

▸ Incidental, and worth knowing: with the transfer buffer taking paragraph `0x141`,
krnl386 relocated to segment `0x542` and behaved identically — **its placement is not
position-dependent**.


## Regression

- `selftest.com` **8/8 PASS on real hardware** after the SysVars change — the guest from
  the other class, run because a fix measured on one guest is a fix for none.
- 209/209 NE checks and every `dostest` battery pass off-VM.
- Imports still XP-safe.
- Re-run after the breakpoint and buffer fixes: `selftest.com` **8/8 PASS** again.
- After the PM INT 21h thunk: `selftest.com` **8/8** and `dosver.com` both PASS on real
  hardware — the DOS path is gated off the WOW-only transfer buffer and is unchanged.

## Rig left with

IFEO `Debugger` **set**, `wowtry.flag` **present**, `pmbp.txt` **removed** (disarmed), and
`wowrun.bat` **now writes `target.txt` itself** so a WOW run can no longer inherit the last
DOS test's program.
`scripts/bmwow.sh` drives a WOW run end to end through `controld` — the watcher path
cannot, because `rt.bat` runs a DOS target out of `bm\tests` and a WOW run is "launch a
16-bit Windows program and let the IFEO hook route it to us".

## Next actions

1. **Find what fills the NE-header buffer at `[0x5a0]`.** krnl386 allocates the selector
   itself (`seg1:0xc181`, via `0x59a0`), `0x1812` is only an `OF_EXIST` probe, and by the
   time `0xd45a` parses it the buffer still holds our PM stub table. That is the wedge.
2. **Reconcile the two loaders.** krnl386 loads modules itself, into memory it manages
   out of our `VirtualAlloc` heap; our NE loader has already put the images in host memory
   at `0x0295xxxx`. At `seg1:0xd4db` krnl386 is copying an NE header out of a buffer that
   holds our PM stub table, so it is reading a module image that was never put there.
   Find what tells it where that image is — that is the next wall.
3. **Work down [`wow32-call-surface.md`](../../research/wow32-call-surface.md)** as krnl386
   demands each ID — `tools/ne/nedis.py --wowfunc <id>` gives the whole story for one.
   `0xc4` (the fatal-error MessageBox), `0x86` (a packed date/time), `0x82`/`0xc9`/`0x71`
   (the three that may **not** be declined) are the ones already seen live.
4. **Find out why the watchdog thread stops after one sample.** It is a diagnostic the
   whole project leans on.
5. Then: user/gdi's KERNEL-funnelled path, then `wowexec`, then an app.
