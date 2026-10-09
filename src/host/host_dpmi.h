/* host_dpmi.h -- what host_dpmi.c offers the host's other files.
 *
 * Declarations only (#335): defined in host_dpmi.c. */
#ifndef NTVDMEX_HOST_DPMI_H
#define NTVDMEX_HOST_DPMI_H
#include "host_state.h"

INT DpmiSelectorIs32(WORD selector);
DWORD DpmiSelectorBase(WORD selector);

#endif
