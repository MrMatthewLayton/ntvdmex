# Inventory — executable formats: `.COM`, MZ, NE, LE, PE

**Spec:** the MS-DOS Programmer's Reference (the EXEC function and the MZ header);
Microsoft's *New Executable* format description (Windows 3.x SDK); the IBM/Microsoft
*LE/LX* format notes; the *PE/COFF* specification. ⚠ **None held in the repo** —
[`../ref/SOURCES.md`](../ref/SOURCES.md).
**Our implementation:**
- `.COM` and MZ: `dos_load` and `dos_load_overlay` in `src/dos/dos_loader.h` (115 lines),
  called for the first program (`main.c:26862`) and for EXEC (`main.c:2284`); the EXEC
  memory, PSP and entry state around it in `main.c` ≈`:2100-2350`.
- NE: `src/wow/ne.h` (545 lines), parse + entry table + names + relocations + a module
  registry, used by the WOW boot (`ne_parse`, `main.c:9259`).
- LE: not loaded by us — the extender loads its own image. `dpmi_le_learn`
  (`main.c:18217`) reads the LE object table only to know which objects are code.
**Off-VM:** `tools/dostest/mcb_test.c` (MZ/COM), `ne_test.c` (NE).
**Probes:** `p_exec`, `p_ovl` (EXEC, `4B01h`, `4B03h`), `p_4b05`.
**Marked:** 2026-10-01, **from the code**.

---

## Headline

**`.COM` and MZ load right for the programs the shelf runs; the header fields that govern
*memory* are ignored.** EXEC hands every child **all of free memory** (`main.c:2217-2224`),
whatever `e_minalloc`/`e_maxalloc` say — so a program linked to leave memory free (for its
own children, or a TSR) is given everything, a program that needs more than is free is
loaded anyway rather than refused with error 8, and "load high" (`e_maxalloc` = 0) is not
honoured.

**The NE loader is careful and complete for what Windows 3.x's own binaries use.** It
refuses, loudly, the three things none of them contain: OS fixups, 32-bit address types
and iterated segments.

**EXEC of a Windows program from the DOS prompt runs its MZ stub.** The EXEC path does not
look for an NE or PE header, so `notepad.exe` typed at our prompt prints "This program
cannot be run in DOS mode" where stock XP NTVDM starts it. The **Open** dialog already
makes the distinction (`open_is_dos_image`, `main.c:10302-10320`); EXEC does not.

| Group | Units | IMPL | PART | STORE | MISS | N/A |
|---|---|---|---|---|---|---|
| §1 `.COM` | 5 | 2 | 2 | — | 1 | — |
| §2 MZ | 12 | 6 | — | — | 5 | 1 |
| §3 NE | 15 | 11 | — | — | 3 | 1 |
| §4 LE / LX | 2 | 1 | — | — | — | 1 |
| §5 PE, and EXEC of a Windows program | 2 | — | — | — | 1 | 1 |
| **Total** | **36** | **20** | **2** | **—** | **10** | **4** |

---

## 1. `.COM`

| Unit | Status | Where / what is missing | Verification |
|---|---|---|---|
| Image copied to `PSP:0100h`, CS=DS=ES=SS=PSP, IP=0100h | **IMPL** | `dos_loader.h:60-67`; DS/ES set by EXEC (`main.c:2338`) | **oracle** (`p_exec`) |
| Size limit | **PART** | `:61`: anything past **`FE00h`** bytes is silently cut; DOS's own ceiling is higher (the 64 KB segment less the PSP and the initial stack word) and it refuses rather than truncates | untested |
| SP = `FFFEh` | **IMPL** | `:64` (DOS lowers it when less than 64 KB is free; we always give the child all of memory, §2) | untested |
| A `0000h` word at the top of the stack, so a final `RET` reaches `INT 20h` | **PART** | written for the **first** program only (`main.c:28280-28281`); an EXEC'd `.COM` child has whatever was at `FFFEh` | untested |
| AL/AH on entry = drive validity of the two FCBs (`00h` or `FFh`) | **MISS** | EXEC sets `AX = 0` (`main.c:2339`) whatever the command line's drives | — |

## 2. MZ

| Field / unit | Status | Where / what is missing | Verification |
|---|---|---|---|
| Signature `MZ` | **IMPL** | `:33` | — |
| Signature `ZM` (also accepted by DOS) | **MISS** | a `ZM` file is loaded as a `.COM` | — |
| `e_cblp` / `e_cp` → image size | **IMPL** | `:39-45`; clamped to the file | `mcb_test.c` |
| `e_cparhdr` → header size | **IMPL** | `:34` | `mcb_test.c` |
| `e_crlc` / `e_lfarlc` → relocations | **IMPL** | `:47-53` | `mcb_test.c`; every shelf `.EXE` |
| Header fields checked against the file length | **MISS** | the relocation loop reads `file + e_lfarlc + i*4` with no bound (`:47-49`) — a damaged header reads past the host's buffer | — |
| `e_ss` / `e_sp`, `e_cs` / `e_ip` | **IMPL** | `:54-57` | **oracle** (`p_exec`) |
| `e_minalloc` | **MISS** | not read; EXEC never refuses for want of memory beyond the image | — |
| `e_maxalloc` | **MISS** | not read; the child always gets all free memory (`main.c:2217-2224`) | — |
| Load high (`e_minalloc` = `e_maxalloc` = 0) | **MISS** | | — |
| `e_csum`, `e_ovno` | **N/A** | DOS ignores both | — |
| `4B01h` returns SS:SP = `e_sp − 2`, CS:IP; `4B03h` overlay relocated by the **factor** | **IMPL** | `main.c:2289-2320`, `dos_loader.h:83-113` | **oracle** (`p_ovl`, single-point for SP) |

## 3. NE

| Unit | Status | Where / what is missing | Verification |
|---|---|---|---|
| Header (MZ → `e_lfanew` → `NE`), every table offset; the non-resident table's absolute DWORD offset | **IMPL** | `ne.h:129-165` | `ne_test.c` |
| Segment table; length 0 = 64 KB; alignment shift 0 = 512 | **IMPL** | `:166-181` | `ne_test.c` |
| `minalloc` above the file length (a BSS tail) | **IMPL** | `ne_seg_alloc_size` `:537-543` | `ne_test.c` |
| Entry table: fixed, moveable (`FFh`) and absolute (`FEh`) bundles; null bundles | **IMPL** | `:202-231` | `ne_test.c` (krnl386's 30 absolutes) |
| Resident and non-resident names (by-name search falls through to non-resident) | **IMPL** | `:256-334` | `ne_test.c` |
| Module reference and imported-names tables | **IMPL** | `:286-301` | `ne_test.c` |
| Relocation chains (walk to `FFFFh`) and ADDITIVE (patch once, add) | **IMPL** | `:396-438` | `ne_test.c`; gdi.exe's 366 additive sites |
| INTERNALREF, including a moveable target named by ordinal | **IMPL** | `:372-383` | `ne_test.c` |
| IMPORTORDINAL / IMPORTNAME through the registry; load → select → relocate order | **IMPL** | `:384-391`, `:443-533` | by hand (Win16 shelf) |
| Address types LOBYTE, SEGMENT, FARADDR, OFFSET16 | **IMPL** | `:419-431` | `ne_test.c` |
| EXPORTED bit enforced on an imported ordinal | **IMPL** | `:342-349` | `ne_test.c` |
| OSFIXUP records | **MISS** | refused with an error line (`:392-393`); none in the corpus | — |
| Address types FARADDR48 / OFFSET32 | **MISS** | defined (`:68-69`), refused at `:432` | — |
| Iterated segments (segment flag `0008h`) | **MISS** | not tested; the bytes would be copied as stored | — |
| Moveable entry thunks (`INT 3Fh`) and segment discarding | **N/A** | resolved direct by decision, recorded at `:463-470` — *"the line that breaks"* when discarding arrives | — |

Resources are [win16.md](win16.md)'s (`wowres.h`), not counted here.

## 4. LE / LX

| Unit | Status | Notes |
|---|---|---|
| Loading an LE image | **N/A** | the extender (DOS/4GW) loads its own program through DPMI; we never place an LE object |
| Reading the object table to learn which objects are code | **IMPL** | `dpmi_le_learn` `main.c:18217`, header found by validated search (`:18209-18215`) — instrumentation for the INT-site patcher, not a loader |

## 5. PE, and EXEC of a Windows program

| Unit | Status | Notes |
|---|---|---|
| Loading a PE image | **N/A** | a Win32 program is not a VDM guest |
| EXEC (`4B00h`) of an NE or PE file from DOS | **MISS** | no header check in the EXEC path (`main.c:2116-2284`), so the MZ stub runs. Stock NTVDM hands a PE to Win32 and an NE to WOW. The Open dialog's test (`main.c:10302-10320`) is the one to reuse |

---

## What to fix, in order

1. EXEC: detect NE/PE (the Open dialog's test) and hand the program on — PE to
   `CreateProcess`, NE to WOW — instead of running the stub.
2. EXEC memory from `e_minalloc`/`e_maxalloc`: refuse with 8 when the image plus
   `e_minalloc` does not fit; give `e_maxalloc` (capped) rather than everything; load high.
3. The `.COM` child's `0000h` stack word and the `.COM` size ceiling; AL/AH FCB validity.
4. Bound the MZ header reads by the file length; accept `ZM`.
