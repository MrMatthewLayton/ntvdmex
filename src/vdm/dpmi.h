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

/* Diagnostic snapshot of the last switch: {ret_cs, ret_ip, code desc lo, hi}. */
extern DWORD g_dpmi_dbg[4];

/* An LDT selector: (index<<3) | TI(=1,LDT) | RPL(=3, ring-3 client). */
#define DPMI_SEL(index)  (WORD)(((index) << 3) | 0x4 | 0x3)

/* Build the two dwords of an LDT descriptor for [base, +limit] with the given access
   byte (0xFA code exec/read DPL3, 0xF2 data r/w DPL3) and flags nibble (bit3=G,
   bit2=D/B: 0 => 16-bit byte-granular, 0x4 => 32-bit stack/data so ESP + exception
   delivery work). */
void dpmi_build_desc(DWORD base, DWORD limit, BYTE access, BYTE flags,
                     DWORD *lo, DWORD *hi);

/* Perform the V86 -> protected-mode switch for a client that just FAR-CALLed the
   DPMI mode-switch entry (served for INT 2Fh AX=1687h). Reads the real-mode return
   frame off the guest stack, installs code+data LDT selectors based at the client's
   real-mode segments, and rewrites the CONTEXT to PM (VM clear, CS=code sel at the
   return offset, SS/DS/ES=data sel). Returns 0 on success, <0 if an LDT install
   failed. On success the caller must NOT advance EIP -- CS:IP were fully rewritten.
   `tib` is the VDM_TIB. `client_is_32bit` selects a 32-bit code/data selector.
   *reg_st / *set_st receive the NtVdmControl status of the LDT register (svc 11) and
   set-entries (svc 10) calls, for logging (pass NULL to ignore). */
int dpmi_switch_to_pm(volatile BYTE *tib, int client_is_32bit,
                      LONG *reg_st, LONG *set_st);

/* Run the guest in protected mode directly in this process (via NtContinue), the way
   ntvdm iret's into the client -- PM is not run by the kernel monitor. Does not return
   on success; the PM guest's INT 31h / faults surface as Win32 exceptions (the VEH).
   `tib` is the VDM_TIB holding the PM CONTEXT set up by dpmi_switch_to_pm. */
void dpmi_run_pm(volatile BYTE *tib);

/* Monitor PM-entry (src/vdm/dpmi_enter.S) -- runs the guest in PM the way ntvdm does
   (0xf04483c) so a PM fault/INT is reflected by the kernel VDM trap handler as a
   VTIB_EVENT, not a Win32 exception. Saves host state into the VDM_TIB host-save
   CONTEXT, sets the [0x714] flag, loads the guest register file, and far-jmps in;
   "returns" (via the kernel) once the guest stops, with VTIB_EVENT set -- like
   v86_run(). Read VTIB_EVENT / the guest CONTEXT afterwards. */
void dpmi_enter_pm(volatile BYTE *tib);

#endif /* VDM_DPMI_H */
