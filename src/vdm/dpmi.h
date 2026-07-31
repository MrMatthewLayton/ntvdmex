/* dpmi.h -- DOS Protected Mode Interface host (M4 slice 3, spike).
 *
 * Reuses the kernel VDM monitor's protected-mode support (ADR-0004): the SAME
 * VdmStartExecution runs PM when the CONTEXT's EFLAGS.VM bit is clear and CS/SS/
 * DS/ES hold LDT selectors. See docs/research/dpmi-under-ntvdmcontrol.md for the
 * recovered mechanism (mode switch = VM bit at VTIB_EFLAGS+0x398; LDT install =
 * NtVdmControl service 10 with the NtSetLdtEntries 6-dword block).
 *
 * SPIKE STATUS: proving the real->PM switch round-trips. INT 2Fh AX=1687h is
 * served (so a client can detect + switch) but this is deliberately gated to the
 * spike and NOT yet a general DPMI advertisement -- the switch must be proven on
 * the real CPU first (see the research note's "do not advertise" gate).
 */
#ifndef VDM_DPMI_H
#define VDM_DPMI_H

#include <windows.h>

/* An LDT selector: (index<<3) | TI(=1,LDT) | RPL(=3, ring-3 client). */
#define DPMI_SEL(index)  (WORD)(((index) << 3) | 0x4 | 0x3)

/* Build the two dwords of a byte-granular 16-bit LDT descriptor for [base, +limit],
   with the given access byte (0xFA code exec/read DPL3, 0xF2 data r/w DPL3). */
void dpmi_build_desc(DWORD base, DWORD limit, BYTE access, DWORD *lo, DWORD *hi);

/* Perform the V86 -> protected-mode switch for a client that just FAR-CALLed the
   DPMI mode-switch entry (served for INT 2Fh AX=1687h). Reads the real-mode return
   frame off the guest stack, installs code+data LDT selectors based at the client's
   real-mode segments, and rewrites the CONTEXT to PM (VM clear, CS=code sel at the
   return offset, SS/DS/ES=data sel). Returns 0 on success, <0 if an LDT install
   failed. On success the caller must NOT advance EIP -- CS:IP were fully rewritten.
   `tib` is the VDM_TIB. `client_is_32bit` selects a 32-bit code/data selector. */
int dpmi_switch_to_pm(volatile BYTE *tib, int client_is_32bit);

#endif /* VDM_DPMI_H */
