/* host_dpmi.h -- what host_dpmi.c offers the host's other files.
 *
 * Declarations only (#335): defined in host_dpmi.c. */
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
#endif
