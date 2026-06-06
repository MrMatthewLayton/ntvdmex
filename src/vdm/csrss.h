/* csrss.h -- the CSRSS side of being a DOS VDM: register as the console VDM and
 * pull the program-to-run out of the VDM command queue. Ported from the spike;
 * contract + struct in ntvdm.h.
 */
#ifndef VDM_CSRSS_H
#define VDM_CSRSS_H

#include <windows.h>
#include "ntvdm.h"

/* Parse the task id ntvdm's launcher passed as "-i<hex>" on our command line
   (the last one wins). GetNextVDMCommand's first-command lookup keys on this
   under IFEO, where our console handle differs from the launcher's. */
ULONG csrss_parse_taskid(const char *cmdline);

/* RegisterConsoleVDM(1, ...) -- register as the console VDM with CSRSS (the
   association GetNextVDMCommand needs). Returns the BOOL result (FALSE if the
   API is unavailable). */
BOOL csrss_register_console(void);

/* GetNextVDMCommand(ci) -- fetch the next queued program. Returns the BOOL
   result; *out_err receives GetLastError() when non-NULL. */
BOOL csrss_get_command(VDM_COMMAND_INFO *ci, DWORD *out_err);

#endif /* VDM_CSRSS_H */
