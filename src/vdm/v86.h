/* v86.h -- drive XP's kernel VDM machinery to run real-mode code in Virtual-8086
 * mode. Wraps the undocumented NtVdmControl path (memory map, VdmInitialize, the
 * self-allocated VDM_TIB, the entry CONTEXT, and the VdmStartExecution/event loop).
 * Ported from tools/vdmhost/vdmhost.c; the contract lives in ntvdm.h.
 *
 * Sequence the host uses: v86_setup_memory() -> v86_init() -> v86_get_tib(),
 * then build the DOS process in low memory, v86_set_entry(), and loop on v86_run().
 */
#ifndef VDM_V86_H
#define VDM_V86_H

#include <windows.h>
#include "ntvdm.h"

/* TEB self-pointer (fs:[0x18]) without the CRT/winternl. */
void *v86_teb(void);

/* Lay down the V86 low-memory address space (the 4-section map covering the full
   640KB) -- must run before v86_init or VdmInitialize access-violates. Returns the
   final NtMapViewOfSection status (>= 0 == success). */
LONG v86_setup_memory(void);

/* NtVdmControl(VdmInitialize): register this process as a VDM with the kernel so
   GetNextVDMCommand works and the guest can run. Returns NTSTATUS (>= 0 ok), or 1
   if ntdll!NtVdmControl is unavailable. Caches the entry point for v86_run(). */
LONG v86_init(void);

/* Find a free 64KB UMA hole and map the EMS page frame there as V86 RAM. Must run
   AFTER v86_init(): pre-mapping a section view in the UMA makes VdmInitialize fail
   with STATUS_UNABLE_TO_FREE_VM (it requires that range free during init). After
   init only part of the UMA is free (e.g. E0000 has just 32KB), so we scan the
   conventional page-frame segments for a hole big enough. Returns the linear base
   of the mapped 64KB frame (0 on failure). The guest learns the segment via EMS
   INT 67h AH=41, so any hole works. */
DWORD v86_map_ems_frame(void);

/* Return the VDM_TIB (TEB+0xF18). The kernel does not allocate it; if absent we
   allocate our own static TIB, initialise the fields ntvdm sets, and register it.
   Returns NULL only if the TEB is unreadable. */
volatile BYTE *v86_get_tib(void);

/* Write the entry V86 CONTEXT into the TIB: CS:IP, SS:SP, DS=ES=FS=GS=psp_seg,
   general registers 0, EFlags = VM, full-context ContextFlags. */
void v86_set_entry(volatile BYTE *tib, WORD cs, WORD ip, WORD ss, WORD sp,
                   WORD psp_seg);

/* Run the guest (VdmStartExecution) until the next stop; returns the event code
   (VDM_EVENT_BOP for a serviceable BOP). *out_status gets the NtVdmControl status
   if non-NULL. Requires v86_init() to have cached NtVdmControl. Runs whichever mode
   the CONTEXT's EFLAGS.VM bit selects: VM=1 -> V86, VM=0 + LDT selectors -> PM
   (recovered from ntvdm fcn.0f00532e; see research/dpmi-under-ntvdmcontrol.md). */
DWORD v86_run(volatile BYTE *tib, LONG *out_status);

/* The kernel's virtual PIC (see the ICA_* offsets + rationale in ntvdm.h). Raising a
   line here is what makes NtVdmControl(VdmQueueInterrupt) able to deliver: the APC it
   queues asks the ICA which vector to inject. Raise, then set VDM_INT_HARDWARE in
   FIXED_NTVDMSTATE, then queue. The ISR bit stays set until v86_ica_eoi(), so the
   guest's EOI write must reach us or that line never fires again. */
void  v86_ica_set_base(unsigned base);  /* experiment: distinguish kernel vs host delivery */
void  v86_ica_raise(unsigned irq);
void  v86_ica_eoi(unsigned irq);
void  v86_ica_set_mask(unsigned irq, int masked);
DWORD v86_ica_state(unsigned irq);   /* IRR | ISR<<8 | IMR<<16, for logging */

/* Raw NtVdmControl passthrough (for DPMI's LDT + PM-cli services). Returns NTSTATUS
   (>= 0 ok). Requires v86_init() to have cached the entry point. */
LONG v86_vdmcontrol(ULONG service, PVOID data);

/* Install up to two LDT descriptors via NtVdmControl service 10 (VdmSetLdtEntries),
   whose ServiceData is exactly the NtSetLdtEntries 6-dword block
   {Sel0,Entry0Low,Entry0Hi,Sel1,Entry1Low,Entry1Hi} (recovered from fcn.0f050100).
   Pass sel1=0 to set only one. Returns NTSTATUS (>= 0 ok). */
LONG v86_set_ldt_entries(WORD sel0, DWORD e0lo, DWORD e0hi,
                         WORD sel1, DWORD e1lo, DWORD e1hi);

/* Register a whole LDT table via NtVdmControl service 11 (VdmSetProcessLdtInfo) --
   the bulk path ntvdm's SetShadowDescriptorEntries uses (fcn.0f0500c9). ServiceData
   is {ptr_to_{DWORD StartSel; DWORD LengthBytes; LDT_ENTRY entries[count]}, count}.
   `entries` is `count` pairs of {low,high} descriptor dwords. This is the step that
   makes the monitor load LDTR (svc 10 alone leaves PM selectors resolving base 0).
   Returns NTSTATUS (>= 0 ok). */
LONG v86_set_process_ldt_info(WORD start_sel, const DWORD *entries, int count);

#endif /* VDM_V86_H */
