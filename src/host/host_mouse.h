/* host_mouse.h -- what host_mouse.c offers the host's other files.
 *
 * Declarations only (#335): defined in host_mouse.c. */
#ifndef NTVDMEX_HOST_MOUSE_H
#define NTVDMEX_HOST_MOUSE_H
#include "host_state.h"

VOID HostMouseButton(INT button, INT down);

#endif
