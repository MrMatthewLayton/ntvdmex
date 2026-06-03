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

#### Baseline control + PE-match attempt (2026-06-02, second push)
- `[FACT]` **Control passes:** with `cmdline` restored to `%SystemRoot%\system32\ntvdm.exe`, the
  interactive trigger runs `dosstub.com` **and** `dosstub.exe` cleanly — `COM-errorlevel=0`,
  `EXE-errorlevel=0`, `DONE`. So the VM's DOS VDM works and both triggers are valid 16-bit programs.
  The failure is **specific to substituting our binary**.
- `[FACT]` **PE-shape match did NOT help.** Rebuilt `vdmhost` to match real `ntvdm` exactly:
  ImageBase `0x0F000000` (low memory kept free for the V86/DOS space — the obvious suspect), CUI
  subsystem v4.0, `DllCharacteristics=0` (no ASLR/NX). It runs fine standalone (STAGE0/1/2, now with
  a real `ConsoleWindow`) but **still no STAGE0 as the VDM host**. So it is *not* a PE-characteristic
  issue.
- `[FACT]` **Failed substitution wedges the kernel:** after our binary fails as the host, the next
  `shutdown -r` **hung ~21 min** (the real ntvdm baseline shut down in ~4). Forcing a QMP
  `system_reset` recovers but dirty-boots, and **loses un-flushed registry writes** (silently
  reverted a `cmdline` change once — wait ~2 min after a `reg add` before any reset).

**The decisive open test (running):** repoint `cmdline` at a **copy of the real ntvdm at a
non-system32 path** (`C:\ntvdmex\realntvdm.exe`) and trigger `dosstub`.
- If it **runs** → the substitution mechanism is path-independent → our binary is rejected for a
  specific (findable) reason → approach still viable.
- If it **fails** → XP requires the host to be `system32\ntvdm.exe` (path/identity locked) → simple
  `cmdline`-repoint to an arbitrary binary is a **dead end**; options become (a) replace
  `system32\ntvdm.exe` itself (the WFP fight ADR-0002 hoped to avoid) or (b) a different hook
  (e.g. a debugger/Image-File-Execution-Options shim, or replacing `ntvdm` and fighting WFP).

Spike-002's "interception confirmed" is now most likely a **standalone run of `wowprobe`**
mis-read as a VDM-host launch (its `GetCommandLineA` = bare path is identical either way).

#### Host-substitution constraint table (2026-06-03) — the core feasibility result
Each row: set `cmdline`/file as shown, reboot, trigger `dosstub` interactively, observe.

| host binary | location | `dosstub` runs? | our STAGE0? |
|---|---|---|---|
| real `ntvdm.exe` | `system32\ntvdm.exe` (canonical) | **YES** (errorlevel 0, DONE) | n/a |
| real `ntvdm.exe` (byte copy) | `C:\ntvdmex\realntvdm.exe` | **NO** (blocks, no DONE) | n/a |
| our `vdmhost.exe` | `C:\ntvdmex\vdmhost.exe` | NO | **no** |
| our `vdmhost.exe` | `C:\WINDOWS\system32\vdmhost.exe` | NO | **no** |
| our `vdmhost.exe` → `ntvdm.exe` | `system32\ntvdm.exe` (replace) | *running* | *running* |

`[FACT]` The **genuine ntvdm fails from a non-system32 path**, and **our binary fails from every
path tried (including system32)** — never executing a single instruction. So XP's DOS-VDM launch is
**not** a transparent "point `cmdline` at any exe" substitution: the host must be the canonical
`C:\WINDOWS\system32\ntvdm.exe`. This **directly contradicts ADR-0002's premise** (repoint `cmdline`
→ our host, no signed-file replacement, no WFP fight). The last row (in progress) tests whether
our binary works when it physically **replaces** `system32\ntvdm.exe` (WFP defeated by also
overwriting `dllcache\ntvdm.exe` — verified it is not auto-restored).

`[FACT]` WFP note: overwriting both `system32\ntvdm.exe` and `system32\dllcache\ntvdm.exe` leaves
our binary in place (no auto-restore within seconds). So file replacement is achievable on XP;
the open question is whether the *launch* accepts our image content once it sits at the canonical
path/name, or validates it further.

#### ⛔ CONCLUSION — host substitution is NOT possible (2026-06-03)
`[FACT]` **Definitive:** our binary was placed AT `C:\WINDOWS\system32\ntvdm.exe` (replacing the
genuine ntvdm; WFP defeated, confirmed 17,507-byte file in place), `cmdline` at the default. Trigger
`dosstub` → **`NO-VDMHOST-LOG`, no `STAGE0`** — our binary still never executes; `dosstub` → "not a
valid Win32 application". So **XP refuses to launch our image as the DOS VDM host under *every*
substitution tried**: `cmdline` repoint to any path, and physical replacement of `system32\ntvdm.exe`
itself. Combined with "the genuine ntvdm only hosts from its canonical system32 path", the DOS-VDM
host launch **validates the host image** — it accepts only the real ntvdm. **ADR-0002's transparent
WOW-host interception is not viable.** (The genuine ntvdm.exe has since been restored on the VM.)

#### ✅ BREAKTHROUGH — IFEO Debugger redirect runs our code transparently (2026-06-03)
`[FACT]` Setting `HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Image File Execution
Options\ntvdm.exe` → `Debugger = C:\ntvdmex\vdmhost.exe` makes XP launch **our binary in ntvdm's
place** whenever the DOS VDM host would start. A 16-bit launch fired our binary and it ran to
completion (`STAGE0/1/2/DONE`) **in the real host context**: live `ConsoleWindow=0x000100ba`,
`StdIn=3 StdOut=7`, and ntvdm's actual command line:
```
C:\ntvdmex\vdmhost.exe "C:\WINDOWS\system32\ntvdm.exe" -f -i1
```
Because IFEO launches us as a *debugger* (a normal process), the host-image validation that blocks
substitution never applies. This is the transparent, registry-only, WFP-free interception
([[ADR-0007]]). It also disproves Spike-002's "no argv": the real DOS host is `ntvdm -f -i1`.

`[FACT]` **ntvdm arg parser** (`0xf01a44a` loop): `-f` stores its arg index at `[ebp-0x4]` and is
**required** (absent → `ExitProcess(0)` via `ds:0xf001034`). `-i<n>` parses `n` as **hex** (call
`0xf00b8af`, base `0x10`) into an instance global. `-w[sm]` = WOW flags; `-o` clears a flag.
`[FACT]` Pre-`GetNextVDMCommand` registration ntvdm performs (what we must replicate so `0x57`
clears): **`NtVdmControl(VdmInitialize)`** (fn `0xf00e668`, called from `0xf01abb6`) and
**`RegisterConsoleVDM(1,…)`** (`0xf014078`; flag = 1 DOS / 2 WOW, plus hardware-event handles +
video-state buffers), bracketed by console-mode setup. This is a large init, not a one-liner — best
built incrementally from the IFEO foothold.

#### VdmInitialize first attempt + the V86 memory prerequisite (2026-06-03)
`[FACT]` Implemented `NtVdmControl(VdmInitialize)` in the IFEO-launched binary with the exact
recovered struct (`VDM_INITIALIZE_DATA{TrapcHandler, IcaUserData}`, `IcaUserData` = 9 ptrs to our
own ICA buffers; trap stub). Result via IFEO: **`NTSTATUS=0xC0000005` (STATUS_ACCESS_VIOLATION)** —
`NtVdmControl` returns the status (doesn't crash us; we continued to STAGE2). Not a permission error
(`0xC0000022`) — a *memory* fault: the kernel sets up V86 mode against the **low address space**,
which we haven't laid down.

`[FACT]` **The pre-`VdmInitialize` V86 memory setup is `ntvdm` fn `0xf00ea75`** (called immediately
before the `VdmInitialize` call at `0xf01abb6`). It builds the V86 RAM:
1. **`NtCreateSection`** (`ds:0xf0014c8`) — section for V86 memory (size arg `0x04000000`, etc.).
2. **`NtFreeVirtualMemory`** (`ds:0xf0014d0`, `MEM_RELEASE 0x8000`) — release the default low
   reservations: base `→0` size `0x9FFFF` (640 KB conventional), then base `0x100000` size
   `0x10000` (HMA).
3. **`NtMapViewOfSection`** (`ds:0xf0014c0`, prot `0x40` = X-RW) — map the section into the freed low
   addresses, base `[ebp-0x10]=0xB0000`. This *is* the V86 address space.

So the order is: **lay down V86 low memory (section + free + map) → `VdmInitialize` → … →
`VdmStartExecution`**. The `0xC0000005` is expected until step 1–3 are done.

#### ✅✅ VdmInitialize SUCCEEDS after V86 memory setup (2026-06-03)
`[FACT]` Implemented ntvdm's V86 memory setup in our IFEO binary and re-ran. Result:
```
STAGE1m: NtCreateSection=0x00000000 map0=0x00000000 map1=0x00000000
STAGE1: NtVdmControl(VdmInitialize)... NTSTATUS=0x00000000 (OK)
returned FALSE   GetLastError=0x00000057   <- GetNextVDMCommand
```
All three memory calls and **`VdmInitialize` succeed**. The kernel has now set up **V86 mode** for our
(IFEO-launched) process — we are a real, kernel-registered VDM with the low address space mapped.
The exact replicated sequence: `NtCreateSection(&h, 0xA, oa{len 0x18}, max=0xB0000, X-RW, SEC_RESERVE,
NULL)` → `NtFreeVirtualMemory(-1, 0→0x9FFFF, RELEASE)` + `(0x100000→0x10000)` →
`NtMapViewOfSection(h, -1, base→0, 0, 0xFFFF, off 0, view 0xFFFF, ViewUnmap=2, 0x40000000, X-RW)` +
the same at `0x100000`.

`[FACT]` **`GetNextVDMCommand` is still `0x57` even with `VdmInitialize` done.** So the program fetch
is gated by a **CSRSS-side** registration, *not* the kernel `VdmInitialize`.

`[FACT]` Disassembled **`kernel32!GetNextVDMCommand`** (`0x7c867f23`, extracted KERNEL32.DLL from the
XP ISO). It does **no client-side VDM check** — reads `VDMState` at struct `+0x98` (our `0x500` →
the "fetch" branch at `0x7c8680ac`), allocates a CSR capture buffer from the `*Len` fields, and
calls `CsrClientCallServer`. So the `0x57` is purely **server-side** (basesrv
`BaseSrvGetNextVDMCommand`), from either:
  (a) the gate `CsrLockProcessByClientId(callerPid)` (`basesrv 0x75b55208` → csrsrv import confirmed)
      — fails if CSRSS doesn't have our process associated with the VDM, **or**
  (b) one of the `CsrValidateMessageBuffer` calls on the captured buffers.
Both return `0xC000000D` (→ Win32 `0x57`). Distinguishing them needs a kernel debugger on the VM or
iterative tests. The fix is the **CSRSS VDM↔host-process association**.

`[FACT]` **`RegisterConsoleVDM(1,…)` implemented and returns TRUE — but `GetNextVDMCommand` is STILL
`0x57`.** (ntvdm's DOS call passes video-buffer/size = 0, so a minimal call works: flag 1, three
`CreateEvent` handles, scratch OUT ptrs.) Because that CSRSS call *succeeded*, CSRSS **can** lock our
process — so the `0x57` is **not** the `CsrLockProcessByClientId` gate. The basesrv command-fetch
path (after the gate) walks a **command list** under crit-sects `0x75b5d5e0`/`0x75b5d600`, keyed by
our process's VDM record `proc->[0x34]`. So `0x57` = **no command queued/bound to our process**.

`[HYPOTHESIS]` The VDM↔host binding (CSRSS `proc->[0x34]` + the queued command) is normally set when
kernel32, *after* creating ntvdm, calls back into CSRSS (`BaseSrvUpdateVDMEntry`) to register the new
host PID. The **IFEO `Debugger` redirect** may break this: kernel32 thinks it launched `ntvdm.exe`
but the process is our debugger, so the command may be bound to a phantom/none. If so, IFEO can *run*
our code but cannot fetch the program via `GetNextVDMCommand` — we'd need the program another way.
**Next (chosen: re-establish the binding from our process, on fast HVF):** call CSRSS
**`BaseSrvUpdateVDMEntry`** ourselves to bind the queued command to our process, then
`GetNextVDMCommand`.
- `[FACT]` It is **not exported** — replicate as a raw `CsrClientCallServer` (ntdll `0x7c912d71`,
  exported). kernel32's wrapper `BaseUpdateVDMEntry` (`0x7c868a2a`) does:
  `CsrClientCallServer(&msg, captureBuf, ApiNumber=0x10006, DataLen=0x18)`. API `0x10006` =
  basesrv(1) api 6. The 0x18-byte API payload (at `msg+0x28`) fields: `+0x00,+0x04,+0x08,+0x0c`
  (DWORDs), `+0x10` (out), `+0x14,+0x16` (WORDs). The wrapper takes 2 args (`[ebp+0x8]`,`[ebp+0xc]`)
  sourced from kernel32's `CreateProcess` VDM state (VDM task id, host info).
- `[OPEN]` The exact payload field values (esp. the VDM **task id** — likely the `-i1` we get = 1 —
  and host PID/handle) need one more trace of a caller (`0x7c842ea7` / `0x7c868e07`) before
  implementing. Server side: `BaseSrvUpdateVDMEntry` = dispatch idx 6 = `0x75b584fe`.
- Fallback if this proves intractable: TCG + gdb-stub to inspect `proc->[0x34]`/the queue at the
  `BaseSrvGetNextVDMCommand` breakpoint.

Stages reached by our IFEO binary (all logged): `STAGE0` → V86 memory (`NtCreateSection`/2×map all 0)
→ `VdmInitialize` NTSTATUS 0 → `RegisterConsoleVDM` TRUE → `GetNextVDMCommand` still `0x57`.

Remaining to a running DOS program: CSRSS registration → `GetNextVDMCommand` (program path) → load
it into the mapped low memory → set `VDM_TIB.VdmContext` (CS:IP/regs) → `NtVdmControl(VdmStartExecution)`.

#### ➡️ PIVOT — transparent host via IFEO + NtVdmControl (the goal is reachable)
The project goal (DOS/Win16 on real V86) does **not** require *becoming* ntvdm transparently. The
WOW-host hijack was one architecture (ADR-0002), now disproven. The viable architecture:

- `[PLAN]` **NTVDMEX as a standalone host.** The user invokes `ntvdmex.exe <dosprog>` directly (or
  we register as the handler for `.COM/.EXE/.PIF`/App Paths). We get the program from **our own
  argv** — *not* CSRSS's `GetNextVDMCommand` — and create the V86 environment **directly** via
  `NtVdmControl(VdmInitialize)` → load program into low memory → `NtVdmControl(VdmStartExecution)`,
  using the recovered contract ([[spike-001 NtVdmControl]] facts above + the `VDM_TIB`/`VdmContext`
  map still to finish).
- This **moots the `GetNextVDMCommand 0x57` blocker** entirely — we never need to be a
  CSRSS-registered VDM. We control the whole launch.
- Our binary already **runs standalone** (STAGE0/1/2 confirmed), so there is no launch obstacle on
  this path. The keystone work is purely the `NtVdmControl` V86 entry (Spike-001), independent of
  the dead WOW-interception route.

**Next:** finish the `VDM_TIB`/`VdmContext` (CONTEXT) byte map and the low-memory reservation
`VdmInitialize` expects, then attempt a minimal V86 entry from a standalone `ntvdmex.exe` — no WOW
repoint, no CSRSS handshake.

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

## 2026-06-03 — `GetNextVDMCommand` returns TRUE (the `0x57` wall was a harness artifact)

The CSRSS handshake "still to recover" above is now recovered from the **server side**
(`reverse/basesrv.dll`, base `0x75b50000`, via radare2). The dispatch table is at `0x75b5d080`:
idx 5 = `BaseSrvCheckVDM` (`0x75b58410`), idx 6 = `BaseSrvUpdateVDMEntry` (`0x75b584fe`),
idx 7 = `BaseSrvGetNextVDMCommand` (`0x75b5702e`).

### How `GetNextVDMCommand` finds the queued command — and what `0x57` actually means
`BaseSrvGetNextVDMCommand` (`0x75b5702e`):
1. `CsrLockProcessByClientId([msg+8])` — locates the **calling** process by the LPC ClientId. Found
   for us fine (so the prior "process binding"/`proc->[0x34]` theory was a red herring).
2. `CsrValidateMessageBuffer` over each buffer pointer in the request Data (at `msg+0x28`).
3. Branch on `Data+0` (TaskId) for the first command (`VDMState & 0x100`):
   - **TaskId != 0** → `0x75b55602`: walk global list `[0x75b5d2ec]`, match `node+0x20 == TaskId`
     **only where `node+4 == 0`** (console not yet bound).
   - **TaskId == 0** → `0x75b555cd`: walk `[0x75b5d2ec]`, match `node+4 == Data+4` (the **console handle**).
4. With the record in hand: `edi = [record+0x30]` (the queued DOS command). For a first command,
   **`if ([record+0x30] == 0) → STATUS_INVALID_PARAMETER (0xC000000D → Win32 0x57)`**.

So `0x57` never meant "not registered as a VDM". It means **the lookup landed on a record with no
queued command** — i.e. the wrong console (or none). `BaseSrvCheckVDM`'s worker (`0x75b5796e`) is
what creates the record and stores the command at `record+0x30`, the assigned task id at
`record+0x20`, and (via the external console-record callback `[0x75b5d2e8]`) the console handle at
`record+4`; it returns that task id to kernel32 as the `-i<n>` on the ntvdm cmdline.

### The fix: trigger in the launcher's console (no code change was even required)
The earlier `0x57` runs all used the **reboot / Run-key interactive trigger**, which launches the
16-bit app in a *different console* than our IFEO host inherits → console-key miss → empty record →
`0x57`. Launching the 16-bit stub **directly in the telnet session** (launcher and our IFEO host
share one console) makes the console-key match, and:

```
CmdLine=[C:\ntvdmex\vdmhost.exe "C:\WINDOWS\system32\ntvdm.exe" -f]   (note: NO -i, TaskId stays 0)
STAGE1m  NtCreateSection=0  map0=0  map1=0
STAGE1   NtVdmControl(VdmInitialize) NTSTATUS=0  (OK)
STAGE1r  RegisterConsoleVDM TRUE
GetNextVDMCommand  returned TRUE     <-- 0x57 GONE
  CurDir : C:\ntvdmex   (CurDirLen 0x0b)   <-- real, launcher-specific; varied with the launcher CWD
```

Reproducible; `CurDirectory` correctly tracked the launcher's working directory across runs, proving
a genuine command fetch (not an empty TRUE). `vdmhost.c` now also parses `-i<n>` → `g_ci.TaskId` to
take the task-id path when present, but under IFEO the cmdline we receive has **no `-i`**, so the
console path is the one that fires.

### Open for the load/execute phase (not a blocker)
The fetch fills `CurDir`/`Title`/`PifLen` but leaves `CmdLine`/`AppName`/`Env` lengths at our input
sizes with non-ASCII buffer content — decode why (VDM_COMMAND_INFO field/handle layout, or whether the
program path arrives on a follow-up call) to recover the image path. Then: load the image into the
mapped low memory → populate `VDM_TIB.VdmContext` (CS:IP/regs) → `NtVdmControl(VdmStartExecution)`.
