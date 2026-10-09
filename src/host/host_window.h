/* host_window.h -- what host_window.c offers the host's other files.
 *
 * Declarations only (#335): defined in host_window.c. */
#ifndef NTVDMEX_HOST_WINDOW_H
#define NTVDMEX_HOST_WINDOW_H
#include "host_state.h"

extern HHOOK g_LowLevelKeyboard;

extern DWORD g_PitDeliverSkipped;
extern DWORD g_Irq1AsyncRetry;
VOID HostRecordFinish(VOID);
INT OtherHostsRunning(VOID);
#endif
