/*
 * wowprobe.c - Spike-002 harness: "does the WOW registry repoint launch us?"
 *
 * When XP runs a 16-bit program it starts a VDM *support process* whose command
 * is read from HKLM\SYSTEM\CurrentControlSet\Control\WOW\cmdline (DOS). If we
 * repoint that value at this stub, XP should launch *us* instead of the real
 * ntvdm.exe. This program proves that happened and -- more valuably -- captures
 * the exact command line XP handed the support process, which is the launch
 * contract we must later honour for real.
 *
 * It does NOT do any VDM work: it records its invocation (log file next to the
 * exe + a MessageBox for immediate proof) and exits. The triggering 16-bit app
 * therefore does not actually run -- expected, and harmless.
 *
 * No CRT (shares src/runtime.c). Imports only kernel32 + user32 -> loads on XP.
 */
#include <windows.h>

/* Append s at p (p points at the write cursor); return the new cursor. */
static char *zput(char *p, const char *s)
{
    while (*s)
        *p++ = *s++;
    *p = 0;
    return p;
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance,
                   LPSTR lpCmdLine, int nCmdShow)
{
    char  report[2048];
    char  self[MAX_PATH];
    char  cwd[MAX_PATH];
    char  logpath[MAX_PATH];
    char *p = report;
    char *e;

    (void)hInstance;
    (void)hPrevInstance;
    (void)lpCmdLine;
    (void)nCmdShow;

    GetModuleFileNameA(NULL, self, sizeof(self));
    cwd[0] = 0;
    GetCurrentDirectoryA(sizeof(cwd), cwd);

    p = zput(p, "NTVDMEX wowprobe -- XP launched us as the VDM support process.\r\n\r\n");
    p = zput(p, "GetCommandLineA():\r\n  ");
    p = zput(p, GetCommandLineA());        /* <-- the datum we came for */
    p = zput(p, "\r\n\r\nCurrentDirectory:\r\n  ");
    p = zput(p, cwd);
    p = zput(p, "\r\n\r\nModuleFileName:\r\n  ");
    p = zput(p, self);
    p = zput(p, "\r\n");

    /* Write "<dir-of-this-exe>\wowprobe.log". */
    e = zput(logpath, self);
    while (e > logpath && e[-1] != '\\')
        --e;                                /* back up to just after last '\' */
    zput(e, "wowprobe.log");

    {
        HANDLE h = CreateFileA(logpath, GENERIC_WRITE, 0, NULL,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (h != INVALID_HANDLE_VALUE) {
            DWORD written;
            WriteFile(h, report, (DWORD)(p - report), &written, NULL);
            CloseHandle(h);
        }
    }

    MessageBoxA(NULL, report, "NTVDMEX wowprobe -- we were launched!",
                MB_OK | MB_ICONINFORMATION);
    return 0;
}
