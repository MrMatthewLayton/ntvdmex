# Inventory — DPMI host (INT 2Fh `1687h`, the mode switch, INT 31h)

**Spec:** *DOS Protected Mode Interface Specification* 0.9 and 1.0 (DPMI Committee).
⚠ **Not held in the repo** — [`../ref/SOURCES.md`](../ref/SOURCES.md). We **claim 0.90**
(`0400h` and `1687h` both say so), so the 1.0-only functions below are not promised to a
client; they are listed because the README names DPMI 1.0 as the target.
**Our implementation:** host code, not a VDD.
- Detection and entry: INT 2Fh `1687h` (`src/host/main.c:29275-29289`); the far-called
  entry is `BOP 50h` (`DPMI_BOP`, `main.c:550`), serviced at `main.c:29405-29420` by
  `dpmi_switch_to_pm` (`src/vdm/dpmi.c:38`); initial client state (ES = PSP selector, the
  environment converted) `main.c:29445+`.
- INT 31h: one `switch (ax)` in the PM service routine, `main.c:22463-23716`; anything
  without a `case` falls to `default:` — CF=1, logged `UNSUP` (`main.c:23710-23713`).
- Interrupts and exceptions: PM IRQ injection `dpmi_inject_pm_irq` (`main.c:24758`),
  reflection to real mode (`main.c:22197-22228`, `dpmi_reflect_irq_to_rm` `:19891`), client
  exception dispatch (`main.c:31022-31094`) and its return (`:31243+`), real-mode callbacks
  `dpmi_invoke_callback` (`:19020`).
- INT 21h from protected mode: a translating subset, `main.c:23720-24440` (§9).

**Probes:** none in `tools/dostest/` asks INT 31h function by function; `dpmitest.asm`,
`dpmiexe.asm`, `pm32*.asm` exercise paths. The verification is the guest shelf (Doom,
Heretic, Hexen, Duke3D, ZAR, heaven7, Win16's krnl386) — **untested** in the README's sense.
**Marked:** 2026-10-01, **from the code**.

---

## Headline

**Everything DOS/4GW, DOS/16M and krnl386 have been seen to call is implemented, and the
holes are exactly where no shelf guest has walked.** Four of them are in DPMI **0.9** core,
not 1.0:

1. **`0300h` (simulate real-mode interrupt) services three vectors** — INT 21h, 33h and
   10h (`:23466-23499`). For every other vector it loads the register block, does nothing,
   copies it back and returns **CF=0** — the caller's own registers as a "successful" answer
   (`:23500-23511`). A Watcom/DJGPP `int86()` for INT 16h (keyboard), 1Ah (time), 15h,
   2Fh, 13h or 67h gets nothing. The unhandled vectors are counted (`g_simint_vec`), not
   refused. And `0300h` writes back `AX BX CX DX SI DI FLAGS` but **not `ES`, `DS` or `BP`**
   (`:23503-23506`), so an INT 21h that answers in `ES:BX` (`35h`, `2Fh`, `34h`, `52h`)
   comes back with the caller's `ES`.
2. **`0503h` (resize memory block) is UNSUP.** A client that grows its heap with `0503h`
   rather than allocate-copy-free is refused.
3. **`0304h` (free real-mode callback) is UNSUP, and there are only 4 callback slots**
   (`DPMI_CB_SLOTS`, `main.c:600`). The spec's minimum is 16; a client that allocates and
   frees callbacks runs out after four.
4. **`0B00h`–`0B03h` (debug watchpoints) are UNSUP.**

And one inconsistency: **`1687h` reports CL = 4 (486, `DPMI_CPU_CLASS`, `main.c:542`) while
`0400h` reports CL = 3 (386, `main.c:22467`)** — one machine described two ways to the same
client. #79's GetWinFlags fix changed the first and not the second.

| Group | Units | IMPL | PART | STORE | MISS | N/A |
|---|---|---|---|---|---|---|
| §1 Detection, entry, initial state | 4 | 3 | 1 | — | — | — |
| §2 Descriptors `0000h`–`000Fh` | 15 | 7 | 6 | — | 1 | 1 |
| §3 DOS memory `0100h`–`0102h` | 3 | 2 | 1 | — | — | — |
| §4 Interrupts, exceptions, virtual IF | 9 | 8 | 1 | — | — | — |
| §5 Translation `0300h`–`0306h` | 7 | 2 | 4 | — | 1 | — |
| §6 Version and capabilities `0400h`/`0401h` | 2 | — | 1 | — | 1 | — |
| §7 Memory `0500h`–`050Bh` | 6 | 2 | 1 | — | 3 | — |
| §8 Paging, physical mapping, vendor, debug | 7 | 4 | 1 | — | 2 | — |
| §9 DPMI 1.0 extras, and NTVDM's private pair | 5 | 2 | — | — | 3 | — |
| §10 INT 21h from protected mode | 6 | 2 | 3 | — | — | 1 |
| **Total** | **64** | **32** | **19** | **—** | **11** | **2** |

---

## 1. Detection, entry and initial state

| Unit | Status | Where / what is missing | Verification |
|---|---|---|---|
| INT 2Fh `1687h`: AX=0, BX bit 0 (32-bit OK), CL, DX=0.90, SI=0 private paragraphs, ES:DI entry | **IMPL** | `main.c:29275-29289` | by hand (every DPMI guest) |
| The mode switch (`BOP 50h`, far-called; AX bit 0 = 32-bit client), failure returns CF=1 in real mode | **IMPL** | `main.c:29405-29444`, `dpmi.c:38`; the initial CS/DS/SS are 16-bit whatever the client's width (the Doom fix) | by hand |
| Initial state: ES = PSP selector (100h limit), PSP:2Ch converted to a selector | **IMPL** | `main.c:29445+` | by hand (DOS/4GW) |
| CPU class reported consistently | **PART** | `1687h` CL = `DPMI_CPU_CLASS` = 4 (`main.c:542`, `:29282`); `0400h` CL = 3 (`:22467`) | — |

## 2. Descriptor management

| AX | Function | Status | Where / what is missing |
|---|---|---|---|
| `0000h` | allocate CX descriptors (contiguous) | **IMPL** | `:22471-22501`; a single descriptor is recycled from the free list |
| `0001h` | free descriptor | **PART** | `:22502-22532`; refuses our own and unallocated selectors, but **reports the refusal as success** (CF untouched, no `8022h`) |
| `0002h` | segment to descriptor | **IMPL** | `:23131-23151`; from a host-private pool, and says so when it spills |
| `0003h` | selector increment | **IMPL** | `:23152-23158`, 8 |
| `0004h`/`0005h` | lock / unlock selector | **N/A** | reserved in 0.9; `default:` CF=1 |
| `0006h` | get segment base | **IMPL** | `:23049-23054` |
| `0007h` | set segment base | **PART** | `:23059-23065`; an invalid selector is **silently ignored** — no CF, no `8022h` |
| `0008h` | set segment limit | **PART** | `:23066-23080`; host chooses G (the ZAR fix); invalid selector silently ignored |
| `0009h` | set access rights | **PART** | `:23081-23109`; marking a region CODE patches its INT sites; invalid selector silently ignored |
| `000Ah` | create alias | **PART** | `:23110-23120`; an invalid source selector aliases the code base instead of failing |
| `000Bh` | get descriptor | **IMPL** | `:23166-23183` |
| `000Ch` | set descriptor | **PART** | `:23184-23212`; rejects indices ≥ **512** while the table holds `DPMI_LDT_MAX` = 2048 (`:2377`) — a selector above `0FFFh` cannot be set this way |
| `000Dh` | allocate specific descriptor (1.0) | **IMPL** | `:22742-22768` |
| `000Eh`/`000Fh` | get / set multiple descriptors (1.0) | **MISS** | `default:` |
| — | NTVDM's vendor descriptor window (krnl386) | **IMPL** | see §9 |

## 3. DOS memory

| AX | Function | Status | Where / what is missing |
|---|---|---|---|
| `0100h` | allocate DOS memory → segment + selector | **IMPL** | `:22769-22822`; on failure names every MCB's owner in the log |
| `0101h` | free DOS memory | **PART** | `:22823-22834`: an invalid selector is not reported (no CF); the selector is zeroed but **not returned to the free list**, so it leaks |
| `0102h` | resize DOS memory | **IMPL** | `:22835-22856` |

## 4. Interrupts, exceptions, virtual interrupt flag

| AX / unit | Function | Status | Where / what is missing |
|---|---|---|---|
| `0200h`/`0201h` | get / set real-mode vector (the IVT itself) | **IMPL** | `:22881-22903` |
| `0202h`/`0203h` | get / set exception handler (00h–1Fh) | **IMPL** | `:22972-23005`; `0202h` with nothing set returns a register-preserving `RETF`, not 0:0 |
| `0204h`/`0205h` | get / set PM interrupt vector | **IMPL** | `:22904-22954`; offset width follows the client (the Doom timer fix) |
| `0900h`–`0902h` | virtual interrupt state | **IMPL** | `:23037-23048` |
| Exception dispatch to the client's handler, and its `RETF` return | **IMPL** | `:31022-31094`, `:31243+` |
| Default action for an exception with no client handler | **PART** | `:31022-31025`: the run **stops** ("no client handler"). The spec's default reflects 00h–05h and 07h to real mode and terminates the client for the rest |
| Hardware IRQs to a PM handler | **IMPL** | `dpmi_inject_pm_irq` `:24758` |
| A PM default IRQ handler reflects to real mode | **IMPL** | `:22197-22228` (ours), `dpmi_reflect_irq_to_rm` `:19891` (the guest's) |
| Software INTs from PM code without a PM handler reflect to real mode | **IMPL** | per-vector arms (10h, 16h, 33h, 1Ah, 11h, 15h, 2Fh, 21h …, `:22118-24440`) — each vector is its own arm, not one generic reflector |

## 5. Translation services

| AX | Function | Status | Where / what is missing |
|---|---|---|---|
| `0300h` | simulate real-mode interrupt | **PART** | `:23403-23514`: ⛔ only INT 21h, 33h, 10h are serviced; every other vector returns its own registers with **CF=0**. `ES`/`DS`/`BP` are not written back. The RMCS's SS:SP is ignored (a host scratch stack is used, `:23420`). Only the low 16 bits of each RMCS register are read and written. (`cfg` opt-in `g_simint_reflect` sends some vectors to the guest's own real-mode handler, `:22447-22462`; default off) |
| `0301h` | call real-mode far procedure | **PART** | `:23515-23690`: runs it in V86 for real; **`CX` (words of stack to copy) is ignored**; after 128 nested events without a return it gives up and still returns **CF=0** (`:23686-23689`) |
| `0302h` | call real-mode procedure with IRET frame | **PART** | same arm, FLAGS pushed; same two gaps |
| `0303h` | allocate real-mode callback | **PART** | `:23691-23709`; **4 slots** (`DPMI_CB_SLOTS`, `:600`) where the spec requires at least 16 |
| `0304h` | free real-mode callback | **MISS** | `default:` — a callback, once allocated, is never released |
| `0305h` | state save/restore addresses | **IMPL** | `:23213-23231`; both routines are register-preserving no-ops, buffer size 40h — nothing of ours needs saving across a raw switch |
| `0306h` | raw mode-switch addresses | **IMPL** | `:23232-23247` |

## 6. Version and capabilities

| AX | Function | Status | Where / what is missing |
|---|---|---|---|
| `0400h` | version: 0.90, flags, CPU, PIC bases | **PART** | `:22464-22470`; AX `005Ah`, BX `0001h`, **CL = 3** (see §1), DX `0870h` (master 08h, slave 70h) |
| `0401h` | capabilities (1.0) | **MISS** | `default:` |

## 7. Memory

| AX | Function | Status | Where / what is missing |
|---|---|---|---|
| `0500h` | free memory information | **PART** | `:23248-23290`: a fixed 64 MB / 4000h pages in every field, **never reduced by allocations** — a client that sizes itself from free pages after allocating is told nothing changed |
| `0501h` | allocate memory block (linear = host address; handle = address) | **IMPL** | `:23334-23373` |
| `0502h` | free memory block | **IMPL** | `:23374-23402`; forgets the block's patch sites too |
| `0503h` | resize memory block | **MISS** | `default:` — DPMI 0.9 core |
| `0504h`–`0507h` | linear memory, page attributes (1.0) | **MISS** | `default:` |
| `0508h`–`050Bh` | map device / conventional memory, block size, memory info (1.0) | **MISS** | `default:` |

## 8. Paging, physical mapping, vendor, debug

| AX | Function | Status | Where / what is missing |
|---|---|---|---|
| `0600h`–`0603h` | lock / unlock linear and real-mode regions | **IMPL** | `:23016-23030`; nothing is ever paged out, so locked is already true |
| `0604h` | page size | **IMPL** | `:23031-23036`, 4096 |
| `0701h`–`0703h` | demand-paging hints | **IMPL** | `:23020-23030`; advisory, CF=0 |
| `0800h` | map physical address | **PART** | `:23308-23326`: **only the VESA linear frame buffer** (`VID_VESA_LFB_PHYS`); any other physical address is refused (deliberately — not a hole into host memory) |
| `0801h` | free physical mapping | **IMPL** | `:23327-23333` |
| `0A00h` | vendor API entry | **MISS** | `:23159-23165`: refused, CF=1 — correct for a host with no vendor API, counted here as MISS because stock NTVDM has one (krnl386 asks for `"MS-DOS"`) |
| `0B00h`–`0B03h` | debug watchpoints | **MISS** | `default:` |

## 9. DPMI 1.0 extras, and NTVDM's private pair

| AX | Function | Status | Where / what is missing |
|---|---|---|---|
| `0C00h`/`0C01h` | TSR services | **MISS** | `default:` |
| `0D00h`–`0D03h` | shared memory | **MISS** | `default:` |
| `0E00h`/`0E01h` | coprocessor status / emulation | **MISS** | `default:` |
| `04F1h` | NTVDM-private: allocate descriptors | **IMPL** | `:22533-22562` (krnl386) |
| `04F2h` | NTVDM-private: commit descriptors written through the window | **IMPL** | `:22563-22741`; patches INT sites in a region committed as code |

## 10. INT 21h from protected mode

DPMI 0.9 itself only says INT 21h is reflected to real mode. We go further for the
calls the shelf makes, translating pointers through the client's selectors.

| Unit | Status | Where / what is missing |
|---|---|---|
| `4Ch` terminate | **IMPL** | `:23722-23734` |
| Direct-translated file and memory calls (`3Ch`/`3Dh`/`5Bh`, `3Eh`, `3Fh`, `40h`, `42h`, `48h`, `49h`, `4Ah`, `25h`/`35h`, `44h`, `26h`/`55h`) | **PART** | `:23764-24374`; ⚠ `40h` to handle 1 or 2 always goes to the console (`:23771`), even after a redirect — the defect the V86 path fixed in #133 |
| Calls through a real-mode transfer buffer (`3Dh 3Fh 40h 41h 43h 4Eh 39h 3Ah 3Bh`) | **PART** | `:24222-24244`, only when `g_pm_xfer_seg` exists |
| Register-only calls passed to the V86 handler | **PART** | `:24279-24328`, a fixed list of 30 AH values; anything else is §10's last row |
| `AX=FF80h` (DOS/16M lock) | **IMPL** | `:24399-24406` |
| Any other AH | **N/A** | `:24407-24440`: logged in full and refused with CF=1 — the honest answer for a call we do not translate |

---

## What to fix, in order

1. `0300h`: service every vector the way a host does — reflect to the real-mode IVT
   entry (ours or the guest's) through the same nested V86 loop `0301h` already runs — and
   write back `ES`, `DS`, `BP`; honour the RMCS stack.
2. `0503h`, then `0304h` with at least 16 callback slots.
3. Make `0400h`'s CL agree with `1687h`.
4. `0001h`/`0007h`–`000Ah`/`0101h`: report `8022h` for a bad selector; return freed
   selectors to the free list.
5. `0301h`/`0302h`: copy `CX` words of stack; return CF=1 when the procedure never returns.
6. `0500h` from the real pool; the default exception action.
7. PM INT 21h `40h`: honour redirection, as the V86 path does.
