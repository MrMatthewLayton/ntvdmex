# Inventory — XMS 3.0, LIM EMS 4.0, VCPI

**Specs:** the *eXtended Memory Specification* version 3.0 (Microsoft/Lotus/Intel/AST); the
*Lotus/Intel/Microsoft Expanded Memory Specification* version 4.0; the *Virtual Control
Program Interface* 1.0 (Phar Lap/Quarterdeck). Ralf Brown's Interrupt List (INT 2Fh
`43xxh`, INT 67h) is the cross-check. ⚠ **None of the three is held in the repo**:
[`../ref/SOURCES.md`](../ref/SOURCES.md) names them (`:64`) and links no copy, and there is no
`docs/ref/xms-ems.md`. The unit lists below are the specs' own function and error tables.
**Our implementation:**
- XMS — the allocator in `src/dos/dos_xms.h` (264 lines); the far-call dispatch `host_xms()`
  in `src/host/main.c` (≈`:6869`–`:6990`, search `query free extended memory`); INT 2Fh
  `4300h`/`4310h` in the BOP 2Fh arm (≈`:28238`, search `XMS installation check`); the entry
  stub `bopxms[]` (≈`:24777`) planted at `DOS_HDLR_SEG:0044h` (≈`:25931`); the HMA in
  `hma_try()` (≈`:966`).
- EMS — the manager in `src/dos/dos_ems.h` (286 lines); the INT 67h dispatch `host_ems()`
  (`main.c` ≈`:7009`–`:7086`); the vector and device name (≈`:25938`–`:25982`); the page
  frame in `v86_map_ems_frame()` (`src/vdm/v86.c:95`).
- `src/dos/dos_extmem.h` (81 lines) — not XMS itself but where INT 15h `AH=87h` resolves an
  address, including one handed out by XMS `0Ch` (lock).
- VCPI — **no code**.

**Off-VM:** `tools/dostest/xms_test.c` (T1–T12), `tools/dostest/ems_test.c` (T1–T12),
`tools/dostest/extmem_test.c` (18 checks). **Rig self-checks** (pass/fail on our host only):
`tools/dostest/xmstest.asm`, `emstest.asm`, and the XMS/EMS steps of `selftest.asm`.
**Oracle probe:** `tools/dostest/p_xms.asm` only (INT 2Fh `4300h`/`4310h`, XMS `00h`, `01h`,
`07h`, `08h`, the HMA read-back); `p_kbc.asm` reads XMS `07h` beside the 8042; `p_int15.asm`
uses XMS `09h`/`0Ch`/`0Ah` to find an address for INT 15h `87h`. **There is no EMS probe and
no VCPI probe.**
**Marked:** 2026-09-29, **from the code**, with citations. ⚠ `src/host/main.c` was being
edited in the working tree while this was written; its line numbers are given with a symbol
or string and will drift — search for the symbol.

⚠ **Verification axis.** Only the `p_xms` rows have been asked of a reference. The latest
five-host diff is `runs/s84/probes/c.txt:227-249` (MS-DOS 6.22 + HIMEM under QEMU, dosbox-x,
PCem, PCem-VESA, NTVDMEX), and stock XP NTVDM was captured separately the same night in
`runs/s84/stock/xms_stock.txt`; the oracle captures are also cached in
`build/dosdiff-cache/p_xms.com-*`. Every other row is at most *untested*: the batteries and rig
self-checks were written against our own model, so they pin what the code does, not what a
driver does.

---

## Headline

**XMS is close to complete for the 2.0 function set; EMS is the LIM 3.2 core with a few 4.0
calls; VCPI does not exist.** XMS `00h`–`11h` all answer, the HMA is real memory, and A20 is
one wire shared with the 8042 and port `92h`. The XMS defects that matter are one of safety
(`0Bh` bounds checks overflow, so a DOS program can move bytes to and from host memory outside
any block) and one of completeness (the 3.0 32-bit functions `88h`/`89h`/`8Eh`/`8Fh` answer
"not implemented" from a driver that reports version 3.00). EMS answers `40h`–`48h`,
`4Bh`–`4Dh`, `51h` and `53h`; **every other LIM 4.0 function answers `84h` "undefined
function"**, so a guest that checked for version 4.0 is told a defined function does not exist.
EMS is also only detectable one of the two ways the spec gives (the vector, not the device
open), and hands out handle 0, which LIM 4.0 reserves for the operating system.

| Surface | Units | IMPL | PART | STORE | MISS | N/A |
|---|---|---|---|---|---|---|
| §1 XMS detection and the entry point | 5 | 4 | — | — | 1 | — |
| §2 XMS driver functions | 25 | 13 | 5 | — | 5 | 2 |
| §3 XMS error codes (BL) | 27 | 15 | 2 | — | 2 | 8 |
| §4 XMS handles, locks, the pool, the HMA, A20 | 11 | 9 | 1 | — | — | 1 |
| **XMS total** | **68** | **41** | **8** | — | **8** | **11** |
| §5 EMS detection, page frame, handles | 10 | 5 | 2 | — | 3 | — |
| §6 EMS functions (every subfunction a unit) | 58 | 11 | 4 | — | 41 | 2 |
| §7 EMS status codes (AH) | 37 | 8 | 3 | — | 22 | 4 |
| **EMS total** | **105** | **24** | **9** | — | **66** | **6** |
| §8 VCPI | 14 | — | — | — | 14 | — |
| **Total** | **187** | **65** | **17** | — | **88** | **17** |

### What has been measured on this surface

| Measurement | Where |
|---|---|
| All five hosts AGREE: `4300h` → `AL=80h`; `00h` → `AX=0300h`, `DX=0001h` (HMA exists); `07h` → `AX=0001h BL=00h` | `runs/s84/probes/c.txt:236-239`, `:247-248` |
| ★ **Stock XP NTVDM reports XMS version 2.00**, revision `0277h` (`AX=0200 BX=0277 DX=0001`); `4300h`, `07h` and the HMA refusal (`91h`) match the DOS oracles. We report 3.00 | `runs/s84/stock/xms_stock.txt` |
| `00h` BX (driver revision): HIMEM `0310h` (6.22, PCem), dosbox-x `0301h`, ours `0300h` — abstained as "a version number for a different program" | `c.txt:238`; `oracle-rules.json` (probe `xms`, case `xms.00.version`, BX); `dos_xms.h:29-35` (0310h tried for #47 and refuted) |
| `08h` BH: 6.22 and both PCems write `AAh` (`BX=AA00`); dosbox-x and stock NTVDM leave the poison (`BX=B100`). Now a setting, "behave like" (#167); ours `B100` by default | `c.txt:242`; `xms_stock.txt`; `main.c` ≈`:6936-6939`; `docs/log/sessions/session-84.md:35`; history `docs/inventory/sweep.md:135` |
| `08h` AX/DX: RAM size per machine (ours `4000h` = 16384 KB) — abstained | `c.txt:240-241`; `oracle-rules.json` |
| HMA: we answered `DX=0` and `BL=90h` until s72; NT had the HMA committed all along. Ours grants `01h` and reads back `A55A`/`1234` through `FFFF:0010`; every oracle **and stock NTVDM** refuse with `91h` (DOS=HIGH) — abstained as configuration | `c.txt:243-246`; `xms_stock.txt`; `docs/inventory/dos-services.md:107-140`; `docs/log/sessions/session-72.md:188`; `oracle-rules.json` `xms.01.request.hma`, `xms.hma.readback` |
| A20 was three flags, XMS owned the only one; the 8042, port `92h` and XMS `03h`–`07h` are one bit now. PCem answers `0101h` to `kbc.a20.readback` and so do we; the enable direction only is tested | `docs/log/sessions/session-77.md:254-295`; `docs/inventory/kbc.md:100`, `:171` |
| MEM /D walked 256 EMS handles until `4Dh` and `53h` existed (#47) | `docs/log/sessions/session-81.md:357`; `dos_ems.h:193-216` |
| Rig self-test `selftest.com` 8/8 including XMS and EMS | `docs/log/sessions/session-37.md:637`; `session-72.md:142` |

---

## 1. XMS — detection and the entry point

| Unit | Status | Where / what is missing | Verification |
|---|---|---|---|
| INT 2Fh `AX=4300h` — installation check, `AL=80h` | **IMPL** | `main.c` ≈`:28238` | **oracle** — `int2F.4300.installed` AGREE on five hosts (`c.txt:236`) and stock |
| INT 2Fh `AX=4310h` — entry point in `ES:BX` | **IMPL** | ≈`:28240`, returns `DOS_HDLR_SEG:XMS_ENTRY_OFF` = `0050:0044` (`main.c:502`) | untested — `int2F.4310.entry.seg.nonzero` has an empty signature, so it compares nothing (`p_xms.asm:46`); the far calls that follow it working is the real evidence |
| XMS switched off (Settings) → neither `4300h` nor `4310h` answers | **IMPL** | `g_xms_on` (`main.c` ≈`:10330`, `:10352`); the "not answering, not answering badly" note at ≈`:28232` | untested |
| The control function: far call → `BOP 43h ; RETF`, dispatched to `host_xms()` | **IMPL** | stub `bopxms[]` ≈`:24777`, planted ≈`:25931`; dispatch ≈`:28331`–`:28359` (EIP += 3 → the RETF) | `xmstest.asm` (rig self-check) |
| The hookable prologue — the spec requires the control function to begin with a short jump and three NOPs, so another program can patch a far jump over those five bytes | **MISS** | our entry is four bytes and the INT 67h stub starts at `0048h` (≈`:25938`), so a 5-byte far-jump patch at `0044h` **overwrites the first byte of the EMS entry** | — |

## 2. XMS — driver functions (AH)

Every call returns `AX=1` on success, `AX=0` with an error in BL on failure; carry is not used
(`main.c` ≈`:6867`). An unknown function falls to `X_FAIL(80h)` (≈`:6982`).

| AH | Function | Status | Where / what is missing | Verification |
|---|---|---|---|---|
| `00h` | Get version: AX=version, BX=revision, DX=HMA exists | **IMPL** | ≈`:6895`; `AX=0300h`, `BX=0300h` (`dos_xms.h:28`, `:35`), `DX = g_hma ? 1 : 0` | **oracle** — AX and DX AGREE with the DOS oracles (`c.txt:237`, `:239`); BX abstained. ⚠ Stock NTVDM answers `AX=0200h` (`xms_stock.txt`) |
| `01h` | Request HMA (DX = bytes, `FFFFh` = TSR/driver) | **IMPL** | ≈`:6897`–`:6905`; `90h` if no HMA, `91h` if taken. DX is not examined, which is the behaviour of HIMEM with `/HMAMIN=0`. ⚠ The comment says a request "larger than the HMA is refused"; no code does that | abstained (DOS=HIGH on every oracle); our read-back passes (`xms.hma.readback`) |
| `02h` | Release HMA | **IMPL** | ≈`:6906`–`:6910`; `90h`, `93h` | untested |
| `03h` | Global enable A20 | **IMPL** | ≈`:6924`–`:6925`: `vdd_input_a20_set(1)`, the one A20 bit | **oracle**, enable direction only (`kbc.a20.readback`, PCem `0101h`) |
| `04h` | Global disable A20 | **PART** | ≈`:6926`–`:6927`: clears the bit, so `07h` and the 8042 read "off"; **the address space does not wrap** (§4, recorded decision). Never returns `94h` | untested |
| `05h` | Local enable A20 | **PART** | shares `03h`'s arm (≈`:6924`); **no nesting count** — the spec counts local enables so that a matching number of local disables is needed | untested |
| `06h` | Local disable A20 | **PART** | shares `04h`'s arm; disables at once even with other local enables outstanding; never `94h` ("A20 still enabled"); no wrap | untested |
| `07h` | Query A20: AX=1 on, 0 off; BL=0 | **IMPL** | ≈`:6928`–`:6930`, reads the controller's bit | **oracle** — `xms.07.query.a20` `AX=0001 BX=B100` AGREE on five hosts (`c.txt:247-248`) and stock |
| `08h` | Query free extended memory: AX=largest KB, DX=total KB | **IMPL** | ≈`:6931`–`:6940`; `dos_xms.h:124-129`; BL=`A0h` when nothing is free; BH `AAh` only under "behave like MS-DOS 6.22" | AX/DX abstained (RAM size); BH per #167 above; `xms_test.c` T1, T2, T5 |
| `09h` | Allocate EMB, DX=KB → DX=handle | **IMPL** | ≈`:6941`; `dos_xms.h:135-149`; 0 KB is legal; `A0h`, `A1h` | `xms_test.c` T2, T4, T5, T12; `xmstest.asm` |
| `0Ah` | Free EMB | **IMPL** | ≈`:6945`; `dos_xms.h:153-161`; `A2h`, `ABh` if locked | `xms_test.c` T9, T11 |
| `0Bh` | Move EMB (DS:SI → 16-byte structure) | **PART** | ≈`:6949`–`:6959`; `dos_xms.h:233-262`. Round trips, EMB↔EMB, overlap in both directions, odd length `A7h` all work. ⛔ **The bounds test `offset + len > size` is 32-bit and wraps** (`dos_xms.h:246`, `:255`): offset `FFFFF000h` with length `2000h` passes and copies from `mem − 1000h`, i.e. host memory outside the block. ⛔ A handle-0 (conventional) endpoint has **no length limit at all** (`:240-242`, `:249-251`), so a long move from `9000:0000` runs past `0x10FFF0` into whatever the host has mapped — the exact hazard `dos_extmem.h:1-20` exists to refuse for INT 15h `87h` | `xms_test.c` T6–T8 (none probes the wrap); `xmstest.asm` |
| `0Ch` | Lock EMB → DX:BX 32-bit address | **PART** | ≈`:6960`; `dos_xms.h:209-216`. Lock count and `ACh` are right. The address is **the host heap pointer**, not a physical address: usable by a client that shares our flat address space, and by INT 15h `87h` (`dos_extmem.h:13-15`, `:44-53`), but not by anything that treats it as physical (a DMA programme, a VCPI client's page tables). A 0-KB block locks to address 0 | `xms_test.c` T9; `p_int15.asm` uses it (`session-81.md:309-316`) |
| `0Dh` | Unlock EMB | **IMPL** | ≈`:6965`; `dos_xms.h:218-224`; `AAh` | `xms_test.c` T9 |
| `0Eh` | Get handle info: BH=lock count, BL=free handles, DX=KB | **IMPL** | ≈`:6969`–`:6974`; `dos_xms.h:186-201` | `xms_test.c` T3, T9 |
| `0Fh` | Reallocate EMB, BX=new KB | **IMPL** | ≈`:6975`–`:6979`; `dos_xms.h:165-183`; content kept, `ABh` if locked, `A0h` | `xms_test.c` T10 |
| `10h` | Request UMB | **IMPL** | ≈`:6980`: `AX=0 BL=B1h DX=0` — the spec's answer for a driver with no UMBs, which we are | untested |
| `11h` | Release UMB | **IMPL** | ≈`:6981`: `B2h` (no UMB can be valid) | untested |
| `12h` | Reallocate UMB (3.0) | **MISS** | falls to `80h` (≈`:6982`); a no-UMB driver's consistent answer would be `B2h` | — |
| `88h` | Query any free extended memory (3.0, 32-bit: EAX, EDX, ECX=highest address) | **MISS** | `80h`. ⚠ We report version 3.00 (`00h`), so a 3.0-aware client may ask this first; stock NTVDM reports 2.00 and is never asked | — |
| `89h` | Allocate any extended memory (3.0, EDX=KB) | **MISS** | `80h` | — |
| `8Eh` | Get extended EMB handle info (3.0, EDX=KB, CX=free handles) | **MISS** | `80h` | — |
| `8Fh` | Reallocate any extended memory (3.0, EBX=KB) | **MISS** | `80h` | — |
| `C8h` | Query free super-extended memory | **N/A** | not an XMS 3.0 function (a later, unofficial extension of the 3.x interface); `80h` is what a 3.0 driver answers | — |
| `C9h` | Allocate super-extended memory | **N/A** | as `C8h` | — |

## 3. XMS — error codes (BL)

A code is **IMPL** when it is raised under the condition the spec gives it, **N/A** when that
condition cannot arise on this machine, **MISS** when the condition can arise and a different
answer (or none) is given.

| BL | Meaning | Status | Where / why |
|---|---|---|---|
| `80h` | Function not implemented | **IMPL** | default arm ≈`:6982`; `dos_xms.h:38` |
| `81h` | VDISK device detected | **N/A** | no CONFIG.SYS drivers load; there is no VDISK to find |
| `82h` | A20 error | **N/A** | A20 is a bit in `vdd_input`; setting it cannot fail |
| `8Eh` | General driver error | **MISS** | defined (`dos_xms.h:39`), never raised. The one condition that fits — the host refusing to commit a block the pool accounting says is free — reports `A0h` instead (`dos_xms.h:143`, `:175`) |
| `8Fh` | Unrecoverable driver error | **N/A** | no such state |
| `90h` | HMA does not exist | **IMPL** | `01h`/`02h` when `g_hma == 0` (≈`:6902`, `:6907`) |
| `91h` | HMA already in use | **IMPL** | ≈`:6903` |
| `92h` | DX less than `/HMAMIN` | **N/A** | there is no HMAMIN; every size is accepted, as `/HMAMIN=0` |
| `93h` | HMA not allocated | **IMPL** | ≈`:6908` |
| `94h` | A20 still enabled | **MISS** | needs the local-enable count (§2 `05h`/`06h`) |
| `A0h` | All extended memory allocated | **IMPL** | `dos_xms.h:138`, `:172`; `08h` BL (≈`:6935`) |
| `A1h` | All handles in use | **IMPL** | `dos_xms.h:140` |
| `A2h` | Invalid handle | **IMPL** | `dos_xms.h:155`, `:168`, `:192`, `:211`, `:220` |
| `A3h` | Invalid source handle | **IMPL** | `dos_xms.h:245` |
| `A4h` | Invalid source offset | **PART** | `dos_xms.h:246` — missed when offset + length wraps 32 bits (§2 `0Bh`) |
| `A5h` | Invalid destination handle | **IMPL** | `dos_xms.h:254` |
| `A6h` | Invalid destination offset | **PART** | `dos_xms.h:255` — same wrap |
| `A7h` | Invalid length (odd) | **IMPL** | `dos_xms.h:238` |
| `A8h` | Move has an invalid overlap | **N/A** | both directions of overlap are copied correctly (`dos_xms.h:258-260`); there is nothing to refuse |
| `A9h` | Parity error | **N/A** | no parity memory |
| `AAh` | Block not locked | **IMPL** | `dos_xms.h:221` |
| `ABh` | Block locked | **IMPL** | `dos_xms.h:156`, `:169` |
| `ACh` | Lock count overflow | **IMPL** | `dos_xms.h:212` (at `FFh`) |
| `ADh` | Lock failed | **N/A** | a block can always be locked in this model |
| `B0h` | Smaller UMB available | **N/A** | no UMBs |
| `B1h` | No UMBs available | **IMPL** | `10h` ≈`:6980` |
| `B2h` | Invalid UMB segment | **IMPL** | `11h` ≈`:6981` |

## 4. XMS — handles, locks, the pool, the HMA, A20

| Unit | Status | Where / what is missing | Verification |
|---|---|---|---|
| Handle table — 64 handles (the spec leaves the number to the driver; HIMEM's default is 32) | **IMPL** | `dos_xms.h:27` | `xms_test.c` T12 |
| Handle values opaque, `1..64`, 0 never issued | **IMPL** | `dos_xms.h:115-119`, `:147` | `xms_test.c` T2, T5 |
| Per-handle 8-bit lock count | **IMPL** | `dos_xms.h:58`, `:209-224` | `xms_test.c` T9 |
| Free and reallocate refused while locked | **IMPL** | `dos_xms.h:156`, `:169` | `xms_test.c` T9 |
| Zero-KB blocks | **IMPL** | `dos_xms.h:131-149` | `xms_test.c` T5 |
| Largest free block = total free (each EMB is its own host allocation, so the pool cannot fragment) | **IMPL** | `dos_xms.h:121-129`; `xms_host_alloc` ≈`:6856` | `xms_test.c` T2 |
| Pool = 16384 KB, and SysVars+`45h` reports the same number (MEM.EXE reads it there, #47) | **IMPL** | `XMS_POOL_KB` `main.c:505`; SysVars ≈`:26547`; `xms_init` ≈`:26637` | MEM reports it (`session-81.md` Part 8) |
| The pool against INT 15h `AH=88h` | **PART** | `88h` answers `3C00h` = 15360 KB of **raw** extended memory (≈`:27938`) while XMS hands out 16384 KB more; HIMEM hooks `88h` and reports what it has left, which is 0. The defect is recorded at the arm (≈`:27928`) and a change was tried and reverted. ⚠ The arm's "matching the XMS pool" is not even numerically true (`3C00h` ≠ 16384) | untested |
| The HMA — 64 KB−16 at linear `100000h`, reached as `FFFF:0010` | **IMPL** | `hma_try()` ≈`:966`–`:993` (query first; commit only if free/reserved) | ours `A55A`/`1234` read back (`dos-services.md:122-126`) |
| A20 address wrap at 1 MB | **N/A** | recorded decision: "an NT VDM does not wrap at 1 MB" (`dos_xms.h:95-104`; `main.c` ≈`:26647`–`:26652`). A program that disables A20 and expects `FFFF:0010` to alias `0000:0000` sees the HMA | — |
| A20 is one bit shared by XMS, the 8042 output port and port `92h` | **IMPL** | ≈`:6911`–`:6930`; `vdd_input_a20_get/set` | **oracle**, enable direction (`kbc.md:100`, `:171`) |

---

## 5. EMS — detection, the page frame, handles

| Unit | Status | Where / what is missing | Verification |
|---|---|---|---|
| INT 67h vector → `DOS_HDLR_SEG:0048h` (`BOP 67h ; IRET`), absent when EMS is off | **IMPL** | `bop67[]` ≈`:24778`; planted ≈`:25938`–`:25949`; dispatch ≈`:28361` | `emstest.asm` step 1 (rig self-check) |
| Detection by the vector: `"EMMXXXX0"` at vector-segment:`000Ah` | **IMPL** | `DOS_EMM_NAME_OFF` `dos_layout.h:26`; `emmname[]` ≈`:24794`; written ≈`:25981` | `emstest.asm`, `selftest.asm:312` (rig self-check) |
| Detection by opening the device: INT 21h `3Dh` on `EMMXXXX0`, then IOCTL `4400h` (is a device) and `4407h` (output ready) — the spec's other documented method | **MISS** | no DOS device exists; the name goes to Win32 through `v86_path()` (`src/dos/dos_int21.c:389`) and the open fails. ⚠ The comment at ≈`:25939` says a program detects the EMM by "following the vector … OR by opening the device" — only the first is wired | — |
| `EMMXXXX0` in the device-driver chain (a walker from the NUL header finds it) | **MISS** | the NUL header terminates at `FFFF:FFFF` (≈`:26554`–`:26560`) | — |
| The page frame — 64 KB, found by scanning `D000h`, `C000h`, `E000h` for a free hole after `VdmInitialize` | **PART** | `v86_map_ems_frame()` `v86.c:95-117`; called ≈`:25262`. ⛔ **When no hole is free it returns 0 and nothing checks**: `ems_init()` gets segment 0 and frame pointer 0 (≈`:26657`), INT 67h stays installed, `41h` answers `BX=0000h AH=00h`, and the first `44h` copies 16 KB **over the IVT** (`dos_ems.h:156`) | logged as `STAGE1: ems_frame lin=` (≈`:25264`) |
| Four 16 KB physical pages, `0`–`3` | **IMPL** | `dos_ems.h:29-31`; `8Bh` above 3 (`:145`). No conventional-memory mappable pages (a 4.0 option) | `ems_test.c` T5 |
| The mapping model — a mapped page is **copied** into the frame and written back when the window is remapped (page-frame shadowing) | **PART** | `dos_ems.h:10-18`, `:108-115`, `:143-161`. Correct for one page in one window. ⚠ The same logical page mapped into two windows is **two copies**, not an alias: a write through one is not seen through the other, and whichever window is remapped last overwrites the other's changes | `ems_test.c` T6–T8 (none maps one page twice) |
| Handle 0 — reserved by LIM 4.0 for the operating system; applications are never given it | **MISS** | `ems_alloc()` takes the first free slot starting at 0 (`dos_ems.h:130-136`), so the first `43h` returns handle 0 | `ems_test.c` does not check the value |
| Handle table — 64 | **IMPL** | `dos_ems.h:32` | `ems_test.c` T12 |
| Pool — 512 pages = 8 MB | **IMPL** | `EMS_POOL_PAGES` `main.c:688`; `ems_init` ≈`:26657` | untested |

## 6. EMS — functions (AH, and AL where the function has subfunctions)

The dispatch is `host_ems()`, `main.c` ≈`:7009`–`:7086`; every AH not listed in its `switch`
falls to `84h` "undefined function" (≈`:7080`).

| AH/AL | Function | Status | Where / what is missing | Verification |
|---|---|---|---|---|
| `40h` | Get status | **IMPL** | ≈`:7021`, always `00h` | untested |
| `41h` | Get page-frame segment → BX | **IMPL** | ≈`:7022` (see §5 for the no-frame case) | `emstest.asm` step 3 |
| `42h` | Unallocated / total pages → BX / DX | **IMPL** | ≈`:7023`; `dos_ems.h:118-121` | `ems_test.c` T1, T3 |
| `43h` | Allocate pages → DX handle | **PART** | ≈`:7027`; `dos_ems.h:125-139`; `89h`, `87h`, `88h`, `85h` right. **Can return handle 0** (§5) | `ems_test.c` T2–T4; `emstest.asm` step 4 |
| `44h` | Map/unmap handle page (AL=physical, BX=logical, `FFFFh` unmaps) | **IMPL** | ≈`:7031`; `dos_ems.h:143-161` | `ems_test.c` T5–T8; `emstest.asm` steps 5–7 |
| `45h` | Deallocate pages | **PART** | ≈`:7037`; `dos_ems.h:164-175`. Never returns `86h` when the handle still has a saved context (`47h`) — it frees the handle and the context with it | `ems_test.c` T11 |
| `46h` | Get version → AL=`40h` | **IMPL** | ≈`:7041` | `emstest.asm` step 2 |
| `47h` | Save page map (for DX) | **IMPL** | ≈`:7042`; `dos_ems.h:257-270`; `8Dh` | `ems_test.c` T9 |
| `48h` | Restore page map | **IMPL** | ≈`:7046`; `dos_ems.h:272-284`; `8Eh` | `ems_test.c` T9 |
| `49h` | (3.x: get I/O port addresses) | **N/A** | withdrawn in 4.0; `84h` is the 4.0 answer | — |
| `4Ah` | (3.x: get translation array) | **N/A** | withdrawn in 4.0; `84h` | — |
| `4Bh` | Get handle count → BX | **IMPL** | ≈`:7050`; `dos_ems.h:187-191` | `ems_test.c` T3, T11 |
| `4Ch` | Get pages owned by handle → BX | **IMPL** | ≈`:7051`; `dos_ems.h:178-184` | `ems_test.c` T3 |
| `4Dh` | Get all handle pages → ES:DI pairs, BX count | **IMPL** | ≈`:7055`–`:7062`; `dos_ems.h:198-211` | `ems_test.c` T12; MEM /D (`session-81.md:357`) |
| `4Eh`/`00h` | Get page map → ES:DI | **MISS** | `84h` | — |
| `4Eh`/`01h` | Set page map ← DS:SI | **MISS** | `84h` | — |
| `4Eh`/`02h` | Get and set page map | **MISS** | `84h` | — |
| `4Eh`/`03h` | Get size of page-map save array | **MISS** | `84h` | — |
| `4Fh`/`00h` | Get partial page map | **MISS** | `84h` | — |
| `4Fh`/`01h` | Set partial page map | **MISS** | `84h` | — |
| `4Fh`/`02h` | Get size of partial save array | **MISS** | `84h` | — |
| `50h`/`00h` | Map/unmap multiple pages, physical-page mode | **MISS** | `84h` | — |
| `50h`/`01h` | Map/unmap multiple pages, segment mode | **MISS** | `84h` | — |
| `51h` | Reallocate pages → BX | **PART** | ≈`:7073`–`:7079`; `dos_ems.h:230-254`; content kept, 0 pages allowed. A request above the whole pool answers `88h`, not `87h` (`:235-236`) | `ems_test.c` T10 |
| `52h`/`00h` | Get handle attribute | **MISS** | `84h` | — |
| `52h`/`01h` | Set handle attribute | **MISS** | `84h` | — |
| `52h`/`02h` | Get attribute capability | **MISS** | `84h` | — |
| `53h`/`00h` | Get handle name → ES:DI | **IMPL** | ≈`:7063`–`:7072`; `dos_ems.h:217-225`; `83h` for an unused handle | `ems_test.c` T12 |
| `53h`/`01h` | Set handle name ← DS:SI | **PART** | same arm. Does not refuse a duplicate non-blank name with `A1h` | `ems_test.c` T12 |
| `54h`/`00h` | Get handle directory | **MISS** | `84h` | — |
| `54h`/`01h` | Search for named handle | **MISS** | `84h` | — |
| `54h`/`02h` | Get total handles | **MISS** | `84h` | — |
| `55h`/`00h` | Alter page map and jump, physical-page mode | **MISS** | `84h` | — |
| `55h`/`01h` | Alter page map and jump, segment mode | **MISS** | `84h` | — |
| `56h`/`00h` | Alter page map and call, physical-page mode | **MISS** | `84h` | — |
| `56h`/`01h` | Alter page map and call, segment mode | **MISS** | `84h` | — |
| `56h`/`02h` | Get page-map stack space size | **MISS** | `84h` | — |
| `57h`/`00h` | Move memory region (conventional ↔ expanded) | **MISS** | `84h` | — |
| `57h`/`01h` | Exchange memory region | **MISS** | `84h` | — |
| `58h`/`00h` | Get mappable physical address array | **MISS** | `84h` — the call a 4.0 client makes to learn where the physical pages are | — |
| `58h`/`01h` | Get number of mappable physical pages | **MISS** | `84h` | — |
| `59h`/`00h` | Get hardware configuration array (OS/E) | **MISS** | `84h`; the spec lets a manager deny OS/E functions with `A4h`, which would be a truthful answer | — |
| `59h`/`01h` | Get unallocated raw page count | **MISS** | `84h` | — |
| `5Ah`/`00h` | Allocate standard pages (0 allowed) | **MISS** | `84h` | — |
| `5Ah`/`01h` | Allocate raw pages | **MISS** | `84h` | — |
| `5Bh`/`00h` | Get alternate map register set (OS/E) | **MISS** | `84h` | — |
| `5Bh`/`01h` | Set alternate map register set | **MISS** | `84h` | — |
| `5Bh`/`02h` | Get alternate map save array size | **MISS** | `84h` | — |
| `5Bh`/`03h` | Allocate alternate map register set | **MISS** | `84h` | — |
| `5Bh`/`04h` | Deallocate alternate map register set | **MISS** | `84h` | — |
| `5Bh`/`05h` | Allocate DMA register set | **MISS** | `84h` | — |
| `5Bh`/`06h` | Enable DMA on alternate map register set | **MISS** | `84h` | — |
| `5Bh`/`07h` | Disable DMA on alternate map register set | **MISS** | `84h` | — |
| `5Bh`/`08h` | Deallocate DMA register set | **MISS** | `84h` | — |
| `5Ch` | Prepare EMM for warm boot | **MISS** | `84h` | — |
| `5Dh`/`00h` | Enable OS/E function set (access key) | **MISS** | `84h` | — |
| `5Dh`/`01h` | Disable OS/E function set | **MISS** | `84h` | — |
| `5Dh`/`02h` | Return access key | **MISS** | `84h` | — |

## 7. EMS — status codes (AH)

Same rule as §3.

| AH | Meaning | Status | Where / why |
|---|---|---|---|
| `80h` | Internal software error | **MISS** | defined (`dos_ems.h:37`), never raised; a host commit failure reports `88h` instead (`dos_ems.h:133`, `:242`) |
| `81h` | Hardware malfunction | **N/A** | no EMS hardware; the pages are host heap |
| `82h` | EMM busy (3.x) | **N/A** | never busy |
| `83h` | Invalid handle | **IMPL** | `ems_get()` callers, `dos_ems.h:147`, `:167`, `:180`, `:221`, `:233`, `:260`, `:275` |
| `84h` | Undefined function | **PART** | right for `49h`/`4Ah`/`5Eh`+ — but also returned for the 41 **defined** 4.0 subfunctions in §6 that are missing (≈`:7080`) |
| `85h` | No more handles | **IMPL** | `dos_ems.h:131` |
| `86h` | Save/restore context error | **MISS** | `45h` on a handle with a saved context (§6) |
| `87h` | More pages than exist | **PART** | `43h` (`dos_ems.h:128`); not `51h` |
| `88h` | More pages than available | **IMPL** | `dos_ems.h:129`, `:236` |
| `89h` | Zero pages requested (`43h`) | **IMPL** | `dos_ems.h:127` |
| `8Ah` | Logical page out of range | **IMPL** | `dos_ems.h:154` |
| `8Bh` | Illegal physical page | **IMPL** | `dos_ems.h:145` |
| `8Ch` | Page-map save area full | **N/A** | one save slot per handle (`dos_ems.h:56-57`); it cannot fill before `8Dh` applies |
| `8Dh` | Context already saved for handle | **IMPL** | `dos_ems.h:261` |
| `8Eh` | No saved context for handle | **IMPL** | `dos_ems.h:276` |
| `8Fh` | Undefined subfunction | **PART** | only `53h` raises it (≈`:7068`); every other subfunction-bearing function is missing whole |
| `90h` | Undefined attribute type | **MISS** | `52h` missing |
| `91h` | Feature not supported | **MISS** | `52h` missing |
| `92h` | Move succeeded, source overwritten (overlap) | **MISS** | `57h` missing |
| `93h` | Region longer than the handle's allocation | **MISS** | `57h` missing |
| `94h` | Conventional and expanded regions overlap | **MISS** | `57h` missing |
| `95h` | Offset beyond the logical page | **MISS** | `57h` missing |
| `96h` | Region longer than 1 MB | **MISS** | `57h` missing |
| `97h` | Same handle, overlapping regions (exchange) | **MISS** | `57h` missing |
| `98h` | Memory type undefined | **MISS** | `57h` missing |
| `99h` | — | **N/A** | not assigned in the 4.0 list (RBIL: unused) |
| `9Ah` | Alternate map register set not supported | **MISS** | `5Bh` missing |
| `9Bh` | All alternate map/DMA sets allocated | **MISS** | `5Bh` missing |
| `9Ch` | Alternate map/DMA sets not supported | **MISS** | `5Bh` missing |
| `9Dh` | Alternate map set not defined/allocated | **MISS** | `5Bh` missing |
| `9Eh` | Dedicated DMA channels not supported | **MISS** | `5Bh` missing |
| `9Fh` | Specified DMA channel not supported | **MISS** | `5Bh` missing |
| `A0h` | No handle with that name | **MISS** | `54h`/`01h` missing |
| `A1h` | Duplicate handle name | **MISS** | `53h`/`01h` does not check (§6); `54h` missing |
| `A2h` | Memory address wraps (move) | **MISS** | `57h` missing |
| `A3h` | Invalid pointer / corrupt source array | **MISS** | `4Eh`/`4Fh`/`50h` missing |
| `A4h` | Access denied by the operating system | **MISS** | `59h`/`5Bh`/`5Dh` missing |

---

## 8. VCPI

**Nothing is implemented.** INT 67h `AH=DEh` falls to `host_ems()`'s default and answers
`AH=84h` (≈`:7080`); VCPI detection requires `AH=00h` from `DE00h`, so every client is told,
coherently, that no VCPI server is present. No probe asks the question of any oracle, so
what stock NTVDM answers is **unmeasured**.

⚠ **Scope is undecided, and should be decided rather than defaulted.** `DE0Ch` hands the
client control at CPL 0 on its own GDT, IDT and page tables, which a user-mode NT process
cannot give it natively; whether that is out of scope (N/A, with the reason recorded) or
something to model through the protected-mode interpreter is a decision nobody has recorded.
Until it is, the rows stay MISS.

| AX | Function | Status |
|---|---|---|
| `DE00h` | Installation check → AH=0, BX=version | **MISS** (answers `84h` = not present) |
| `DE01h` | Get protected-mode interface (page table, GDT descriptors, entry offset) | **MISS** |
| `DE02h` | Get maximum physical memory address | **MISS** |
| `DE03h` | Get number of free 4 KB pages | **MISS** |
| `DE04h` | Allocate a 4 KB page → physical address | **MISS** |
| `DE05h` | Free a 4 KB page | **MISS** |
| `DE06h` | Get physical address of a page in the first megabyte | **MISS** |
| `DE07h` | Read CR0 | **MISS** |
| `DE08h` | Read debug registers DR0–DR7 | **MISS** |
| `DE09h` | Load debug registers | **MISS** |
| `DE0Ah` | Get 8259A interrupt vector mappings | **MISS** |
| `DE0Bh` | Set 8259A interrupt vector mappings | **MISS** |
| `DE0Ch` | Switch from V86 mode to protected mode | **MISS** |
| — | The protected-mode far-call entry from `DE01h` (`DE03h`–`DE05h` from PM; `DE0Ch` back to V86) | **MISS** |

---

## Gaps worth closing

Ordered by importance: safety first, then what makes a guest take a wrong branch, then
completeness.

1. **XMS `0Bh` can move bytes to and from host memory outside any block.** The EMB bounds
   test wraps at 32 bits (`dos_xms.h:246`, `:255`) and a conventional endpoint has no limit
   at all (`:240-251`). Any DOS program can read or overwrite host memory — or fault the host
   — through the XMS driver, which is the hazard `dos_extmem.h` was written to refuse for INT
   15h `87h`. Compare in 64 bits (or `offset > size || len > size − offset`) and bound a
   conventional endpoint to `0x10FFF0`; add the wrap case to `xms_test.c` T8.
2. **EMS cannot be found by the open-handle method.** LIM 4.0 documents two detection
   methods; we answer only the vector one (§5). A program that opens `EMMXXXX0` and asks IOCTL
   `4400h`/`4407h` concludes there is no expanded memory at all. Needs an `EMMXXXX0` character
   device answering `3Dh`, `4400h`, `4407h` and `3Eh` (and a chain entry, for walkers).
3. **LIM 4.0 is advertised (`46h` → `40h`) and 41 of its subfunctions answer "undefined
   function".** The ones a 4.0 client is most likely to call are `58h` (where the physical
   pages are), `4Eh`/`4Fh` (page-map save/restore, used by TSRs and interrupt handlers),
   `50h` (map multiple) and `57h` (move/exchange). `84h` on a defined function is the PART
   shape the README warns about: a plausible wrong answer. The OS/E set (`59h`, `5Bh`, `5Dh`)
   can truthfully answer `A4h` "access denied" instead of being built.
4. **No EMS page frame means an EMS that writes over the IVT.** When
   `v86_map_ems_frame()` finds no free 64 KB (`v86.c:116`), nothing checks the 0 it returns
   (`main.c` ≈`:26657`): the INT 67h vector stays installed, `41h` reports segment 0, and the
   first `44h` copies 16 KB to linear 0. Treat a zero frame as "EMS off" (no vector, no name),
   or answer `80h`, and log it.
5. **XMS reports version 3.00 without the 3.0 functions.** `88h`, `89h`, `8Eh`, `8Fh` answer
   `80h` (§2). Each is a 32-bit twin of `08h`, `09h`, `0Eh`, `0Fh` on the same allocator, so
   the cost is small; `12h` should answer `B2h` like its UMB siblings. ⚠ Stock NTVDM
   reports **2.00** (`runs/s84/stock/xms_stock.txt`) and 6.22's HIMEM 3.00, so this is also a
   "behave like" question (#167) — but the spec says a 3.00 driver has these functions, so
   the version we report and the functions we answer must agree either way.

After those:

6. **EMS handle 0** is given to the first application (`dos_ems.h:130`); LIM 4.0 reserves it
   for the operating system. Start application handles at 1 and keep 0 as the 0-page system
   handle, so `4Bh`, `4Dh` and MEM /D report what a real EMM does.
7. **One logical page in two windows is two copies** (§5, the mapping model). Either refuse
   nothing and write back/refresh every window that shows the same page on each map, or record
   the limitation as a decision.
8. **INT 15h `88h` and XMS hand out the same memory twice** (§4). HIMEM's behaviour is to
   report 0 from `88h` once it owns extended memory. The arm records the defect and a reverted
   attempt (≈`:27928`); it needs doing deliberately with Doom and the DOS batteries re-gated.
9. **A20 local enable/disable have no count and `94h` is never returned** (§2 `05h`/`06h`).
10. **The XMS entry cannot be hooked** — no short-jump prologue, and a hooker's 5-byte far
    jump lands on the INT 67h stub (§1). Move the INT 67h stub and give the entry
    `EB 03 90 90 90` before the BOP.
11. **The smaller EMS defects:** `45h` without `86h`; `51h` without `87h`; `53h`/`01h` without
    `A1h`; host commit failures reported as "out of memory" (`88h`, XMS `A0h`) rather than
    `80h`/`8Eh`.
12. **XMS `0Ch` returns a host address, not a physical one** (§2). Record which clients this
    can serve (DPMI clients on our flat space, INT 15h `87h`) and which it cannot (anything
    that programs DMA or page tables from it), next to the VCPI scope decision.
13. **VCPI scope** — decide N/A-with-reason or build (§8), and add a `DE00h` row to a probe so
    stock NTVDM's answer is measured rather than assumed.
14. **Probes.** There is no EMS probe and `p_xms` asks 5 of 25 XMS functions. An oracle probe
    for `09h`–`0Fh` (with poisoned outputs and the error paths), EMS `40h`–`4Dh`, `51h`, `53h`,
    `58h` and `DE00h` would move most of this document off *untested*.
