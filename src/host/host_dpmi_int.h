/* host_dpmi_int.h -- what host_dpmi_int.c offers the host's other files.
 *
 * Declarations only (#335): defined in host_dpmi_int.c. */
#ifndef NTVDMEX_HOST_DPMI_INT_H
#define NTVDMEX_HOST_DPMI_INT_H
#include "host_state.h"

extern WORD g_WowLastId;
extern WORD g_WowLastFrom;

/* Defined in host_dpmi_int.c (#335). */
extern DWORD g_WowPspLinear[WOW_PSP_TRACK];
extern WORD g_WowPspEnvironment[WOW_PSP_TRACK];
extern DWORD g_WowSyncWrites;
#endif
