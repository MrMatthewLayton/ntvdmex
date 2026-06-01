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

## Action
All `[VERIFY]` items above are the explicit objectives of
[Spike-001](../spikes/spike-001-v86-keystone.md).
