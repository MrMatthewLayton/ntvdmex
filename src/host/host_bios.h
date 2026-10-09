/* host_bios.h -- what host_bios.c offers the host's other files.
 *
 * Declarations only (#335): defined in host_bios.c. */
#ifndef NTVDMEX_HOST_BIOS_H
#define NTVDMEX_HOST_BIOS_H
#include "host_state.h"

VOID SerialOut(PCSTR buffer, PCSTR end);

#endif
