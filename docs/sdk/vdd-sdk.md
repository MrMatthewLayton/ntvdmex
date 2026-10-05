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
- **Provided:** every register accessor (`get/setAX`…`GS`, the 8-bit halves, `EAX`…,
  the flags `CF ZF SF OF PF AF IF DF`, `getMSW`), `VdmMapFlat` / `VdmUnmapFlat` /
  `VdmFlushCache`, `VDDInstallIOHook` / `VDDDeInstallIOHook`, `VDDSimulateInterrupt`,
  `VDDTerminateVDM`.
- ⚠ **I/O handlers are STDCALL.** `nt_vdd.h` declares `PFNVDD_INB` etc. with no
  convention, but NT and the DDK compile with `__stdcall` as the default, and stock
  NTVDM calls them that way (a cdecl handler made stock die at the first `IN`).

Build one: `tests/probes/dos/isvtest/build.sh` (`ntvdm.def` → `libntvdm.a` with
`dlltool -k`, then one compiler line).

## What is not here yet

- From the Microsoft API: memory hooks (`VDDInstallMemoryHook`), DMA (`VDDRequestDMA`…),
  `VDDReserveIrqLine`, `VDDAllocMem`, user hooks (`VDDInstallUserHook`), and the
  registry-listed VDDs NTVDM loads at start-up (`VirtualDeviceDrivers`).
- In our own ABI: `claim_mem` is accepted but never dispatched, and `claim_int` only reaches
  a device for a vector the host has a BIOS stub for (#315).
- No versioned SDK drop.

## Files

| path | what |
|---|---|
| `sdk/include/ntvdmex-vdd.h` | the whole public ABI, self-contained |
| `sdk/sample/portecho.c` | a complete device in one file |
| `sdk/sample/abi_check.c` | fails the build if the SDK header drifts from `src/vdd/ntvdd.h` |
| `sdk/build-sample.sh` | one compiler line |
| `tests/probes/dos/vddtest.asm` | a DOS driver that detects and drives the sample |
| `tests/probes/dos/isvtest/` | a Microsoft-ABI VDD (`isvtest.c`, `ntvdm.def`, `build.sh`) |
| `tests/probes/dos/p_isv.asm` | its DOS half: RegisterModule / DispatchCall / port I/O / UnRegisterModule |
