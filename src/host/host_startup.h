/* host_startup.h -- start-up: WinMain's steps -- configuring, registering the VDM, loading the program, building DOS, attaching the devices, starting the guest.
 *
 * Declarations only (#335): defined in host_startup.c. */
#ifndef NTVDMEX_HOST_STARTUP_H
#define NTVDMEX_HOST_STARTUP_H
#include "host_state.h"

INT StartupClaimInstance(INT *exitCodeOut);
INT StartupRunInstallVerb(INT *exitCodeOut);
VOID StartupStartGuest( PSTR *cursorIo, PSTR *baseIo, HANDLE *uiThreadIo, DOS_MACHINE *machine, DOS_IMAGE *image, volatile BYTE * const tib, CHAR *report);
VOID StartupConnectDosToHost(DOS_MACHINE *machine);
PSTR StartupStartServices(PSTR cursor);
VOID StartupLoadTuningKnobs(VOID);
PSTR StartupAttachDevices(PSTR cursor);
VOID StartupBuildDos( PSTR *cursorIo, const DWORD readCount, DOS_IMAGE *image, CHAR *programPathBuffer, CHAR *args, DOS_MACHINE *machine, CHAR *report);
VOID StartupLoadProgram( PSTR *cursorIo, DWORD *readCountIo, CHAR *programPathBuffer, CHAR *args, const INT wowCommandFromCsrss);
INT StartupRegisterVdm( PSTR *cursorIo, LONG *vdmStatusIo, INT *wowCommandFromCsrssIo, CHAR *args, CHAR *programPathBuffer, DWORD *readCountIo, CHAR *report, volatile BYTE * *tibIo, INT *exitCodeOut);
INT StartupConfigure(PSTR *cursorIo, CHAR *report, INT *exitCodeOut);

#endif
