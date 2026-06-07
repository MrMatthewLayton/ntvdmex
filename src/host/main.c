/* main.c -- the clean DOS VDM host (console). Orchestrates the pipeline the
 * tools/vdmhost spike proved, now wired through the src/ modules:
 *   log -> CSRSS handshake (csrss) -> V86 bring-up (v86) -> load + build the DOS
 *   process (dos_loader/dos_psp/dos_mcb) -> service INT 21h in V86 (dos_int21).
 * No CRT (src/runtime.c supplies the entry + mem*); imports only XP system DLLs.
 *
 * Video/GUI is deliberately out of scope here (that is M3, the Luna window); this
 * host writes DOS output to the console and a trace to LOG_PATH.
 */
#include <windows.h>
#include "ntvdm.h"
#include "v86.h"
#include "csrss.h"
#include "log.h"
#include "dos_mcb.h"
#include "dos_loader.h"
#include "dos_psp.h"
#include "dos_env.h"
#include "dos_int21.h"
#include "dos_layout.h"
#include "vdd_bus.h"
#include "vdd_pit.h"

#define LOG_PATH    "C:\\ntvdmex\\ntvdmhost.log"
#define TARGET_PATH "C:\\ntvdmex\\target.txt"

/* CSRSS receive buffers + program image (no CRT heap; static = zero-init). */
static char g_cmd[1024], g_app[1024], g_cur[512], g_pif[512];
static char g_env[8192], g_desk[512], g_title[512], g_rsv[512];
static VDM_COMMAND_INFO g_ci;
static BYTE filebuf[0x20000];

/* M3 slice-1b: the device bus + its VDDs live for the life of the host. */
static vdd_bus  g_bus;
static pit_state g_pit;
static ntvdd     g_pit_dev;
static int       g_irq_pending = -1;        /* last IRQ a VDD raised (spike obs) */

static void host_irq_sink(void *ctx, uint8_t irq) { (void)ctx; g_irq_pending = irq; }

/* Decode + service a V86 IN/OUT that #GP-faulted (event 2), dispatch it to the
   bus, and advance EIP past the instruction so the guest resumes. Returns 1 if
   the faulting instruction was a (supported) I/O op we handled, 0 if it was a
   genuine GP fault the caller should stop on. Appends a trace line via *tpp. */
static int host_try_io(volatile BYTE *tib, vdd_bus *bus, char **tpp)
{
    DWORD cs = VDM_REG(tib, VTIB_CS)  & 0xFFFF;
    DWORD ip = VDM_REG(tib, VTIB_EIP) & 0xFFFF;
    volatile BYTE *code = (volatile BYTE *)((cs << 4) + ip);   /* absolute V86 */
    char *p = *tpp;
    int i = 0, opsize = 2, is_in, width, used_dx, len;
    BYTE op; uint16_t port; uint32_t val, eax;

    while (code[i] == 0x66 || code[i] == 0x67 ||
           code[i] == 0xF2 || code[i] == 0xF3) {        /* prefixes            */
        if (code[i] == 0x66) opsize = 4;
        if (++i > 4) return 0;
    }
    op = code[i];
    switch (op) {
    case 0xE4: is_in = 1; width = 1;      used_dx = 0; break;  /* IN  AL,ib    */
    case 0xE5: is_in = 1; width = opsize; used_dx = 0; break;  /* IN  eAX,ib   */
    case 0xE6: is_in = 0; width = 1;      used_dx = 0; break;  /* OUT ib,AL    */
    case 0xE7: is_in = 0; width = opsize; used_dx = 0; break;  /* OUT ib,eAX   */
    case 0xEC: is_in = 1; width = 1;      used_dx = 1; break;  /* IN  AL,DX    */
    case 0xED: is_in = 1; width = opsize; used_dx = 1; break;  /* IN  eAX,DX   */
    case 0xEE: is_in = 0; width = 1;      used_dx = 1; break;  /* OUT DX,AL    */
    case 0xEF: is_in = 0; width = opsize; used_dx = 1; break;  /* OUT DX,eAX   */
    default:   return 0;                       /* not an I/O op -> real fault  */
    }
    if (used_dx) { port = (uint16_t)VDM_REG(tib, VTIB_EDX); len = i + 1; }
    else         { port = code[i + 1];                      len = i + 2; }

    eax = VDM_REG(tib, VTIB_EAX);
    if (is_in) {
        val = 0;
        vdd_bus_io(bus, port, (uint8_t)width, 1, &val);
        if (width == 1)      VDM_REG(tib, VTIB_EAX) = (eax & 0xFFFFFF00u) | (val & 0xFF);
        else if (width == 2) VDM_REG(tib, VTIB_EAX) = (eax & 0xFFFF0000u) | (val & 0xFFFF);
        else                 VDM_REG(tib, VTIB_EAX) = val;
        p = zput(p, "  IO in  port=0x"); p = zhex(p, port);
        p = zput(p, " w="); p = zhex(p, (unsigned)width);
        p = zput(p, " -> 0x"); p = zhex(p, val); p = zput(p, "\r\n");
    } else {
        val = (width == 1) ? (eax & 0xFF) : (width == 2) ? (eax & 0xFFFF) : eax;
        vdd_bus_io(bus, port, (uint8_t)width, 0, &val);
        p = zput(p, "  IO out port=0x"); p = zhex(p, port);
        p = zput(p, " w="); p = zhex(p, (unsigned)width);
        p = zput(p, " val=0x"); p = zhex(p, val); p = zput(p, "\r\n");
    }
    VDM_REG(tib, VTIB_EIP) = (ip + len) & 0xFFFF;          /* step past I/O    */
    *tpp = p;
    return 1;
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR lpCmd, int nShow)
{
    char report[8192]; char *p = report; char *base;
    volatile BYTE *tib, *hdlr;
    DWORD nread = 0, err = 0, ev; LONG st;
    dos_image_t img;
    dos_machine_t m;
    char dosout[1024];
    char progpath[768]; char args[256];
    unsigned i; int guard;
    static const BYTE bop[] = { VDM_BOP0, VDM_BOP1, 0x20, 0xCF };  /* BOP 0x20 ; iret */

    (void)hInst; (void)hPrev; (void)lpCmd; (void)nShow;
    progpath[0] = 0; args[0] = 0;

    p = zput(p, "NTVDMEX clean host\r\nSTAGE0: WinMain entered\r\n");
    log_write(LOG_PATH, report, p);

    /* CSRSS command-info: receive buffers + first-command state + IFEO task id. */
    g_ci.CmdLine = g_cmd; g_ci.CmdLen = sizeof(g_cmd);
    g_ci.AppName = g_app; g_ci.AppLen = sizeof(g_app);
    g_ci.PifFile = g_pif; g_ci.PifLen = sizeof(g_pif);
    g_ci.CurDirectory = g_cur; g_ci.CurDirectoryLen = sizeof(g_cur);
    g_ci.Env = g_env; g_ci.EnvLen = sizeof(g_env);
    g_ci.Desktop = g_desk; g_ci.DesktopLen = sizeof(g_desk);
    g_ci.Title = g_title; g_ci.TitleLen = sizeof(g_title);
    g_ci.Reserved = g_rsv; g_ci.ReservedLen = sizeof(g_rsv);
    g_ci.StartupInfo.cb = sizeof(STARTUPINFOA);
    g_ci.VDMState = VDM_GET_FIRST_COMMAND;
    g_ci.TaskId   = csrss_parse_taskid(GetCommandLineA());

    /* V86 address space, then register as a VDM with the kernel (order matters). */
    v86_setup_memory();
    st = v86_init();
    p = zput(p, "STAGE1: v86_init NTSTATUS=0x"); p = zhex(p, (unsigned)st); p = zput(p, "\r\n");

    /* CSRSS: register as the console VDM, then fetch the program to run. */
    csrss_register_console();
    if (csrss_get_command(&g_ci, &err)) {
        p = zput(p, "STAGE1: program "); p = zput(p, g_cur);
        p = zput(p, "\\"); p = zput(p, g_title); p = zput(p, "\r\n");
    } else {
        p = zput(p, "STAGE1: GetNextVDMCommand FALSE err=0x"); p = zhex(p, err); p = zput(p, "\r\n");
    }
    log_write(LOG_PATH, report, p);

    tib = v86_get_tib();
    if (!tib) {
        p = zput(p, "STAGE1: no VDM_TIB -- abort\r\n"); log_append(LOG_PATH, report, p);
        return 1;
    }

    /* Load the program: C:\ntvdmex\target.txt override, else CurDir\Title, else a
       tiny exit stub. (target.txt decouples DOS-kernel tests from Title recovery.) */
    {
        HANDLE ht = CreateFileA(TARGET_PATH, GENERIC_READ, FILE_SHARE_READ, NULL,
                                OPEN_EXISTING, 0, NULL);
        if (ht != INVALID_HANDLE_VALUE) {
            char tpath[512]; DWORD tn = 0; char *q; char *a = 0;
            ReadFile(ht, tpath, sizeof(tpath) - 1, &tn, NULL); CloseHandle(ht);
            tpath[tn < sizeof(tpath) ? tn : sizeof(tpath) - 1] = 0;
            for (q = tpath; *q; ++q) {                  /* split "path [args]"; stop at CR/LF */
                if (*q == '\r' || *q == '\n') { *q = 0; break; }
                if (*q == ' ' && !a) { *q = 0; a = q + 1; }
            }
            if (tpath[0]) {
                HANDLE hf = CreateFileA(tpath, GENERIC_READ, FILE_SHARE_READ, NULL,
                                        OPEN_EXISTING, 0, NULL);
                if (hf != INVALID_HANDLE_VALUE) { ReadFile(hf, filebuf, sizeof(filebuf), &nread, NULL); CloseHandle(hf); }
                zput(progpath, tpath);                  /* env argv[0] */
                if (a) zput(args, a);                   /* PSP command tail */
                p = zput(p, "STAGE2: target.txt loaded 0x"); p = zhex(p, nread);
                p = zput(p, " from "); p = zput(p, tpath);
                if (a && a[0]) { p = zput(p, " args=["); p = zput(p, a); p = zput(p, "]"); }
                p = zput(p, "\r\n");
            }
        }
    }
    if (!nread && g_cur[0] && g_title[0]) {
        char path[768]; char *pp = path; HANDLE hf;
        pp = zput(pp, g_cur); pp = zput(pp, "\\"); pp = zput(pp, g_title);
        hf = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
        if (hf != INVALID_HANDLE_VALUE) { ReadFile(hf, filebuf, sizeof(filebuf), &nread, NULL); CloseHandle(hf); }
        zput(progpath, path);                       /* env argv[0] */
        if (g_cmd[0]) zput(args, g_cmd);            /* best-effort: CmdLine if CSRSS populated it */
        p = zput(p, "STAGE2: loaded 0x"); p = zhex(p, nread);
        p = zput(p, " from "); p = zput(p, path); p = zput(p, "\r\n");
    }
    if (!nread) {
        static const BYTE stub[] = { 0xB4, 0x4C, 0xCD, 0x21 };   /* mov ah,4Ch; int 21h */
        for (i = 0; i < sizeof(stub); ++i) filebuf[i] = stub[i];
        nread = sizeof(stub);
        p = zput(p, "STAGE2: embedded fallback\r\n");
    }

    /* Build the DOS process in conventional memory (base=NULL => absolute V86). */
    img = dos_load(NULL, filebuf, nread, DOS_PSP_SEG);

    hdlr = (volatile BYTE *)(DOS_HDLR_SEG << 4);            /* INT 21h BOP handler */
    for (i = 0; i < sizeof(bop); ++i) hdlr[i] = bop[i];
    *(volatile WORD *)0x84 = 0x0000;                        /* IVT[0x21].offset    */
    *(volatile WORD *)0x86 = DOS_HDLR_SEG;                  /* IVT[0x21].segment   */
    hdlr[DOS_DBCS_OFF] = 0; hdlr[DOS_DBCS_OFF + 1] = 0;     /* empty DBCS table    */

    dos_psp_build(NULL, DOS_PSP_SEG, DOS_ENV_SEG, DOS_MEM_TOP);
    dos_env_build(NULL, DOS_ENV_SEG, progpath[0] ? progpath : "C:\\PROGRAM.COM");  /* M2.5: env */
    dos_cmdtail_build(NULL, DOS_PSP_SEG, args);                                    /* M2.5: args */
    dos_int21_init(&m, dos_mcb_init(NULL));

    /* M3 slice-1b: stand up the device bus (NULL base => absolute V86 addresses)
       and add the PIT VDD, which claims ports 0x40-0x43 + INT 08h/1Ah. I/O on
       those ports now reflects as a #GP (event 2) and dispatches through the bus;
       the INT/IRQ wiring (IVT BOP stubs + ICA delivery) is the next sub-step. */
    vdd_bus_init(&g_bus, NULL);
    vdd_bus_set_sinks(&g_bus, host_irq_sink, NULL, NULL, NULL);
    g_pit_dev = vdd_pit_device(&g_pit);
    vdd_bus_add(&g_bus, &g_pit_dev);

    v86_set_entry(tib, img.cs, img.ip, img.ss, img.sp, DOS_PSP_SEG);
    if (!img.is_exe)                                        /* .COM near-ret guard */
        *(volatile WORD *)(((DWORD)DOS_PSP_SEG << 4) + 0xFFFE) = 0;

    p = zput(p, img.is_exe ? "STAGE2: running .EXE (entry 0x"
                           : "STAGE2: running .COM (entry 0x");
    p = zhex(p, img.cs); p = zput(p, ":0x"); p = zhex(p, img.ip); p = zput(p, ")...\r\n");
    log_write(LOG_PATH, report, p);
    base = p;                       /* preamble is on disk; the loop appends from here */

    SetCurrentDirectoryA(g_cur);    /* DOS relative paths resolve against CurDir */

    /* Service loop: run V86 until a BOP, dispatch INT 21h, step past the BOP, re-enter. */
    m.tib = tib; m.out = dosout; m.out_cap = sizeof(dosout); m.out_len = 0;
    for (guard = 0; guard < 4000; ++guard) {
        ev = v86_run(tib, &st);
        /* I/O #GP (event 2): if it's an IN/OUT we can decode, service it via the
           VDD bus and resume; otherwise it's a genuine fault -> fall through. */
        if (ev == VDM_EVENT_GPFAULT && host_try_io(tib, &g_bus, &p)) {
            log_append(LOG_PATH, base, p); p = base;
            continue;
        }
        if (ev != VDM_EVENT_BOP) {
            DWORD csv = VDM_REG(tib, VTIB_CS) & 0xFFFF;
            DWORD ipv = VDM_REG(tib, VTIB_EIP) & 0xFFFF;
            volatile BYTE *cp = (volatile BYTE *)((csv << 4) + ipv);
            BYTE ib[8]; unsigned k;
            for (k = 0; k < 8; ++k) ib[k] = cp[k];
            p = zput(p, "STAGE2: stop event=0x"); p = zhex(p, ev);
            p = zput(p, " status=0x"); p = zhex(p, (unsigned)st);
            p = zput(p, " info=0x"); p = zhex(p, VDM_REG(tib, VTIB_EVENT_INFO));
            p = zput(p, " CS:IP=0x"); p = zhex(p, csv);
            p = zput(p, ":0x"); p = zhex(p, ipv); p = zput(p, "\r\n");
            p = zput(p, "  bytes@CS:IP: "); p = zdump(p, ib, 8);
            p = zput(p, "  VTIB[5A8..]: "); p = zdump(p, (const void *)(tib + 0x5A8), 0x20);
            log_append(LOG_PATH, base, p); p = base;
            break;
        }
        m.tp = p;
        if (!dos_int21(&m)) {                       /* AH=4Ch -> terminate */
            p = m.tp; VDM_REG(tib, VTIB_EIP) += 3;
            log_append(LOG_PATH, base, p); p = base;
            break;
        }
        p = m.tp;
        VDM_REG(tib, VTIB_EIP) += 3;                /* past the 3-byte BOP -> the IRET */
        log_append(LOG_PATH, base, p); p = base;
    }

    g_ci.ExitCode = (ULONG)m.exit_code;             /* M2.5: errorlevel (shell notify = best-effort TODO) */

    /* Flush captured DOS output to the console + the log. */
    {
        HANDLE hcon = CreateFileA("CONOUT$", GENERIC_WRITE, FILE_SHARE_WRITE, NULL,
                                  OPEN_EXISTING, 0, NULL);
        if (m.out_len > 0) {
            m.out[m.out_len] = 0;
            if (hcon != INVALID_HANDLE_VALUE) { DWORD w; WriteFile(hcon, m.out, m.out_len, &w, NULL); }
            p = zput(p, "  ==> DOS OUTPUT: ["); p = zput(p, m.out); p = zput(p, "]\r\n");
        }
        if (hcon != INVALID_HANDLE_VALUE) CloseHandle(hcon);
    }
    p = zput(p, "STAGE2: complete\r\n");
    log_append(LOG_PATH, base, p);
    return 0;
}
