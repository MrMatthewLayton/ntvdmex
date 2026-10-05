/* v86.h -- drive XP's kernel VDM machinery to run real-mode code in Virtual-8086
 * mode. Wraps the undocumented NtVdmControl path (memory map, VdmInitialize, the
 * self-allocated VDM_TIB, the entry CONTEXT, and the VdmStartExecution/event loop).
 * Ported from tools/vdmhost/vdmhost.c; the contract lives in ntvdm.h.
 *
 * Sequence the host uses: VdmSetupMemory() -> VdmRegisterWithKernel() -> VdmGetTib(),
 * then build the DOS process in low memory, VdmSetEntry(), and loop on VdmRunGuest().
 *
 * The function names avoid the NT ones on purpose: VdmInitialize, VdmSetLdtEntries and
 * VdmStartExecution are NtVdmControl services, and VdmMapFlat is an ntvdm.exe export.
 */
#ifndef NTVDMEX_VDM_V86_H
#define NTVDMEX_VDM_V86_H

#include <windows.h>
#include "ntvdm.h"

/* TEB self-pointer (fs:[0x18]) without the CRT/winternl. */
PVOID VdmGetTeb(VOID);

/* Lay down the V86 low-memory address space (the 4-section map covering the full
   640KB) -- must run before VdmRegisterWithKernel or VdmInitialize access-violates.
   Returns the final NtMapViewOfSection status (>= 0 == success). */
LONG VdmSetupMemory(VOID);

/* NtVdmControl(VdmInitialize): register this process as a VDM with the kernel so
   GetNextVDMCommand works and the guest can run. Returns NTSTATUS (>= 0 ok), or 1
   if ntdll!NtVdmControl is unavailable. Caches the entry point for VdmRunGuest(). */
LONG VdmRegisterWithKernel(VOID);

/* Find a free 64KB UMA hole and map the EMS page frame there as V86 RAM. Must run
   AFTER VdmRegisterWithKernel(): pre-mapping a section view in the UMA makes
   VdmInitialize fail with STATUS_UNABLE_TO_FREE_VM (it requires that range free during
   init). After init only part of the UMA is free (e.g. E0000 has just 32KB), so we scan
   the conventional page-frame segments for a hole big enough. Returns the linear base
   of the mapped 64KB frame (0 on failure). The guest learns the segment via EMS
   INT 67h AH=41, so any hole works. */
DWORD VdmMapEmsFrame(VOID);

/* Return the VDM_TIB (TEB+0xF18). The kernel does not allocate it; if absent we
   allocate our own static TIB, initialise the fields ntvdm sets, and register it.
   Returns NULL only if the TEB is unreadable. */
volatile BYTE *VdmGetTib(VOID);

/* Write the entry V86 CONTEXT into the TIB: CS:IP, SS:SP, DS=ES=FS=GS=pspSegment,
   general registers 0, EFlags = VM, full-context ContextFlags. */
VOID VdmSetEntry(_Inout_ volatile BYTE *tib, _In_ WORD codeSegment, _In_ WORD instructionPointer,
                 _In_ WORD stackSegment, _In_ WORD stackPointer, _In_ WORD pspSegment);

/* Run the guest (VdmStartExecution) until the next stop; returns the event code
   (VDM_EVENT_BOP for a serviceable BOP). *status gets the NtVdmControl status if
   non-NULL. Requires VdmRegisterWithKernel() to have cached NtVdmControl. Runs whichever
   mode the CONTEXT's EFLAGS.VM bit selects: VM=1 -> V86, VM=0 + LDT selectors -> PM
   (recovered from ntvdm fcn.0f00532e; see research/dpmi-under-ntvdmcontrol.md). */
DWORD VdmRunGuest(_In_ volatile BYTE *tib, _Out_opt_ LONG *status);

/* The kernel's virtual PIC (see the ICA_* offsets + rationale in ntvdm.h). Raising a
   line here is what makes NtVdmControl(VdmQueueInterrupt) able to deliver: the APC it
   queues asks the ICA which vector to inject. Raise, then set VDM_INT_HARDWARE in
   FIXED_NTVDMSTATE, then queue. The ISR bit stays set until VdmIcaEndOfInterrupt(), so
   the guest's EOI write must reach us or that line never fires again. */
VOID  VdmIcaSetBase(_In_ UINT vectorBase);  /* experiment: distinguish kernel vs host delivery */
VOID  VdmIcaRaise(_In_ UINT irq);
VOID  VdmIcaEndOfInterrupt(_In_ UINT irq);
VOID  VdmIcaSetMask(_In_ UINT irq, _In_ INT isMasked);
DWORD VdmIcaGetState(_In_ UINT irq);   /* IRR | ISR<<8 | IMR<<16, for logging */

/* Raw NtVdmControl passthrough (for DPMI's LDT + PM-cli services). Returns NTSTATUS
   (>= 0 ok). Requires VdmRegisterWithKernel() to have cached the entry point. */
LONG VdmControl(_In_ ULONG service, _In_opt_ PVOID serviceData);

/* Install up to two LDT descriptors via NtVdmControl service 10 (VdmSetLdtEntries),
   whose ServiceData is exactly the NtSetLdtEntries 6-dword block
   {Sel0,Entry0Low,Entry0Hi,Sel1,Entry1Low,Entry1Hi} (recovered from fcn.0f050100).
   Pass secondSelector=0 to set only one. Returns NTSTATUS (>= 0 ok). */
LONG VdmInstallLdtEntries(_In_ WORD firstSelector, _In_ DWORD firstLow, _In_ DWORD firstHigh,
                          _In_ WORD secondSelector, _In_ DWORD secondLow, _In_ DWORD secondHigh);

/* Register a whole LDT table via NtVdmControl service 11 (VdmSetProcessLdtInfo) --
   the bulk path ntvdm's SetShadowDescriptorEntries uses (fcn.0f0500c9). ServiceData
   is {ptr_to_{DWORD StartSel; DWORD LengthBytes; LDT_ENTRY entries[count]}, count}.
   `entries` is `count` pairs of {low,high} descriptor dwords. This is the step that
   makes the monitor load LDTR (svc 10 alone leaves PM selectors resolving base 0).
   Returns NTSTATUS (>= 0 ok). */
LONG VdmRegisterLdtTable(_In_ WORD startSelector, _In_reads_(2 * count) const DWORD *entries,
                         _In_ INT count);

#endif /* NTVDMEX_VDM_V86_H */
