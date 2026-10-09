/* host_diag.h -- crash handling and diagnostics: the fatal dump, the PM-fault handler, the
 *   watchdog, the end-of-run reports and the probe loaders.
 *
 * Declarations only (#335): defined in host_diag.c. */
#ifndef NTVDMEX_HOST_DIAG_H
#define NTVDMEX_HOST_DIAG_H
#include "host_state.h"

extern LONG g_IfvTraceCount;
extern DWORD g_IfvReenter[PIC_LINES];
extern DWORD g_AsyncEarlyBailLogged;

VOID AsyncWhyReport(VOID);
VOID IfvReport(VOID);
/* Defined in host_diag.c (#335). */
VOID DsProbeLoad(VOID);
extern INT g_PmVehPass;
extern volatile LONG g_DpmiWatchdogGeneration;
extern volatile DWORD g_DpmiEnterCs;
extern volatile DWORD g_DpmiEnterEip;
extern volatile DWORD g_DpmiLastEvent;
extern volatile DWORD g_DpmiLastVector;
LONG CALLBACK DpmiCrashVeh(EXCEPTION_POINTERS *pointers);
LONG WINAPI HostUnhandledFilter(EXCEPTION_POINTERS *pointers);
DWORD WINAPI DpmiWatchdog(LPVOID param);
#endif
