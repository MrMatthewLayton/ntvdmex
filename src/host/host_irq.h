/* host_irq.h -- interrupt injection: the asynchronous IRQ path into V86 and protected mode,
 *   IF/VIF, and the host's IRQ sink.
 *
 * Declarations only (#335): defined in host_irq.c. */
#ifndef NTVDMEX_HOST_IRQ_H
#define NTVDMEX_HOST_IRQ_H
#include "host_state.h"

extern DWORD g_Irq1AsyncInjected;
VOID SkipIfSiteNote(DWORD codeSegment, DWORD instructionPointer, DWORD stub);
UINT IrqPmVector(UINT irq);
extern DWORD g_IrqRaisedAny;
extern DWORD g_QiBits;
extern INT g_QiKeysAsync;
extern volatile LONG g_AsyncContextWrite;
extern DWORD g_QiCalls;
extern volatile LONG g_QiStatus;
extern DWORD g_AsyncNestBlocked;
extern DWORD g_IfvCensus[IFV_PATHS][8];
extern DWORD g_IfvShadow[PIC_LINES];
extern DWORD g_IfvStarveCount;
extern DWORD g_IfvStarveT0;
extern DWORD g_IfvStarveMaximumMs;
extern INT g_IfvStarveOpen;
VOID IfvNote(INT path, DWORD flags);
extern DWORD g_AsyncPmInjected;
extern DWORD g_AsyncInjectedLine[PIC_LINES];
extern DWORD g_PmWatch[DPMI_WATCH_MAX];
extern BYTE g_PmWatchRel[DPMI_WATCH_MAX];
DWORD PmWatchAddress(INT index);
extern DWORD g_PmInjectDecl[2];
extern DWORD g_PmInjectDeclTl[IRQ0TL_SECS];
VOID PmInjectDeclineNote(INT why, WORD cs, DWORD eip);
extern INT g_AsyncSiteCount;
extern INT g_AsyncSiteFull;
INT AsyncInjectIrq(UINT irq);
INT AsyncVectorIsOurStub(UINT irq);
VOID HostIrqSink(PVOID context, BYTE irq);
INT IfOrVif(DWORD flags);
INT IsOurStubCsIp(DWORD cs, DWORD ip);
VOID VdmStateSample(PCSTR label, volatile BYTE *tib, INT *budget);
VOID InjectInt(volatile BYTE *tib, UINT vector);
DWORD WINAPI QueueIrqProbeThread(LPVOID parameter);
INT V86DeliverDeviceIrq(volatile BYTE *tib);

#endif
