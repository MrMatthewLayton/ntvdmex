# Inventory — Win16 (KERNEL / USER / GDI), and how it is now tested

**Spec:** Windows 3.1 SDK Programmer's Reference; Wine (`krnl386.exe16`, `user.exe16`,
`gdi.exe16`); ReactOS; *Undocumented Windows*. The NE format is documented.
**Oracle for what has no spec:** stock `ntvdm` on the same rig —
[`research/stock-vdm-dump-oracle`](../research/).
**Harness:** `tools/ne/mkne.py`, `tools/wintest/`.
**Started:** 2026-09-24.

---

## Headline — the blocker was never the tests, it was that we could not build a binary

> **Owed since the spec-first directive:** *"Deterministic Win16 tests."* I had conceded
> too early that Win16 "cannot be spec-first". It can: the API semantics are fully
> specified, and only the **WOW32 thunk ABI** is genuinely unspecified — and that has a
> live oracle.

Every Win16 check this project had ever run was **"launch Notepad and look at it"**. That
needs a human, needs a screenshot, and cannot say *which* of four hundred calls behind the
window is wrong. What actually blocked the tests was more basic: **nothing in this repo
could build a Win16 binary.** There is no OpenWatcom on this machine, none in Homebrew,
and no 16-bit Windows toolchain in the tree — `tools/wowprobe/make-dosexe.sh` emits an MZ
header with `printf`.

So nasm writes the 16-bit code and **`tools/ne/mkne.py` writes the NE around it**.

```bash
./tools/wintest/build.sh          # nasm + mkne.py -> build/wintest/*.EXE
./tools/wintest/run.sh w_kernel   # stage on the rig, launch via IFEO, print the result
```

### Everything in the linker was read off a real binary

`guest/win16/TASKMAN.EXE` (3744 bytes, 2 segments, KERNEL+USER) is the model, read with
this project's own `tools/ne/nedump.py` and `nedis.py` — **not** from memory:

- align shift 4, prog flags `0x0302` (DGROUP=MULTIPLEDATA), other flags `0x08`, target
  Windows, expects 3.10, `CS:IP` = seg1:entry, `SS:SP` = seg2:`0x0000`.
- Import call sites carry `9A FF FF 00 00` — an **unrelocated** far call whose operand is
  the chain terminator `0xFFFF` and segment 0 — with relocation addrtype 3 (FAR_ADDR 32)
  and reloctype 1 (IMPORTORDINAL). Verified at three sites: `0x04bd`→KERNEL.91,
  `0x04e7`→KERNEL.30, `0x04f0`→USER.5.
- The startup handshake is TASKMAN's own `__astart`, disassembled at seg1:`0x04b9`:
  **InitTask → WaitEvent(0) → InitApp(hInstance)**, each checked for `AX≠0`, with the
  entry-point register contract falling straight out of it (`CX` stack size, `SI`
  hPrevInstance, `DI` hInstance, `BX:ES` command line, `DX` nCmdShow).
- Every ordinal was resolved from `guest/win16/krnl386.exe`'s own export tables.
- The exit is `INT 21h AH=4Ch` — the path TASKMAN itself falls back to at seg1:`0x04b7`.

`build.sh` validates each generated `.EXE` with **our own `nedump.py`** before it ever
reaches the rig: a reader verified against real Win16 binaries is the cheapest possible
check that the writer produced something loadable.

---

## First result — measured on the bare-metal rig, 2026-09-24

```
#PROBE w16kernel
STEP=I AX=0BAF          InitTask succeeded
STEP=W                  WaitEvent returned
STEP=A                  InitApp succeeded
CASE=kernel.getversion  SIG=AX AX=5F03
CASE=kernel.getwinflags SIG=AX AX=4C25
#END
```

✅ **The chain works end to end**: NE linker → relocations → import table → the startup
handshake → a file written out of a running Win16 task, on real hardware.

⚠ **The two values are NOT verified.** They are *ours*, and nothing has asked another
machine. **A number this probe produces is a measurement, not a pass**, until it is
compared with stock `ntvdm` on the same rig — which is the documented authority for
everything WOW. That comparison is the next step and it is owed.

---

## Three mistakes, and the one that mattered

### ⛔⛔⛔ A near `call` into a far-returning stub

The import table was first a set of 5-byte `EA` (`jmp far`) stubs reached by a **near**
`call`, on my reasoning that *"its RETF returns straight to the near caller"*. **It does
not.** A Win16 API ends in `RETF`, which pops **two** words; a near call pushed one. The
API returned to `0000:<offset>` and wandered off.

⇒ Replaced with an **import address table** of 4-byte far pointers called as
`call far [cs:slot]`. That needs no stub, costs one relocation per *function* rather than
per call site, and **cannot get the call kind wrong**.

### ⛔⛔ And I read the evidence backwards before finding it

The probe reported `STEP=S` and stopped. I concluded **"InitTask returned 0"**, and
restructured the startup around that — moving `InitTask` ahead of the file open on the
theory that an intervening `INT 21h` had clobbered its input registers. It made no
difference, because the defect was the call kind.

> **The progress markers were telling the truth; my inference from them was the fiction.**
> The file stopped at the first API call *whichever* call it was — which is exactly what a
> broken call convention looks like, and nothing like a specific function failing.

The reordering is **kept, and relabelled**: matching TASKMAN is worth doing on its own
merits, but it is recorded as a fix for a defect that did not exist rather than left
looking like a diagnosis that worked.

### ⛔ The scaffolding depended on the thing under test

The first cut wrote its output with KERNEL's `_lcreat`/`_lwrite`/`_lclose`. It produced no
file, and there was then **no way to tell which of InitTask, InitApp or `_lcreat` had
failed** — because the thing that would have reported it was the thing being tested.
`probe.inc` has said this since the beginning: *"nothing here uses a BIOS call, on purpose:
this scaffolding must not itself depend on the layer under test."* That rule is not about
DOS; it is about scaffolding. Output now goes through `INT 21h`.

---

## Open questions this raised

| | |
|---|---|
| **A relative path does not land in the launch directory** | The probe's `INT 21h AH=3Ch` on `W16OUT.TXT` **succeeds** (`-> AX=5 CF=0`) but the file is not in the folder the app was launched from. The WOW command fetch reports `cur=[…\demo\win16\w16kern]`, while the DOS kernel resolves the relative name against its **own** current directory. ⚠ Unresolved: should the DOS CDS follow the `cur=` the WOW fetch hands us? The probe names its file absolutely rather than depend on the answer |
| `kernel.getversion` = `5F03`, `kernel.getwinflags` = `4C25` | **Unverified.** Needs the stock-ntvdm comparison |
| `WowFailedExec` is **not** a failure signal | It appears once in a **successful** Notepad run too. I briefly took it as proof the module had been rejected. It is not |

## Next

1. ★★★ **Run the same `.EXE` under stock `ntvdm`** and diff. That turns every value here
   from a measurement into a verdict, and it is the whole point of having a deterministic
   test.
2. **Widen the cases** — KERNEL's memory API (GlobalAlloc/Size/Free round-trips),
   `lstrlen`/`lstrcmp`, the file API, then USER and GDI. The harness makes each of these
   a handful of lines.
3. **Fold the output into `dosdiff.py`'s parser**, which already understands
   `#PROBE`/`CASE=`/`#END`, so Win16 rows join the same diff table as everything else.
