# Glossary

| Term | Meaning |
|------|---------|
| **NTVDM** | NT Virtual DOS Machine — the XP component that hosts 16-bit DOS/Win16 apps. The thing we replace. |
| **NTVDMEX** | This project. NTVDM, Extended — a from-scratch, V86-based, pluggable replacement. |
| **V86 / Virtual-8086 mode** | A sub-mode of 32-bit protected mode that runs real-mode 16-bit code on the real CPU under OS supervision. Gone in x86-64 long mode. |
| **Real mode** | The CPU's 16-bit segment×16+offset mode (1MB+64KB address space). What DOS code expects. |
| **Protected mode** | 32-bit mode with descriptors/paging. Host OS, DPMI extenders, and Win16 PM code live here. |
| **`NtVdmControl`** | The single (undocumented) syscall through which a usermode VDM host drives the kernel's V86 machinery. |
| **`VDM_TIB`** | Per-thread VDM Thread Information Block holding V86 register/segment/control state shared with the kernel. |
| **VDD** | Virtual Device Driver — usermode DLL that services virtualized hardware (I/O ports, memory). Our third-party plug-in unit. |
| **WOW** | Windows-on-Windows (16-bit): the layer that runs Win16 apps by thunking to Win32 (`krnl386`/`user`/`gdi`). |
| **Thunk** | Code that marshals a call between 16-bit (16:16) and 32-bit (flat) worlds, translating pointers/params. |
| **NE / MZ** | Executable formats: `MZ` = DOS, `NE` = 16-bit Windows (New Executable). |
| **DPMI / XMS / EMS** | DOS memory/extender interfaces: DOS Protected Mode Interface, eXtended/Expanded Memory Specs. |
| **WFP** | Windows File Protection — restores protected `system32` files if changed. The real obstacle to file replacement. |
| **DSE / KMCS** | Driver Signature Enforcement / Kernel-Mode Code Signing — Vista x64+ only; **not** on XP-32. |
| **dllcache** | `%SystemRoot%\system32\dllcache` — WFP's backup store of protected files. |
| **Luna** | The XP visual theme NTVDMEX windows must match (requirement #4). |
| **IVT** | Interrupt Vector Table at physical/linear 0 — real-mode interrupt dispatch table. |
| **Fast486** | ReactOS's software 486 emulator. Why ReactOS is *not* a V86 reference. |
| **dosemu** | Linux DOS host using the kernel's `vm86()`/KVM — closest architectural analog to NTVDMEX. |
