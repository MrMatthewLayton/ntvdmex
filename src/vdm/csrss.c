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
