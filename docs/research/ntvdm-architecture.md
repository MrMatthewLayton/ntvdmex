# How real NTVDM is structured on XP-32

> Confidence tags per claim. Items marked `[VERIFY]`/`[BELIEF]` should be confirmed by
> disassembly of the shipping XP SP3 binaries during M0/M1.

## Launch path
- A 16-bit image (`MZ` DOS, `NE` Win16) passed to `CreateProcess` is detected by the loader;
  the OS launches a **VDM support process** instead of the image directly. `[FACT]`
- Which support process is read from the registry under
  `HKLM\SYSTEM\CurrentControlSet\Control\WOW`: `cmdline` (DOS) and `wowcmdline` (Win16).
  `[BELIEF — confirm exact names/format]`
- DOS gets a fresh `ntvdm.exe`; Win16 apps share one WOW VDM by default ("shared WOW VDM"),
  optionally "run in separate memory space". `[BELIEF]`

## Process-side components
- `ntvdm.exe` — the VDM host (usermode). `[FACT]`
- DOS personality: 16-bit DOS images / `ntio.sys` + `ntdos.sys` analog, INT 21h handling,
  PSP/FCB, DOS memory (MCBs), command interpreter. `[BELIEF]`
- WOW personality: `wowexec.exe`, `wow32.dll`, and 16-bit `krnl386.exe`/`user.exe`/`gdi.exe`
  with thunks down to Win32. `[BELIEF]`
- `vdmdbg.dll` — debugging support for VDMs. `[BELIEF]`
- **VDDs** (Virtual Device Drivers) — usermode DLLs that service virtualized hardware
  (`VDDInstallIOHook`, `VDDInstallMemoryHook`, etc.). This is the model we expose to third
  parties (requirement #13). `[BELIEF]`

## Kernel-side support
- V86 entry/exit, GP-fault trapping for I/O ports & privileged instructions, and interrupt
  reflection live in `ntoskrnl` (the `Vdm*` routines). Driven from usermode via the
  `NtVdmControl` syscall. `[BELIEF]` — see [ntvdmcontrol-and-v86.md](ntvdmcontrol-and-v86.md).
- Per-thread `VDM_TIB` carries the V86 register/segment state and control fields the kernel
  reads/writes on each trap. `[BELIEF — structure layout to recover]`

## What this means for NTVDMEX
- We can replace the **usermode** side wholesale (our `ntvdmex.exe` + our VDDs).
- The **kernel** side we intend to *reuse* (ADR-0004) — so we must conform to the `VDM_TIB`
  and low-memory layout the stock kernel expects. The size of that conformance burden is the
  central unknown the project must resolve first.

## Open questions to resolve via disassembly
- [ ] Exact `VDM_TIB` layout for XP SP3 and where its pointer lives (TEB offset?). `[VERIFY]`
- [ ] Full `NtVdmControl` service enum + per-service argument structures. `[VERIFY]`
- [ ] Required low-memory reservation (range, attributes) before `VdmInitialize`. `[VERIFY]`
- [ ] Whether any check ties VDM init to the image identity/path of the caller. `[VERIFY]`
