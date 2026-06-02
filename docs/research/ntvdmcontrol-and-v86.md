# The V86 / `NtVdmControl` execution contract

This is the heart of the project. Everything else is comparatively well-trodden; **this** is
the part with no open-source precedent.

## V86 mode basics `[FACT]`
- Virtual-8086 is a sub-mode of 32-bit protected mode that runs real-mode (16-bit) code with
  real-mode semantics (segment×16+offset addressing, 1MB+64KB space) while the OS stays in
  control.
- Privileged/sensitive operations (most I/O port access, `CLI`/`STI` depending on IOPL/VME,
  far control transfers to privileged things) **trap to the kernel** (#GP), which then
  emulates/reflects them. This trap-and-reflect loop is how the OS virtualizes the machine.
- V86 exists **only** in 32-bit mode. It is gone in x86-64 long mode — which is precisely why
  64-bit Windows dropped V86-based NTVDM and why we are 32-bit-locked.

## The usermode→kernel bridge `[BELIEF]`
- `NTSTATUS NtVdmControl(VDMSERVICECLASS Service, PVOID ServiceData)` is the single syscall.
- Service classes (names approximate; to confirm): `VdmInitialize`, `VdmStartExecution`,
  `VdmFeatures`, `VdmQueueInterrupt`, `VdmDelayInterrupt`, `VdmSetInt21Handler`, etc.
- The kernel maintains V86 state in a per-thread `VDM_TIB`; on each trap back to usermode the
  host inspects/updates that state, services the cause (port I/O, software interrupt, etc.),
  and re-enters via `VdmStartExecution`.

## The expected world the host must build `[VERIFY]`
Before `VdmInitialize` succeeds, the host is believed to need:
1. The first ~1MB+64KB reserved/committed at **virtual address 0** (real-mode memory image),
   plus the HMA and an interrupt-vector table at 0.
2. A populated `VDM_TIB` (registers, segment bases, control/flags fields).
3. I/O-port and interrupt hook tables wired so reflected faults reach our handlers.

## Why ReactOS doesn't help here
ReactOS NTVDM uses the **Fast486** software emulator and does not enter V86 or call this
kernel path. So ReactOS validates the *DOS/VDD/WOW logic on top*, but **not** the
`NtVdmControl`/V86 contract. The only ground truth is the shipping XP binaries (disassembly)
plus our own spike. `[FACT]`

## Closest working analog: Linux `dosemu`
`dosemu`/`dosemu2` runs real DOS code in V86 via the Linux `vm86()` syscall (older path) and
later via KVM. The *shape* — usermode host + kernel V86 facility + trap-and-reflect loop — is
the same as ours, just a different OS. Useful for understanding the loop structure, signal/
fault handling discipline, and the real-mode memory image setup. `[FACT]`

## Recovered from XP SP3 disassembly (2026-06-02)
Source: `ntdll.dll` (build 5.1.2600.5512, uncompressed on the install CD at `I386\NTDLL.DLL`).
Binaries kept in the gitignored `reverse/` dir (MS-copyrighted — never committed).

- `[FACT]` **`NtVdmControl` = syscall `0x10C` (268)**, 2 dword args → confirms the signature
  `NTSTATUS NtVdmControl(VDMSERVICECLASS Service, PVOID ServiceData)`. Exported at ordinal 358
  (`Nt`) / 1167 (`Zw`), RVA `0xdf00`. Classic XP stub:
  `B8 0C 01 00 00 | BA 00 03 FE 7F | FF 12 | C2 08 00`
  (`mov eax,10Ch; mov edx,7FFE0300h; call [edx]; ret 8`). We can either import it from `ntdll`
  or emit this stub directly.
### `VDMSERVICECLASS` — recovered from `ntvdm.exe` (2026-06-02)
The `I386\*.EX_/*.DL_` files are **MSCF cabinets** (not SZDD); extract with `cabextract`. From the
decompressed `ntvdm.exe`, 14 `call dword [NtVdmControl]` sites; the `push` for arg1 (Service)
gives the class:
- `[FACT]` **`VdmInitialize = 3`** — site `0xf00e6c2`: `push eax; push 3` (ServiceData = result of
  a prior alloc call).
- `[FACT]` Service **9** (printer init) — site `0xf01b51a` fills a struct at `[eax+0x5c4..0x5ec]`
  before the call.
- `[FACT]` Immediates observed: **2, 4, 5, 6, 8, 9, 10, 11, 12, 13**.
- `[FACT]` **`VdmStartExecution = 0`** is *not* a direct IAT call — ntvdm caches the pointer
  (`mov esi,[NtVdmControl]` at `0xf0048ea`/`0xf041509`) and `call esi` in the execution loop. That
  it's the only low service absent from the immediate set is consistent with `0`.
- `[BELIEF]` Full enum (corroborated by the above usage; the values to code against):
  `0 VdmStartExecution · 1 VdmQueueInterrupt · 2 VdmDelayInterrupt · 3 VdmInitialize ·
  4 VdmFeatures · 5 VdmSetInt21Handler · 6 VdmQueryDir · 7 VdmPrinterDirectIoOpen ·
  8 VdmPrinterDirectIoClose · 9 VdmPrinterInitialize · 10 VdmSetLdtEntries ·
  11 VdmSetProcessLdtInfo · 12 VdmAdlibEmulation · 13 VdmPMCliControl · 14 VdmQueryVdmProcess`.
  Keystone pair for M1: **VdmInitialize=3** (confirmed) and **VdmStartExecution=0** (strongly
  inferred — verify against `ntoskrnl`'s dispatch when convenient).

### How `ntvdm` learns its DOS program — answers Spike-002's no-argv finding
`[FACT]` `ntvdm.exe` imports from **kernel32**: `GetNextVDMCommand`, `ExitVDM`,
`SetVDMCurrentDirectories`, `RegisterConsoleVDM`, `VDMConsoleOperation`, `WriteConsoleInputVDMW`.
**`GetNextVDMCommand`** is the call that pulls the next DOS command (program path + args + env +
PSP info) from the **CSRSS/BaseSrv VDM queue** — confirming Spike-002's inference that the target
program arrives via the CSRSS channel, not the command line. Our host's startup sequence is
therefore: register with CSRSS (the support-process handshake) → `NtVdmControl(VdmInitialize)` →
`GetNextVDMCommand` to get the program → load it into low memory → `NtVdmControl(VdmStartExecution)`.

### Still to recover
The `VDM_TIB` layout and where its pointer lives (TEB offset); the exact `ServiceData` structs for
`VdmInitialize`/`VdmStartExecution`; the low-memory reservation `VdmInitialize` expects; and the
CSRSS support-process registration handshake `GetNextVDMCommand` depends on.

## Action
Remaining `[VERIFY]` items above are the explicit objectives of
[Spike-001](../spikes/spike-001-v86-keystone.md). Extracted MS binaries live in the gitignored
`reverse/` dir (`cabextract` from the install CD's `I386\`).
