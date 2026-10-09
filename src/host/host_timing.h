/* host_timing.h -- what host_timing.c offers the host's other files.
 *
 * Declarations only (#335): defined in host_timing.c. */
#ifndef NTVDMEX_HOST_TIMING_H
#define NTVDMEX_HOST_TIMING_H
#include "host_state.h"

UINT64 HostTimeMicroseconds(VOID);

VOID HostPitResyncCheck(VOID);
VOID HostPitSync(VOID);
VOID PitLatchNote(BYTE command);
VOID RetraceNote(volatile BYTE *tib, WORD port, INT isIn, DWORD cs, DWORD ipAfter);
VOID Irq0Latch(VOID);
INT PmTickTake(VOID);
VOID TickDeliveredNote(VOID);
extern LONGLONG g_Irq0TimePrevious;
extern LONGLONG g_Irq0Start;
extern DWORD g_Irq0AttemptsCount;
extern DWORD g_Irq0NieCount;
extern DWORD g_Irq0YieldCount;
extern DWORD g_Irq0RaiseCount;
extern DWORD g_IrqNInjected;
extern DWORD g_IrqNRefuseTotal;
INT Irq0CanDeliver(VOID);
VOID Irq0Ack(VOID);
#endif
