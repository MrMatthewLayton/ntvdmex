# Inventory — Win16 (KERNEL / USER / GDI), and how it is now tested

**Spec:** Windows 3.1 SDK Programmer's Reference; Wine (`krnl386.exe16`, `user.exe16`,
`gdi.exe16`); ReactOS; *Undocumented Windows*. The NE format is documented.
**Oracle for what has no spec:** stock `ntvdm` on the same rig —
[`research/stock-vdm-dump-oracle`](../research/).
**Harness:** `tools/ne/mkne.py`, `tests/probes/win16/`.
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
./tests/probes/win16/build.sh          # nasm + mkne.py -> build/wintest/*.EXE
./tests/probes/win16/run.sh w_kernel   # stage on the rig, launch via IFEO, print the result
```

### Everything in the linker was checked against a real binary

The NE format is documented; a small real Win16 program (`TASKMAN.EXE`, 2 segments,
KERNEL+USER imports) was read with this project's own `tools/ne/nedump.py` as a check on
the header values — **not** from memory:

- align shift 4, prog flags `0x0302` (DGROUP=MULTIPLEDATA), other flags `0x08`, target
  Windows, expects 3.10, `CS:IP` = segment 1:entry, `SS:SP` = segment 2:`0x0000`.
- An import is a far call whose 4-byte operand is left **unrelocated** (`FFFF:0000`, the
  relocation chain terminator) and named by a relocation of addrtype 3 (FAR_ADDR 32) and
  reloctype 1 (IMPORTORDINAL).
- The startup handshake is the documented Win16 task-start contract:
  **InitTask → WaitEvent(0) → InitApp(hInstance)**, each checked for `AX≠0`, with the
  entry-point registers `CX` stack size, `SI` hPrevInstance, `DI` hInstance, `BX:ES`
  command line, `DX` nCmdShow.
- Every ordinal was resolved from `krnl386.exe`'s own export tables.
- The exit is `INT 21h AH=4Ch`.

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

## ✅ …and compared against stock `ntvdm` — which immediately found a defect

`./tests/probes/win16/stock.sh w_kernel` runs the **same `.EXE`** twice on the same box: once
through the IFEO hook (us) and once with the key dropped (stock). **Reproduced twice,
identical both times:**

| case | ours | stock | |
|---|---|---|---|
| `kernel.getversion` | `5F03` | `5F03` | ✅ **AGREE — verified** |
| `kernel.getwinflags` | ~~`4C25`~~ → **`4C29`** | **`4C29`** | ✅ **CLOSED 2026-09-25** — see below |

★ **That is the first Win16 row this project has ever been able to call *verified* rather
than *looks right on screen*.** And the second row is a genuine defect, found by the first
deterministic test that ran.

⚠ **Neither value is produced by our code.** Both runs load the same
`C:\WINDOWS\SYSTEM32\KRNL386.EXE`, so the difference is in what our VDM *presents* to
it, not in a value we return.

**The differing bits are `0x04` (ours) against `0x08` (stock).**

⚠⚠ **The obvious reading — `WF_CPU386` vs `WF_CPU486` — is an INTERPRETATION, not a
measurement, and it is recorded as one.** Those constants are from memory; the Win3.1 SDK
is not in this tree and nothing here has confirmed them. A plausible mechanism (krnl386
separating 386 from 486 by whether the `AC` flag can be toggled, and our V86 environment
not permitting it) is likewise a **hypothesis with no evidence behind it yet**. Writing
either down as fact is exactly the mistake this project keeps paying for.

▶ ~~**Next:** find where `GetWinFlags` (KERNEL.132) gets the value.~~ **✅ DONE
2026-09-25** — by studying `krnl386.exe` for interoperability.

### ✅✅ CLOSED: it was a **DPMI** answer, not a WOW one

`GetWinFlags` returns a WINFLAGS word that krnl386 computes once, at start-up, from the
host's **DPMI installation check** (`INT 2Fh AX=1687h`): the processor bits follow the
`CL` (processor type) it is given — `CL=3` yields bit `0x0004`, `CL>3` yields bit
`0x0008`, and `CL<3` sets neither.

⇒ **The differing bit is `CL` from `INT 2Fh AX=1687h`, and we hardcoded `3`** (two sites
in `main.c`, with a comment reading "CL=3 (386)" written long before anyone knew what
consumed it). The `WF_CPU386`/`WF_CPU486` names are still from memory and still not
confirmed — but they are no longer load-bearing: what the fix rests on is that **stock
returns `CL>3`** (proved by its measured `4C29`) and that **`4` is the smallest such
value**.

⛔ The `AC`-flag hypothesis is **refuted**: nothing in this path tests a flag. It was a
plausible mechanism with no evidence, and it was wrong.

✅ **`DPMI_CPU_CLASS = 0x04`, and the row now reads `4C29`:**

```
CASE=kernel.getversion  SIG=AX AX=5F03
CASE=kernel.getwinflags SIG=AX AX=4C29
```

⚠ **That is "we now produce the value stock was measured to produce", not a fresh
side-by-side.** Stock's `4C29` is the reading from two earlier bracketed runs; re-running
`tests/probes/win16/stock.sh` would make it a same-day comparison, and that needs a human
because the bracket drops the IFEO key.

⚠ `tests/probes/dos/p_dpmins.com` records why no cheap oracle exists here: **MS-DOS 6.22 and
DOSBox-X both leave every register untouched** — neither has a DPMI host at all.

---

## ★ Widened (#163, 2026-10-02): KERNEL memory / strings / files, USER, GDI — 99 rows

`tests/probes/win16/w16.inc` is now the skeleton **once** (manifest, IAT called `call far
[cs:slot]`, TASKMAN's startup, report through INT 21h, absolute 8.3 output path), so a test
is its imports and its cases. Five new tests; every ordinal read off
`guest/ne/{krnl386,user,gdi}.exe` (⚠ `lstrcmp`/`lstrcmpi` are **USER**.430/471, not KERNEL).

**Expected values come from the Windows 3.1 SDK contract**, reduced to what it promises
(nonzero / ≥ N / sign of a comparison / bytes preserved). What the contract leaves open or
what describes the machine is emitted as a `*.raw` row with its **stock value OWED** —
`tests/probes/win16/stock.sh` needs supervision (it drops the IFEO key) and was not run.

Measured on the rig (host `b0e3b5b9`), runs `runs/s87_ide/w16/`:

| test | rows | match the contract | ⛔ differ | raw / owed |
|---|---|---|---|---|
| `w_kmem` — Global/Local alloc, lock, size, realloc, zero-init, free | 16 | 14 | — | `gsize.raw` `0400`, `gflags.raw` `0000` |
| `w_kstr` — lstrlen/cpy/cat/cpyn/cmp/cmpi, AnsiUpper/Lower, IsChar*, wvsprintf | 18 | 18 | — | — |
| `w_kfile` — _lcreat/_lwrite/_llseek/_lread/_lclose/_lopen, OpenFile, GetDriveType, dirs | 22 | 18 | `of.cbytes`, `sysdir.colon` | `windir.len` `000A`, `sysdir.len` `0007` |
| `w_user` — RECT arithmetic, desktop window, metrics | 23 | 17 | `desktop.ok`, `desktop.iswindow` | `cxscreen` `0690`, `cyscreen` `041A`, `dblclick` `01F4`, `syscolor.window` `FFFF` |
| `w_gdi` — memory DC, mono bitmap, PatBlt/Set/GetPixel, DC defaults | 20 | 17 | `bkmode` | `bitspixel` `0020`, `planes` `0001` |

**What differs, and what it is:**
- ⛔ **`gdi.bkmode` `0000`** (SDK default OPAQUE = 2): GetBkMode (GDI.76, WOW id `0x4c`) is
  **UNIMPLEMENTED, STEPPED OVER** in our WOW32 — the log says so; the 0 is the harness
  sentinel, not an answer.
- ⛔ **`user.desktop.ok` / `.iswindow` `0000`**: GetDesktopWindow answers NULL **by
  decision** (`wowuser.h`: "0 travels correctly" into `GetDC`). The SDK contract is a real
  handle, and `IsWindow` of it TRUE — the decision is now contradicted by a test.
- ⛔ **`kfile.sysdir.colon` `0000`, `sysdir.len` `0007`**: GetSystemDirectory (KERNEL.135)
  returns a 7-character string with no drive. No WOW32 call is made for it (krnl386 answers
  from its own state); GetWindowsDirectory beside it is right (`C:\WINDOWS`, 10).
- ⚠ **`kfile.of.cbytes` `0040`** where the SDK's `sizeof(OFSTRUCT)` is `0088`: `0040` is
  8 + the 56-character path, i.e. krnl386 records the bytes it filled. OpenFile ran inside
  krnl386 and DOS with no WOW32 thunk, so this is very likely **stock's answer too** —
  owed a stock run before it is called a defect.
- (Test defect, found and fixed: `kstr.lstrcat.ret` first read `15F4` because the probe kept
  a value in BX across a call — the callee preserves DS/SI/DI/BP only. Now `0001`.)

**Stock values owed** (run `tests/probes/win16/stock.sh <test>` under supervision): every row of
the five tests, and specifically the raw rows above plus `kfile.of.cbytes`.

✅ Scratch hygiene: `w_kfile`'s only file is absolute (`debug\out\W16F.TMP`) and its
`OF_DELETE` case removes it — measured `0001`, then `OF_EXIST` `FFFF` / nErrCode 2. Nothing
lands in the rig's `C:\WINDOWS`.

Not done from #163's list: folding these outputs into `dosdiff.py`'s parser (item 5) —
filed as a remainder.

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

Tracked in GitHub: [#164](https://github.com/MrMatthewLayton/ntvdmex/issues/164) (the list that was here was moved there verbatim, 2026-09-27).

## Next

Tracked in GitHub: [#163](https://github.com/MrMatthewLayton/ntvdmex/issues/163) (the list that was here was moved there verbatim, 2026-09-27).

