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

/* --- NtVdmControl(VdmInitialize) contract, recovered from ntvdm 0xf00e668 -------
 * NtVdmControl(3, &VDM_INITIALIZE_DATA). The kernel registers this process as a
 * VDM (sets TEB.Vdm, trap/ICA handlers) -- the step kernel32!GetNextVDMCommand
 * needs done before it will return a command (else 0x57). ntvdm passes its real
 * trap handler + 9 ICA-state pointers; we pass a trap-handler stub + our own
 * static ICA buffers (the kernel stores/probes these; the stub is only *called*
 * later on a V86 fault, which we don't trigger here). */
#define VDM_SVC_VdmInitialize  3

typedef struct {            /* VDMICAUSERDATA -- 9 pointers (XP ntvdm fills 9) */
    PVOID pIcaLock, pIcaMaster, pIcaSlave, pDelayIrq, pUndelayIrq,
          pDelayIret, pIretHooked, pAddrIretBopTable, p9;
} VDMICAUSERDATA;

typedef struct {            /* VDM_INITIALIZE_DATA */
    PVOID TrapcHandler;
    VDMICAUSERDATA *IcaUserData;
} VDM_INITIALIZE_DATA;

typedef LONG (WINAPI *PFN_NtVdmControl)(ULONG Service, PVOID ServiceData);

/* --- V86 low-memory setup (ntvdm fn 0xf00ea75, runs right before VdmInitialize).
 * Lay down the V86 address space so VdmInitialize stops AV'ing: create a section,
 * release the default low reservations, map the section X-RW into low memory. */
typedef struct {                /* OBJECT_ATTRIBUTES (24 bytes) */
    ULONG Length; PVOID RootDirectory, ObjectName; ULONG Attributes;
    PVOID SecurityDescriptor, SecurityQOS;
} OBJ_ATTR;

typedef LONG (WINAPI *PFN_NtCreateSection)(PHANDLE, ULONG, OBJ_ATTR *,
            LARGE_INTEGER *, ULONG, ULONG, HANDLE);
typedef LONG (WINAPI *PFN_NtFreeVirtualMemory)(HANDLE, PVOID *, SIZE_T *, ULONG);
typedef LONG (WINAPI *PFN_NtMapViewOfSection)(HANDLE, HANDLE, PVOID *, ULONG,
            SIZE_T, LARGE_INTEGER *, SIZE_T *, ULONG, ULONG, ULONG);

#define MEM_RELEASE_NT  0x8000
#define SEC_RESERVE_NT  0x04000000
#define VDM_MAP_FLAG    0x40000000   /* ntvdm's AllocationType for the V86 map */

/* Static ICA-state backing store (zero-init). Generous sizes so the kernel's
   probes/stores land in valid writable memory. */
static BYTE g_ica_lock[256], g_ica_master[256], g_ica_slave[256], g_ica_bop[1024];
static DWORD g_delayirq, g_undelayirq, g_delayiret, g_irethooked, g_p9;
static VDMICAUSERDATA g_ica;
static VDM_INITIALIZE_DATA g_initdata;

/* Trap handler stub -- stored by the kernel, only invoked on a V86 fault. */
static void __stdcall TrapcStub(void) { }

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

/* Overwrite C:\ntvdmex\vdmhost.log with [report..p). Fixed path (no
   GetModuleFileNameA) so this works identically standalone and as a VDM host.
   Called at two stages so a crash localises to the call between them. */
static void writelog(const char *buf, char *p)
{
    HANDLE h = CreateFileA("C:\\ntvdmex\\vdmhost.log", GENERIC_WRITE, 0, NULL,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD wr; WriteFile(h, buf, (DWORD)(p - buf), &wr, NULL);
        CloseHandle(h);
    }
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR lpCmd, int nShow)
{
    char  report[10240];
    char *p = report;
    PFN_GetNextVDMCommand pfn;
    PFN_NtVdmControl      ntvdmctl;

    (void)hInst; (void)hPrev; (void)lpCmd; (void)nShow;

    /* STAGE 0: prove we ran at all -- written before any other API call. */
    p = zput(p, "NTVDMEX vdmhost log (incremental)\r\nSTAGE0: WinMain entered\r\n");
    writelog(report, p);

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

    /* Do we actually have a console? (the console-subsystem hypothesis) */
    p = zput(p, "ConsoleWindow=0x"); p = zhex(p, (unsigned)(ULONG_PTR)GetConsoleWindow());
    p = zput(p, "  StdIn=0x");  p = zhex(p, (unsigned)(ULONG_PTR)GetStdHandle(STD_INPUT_HANDLE));
    p = zput(p, "  StdOut=0x"); p = zhex(p, (unsigned)(ULONG_PTR)GetStdHandle(STD_OUTPUT_HANDLE));
    p = zput(p, "  CmdLine=["); p = zput(p, GetCommandLineA()); p = zput(p, "]\r\n");

    /* STAGE 1m: lay down the V86 low-memory address space (ntvdm 0xf00ea75) before
       VdmInitialize, else VdmInitialize AV's (0xC0000005). */
    {
        HANDLE hntdll = GetModuleHandleA("ntdll.dll");
        PFN_NtCreateSection     NtCreateSection =
            (PFN_NtCreateSection)GetProcAddress(hntdll, "NtCreateSection");
        PFN_NtFreeVirtualMemory NtFreeVirtualMemory =
            (PFN_NtFreeVirtualMemory)GetProcAddress(hntdll, "NtFreeVirtualMemory");
        PFN_NtMapViewOfSection  NtMapViewOfSection =
            (PFN_NtMapViewOfSection)GetProcAddress(hntdll, "NtMapViewOfSection");
        HANDLE hSec = NULL;
        OBJ_ATTR oa; LARGE_INTEGER maxsize, off;
        PVOID base; SIZE_T sz; LONG st;
        unsigned i; char *q = (char *)&oa;
        for (i = 0; i < sizeof(oa); ++i) q[i] = 0;
        oa.Length = 0x18;
        maxsize.QuadPart = 0xB0000;
        st = NtCreateSection(&hSec, 0xA, &oa, &maxsize, PAGE_EXECUTE_READWRITE,
                             SEC_RESERVE_NT, NULL);
        p = zput(p, "STAGE1m: NtCreateSection=0x"); p = zhex(p, (unsigned)st);
        base = (PVOID)1;        sz = 0x9FFFF;  NtFreeVirtualMemory((HANDLE)-1, &base, &sz, MEM_RELEASE_NT);
        base = (PVOID)0x100000; sz = 0x10000;  NtFreeVirtualMemory((HANDLE)-1, &base, &sz, MEM_RELEASE_NT);
        base = (PVOID)1;        sz = 0xFFFF;  off.QuadPart = 0;
        st = NtMapViewOfSection(hSec, (HANDLE)-1, &base, 0, 0xFFFF, &off, &sz, 2,
                                VDM_MAP_FLAG, PAGE_EXECUTE_READWRITE);
        p = zput(p, " map0=0x"); p = zhex(p, (unsigned)st);
        base = (PVOID)0x100000; sz = 0x10000; off.QuadPart = 0;
        st = NtMapViewOfSection(hSec, (HANDLE)-1, &base, 0, 0x10000, &off, &sz, 2,
                                VDM_MAP_FLAG, PAGE_EXECUTE_READWRITE);
        p = zput(p, " map1=0x"); p = zhex(p, (unsigned)st); p = zput(p, "\r\n");
        writelog(report, p);
    }

    /* STAGE 1a: NtVdmControl(VdmInitialize) -- register as a VDM with the kernel so
       kernel32!GetNextVDMCommand stops returning 0x57. Build VDM_INITIALIZE_DATA
       with our own ICA buffers + trap stub. Log the NTSTATUS. (If this faults/hangs,
       the log stops here -> that call is the problem.) */
    g_ica.pIcaLock = g_ica_lock; g_ica.pIcaMaster = g_ica_master;
    g_ica.pIcaSlave = g_ica_slave; g_ica.pDelayIrq = &g_delayirq;
    g_ica.pUndelayIrq = &g_undelayirq; g_ica.pDelayIret = &g_delayiret;
    g_ica.pIretHooked = &g_irethooked; g_ica.pAddrIretBopTable = g_ica_bop;
    g_ica.p9 = &g_p9;
    g_initdata.TrapcHandler = (PVOID)&TrapcStub;
    g_initdata.IcaUserData  = &g_ica;
    p = zput(p, "STAGE1: NtVdmControl(VdmInitialize)... ");
    writelog(report, p);
    ntvdmctl = (PFN_NtVdmControl)GetProcAddress(
                   GetModuleHandleA("ntdll.dll"), "NtVdmControl");
    if (!ntvdmctl) {
        p = zput(p, "NtVdmControl NOT exported.\r\n");
    } else {
        LONG st = ntvdmctl(VDM_SVC_VdmInitialize, &g_initdata);
        p = zput(p, "NTSTATUS=0x"); p = zhex(p, (unsigned)st);
        p = zput(p, (st >= 0) ? " (OK)\r\n" : " (FAIL)\r\n");
    }
    writelog(report, p);

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

    /* STAGE 2: full result (overwrites STAGE0/1). Log-only -- no MessageBox, so we
       never block in a non-interactive window station; the file is the channel. */
    p = zput(p, "STAGE2: complete\r\n");
    writelog(report, p);
    return 0;
}
