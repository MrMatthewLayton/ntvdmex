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

/* RegisterConsoleVDM (kernel32) -- registers us as the console VDM with CSRSS.
   11 args, matching ntvdm's call at 0xf014078; DOS uses flag 1 and 0 for the
   video-state buffer/size (args 8,9). */
typedef BOOL (WINAPI *PFN_RegisterConsoleVDM)(DWORD, HANDLE, HANDLE, HANDLE,
            DWORD, PVOID, PVOID, PVOID, DWORD, PVOID, PVOID);

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
/* TEB self-pointer (fs:[0x18]) without the CRT/winternl. */
static void *get_teb(void) { void *t; __asm__ volatile("movl %%fs:0x18,%0" : "=r"(t)); return t; }

/* Raw hex dump of n bytes at b, space-separated, for inspecting struct fields. */
static char *zdump(char *p, const void *b, unsigned n) {
    const unsigned char *q = (const unsigned char *)b; unsigned i;
    for (i = 0; i < n; ++i) {
        *p++ = "0123456789abcdef"[q[i] >> 4];
        *p++ = "0123456789abcdef"[q[i] & 0xf];
        *p++ = ((i & 0xf) == 0xf) ? '\n' : ' ';
    }
    *p = 0; return p;
}

/* Static (zero-initialised) receive buffers -- no CRT heap. */
static char g_cmd[1024], g_app[1024], g_cur[512], g_pif[512];
static char g_env[8192], g_desk[512], g_title[512], g_rsv[512];
static VDM_COMMAND_INFO g_ci;
/* Our own VDM_TIB (the kernel never sets TEB->Vdm; ntvdm self-allocates one). 0x674
   bytes per ntvdm's 0xf044374; round up for safety. 16-aligned. */
static BYTE g_vdmtib[0x700] __attribute__((aligned(16)));

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
    /* VDMState for the first command fetch. Ground truth (hexdump of g_ci after the
       call + server disasm): with the receive buffers we provide, the server fills
       Title = the program name ("dosstub.com") and CurDirectory = the working dir
       ("C:\ntvdmex") -- together they identify the program (CurDir\Title). It does NOT
       populate AppName/CmdLine here (their *Len stay at our input sizes); the real
       ntvdm fetches the full command line via a separate multi-call protocol -- its
       env fetch uses VDMState 0x404 then 0x40c with a realloc-retry and NULL string
       buffers (call site 0xf00acfa/0xad99), and the exec-BOP path (0xf04ed86) checks
       CmdLen afterwards. Replicating that is the execution-phase task. 0x100 alone and
       0x500 both yield the same Title+CurDir, so we keep the simple first-command flag. */
    g_ci.VDMState     = VDM_GET_FIRST_COMMAND;

    /* Do we actually have a console? (the console-subsystem hypothesis) */
    p = zput(p, "ConsoleWindow=0x"); p = zhex(p, (unsigned)(ULONG_PTR)GetConsoleWindow());
    p = zput(p, "  StdIn=0x");  p = zhex(p, (unsigned)(ULONG_PTR)GetStdHandle(STD_INPUT_HANDLE));
    p = zput(p, "  StdOut=0x"); p = zhex(p, (unsigned)(ULONG_PTR)GetStdHandle(STD_OUTPUT_HANDLE));
    p = zput(p, "  CmdLine=["); p = zput(p, GetCommandLineA()); p = zput(p, "]\r\n");

    /* TaskId: route GetNextVDMCommand's server-side lookup to the *task-id* path.
       Recovered from basesrv!BaseSrvGetNextVDMCommand (0x75b57221): for the first
       command (VDMState&0x100), if Data+0 (TaskId) != 0 it keys the lookup on the
       task id (0x75b55602: walk the VDM list, match node+0x20==TaskId where node+4==0)
       instead of the console handle (0x75b555cd). Under IFEO our console handle differs
       from the launcher's, so the console path misses -> 0x57. CheckVDM assigned this
       VDM the task id it passed us as "-i<n>" (hex), so feeding it back finds the
       queued command regardless of which console we landed in. */
    {
        const char *q = GetCommandLineA();
        ULONG tid = 0;
        while (*q) {
            if (q[0] == '-' && (q[1] == 'i' || q[1] == 'I')) {
                const char *r = q + 2;
                tid = 0;
                while (*r == ' ') ++r;
                for (;;) {
                    char c = *r;
                    if (c >= '0' && c <= '9')              tid = tid * 16 + (ULONG)(c - '0');
                    else if ((c | 0x20) >= 'a' && (c | 0x20) <= 'f')
                                                           tid = tid * 16 + (ULONG)((c | 0x20) - 'a' + 10);
                    else break;
                    ++r;
                }
            }
            ++q;
        }
        g_ci.TaskId = tid;          /* take the last -i<n> on the line */
        p = zput(p, "Parsed TaskId=0x"); p = zhex(p, tid); p = zput(p, "\r\n");
    }

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

    /* STAGE 1r: RegisterConsoleVDM(1,...) -- register as the console VDM with CSRSS.
       Likely the missing CSRSS-side association that makes GetNextVDMCommand return
       a command instead of 0x57. 3 hardware events + scratch OUT pointers; video
       buffer/size = 0 like ntvdm's DOS path. */
    {
        PFN_RegisterConsoleVDM RegisterConsoleVDM =
            (PFN_RegisterConsoleVDM)GetProcAddress(
                GetModuleHandleA("kernel32.dll"), "RegisterConsoleVDM");
        HANDLE hStart = CreateEventA(NULL, TRUE, FALSE, NULL);
        HANDLE hEnd   = CreateEventA(NULL, TRUE, FALSE, NULL);
        HANDLE hErr   = CreateEventA(NULL, TRUE, FALSE, NULL);
        static DWORD out6, out10; static PVOID out7, out11;
        p = zput(p, "STAGE1r: RegisterConsoleVDM... ");
        writelog(report, p);
        if (!RegisterConsoleVDM) {
            p = zput(p, "NOT exported.\r\n");
        } else {
            BOOL ok = RegisterConsoleVDM(1, hStart, hEnd, hErr, 0,
                          &out6, &out7, 0, 0, &out10, &out11);
            DWORD e = GetLastError();
            p = zput(p, ok ? "TRUE" : "FALSE");
            p = zput(p, " err=0x"); p = zhex(p, e); p = zput(p, "\r\n");
        }
        writelog(report, p);
    }

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
        /* Ground-truth: what did the server actually write, and where? Dump the
           whole VDM_COMMAND_INFO (0xA0) + the head of each receive buffer. */
        p = zput(p, "g_ci raw (off 0x00):\r\n"); p = zdump(p, &g_ci, 0xA0);
        p = zput(p, "\r\ng_app:\r\n");   p = zdump(p, g_app, 80);
        p = zput(p, "\r\ng_cmd:\r\n");   p = zdump(p, g_cmd, 48);
        p = zput(p, "\r\ng_cur:\r\n");   p = zdump(p, g_cur, 32);
        p = zput(p, "\r\ng_pif:\r\n");   p = zdump(p, g_pif, 48);
        p = zput(p, "\r\ng_title:\r\n"); p = zdump(p, g_title, 48);
        p = zput(p, "\r\n");
        /* The decoded program: CurDirectory + "\" + Title. */
        if (g_cur[0] && g_title[0]) {
            p = zput(p, "==> RESOLVED PROGRAM: ");
            p = zput(p, g_cur); p = zput(p, "\\"); p = zput(p, g_title);
            p = zput(p, "\r\n");
        }
    }

    /* STAGE 3: attempt REAL V86 execution. VdmStartExecution = NtVdmControl(0, NULL)
       (recovered from ntvdm's CPU loop 0xf005350). The kernel runs the CONTEXT stored
       at VDM_TIB+0x2D8 in V86 mode until a fault/event, then returns. Field offsets in
       VDM_TIB (= *(PVOID*)(TEB+0xF18)), from ntvdm's getXX accessors + standard x86
       CONTEXT: ContextFlags 0x2D8, Gs/Fs/Es/Ds 0x364/68/6C/70, Edi/Esi/Ebx/Edx/Ecx/Eax/Ebp
       0x374..0x38C, Eip 0x390, SegCs 0x394, EFlags 0x398 (VM=0x20000), Esp 0x39C, SegSs 0x3A0.
       Tiny V86 program at 0x200:0 (host-linear 0x2000, inside our mapped low memory):
         B8 EF BE  mov ax,0xBEEF
         A3 80 00  mov [0x0080],ax     ; -> 0x200:0x80 = linear 0x2080
         F4        hlt                 ; privileged in V86 -> #GP -> back to host
       If the real CPU executed it, EAX low word = 0xBEEF on return and 0xBEEF lands at
       host-linear 0x2080. */
    if (ntvdmctl) {
        BYTE *teb = (BYTE *)get_teb();
        BYTE *tib = *(BYTE **)(teb + 0xF18);
        p = zput(p, "STAGE3: existing VDM_TIB=0x"); p = zhex(p, (unsigned)(ULONG_PTR)tib);
        p = zput(p, "\r\n");
        /* The kernel does NOT set TEB->Vdm; ntvdm allocates the VDM_TIB itself and
           registers the pointer (0xf044517: TEB[0xF18] = &tib), then inits it
           (0xf044374). Replicate that minimally into our own static buffer. */
        if (!tib) {
            tib = g_vdmtib;
            *(DWORD *)(tib + 0x000) = 0x674;          /* VDM_TIB.Size */
            *(DWORD *)(tib + 0x098) = 0;
            *(DWORD *)(tib + 0x09c) = 0x3b;
            *(DWORD *)(tib + 0x0a0) = 0x23;
            *(DWORD *)(tib + 0x0a4) = 0x23;
            *(DWORD *)(tib + 0x66c) = 0xffffffff;
            tib[0x5e4] = 1; tib[0x5e5] = 1; tib[0x5e6] = 1; tib[0x670] = 0;
            *(BYTE **)(teb + 0xF18) = tib;            /* register with our TEB */
            p = zput(p, "STAGE3: allocated+registered VDM_TIB=0x");
            p = zhex(p, (unsigned)(ULONG_PTR)tib); p = zput(p, "\r\n");
        }
        writelog(report, p);
        if (tib) {
            static const BYTE prog[] = { 0xB8,0xEF,0xBE, 0xA3,0x80,0x00, 0xF4 };
            volatile BYTE *code = (volatile BYTE *)0x2000;   /* 0x200:0 */
            unsigned i; LONG st;
            for (i = 0; i < sizeof(prog); ++i) code[i] = prog[i];
            *(volatile WORD *)0x2080 = 0;                    /* clear marker */
            #define TF(off) (*(volatile DWORD *)(tib + (off)))
            TF(0x2D8) = 0x10007;                            /* ContextFlags */
            TF(0x364) = 0x200; TF(0x368) = 0x200; TF(0x36C) = 0x200; TF(0x370) = 0x200;
            TF(0x374) = 0; TF(0x378) = 0; TF(0x37C) = 0; TF(0x380) = 0;
            TF(0x384) = 0; TF(0x388) = 0; TF(0x38C) = 0;
            TF(0x390) = 0x0000;                             /* Eip */
            TF(0x394) = 0x0200;                             /* SegCs */
            TF(0x398) = 0x20202;                            /* EFlags: VM | IF | reserved (ntvdm 0x202|VM) */
            TF(0x39C) = 0x1000;                             /* Esp */
            TF(0x3A0) = 0x0200;                             /* SegSs */
            p = zput(p, "STAGE3: pre-exec, calling NtVdmControl(0,NULL)...\r\n");
            writelog(report, p);
            st = ntvdmctl(0, NULL);
            p = zput(p, "STAGE3: returned NTSTATUS=0x"); p = zhex(p, (unsigned)st);
            p = zput(p, "\r\n  EAX=0x");        p = zhex(p, TF(0x388));
            p = zput(p, " EIP=0x");             p = zhex(p, TF(0x390));
            p = zput(p, " CS=0x");              p = zhex(p, TF(0x394));
            p = zput(p, " EFL=0x");             p = zhex(p, TF(0x398));
            p = zput(p, "\r\n  event[0x5a8]=0x"); p = zhex(p, *(volatile DWORD *)(tib + 0x5a8));
            p = zput(p, " ilen[0x5ac]=0x");      p = zhex(p, *(volatile DWORD *)(tib + 0x5ac));
            p = zput(p, "\r\n  mem[0x2080]=0x");  p = zhex(p, *(volatile WORD *)0x2080);
            p = zput(p, "\r\n");
            #undef TF
        }
        writelog(report, p);
    }

    /* STAGE 2: full result (overwrites STAGE0/1). Log-only -- no MessageBox, so we
       never block in a non-interactive window station; the file is the channel. */
    p = zput(p, "STAGE2: complete\r\n");
    writelog(report, p);
    return 0;
}
