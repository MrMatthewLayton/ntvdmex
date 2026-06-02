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

`[FACT]` `BOOL WINAPI GetNextVDMCommand(PVDM_COMMAND_INFO)` — `ntvdm` builds a ~0xA0-byte
`VDM_COMMAND_INFO` on the stack and passes `&struct` (clean 1-arg call at `0xf04ed86`). Struct
(ReactOS `subsys/win/vdm.h`, matches): `TaskId, CreationFlags, ExitCode, CodePage, Std{In,Out,Err},
CmdLine, AppName, PifFile, CurDirectory, Env, EnvLen, STARTUPINFOA, Desktop+Len, Title+Len,
Reserved+Len, {Cmd,App,Pif,CurDirectory}Len, VDMState, CurrentDrive, ComingFromBat`. Caller supplies
the string buffers + their `*Len` sizes; flags go in `VDMState` (`VDM_GET_FIRST_COMMAND=0x100`,
`VDM_GET_ENVIRONMENT=0x400`). Exercised by `tools/vdmhost/` (Spike-001 step 1).

`[FACT]` **Test result (2026-06-02, automated via telnet):** with our ReactOS-derived struct,
`GetNextVDMCommand` returns **FALSE / 0x57 (ERROR_INVALID_PARAMETER)** — *both* standalone *and*
when XP launches `vdmhost` as the real VDM host (after the `cmdline` repoint + reboot). So it isn't
a "not really the VDM host" problem. The struct **size matches** XP exactly (`ntvdm` uses a 160-byte
/ `0xa0` `VDM_COMMAND_INFO` at `ebp-0xa0`, same as ours), so the layout is close; the failure is a
wrong field *value/flag*. `ntvdm` populates the struct inside a helper (`0xf04d3af`) and only
conditionally sets `[struct+8]=8` — the exact first-call flag setup (its `VDM_GET_FIRST_COMMAND`
path, likely `0xf00e126`) is the next thing to replicate. **Next:** trace that helper, fix the
struct setup in `vdmhost`, push via TFTP (`tftp -i 10.0.2.2 GET ...`), re-test over telnet.

### `VdmInitialize` ServiceData — partial (2026-06-02)
`[FACT]` The `VdmInitialize` function (`ntvdm.exe` `0xf00e668`) builds a small `ServiceData`
struct on the stack and passes `&struct`; the struct's second field points to a **table of 9
pointers** into ntvdm's own code/data (`0xf09b6e0, 0xf079f20, 0xf079f54, 0xf0639b8, 0xf06b870,
0xf06b874, 0xf06b878, 0xf06b87c, 0xf0639bc`) — ntvdm's interrupt/I-O/fault handler & table
addresses. This is the `VDM_INITIALIZE_DATA` shape: the host hands the kernel pointers to the
tables it will service traps from. (Field-by-field semantics not yet decoded.)

`[FACT]` TEB access in ntvdm is overwhelmingly `fs:[0x18]` (TEB self-ptr, 310×); the `VDM_TIB`
pointer is reached as `[TEB + VdmOffset]` after loading the self-ptr — the exact `TEB.Vdm` offset
not yet pinned.

### Cross-validated against ReactOS (2026-06-02)
ReactOS's `sdk/include/ndk/ketypes.h` defines these, and they **match our XP SP3 disassembly
exactly** — two independent sources agreeing:

- `[FACT]` **`VDMSERVICECLASS`** (ReactOS == our recovered values):
  `0 VdmStartExecution · 1 VdmQueueInterrupt · 2 VdmDelayInterrupt · 3 VdmInitialize ·
  4 VdmFeatures · 5 VdmSetInt21Handler · 6 VdmQueryDir · 7 VdmPrinterDirectIoOpen ·
  8 VdmPrinterDirectIoClose · 9 VdmPrinterInitialize · 10 VdmSetLdtEntries ·
  11 VdmSetProcessLdtInfo · 12 VdmAdlibEmulation · 13 VdmPMCliControl · 14 VdmQueryVdmProcess`.
- `[FACT]` **`VDM_INITIALIZE_DATA`** = `{ PVOID TrapcHandler; PVDMICAUSERDATA IcaUserData; }`.
  Confirmed by the `ntvdm` disasm: `ServiceData = &{TrapcHandler=0xf044820, IcaUserData=&table}`.
- `[FACT]` **`VDMICAUSERDATA`** = pointers `{ pIcaLock, pIcaMaster, pIcaSlave, pDelayIrq,
  pUndelayIrq, pDelayIret, pIretHooked, pAddrIretBopTable, ... }` (ReactOS lists 11; XP SP3's
  `ntvdm` fills **9** — the trailing WOW/idle fields are newer). The 9 pointers ntvdm passes are
  its own ICA (interrupt-controller) lock + master/slave PIC state + delay/iret hook tables.

### `VDM_TIB` — the genuinely-undocumented piece (XP-only ground truth)
`[FACT]` **`TEB.Vdm` is at offset `0xF18`** (32-bit). Confirmed in `ntvdm.exe`: the VDM_TIB pointer
is fetched as `mov eax, fs:[0x18]` (TEB self) → `mov reg, [eax+0xF18]` (≈30 sites).
`[FACT]` A VDM_TIB field is accessed at **`+0x2D8`** (`add esi, 0x2d8` after the load).
`[FACT]` **ReactOS does NOT define `VDM_TIB`** — it uses Fast486 emulation, never the real V86 TIB.
This is precisely the gap the project identified: ReactOS validates DOS/VDD logic but *not* the
V86/`NtVdmControl` path. So the VDM_TIB byte layout must come from the XP `ntvdm`/`ntoskrnl`
disassembly alone (it holds the `CONTEXT VdmContext` we set before `VdmStartExecution`, plus fault
info). Next increment: map the fields ntvdm reads/writes off `[TEB+0xF18]` — especially the
`VdmContext` (CONTEXT) offset, since that carries the V86 CS:IP/registers.

### Still to recover
`VDM_TIB` field map (VdmContext offset, fault info); the low-memory reservation `VdmInitialize`
expects; and the CSRSS support-process registration handshake (`csrsrv`/`basesrv` side) that must
precede `GetNextVDMCommand`.

Sources for the cross-reference: ReactOS `ndk/ketypes.h` (VDMSERVICECLASS, VDM_INITIALIZE_DATA,
VDMICAUSERDATA); TEB.Vdm offset 0xF18 from public TEB documentation.

## Action
Remaining `[VERIFY]` items above are the explicit objectives of
[Spike-001](../spikes/spike-001-v86-keystone.md). Extracted MS binaries live in the gitignored
`reverse/` dir (`cabextract` from the install CD's `I386\`).
