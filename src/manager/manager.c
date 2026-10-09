/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * Ntvdmex.exe, the NTVDMEX manager. GH #281, session 88.
 *
 * ONE tray icon for every running DOS and Win16 program. The user's design:
 * 1. No NTVDMEX process at boot.
 * 2. The first program to start (DOS or Win16) brings this up; it adds the icon.
 * 3-5. Every further program -- each in its own ntvdmhost.exe -- appears as an
 *      entry in the icon's menu.
 * 6. When the last one closes, this exits; the next program starts it again.
 *
 * WHY A SEPARATE PROCESS AND NOT ONE HOST FOR EVERYTHING:
 * NT gives a process ONE V86 machine: the guest's first megabyte lives at address
 * 0 of the process, and a DPMI guest's selectors live in the process's one LDT.
 * Two DOS programs cannot share a process, which is why stock XP gives each its own
 * ntvdm.exe. So the hosts stay separate and this coordinates them. (Sharing ONE
 * host between Win16 programs is a different and deferred question -- #280.)
 *
 * WHAT IT KNOWS AND HOW:
 * Hosts announce themselves every couple of seconds (src/host/mgrproto.h). This
 * holds a SYNCHRONIZE handle on each and drops the entry when it signals -- a
 * crashed host sends no goodbye, so the handle is the only reliable end. It is not
 * the hosts' parent (Explorer starts them, through the IFEO key) and does not need
 * to be.
 *
 * [CAUTION]: NO C RUNTIME, like every executable in this project (see CMakeLists.txt):
 * Win32 calls and src/runtime.c only.
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#include <windows.h>
#include <shellapi.h>
#include "../host/mgrproto.h"

#define WM_MGRTRAY              (WM_APP + 1)
#define TRAY_ID                 1
#define IDI_MAINICON            101
#define MGR_MAX                 48      /* MsgWaitForMultipleObjects allows 63 */
#define EMPTY_GRACE_MS          4000    /* A program that is just starting gets time */
#define FIRST_HELLO_MS          15000   /* Started, but nobody ever announced: go */
#define MGR_POLL_MS             500     /* How often the list and the clock are checked */

/* Menu ids: globals below 100; per-program ones are 1000 + slot*8 + action. */
#define IDM_G_SETTINGS          1
#define IDM_G_ABOUT             2
#define IDM_G_EXITALL           3
#define IDM_P_BASE              1000
#define IDM_P_PER_SLOT          8
#define IDM_P_SHOW              0
#define IDM_P_CLOSE             2

/* What the user reads. */
#define MGR_TIP_SIZE            64
#define MGR_TIP_ONE_PROGRAM     "NTVDMEX - %s"
#define MGR_TIP_PROGRAMS        "NTVDMEX - %d programs"
#define MGR_EMPTY_STRING        ""
#define MGR_MENU_SHOW           "Show"
#define MGR_MENU_CLOSE_PROGRAM  "Close Program"
#define MGR_MENU_STARTING       "(starting)"
#define MGR_MENU_NO_PROGRAMS    "(no programs running)"
#define MGR_MENU_SETTINGS       "Settings..."
#define MGR_MENU_ABOUT          "About"
#define MGR_MENU_EXIT_ALL       "Exit All"
#define MGR_ABOUT_TITLE         "NTVDMEX#NTVDMEX -- New Technology Virtual DOS Manager, Extended"
#define MGR_ABOUT_TEXT          "One icon for every running DOS and 16-bit Windows program."
#define MGR_WINDOW_TITLE        "NTVDMEX"
#define MGR_TASKBAR_CREATED     "TaskbarCreated"

#define MGR_NO_SLOT             (-1)
#define MGR_NO_MENU_FLAGS       0
#define MGR_NO_MENU_ID          0
#define MGR_EXIT_OK             0
#define MGR_EXIT_FAILED         1
#define MGR_END_OF_STRING       0
#define MGR_ONE_PROGRAM         1
#define MGR_DRAIN_ALL           0       /* PeekMessage's min/max filter: everything */

typedef struct _MGR_SESSION
{
    BOOL   IsUsed;
    DWORD  ProcessId;
    DWORD  Kind;
    HWND CommandWindow;
    HWND ShowTargetWindow;
    HANDLE Process;
    DWORD  Sequence;                  /* registration order -- the menu's order */
    CHAR   Name[MGR_NAME_SIZE];
} MGR_SESSION, *PMGR_SESSION;

static MGR_SESSION g_Sessions[MGR_MAX];
static DWORD  g_Sequence;
static HWND   g_Window;
static UINT g_CommandMessage;
static UINT g_TaskbarCreatedMessage;
static BOOL   g_HasTrayIcon;
static HICON  g_Icon;

static INT MgrSessionCount(VOID)
{
    INT slot;
    INT count = 0;

    for (slot = 0; slot < MGR_MAX; ++slot)
        if (g_Sessions[slot].IsUsed)
            ++count;
    return count;
}

static VOID MgrTrayTip(NOTIFYICONDATAA *notifyData)
{
    INT count = MgrSessionCount();
    CHAR tip[MGR_TIP_SIZE];

    if (count == MGR_ONE_PROGRAM)
    {
        INT slot;
        for (slot = 0; slot < MGR_MAX && !g_Sessions[slot].IsUsed; ++slot) ;
        wsprintfA(tip, MGR_TIP_ONE_PROGRAM, slot < MGR_MAX ? g_Sessions[slot].Name : MGR_EMPTY_STRING);
    }
    else
    {
        wsprintfA(tip, MGR_TIP_PROGRAMS, count);
    }
    lstrcpynA(notifyData->szTip, tip, sizeof notifyData->szTip);
}

static VOID MgrTraySet(DWORD operation)
{
    NOTIFYICONDATAA notifyData;

    ZeroMemory(&notifyData, sizeof notifyData);
    notifyData.cbSize = sizeof notifyData;
    notifyData.hWnd = g_Window;
    notifyData.uID = TRAY_ID;
    if (operation != NIM_DELETE)
    {
        notifyData.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
        notifyData.uCallbackMessage = WM_MGRTRAY;
        notifyData.hIcon = g_Icon;
        MgrTrayTip(&notifyData);
    }
    if (Shell_NotifyIconA(operation, &notifyData))
    {
        if (operation == NIM_ADD)
            g_HasTrayIcon = TRUE;
        if (operation == NIM_DELETE)
            g_HasTrayIcon = FALSE;
    }
    else if (operation == NIM_MODIFY && g_HasTrayIcon)
    {
        /* Explorer lost it without telling us (it was restarted): put it back. */
        g_HasTrayIcon = FALSE;
        MgrTraySet(NIM_ADD);
    }
}

static VOID MgrSessionDrop(INT slot)
{
    if (!g_Sessions[slot].IsUsed)
        return;
    if (g_Sessions[slot].Process)
        CloseHandle(g_Sessions[slot].Process);
    ZeroMemory(&g_Sessions[slot], sizeof g_Sessions[slot]);
    if (g_HasTrayIcon)
        MgrTraySet(NIM_MODIFY);
}

/* Upsert by pid. Returns TRUE if accepted. */
static BOOL MgrSessionHello(PCMGR_MESSAGE message)
{
    INT slot;
    INT freeSlot = MGR_NO_SLOT;
    BOOL hasChanged = FALSE;

    for (slot = 0; slot < MGR_MAX; ++slot)
    {
        if (g_Sessions[slot].IsUsed && g_Sessions[slot].ProcessId == message->ProcessId)
            break;
        if (!g_Sessions[slot].IsUsed && freeSlot < 0)
            freeSlot = slot;
    }
    if (slot == MGR_MAX)
    {
        HANDLE process;
        if (freeSlot < 0)
            return FALSE;                                     /* full */
        process = OpenProcess(SYNCHRONIZE, FALSE, message->ProcessId);
        if (!process)
            return FALSE;                                    /* it is already gone */
        slot = freeSlot;
        g_Sessions[slot].IsUsed = TRUE;
        g_Sessions[slot].ProcessId = message->ProcessId;
        g_Sessions[slot].Process = process;
        g_Sessions[slot].Sequence = ++g_Sequence;
        hasChanged = TRUE;
    }
    g_Sessions[slot].Kind = message->Kind;
    g_Sessions[slot].CommandWindow  = (HWND)(ULONG_PTR)message->CommandWindow;
    g_Sessions[slot].ShowTargetWindow = (HWND)(ULONG_PTR)message->ShowTargetWindow;
    if (lstrcmpA(g_Sessions[slot].Name, message->Name) != 0)
    {
        lstrcpynA(g_Sessions[slot].Name, message->Name, MGR_NAME_SIZE);
        hasChanged = TRUE;
    }
    if (!g_HasTrayIcon)
        MgrTraySet(NIM_ADD);
    else if (hasChanged)
        MgrTraySet(NIM_MODIFY);
    return TRUE;
}

static VOID MgrSessionCommand(INT slot, WPARAM command)
{
    if (slot < 0 || slot >= MGR_MAX || !g_Sessions[slot].IsUsed)
        return;
    if (command == MGR_COMMAND_SHOW && g_Sessions[slot].ShowTargetWindow && IsWindow(g_Sessions[slot].ShowTargetWindow))
    {
        if (IsIconic(g_Sessions[slot].ShowTargetWindow))
            ShowWindow(g_Sessions[slot].ShowTargetWindow, SW_RESTORE);
        SetForegroundWindow(g_Sessions[slot].ShowTargetWindow);
        return;
    }
    if (g_Sessions[slot].CommandWindow && IsWindow(g_Sessions[slot].CommandWindow))
        PostMessageA(g_Sessions[slot].CommandWindow, g_CommandMessage, command, 0);
}

/* The menu, in the order programs started. */
static VOID MgrTrayMenu(VOID)
{
    HMENU menu = CreatePopupMenu();
    POINT cursor;
    INT order[MGR_MAX];
    INT count = 0;
    INT slot;
    INT position;
    INT menuId;

    if (!menu)
        return;
    for (slot = 0; slot < MGR_MAX; ++slot)
        if (g_Sessions[slot].IsUsed)
            order[count++] = slot;
    for (slot = 1; slot < count; ++slot)                         /* insertion sort by sequence */
        for (position = slot; position > 0 && g_Sessions[order[position - 1]].Sequence > g_Sessions[order[position]].Sequence; --position)
        {
            INT swapped = order[position];
            order[position] = order[position - 1];
            order[position - 1] = swapped;
        }
    for (position = 0; position < count; ++position)
    {
        HMENU submenu = CreatePopupMenu();
        slot = order[position];
        if (!submenu)
            continue;
        menuId = IDM_P_BASE + slot * IDM_P_PER_SLOT;
        /* No per-program Settings (user, s88): settings are global, so they live
         * once, below, and not in every submenu.
         */
        AppendMenuA(submenu, MF_STRING, menuId + IDM_P_SHOW,     MGR_MENU_SHOW);
        AppendMenuA(submenu, MF_STRING, menuId + IDM_P_CLOSE,    MGR_MENU_CLOSE_PROGRAM);
        AppendMenuA(menu, MF_POPUP | MF_STRING, (UINT_PTR)submenu,
                    g_Sessions[slot].Name[0] ? g_Sessions[slot].Name : MGR_MENU_STARTING);
    }
    if (!count)
        AppendMenuA(menu, MF_STRING | MF_GRAYED, MGR_NO_MENU_ID, MGR_MENU_NO_PROGRAMS);
    AppendMenuA(menu, MF_SEPARATOR, MGR_NO_MENU_ID, NULL);
    AppendMenuA(menu, MF_STRING | (count ? MGR_NO_MENU_FLAGS : MF_GRAYED), IDM_G_SETTINGS, MGR_MENU_SETTINGS);
    AppendMenuA(menu, MF_STRING, IDM_G_ABOUT, MGR_MENU_ABOUT);
    AppendMenuA(menu, MF_STRING | (count ? MGR_NO_MENU_FLAGS : MF_GRAYED), IDM_G_EXITALL, MGR_MENU_EXIT_ALL);
    GetCursorPos(&cursor);
    /* [CAUTION]: The documented tray dance: foreground first, a stray post after, or the
     * menu does not dismiss when you click away from it.
     */
    SetForegroundWindow(g_Window);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON, cursor.x, cursor.y, 0, g_Window, NULL);
    PostMessageA(g_Window, WM_NULL, 0, 0);
    DestroyMenu(menu);                                 /* destroys the submenus too */
}

static VOID MgrOnCommand(UINT commandId)
{
    INT slot;

    if (commandId >= IDM_P_BASE)
    {
        slot = (INT)(commandId - IDM_P_BASE) / IDM_P_PER_SLOT;
        switch ((commandId - IDM_P_BASE) % IDM_P_PER_SLOT)
        {
        case IDM_P_SHOW:
            MgrSessionCommand(slot, MGR_COMMAND_SHOW);
        break;

        case IDM_P_CLOSE:
            MgrSessionCommand(slot, MGR_COMMAND_CLOSE_PROGRAM);
        break;
        }
        return;
    }
    switch (commandId)
    {
    case IDM_G_SETTINGS:
        /* Settings are machine-wide (registry + share file); any host's dialog
         * edits the same values, so the oldest program's is as good as any.
         */
        {   INT oldest = MGR_NO_SLOT;
            for (slot = 0; slot < MGR_MAX; ++slot)
                if (g_Sessions[slot].IsUsed && (oldest < 0 || g_Sessions[slot].Sequence < g_Sessions[oldest].Sequence))
                    oldest = slot;
            MgrSessionCommand(oldest, MGR_COMMAND_SETTINGS); }
        break;

    case IDM_G_ABOUT:
        ShellAboutA(g_Window, MGR_ABOUT_TITLE,
                    MGR_ABOUT_TEXT, g_Icon);
        break;

    case IDM_G_EXITALL:
        for (slot = 0; slot < MGR_MAX; ++slot)
            if (g_Sessions[slot].IsUsed)
                MgrSessionCommand(slot, MGR_COMMAND_EXIT);
        break;
    }
}

static LRESULT CALLBACK MgrWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (message == g_TaskbarCreatedMessage && g_TaskbarCreatedMessage)     /* Explorer restarted */
    {
        g_HasTrayIcon = FALSE;
        if (MgrSessionCount())
            MgrTraySet(NIM_ADD);
        return 0;
    }
    switch (message)
    {
    case WM_COPYDATA:
    {
        const COPYDATASTRUCT *copyData = (const COPYDATASTRUCT *)lParam;
        MGR_MESSAGE received;
        if (!copyData || copyData->dwData != MGR_MAGIC || copyData->cbData < sizeof received || !copyData->lpData)
            return FALSE;
        CopyMemory(&received, copyData->lpData, sizeof received);
        if (received.Magic != MGR_MAGIC || received.Version != MGR_VERSION)
            return FALSE;
        received.Name[MGR_NAME_SIZE - 1] = MGR_END_OF_STRING;
        if (received.Operation == MGR_OP_HELLO)
            return MgrSessionHello(&received);
        if (received.Operation == MGR_OP_BYE)
        {
            INT slot;
            for (slot = 0; slot < MGR_MAX; ++slot)
                if (g_Sessions[slot].IsUsed && g_Sessions[slot].ProcessId == received.ProcessId)
                    MgrSessionDrop(slot);
            return TRUE;
        }
        return FALSE;
    }

    case WM_MGRTRAY:
        if (lParam == WM_RBUTTONUP || lParam == WM_CONTEXTMENU)
        {
            MgrTrayMenu();
            return 0;
        }
        if (lParam == WM_LBUTTONDBLCLK)
        {
            /* One program: bring it forward, which is what a tray icon does. More
             * than one: there is no single answer, so show the menu.
             */
            if (MgrSessionCount() == MGR_ONE_PROGRAM)
            {
                INT slot;
                for (slot = 0; slot < MGR_MAX && !g_Sessions[slot].IsUsed; ++slot) ;
                MgrSessionCommand(slot, MGR_COMMAND_SHOW);
            }
            else
            {
                MgrTrayMenu();
            }
        }
        return 0;

    case WM_COMMAND:
        MgrOnCommand(LOWORD(wParam));
        return 0;

    case WM_DESTROY:
        if (g_HasTrayIcon)
            MgrTraySet(NIM_DELETE);
        PostQuitMessage(MGR_EXIT_OK);
        return 0;
    }
    return DefWindowProcA(window, message, wParam, lParam);
}

int WINAPI WinMain(
    HINSTANCE instance,
    HINSTANCE previousInstance,
    LPSTR commandLine,
    int showCommand)
{
    WNDCLASSA windowClass;
    HANDLE singleInstance;
    DWORD startedAt = GetTickCount();
    DWORD emptySince = 0;
    BOOL hasEverHadPrograms = FALSE;

    (VOID)previousInstance;
    (VOID)commandLine;
    (VOID)showCommand;

    /* One manager per session. A second one started in a race simply leaves: the
     * first answers every host, and they find it by class name.
     */
    singleInstance = CreateMutexA(NULL, TRUE, MGR_MUTEX);
    if (!singleInstance || GetLastError() == ERROR_ALREADY_EXISTS)
        return MGR_EXIT_OK;

    g_CommandMessage          = RegisterWindowMessageA(MGR_CMD_MSGNAME);
    g_TaskbarCreatedMessage = RegisterWindowMessageA(MGR_TASKBAR_CREATED);
    g_Icon = LoadIconA(instance, MAKEINTRESOURCEA(IDI_MAINICON));
    if (!g_Icon)
        g_Icon = LoadIconA(NULL, IDI_APPLICATION);

    ZeroMemory(&windowClass, sizeof windowClass);
    windowClass.lpfnWndProc   = MgrWindowProc;
    windowClass.hInstance     = instance;
    windowClass.lpszClassName = MGR_CLASS;
    windowClass.hIcon         = g_Icon;
    if (!RegisterClassA(&windowClass))
        return MGR_EXIT_FAILED;
    /* A real (hidden) top-level window, not HWND_MESSAGE: the tray needs a window
     * that can take the foreground for its menu, and hosts find it by class.
     */
    g_Window = CreateWindowExA(WS_EX_TOOLWINDOW, MGR_CLASS, MGR_WINDOW_TITLE, WS_POPUP,
                             0, 0, 0, 0, NULL, NULL, instance, NULL);
    if (!g_Window)
        return MGR_EXIT_FAILED;

    for (;;)
    {
        HANDLE processes[MGR_MAX];
        INT slotOfHandle[MGR_MAX];
        INT handleCount = 0;
        INT slot;
        DWORD  waitResult;
        MSG    queued;
        for (slot = 0; slot < MGR_MAX; ++slot)
            if (g_Sessions[slot].IsUsed && g_Sessions[slot].Process)
            {
                processes[handleCount] = g_Sessions[slot].Process;
                slotOfHandle[handleCount] = slot;
                ++handleCount;
            }
        waitResult = MsgWaitForMultipleObjects((DWORD)handleCount, processes, FALSE, MGR_POLL_MS, QS_ALLINPUT);
        if (waitResult < WAIT_OBJECT_0 + (DWORD)handleCount)
        {
            MgrSessionDrop(slotOfHandle[waitResult - WAIT_OBJECT_0]);       /* that host has exited */
        }
        while (PeekMessageA(&queued, NULL, MGR_DRAIN_ALL, MGR_DRAIN_ALL, PM_REMOVE))
        {
            if (queued.message == WM_QUIT)
                goto out;
            TranslateMessage(&queued);
            DispatchMessageA(&queued);
        }
        /* [INFO]: THE LIFETIME RULE (user, step 6): when the last program has gone, so do
         * we -- after a short grace, because a program that is starting right now
         * announces itself a moment later. And a manager nobody ever spoke to
         * (its host died before announcing) leaves too.
         */
        if (MgrSessionCount())
        {
            hasEverHadPrograms = TRUE;
            emptySince = 0;
        }
        else if (hasEverHadPrograms)
        {
            if (!emptySince)
                emptySince = GetTickCount();
            else if (GetTickCount() - emptySince >= EMPTY_GRACE_MS)
                break;
        }
        else if (GetTickCount() - startedAt >= FIRST_HELLO_MS)
            break;
    }
out:
    if (g_HasTrayIcon)
        MgrTraySet(NIM_DELETE);
    DestroyWindow(g_Window);
    for (;;)                                        /* drain WM_DESTROY etc. */
    {
        MSG queued;
        if (!PeekMessageA(&queued, NULL, MGR_DRAIN_ALL, MGR_DRAIN_ALL, PM_REMOVE))
            break;
        DispatchMessageA(&queued);
    }
    CloseHandle(singleInstance);
    return MGR_EXIT_OK;
}
