# Session 31 — 2026-08-27 — the WOW32 interface is pinned, and krnl386 opens a file

Continues [session 30](session-30.md). GH #128. Branch `m9/completeness`.

**Where it started:** krnl386 halted at message #2 of its own five-message error table,
`NTVDM KERNEL: Unable to initialize heap`, having named three WOW32 functions it wanted.
82 function IDs were enumerated as bare integers and none were implemented.

**Where it ended:** the heap error is gone, krnl386 **opens a real file through our own
DOS layer and gets handle 5 back**, and the WOW32 calling convention is pinned to the byte
and confirmed against the rig. It now stops somewhere new.

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

## Where it stops

The guest is resumed at our default PM stub `0x14f:0x95` — the IRET of the INT 31h
vector — after an `04F2` descriptor commit, and does not come back. The stack frame there
reads `[IP=0x662f][CS=0x000f][FLAGS=0x0202]`, which decodes cleanly as krnl386's
`lcall cs:[0x7c]` chain-to-host site. **Breakpoints at the correct linear addresses for
`0x662f`, `0x5cf8`, `0x5a42` and `0x5a4f` never fire**, including on the two earlier passes
that *survived*.

⇒ **That frame is stale and the naive reading of it is wrong.** Recorded as an open
question, not a conclusion. The next thread is how the guest arrives at `0x14f` and what
IRET frame the host actually pushes for it (`dpmi_service_pm_int` pushes
`FLAGS / g_pmret_sel / DPMI_PMRET_OFF`, which is *not* what the stack showed).

## Regression

- `selftest.com` **8/8 PASS on real hardware** after the SysVars change — the guest from
  the other class, run because a fix measured on one guest is a fix for none.
- 209/209 NE checks and every `dostest` battery pass off-VM.
- Imports still XP-safe.

## Rig left with

IFEO `Debugger` **set**, `wowtry.flag` **present**, `pmbp.txt` **removed** (disarmed).
`scripts/bmwow.sh` drives a WOW run end to end through `controld` — the watcher path
cannot, because `rt.bat` runs a DOS target out of `bm\tests` and a WOW run is "launch a
16-bit Windows program and let the IFEO hook route it to us".

## Next actions

1. **Resolve the `0x14f:0x95` wedge.** Read how the host places the guest on its default
   PM stub and what frame it pushes; the stack dump and the code disagree.
2. **Work down [`wow32-call-surface.md`](../../research/wow32-call-surface.md)** as krnl386
   demands each ID — `tools/ne/nedis.py --wowfunc <id>` gives the whole story for one.
   `0xc4` (the fatal-error MessageBox), `0x86` (a packed date/time), `0x82`/`0xc9`/`0x71`
   (the three that may **not** be declined) are the ones already seen live.
3. **Find out why the watchdog thread stops after one sample.** It is a diagnostic the
   whole project leans on.
4. Then: user/gdi's KERNEL-funnelled path, then `wowexec`, then an app.
