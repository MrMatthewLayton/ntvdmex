/*
 * vdmhost.c - Spike-001 step 1: receive the DOS command from CSRSS.
 *
 * Spike-002 proved XP launches our binary as the DOS VDM support process. The
 * next question: can we pull the actual program-to-run out of the CSRSS VDM
 * queue the way real ntvdm does? Real ntvdm calls kernel32!GetNextVDMCommand
 * with a VDM_COMMAND_INFO (recovered: ntvdm builds a ~160-byte struct and calls
 * it at 0xf04ed86; struct layout from ReactOS, cross-checked).
 *
 * This stub, launched as the VDM host (via the Control\WOW\cmdline repoint),
 * calls GetNextVDMCommand once and logs the result (AppName/CmdLine/CurDir/flags
 * + the BOOL return and GetLastError). If it succeeds we have the program info;
 * if it fails the error guides the next iteration. No VDM init yet.
 *
 * No CRT (shares src/runtime.c); imports only kernel32 + user32 -> loads on XP.
 */
#include <windows.h>

/* VDM_COMMAND_INFO -- from ReactOS sdk/include/reactos/subsys/win/vdm.h,
   matches the ~0xA0-byte struct ntvdm.exe passes to GetNextVDMCommand. */
typedef struct {
    ULONG  TaskId;
    ULONG  CreationFlags;
    ULONG  ExitCode;
    ULONG  CodePage;
    HANDLE StdIn, StdOut, StdErr;
    LPSTR  CmdLine, AppName, PifFile, CurDirectory, Env;
    ULONG  EnvLen;
    STARTUPINFOA StartupInfo;
    LPSTR  Desktop;  ULONG DesktopLen;
    LPSTR  Title;    ULONG TitleLen;
    LPVOID Reserved; ULONG ReservedLen;
    USHORT CmdLen, AppLen, PifLen, CurDirectoryLen, VDMState, CurrentDrive;
    BOOLEAN ComingFromBat;
} VDM_COMMAND_INFO;

#define VDM_GET_FIRST_COMMAND  0x100
#define VDM_GET_ENVIRONMENT    0x400

typedef BOOL (WINAPI *PFN_GetNextVDMCommand)(VDM_COMMAND_INFO *);

static char *zput(char *p, const char *s) { while (*s) *p++ = *s++; *p = 0; return p; }
static char *zhex(char *p, unsigned v) {
    int i; char t[9]; t[8] = 0;
    for (i = 7; i >= 0; --i) { t[i] = "0123456789abcdef"[v & 0xf]; v >>= 4; }
    return zput(p, t);
}

/* Static (zero-initialised) receive buffers -- no CRT heap. */
static char g_cmd[1024], g_app[1024], g_cur[512], g_pif[512];
static char g_env[8192], g_desk[512], g_title[512], g_rsv[512];
static VDM_COMMAND_INFO g_ci;

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR lpCmd, int nShow)
{
    char  report[10240];
    char  self[MAX_PATH], logpath[MAX_PATH];
    char *p = report, *e;
    PFN_GetNextVDMCommand pfn;

    (void)hInst; (void)hPrev; (void)lpCmd; (void)nShow;

    g_ci.CmdLine      = g_cmd;   g_ci.CmdLen          = sizeof(g_cmd);
    g_ci.AppName      = g_app;   g_ci.AppLen          = sizeof(g_app);
    g_ci.PifFile      = g_pif;   g_ci.PifLen          = sizeof(g_pif);
    g_ci.CurDirectory = g_cur;   g_ci.CurDirectoryLen = sizeof(g_cur);
    g_ci.Env          = g_env;   g_ci.EnvLen          = sizeof(g_env);
    g_ci.Desktop      = g_desk;  g_ci.DesktopLen      = sizeof(g_desk);
    g_ci.Title        = g_title; g_ci.TitleLen        = sizeof(g_title);
    g_ci.Reserved     = g_rsv;   g_ci.ReservedLen     = sizeof(g_rsv);
    g_ci.StartupInfo.cb = sizeof(STARTUPINFOA);
    g_ci.VDMState     = VDM_GET_FIRST_COMMAND | VDM_GET_ENVIRONMENT;

    p = zput(p, "NTVDMEX vdmhost -- calling kernel32!GetNextVDMCommand\r\n\r\n");

    pfn = (PFN_GetNextVDMCommand)GetProcAddress(
              GetModuleHandleA("kernel32.dll"), "GetNextVDMCommand");
    if (!pfn) {
        p = zput(p, "GetNextVDMCommand NOT exported by kernel32 on this build.\r\n");
    } else {
        BOOL  ok  = pfn(&g_ci);
        DWORD err = GetLastError();
        p = zput(p, "returned "); p = zput(p, ok ? "TRUE" : "FALSE");
        p = zput(p, "   GetLastError=0x"); p = zhex(p, err); p = zput(p, "\r\n\r\n");
        p = zput(p, "AppName : "); p = zput(p, g_app); p = zput(p, "\r\n");
        p = zput(p, "CmdLine : "); p = zput(p, g_cmd); p = zput(p, "\r\n");
        p = zput(p, "CurDir  : "); p = zput(p, g_cur); p = zput(p, "\r\n");
        p = zput(p, "PifFile : "); p = zput(p, g_pif); p = zput(p, "\r\n");
        p = zput(p, "VDMState=0x");     p = zhex(p, g_ci.VDMState);
        p = zput(p, "  CurrentDrive="); p = zhex(p, g_ci.CurrentDrive);
        p = zput(p, "  TaskId=0x");     p = zhex(p, g_ci.TaskId);
        p = zput(p, "  ExitCode=0x");   p = zhex(p, g_ci.ExitCode);
        p = zput(p, "\r\npost-call *Len (req'd sizes if too-small): ");
        p = zput(p, "Cmd=0x");    p = zhex(p, g_ci.CmdLen);
        p = zput(p, " App=0x");   p = zhex(p, g_ci.AppLen);
        p = zput(p, " Pif=0x");   p = zhex(p, g_ci.PifLen);
        p = zput(p, " CurDir=0x");p = zhex(p, g_ci.CurDirectoryLen);
        p = zput(p, " Env=0x");   p = zhex(p, g_ci.EnvLen);
        p = zput(p, " Desk=0x");  p = zhex(p, g_ci.DesktopLen);
        p = zput(p, " Title=0x"); p = zhex(p, g_ci.TitleLen);
        p = zput(p, " Rsv=0x");   p = zhex(p, g_ci.ReservedLen);
        p = zput(p, "\r\n");
    }

    /* Log next to this exe: <dir>\vdmhost.log */
    GetModuleFileNameA(NULL, self, sizeof(self));
    e = zput(logpath, self);
    while (e > logpath && e[-1] != '\\') --e;
    zput(e, "vdmhost.log");
    {
        HANDLE h = CreateFileA(logpath, GENERIC_WRITE, 0, NULL,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (h != INVALID_HANDLE_VALUE) {
            DWORD wr; WriteFile(h, report, (DWORD)(p - report), &wr, NULL);
            CloseHandle(h);
        }
    }
    MessageBoxA(NULL, report, "NTVDMEX vdmhost -- GetNextVDMCommand",
                MB_OK | MB_ICONINFORMATION);
    return 0;
}
