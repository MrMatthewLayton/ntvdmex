/* csrss.c -- see csrss.h. Faithful port from tools/vdmhost/vdmhost.c. */
#include "csrss.h"

ULONG csrss_parse_taskid(const char *cmdline)
{
    const char *q = cmdline;
    ULONG tid = 0;
    while (*q) {
        if (q[0] == '-' && (q[1] == 'i' || q[1] == 'I')) {
            const char *r = q + 2;
            tid = 0;
            while (*r == ' ') ++r;
            for (;;) {
                char c = *r;
                if (c >= '0' && c <= '9')
                    tid = tid * 16 + (ULONG)(c - '0');
                else if ((c | 0x20) >= 'a' && (c | 0x20) <= 'f')
                    tid = tid * 16 + (ULONG)((c | 0x20) - 'a' + 10);
                else break;
                ++r;
            }
        }
        ++q;
    }
    return tid;          /* last -i<n> on the line */
}

BOOL csrss_register_console(void)
{
    PFN_RegisterConsoleVDM RegisterConsoleVDM =
        (PFN_RegisterConsoleVDM)GetProcAddress(
            GetModuleHandleA("kernel32.dll"), "RegisterConsoleVDM");
    HANDLE hStart, hEnd, hErr;
    DWORD out6 = 0, out10 = 0; PVOID out7 = NULL, out11 = NULL;
    if (!RegisterConsoleVDM) return FALSE;
    hStart = CreateEventA(NULL, TRUE, FALSE, NULL);
    hEnd   = CreateEventA(NULL, TRUE, FALSE, NULL);
    hErr   = CreateEventA(NULL, TRUE, FALSE, NULL);
    /* DOS path: flag 1, no video-state buffer/size (args 8,9 = 0). */
    return RegisterConsoleVDM(1, hStart, hEnd, hErr, 0,
                              &out6, &out7, 0, 0, &out10, &out11);
}

BOOL csrss_get_command(VDM_COMMAND_INFO *ci, DWORD *out_err)
{
    PFN_GetNextVDMCommand pfn =
        (PFN_GetNextVDMCommand)GetProcAddress(
            GetModuleHandleA("kernel32.dll"), "GetNextVDMCommand");
    BOOL ok;
    if (!pfn) { if (out_err) *out_err = ERROR_PROC_NOT_FOUND; return FALSE; }
    ok = pfn(ci);
    if (out_err) *out_err = GetLastError();
    return ok;
}

char csrss_next_app[1024], csrss_next_cmd[1024], csrss_next_cur[512];

BOOL csrss_task_done(ULONG task_id, ULONG exit_code, DWORD *out_err, BOOL *out_exitvdm)
{
    typedef BOOL (WINAPI *PFN_ExitVDM)(BOOL, ULONG);
    HMODULE k32 = GetModuleHandleA("kernel32.dll");
    PFN_GetNextVDMCommand pfn = (PFN_GetNextVDMCommand)GetProcAddress(k32, "GetNextVDMCommand");
    PFN_ExitVDM pexit = (PFN_ExitVDM)GetProcAddress(k32, "ExitVDM");
    static char pif[512], env[8192], desk[512], title[512], rsv[512];
    VDM_COMMAND_INFO ci;
    BOOL ok = FALSE;
    int k;
    if (out_exitvdm) *out_exitvdm = FALSE;
    if (!pfn) { if (out_err) *out_err = ERROR_PROC_NOT_FOUND; return FALSE; }
    ZeroMemory(&ci, sizeof ci);
    csrss_next_app[0] = csrss_next_cmd[0] = csrss_next_cur[0] = 0;
    ci.CmdLine = csrss_next_cmd; ci.CmdLen = sizeof csrss_next_cmd;
    ci.AppName = csrss_next_app; ci.AppLen = sizeof csrss_next_app;
    ci.PifFile = pif; ci.PifLen = sizeof pif;
    ci.CurDirectory = csrss_next_cur; ci.CurDirectoryLen = sizeof csrss_next_cur;
    ci.Env = env; ci.EnvLen = sizeof env;       ci.Desktop = desk; ci.DesktopLen = sizeof desk;
    ci.Title = title; ci.TitleLen = sizeof title; ci.Reserved = rsv; ci.ReservedLen = sizeof rsv;
    ci.StartupInfo.cb = sizeof(STARTUPINFOA);
    ci.TaskId = task_id;
    ci.ExitCode = exit_code;
    /* VDM_FLAG_DOS, exactly as stock ntvdm's cmdGetNextCmd reports (reverse/ntvdm.exe
       0xf00ac1e: `or byte [VDMState], 4`, ExitCode from the DOS block). This call
       releases the launcher and then WAITS for the console's next command -- there
       is no non-blocking form of the report (DONT_WAIT was tried: it blocked too). */
    ci.VDMState = 0x04;
    ok = pfn(&ci);
    if (out_err) *out_err = GetLastError();
    csrss_next_app[sizeof csrss_next_app - 1] = 0; csrss_next_cmd[sizeof csrss_next_cmd - 1] = 0;
    for (k = 0; csrss_next_cmd[k]; ++k) if (csrss_next_cmd[k] == '\r' || csrss_next_cmd[k] == '\n') { csrss_next_cmd[k] = 0; break; }
    if (pexit && out_exitvdm) *out_exitvdm = pexit(FALSE, 0);
    return ok;
}

BOOL csrss_exit_vdm(void)
{
    typedef BOOL (WINAPI *PFN_ExitVDM)(BOOL, ULONG);
    PFN_ExitVDM pexit = (PFN_ExitVDM)GetProcAddress(GetModuleHandleA("kernel32.dll"), "ExitVDM");
    if (!pexit) { SetLastError(ERROR_PROC_NOT_FOUND); return FALSE; }
    return pexit(FALSE, 0);
}
