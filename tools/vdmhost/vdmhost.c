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

/* DOS file-handle table: DOS handle (small int) -> Win32 HANDLE. 0/1/2 are the
   console (handled specially); files use slots 5+. Zero-initialised. */
static HANDLE g_fh[24];

/* M2.4 DOS memory management. Conventional memory is an MCB (Memory Control
   Block) chain: each 16-byte MCB precedes the block it owns -- [0]='M' (member)
   or 'Z' (last), [1..2]=owner PSP (0=free, 8=DOS), [3..4]=size in paragraphs.
   g_first_mcb is the chain root; the allocator (AH=48/49/4A) walks it. */
static WORD g_first_mcb;
/* Current Disk Transfer Area (DOS default = PSP:0x80). Set via AH=1Ah. */
static WORD g_dta_seg = 0x0100, g_dta_off = 0x0080;

/* Write one MCB at paragraph `seg`. */
#define MCB_SET(seg, sig, owner, paras) do {                        \
        volatile BYTE *m_ = (volatile BYTE *)((DWORD)(seg) << 4);   \
        m_[0] = (BYTE)(sig);                                        \
        *(volatile WORD *)(m_ + 1) = (WORD)(owner);                 \
        *(volatile WORD *)(m_ + 3) = (WORD)(paras);                 \
        m_[8] = 0;                                                  \
    } while (0)

/* Copy an ASCIIZ string out of V86 memory (seg:off) into a host buffer. */
static void v86_str(DWORD seg, DWORD off, char *dst, int max) {
    const volatile BYTE *s = (const volatile BYTE *)((seg << 4) + (off & 0xFFFF));
    int i;
    for (i = 0; i < max - 1 && s[i]; ++i) dst[i] = (char)s[i];
    dst[i] = 0;
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

/* Append [buf..p) to the log (writelog overwrites; this appends). Used inside
   the V86 service loop so a host crash still leaves the trace-so-far on disk,
   and the in-memory report buffer is reset each iteration rather than grown
   unbounded (which previously smashed the host stack on busy guests). */
static void applog(const char *buf, char *p)
{
    HANDLE h = CreateFileA("C:\\ntvdmex\\vdmhost.log", FILE_APPEND_DATA,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
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
        p = zput(p, " map1=0x"); p = zhex(p, (unsigned)st);
        /* M2.1 Map 3 (ntvdm 0xf00ea75): the REST of conventional memory --
           section[0x10000..] -> linear 0x10000, size 0x90000 (covers 0x10000-0x9FFFF).
           Without this the guest faults the moment it touches memory above 64KB. */
        base = (PVOID)0x10000; sz = 0x90000; off.QuadPart = 0x10000;
        st = NtMapViewOfSection(hSec, (HANDLE)-1, &base, 0, 0x90000, &off, &sz, 2,
                                VDM_MAP_FLAG, PAGE_EXECUTE_READWRITE);
        p = zput(p, " map2c=0x"); p = zhex(p, (unsigned)st); p = zput(p, "\r\n");
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
            /* M2.1: a real DOS process. PSP at PSP_SEG:0, program loaded at PSP_SEG:0x100,
               CS=DS=ES=SS=PSP_SEG, SP=0xFFFE. INT 21h (IOPL=0, sensitive) reflects through
               IVT[0x21] -> a BOP handler at HDLR_SEG:0; the BOP returns event 4; we service
               AH=09h/02h/4Ch, advance EIP past the 3-byte BOP, and re-enter so the handler's
               IRET resumes the program. Memory map (incl. M2.1 Map 3) gives the full 640KB. */
            enum { PSP_SEG = 0x0100, HDLR_SEG = 0x0050, ENV_SEG = 0x0060 };
            static const BYTE prog[] = { 0xB4,0x4C, 0xCD,0x21 };   /* fallback: exit */
            static const BYTE bop[]  = { 0xC4,0xC4,0x20, 0xCF };   /* BOP 0x20 ; iret */
            volatile BYTE *psp  = (volatile BYTE *)(PSP_SEG  << 4);          /* 0x1000 */
            volatile BYTE *code = (volatile BYTE *)((PSP_SEG << 4) + 0x100); /* 0x1100 */
            volatile BYTE *hdlr = (volatile BYTE *)(HDLR_SEG << 4);          /* 0x0500 */
            volatile BYTE *env  = (volatile BYTE *)(ENV_SEG  << 4);          /* 0x0600 */
            HANDLE hcon = CreateFileA("CONOUT$", GENERIC_WRITE, FILE_SHARE_WRITE, NULL,
                                      OPEN_EXISTING, 0, NULL);
            char dosout[1024]; char *op = dosout;                  /* captured DOS output */
            char *base;                            /* incremental-log reset marker */
            static BYTE filebuf[0x20000];          /* host copy of the program image */
            unsigned i; LONG st; DWORD ev, ah, nread = 0; int guard;
            WORD ecs = PSP_SEG, eip = 0x100, ess = PSP_SEG, esp = 0xFFFE; /* entry CS:IP/SS:SP */
            int is_exe = 0;
            (void)st;
            /* EXPERIMENT override (M2 validation): if C:\ntvdmex\target.txt names a
               program, load it directly. Decouples DOS-kernel testing from the flaky
               VDM Title recovery (the M2.5 problem) and lets us retarget binaries
               (mem.exe, debug.exe, ...) by editing a text file -- no rebuild per run. */
            {
                HANDLE ht = CreateFileA("C:\\ntvdmex\\target.txt", GENERIC_READ,
                                        FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
                if (ht != INVALID_HANDLE_VALUE) {
                    char tp[512]; DWORD tn = 0; char *q;
                    ReadFile(ht, tp, sizeof(tp) - 1, &tn, NULL); CloseHandle(ht);
                    tp[tn < sizeof(tp) ? tn : sizeof(tp) - 1] = 0;
                    for (q = tp; *q; ++q) if (*q == '\r' || *q == '\n' || *q == ' ') { *q = 0; break; }
                    if (tp[0]) {
                        HANDLE hf = CreateFileA(tp, GENERIC_READ, FILE_SHARE_READ, NULL,
                                                OPEN_EXISTING, 0, NULL);
                        if (hf != INVALID_HANDLE_VALUE) {
                            ReadFile(hf, filebuf, sizeof(filebuf), &nread, NULL);
                            CloseHandle(hf);
                        }
                        p = zput(p, "STAGE3: target.txt override loaded 0x"); p = zhex(p, nread);
                        p = zput(p, " bytes from "); p = zput(p, tp); p = zput(p, "\r\n");
                    }
                }
            }
            /* Read the program from disk (resolved CurDir\Title; .COM/.EXE) into a host buffer. */
            if (!nread && g_cur[0] && g_title[0]) {
                char path[768]; char *pe; HANDLE hf; const char *t; int has_dot = 0;
                char *pp = path;
                pp = zput(pp, g_cur); pp = zput(pp, "\\"); pp = zput(pp, g_title);
                pe = pp;                                   /* end of CurDir\Title */
                for (t = g_title; *t; ++t) if (*t == '.') has_dot = 1;
                hf = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                                 OPEN_EXISTING, 0, NULL);
                if (hf == INVALID_HANDLE_VALUE && !has_dot) {
                    zput(pe, ".COM");
                    hf = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                                     OPEN_EXISTING, 0, NULL);
                    if (hf == INVALID_HANDLE_VALUE) {
                        zput(pe, ".EXE");
                        hf = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                                         OPEN_EXISTING, 0, NULL);
                    }
                }
                if (hf != INVALID_HANDLE_VALUE) {
                    ReadFile(hf, filebuf, sizeof(filebuf), &nread, NULL);
                    CloseHandle(hf);
                }
                p = zput(p, "STAGE3: loaded "); p = zhex(p, nread);
                p = zput(p, " bytes from "); p = zput(p, path); p = zput(p, "\r\n");
            }
            if (!nread) {                          /* fallback to embedded opcodes */
                for (i = 0; i < sizeof(prog); ++i) filebuf[i] = prog[i];
                nread = sizeof(prog);
                p = zput(p, "STAGE3: using embedded program bytes\r\n");
            }

            /* Place the image: MZ .EXE (header + relocations + CS:IP/SS:SP) or flat .COM. */
            if (nread >= 0x1C && filebuf[0] == 'M' && filebuf[1] == 'Z') {
                const WORD *h = (const WORD *)filebuf;
                DWORD hdrsize  = (DWORD)h[4] * 16;            /* e_cparhdr paragraphs   */
                DWORD e_crlc   = h[3];                        /* relocation count       */
                DWORD e_lfarlc = h[12];                       /* reloc table offset     */
                DWORD load_seg = PSP_SEG + 0x10;             /* image loads after PSP  */
                volatile BYTE *img = (volatile BYTE *)((DWORD)load_seg << 4); /* 0x1100 */
                DWORD totalused = h[2] ? ((DWORD)(h[2]-1)*512 + (h[1] ? h[1] : 512)) : nread;
                DWORD imgsize;
                if (totalused > nread) totalused = nread;
                imgsize = totalused > hdrsize ? totalused - hdrsize : 0;
                for (i = 0; i < imgsize; ++i) img[i] = filebuf[hdrsize + i];
                for (i = 0; i < e_crlc; ++i) {               /* apply relocations      */
                    DWORD ro = *(const WORD *)(filebuf + e_lfarlc + i*4);
                    DWORD rs = *(const WORD *)(filebuf + e_lfarlc + i*4 + 2);
                    volatile WORD *loc = (volatile WORD *)((((DWORD)load_seg + rs) << 4) + ro);
                    *loc = (WORD)(*loc + load_seg);
                }
                ecs = (WORD)(load_seg + h[11]); eip = h[10]; /* e_cs/e_ip */
                ess = (WORD)(load_seg + h[7]);  esp = h[8];  /* e_ss/e_sp */
                is_exe = 1;
                p = zput(p, "  MZ .EXE: hdr=0x"); p = zhex(p, hdrsize);
                p = zput(p, " img=0x"); p = zhex(p, imgsize);
                p = zput(p, " relocs=0x"); p = zhex(p, e_crlc);
                p = zput(p, " CS:IP=0x"); p = zhex(p, ecs); p = zput(p, ":0x"); p = zhex(p, eip);
                p = zput(p, " SS:SP=0x"); p = zhex(p, ess); p = zput(p, ":0x"); p = zhex(p, esp);
                p = zput(p, "\r\n");
            } else {                                         /* flat .COM at PSP:0x100 */
                DWORD n = nread > 0xFE00 ? 0xFE00 : nread;
                for (i = 0; i < n; ++i) code[i] = filebuf[i];
                ecs = PSP_SEG; eip = 0x100; ess = PSP_SEG; esp = 0xFFFE;
            }

            /* INT 21h handler stub (BOP) + IVT vector. */
            for (i = 0; i < sizeof(bop); ++i) hdlr[i] = bop[i];
            *(volatile WORD *)0x84 = 0x0000;                /* IVT[0x21].offset  */
            *(volatile WORD *)0x86 = HDLR_SEG;              /* IVT[0x21].segment */
            /* Empty DBCS lead-byte table for INT 21h AH=63h: on US/non-DBCS DOS the
               table is a single 0000 terminator. Parked just past the 4-byte BOP in
               HDLR_SEG (linear 0x510); returned to guests as HDLR_SEG:0x10. */
            hdlr[0x10] = 0; hdlr[0x11] = 0;

            /* Minimal (empty) environment block. */
            env[0] = 0; env[1] = 0; env[2] = 0;

            /* Program Segment Prefix. */
            for (i = 0; i < 0x100; ++i) psp[i] = 0;
            psp[0x00] = 0xCD; psp[0x01] = 0x20;                    /* INT 20h          */
            *(volatile WORD *)(psp + 0x02) = 0xA000;               /* top-of-mem seg   */
            psp[0x18]=1; psp[0x19]=1; psp[0x1A]=1; psp[0x1B]=0; psp[0x1C]=2; /* JFT     */
            for (i = 0x1D; i < 0x2C; ++i) psp[i] = 0xFF;
            *(volatile WORD *)(psp + 0x2C) = ENV_SEG;              /* env segment      */
            *(volatile WORD *)(psp + 0x32) = 0x14;                 /* JFT size         */
            *(volatile WORD *)(psp + 0x34) = 0x18;                 /* JFT ptr off      */
            *(volatile WORD *)(psp + 0x36) = PSP_SEG;              /* JFT ptr seg      */
            *(volatile DWORD *)(psp + 0x38) = 0xFFFFFFFF;          /* prev PSP         */
            psp[0x50] = 0xCD; psp[0x51] = 0x21; psp[0x52] = 0xCB;  /* INT 21h ; RETF   */
            /* Command tail: EMPTY (length 0) until real args are wired -- CmdLine isn't yet
               populated by GetNextVDMCommand (the deferred M2.5 problem). A bogus tail makes
               real programs misparse their args (mem.exe HELLO -> "Parse Error"), so no-args
               is the safe default; psp[0x81] holds the 0x0D the parser scans for. */
            { psp[0x80] = 0; psp[0x81] = 0x0D; }

            /* M2.4: lay down the MCB chain over conventional memory. Three blocks,
               physically contiguous from ENV_SEG-1 up to the 0xA000 (640KB) top:
                 0x005F  env block      -> data at ENV_SEG (0x60), owned by our PSP
                 0x0070  DOS resident    -> filler standing in for resident DOS
                 0x00FF  program block   -> PSP+image; DOS-style it owns ALL memory
                                            to the top ('Z' = last), which is exactly
                                            why compiled .EXEs call AH=4Ah at startup
                                            to shrink it and free the tail.
               (0x5F+1+0x10=0x70; 0x70+1+0x8E=0xFF; 0xFF+1+0x9F00=0xA000.) */
            MCB_SET(0x005F, 'M', PSP_SEG, 0x0010);
            MCB_SET(0x0070, 'M', 0x0008,  0x008E);
            MCB_SET(0x00FF, 'Z', PSP_SEG, 0x9F00);
            g_first_mcb = 0x005F;
            g_dta_seg = PSP_SEG; g_dta_off = 0x0080;       /* default DTA = PSP:0x80 */

            /* Entry context. DS=ES=PSP_SEG (point at the PSP); CS:IP/SS:SP from the loader
               (.COM: PSP_SEG / 0x100 / 0xFFFE; .EXE: from the MZ header). AX=0 (drives valid). */
            #define TF(off) (*(volatile DWORD *)(tib + (off)))
            TF(0x2D8) = 0x10007;
            TF(0x364) = PSP_SEG; TF(0x368) = PSP_SEG; TF(0x36C) = PSP_SEG; TF(0x370) = PSP_SEG;
            TF(0x374) = 0; TF(0x378) = 0; TF(0x37C) = 0; TF(0x380) = 0;
            TF(0x384) = 0; TF(0x388) = 0; TF(0x38C) = 0;
            TF(0x390) = eip;                                /* Eip   */
            TF(0x394) = ecs;                                /* SegCs */
            TF(0x398) = 0x20002;                            /* EFlags: VM + reserved, IOPL=0 */
            TF(0x39C) = esp;                                /* Esp   */
            TF(0x3A0) = ess;                                /* SegSs */
            if (!is_exe)                                    /* .COM: push 0 -> near RET hits PSP:0 */
                *(volatile WORD *)(((DWORD)PSP_SEG << 4) + 0xFFFE) = 0;
            p = zput(p, is_exe ? "STAGE3: running DOS .EXE (entry 0x"
                               : "STAGE3: running DOS .COM (entry 0x");
            p = zhex(p, ecs); p = zput(p, ":0x"); p = zhex(p, eip); p = zput(p, ")...\r\n");
            writelog(report, p);
            base = p;                      /* everything up to here is on disk; the loop
                                              appends from here and resets p each pass */

            SetCurrentDirectoryA(g_cur);   /* DOS relative paths resolve against CurDir */
            #define R_AX TF(0x388)
            #define R_BX TF(0x37C)
            #define R_CX TF(0x384)
            #define R_DX TF(0x380)
            #define R_DS TF(0x370)
            #define R_ES TF(0x36C)
            #define R_SI TF(0x378)
            #define OUTC(c)  do { if (op < dosout + sizeof(dosout) - 1) *op++ = (char)(c); } while (0)
            #define SETAX(v) (R_AX = (R_AX & 0xFFFF0000u) | ((DWORD)(v) & 0xFFFF))
            #define SET16(r, v) ((r) = ((r) & 0xFFFF0000u) | ((DWORD)(v) & 0xFFFF))
            /* CF is returned via the FLAGS the INT pushed on the V86 stack (SS:SP+4),
               because the handler's IRET restores FLAGS from there -- the live EFlags
               we'd set in the context get clobbered. AX et al. persist normally. */
            #define OKCF()   (*pfl &= (WORD)~1)
            #define ERRCF()  (*pfl |= 1)
            for (guard = 0; guard < 4000; ++guard) {
                volatile WORD *pfl;
                st = ntvdmctl(0, NULL);
                ev = TF(0x5a8);
                if (ev != 4) {                  /* fault / other -> stop */
                    p = zput(p, "STAGE3: stop, event=0x"); p = zhex(p, ev);
                    p = zput(p, " CS:IP=0x"); p = zhex(p, TF(0x394));
                    p = zput(p, ":0x"); p = zhex(p, TF(0x390));
                    p = zput(p, " info[0x5b0]=0x"); p = zhex(p, TF(0x5b0));
                    p = zput(p, "\r\n"); break;
                }
                pfl = (volatile WORD *)(((TF(0x3A0) & 0xFFFF) << 4)
                                        + (((TF(0x39C) & 0xFFFF) + 4) & 0xFFFF));
                ah = (R_AX >> 8) & 0xFF;
                if (ah == 0x4C) {                       /* terminate */
                    p = zput(p, "  ==> DOS terminate (AH=4Ch), exit code AL=0x");
                    p = zhex(p, R_AX & 0xFF); p = zput(p, "\r\n");
                    break;
                } else if (ah == 0x02) {                /* print char DL */
                    OUTC(R_DX & 0xFF); OKCF();
                } else if (ah == 0x09) {                /* print $-string DS:DX */
                    const volatile BYTE *s = (const volatile BYTE *)((R_DS << 4) + (R_DX & 0xFFFF));
                    int k; for (k = 0; k < 1024 && *s != '$'; ++k, ++s) OUTC(*s);
                    OKCF();
                } else if (ah == 0x40) {                /* write: BX=handle CX=cnt DS:DX=buf */
                    DWORD h = R_BX & 0xFFFF, cnt = R_CX & 0xFFFF;
                    const char *b = (const char *)((R_DS << 4) + (R_DX & 0xFFFF));
                    if (h == 1 || h == 2) { DWORD k; for (k = 0; k < cnt; ++k) OUTC(b[k]); SETAX(cnt); OKCF(); }
                    else if (h < 24 && g_fh[h]) { DWORD w = 0; WriteFile(g_fh[h], b, cnt, &w, NULL); SETAX(w); OKCF(); }
                    else { SETAX(6); ERRCF(); }
                } else if (ah == 0x3C || ah == 0x3D) {  /* create / open: DS:DX=ASCIIZ name */
                    char fn[300]; DWORD slot; HANDLE f;
                    v86_str(R_DS, R_DX, fn, sizeof(fn));
                    if (ah == 0x3C)
                        f = CreateFileA(fn, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ,
                                        NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
                    else {
                        DWORD m = R_AX & 3;
                        DWORD acc = (m == 1) ? GENERIC_WRITE
                                  : (m == 2) ? (GENERIC_READ | GENERIC_WRITE) : GENERIC_READ;
                        f = CreateFileA(fn, acc, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                                        FILE_ATTRIBUTE_NORMAL, NULL);
                    }
                    if (f != INVALID_HANDLE_VALUE) {
                        for (slot = 5; slot < 24 && g_fh[slot]; ++slot) {}
                        if (slot < 24) { g_fh[slot] = f; SETAX(slot); OKCF(); }
                        else { CloseHandle(f); SETAX(4); ERRCF(); }
                    } else { SETAX(2); ERRCF(); }
                    p = zput(p, "  INT21 AH=0x"); p = zhex(p, ah);
                    p = zput(p, " ["); p = zput(p, fn); p = zput(p, "] -> AX=0x");
                    p = zhex(p, R_AX & 0xFFFF); p = zput(p, (*pfl & 1) ? " (err)\r\n" : "\r\n");
                } else if (ah == 0x3E) {                /* close: BX=handle */
                    DWORD h = R_BX & 0xFFFF;
                    if (h >= 5 && h < 24 && g_fh[h]) { CloseHandle(g_fh[h]); g_fh[h] = 0; }
                    OKCF();
                } else if (ah == 0x3F) {                /* read: BX=handle CX=cnt ->DS:DX */
                    DWORD h = R_BX & 0xFFFF, cnt = R_CX & 0xFFFF, rd = 0;
                    void *b = (void *)((R_DS << 4) + (R_DX & 0xFFFF));
                    if (h >= 5 && h < 24 && g_fh[h]) { ReadFile(g_fh[h], b, cnt, &rd, NULL); SETAX(rd); OKCF(); }
                    else if (h == 0) { SETAX(0); OKCF(); }   /* stdin: EOF for now */
                    else { SETAX(6); ERRCF(); }
                } else if (ah == 0x42) {                /* lseek: AL=org BX=h CX:DX=off */
                    DWORD h = R_BX & 0xFFFF, meth = R_AX & 0xFF;
                    LONG dist = (LONG)(((R_CX & 0xFFFF) << 16) | (R_DX & 0xFFFF));
                    if (h >= 5 && h < 24 && g_fh[h]) {
                        DWORD np = SetFilePointer(g_fh[h], dist, NULL, meth);
                        SETAX(np & 0xFFFF);
                        R_DX = (R_DX & 0xFFFF0000u) | ((np >> 16) & 0xFFFF); OKCF();
                    } else { SETAX(6); ERRCF(); }
                } else if (ah == 0x30) {                /* get DOS version -> 5.0 */
                    SETAX(0x0005); OKCF();
                } else if (ah == 0x44) {                /* IOCTL (C-runtime isatty etc.) */
                    BYTE al = (BYTE)(R_AX & 0xFF);
                    WORD bx = (WORD)(R_BX & 0xFFFF);
                    if (al == 0x00) {                   /* get device info -> DX */
                        /* handles 0..4 = character device/console (bit7+console bits);
                           file handles (slots 5+) = block device on drive C: (=2). */
                        SET16(R_DX, (bx < 5) ? 0x80D3 : 0x0002); OKCF();
                    } else if (al == 0x06 || al == 0x07) { /* in/out status -> AL=FF (ready) */
                        SETAX((R_AX & 0xFF00) | 0xFF); OKCF();
                    } else { OKCF(); }                  /* other subfns: benign success */
                    p = zput(p, "  INT21 AH=44 ioctl AL=0x"); p = zhex(p, al);
                    p = zput(p, " BX=0x"); p = zhex(p, bx); p = zput(p, "\r\n");
                } else if (ah == 0x63) {                /* get DBCS lead-byte table */
                    if ((R_AX & 0xFF) == 0) {           /* AL=0: return DS:SI -> empty table */
                        SET16(R_DS, HDLR_SEG); SET16(R_SI, 0x10);
                    }
                    SETAX(R_AX & 0xFF00);               /* AL=0 = success */
                    OKCF();
                    p = zput(p, "  INT21 AH=63 DBCS lead-byte table\r\n");
                } else if (ah == 0x25) {                /* set interrupt vector: AL=int DS:DX=hdlr */
                    DWORD v = (R_AX & 0xFF) * 4;
                    *(volatile WORD *)(v)     = (WORD)(R_DX & 0xFFFF);
                    *(volatile WORD *)(v + 2) = (WORD)(R_DS & 0xFFFF);
                    OKCF();
                } else if (ah == 0x35) {                /* get interrupt vector: AL=int -> ES:BX */
                    DWORD v = (R_AX & 0xFF) * 4;
                    SET16(R_BX, *(volatile WORD *)(v));
                    SET16(R_ES, *(volatile WORD *)(v + 2));
                    OKCF();
                } else if (ah == 0x48) {                /* allocate BX paras -> AX=seg (err: BX=max) */
                    WORD want = (WORD)(R_BX & 0xFFFF), m = g_first_mcb, biggest = 0;
                    int done = 0;
                    for (;;) {
                        volatile BYTE *mc = (volatile BYTE *)((DWORD)m << 4);
                        BYTE sig = mc[0]; WORD own = *(volatile WORD *)(mc + 1);
                        WORD sz = *(volatile WORD *)(mc + 3);
                        if (own == 0) {                 /* free block */
                            if (sz > biggest) biggest = sz;
                            if (sz >= want) {
                                if (sz >= (WORD)(want + 1)) {   /* split off a free tail */
                                    volatile BYTE *nm =
                                        (volatile BYTE *)(((DWORD)m + 1 + want) << 4);
                                    nm[0] = sig;                /* tail inherits last-status */
                                    *(volatile WORD *)(nm + 1) = 0;
                                    *(volatile WORD *)(nm + 3) = (WORD)(sz - want - 1);
                                    nm[8] = 0;
                                    mc[0] = 'M';
                                    *(volatile WORD *)(mc + 3) = want;
                                }
                                *(volatile WORD *)(mc + 1) = PSP_SEG;
                                SET16(R_AX, m + 1); OKCF(); done = 1;
                            }
                        }
                        if (done || sig == 'Z') break;
                        m = (WORD)(m + 1 + sz);
                    }
                    if (!done) { SET16(R_AX, 8); SET16(R_BX, biggest); ERRCF(); }
                    p = zput(p, "  INT21 AH=48 alloc 0x"); p = zhex(p, want);
                    p = zput(p, (*pfl & 1) ? " -> err max=0x" : " -> seg=0x");
                    p = zhex(p, (*pfl & 1) ? biggest : (R_AX & 0xFFFF)); p = zput(p, "\r\n");
                } else if (ah == 0x49) {                /* free block: ES=segment */
                    WORD m = (WORD)((R_ES & 0xFFFF) - 1);
                    volatile BYTE *mc = (volatile BYTE *)((DWORD)m << 4);
                    if (mc[0] == 'M' || mc[0] == 'Z') {
                        *(volatile WORD *)(mc + 1) = 0;         /* mark free */
                        while (mc[0] == 'M') {                  /* coalesce forward */
                            WORD sz = *(volatile WORD *)(mc + 3);
                            volatile BYTE *nm =
                                (volatile BYTE *)(((DWORD)m + 1 + sz) << 4);
                            if (*(volatile WORD *)(nm + 1) != 0) break;
                            if (nm[0] != 'M' && nm[0] != 'Z') break;
                            *(volatile WORD *)(mc + 3) =
                                (WORD)(sz + 1 + *(volatile WORD *)(nm + 3));
                            mc[0] = nm[0];
                        }
                        OKCF();
                    } else { SET16(R_AX, 9); ERRCF(); }
                    p = zput(p, "  INT21 AH=49 free seg=0x"); p = zhex(p, R_ES & 0xFFFF);
                    p = zput(p, (*pfl & 1) ? " (err)\r\n" : "\r\n");
                } else if (ah == 0x4A) {                /* resize: ES=block BX=new paras */
                    WORD m = (WORD)((R_ES & 0xFFFF) - 1), want = (WORD)(R_BX & 0xFFFF);
                    volatile BYTE *mc = (volatile BYTE *)((DWORD)m << 4);
                    if (mc[0] != 'M' && mc[0] != 'Z') { SET16(R_AX, 9); ERRCF(); }
                    else {
                        WORD cur = *(volatile WORD *)(mc + 3); BYTE sig = mc[0]; int ok = 0;
                        if (want <= cur) {                      /* shrink (free the tail) */
                            if ((WORD)(cur - want) >= 1) {
                                volatile BYTE *nm =
                                    (volatile BYTE *)(((DWORD)m + 1 + want) << 4);
                                nm[0] = sig; *(volatile WORD *)(nm + 1) = 0;
                                *(volatile WORD *)(nm + 3) = (WORD)(cur - want - 1);
                                nm[8] = 0; mc[0] = 'M'; *(volatile WORD *)(mc + 3) = want;
                            }
                            ok = 1;
                        } else if (sig == 'M') {                /* grow into a free neighbour */
                            volatile BYTE *nm =
                                (volatile BYTE *)(((DWORD)m + 1 + cur) << 4);
                            if (*(volatile WORD *)(nm + 1) == 0) {
                                WORD avail = (WORD)(cur + 1 + *(volatile WORD *)(nm + 3));
                                if (avail >= want) {
                                    if ((WORD)(avail - want) >= 1) {
                                        volatile BYTE *n2 =
                                            (volatile BYTE *)(((DWORD)m + 1 + want) << 4);
                                        n2[0] = nm[0]; *(volatile WORD *)(n2 + 1) = 0;
                                        *(volatile WORD *)(n2 + 3) = (WORD)(avail - want - 1);
                                        n2[8] = 0; *(volatile WORD *)(mc + 3) = want; mc[0] = 'M';
                                    } else { *(volatile WORD *)(mc + 3) = avail; mc[0] = nm[0]; }
                                    ok = 1;
                                }
                            }
                        }
                        if (!ok) {                              /* fail: BX = max available */
                            WORD max = cur;
                            if (sig == 'M') {
                                volatile BYTE *nm =
                                    (volatile BYTE *)(((DWORD)m + 1 + cur) << 4);
                                if (*(volatile WORD *)(nm + 1) == 0)
                                    max = (WORD)(cur + 1 + *(volatile WORD *)(nm + 3));
                            }
                            SET16(R_AX, 8); SET16(R_BX, max); ERRCF();
                        } else OKCF();
                    }
                    p = zput(p, "  INT21 AH=4A resize seg=0x"); p = zhex(p, R_ES & 0xFFFF);
                    p = zput(p, " -> 0x"); p = zhex(p, want);
                    p = zput(p, (*pfl & 1) ? " (err)\r\n" : "\r\n");
                } else if (ah == 0x51 || ah == 0x62) {  /* get current PSP -> BX */
                    SET16(R_BX, PSP_SEG); OKCF();
                } else if (ah == 0x50) {                /* set current PSP (single process) */
                    OKCF();
                } else if (ah == 0x1A) {                /* set DTA = DS:DX */
                    g_dta_seg = (WORD)(R_DS & 0xFFFF); g_dta_off = (WORD)(R_DX & 0xFFFF);
                    OKCF();
                } else if (ah == 0x2F) {                /* get DTA -> ES:BX */
                    SET16(R_ES, g_dta_seg); SET16(R_BX, g_dta_off); OKCF();
                } else if (ah == 0x19) {                /* get current drive -> AL (C: = 2) */
                    SETAX((R_AX & 0xFF00) | 0x02); OKCF();
                } else if (ah == 0x0E) {                /* select drive -> AL = #drives */
                    SETAX((R_AX & 0xFF00) | 0x03); OKCF();
                } else if (ah == 0x0D) {                /* disk reset (flush) -> nop */
                    OKCF();
                } else if (ah == 0x33) {                /* get/set Ctrl-Break -> off */
                    if ((R_AX & 0xFF) == 0) SET16(R_DX, 0);
                    OKCF();
                } else {                                /* unhandled service */
                    p = zput(p, "  INT21 AH=0x"); p = zhex(p, ah); p = zput(p, " unhandled\r\n");
                    ERRCF();
                }
                TF(0x390) += 3;                         /* advance past the 3-byte BOP -> IRET */
                applog(base, p); p = base;              /* flush this pass; bound the buffer */
            }
            applog(base, p); p = base;                  /* flush any break-path trace line */
            #undef R_AX
            #undef R_BX
            #undef R_CX
            #undef R_DX
            #undef R_DS
            #undef R_ES
            #undef R_SI
            #undef OUTC
            #undef SETAX
            #undef SET16
            #undef OKCF
            #undef ERRCF
            *op = 0;
            if (op != dosout) {
                DWORD wr;
                if (hcon != INVALID_HANDLE_VALUE)
                    WriteFile(hcon, dosout, (DWORD)(op - dosout), &wr, NULL);
                p = zput(p, "  ==> DOS OUTPUT: ["); p = zput(p, dosout); p = zput(p, "]\r\n");
            }
            if (hcon != INVALID_HANDLE_VALUE) CloseHandle(hcon);
            applog(base, p); p = base;          /* append DOS output; loop trace already on disk */
            #undef TF
        }
    }

    /* STAGE 2: complete. Appended (the loop trace was streamed incrementally via
       applog, so we must not overwrite it). Log-only -- no MessageBox, so we never
       block in a non-interactive window station; the file is the channel. */
    { char s2[24]; applog(s2, zput(s2, "STAGE2: complete\r\n")); }
    return 0;
}
