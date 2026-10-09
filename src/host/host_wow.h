/* host_wow.h -- what host_wow.c offers the host's other files.
 *
 * Declarations only (#335): defined in host_wow.c. */
#ifndef NTVDMEX_HOST_WOW_H
#define NTVDMEX_HOST_WOW_H
#include "host_state.h"

PVOID ShimMapFlat(WORD segment, DWORD offset, INT isProtectedMode);
VOID WowShimsLoad(VOID);

/* Defined in host_wow.c (#335). */
INT WowDlgIsSelectorAbsent(WORD selector);
#endif
