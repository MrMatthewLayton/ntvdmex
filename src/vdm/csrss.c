/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * See csrss.h. Faithful port from tools/vdmhost/vdmhost.c.
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#include "csrss.h"

#define CSRSS_KERNEL32_NAME         "kernel32.dll"
#define CSRSS_REGISTER_CONSOLE_VDM  "RegisterConsoleVDM"
#define CSRSS_GET_NEXT_VDM_COMMAND  "GetNextVDMCommand"
#define CSRSS_EXIT_VDM              "ExitVDM"

/* The task-id switch the launcher passes: "-i<hex>" (either case). */
#define CSRSS_SWITCH_CHAR           '-'
#define CSRSS_TASK_ID_LOWER         'i'
#define CSRSS_TASK_ID_UPPER         'I'
#define CSRSS_TASK_ID_FIRST_DIGIT   2       /* The digits follow "-i" */
#define CSRSS_SPACE                 ' '
#define CSRSS_HEX_RADIX             16
#define CSRSS_LOWER_CASE_BIT        0x20    /* ASCII: OR folds A-F onto a-f */
#define CSRSS_HEX_LETTER_VALUE      10      /* 'a' = 10 */
#define CSRSS_NO_TASK_ID            0

ULONG CsrssParseTaskId(PCSTR commandLine)
{
    PCSTR cursor = commandLine;
    ULONG taskId = CSRSS_NO_TASK_ID;

    while (*cursor)
    {
        if (cursor[0] == CSRSS_SWITCH_CHAR && (cursor[1] == CSRSS_TASK_ID_LOWER || cursor[1] == CSRSS_TASK_ID_UPPER))
        {
            PCSTR digit = cursor + CSRSS_TASK_ID_FIRST_DIGIT;
            taskId = CSRSS_NO_TASK_ID;
            while (*digit == CSRSS_SPACE)
                ++digit;
            for (;;)
            {
                CHAR character = *digit;
                if (character >= '0' && character <= '9')
                    taskId = taskId * CSRSS_HEX_RADIX + (ULONG)(character - '0');
                else if ((character | CSRSS_LOWER_CASE_BIT) >= 'a' && (character | CSRSS_LOWER_CASE_BIT) <= 'f')
                    taskId = taskId * CSRSS_HEX_RADIX + (ULONG)((character | CSRSS_LOWER_CASE_BIT) - 'a' + CSRSS_HEX_LETTER_VALUE);
                else
                    break;
                ++digit;
            }
        }
        ++cursor;
    }
    return taskId;          /* last -i<n> on the line */
}

/* RegisterConsoleVDM's arguments on the DOS path: flag 1, no video-state buffer/size. */
#define CSRSS_CONSOLE_VDM_DOS   1
#define CSRSS_NO_VIDEO_STATE    0

BOOL CsrssRegisterConsole(VOID)
{
    PFN_RegisterConsoleVDM RegisterConsoleVDM =
        (PFN_RegisterConsoleVDM)GetProcAddress(
            GetModuleHandleA(CSRSS_KERNEL32_NAME), CSRSS_REGISTER_CONSOLE_VDM);
    HANDLE startEvent;
    HANDLE endEvent;
    HANDLE errorEvent;
    DWORD sixthOut = 0;
    DWORD tenthOut = 0;
    PVOID seventhOut = NULL;
    PVOID eleventhOut = NULL;
    if (!RegisterConsoleVDM)
        return FALSE;
    startEvent = CreateEventA(NULL, TRUE, FALSE, NULL);
    endEvent   = CreateEventA(NULL, TRUE, FALSE, NULL);
    errorEvent = CreateEventA(NULL, TRUE, FALSE, NULL);
    /* DOS path: flag 1, no video-state buffer/size (args 8,9 = 0). */
    return RegisterConsoleVDM(CSRSS_CONSOLE_VDM_DOS, startEvent, endEvent, errorEvent, CSRSS_NO_VIDEO_STATE,
                              &sixthOut, &seventhOut, CSRSS_NO_VIDEO_STATE, CSRSS_NO_VIDEO_STATE, &tenthOut, &eleventhOut);
}

BOOL CsrssGetCommand(VDM_COMMAND_INFO *commandInfo, DWORD *lastError)
{
    PFN_GetNextVDMCommand GetNextVDMCommand =
        (PFN_GetNextVDMCommand)GetProcAddress(
            GetModuleHandleA(CSRSS_KERNEL32_NAME), CSRSS_GET_NEXT_VDM_COMMAND);
    BOOL succeeded;
    if (!GetNextVDMCommand)
    {
        if (lastError)
            *lastError = ERROR_PROC_NOT_FOUND;
        return FALSE;
    }
    succeeded = GetNextVDMCommand(commandInfo);
    if (lastError)
        *lastError = GetLastError();
    return succeeded;
}

CHAR g_CsrssNextApp[CSRSS_APP_NAME_SIZE];
CHAR g_CsrssNextCommand[CSRSS_COMMAND_LINE_SIZE];
CHAR g_CsrssNextDirectory[CSRSS_DIRECTORY_SIZE];
HANDLE g_CsrssNextStandardHandles[CSRSS_STANDARD_HANDLES];   /* the next command's StdIn/StdOut/StdErr as CSRSS placed them
                               in THIS process (s73: handed to the relaunched host) */

/* The other buffers GetNextVDMCommand fills in for the next command. */
#define CSRSS_PIF_SIZE          512
#define CSRSS_ENVIRONMENT_SIZE  8192
#define CSRSS_DESKTOP_SIZE      512
#define CSRSS_TITLE_SIZE        512
#define CSRSS_RESERVED_SIZE     512
#define CSRSS_EXIT_VDM_FLAGS    0   /* ExitVDM's second argument */
#define CSRSS_END_OF_STRING     0
#define CSRSS_CARRIAGE_RETURN   '\r'
#define CSRSS_LINE_FEED         '\n'
#define CSRSS_LAST_CHAR_OFFSET  1

BOOL CsrssTaskDone(ULONG taskId, ULONG exitCode, DWORD *lastError, BOOL *didExitVdm)
{
    typedef BOOL (WINAPI *PFN_ExitVDM)(BOOL, ULONG);
    HMODULE kernel32 = GetModuleHandleA(CSRSS_KERNEL32_NAME);
    PFN_GetNextVDMCommand GetNextVDMCommand = (PFN_GetNextVDMCommand)GetProcAddress(kernel32, CSRSS_GET_NEXT_VDM_COMMAND);
    PFN_ExitVDM ExitVDM = (PFN_ExitVDM)GetProcAddress(kernel32, CSRSS_EXIT_VDM);
    static CHAR pifFile[CSRSS_PIF_SIZE];
    static CHAR environment[CSRSS_ENVIRONMENT_SIZE];
    static CHAR desktop[CSRSS_DESKTOP_SIZE];
    static CHAR title[CSRSS_TITLE_SIZE];
    static CHAR reserved[CSRSS_RESERVED_SIZE];
    VDM_COMMAND_INFO commandInfo;
    BOOL succeeded = FALSE;
    INT charIndex;
    if (didExitVdm)
        *didExitVdm = FALSE;
    if (!GetNextVDMCommand)
    {
        if (lastError)
            *lastError = ERROR_PROC_NOT_FOUND;
        return FALSE;
    }
    ZeroMemory(&commandInfo, sizeof commandInfo);
    g_CsrssNextApp[0] = g_CsrssNextCommand[0] = g_CsrssNextDirectory[0] = CSRSS_END_OF_STRING;
    g_CsrssNextStandardHandles[CSRSS_STD_IN] = g_CsrssNextStandardHandles[CSRSS_STD_OUT] = g_CsrssNextStandardHandles[CSRSS_STD_ERR] = NULL;
    commandInfo.CmdLine = g_CsrssNextCommand;
    commandInfo.CmdLen = sizeof g_CsrssNextCommand;
    commandInfo.AppName = g_CsrssNextApp;
    commandInfo.AppLen = sizeof g_CsrssNextApp;
    commandInfo.PifFile = pifFile;
    commandInfo.PifLen = sizeof pifFile;
    commandInfo.CurDirectory = g_CsrssNextDirectory;
    commandInfo.CurDirectoryLen = sizeof g_CsrssNextDirectory;
    commandInfo.Env = environment;
    commandInfo.EnvLen = sizeof environment;
    commandInfo.Desktop = desktop;
    commandInfo.DesktopLen = sizeof desktop;
    commandInfo.Title = title;
    commandInfo.TitleLen = sizeof title;
    commandInfo.Reserved = reserved;
    commandInfo.ReservedLen = sizeof reserved;
    commandInfo.StartupInfo.cb = sizeof(STARTUPINFOA);
    commandInfo.TaskId = taskId;
    commandInfo.ExitCode = exitCode;
    /* VDM_FLAG_DOS, exactly as stock ntvdm's cmdGetNextCmd reports (reverse/ntvdm.exe
     * 0xf00ac1e: `or byte [VDMState], 4`, ExitCode from the DOS block). This call
     * releases the launcher and then WAITS for the console's next command -- there
     * is no non-blocking form of the report (DONT_WAIT was tried: it blocked too).
     */
    commandInfo.VDMState = VDM_FLAG_DOS;
    succeeded = GetNextVDMCommand(&commandInfo);
    if (lastError)
        *lastError = GetLastError();
    g_CsrssNextApp[sizeof g_CsrssNextApp - CSRSS_LAST_CHAR_OFFSET] = CSRSS_END_OF_STRING;
    g_CsrssNextCommand[sizeof g_CsrssNextCommand - CSRSS_LAST_CHAR_OFFSET] = CSRSS_END_OF_STRING;
    if (succeeded)
    {
        g_CsrssNextStandardHandles[CSRSS_STD_IN] = commandInfo.StdIn;
        g_CsrssNextStandardHandles[CSRSS_STD_OUT] = commandInfo.StdOut;
        g_CsrssNextStandardHandles[CSRSS_STD_ERR] = commandInfo.StdErr;
    }
    for (charIndex = 0; g_CsrssNextCommand[charIndex]; ++charIndex) if (g_CsrssNextCommand[charIndex] == CSRSS_CARRIAGE_RETURN || g_CsrssNextCommand[charIndex] == CSRSS_LINE_FEED)
    {
        g_CsrssNextCommand[charIndex] = CSRSS_END_OF_STRING;
        break;
    }
    if (ExitVDM && didExitVdm)
        *didExitVdm = ExitVDM(FALSE, CSRSS_EXIT_VDM_FLAGS);
    return succeeded;
}

BOOL CsrssExitVdm(VOID)
{
    typedef BOOL (WINAPI *PFN_ExitVDM)(BOOL, ULONG);
    PFN_ExitVDM ExitVDM = (PFN_ExitVDM)GetProcAddress(GetModuleHandleA(CSRSS_KERNEL32_NAME), CSRSS_EXIT_VDM);
    if (!ExitVDM)
    {
        SetLastError(ERROR_PROC_NOT_FOUND);
        return FALSE;
    }
    return ExitVDM(FALSE, CSRSS_EXIT_VDM_FLAGS);
}
