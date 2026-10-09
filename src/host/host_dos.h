/* host_dos.h -- what host_dos.c offers the host's other files.
 *
 * Declarations only (#335): defined in host_dos.c. */
#ifndef NTVDMEX_HOST_DOS_H
#define NTVDMEX_HOST_DOS_H
#include "host_state.h"

PDOS_DISK_GEOMETRY DiskFor(UINT drive);
INT DiskIo(UINT drive, UINT32 lba, UINT count, BYTE *guest, INT write);
VOID HostSetFlags(volatile BYTE *tib, BYTE carryFlag, BYTE zeroFlag);
VOID HostXms(volatile BYTE *tib);
VOID HostEms(volatile BYTE *tib);

#endif
