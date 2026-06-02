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
wrong field *value/flag*. `ntvdm` populates the struct inside a helper (`0xf04d3af`); `0xf00e126` turned out to be
`GetNextVDMCommand(NULL)` (an ack/cleanup variant), not the fetch.

Refined findings (automated via telnet+TFTP):
- `[FACT]` Standalone `0x57` is **"not a VDM process"**, not a buffer-size negotiation: the
  post-call `*Len` fields are **unchanged** (ReactOS shows `STATUS_INVALID_PARAMETER` *would*
  update them to required sizes if a buffer were too small — it didn't).
- `[FACT]` **VDM-host launches need an interactive logon session.** Creating the `ntvdmex` telnet
  account flipped XP from auto-logon to the account-picker, so after a plain reboot *no one is
  logged in*. Telnet (a service) still works — `reg`, file ops, TFTP, and *standalone* `vdmhost`
  all run — but triggering a 16-bit app (`dosstub`) does **not** bring up the VDM host with no
  interactive desktop, which is why the VDM-host logs came back empty. The earlier successful
  VDM-host run was while a user was logged in.
- Mitigation: enable **auto-logon** (`Winlogon\AutoAdminLogon=1` + `DefaultUserName`/`Password`)
  so XP always boots to a desktop — reproducible interactive session for VDM-host tests.

#### Server side: where `0x57` actually comes from (basesrv disasm, 2026-06-02)
Traced the CSRSS end of the call. `basesrv.dll` (`ImageBase 0x75b50000`) registers its API dispatch
table in `ServerDllInitialization`: table at **`0x75b5d080`, 32 entries**. BaseSrv API **index 7 =
`GetNextVDMCommand` handler = `0x75b5702e`** (`BaseSrvGetNextVDMCommand`). Its logic:

- `[FACT]` First it calls a gate at **`0x75b55208`**: `CsrLockProcessByClientId(callerPid, &proc)`
  then `*out = proc->[+0x34]` (the **per-process VDM data pointer** CSRSS attaches to a process it
  created as a VDM). The gate returns `STATUS_INVALID_PARAMETER (0xC000000D → Win32 0x57)` only if
  the *process lookup* fails — but everything downstream dereferences `proc->[0x34]`.
- `[FACT]` Then each client buffer (`CmdLine/AppName/PifFile/CurDir/...` at `msg+0x28+{0x24,0x28,
  0x2c,0x30,0x48,0x50,0x44,0x3c}`, lengths at `+0x5a..`) is checked by **`CsrValidateMessageBuffer`**
  (`ds:0x75b51014`); any pointer not inside the CSR capture region → `0xC000000D`.
- `[FACT]` `VDMState` is read at payload `+0x62`; the handler tests bits `0x800`, `0x2`, `0x10`,
  `0x40`, and sign(`0x8000`) to branch between "fetch first command" vs ack/notify variants.

So `0x57` means **either** the caller isn't a CSRSS-created VDM (its `proc->[0x34]` VDM record is
absent) **or** the buffer pointers fail capture-region validation. Standalone, it's the former.
The fix path is to be launched *as* a real VDM by CSRSS (the WOW `cmdline` repoint) **and** in a
context where CSRSS actually builds the VDM record.

Subsystem note: real `ntvdm.exe` is **Subsystem 3 (Console/CUI), SubsystemVersion 4.0**; our
`vdmhost` was GUI (2). We tried a console build — it made no positive difference (see the STAGE
finding) and is **not** the cause of `0x57`. `vdmhost` is back to GUI (matching the Spike-002
`wowprobe`), now **log-only** (no `MessageBox`) so it can't block in any window station.

#### Interactive-trigger harness (2026-06-02)
`[FACT]` A 16-bit launch from the **telnet/service window station** (or an `at`/SYSTEM job) does
**not** fire the WOW/VDM host path — `dosstub` returns silently, no host. The VDM host only launches
from a real **interactive** session. Harness now: **auto-logon** (`Winlogon\AutoAdminLogon=1`,
`DefaultUserName`/`DefaultPassword=ntvdmex`) + a **Run-key trigger**
(`HKLM\...\CurrentVersion\Run\vdmtrig` → `C:\ntvdmex\vdmtrig.bat`, runs `dosstub` at logon). Reboot →
auto-logon → trigger fires. Verified working via QMP screendump (desktop + the trigger's `cmd` box).

#### ⚠️ STAGE finding — our binary does NOT execute as the VDM host (2026-06-02)
`[FACT]` Instrumented `vdmhost` to write `C:\ntvdmex\vdmhost.log` as its **very first instruction**
(`STAGE0`, before any other API call), then again before/after `GetNextVDMCommand` (`STAGE1`/`STAGE2`).
When `dosstub.com` is run interactively (Run-key trigger, desktop confirmed up):
- **No `vdmhost.log` at all — not even `STAGE0`.** Tried both GUI and console builds.
- `tasklist` shows **no `vdmhost.exe` and no `ntvdm.exe`** process while the launch is blocked.
- The `start /wait dosstub.com` blocks on a **"C:\ntvdmex\dosstub.com is not a valid Win32
  application"** dialog; dismissing it lets the batch finish.

So XP raises "not a valid Win32 application" **without ever running our repointed binary**. This
**overturns the earlier reading** that that dialog was the "success signature" (our host launching
but not completing the handshake). It is unclear our binary is being launched as the VDM host at all.

`[FACT]` The instrumentation is **proven sound**: the same `vdmhost.exe` run *standalone* writes all
three stages — `STAGE0` / console diag (`ConsoleWindow=0`, valid `StdIn`/`StdOut`) / `STAGE1` /
`returned FALSE GetLastError=0x57` / `STAGE2`. So "no `STAGE0` as VDM host" is a real negative, not a
logging artifact. (This contradicts the Spike-002 note that `wowprobe` logged its bare cmdline *as*
the VDM host — hence priority #2 below: re-check `wowprobe` the same way; one of the two reads is
mis-attributed.)

`[FACT]` **`cmdline` format is a launch template with parameters, not a bare path.** The sibling
`wowcmdline` (Win16 host) default is `%SystemRoot%\system32\ntvdm.exe -a %SystemRoot%\system32\krnl386`
— note the `-a <krnl386>` parameters. The DOS `cmdline` default (which we overwrote with a bare
`C:\ntvdmex\vdmhost.exe`) almost certainly had its own template/params. The true default is **lost**
(the Spike-002 `wow-backup.reg` was captured *after* an earlier repoint, so it already reads
`C:\ntvdmex\...`). Recovering the genuine default `cmdline` (fresh XP image, or how `csrss`/`basesrv`
builds the launch line) is the leading suspect for why our host never executes.

**Next (in priority order):**
1. Recover the genuine default DOS `cmdline` (mount a clean XP hive, or read the CSRSS
   `BaseSrvCheckVDM` launch-line construction). Repoint preserving its exact format, just swapping
   the exe path — then re-test for `STAGE0`.
2. Re-validate the premise: repoint at the Spike-002 `wowprobe` with the same STAGE0-first
   instrumentation; does `wowprobe` write `STAGE0` as a VDM host? If no, Spike-002's "launch as VDM
   host" needs reinterpreting (its evidence may have been a standalone run).
3. Check whether CSRSS creates the host **suspended** and kills it before our entry point if an
   early pre-`main` CSRSS handshake is missing (real `ntvdm` does CSRSS setup very early).

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
