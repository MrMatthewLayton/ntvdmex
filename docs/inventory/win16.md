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

## ✅ …and compared against stock `ntvdm` — which immediately found a defect

`./tools/wintest/stock.sh w_kernel` runs the **same `.EXE`** twice on the same box: once
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

▶ ~~**Next:** read `GetWinFlags` (KERNEL.132) out of `krnl386.exe` with `tools/ne/nedis.py`
and find where the value actually comes from.~~ **✅ DONE 2026-09-25 — and the answer was
in the binary, exactly as predicted.**

### ✅✅ CLOSED: it was a **DPMI** answer, not a WOW one

`GetWinFlags` is KERNEL.132 → segment 3, offset `0x4B`, and it is four instructions:

```
004b: push ds ; call <set DS>
004f: xor ax,ax ; push ax ; lcall <helper>
0057: test ax,0x400
005a: mov ax,[0x464]          ; ** the whole WINFLAGS word lives here **
005d: je +3 ; and ah,0xBF     ; clears bit 0x4000 when the helper says so
0062: xor dx,dx ; pop ds ; retf
```

`[0x464]` is written in **one** place that matters — segment 1, `0xD68A`
(`tools/ne/nedis.py guest/ne/krnl386.exe 1 0xd650 0x70`):

```
mov ax,0x1687 ; int 2Fh      ; ** the DPMI installation check **
or  ax,ax  ; jne -> bail
xor bh,bh
cmp cl,3   ; jb  -> bail
mov bl,4   ; CL == 3  -> 0x0004
je  +2
mov bl,8   ; CL >  3  -> 0x0008
mov [0x464],bx
```

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
`tools/wintest/stock.sh` would make it a same-day comparison, and that needs a human
because the bracket drops the IFEO key.

⚠ `tools/dostest/p_dpmins.com` records why no cheap oracle exists here: **MS-DOS 6.22 and
DOSBox-X both leave every register untouched** — neither has a DPMI host at all.

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
| ~~`kernel.getwinflags` `4C25` vs stock `4C29`~~ | ✅ **CLOSED 2026-09-25.** It was `INT 2Fh AX=1687h`'s **CL**, which we hardcoded to 3; krnl386 turns `CL>3` into the other bit. **A Win16 mismatch whose cause was in the DPMI layer.** |
| `WowFailedExec` is **not** a failure signal | It appears once in a **successful** Notepad run too. I briefly took it as proof the module had been rejected. It is not |

## Next

1. ✅ ~~**Run the same `.EXE` under stock `ntvdm`**~~ — done, `tools/wintest/stock.sh`.
2. ✅ ~~★★★ **Find out where `GetWinFlags`'s value comes from**~~ — **DONE**, by reading KERNEL.132 out of
   `krnl386.exe` rather than theorising about it.
3. **Widen the cases** — KERNEL's memory API (GlobalAlloc/Size/Free round-trips),
   `lstrlen`/`lstrcmp`, the file API, then USER and GDI. The harness makes each of these
   a handful of lines.
5. **Fold the output into `dosdiff.py`'s parser**, which already understands
   `#PROBE`/`CASE=`/`#END`, so Win16 rows join the same diff table as everything else.
