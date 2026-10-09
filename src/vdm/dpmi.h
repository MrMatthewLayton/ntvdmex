/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * DOS Protected Mode Interface host (M4 slice 3, spike).
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
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#ifndef NTVDMEX_VDM_DPMI_H
#define NTVDMEX_VDM_DPMI_H

#include <windows.h>
#include "../ntvdmex_x86.h"     /* defines only: X86_SELECTOR_INDEX_SHIFT */

/* Diagnostic snapshot of the last switch: {return CS, return linear, code desc lo, hi}. */
#define DPMI_DEBUG_RETURN_CS        0
#define DPMI_DEBUG_RETURN_LINEAR    1
#define DPMI_DEBUG_CODE_LOW         2
#define DPMI_DEBUG_CODE_HIGH        3
#define DPMI_DEBUG_ENTRIES          4
extern DWORD g_DpmiDebug[DPMI_DEBUG_ENTRIES];

/* Linear bases of the three initial selectors the switch installs, published so the
 * host can record them for dpmi_sel_base(): [0]=code (CS, sel 0x0F), [1]=data (DS,
 * sel 0x17), [2]=stack (SS, sel 0x1F). For a .COM these are equal; for a real .EXE
 * (CS!=DS!=SS) they diverge, which is why they are tracked per-selector.
 */
#define DPMI_INITIAL_CODE       0
#define DPMI_INITIAL_DATA       1
#define DPMI_INITIAL_STACK      2
#define DPMI_INITIAL_SELECTORS  3
extern DWORD g_DpmiSegmentBase[DPMI_INITIAL_SELECTORS];

/* Client width from the mode-switch AX bit0 (1 = 32-bit client, e.g. DOS/4GW). NOTE:
 * this must NOT set the D/B bit of the initial CS/DS/SS -- those stay 16-bit because
 * the client's post-switch code also has to run in real mode on the failure path.
 * See the note in DpmiSwitchToProtectedMode(). Kept for DPMI API register widths.
 */
extern INT g_DpmiIsClient32;

/* An LDT selector: (index<<3) | TI(=1,LDT) | RPL(=3, ring-3 client). */
#define DPMI_SELECTOR_TI_LDT    0x4
#define DPMI_SELECTOR_RPL3      0x3
#define DPMI_SELECTOR(index)    (WORD)(((index) << X86_SELECTOR_INDEX_SHIFT) | DPMI_SELECTOR_TI_LDT | DPMI_SELECTOR_RPL3)

/* Build the two dwords of an LDT descriptor for [base, +limit] with the given access
 * byte (0xFA code exec/read DPL3, 0xF2 data r/w DPL3) and flags nibble (bit3=G,
 * bit2=D/B: 0 => 16-bit byte-granular, 0x4 => 32-bit stack/data so ESP + exception
 * delivery work).
 */
VOID DpmiBuildDescriptor(
    _In_ DWORD base,
    _In_ DWORD limit,
    _In_ BYTE access,
    _In_ BYTE flags,
    _Out_ DWORD *descriptorLow,
    _Out_ DWORD *descriptorHigh);

/* Perform the V86 -> protected-mode switch for a client that just FAR-CALLed the
 * DPMI mode-switch entry (served for INT 2Fh AX=1687h). Reads the real-mode return
 * frame off the guest stack, installs code+data LDT selectors based at the client's
 * real-mode segments, and rewrites the CONTEXT to PM (VM clear, CS=code sel at the
 * return offset, SS/DS/ES=data sel). Returns 0 on success, <0 if an LDT install
 * failed. On success the caller must NOT advance EIP -- CS:IP were fully rewritten.
 * `tib` is the VDM_TIB. `isClient32` records the client's declared width.
 * *registerStatus / *setStatus receive the NtVdmControl status of the LDT register
 * (svc 11) and set-entries (svc 10) calls, for logging (pass NULL to ignore).
 */
INT DpmiSwitchToProtectedMode(
    _Inout_ volatile BYTE *tib,
    _In_ INT isClient32,
    _Out_opt_ LONG *registerStatus,
    _Out_opt_ LONG *setStatus);

/* Run the guest in protected mode directly in this process (via NtContinue), the way
 * ntvdm iret's into the client -- PM is not run by the kernel monitor. Does not return
 * on success; the PM guest's INT 31h / faults surface as Win32 exceptions (the VEH).
 * `tib` is the VDM_TIB holding the PM CONTEXT set up by DpmiSwitchToProtectedMode.
 */
VOID DpmiRunProtectedMode(_In_ volatile BYTE *tib);

/* Monitor PM-entry (src/vdm/dpmi_enter.S) -- runs the guest in PM the way ntvdm does
 * (0xf04483c) so a PM fault/INT is reflected by the kernel VDM trap handler as a
 * VTIB_EVENT, not a Win32 exception. Saves host state into the VDM_TIB host-save
 * CONTEXT, sets the [0x714] flag, loads the guest register file, and far-jmps in;
 * "returns" (via the kernel) once the guest stops, with VTIB_EVENT set -- like
 * VdmRunGuest(). Read VTIB_EVENT / the guest CONTEXT afterwards.
 */
VOID DpmiEnterProtectedMode(_Inout_ volatile BYTE *tib);

#endif /* NTVDMEX_VDM_DPMI_H */
