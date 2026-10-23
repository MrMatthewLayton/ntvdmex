# Writing a device for NTVDMEX

GH #11 · ADR-0008 · header: `sdk/include/ntvdmex-vdd.h` · sample: `sdk/sample/portecho.c`

A **VDD** is a DLL that claims some hardware — a range of I/O ports, a window of guest
memory, an interrupt vector, a per-frame tick — and is called back when the guest touches
it. That is the whole model. Your device sees the same bus every built-in device uses; the
video card, the UART and the Sound Blaster are written against this ABI too.

## The shortest possible driver

```c
#include <ntvdmex-vdd.h>

static uint8_t g_latch;

static void my_in (void *self, uint16_t port, uint8_t w, uint32_t *val)
{ (void)self; (void)port; (void)w; *val = g_latch; }
static void my_out(void *self, uint16_t port, uint8_t w, uint32_t  val)
{ (void)self; (void)port; (void)w; g_latch = (uint8_t)val; }

NTVDMEX_VDD_EXPORT int NtvdmexVddInit(const ntvdmex_vdd_api *api,
                                      ntvdmex_vdd_bus *bus)
{
    if (api->version != NTVDMEX_VDD_ABI_VERSION) return -1;
    return api->claim_ports(bus, 0x2E0, 0x2E7, my_in, my_out, 0);
}
```

Build it (see `sdk/build-sample.sh`):

```
i686-w64-mingw32-gcc -std=c99 -O2 -shared -I sdk/include \
    -o mydev.dll mydev.c -nostdlib -Wl,--entry,0 -lkernel32
```

Load it by putting the DLL's full path on its own line in **`vdd.txt`**, next to the host:

```
C:\Documents and Settings\All Users\Documents\ntvdmex\mydev.dll
```

The host logs every step to `ntvdmhost.log`, and you should read it the first time:

```
VDD: mydev: 0x2E0..0x2E7 claimed
VDD: [C:\...\mydev.dll] initialised
VDD: third-party list [C:\...\vdd.txt] -- 0x00000001 entr(ies), 0x00000001 initialised
```

## Why your driver imports nothing from the host

Microsoft's VDD ABI has drivers **import** `VDDInstallIOHook` and friends from `NTVDM.EXE`
by name, which binds a driver to the *filename* of the process hosting it. Ours does not:
the host hands you a table of function pointers at init. Three consequences, all
deliberate:

- your DLL loads under a host named anything (ours is `ntvdmhost.exe`, not `ntvdm.exe`);
- you can unit-test the whole driver off-VM with a stub table and no virtual machine;
- the ABI can grow without breaking a built driver — `size` and `version` are checked and
  new members are only ever **appended**.

`objdump -p portecho.dll` on the sample shows one export and **no import table at all**.

## Rules that are not expressible in the header

**Claim in `NtvdmexVddInit` and nowhere else.** Init is the only moment the host guarantees
the bus is quiet. A claim from inside a callback races the dispatcher that is calling you.

**Read every `claim_*` return value.** The host's tables are finite. A refused claim leaves
your device unreachable, and the guest then reads `0xFF` from what looks like an empty ISA
slot — indistinguishable from hardware that was never fitted. That exact failure cost this
project a whole investigation once, when the port table was full by one entry and the
MPU-401 fell off the bus silently. The host now logs refusals loudly; you should too.

**`width` is in BYTES — 1, 2 or 4 — not bits.** A guest may read a 16-bit port with a single
`in ax,dx`. A device that assumes 1 truncates silently.

**Answer `0xFF` for registers you do not implement**, not `0`. That is what an unclaimed ISA
address does, and a detection routine that probes for "anything at all" will otherwise
conclude your card is present and broken.

**Do not block.** Your callback runs on the exec thread, between two guest instructions.
Sleeping there stops the guest's timer, its music and its screen.

**The registers arrive as a struct you are given**, not as global get/set macros. This is the
one place ADR-0008 diverges from Microsoft's ABI on purpose: it is what keeps a device
testable with no VM. On an interrupt service, remember `cf` — DOS reports failure in the
carry flag, and a service that forgets it reports success.

## Microsoft-ABI VDDs (s91)

An existing VDD written for NT's own NTVDM loads and runs **unmodified** — the
binary-compatibility veneer ADR-0008 deferred. Measured: `tests/probes/dos/p_isv.com` with
`tests/probes/dos/isvtest/ISVTEST.DLL` (built to the DDK's declarations, linked against
`NTVDM.EXE` through an import library) gives **identical answers under stock NTVDM and
NTVDMEX, 10/10**.

How it works:

- **The import problem is solved by name.** A DDK VDD imports `getAX`, `VDDInstallIOHook`
  … from the literal module `NTVDM.EXE`. NTVDMEX loads its own `bin\wowshim\NTVDM.EXE`
  (`src/shim/wowshim.c`, also the WOW stand-in) before the VDD, so the loader resolves
  those imports to it; every export forwards to the host.
- **Loading is the DDK's third-party BOP** (`isvbop.inc`): `C4 C4 58 00` RegisterModule
  (DS:SI DLL, DS:DI init routine, DS:BX dispatch routine; CF + AX = 1/2/3 on failure),
  `C4 C4 58 01` UnRegisterModule, `C4 C4 58 02` DispatchCall (AX = handle).
- **Loading from the registry** (#11), as NTVDM does when a VDM starts: every DLL named in
  `HKLM\SYSTEM\CurrentControlSet\Control\VirtualDeviceDrivers`, value `VDD`
  (`REG_MULTI_SZ`), is loaded with `NTVDM.EXE` already in place. A VDD sets itself up in
  its `DllEntryPoint`, so loading is the whole protocol. Each load is logged
  (`VDDSVC: registry VDD [...]`); one that fails to load is skipped. `cfg\noregvdd.flag`
  turns the list off.
- **Provided** — the whole service API of the DDK's `vddsvc.h`:
  - every register accessor (`get/setAX`…`GS`, the 8-bit halves, `EAX`…, the flags
    `CF ZF SF OF PF AF IF DF`, `getMSW`), `VdmMapFlat` / `VdmUnmapFlat` / `VdmFlushCache`;
  - `VDDInstallIOHook` / `VDDDeInstallIOHook`, `VDDSimulateInterrupt` (=
    `call_ica_hw_interrupt`), `VDDTerminateVDM`;
  - SDK 2 (#11): `VDDInstallMemoryHook` / `VDDDeInstallMemoryHook`, `VDDAllocMem` /
    `VDDFreeMem` / `VDDIncludeMem` / `VDDExcludeMem`, `VDDRequestDMA` / `VDDQueryDMA` /
    `VDDSetDMA`, `VDDReserveIrqLine` / `VDDReleaseIrqLine`, `VDDInstallUserHook` /
    `VDDDeInstallUserHook`.
- ⚠ **I/O handlers are STDCALL.** `nt_vdd.h` declares `PFNVDD_INB` etc. with no
  convention, but NT and the DDK compile with `__stdcall` as the default, and stock
  NTVDM calls them that way (a cdecl handler made stock die at the first `IN`). The same
  goes for memory-hook handlers and user hooks.

Build one: `tests/probes/dos/isvtest/build.sh` (`ntvdm.def` → `libntvdm.a` with
`dlltool -k`, then one compiler line), or against the SDK: `sdk/lib/ntvdm.def` lists every
export with its STDCALL decoration and `sdk/include/ntvdmex-vddsvc.h` declares the SDK 2
calls; `sdk/sample/vddsvc.c` uses all of them (`sdk/build-sample.sh` builds it). Link the
import library BY PATH: the mingw-w64 toolchain ships a `libntvdm.a` of its own, which
`-lntvdm` can find first.

### What NTVDMEX does with each call, and where it differs from NTVDM

| call | here |
|---|---|
| `VDDInstallMemoryHook` | The range (inside the VDM's 1 MB + HMA; no overlap with another hook) is recorded and its pages marked hooked. ⚠ **Only accesses the HOST INTERPRETER makes reach the handler** (planar and mode-Y video, and other interpreted stretches). NTVDM traps a hooked range by leaving its pages not present and taking the fault; here marking a page of the V86 window inaccessible freezes the guest on real hardware (measured for A0000), so a guest running natively on the CPU reads and writes the page as plain memory. The handler gets the address and 0 = read / 1 = write; if it then makes the memory present (`VDDAllocMem`) the access lands there, otherwise a read floats high (FFh) and a write is dropped. |
| `VDDAllocMem` / `VDDFreeMem` | Commits the range's pages (inside the VDM's range); `VDDFreeMem` decommits only pages `VDDAllocMem` committed, and a hooked range is hooked again. |
| `VDDIncludeMem` / `VDDExcludeMem` | Checked and logged, and nothing else: they tell NTVDM's memory manager a range is or is not usable as upper memory, and this host provides no UMBs. |
| `VDDRequestDMA` | Moves bytes between the guest's DMA buffer and yours in the direction the channel's mode names (read transfer → your buffer is filled; write transfer → it is emptied), advancing address and count and raising terminal count exactly as a device on the 8237 model does. A NULL buffer or zero length answers the bytes left. Verify-mode, masked and cascade channels move nothing. |
| `VDDQueryDMA` / `VDDSetDMA` | The channel's current address, count, page, status (bit *line* = TC, bit *line*+4 = request), mode and mask. `VDDSetDMA` writes the fields its flags name (`ADDR 1`, `COUNT 2`, `PAGE 4`, `STATUS 8`). |
| `VDDReserveIrqLine` | A line the host's own devices do not raise (by default only 9 is free; FFFFh asks for any free line, in AT add-in order). Answers the line or FFFFh. The host's own lines cannot be released. |
| `VDDInstallUserHook` | Create and terminate are called with the program's PSP when a program is EXECed and when one ends (AH=4Ch, INT 20h, ...); block and resume around the background pause (#219: the window loses focus; never under Win16 or headless). The very first program is already running when a VDD loaded by RegisterModule installs its hook. |

## What is not here yet

- Memory hooks are not seen by native V86 execution (above).
- `claim_int` is delivered for the user vectors (60h–66h, 68h–6Fh, 78h–FEh, a generic stub
  each, #315); a claim on a DOS, BIOS or IRQ vector is logged and refused rather than stolen
  from the system.

## SDK versions

| version | what changed |
|---|---|
| 1 (s91) | Our ABI (`ntvdmex-vdd.h`, ABI version 1); Microsoft-ABI VDDs through the third-party BOP: register accessors, `VdmMapFlat`, I/O hooks, `VDDSimulateInterrupt`, `VDDTerminateVDM`. |
| 2 (#11) | `claim_mem` is delivered (to the interpreter's accesses, as memory hooks are). Microsoft ABI: memory hooks, `VDDAllocMem`/`FreeMem`/`IncludeMem`/`ExcludeMem`, DMA, IRQ lines, user hooks, and the registry's VDD list. `sdk/lib/ntvdm.def`, `sdk/include/ntvdmex-vddsvc.h`, `sdk/sample/vddsvc.c`. `NTVDMEX_VDD_SDK_VERSION` in the header. The ABI version stays 1. |

## Files

| path | what |
|---|---|
| `sdk/include/ntvdmex-vdd.h` | the whole public ABI, self-contained |
| `sdk/sample/portecho.c` | a complete device in one file |
| `sdk/sample/abi_check.c` | fails the build if the SDK header drifts from `src/vdd/ntvdd.h` |
| `sdk/build-sample.sh` | one compiler line per sample |
| `sdk/include/ntvdmex-vddsvc.h` | the Microsoft-ABI SDK 2 calls, declared without the DDK |
| `sdk/lib/ntvdm.def` | every `NTVDM.EXE` export, decorated, for an import library |
| `sdk/sample/vddsvc.c` | a Microsoft-ABI VDD using every SDK 2 call |
| `tests/probes/dos/vddtest.asm` | a DOS driver that detects and drives the sample |
| `tests/probes/dos/isvtest/` | a Microsoft-ABI VDD (`isvtest.c`, `ntvdm.def`, `build.sh`) |
| `tests/probes/dos/p_isv.asm` | its DOS half: RegisterModule / DispatchCall / port I/O / UnRegisterModule |
