/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * Protected mode: the DPMI host -- descriptors and the LDT, fault trampolines,
 *   code patching and breakpoints, callbacks, PM IRQ injection, client teardown.
 *
 * Declarations only (#335): defined in host_dpmi.c.
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#ifndef NTVDMEX_HOST_DPMI_H
#define NTVDMEX_HOST_DPMI_H
#include "host_state.h"

INT DpmiSelectorIs32(WORD selector);
DWORD DpmiSelectorBase(WORD selector);

INT DpmiAsyncInjectPm(UINT irq, CONTEXT *context);
WORD DpmiSegmentToDescriptor(WORD segment);
INT DpmiSelectorDescriptor(WORD selector, UINT32 *accessRights, UINT32 *limit);
INT DpmiServicePmInt(DOS_MACHINE *machine, volatile BYTE *tib, DWORD vector, UINT steps);
/* Defined in host_dpmi.c (#335). */
extern WORD g_PmTransferParagraphs;
INT DpmiHostIndex(VOID);
VOID DpmiInstall(INT index);
VOID DpmiArmFaultTrampoline(volatile BYTE *tib, WORD flag);
DWORD DpmiBopVector(DWORD csValue, DWORD eip);
DWORD DpmiPmEip(volatile BYTE *tib);
INT DpmiNestedFault(volatile BYTE *tib, DWORD event, DWORD eip);
INT DpmiInjectPmIrq(DOS_MACHINE *machine, volatile BYTE *tib, UINT interruptVector, UINT steps);
extern UINT g_DpmiCpMaximum;
extern DWORD g_BreakpointDump[DPMI_BP_MAX];
extern DWORD g_BreakpointSkip[DPMI_BP_MAX];
extern DWORD g_BreakpointMode[DPMI_BP_MAX];
extern BYTE g_BreakpointPending[DPMI_BP_MAX];
extern DWORD g_BreakpointReport[DPMI_BP_MAX];
extern INT g_DpmiBlockCount;
extern DWORD g_DpmiOwned[DPMI_OWNED_MAX];
extern INT g_DpmiOwnedCount;
extern INT g_LeCodeCount;
extern WORD g_PmDefaultSelector;
extern WORD g_LdtFree[DPMI_LDT_MAX];
extern INT g_LdtFreeCount;
extern INT g_PmWatchCount;
extern DWORD g_PmCooperativeLine[PIC_LINES_PER_CHIP];
extern INT g_HostPoolSpill;
extern WORD g_DpmiHandlerSelector;
WORD DpmiHandlerCodeSelector(VOID);
VOID DpmiSegmentToDescriptorForget(WORD selector);
VOID DpmiInstallDefaultPmHandlers(DOS_MACHINE *machine);
VOID DpmiInstallFaultTrampoline(VOID);
DWORD DpmiRecoverFlatEip(DWORD lo16, BYTE vector, INT *candidateCount);
VOID DpmiPatchCodeRegion(DWORD base, DWORD limit, INT is32BitRegion);
VOID DpmiLeLearn(const BYTE *buffer, DWORD length);
VOID DpmiScanCodeBlocks(VOID);
VOID DpmiBreakpointLoad(VOID);
VOID DpmiBreakpointResolveCodeBase(DWORD base);
VOID DpmiBreakpointResolveSegment(UINT segmentNumber, DWORD base);
VOID DpmiBreakpointArm(VOID);
VOID DpmiBreakpointRearmPending(DWORD currentLinear);
INT DpmiBreakpointDisarm(DWORD linear);
VOID DpmiUnpatch(VOID);
VOID DpmiRepatch(VOID);
VOID DpmiInvokeCallback(DOS_MACHINE *machine, volatile BYTE *tib, INT slot);
extern BYTE g_PmDispatch[IVT_VECTORS];
extern DWORD g_PmDispatchCount[IVT_VECTORS][BYTE_VALUES];
INT DpmiDispatchToPmHandler(DOS_MACHINE *machine, volatile BYTE *tib, DWORD vector, UINT steps);
DWORD DpmiCallerOffset(volatile BYTE *tib, DWORD offset);
DWORD DpmiRmcsPointer(volatile BYTE *tib, DWORD esBase);
VOID RmcsToTib(volatile BYTE *tib, const RMCS_REGS *registers);
VOID TibToRmcs(volatile BYTE *tib, RMCS_REGS *registers, WORD flags);
VOID DpmiRmcsProbe(volatile BYTE *tib, DWORD esBase, UINT slot, DWORD interruptNumber);
INT DpmiOwnedFind(DWORD handle);
VOID DpmiLdtRelease(INT index);
INT DpmiLdtTake(VOID);
INT DpmiClientSelectorOk(WORD selector);
PSTR PmInt21Transfer(DOS_MACHINE *machine, volatile BYTE *tib, DWORD ah, PSTR cursor);
PSTR PmInt21Lfn(DOS_MACHINE *machine, volatile BYTE *tib, PSTR cursor);
INT DpmiReflectIrqToRm(DOS_MACHINE *machine, volatile BYTE *tib, UINT vector);
VOID DpmiEnsurePmReturnSelector(VOID);
INT DpmiInjectPmMouseCallback(DOS_MACHINE *machine, volatile BYTE *tib, UINT steps);
VOID DpmiClientTeardown(VOID);
#endif
