# Inventory — DPMI host (INT 2Fh `1687h`, the mode switch, INT 31h)

**Spec:** *DOS Protected Mode Interface Specification* 0.9 and 1.0 (DPMI Committee).
⚠ **Not held in the repo** — [`../ref/SOURCES.md`](../ref/SOURCES.md). We **claim 0.90**
(`0400h` and `1687h` both say so), so the 1.0-only functions below are not promised to a
client; they are listed because the README names DPMI 1.0 as the target.
**Our implementation:** host code, not a VDD.
- Detection and entry: INT 2Fh `1687h` (`src/host/main.c:29443-29457`); the far-called
  entry is `BOP 50h` (`DPMI_BOP`, `main.c:550`), serviced at `main.c:29573-29588` by
  `dpmi_switch_to_pm` (`src/vdm/dpmi.c:38`); initial client state (ES = PSP selector, the
  environment converted) `main.c:29613+`.
- INT 31h: one `switch (ax)` in the PM service routine, `main.c:22569-23822`; anything
  without a `case` falls to `default:` — CF=1, logged `UNSUP` (`main.c:23816-23819`).
- Interrupts and exceptions: PM IRQ injection `dpmi_inject_pm_irq` (`main.c:24864`),
  reflection to real mode (`main.c:22303-22334`, `dpmi_reflect_irq_to_rm` `:19995`), client
  exception dispatch (`main.c:31191-31263`) and its return (`:31412+`), real-mode callbacks
  `dpmi_invoke_callback` (`:19124`).
- INT 21h from protected mode: a translating subset, `main.c:23826-24546` (§9).

**Probes:** `tests/probes/dos/p_dpmi31.com` (#248) asks INT 31h function by function from a
real 16-bit client — 0400h vs 1687h, every bad-selector path, 0100h/0101h × 2100, 0303h × 17,
0304h, 0501h–0503h — each row graded against the spec (expected value beside each `EMIT`).
⚠ **No oracle can answer it:** MS-DOS 6.22, DOSBox-X and PCem have no DPMI host
(`p_dpmins.com`), so they print one `nodpmi` row; stock ntvdm needs the IFEO bracket.
Off-VM: `tests/unit/dpmisvc_test.c` holds the spec-decided rules (`src/host/dpmi_svc.h`).
`dpmitest.asm`, `dpmiexe.asm`, `pm32*.asm` exercise paths; the guest shelf (Doom, Heretic,
Hexen, Duke3D, ZAR, heaven7, Win16's krnl386) remains the integration test.
**Marked:** 2026-10-01, **from the code**; #248 rows re-marked 2026-10-02 (`runs/s87_dpmi/`).

---

## Headline

**Everything DOS/4GW, DOS/16M and krnl386 have been seen to call is implemented, and the
holes are exactly where no shelf guest has walked.** Four of them were in DPMI **0.9** core,
not 1.0 (the first is closed by #247):

1. ~~**`0300h` (simulate real-mode interrupt) services three vectors**~~ — **fixed by #247**
   (see §5): every vector now runs from the real-mode IVT, ours included, and every
   register is written back. Was: INT 21h, 33h and 10h only; every other vector echoed
   with **CF=0**; `ES`/`DS`/`BP` dropped; and INT 21h's carry never reached the RMCS at all.
2. ~~**`0503h` (resize memory block) is UNSUP.**~~ — **fixed by #248** (§7).
3. ~~**`0304h` is UNSUP, and there are only 4 callback slots.**~~ — **fixed by #248**: 16
   slots (the spec's minimum) at `DOS_HDLR_SEG:0090h–00CFh`, and `0304h` frees them (§5).
4. **`0B00h`–`0B03h` (debug watchpoints) are UNSUP.**

~~And one inconsistency: `1687h` reported CL = 4 while `0400h` reported CL = 3~~ — **fixed by
#248**: both read `DPMI_CPU_CLASS` from `src/host/dpmi_svc.h` (§1, §6).

| Group | Units | IMPL | PART | STORE | MISS | N/A |
|---|---|---|---|---|---|---|
| §1 Detection, entry, initial state | 4 | 4 | — | — | — | — |
| §2 Descriptors `0000h`–`000Fh` | 15 | 10 | 3 | — | 1 | 1 |
| §3 DOS memory `0100h`–`0102h` | 3 | 3 | — | — | — | — |
| §4 Interrupts, exceptions, virtual IF | 9 | 8 | 1 | — | — | — |
| §5 Translation `0300h`–`0306h` | 7 | 4 | 3 | — | — | — |
| §6 Version and capabilities `0400h`/`0401h` | 2 | 1 | — | — | 1 | — |
| §7 Memory `0500h`–`050Bh` | 6 | 3 | 1 | — | 2 | — |
| §8 Paging, physical mapping, vendor, debug | 7 | 4 | 1 | — | 2 | — |
| §9 DPMI 1.0 extras, and NTVDM's private pair | 5 | 2 | — | — | 3 | — |
| §10 INT 21h from protected mode | 6 | 2 | 3 | — | — | 1 |
| **Total** | **64** | **41** | **12** | **—** | **9** | **2** |

---

## 1. Detection, entry and initial state

| Unit | Status | Where / what is missing | Verification |
|---|---|---|---|
| INT 2Fh `1687h`: AX=0, BX bit 0 (32-bit OK), CL, DX=0.90, SI=0 private paragraphs, ES:DI entry | **IMPL** | `main.c:29443-29457` | by hand (every DPMI guest) |
| The mode switch (`BOP 50h`, far-called; AX bit 0 = 32-bit client), failure returns CF=1 in real mode | **IMPL** | `main.c:29573-29612`, `dpmi.c:38`; the initial CS/DS/SS are 16-bit whatever the client's width (the Doom fix) | by hand |
| Initial state: ES = PSP selector (100h limit), PSP:2Ch converted to a selector | **IMPL** | `main.c:29613+` | by hand (DOS/4GW) |
| CPU class reported consistently | **IMPL** (#248) | `1687h` (V86 and PM) and `0400h` all report `DPMI_CPU_CLASS` = 4 (`dpmi_svc.h`) | `p_dpmi31` `int31.0400.cl_eq_1687` |

## 2. Descriptor management

| AX | Function | Status | Where / what is missing |
|---|---|---|---|
| `0000h` | allocate CX descriptors (contiguous) | **IMPL** | `:22577-22607`; a single descriptor is recycled from the free list |
| `0001h` | free descriptor | **IMPL** (#248) | null, GDT, off-table, never-allocated and already-freed selectors are `8022h`; our own (initial CS/DS/SS, PSP, environment, host stubs) still answer success and are never recycled. ⚠ Under WOW only the range is checked — krnl386 owns the table (`dpmi_svc.h`) |
| `0002h` | segment to descriptor | **IMPL** | `:23237-23257`; from a host-private pool, and says so when it spills |
| `0003h` | selector increment | **IMPL** | `:23258-23264`, 8 |
| `0004h`/`0005h` | lock / unlock selector | **N/A** | reserved in 0.9; `default:` CF=1 |
| `0006h` | get segment base | **IMPL** | `:23155-23160` |
| `0007h` | set segment base | **IMPL** (#248) | invalid selector → `8022h` (same rule and WOW exception as `0001h`). `8025h` (base outside the client's space) not checked |
| `0008h` | set segment limit | **PART** | host chooses G (the ZAR fix); invalid selector → `8022h` (#248). ⚠ the spec's `8021h` for a limit > 1 MB whose low 12 bits are not all set is not checked |
| `0009h` | set access rights | **PART** | marking a region CODE patches its INT sites; invalid selector → `8022h` (#248). ⚠ the access byte itself is not validated (`8021h`) — DPL is forced to 3 at install instead (ZAR) |
| `000Ah` | create alias | **IMPL** (#248) | invalid source → `8022h` (was: an alias of the code base); no descriptor → `8011h` |
| `000Bh` | get descriptor | **IMPL** | `:23272-23289` |
| `000Ch` | set descriptor | **PART** | `:23290-23318`; rejects indices ≥ **512** while the table holds `DPMI_LDT_MAX` = 2048 (`:2377`) — a selector above `0FFFh` cannot be set this way |
| `000Dh` | allocate specific descriptor (1.0) | **IMPL** | `:22848-22874` |
| `000Eh`/`000Fh` | get / set multiple descriptors (1.0) | **MISS** | `default:` |
| — | NTVDM's vendor descriptor window (krnl386) | **IMPL** | see §9 |

## 3. DOS memory

| AX | Function | Status | Where / what is missing |
|---|---|---|---|
| `0100h` | allocate DOS memory → segment + selector | **IMPL** | `:22875-22928`; on failure names every MCB's owner in the log |
| `0101h` | free DOS memory | **IMPL** (#248) | the selector must name a live `0100h` block (`8022h` otherwise — the PSP selector used to free the program's own block); a DOS refusal returns DOS's code; the descriptor goes back on the free list and `0100h` takes from it (the leak ran the LDT dry after ~2000 calls, then the failure path's MCB dump overran a 2 KB stack buffer and killed the host — `runs/s87_dpmi/p31_base_host.log`) |
| `0102h` | resize DOS memory | **IMPL** | `:22941-22962` |

## 4. Interrupts, exceptions, virtual interrupt flag

| AX / unit | Function | Status | Where / what is missing |
|---|---|---|---|
| `0200h`/`0201h` | get / set real-mode vector (the IVT itself) | **IMPL** | `:22987-23009` |
| `0202h`/`0203h` | get / set exception handler (00h–1Fh) | **IMPL** | `:23078-23111`; `0202h` with nothing set returns a register-preserving `RETF`, not 0:0 |
| `0204h`/`0205h` | get / set PM interrupt vector | **IMPL** | `:23010-23060`; offset width follows the client (the Doom timer fix) |
| `0900h`–`0902h` | virtual interrupt state | **IMPL** | `:23143-23154` |
| Exception dispatch to the client's handler, and its `RETF` return | **IMPL** | `:31191-31263`, `:31412+` |
| Default action for an exception with no client handler | **PART** | `:31191-31194`: the run **stops** ("no client handler"). The spec's default reflects 00h–05h and 07h to real mode and terminates the client for the rest |
| Hardware IRQs to a PM handler | **IMPL** | `dpmi_inject_pm_irq` `:24864` |
| A PM default IRQ handler reflects to real mode | **IMPL** | `:22303-22334` (ours), `dpmi_reflect_irq_to_rm` `:19995` (the guest's) |
| Software INTs from PM code without a PM handler reflect to real mode | **IMPL** | per-vector arms (10h, 16h, 33h, 1Ah, 11h, 15h, 2Fh, 21h …, `:22224-24546`) — each vector is its own arm, not one generic reflector |

## 5. Translation services

| AX | Function | Status | Where / what is missing |
|---|---|---|---|
| `0300h` | simulate real-mode interrupt | **IMPL** (#247) | Routing `simint_route()` (`src/host/dpmi_rmcs.h`), decided above the INT 31h switch: **every vector runs from the real-mode IVT** through the `0302h` arm — the guest's handler or our own stub, whose BOP the nested loop now services through `v86_bios_bop()`, the exec loop's own code. Fast path in `case 0x0300`: INT 21h always, 33h/10h while the IVT holds our stub, host-side with no stack and CF/ZF returned through FLAGS (was: written into a frame at `0100:FF04` inside the guest, RMCS got CF=0). All 32-bit registers, FLAGS, `ES DS FS GS` read and written; CS:IP/SS:SP never written; RMCS SS:SP honoured (zero → host default `code_base:FF00`); `CX` words copied. ⚠ Deviations kept on purpose: a guest-hooked real-mode INT 21h is still answered host-side; the handler is entered with IF **set** (ZAR's s81 proof). A null vector is not run. Rollback lever `cfg\simintrefl_off.flag` = pre-#247 routing. Off-VM: `tests/unit/rmcs_test.c` |
| `0301h` | call real-mode far procedure | **PART** | runs it in V86 for real; `CX` words copied (#247; a CX that does not fit is logged and NOT copied rather than refused); full 32-bit + FS/GS marshalling (#247); a BIOS call from the procedure is serviced (#247). ⛔ after 128 nested events without a return it gives up and still returns **CF=0** |
| `0302h` | call real-mode procedure with IRET frame | **PART** | same arm, FLAGS pushed; same remaining gap |
| `0303h` | allocate real-mode callback | **PART** | 16 slots (#248; was 4), `8015h` when full. ⚠ The callback's ENTRY contract is not the spec's: the host pre-pops the far return into the RMCS and hands the handler `DS:SI = 0017h:0` instead of the real-mode SS:SP — a spec-conforming handler pops twice. Never exercised: no shelf guest has invoked a callback in any recorded run |
| `0304h` | free real-mode callback | **IMPL** (#248) | `CX:DX` must be exactly a live callback address, else `8024h` |
| `0305h` | state save/restore addresses | **IMPL** | `:23319-23337`; both routines are register-preserving no-ops, buffer size 40h — nothing of ours needs saving across a raw switch |
| `0306h` | raw mode-switch addresses | **IMPL** | `:23338-23353` |

## 6. Version and capabilities

| AX | Function | Status | Where / what is missing |
|---|---|---|---|
| `0400h` | version: 0.90, flags, CPU, PIC bases | **IMPL** (#248) | AX `005Ah`, BX `0001h`, CL = `DPMI_CPU_CLASS` (4, = `1687h`), DX `0870h` (master 08h, slave 70h). The dormant `pmkernel.flag` spike path now gives the same answer (it said CL=3 and swapped the PIC bases) |
| `0401h` | capabilities (1.0) | **MISS** | `default:` |

## 7. Memory

| AX | Function | Status | Where / what is missing |
|---|---|---|---|
| `0500h` | free memory information | **PART** | `:23354-23396`: a fixed 64 MB / 4000h pages in every field, **never reduced by allocations** — a client that sizes itself from free pages after allocating is told nothing changed |
| `0501h` | allocate memory block (linear = host address; handle = address) | **IMPL** | the handle record (`g_dpmi_owned`, 4096) refuses with `8016h` when full rather than overflow silently (#248) |
| `0502h` | free memory block | **IMPL** | forgets the block's patch sites too; a handle not in the record is `8023h` (#248 — it used to `VirtualFree` whatever address it was given, host memory included) |
| `0503h` | resize memory block | **IMPL** (#248) | in place while the size fits the committed pages; otherwise a new block, the contents copied, patch-map entries re-keyed, the old freed; new address and handle returned. `8021h` size 0, `8023h` unknown handle |
| `0504h`–`0507h` | linear memory, page attributes (1.0) | **MISS** | `default:` |
| `0508h`–`050Bh` | map device / conventional memory, block size, memory info (1.0) | **MISS** | `default:` |

## 8. Paging, physical mapping, vendor, debug

| AX | Function | Status | Where / what is missing |
|---|---|---|---|
| `0600h`–`0603h` | lock / unlock linear and real-mode regions | **IMPL** | `:23122-23136`; nothing is ever paged out, so locked is already true |
| `0604h` | page size | **IMPL** | `:23137-23142`, 4096 |
| `0701h`–`0703h` | demand-paging hints | **IMPL** | `:23126-23136`; advisory, CF=0 |
| `0800h` | map physical address | **PART** | `:23414-23432`: **only the VESA linear frame buffer** (`VID_VESA_LFB_PHYS`); any other physical address is refused (deliberately — not a hole into host memory) |
| `0801h` | free physical mapping | **IMPL** | `:23433-23439` |
| `0A00h` | vendor API entry | **MISS** | `:23265-23271`: refused, CF=1 — correct for a host with no vendor API, counted here as MISS because stock NTVDM has one (krnl386 asks for `"MS-DOS"`) |
| `0B00h`–`0B03h` | debug watchpoints | **MISS** | `default:` |

## 9. DPMI 1.0 extras, and NTVDM's private pair

| AX | Function | Status | Where / what is missing |
|---|---|---|---|
| `0C00h`/`0C01h` | TSR services | **MISS** | `default:` |
| `0D00h`–`0D03h` | shared memory | **MISS** | `default:` |
| `0E00h`/`0E01h` | coprocessor status / emulation | **MISS** | `default:` |
| `04F1h` | NTVDM-private: allocate descriptors | **IMPL** | `:22639-22668` (krnl386) |
| `04F2h` | NTVDM-private: commit descriptors written through the window | **IMPL** | `:22669-22847`; patches INT sites in a region committed as code |

## 10. INT 21h from protected mode

DPMI 0.9 itself only says INT 21h is reflected to real mode. We go further for the
calls the shelf makes, translating pointers through the client's selectors.

| Unit | Status | Where / what is missing |
|---|---|---|
| `4Ch` terminate | **IMPL** | `:23828-23840` |
| Direct-translated file and memory calls (`3Ch`/`3Dh`/`5Bh`, `3Eh`, `3Fh`, `40h`, `42h`, `48h`, `49h`, `4Ah`, `25h`/`35h`, `44h`, `26h`/`55h`) | **PART** | `:23870-24480`; ⚠ `40h` to handle 1 or 2 always goes to the console (`:23877`), even after a redirect — the defect the V86 path fixed in #133 |
| Calls through a real-mode transfer buffer (`3Dh 3Fh 40h 41h 43h 4Eh 39h 3Ah 3Bh`) | **PART** | `:24328-24350`, only when `g_pm_xfer_seg` exists |
| Register-only calls passed to the V86 handler | **PART** | `:24385-24434`, a fixed list of 30 AH values; anything else is §10's last row |
| `AX=FF80h` (DOS/16M lock) | **IMPL** | `:24505-24512` |
| Any other AH | **N/A** | `:24513-24546`: logged in full and refused with CF=1 — the honest answer for a call we do not translate |

---

## What to fix, in order

1. ~~`0300h`: service every vector the way a host does~~ — done, #247.
2. ~~`0503h`, then `0304h` with at least 16 callback slots.~~ — done, #248.
3. ~~Make `0400h`'s CL agree with `1687h`.~~ — done, #248.
4. ~~`0001h`/`0007h`–`000Ah`/`0101h`: report `8022h` for a bad selector; return freed
   selectors to the free list.~~ — done, #248. Remaining: `0006h` still answers an
   unallocated selector; `0008h`/`0009h` value validation (`8021h`).
4a. The `0303h` callback entry contract (DS:SI = real-mode SS:SP, the handler pops).
5. `0301h`/`0302h`: return CF=1 when the procedure never returns (the `CX` copy landed with #247).
6. `0500h` from the real pool; the default exception action.
7. PM INT 21h `40h`: honour redirection, as the V86 path does.
