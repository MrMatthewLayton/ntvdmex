/*
 * manager.c -- ntvdmex.exe, the NTVDMEX manager. GH #281, session 88.
 *
 * ONE tray icon for every running DOS and Win16 program. The user's design:
 *   1. No NTVDMEX process at boot.
 *   2. The first program to start (DOS or Win16) brings this up; it adds the icon.
 *   3-5. Every further program -- each in its own ntvdmhost.exe -- appears as an
 *        entry in the icon's menu.
 *   6. When the last one closes, this exits; the next program starts it again.
 *
 * ── WHY A SEPARATE PROCESS AND NOT ONE HOST FOR EVERYTHING ──────────────────
 * NT gives a process ONE V86 machine: the guest's first megabyte lives at address
 * 0 of the process, and a DPMI guest's selectors live in the process's one LDT.
 * Two DOS programs cannot share a process, which is why stock XP gives each its own
 * ntvdm.exe. So the hosts stay separate and this coordinates them. (Sharing ONE
 * host between Win16 programs is a different and deferred question -- #280.)
 *
 * ── WHAT IT KNOWS AND HOW ───────────────────────────────────────────────────
 * Hosts announce themselves every couple of seconds (src/host/mgrproto.h). This
 * holds a SYNCHRONIZE handle on each and drops the entry when it signals -- a
 * crashed host sends no goodbye, so the handle is the only reliable end. It is not
 * the hosts' parent (Explorer starts them, through the IFEO key) and does not need
 * to be.
 *
 * ⚠ NO C RUNTIME, like every executable in this project (see CMakeLists.txt):
 *   Win32 calls and src/runtime.c only.
 */
#include <windows.h>
#include <shellapi.h>
#include "../host/mgrproto.h"

#define WM_MGRTRAY      (WM_APP + 1)
#define TRAY_ID         1
#define IDI_MAINICON    101
#define MGR_MAX         48            /* MsgWaitForMultipleObjects allows 63 */
#define EMPTY_GRACE_MS  4000          /* a program that is just starting gets time */
#define FIRST_HELLO_MS  15000         /* started, but nobody ever announced: go   */

/* Menu ids: globals below 100; per-program ones are 1000 + slot*8 + action. */
#define IDM_G_SETTINGS  1
#define IDM_G_ABOUT     2
#define IDM_G_EXITALL   3
#define IDM_P_BASE      1000
#define IDM_P_SHOW      0
#define IDM_P_SETTINGS  1
#define IDM_P_CLOSE     2

typedef struct {
    int    used;
    DWORD  pid;
    DWORD  kind;
    HWND   cmd, show;
    HANDLE proc;
    DWORD  seq;                       /* registration order -- the menu's order */
    char   name[MGR_NAME_CB];
} sess_t;

static sess_t g_s[MGR_MAX];
static DWORD  g_seq;
static HWND   g_hwnd;
static UINT   g_cmdmsg, g_taskbar_created;
static int    g_tray;
static HICON  g_icon;

static int sess_count(void)
{
    int i, n = 0;
    for (i = 0; i < MGR_MAX; ++i) if (g_s[i].used) ++n;
    return n;
}

static void tray_tip(NOTIFYICONDATAA *nid)
{
    int n = sess_count();
    char t[64];
    if (n == 1) {
        int i;
        for (i = 0; i < MGR_MAX && !g_s[i].used; ++i) ;
        wsprintfA(t, "NTVDMEX - %s", i < MGR_MAX ? g_s[i].name : "");
    } else {
        wsprintfA(t, "NTVDMEX - %d programs", n);
    }
    lstrcpynA(nid->szTip, t, sizeof nid->szTip);
}

static void tray_set(DWORD op)
{
    NOTIFYICONDATAA nid;
    ZeroMemory(&nid, sizeof nid);
    nid.cbSize = sizeof nid; nid.hWnd = g_hwnd; nid.uID = TRAY_ID;
    if (op != NIM_DELETE) {
        nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
        nid.uCallbackMessage = WM_MGRTRAY;
        nid.hIcon = g_icon;
        tray_tip(&nid);
    }
    if (Shell_NotifyIconA(op, &nid)) {
        if (op == NIM_ADD) g_tray = 1;
        if (op == NIM_DELETE) g_tray = 0;
    } else if (op == NIM_MODIFY && g_tray) {
        /* Explorer lost it without telling us (it was restarted): put it back. */
        g_tray = 0; tray_set(NIM_ADD);
    }
}

static void sess_drop(int i)
{
    if (!g_s[i].used) return;
    if (g_s[i].proc) CloseHandle(g_s[i].proc);
    ZeroMemory(&g_s[i], sizeof g_s[i]);
    if (g_tray) tray_set(NIM_MODIFY);
}

/* Upsert by pid. Returns 1 if accepted. */
static int sess_hello(const mgr_msg_t *m)
{
    int i, fr = -1, changed = 0;
    for (i = 0; i < MGR_MAX; ++i) {
        if (g_s[i].used && g_s[i].pid == m->pid) break;
        if (!g_s[i].used && fr < 0) fr = i;
    }
    if (i == MGR_MAX) {
        HANDLE ph;
        if (fr < 0) return 0;                       /* full */
        ph = OpenProcess(SYNCHRONIZE, FALSE, m->pid);
        if (!ph) return 0;                          /* it is already gone */
        i = fr;
        g_s[i].used = 1; g_s[i].pid = m->pid; g_s[i].proc = ph; g_s[i].seq = ++g_seq;
        changed = 1;
    }
    g_s[i].kind = m->kind;
    g_s[i].cmd  = (HWND)(ULONG_PTR)m->cmdhwnd;
    g_s[i].show = (HWND)(ULONG_PTR)m->showhwnd;
    if (lstrcmpA(g_s[i].name, m->name) != 0) {
        lstrcpynA(g_s[i].name, m->name, MGR_NAME_CB);
        changed = 1;
    }
    if (!g_tray) tray_set(NIM_ADD);
    else if (changed) tray_set(NIM_MODIFY);
    return 1;
}

static void sess_cmd(int i, WPARAM c)
{
    if (i < 0 || i >= MGR_MAX || !g_s[i].used) return;
    if (c == MGRCMD_SHOW && g_s[i].show && IsWindow(g_s[i].show)) {
        if (IsIconic(g_s[i].show)) ShowWindow(g_s[i].show, SW_RESTORE);
        SetForegroundWindow(g_s[i].show);
        return;
    }
    if (g_s[i].cmd && IsWindow(g_s[i].cmd))
        PostMessageA(g_s[i].cmd, g_cmdmsg, c, 0);
}

/* The menu, in the order programs started. */
static void tray_menu(void)
{
    HMENU m = CreatePopupMenu();
    POINT pt;
    int order[MGR_MAX], n = 0, i, j, id;
    if (!m) return;
    for (i = 0; i < MGR_MAX; ++i) if (g_s[i].used) order[n++] = i;
    for (i = 1; i < n; ++i)                         /* insertion sort by seq */
        for (j = i; j > 0 && g_s[order[j - 1]].seq > g_s[order[j]].seq; --j) {
            int t = order[j]; order[j] = order[j - 1]; order[j - 1] = t;
        }
    for (j = 0; j < n; ++j) {
        HMENU sub = CreatePopupMenu();
        i = order[j];
        if (!sub) continue;
        id = IDM_P_BASE + i * 8;
        AppendMenuA(sub, MF_STRING, id + IDM_P_SHOW,     "Show");
        AppendMenuA(sub, MF_STRING, id + IDM_P_SETTINGS, "Settings...");
        AppendMenuA(sub, MF_SEPARATOR, 0, NULL);
        AppendMenuA(sub, MF_STRING, id + IDM_P_CLOSE,    "Close Program");
        AppendMenuA(m, MF_POPUP | MF_STRING, (UINT_PTR)sub,
                    g_s[i].name[0] ? g_s[i].name : "(starting)");
    }
    if (!n) AppendMenuA(m, MF_STRING | MF_GRAYED, 0, "(no programs running)");
    AppendMenuA(m, MF_SEPARATOR, 0, NULL);
    AppendMenuA(m, MF_STRING | (n ? 0 : MF_GRAYED), IDM_G_SETTINGS, "Settings...");
    AppendMenuA(m, MF_STRING, IDM_G_ABOUT, "About");
    AppendMenuA(m, MF_STRING | (n ? 0 : MF_GRAYED), IDM_G_EXITALL, "Exit All");
    GetCursorPos(&pt);
    /* ⚠ The documented tray dance: foreground first, a stray post after, or the
         menu does not dismiss when you click away from it. */
    SetForegroundWindow(g_hwnd);
    TrackPopupMenu(m, TPM_RIGHTBUTTON, pt.x, pt.y, 0, g_hwnd, NULL);
    PostMessageA(g_hwnd, WM_NULL, 0, 0);
    DestroyMenu(m);                                 /* destroys the submenus too */
}

static void on_command(UINT id)
{
    int i;
    if (id >= IDM_P_BASE) {
        i = (int)(id - IDM_P_BASE) / 8;
        switch ((id - IDM_P_BASE) % 8) {
        case IDM_P_SHOW:     sess_cmd(i, MGRCMD_SHOW);      break;
        case IDM_P_SETTINGS: sess_cmd(i, MGRCMD_SETTINGS);  break;
        case IDM_P_CLOSE:    sess_cmd(i, MGRCMD_CLOSEPROG); break;
        }
        return;
    }
    switch (id) {
    case IDM_G_SETTINGS:
        /* Settings are machine-wide (registry + share file); any host's dialog
           edits the same values, so the oldest program's is as good as any. */
        {   int best = -1;
            for (i = 0; i < MGR_MAX; ++i)
                if (g_s[i].used && (best < 0 || g_s[i].seq < g_s[best].seq)) best = i;
            sess_cmd(best, MGRCMD_SETTINGS); }
        break;
    case IDM_G_ABOUT:
        ShellAboutA(g_hwnd, "NTVDMEX#NTVDMEX -- New Technology Virtual DOS Manager, Extended",
                    "One icon for every running DOS and 16-bit Windows program.", g_icon);
        break;
    case IDM_G_EXITALL:
        for (i = 0; i < MGR_MAX; ++i) if (g_s[i].used) sess_cmd(i, MGRCMD_EXIT);
        break;
    }
}

static LRESULT CALLBACK wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == g_taskbar_created && g_taskbar_created) {   /* Explorer restarted */
        g_tray = 0;
        if (sess_count()) tray_set(NIM_ADD);
        return 0;
    }
    switch (msg) {
    case WM_COPYDATA: {
        const COPYDATASTRUCT *cd = (const COPYDATASTRUCT *)lp;
        mgr_msg_t m;
        if (!cd || cd->dwData != MGR_MAGIC || cd->cbData < sizeof m || !cd->lpData)
            return FALSE;
        CopyMemory(&m, cd->lpData, sizeof m);
        if (m.magic != MGR_MAGIC || m.ver != MGR_VER) return FALSE;
        m.name[MGR_NAME_CB - 1] = 0;
        if (m.op == MGR_OP_HELLO) return sess_hello(&m);
        if (m.op == MGR_OP_BYE) {
            int i;
            for (i = 0; i < MGR_MAX; ++i) if (g_s[i].used && g_s[i].pid == m.pid) sess_drop(i);
            return TRUE;
        }
        return FALSE;
    }
    case WM_MGRTRAY:
        if (lp == WM_RBUTTONUP || lp == WM_CONTEXTMENU) { tray_menu(); return 0; }
        if (lp == WM_LBUTTONDBLCLK) {
            /* One program: bring it forward, which is what a tray icon does. More
               than one: there is no single answer, so show the menu. */
            if (sess_count() == 1) {
                int i;
                for (i = 0; i < MGR_MAX && !g_s[i].used; ++i) ;
                sess_cmd(i, MGRCMD_SHOW);
            } else {
                tray_menu();
            }
        }
        return 0;
    case WM_COMMAND:
        on_command(LOWORD(wp));
        return 0;
    case WM_DESTROY:
        if (g_tray) tray_set(NIM_DELETE);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(h, msg, wp, lp);
}

int WINAPI WinMain(HINSTANCE hi, HINSTANCE prev, LPSTR cmd, int show)
{
    WNDCLASSA wc;
    HANDLE single;
    DWORD started = GetTickCount(), empty_since = 0;
    int ever = 0;
    (void)prev; (void)cmd; (void)show;

    /* One manager per session. A second one started in a race simply leaves: the
       first answers every host, and they find it by class name. */
    single = CreateMutexA(NULL, TRUE, MGR_MUTEX);
    if (!single || GetLastError() == ERROR_ALREADY_EXISTS) return 0;

    g_cmdmsg          = RegisterWindowMessageA(MGR_CMD_MSGNAME);
    g_taskbar_created = RegisterWindowMessageA("TaskbarCreated");
    g_icon = LoadIconA(hi, MAKEINTRESOURCEA(IDI_MAINICON));
    if (!g_icon) g_icon = LoadIconA(NULL, IDI_APPLICATION);

    ZeroMemory(&wc, sizeof wc);
    wc.lpfnWndProc   = wndproc;
    wc.hInstance     = hi;
    wc.lpszClassName = MGR_CLASS;
    wc.hIcon         = g_icon;
    if (!RegisterClassA(&wc)) return 1;
    /* A real (hidden) top-level window, not HWND_MESSAGE: the tray needs a window
       that can take the foreground for its menu, and hosts find it by class. */
    g_hwnd = CreateWindowExA(WS_EX_TOOLWINDOW, MGR_CLASS, "NTVDMEX", WS_POPUP,
                             0, 0, 0, 0, NULL, NULL, hi, NULL);
    if (!g_hwnd) return 1;

    for (;;) {
        HANDLE hs[MGR_MAX];
        int    idx[MGR_MAX], n = 0, i;
        DWORD  r;
        MSG    msg;
        for (i = 0; i < MGR_MAX; ++i)
            if (g_s[i].used && g_s[i].proc) { hs[n] = g_s[i].proc; idx[n] = i; ++n; }
        r = MsgWaitForMultipleObjects((DWORD)n, hs, FALSE, 500, QS_ALLINPUT);
        if (r < WAIT_OBJECT_0 + (DWORD)n) {
            sess_drop(idx[r - WAIT_OBJECT_0]);       /* that host has exited */
        }
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) goto out;
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
        /* ★ THE LIFETIME RULE (user, step 6): when the last program has gone, so do
             we -- after a short grace, because a program that is starting right now
             announces itself a moment later. And a manager nobody ever spoke to
             (its host died before announcing) leaves too. */
        if (sess_count()) { ever = 1; empty_since = 0; }
        else if (ever) {
            if (!empty_since) empty_since = GetTickCount();
            else if (GetTickCount() - empty_since >= EMPTY_GRACE_MS) break;
        } else if (GetTickCount() - started >= FIRST_HELLO_MS) break;
    }
out:
    if (g_tray) tray_set(NIM_DELETE);
    DestroyWindow(g_hwnd);
    for (;;) {                                      /* drain WM_DESTROY etc. */
        MSG msg;
        if (!PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) break;
        DispatchMessageA(&msg);
    }
    CloseHandle(single);
    return 0;
}
