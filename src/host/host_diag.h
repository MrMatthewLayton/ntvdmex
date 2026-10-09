/* host_diag.h -- what host_diag.c offers the host's other files.
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
#endif
