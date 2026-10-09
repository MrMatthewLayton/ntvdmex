/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * Installation and recovery: becoming the machine's VDM, reversibly; the recent
 *   list; the command line.
 *
 * Its own translation unit (#335): declared in host_install.h.
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#include "host_state.h"
#include "log.h"
#include <tlhelp32.h>
#include "install.h"
#include "host_install.h"

/* THE RECOVERY PATH: A MACHINE MUST NOT LOSE ITS VDM. (GH #132):
 * We install by pointing ntvdm.exe's IFEO Debugger value at ourselves, so a
 * host that wedges on startup breaks EVERY DOS and Win16 launch on the box,
 * and the fix needs a registry edit with no working VDM to make it from.
 * Session 52 wedged the rig twice in one day, both on a blocking Win32 call
 * before the host had a window; neither could be recovered remotely.
 * The counter is raised on every start and cleared THE MOMENT THE HOST HAS A
 * WINDOW (recovery_started, on the UI thread) -- that is the point past which the
 * s52 shape cannot happen, because a host with a window can be closed. Three
 * strikes and we drop the IFEO value: better to hand the machine back to
 * Microsoft's ntvdm than to leave it with no working 16-bit subsystem at all.
 *
 * [WARNING]: IT WAS "cleared only on a clean exit" UNTIL 2026-09-12, AND THAT UNINSTALLED
 * US ON A HEALTHY BOX: the X button ends in TerminateProcess (s63) and a guest
 * crash never exits cleanly either, so three ordinary play-tests were three
 * strikes and the user's next launch silently ran stock ntvdm for the rest of
 * the day. See src/dos/dos_recovery.h.
 */
#define STARTFAIL_PATH  OUT_("startfail.txt")
UINT RecoveryRead(VOID)
{
    CHAR buffer[16];
    DWORD bytesRead = 0;
    UINT value = 0;
    HANDLE handle = CreateFileA(STARTFAIL_PATH, GENERIC_READ, FILE_SHARE_READ, NULL,
                           OPEN_EXISTING, 0, NULL);

    if (handle == INVALID_HANDLE_VALUE)
        return 0;
    if (ReadFile(handle, buffer, sizeof(buffer) - 1, &bytesRead, NULL))
        value = DosRecoveryParseFailureCount(buffer, bytesRead);
    CloseHandle(handle);
    return value;
}

VOID RecoveryWrite(UINT value)
{
    CHAR buffer[16];
    INT index = 0;
    DWORD bytesWritten = 0;
    HANDLE handle = CreateFileA(STARTFAIL_PATH, GENERIC_WRITE, FILE_SHARE_READ, NULL,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);

    if (handle == INVALID_HANDLE_VALUE)
        return;
    if (value >= DECIMAL_RADIX)
        buffer[index++] = (CHAR)('0' + (value / DECIMAL_RADIX) % DECIMAL_RADIX);
    buffer[index++] = (CHAR)('0' + value % DECIMAL_RADIX);
    buffer[index++] = '\r';
    buffer[index++] = '\n';
    WriteFile(handle, buffer, (DWORD)index, &bytesWritten, NULL);
    FlushFileBuffers(handle);                /* the next start may be after a crash */
    CloseHandle(handle);
}

/* The start SUCCEEDED: the host has a window (or tray icon) on the desktop. Called
 * from the UI thread right after ShowWindow/TrayAdd, and again from the clean-exit
 * path so a headless run that never got that far still clears on its way out.
 */
VOID RecoveryOk(VOID)
{
    DeleteFileA(STARTFAIL_PATH);
}

/* THE INSTALLER. (GH #13) (Importance = 1):
 * Everything above install.h's line is decision-making with no Windows in it and
 * is tested off the VM; this is the half that touches the machine.
 *
 * [CAUTION]: HKLM, SO IT NEEDS ADMINISTRATOR. On XP the logged-in user usually is one, but
 * "usually" is not "always" and a failed write must say WHY rather than reporting
 * a success the machine did not perform -- that is the failure this whole feature
 * exists to remove. Every function here returns the Win32 error so the caller can.
 */

/* Read the current Debugger value. Returns 1 if a value was read (into buf). */
static INT InstallRead(PSTR buffer, DWORD cap)
{
    HKEY key;
    DWORD valueType = 0;
    DWORD size = cap;
    LONG status = RegOpenKeyExA(HKEY_LOCAL_MACHINE, INSTALL_KEY, 0, KEY_QUERY_VALUE, &key);

    buffer[0] = 0;
    if (status != ERROR_SUCCESS)
        return 0;
    status = RegQueryValueExA(key, INSTALL_VAL, NULL, &valueType, (LPBYTE)buffer, &size);
    RegCloseKey(key);
    if (status != ERROR_SUCCESS || valueType != REG_SZ)
    {
        buffer[0] = 0;
        return 0;
    }
    if (size >= cap)
        size = cap - 1;
    buffer[size] = 0;
    return 1;
}

/* The displaced value, kept beside our own settings. */
static INT InstallPreviousRead(PSTR buffer, DWORD cap)
{
    HKEY key;
    DWORD valueType = 0;
    DWORD size = cap;
    LONG status = RegOpenKeyExA(HKEY_CURRENT_USER, NTVDMEX_REG_KEY, 0, KEY_QUERY_VALUE, &key);

    buffer[0] = 0;
    if (status != ERROR_SUCCESS)
        return 0;
    status = RegQueryValueExA(key, INSTALL_PREV_VAL, NULL, &valueType, (LPBYTE)buffer, &size);
    RegCloseKey(key);
    if (status != ERROR_SUCCESS || valueType != REG_SZ || !buffer[0])
    {
        buffer[0] = 0;
        return 0;
    }
    if (size >= cap)
        size = cap - 1;
    buffer[size] = 0;
    return 1;
}

static VOID InstallPreviousWrite(PCSTR value)
{
    HKEY key;
    DWORD disposition;

    if (RegCreateKeyExA(HKEY_CURRENT_USER, NTVDMEX_REG_KEY, 0, NULL, 0,
                        KEY_SET_VALUE, NULL, &key, &disposition) != ERROR_SUCCESS)
        return;
    if (value && value[0])
    {
        DWORD length = 0;
        while (value[length])
            ++length;
        RegSetValueExA(key, INSTALL_PREV_VAL, 0, REG_SZ, (const BYTE *)value, length + 1);
    }
    else
    {
        RegDeleteValueA(key, INSTALL_PREV_VAL);
    }
    RegCloseKey(key);
}

INT MruLoad(CHAR out[MRU_MAX][MAX_PATH])
{
    HKEY key;
    INT count = 0;
    INT index;

    if (RegOpenKeyExA(HKEY_CURRENT_USER, NTVDMEX_REG_KEY, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS)
        return 0;
    for (index = 0; index < MRU_MAX; ++index)
    {
        CHAR name[16];
        CHAR *cursor = LogPut(name, HOST_REG_RECENT_PREFIX);
        DWORD valueType = 0;
        DWORD size = MAX_PATH;
        cursor = LogDecimal(cursor, (DWORD)(index + 1)); *cursor = 0;
        if (RegQueryValueExA(key, name, NULL, &valueType, (LPBYTE)out[count], &size) != ERROR_SUCCESS
            || valueType != REG_SZ || size < 2)
            continue;
        out[count][size < MAX_PATH ? size : MAX_PATH - 1] = 0;
        if (out[count][0])
            ++count;
    }
    RegCloseKey(key);
    return count;
}

VOID MruAdd(PCSTR path)
{
    CHAR list[MRU_MAX][MAX_PATH];
    CHAR longPath[MAX_PATH];
    HKEY key;
    DWORD disposition;
    DWORD longLength;
    INT count;
    INT index;
    INT written = 0;
    PCSTR baseName;

    if (!path || !path[0] || lstrlenA(path) >= MAX_PATH)
        return;
    /* CSRSS hands us 8.3 names; the menu is for a person. */
    longLength = GetLongPathNameA(path, longPath, sizeof longPath);
    if (longLength && longLength < sizeof longPath)
        path = longPath;
    for (baseName = path, index = 0; path[index]; ++index)
        if (path[index] == '\\')
            baseName = path + index + 1;
    if (!lstrcmpiA(baseName, LAUNCH_STUB_NAME) || !lstrcmpiA(baseName, HOST_HARNESS_STUB_NAME))
        return;                                                                                           /* ours */
    count = MruLoad(list);
    if (RegCreateKeyExA(HKEY_CURRENT_USER, NTVDMEX_REG_KEY, 0, NULL, 0,
                        KEY_SET_VALUE, NULL, &key, &disposition) != ERROR_SUCCESS)
        return;
    for (index = -1; index < count && written < MRU_MAX; ++index)
    {
        PCSTR value = (index < 0) ? path : list[index];
        CHAR name[16];
        CHAR *cursor = LogPut(name, HOST_REG_RECENT_PREFIX);
        if (index >= 0 && !lstrcmpiA(value, path))
            continue;                                                 /* moved to the front */
        cursor = LogDecimal(cursor, (DWORD)(++written)); *cursor = 0;
        RegSetValueExA(key, name, 0, REG_SZ, (const BYTE *)value, (DWORD)lstrlenA(value) + 1);
    }
    RegCloseKey(key);
}

/* Set (v non-NULL) or delete (v NULL) the IFEO Debugger value. Returns a Win32
 * error code; ERROR_SUCCESS means the machine now says what we asked it to.
 */
static LONG InstallWrite(PCSTR value)
{
    HKEY key;
    DWORD disposition;
    LONG status;

    status = RegCreateKeyExA(HKEY_LOCAL_MACHINE, INSTALL_KEY, 0, NULL, 0,
                         KEY_SET_VALUE, NULL, &key, &disposition);
    if (status != ERROR_SUCCESS)
        return status;
    if (value) { DWORD length = 0;
    while (value[length])
        ++length;
             status = RegSetValueExA(key, INSTALL_VAL, 0, REG_SZ, (const BYTE *)value, length + 1); }
    else   { status = RegDeleteValueA(key, INSTALL_VAL);
             if (status == ERROR_FILE_NOT_FOUND)
                 status = ERROR_SUCCESS; }
    RegCloseKey(key);
    return status;
}

/* Our own full path, which is what the value has to contain. */
static VOID InstallSelfPath(PSTR buffer, DWORD cap)
{
    if (!GetModuleFileNameA(NULL, buffer, cap))
        buffer[0] = 0;
}

/* HOW MANY OF WINDOWS' OWN ntvdm.exe ARE RUNNING RIGHT NOW:
 * The IFEO Debugger value is consulted when a NEW ntvdm.exe is created and never
 * again, and XP does not create one per program: every Win16 program shares ONE
 * resident WOW VDM that outlives the program that started it, and a DOS program
 * run from a console reuses that console's VDM. So a stock ntvdm.exe that is alive
 * when /install runs keeps taking every launch it would have taken anyway, and the
 * key we just wrote routes nothing until it exits -- or the box reboots.
 * - Field report, 2026-09-18 (a friend's machine): the tester ran Notepad, Paint,
 *   WinMine and Solitaire under stock ntvdm FIRST, to have something to compare
 *   against, then installed NTVDMEX. Every Win16 launch after that still drew under
 *   stock; a reboot "fixed" it. That is exactly the sequence an alpha tester follows,
 *   and nothing in the install told them. It has to.
 * Counts by image name from the process list; our own host is ntvdmhost.exe, so it
 * is never mistaken for one. -1 when the list cannot be read at all, so the caller
 * can say "could not tell" rather than "none".
 */
static INT InstallResidentVdms(VOID)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32 entry;
    INT count = 0;

    if (snap == INVALID_HANDLE_VALUE)
        return -1;
    entry.dwSize = sizeof entry;
    if (Process32First(snap, &entry))
    {
        do
        {
            static const CHAR want[] = HOST_STOCK_NTVDM_NAME;
            INT index;
            for (index = 0; want[index]; ++index)
            {
                CHAR character = entry.szExeFile[index];
                if (character >= 'A' && character <= 'Z')
                    character = (CHAR)(character - 'A' + 'a');
                if (character != want[index])
                    break;
            }
            if (!want[index] && !entry.szExeFile[index])
                ++count;
        } while (Process32Next(snap, &entry));
    }
    CloseHandle(snap);
    return count;
}

/* The sentence that goes with a non-zero count, for /install and /status alike. */
static PSTR InstallResidentText(PSTR cursor, INT count)
{
    if (count == 0)
        return cursor;
    if (count < 0) return LogPut(cursor, "(Could not read the process list, so whether one of "
                              "Windows' own ntvdm.exe is still running is unknown.)\r\n");
    cursor = LogPut(cursor, "\r\n!! ");
    cursor = LogDecimal(cursor, (UINT)count);
    cursor = LogPut(cursor, count == 1 ? " copy of Windows' own ntvdm.exe is still running."
                       : " copies of Windows' own ntvdm.exe are still running.");
    cursor = LogPut(cursor, "\r\n   Programs that are already running, and 16-bit Windows programs "
                "started\r\n   from now on, keep using it: Windows only asks for NTVDMEX "
                "when it starts a\r\n   NEW ntvdm.exe. Close every MS-DOS and 16-bit "
                "Windows program (or reboot),\r\n   then run status.bat -- it should "
                "no longer print this.\r\n");
    return cursor;
}

/* DO IT, AND REPORT WHAT ACTUALLY HAPPENED:
 * `want` is 1 to install, 0 to uninstall. The message is composed here rather
 * than by the caller so the same words are used from the command line and from
 * the menu -- and so a refusal names the program it is refusing to disturb.
 *
 * [INFO]: IT VERIFIES BY READING BACK. This project has been bitten more than once by a
 * registry value that reads correctly and does not route, and by a write that
 * silently did nothing; reporting success on the strength of a return code alone
 * is the same class of claim. Read it again and classify it again.
 */
INT InstallPerform(INT want, INT force, PSTR message, DWORD cap)
{
    CHAR self[NTVDMEX_PATH_MAX];
    CHAR current[NTVDMEX_PATH_MAX];
    CHAR prev[NTVDMEX_PATH_MAX];
    CHAR currentBefore[NTVDMEX_PATH_MAX];
    PSTR cursor = message;
    INSTALL_STATE state;
    INSTALL_STATE oldState;
    INSTALL_ACTION installAction;
    LONG status = ERROR_SUCCESS;
    INT havePrevious;

    (VOID)cap;

    InstallSelfPath(self, sizeof self);
    InstallRead(current, sizeof current);
    havePrevious = InstallPreviousRead(prev, sizeof prev);
    /* #195, measured on the rig: installing from copy B saved copy A (bin\) as "the
     * value to restore", so uninstalling from A "restored" A itself and then failed its
     * own read-back. A saved value that is another NTVDMEX is not somebody else's
     * setting to give back -- ignore it (and never save one, below).
     */
    if (havePrevious && InstallNamesNtvdmex(prev))
        havePrevious = 0;
    state  = InstallClassify(current[0] ? current : NULL, self);
    oldState = state; LogPut(currentBefore, current);                      /* what was there, for the report */
    installAction = InstallPlanEx(state, want, havePrevious, InstallNamesNtvdmex(current), force);

    switch (installAction)
    {
    case INSTALL_ACT_NOTHING:
        cursor = LogPut(cursor, want ? "NTVDMEX is already installed as this machine's VDM.\r\n"
                         : "NTVDMEX is not installed; nothing to remove.\r\n");
        break;

    case INSTALL_ACT_REFUSE:
        cursor = LogPut(cursor, "REFUSED: the ntvdm.exe Debugger value points at another "
                    "program, not at NTVDMEX:\r\n    ");
        cursor = LogPut(cursor, current);
        cursor = LogPut(cursor, "\r\nRemoving it would break whatever that is. Nothing changed.\r\n"
                    "If you are sure it should go, run:  ntvdmhost.exe /uninstall /force\r\n");
        return 0;

    case INSTALL_ACT_WRITE:
        /* Save what we are about to displace, so uninstall can put it back. Only
         * when it is somebody else's -- overwriting our own path with our own path
         * must not record US as the thing to restore.
         */
        if (state == INSTALL_OTHER && !InstallNamesNtvdmex(current))
            InstallPreviousWrite(current);
        status = InstallWrite(self);
        break;

    case INSTALL_ACT_RESTORE:
        status = InstallWrite(prev);
        if (status == ERROR_SUCCESS)
            InstallPreviousWrite(NULL);
        break;

    case INSTALL_ACT_DELETE:
        status = InstallWrite(NULL);
        break;
    }

    if (status != ERROR_SUCCESS)
    {
        cursor = LogPut(cursor, status == ERROR_ACCESS_DENIED
            ? "FAILED: access denied writing HKEY_LOCAL_MACHINE.\r\n"
              "Installing changes a machine-wide setting, so it needs an "
              "Administrator account.\r\n"
            : "FAILED: could not write the registry (error 0x");
        if (status != ERROR_ACCESS_DENIED)
        {
            cursor = LogHex(cursor, (DWORD)status);
            cursor = LogPut(cursor, ").\r\n");
        }
        return 0;
    }
    if (installAction == INSTALL_ACT_NOTHING)
        return 1;

    /* -- THE READ-BACK. */
    InstallRead(current, sizeof current);
    state = InstallClassify(current[0] ? current : NULL, self);
    if (want && state != INSTALL_OURS)
    {
        cursor = LogPut(cursor, "FAILED: the value was written but does not read back as ours.\r\n");
        return 0;
    }
    if (!want && state == INSTALL_OURS)
    {
        cursor = LogPut(cursor, "FAILED: the value was removed but still reads back as ours.\r\n");
        return 0;
    }
    if (want)
    {
        cursor = LogPut(cursor, "INSTALLED. Every MS-DOS and 16-bit Windows launch on this "
                    "machine now runs through NTVDMEX:\r\n    ");
        cursor = LogPut(cursor, self);
        cursor = LogPut(cursor, "\r\nUninstall with:  ntvdmhost.exe /uninstall\r\n");
        cursor = InstallResidentText(cursor, InstallResidentVdms());
    }
    else
    {
        cursor = LogPut(cursor, installAction == INSTALL_ACT_RESTORE
            ? "UNINSTALLED, and the Debugger value we displaced has been put back:\r\n    "
            : "UNINSTALLED. This machine uses its own ntvdm.exe again.\r\n");
        if (installAction == INSTALL_ACT_RESTORE)
        {
            cursor = LogPut(cursor, current);
            cursor = LogPut(cursor, "\r\n");
        }
        if (oldState == INSTALL_OTHER)                   /* #195: say what we removed */
        {
            cursor = LogPut(cursor, "Removed a Debugger value that named ");
            cursor = LogPut(cursor, InstallNamesNtvdmex(currentBefore) ? "another copy of NTVDMEX:\r\n    "
                                                    : "another program (/force):\r\n    ");
            cursor = LogPut(cursor, currentBefore); cursor = LogPut(cursor, "\r\n");
        }
    }
    return 1;
}

/* Where we stand right now, in words, for `/status` and for the menu.
 * RETURNS the state too, because a caller that must ACT on it should never have to
 * read the prose. package/smoke.bat did exactly that -- it grepped /status for
 * "installed as this machine", a sentence only /install ever prints -- so it
 * declared NTVDMEX uninstalled the moment after install.bat said otherwise. Two
 * layers that have to agree about a string, don't. (s72, found by hand on the rig.)
 */
INSTALL_STATE InstallStatusText(PSTR message, DWORD cap)
{
    CHAR self[NTVDMEX_PATH_MAX];
    CHAR current[NTVDMEX_PATH_MAX];
    PSTR cursor = message;
    INSTALL_STATE state;

    (VOID)cap;
    InstallSelfPath(self, sizeof self);
    InstallRead(current, sizeof current);
    state = InstallClassify(current[0] ? current : NULL, self);
    cursor = LogPut(cursor, "This executable:\r\n    "); cursor = LogPut(cursor, self); cursor = LogPut(cursor, "\r\n\r\n");
    switch (state)
    {
    case INSTALL_OURS:
        cursor = LogPut(cursor, "INSTALLED -- MS-DOS and 16-bit Windows launches on this machine "
                    "run through NTVDMEX.\r\n");
        cursor = InstallResidentText(cursor, InstallResidentVdms());
        break;

    case INSTALL_OTHER:
        cursor = LogPut(cursor, "NOT INSTALLED, and the ntvdm.exe Debugger value belongs to "
                    "another program:\r\n    ");
        cursor = LogPut(cursor, current);
        cursor = LogPut(cursor, "\r\n");
        break;

    default:
        cursor = LogPut(cursor, "NOT INSTALLED -- this machine uses its own ntvdm.exe.\r\n");
        break;
    }
    return state;
}

/* Which verb, if any, this command line asks for: 0 install, 1 uninstall,
 * 2 status, -1 none. The verb must be the FIRST argument -- see the call site.
 */
INT InstallVerb(PCSTR command)
{
    static PCSTR const verbs[INSTALL_VERBS] = { "install", "uninstall", "status" };
    INT index;
    INT characterIndex;
    if (!command)
        return INSTALL_VERB_NONE;
    /* Step over argv[0], quoted or not. */
    if (*command == '"')
    {
        ++command;
        while (*command && *command != '"')
            ++command;
        if (*command)
            ++command;
    }
    else
    {
        while (*command && *command != ' ' && *command != '\t')
            ++command;
    }
    while (*command == ' ' || *command == '\t') ++command;
    if (*command != '/' && *command != '-')
        return INSTALL_VERB_NONE;
    while (*command == '/' || *command == '-')
        ++command;
    for (index = 0; index < INSTALL_VERBS; ++index)
    {
        for (characterIndex = 0; verbs[index][characterIndex]; ++characterIndex)
        {
            CHAR character = command[characterIndex];
            if (character >= 'A' && character <= 'Z')
                character = (CHAR)(character - 'A' + 'a');
            if (character != verbs[index][characterIndex])
                break;
        }
        if (!verbs[index][characterIndex] && (!command[characterIndex] || command[characterIndex] == ' ' || command[characterIndex] == '\t'))
            return index;
    }
    return INSTALL_VERB_NONE;
}

/* `/force` anywhere after the verb (#195): /uninstall /force removes a value that names
 * another program. Deliberately only a command-line switch, never a menu item.
 */
INT CommandLineHasForce(PCSTR command)
{
    PCSTR cursor;

    if (!command)
        return 0;
    for (cursor = command; *cursor; ++cursor)
        if ((*cursor == '/' || *cursor == '-') && (cursor[1]|ASCII_CASE_BIT) == 'f' && (cursor[2]|ASCII_CASE_BIT) == 'o'
            && (cursor[3]|ASCII_CASE_BIT) == 'r' && (cursor[4]|ASCII_CASE_BIT) == 'c' && (cursor[5]|ASCII_CASE_BIT) == 'e'
            && (!cursor[6] || cursor[6] == ' ' || cursor[6] == '\t' || cursor[6] == '"'))
            return 1;
    return 0;
}

/* Does this command line carry NOTHING after argv[0]? That is the double-click / Start
 * menu shape, and it is safe to test for: Windows hands an IFEO-substituted VDM the
 * ORIGINAL command line, whose first argument is always the path to ntvdm.exe, so a
 * real VDM launch always has arguments. Same reasoning InstallVerb() already relies on.
 */
INT CommandLineBare(PCSTR command)
{
    if (!command)
        return 0;
    if (*command == '"')
    {
        ++command;
        while (*command && *command != '"')
            ++command;
        if (*command)
            ++command;
    }
    else
    {
        while (*command && *command != ' ' && *command != '\t')
            ++command;
    }
    while (*command == ' ' || *command == '\t') ++command;
    return *command == 0;
}

/* stdout if we have one, a message box if we do not. */
/* "I WANT TO OPEN NTVDMEX AND SEE IT." (Importance = 3):
 * Write the four-byte DOS stub and run it, so CSRSS builds a VDM that the IFEO key
 * hands back to us WITH the privilege this process cannot have. See LAUNCH_STUB_NAME
 * for why it cannot be done directly -- measured, `NtVdmControl` -> 0xC0000022.
 *
 * [CAUTION]: REFUSE LOUDLY IF WE ARE NOT INSTALLED. Without the IFEO key the stub runs under
 * STOCK ntvdm and the user gets *a* DOS box -- someone else's -- which is the most
 * confusing possible outcome: it looks like it worked. Checking first costs one
 * registry read. `InstallStatusText` is the same check `/status` reports.
 *
 * [CAUTION]: %TEMP%, not the install directory: a per-user path we can always write, on a
 * product that may be installed read-only under Program Files. Rewritten every time,
 * so a truncated or tampered stub cannot persist.
 * Returns a process exit code.
 */
INT LaunchShellVdm(VOID)
{
    CHAR stub[MAX_PATH + 32];
    CHAR message[1024];
    DWORD length;
    DWORD bytesWritten = 0;
    HANDLE handle;
    STARTUPINFOA startupInfo;
    PROCESS_INFORMATION processInfo;

    {   INSTALL_STATE state = InstallStatusText(message, sizeof message);
        if (state != INSTALL_OURS)
        {
            /* The status text says WHICH of the two it is; add what to do about it. */
#define HOST_NOT_THE_VDM_TEXT "\r\n\r\nNTVDMEX has to be the machine's VDM before it can open a " \
                 "DOS session of its own.\r\n\r\nRun:    ntvdmhost.exe /install\r\n" \
                 "(as an administrator), then try again."
            LogPut(message + lstrlenA(message), HOST_NOT_THE_VDM_TEXT);
            InstallReport(message, FALSE);
            return 1;
        } }

    length = GetTempPathA(MAX_PATH, stub);
    if (!length || length > MAX_PATH)
    {
        LogPut(stub, HOST_DEFAULT_DRIVE_ROOT);
        length = 3;
    }
    if (stub[length - 1] != '\\')
    {
        stub[length++] = '\\';
        stub[length] = 0;
    }
    LogPut(stub + length, LAUNCH_STUB_NAME);

    handle = CreateFileA(stub, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                    FILE_ATTRIBUTE_NORMAL, NULL);
    if (handle == INVALID_HANDLE_VALUE)
    {
        LogPut(message, HOST_STUB_WRITE_FAILED_TEXT);
        LogPut(message + lstrlenA(message), stub);
        InstallReport(message, FALSE);
        return 1;
    }
    {   static const BYTE exitStub[] = { X86_OP_MOV_AH_IMM, DOS_FN_EXIT, X86_OP_INT, VECTOR_DOS };   /* mov ah,4Ch; int 21h */
        BOOL isWritten = WriteFile(handle, exitStub, sizeof exitStub, &bytesWritten, NULL);
        CloseHandle(handle);
        /* [CAUTION]: A SHORT WRITE IS NOT A SUCCESS. A truncated stub is not a DOS image and
         * CreateProcess would report something unrelated to the real cause.
         */
        if (!isWritten || bytesWritten != sizeof exitStub)
        {
            LogPut(message, HOST_STUB_INCOMPLETE_TEXT);
            LogPut(message + lstrlenA(message), stub);
            InstallReport(message, FALSE);
            return 1;
        } }

    {
        INT index;
        for (index = 0; index < (INT)sizeof startupInfo; ++index)
            ((PSTR)&startupInfo)[index] = 0;
    }
    startupInfo.cb = sizeof startupInfo;
    if (!CreateProcessA(stub, NULL, NULL, NULL, FALSE, 0, NULL, NULL, &startupInfo, &processInfo))
    {
        LogPut(message, HOST_SESSION_START_FAILED_TEXT);
        LogPut(message + lstrlenA(message), stub);
        LogPut(message + lstrlenA(message), HOST_SESSION_ERROR_TEXT);
        {
            PSTR end = message + lstrlenA(message);
            end = LogHex(end, GetLastError());
            *end = 0;
        }
        InstallReport(message, FALSE);
        return 1;
    }
    CloseHandle(processInfo.hThread);
    CloseHandle(processInfo.hProcess);
    /* [CAUTION]: We exit immediately and deliberately. The VDM is a SEPARATE process and owns
     * the window; waiting here would leave a pointless second process alive for the
     * whole session and make the launcher look like the thing that hung.
     */
    return 0;
}

VOID InstallReport(PCSTR message, INT isOk)
{
    HANDLE handle = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD valueType = (handle && handle != INVALID_HANDLE_VALUE) ? GetFileType(handle) : FILE_TYPE_UNKNOWN;

    if (valueType != FILE_TYPE_UNKNOWN)
    {
        DWORD length = 0;
        DWORD bytesWritten;
        while (message[length])
            ++length;
        WriteFile(handle, message, length, &bytesWritten, NULL);
        return;
    }
    MessageBoxA(NULL, message, HOST_PRODUCT_NAME,
                MB_OK | (isOk ? MB_ICONINFORMATION : MB_ICONERROR));
}

/* Take ourselves out of the launch path. Needs the privilege the installer had;
 * if it fails, SAY SO -- a recovery step that silently does nothing is worse
 * than none, because the next start believes it was handled.
 */
VOID RecoveryUninstall(PSTR *logCursor)
{
    HKEY key;
    LONG status;

    status = RegOpenKeyExA(HKEY_LOCAL_MACHINE,
        INSTALL_KEY,
        0, KEY_SET_VALUE, &key);
    if (status == ERROR_SUCCESS)
    {
        status = RegDeleteValueA(key, INSTALL_VAL);
        RegCloseKey(key);
    }
    *logCursor = LogPut(*logCursor, status == ERROR_SUCCESS
        ? "STAGE0: RECOVERY -- IFEO Debugger REMOVED, the machine's own ntvdm "
          "takes over. Re-install when the fault is fixed. (GH #132)\r\n"
        : "STAGE0: RECOVERY -- could NOT remove the IFEO Debugger value (no "
          "privilege?); the machine still routes here. (GH #132)\r\n");
}

/* WIN16 / WOW PASSTHROUGH (GH #129):
 * See the call site at the top of WinMain for the measured launch shapes. Two
 * helpers: one decides, one hands off. They live away from the DOS machinery
 * because they run before ANY of it is initialised.
 */
/* Step over argv[0] (which Windows may have quoted) and return the rest. Under an
 * IFEO Debugger hook argv[0] is OUR exe, and what follows is the ORIGINAL command
 * line, starting with the quoted path of the program really being launched.
 */
PCSTR CommandLineAfterArgv0(PCSTR cursor)
{
    if (*cursor == '"')
    {
        ++cursor;
        while (*cursor && *cursor != '"')
            ++cursor;
        if (*cursor)
            ++cursor;
    }
    else
    {
        while (*cursor && *cursor != ' ')
            ++cursor;
    }
    while (*cursor == ' ') ++cursor;
    return cursor;
}
