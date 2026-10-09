/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * The CSRSS side of being a DOS VDM: register as the console VDM and
 * pull the program-to-run out of the VDM command queue. Ported from the spike;
 * contract + struct in ntvdm.h.
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#ifndef NTVDMEX_VDM_CSRSS_H
#define NTVDMEX_VDM_CSRSS_H

#include <windows.h>
#include "ntvdm.h"

/* The next command's buffers (see CsrssTaskDone). */
#define CSRSS_APP_NAME_SIZE         1024
#define CSRSS_COMMAND_LINE_SIZE     1024
#define CSRSS_DIRECTORY_SIZE        512
#define CSRSS_STANDARD_HANDLES      3   /* StdIn, StdOut, StdErr */
#define CSRSS_STD_IN                0
#define CSRSS_STD_OUT               1
#define CSRSS_STD_ERR               2

/* Parse the task id ntvdm's launcher passed as "-i<hex>" on our command line
 * (the last one wins). GetNextVDMCommand's first-command lookup keys on this
 * under IFEO, where our console handle differs from the launcher's.
 */
ULONG CsrssParseTaskId(_In_ PCSTR commandLine);

/* RegisterConsoleVDM(1, ...) -- register as the console VDM with CSRSS (the
 * association GetNextVDMCommand needs). Returns the BOOL result (FALSE if the
 * API is unavailable).
 */
BOOL CsrssRegisterConsole(VOID);

/* GetNextVDMCommand(commandInfo) -- fetch the next queued program. Returns the BOOL
 * result; *lastError receives GetLastError() when non-NULL.
 */
BOOL CsrssGetCommand(_Inout_ VDM_COMMAND_INFO *commandInfo, _Out_opt_ DWORD *lastError);

/* THE TASK IS OVER, AND SO IS THE VDM. (s72) Stock ntvdm reports a program's
 * exit code to CSRSS with GetNextVDMCommand (VDM_FLAG_DOS, ExitCode set, no
 * FIRST_TASK) -- which is what releases the launcher waiting on the task -- and
 * calls ExitVDM when it leaves a console. We did neither, so after our first
 * program CSRSS still believed a VDM owned the console and the SECOND DOS program
 * typed into the same cmd window was queued to a host that no longer existed and
 * never ran (measured: fetch2.bat's "direct 2" produced no log at all). One
 * task per host, then ExitVDM: the next launch in that console gets a fresh
 * host. DONT_WAIT: if CSRSS already has a follow-up queued we are not going to
 * run it, and a FALSE is fine. Returns what GetNextVDMCommand said, for the log.
 */
BOOL CsrssTaskDone(
    _In_ ULONG taskId,
    _In_ ULONG exitCode,
    _Out_opt_ DWORD *lastError,
    _Out_opt_ BOOL *didExitVdm);
/* If CsrssTaskDone returned TRUE, CSRSS handed us the console's NEXT command
 * (a program launched into this console before ExitVDM); these hold it.
 */
extern CHAR g_CsrssNextApp[CSRSS_APP_NAME_SIZE];
extern CHAR g_CsrssNextCommand[CSRSS_COMMAND_LINE_SIZE];
extern CHAR g_CsrssNextDirectory[CSRSS_DIRECTORY_SIZE];
extern HANDLE g_CsrssNextStandardHandles[CSRSS_STANDARD_HANDLES];
/* ExitVDM(FALSE, 0): a DOS VDM leaving its console. Separate so a hang in either
 * call names itself in the log.
 */
BOOL CsrssExitVdm(VOID);

#endif /* NTVDMEX_VDM_CSRSS_H */
