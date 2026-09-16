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

/* THE TASK IS OVER, AND SO IS THE VDM. (s72) Stock ntvdm reports a program's
   exit code to CSRSS with GetNextVDMCommand (VDM_FLAG_DOS, ExitCode set, no
   FIRST_TASK) -- which is what releases the launcher waiting on the task -- and
   calls ExitVDM when it leaves a console. We did neither, so after our first
   program CSRSS still believed a VDM owned the console and the SECOND DOS program
   typed into the same cmd window was queued to a host that no longer existed and
   never ran (measured: fetch2.bat's "direct 2" produced no log at all). One
   task per host, then ExitVDM: the next launch in that console gets a fresh
   host. DONT_WAIT: if CSRSS already has a follow-up queued we are not going to
   run it, and a FALSE is fine. Returns what GetNextVDMCommand said, for the log. */
BOOL csrss_task_done(ULONG task_id, ULONG exit_code, DWORD *out_err, BOOL *out_exitvdm);
/* If csrss_task_done returned TRUE, CSRSS handed us the console's NEXT command
   (a program launched into this console before ExitVDM); these hold it. */
extern char csrss_next_app[1024], csrss_next_cmd[1024], csrss_next_cur[512];
extern HANDLE csrss_next_std[3];
/* ExitVDM(FALSE, 0): a DOS VDM leaving its console. Separate so a hang in either
   call names itself in the log. */
BOOL csrss_exit_vdm(void);

#endif /* VDM_CSRSS_H */
