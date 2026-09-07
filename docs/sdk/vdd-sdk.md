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

## What is not here yet

The **`vddsvc.h` binary-compatibility veneer** of ADR-0008 — the layer that would let an
existing Microsoft-ABI VDD (VDMSound and its lineage) load unmodified. The ADR defers it
explicitly to "when the audience is reached". Beyond writing the veneer itself, note the
problem named above: those drivers import from the literal name `NTVDM.EXE`, so hosting
them needs an import-resolution answer as well as an ABI one.

There is also no packaging story yet: no versioned SDK drop, no import library, one sample.

## Files

| path | what |
|---|---|
| `sdk/include/ntvdmex-vdd.h` | the whole public ABI, self-contained |
| `sdk/sample/portecho.c` | a complete device in one file |
| `sdk/sample/abi_check.c` | fails the build if the SDK header drifts from `src/vdd/ntvdd.h` |
| `sdk/build-sample.sh` | one compiler line |
| `tools/dostest/vddtest.asm` | a DOS driver that detects and drives the sample |
