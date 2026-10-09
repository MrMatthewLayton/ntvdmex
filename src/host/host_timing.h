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
#endif
