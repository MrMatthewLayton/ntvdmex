/* main.c -- the clean DOS VDM host (windowed). Orchestrates the pipeline the
 * tools/vdmhost spike proved, now wired through the src/ modules:
 *   log -> CSRSS handshake (csrss) -> V86 bring-up (v86) -> load + build the DOS
 *   process (dos_loader/dos_psp/dos_mcb) -> service INT 21h/10h + I/O in V86,
 *   routing screen output to the video VDD and presenting via DirectDraw.
 * No CRT (src/runtime.c supplies the entry + mem*); imports only XP system DLLs.
 *
 * M3 merge: the host now owns a GUI window on a UI thread (present_ddraw) and the
 * V86/DOS engine runs on the main thread (the VdmInitialize thread). DOS console
 * output (INT 21h AH=02/09/40) and INT 10h are routed into the video VDD, whose
 * 80x25 cell grid is rendered + blitted into the Luna-themed window. The host
 * stays a CUI subsystem image so the CSRSS VDM handshake still binds.
 */
#include <windows.h>
#include <commctrl.h>
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
#include "vdd_video.h"
#include "vdd_input.h"
#include "present_ddraw.h"

#define LOG_PATH    "C:\\ntvdmex\\ntvdmhost.log"
#define TARGET_PATH "C:\\ntvdmex\\target.txt"

/* CSRSS receive buffers + program image (no CRT heap; static = zero-init). */
static char g_cmd[1024], g_app[1024], g_cur[512], g_pif[512];
static char g_env[8192], g_desk[512], g_title[512], g_rsv[512];
static VDM_COMMAND_INFO g_ci;
static BYTE filebuf[0x20000];

/* The device bus + its VDDs + the presentation layer live for the host's life. */
static vdd_bus      g_bus;
static pit_state    g_pit;       static ntvdd g_pit_dev;
static video_state  g_vid;       static ntvdd g_vid_dev;
static input_state  g_in;        static ntvdd g_in_dev;
static present_ddraw g_pd;
static int          g_irq_pending = -1;     /* last IRQ a VDD raised (spike obs) */
static CRITICAL_SECTION g_lock;             /* serialises all bus dispatch       */
static HWND         g_hwnd;
static HANDLE       g_key_event;            /* signalled when a key is pushed     */
static volatile LONG g_running = 1;         /* 0 once the window is closed         */
static HMENU        g_savedmenu;            /* stashed menu while hidden           */

static void host_irq_sink(void *ctx, uint8_t irq) { (void)ctx; g_irq_pending = irq; }

/* DOS console output (INT 21h AH=02/09/40) -> the video VDD teletype. */
static void host_conout(void *ctx, uint8_t ch)
{
    (void)ctx;
    EnterCriticalSection(&g_lock);
    vdd_video_putc(&g_vid, ch);
    LeaveCriticalSection(&g_lock);
}

/* DOS console input (INT 21h AH=01/07/08/0A) -> the keyboard VDD ring, blocking
   on the V86 thread until the UI thread pushes a key (or the window closes). */
static int host_conin(void *ctx)
{
    uint16_t k; int got;
    (void)ctx;
    for (;;) {
        EnterCriticalSection(&g_lock);
        got = vdd_input_pop(&g_in, &k);
        LeaveCriticalSection(&g_lock);
        if (got) return k & 0xFF;
        if (!g_running) return 0x1B;            /* window gone -> unblock as ESC   */
        WaitForSingleObject(g_key_event, 50);
    }
}

/* Non-blocking console read (INT 21h AH=06 DL=FF): a key char, or -1 if none. */
static int host_coninnb(void *ctx)
{
    uint16_t k; int got;
    (void)ctx;
    EnterCriticalSection(&g_lock);
    got = vdd_input_pop(&g_in, &k);
    LeaveCriticalSection(&g_lock);
    return got ? (k & 0xFF) : -1;
}

/* Non-blocking console status (INT 21h AH=0B / AH=06 DL=FF peek): 1 if a key is ready. */
static int host_conpeek(void *ctx)
{
    uint16_t k; int got;
    (void)ctx;
    EnterCriticalSection(&g_lock);
    got = vdd_input_peek(&g_in, &k);
    LeaveCriticalSection(&g_lock);
    return got;
}

/* Reflect CF/ZF a bus interrupt returned onto the FLAGS the INT pushed on the
   V86 stack (SS:SP+4) -- the handler's IRET restores them. */
static void host_set_flags(volatile BYTE *tib, uint8_t cf, uint8_t zf)
{
    volatile WORD *pfl = (volatile WORD *)(((VDM_REG(tib, VTIB_SS) & 0xFFFF) << 4)
                         + (((VDM_REG(tib, VTIB_ESP) & 0xFFFF) + 4) & 0xFFFF));
    if (cf) *pfl |= 0x0001; else *pfl &= (WORD)~0x0001;
    if (zf) *pfl |= 0x0040; else *pfl &= (WORD)~0x0040;
}

/* --- menu + status bar (scaffold; most items are stubs for now) ------------ */
static char g_progname[64] = "(none)";      /* shown in the status bar          */

enum {                                       /* wired command IDs                */
    IDM_STUB = 1,                            /* every not-yet-wired item          */
    IDM_FILE_EXIT, IDM_FILE_CLOSEPROG,
    IDM_DISP_FULLSCREEN, IDM_DISP_SHOWMENU,
    IDM_HELP_ABOUT
};

static void mi (HMENU m, const char *s, UINT id) { AppendMenuA(m, MF_STRING, id, s); }
static void msep(HMENU m) { AppendMenuA(m, MF_SEPARATOR, 0, NULL); }
static void msub(HMENU p, const char *s, HMENU c) { AppendMenuA(p, MF_POPUP, (UINT_PTR)c, s); }
static HMENU mpop(void) { return CreatePopupMenu(); }

/* Build the full menu tree (the structure is the scaffold; only a few items are
   wired -- the rest carry IDM_STUB and no-op until they're implemented). */
static HMENU build_menu(void)
{
    HMENU bar = CreateMenu(), m, s;
    m = mpop();                                                   /* File         */
    mi(m, "Open Executable...\tCtrl+O", IDM_STUB);
    msub(m, "Open Recent", (s=mpop(), mi(s,"(empty)",IDM_STUB), s));
    msep(m);
    msub(m, "Save State", (s=mpop(), mi(s,"Quick-Save\tF5",IDM_STUB), mi(s,"Slot 1...9",IDM_STUB), s));
    msub(m, "Load State", (s=mpop(), mi(s,"Quick-Load\tF9",IDM_STUB), mi(s,"Slot 1...9",IDM_STUB), s));
    msep(m);
    msub(m, "Configuration", (s=mpop(), mi(s,"Edit Config File...",IDM_STUB),
        mi(s,"Reload Configuration",IDM_STUB), mi(s,"Save Current as Default",IDM_STUB),
        mi(s,"Open Config Folder",IDM_STUB), mi(s,"Configuration Tool...",IDM_STUB), s));
    msep(m);
    mi(m, "Restart Machine", IDM_STUB);
    mi(m, "Close Program", IDM_FILE_CLOSEPROG);
    mi(m, "Exit\tAlt+F4", IDM_FILE_EXIT);
    msub(bar, "File", m);

    m = mpop();                                                   /* Edit         */
    mi(m,"Mark / Select Region",IDM_STUB); mi(m,"Copy\tCtrl+C",IDM_STUB);
    mi(m,"Copy Whole Screen",IDM_STUB); mi(m,"Paste\tCtrl+V",IDM_STUB); mi(m,"Select All",IDM_STUB);
    msub(bar, "Edit", m);

    m = mpop();                                                   /* CPU          */
    msub(m,"CPU Type",(s=mpop(),mi(s,"8086",IDM_STUB),mi(s,"286",IDM_STUB),mi(s,"386",IDM_STUB),mi(s,"486",IDM_STUB),mi(s,"Pentium",IDM_STUB),s));
    msub(m,"Core",(s=mpop(),mi(s,"Auto",IDM_STUB),mi(s,"Normal",IDM_STUB),mi(s,"Dynamic",IDM_STUB),mi(s,"Simple",IDM_STUB),s));
    mi(m,"FPU",IDM_STUB); msep(m);
    msub(m,"Speed",(s=mpop(),mi(s,"Auto",IDM_STUB),mi(s,"Max",IDM_STUB),mi(s,"Fixed cycles...",IDM_STUB),s));
    mi(m,"Turbo",IDM_STUB); msep(m);
    msub(m,"Memory",(s=mpop(),mi(s,"Conventional",IDM_STUB),mi(s,"XMS",IDM_STUB),mi(s,"EMS",IDM_STUB),mi(s,"UMB",IDM_STUB),mi(s,"A20",IDM_STUB),s));
    msub(bar, "CPU", m);

    m = mpop();                                                   /* Display      */
    mi(m,"Fullscreen\tAlt+Enter",IDM_DISP_FULLSCREEN);
    msub(m,"Window Size",(s=mpop(),mi(s,"1x",IDM_STUB),mi(s,"2x",IDM_STUB),mi(s,"3x",IDM_STUB),mi(s,"Custom",IDM_STUB),s));
    msub(m,"Renderer",(s=mpop(),mi(s,"GDI",IDM_STUB),mi(s,"DirectDraw",IDM_STUB),mi(s,"Direct3D9",IDM_STUB),mi(s,"OpenGL",IDM_STUB),s));
    msub(m,"Scaler",(s=mpop(),mi(s,"None",IDM_STUB),mi(s,"Scale2x",IDM_STUB),mi(s,"hq2x",IDM_STUB),mi(s,"Scanlines",IDM_STUB),mi(s,"CRT",IDM_STUB),s));
    msub(m,"Filtering",(s=mpop(),mi(s,"Nearest",IDM_STUB),mi(s,"Bilinear",IDM_STUB),s));
    mi(m,"Aspect Ratio Correction",IDM_STUB);
    msub(m,"Frame Skip",(s=mpop(),mi(s,"0",IDM_STUB),mi(s,"1",IDM_STUB),mi(s,"2",IDM_STUB),s));
    mi(m,"VSync",IDM_STUB); msep(m);
    mi(m,"Show Menu Bar",IDM_DISP_SHOWMENU);
    msub(bar, "Display", m);

    m = mpop();                                                   /* Audio        */
    mi(m,"Master Volume / Mute",IDM_STUB); msep(m);
    msub(m,"Sound Blaster",(s=mpop(),mi(s,"SB16",IDM_STUB),mi(s,"AWE32",IDM_STUB),mi(s,"Address / IRQ / DMA",IDM_STUB),s));
    msub(m,"AdLib / OPL",(s=mpop(),mi(s,"OPL2",IDM_STUB),mi(s,"OPL3",IDM_STUB),s));
    msub(m,"MIDI",(s=mpop(),mi(s,"Host GM",IDM_STUB),mi(s,"MT-32",IDM_STUB),mi(s,"SoundFont...",IDM_STUB),s));
    mi(m,"PC Speaker",IDM_STUB); msub(m,"Gravis Ultrasound",(s=mpop(),mi(s,"Enable",IDM_STUB),s));
    msub(m,"Tandy / CMS",(s=mpop(),mi(s,"Enable",IDM_STUB),s)); msep(m);
    msub(m,"Sample Rate / Buffer",(s=mpop(),mi(s,"22050",IDM_STUB),mi(s,"44100",IDM_STUB),s));
    msub(bar, "Audio", m);

    m = mpop();                                                   /* Input        */
    msub(m,"Mouse",(s=mpop(),mi(s,"Capture\tCtrl+F10",IDM_STUB),mi(s,"Seamless",IDM_STUB),mi(s,"Sensitivity",IDM_STUB),s));
    msub(m,"Keyboard",(s=mpop(),mi(s,"Layout",IDM_STUB),mi(s,"Typematic rate",IDM_STUB),mi(s,"Send Ctrl+Alt+Del",IDM_STUB),s));
    msub(m,"Joystick",(s=mpop(),mi(s,"Type",IDM_STUB),mi(s,"Map to gamepad",IDM_STUB),mi(s,"Calibrate",IDM_STUB),s));
    msep(m); mi(m,"Key Mapper...",IDM_STUB);
    msub(bar, "Input", m);

    m = mpop();                                                   /* Drive        */
    mi(m,"Mount Folder as Drive...",IDM_STUB); mi(m,"Mount Disk / CD Image...",IDM_STUB);
    mi(m,"Mount Physical Drive...",IDM_STUB); msub(m,"Unmount",(s=mpop(),mi(s,"(none)",IDM_STUB),s)); msep(m);
    mi(m,"Boot from Drive / Image...",IDM_STUB); mi(m,"Swap Disk\tCtrl+F4",IDM_STUB); msep(m);
    mi(m,"Drive Status...",IDM_STUB);
    msub(bar, "Drive", m);

    m = mpop();                                                   /* Capture      */
    mi(m,"Take Screenshot\tCtrl+F5",IDM_STUB);
    msub(m,"Record Video (AVI)",(s=mpop(),mi(s,"Start / Stop",IDM_STUB),s));
    msub(m,"Record Audio (WAV)",(s=mpop(),mi(s,"Start / Stop",IDM_STUB),s));
    msub(m,"Record OPL / MIDI",(s=mpop(),mi(s,"Start / Stop",IDM_STUB),s)); msep(m);
    mi(m,"Open Capture Folder",IDM_STUB); mi(m,"Capture Settings...",IDM_STUB);
    msub(bar, "Capture", m);

    m = mpop();                                                   /* Debug        */
    mi(m,"Pause / Resume\tPause",IDM_STUB); mi(m,"Step Instruction",IDM_STUB);
    mi(m,"Debugger Console...",IDM_STUB); mi(m,"Registers / Memory / Disassembly",IDM_STUB); msep(m);
    msub(m,"Logging",(s=mpop(),mi(s,"Levels...",IDM_STUB),mi(s,"Log to file",IDM_STUB),s));
    mi(m,"Performance Overlay",IDM_STUB);
    msub(bar, "Debug", m);

    m = mpop();                                                   /* Help         */
    mi(m,"Quick Start",IDM_STUB); mi(m,"Keyboard Shortcuts...",IDM_STUB); msep(m);
    mi(m,"About",IDM_HELP_ABOUT);
    msub(bar, "Help", m);
    return bar;
}

static HWND g_status;                        /* the native comctl32 status bar    */

/* Create the native status bar child + set its text; record its height so the
   video blit reserves that strip. */
/* Window caption: "Virtual MS-DOS Prompt" idle, "... - PROG.EXE" while a program
   runs. Safe to call from the V86 thread (SetWindowText posts WM_SETTEXT). */
#define VDM_WIN_TITLE "Virtual MS-DOS Prompt"
static void set_window_title(void)
{
    char t[128]; char *p = t; const char *s = VDM_WIN_TITLE;
    if (!g_hwnd) return;
    while (*s) *p++ = *s++;
    if (g_progname[0] && g_progname[0] != '(') {       /* not "(none)" */
        const char *d = " - ", *b = g_progname;
        while (*d) *p++ = *d++;
        while (*b) *p++ = *b++;
    }
    *p = 0;
    SetWindowTextA(g_hwnd, t);
}

static void make_status(HWND parent, HINSTANCE hi)
{
    char line[96]; char *p = line; RECT sr;
    const char *a = "NTVDMEX  -  ";
    g_status = CreateWindowExA(0, STATUSCLASSNAME, NULL,
                               WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP,
                               0, 0, 0, 0, parent, NULL, hi, NULL);
    if (!g_status) return;
    while (*a) *p++ = *a++;
    { const char *b = g_progname; while (*b) *p++ = *b++; } *p = 0;
    SendMessageA(g_status, SB_SETTEXTA, 0, (LPARAM)line);
    GetWindowRect(g_status, &sr);
    if (sr.bottom > sr.top) g_pd.status_h = sr.bottom - sr.top;
}

/* --- the UI thread: window + present + frame timer ------------------------- */
static LRESULT CALLBACK wnd_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_TIMER:
        EnterCriticalSection(&g_lock);
        vdd_bus_frame(&g_bus);          /* tick PIT + render into g_vid.frame       */
        present_ddraw_snapshot(&g_pd, &g_vid.frame);  /* consistent copy UNDER lock  */
        LeaveCriticalSection(&g_lock);
        present_ddraw_present(&g_pd);   /* vsync'd blit OUTSIDE the lock             */
        return 0;
    case WM_ERASEBKGND:
        return 1;                       /* we own the whole client -> no white erase */
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) { SetCursor(NULL); return TRUE; }  /* hide over video */
        break;
    case WM_PAINT: {                     /* re-blit the last snapshot on expose/move   */
        PAINTSTRUCT ps; BeginPaint(h, &ps);
        present_ddraw_present(&g_pd);
        EndPaint(h, &ps);
        return 0; }
    case WM_SIZE:
        if (g_status) SendMessageA(g_status, WM_SIZE, 0, 0);  /* let it re-dock     */
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDM_FILE_EXIT: DestroyWindow(h); return 0;
        case IDM_DISP_FULLSCREEN: present_ddraw_set_fullscreen(&g_pd, !g_pd.fullscreen); return 0;
        case IDM_DISP_SHOWMENU: {
            HMENU cur = GetMenu(h);
            if (cur) { g_savedmenu = cur; SetMenu(h, NULL); } else SetMenu(h, g_savedmenu);
            return 0; }
        case IDM_HELP_ABOUT:
            MessageBoxA(h, "NTVDMEX -- New Technology Virtual DOS Manager, Extended\n"
                          "A from-scratch ntvdm.exe for Windows XP (DOS on the real CPU in V86).",
                          "About NTVDMEX", MB_OK | MB_ICONINFORMATION); return 0;
        default: return 0;               /* IDM_STUB / not-yet-wired items: no-op      */
        }
    case WM_SYSKEYDOWN:                  /* Alt+Enter -> toggle fullscreen          */
        if (wp == VK_RETURN) { present_ddraw_set_fullscreen(&g_pd, !g_pd.fullscreen); return 0; }
        break;
    case WM_KEYDOWN:
        if (wp == VK_F11) { present_ddraw_set_fullscreen(&g_pd, !g_pd.fullscreen); return 0; }
        break;
    case WM_CHAR:                        /* translated ASCII -> keyboard ring     */
        EnterCriticalSection(&g_lock);
        vdd_input_push(&g_in, (uint16_t)(wp & 0xFF));
        LeaveCriticalSection(&g_lock);
        if (g_key_event) SetEvent(g_key_event);
        return 0;
    case WM_DESTROY:
        InterlockedExchange(&g_running, 0);
        if (g_key_event) SetEvent(g_key_event);   /* unblock the V86 thread        */
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(h, msg, wp, lp);
}

static DWORD WINAPI ui_thread(LPVOID arg)
{
    WNDCLASSA wc; MSG msg; RECT rc;
    HINSTANCE hi = GetModuleHandleA(NULL);
    INITCOMMONCONTROLSEX icc;
    (void)arg;
    icc.dwSize = sizeof icc; icc.dwICC = ICC_WIN95_CLASSES;
    InitCommonControlsEx(&icc);                  /* activate the Luna (v6) context */
    ZeroMemory(&wc, sizeof wc);
    wc.lpfnWndProc = wnd_proc; wc.hInstance = hi;
    wc.hCursor = LoadCursorA(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = "NtvdmexHostWindow";
    if (!RegisterClassA(&wc)) return 1;
    rc.left = 0; rc.top = 0; rc.right = VID_FB_W; rc.bottom = VID_FB_H + PRESENT_STATUS_H;
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, TRUE);   /* TRUE: window has a menu  */
    g_hwnd = CreateWindowA(wc.lpszClassName, VDM_WIN_TITLE, WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                           CW_USEDEFAULT, CW_USEDEFAULT, rc.right - rc.left, rc.bottom - rc.top,
                           NULL, NULL, hi, NULL);
    if (!g_hwnd) return 1;
    SetMenu(g_hwnd, build_menu());
    present_ddraw_init(&g_pd, g_hwnd);          /* GDI windowed; DDraw for fullscreen */
    make_status(g_hwnd, hi);                     /* native themed status bar          */
    ShowWindow(g_hwnd, SW_SHOW); UpdateWindow(g_hwnd);
    SetTimer(g_hwnd, 1, 33, NULL);              /* ~30 Hz present/PIT tick         */
    while (GetMessageA(&msg, NULL, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageA(&msg); }
    present_ddraw_shutdown(&g_pd);
    return 0;
}

/* --- guest register view <-> VDM_TIB CONTEXT (for bus interrupt dispatch) --- */
static void regs_load(ntvdd_regs *r, volatile BYTE *tib)
{
    r->eax = VDM_REG(tib, VTIB_EAX); r->ebx = VDM_REG(tib, VTIB_EBX);
    r->ecx = VDM_REG(tib, VTIB_ECX); r->edx = VDM_REG(tib, VTIB_EDX);
    r->esi = VDM_REG(tib, VTIB_ESI); r->edi = VDM_REG(tib, VTIB_EDI);
    r->ebp = VDM_REG(tib, VTIB_EBP);
    r->ds = (uint16_t)VDM_REG(tib, VTIB_DS); r->es = (uint16_t)VDM_REG(tib, VTIB_ES);
    r->cf = 0;
}
static void regs_store(ntvdd_regs *r, volatile BYTE *tib)
{
    VDM_REG(tib, VTIB_EAX) = r->eax; VDM_REG(tib, VTIB_EBX) = r->ebx;
    VDM_REG(tib, VTIB_ECX) = r->ecx; VDM_REG(tib, VTIB_EDX) = r->edx;
}

/* Decode + service a V86 IN/OUT that #GP-faulted (event 2), dispatch it to the
   bus, and advance EIP past the instruction so the guest resumes. Returns 1 if
   the faulting instruction was a (supported) I/O op we handled, 0 if it was a
   genuine GP fault the caller should stop on. No per-call logging -- I/O traps
   are hot (a palette set is ~768 OUTs); flushing the trace file each one stalls. */
static int host_try_io(volatile BYTE *tib, vdd_bus *bus)
{
    DWORD cs = VDM_REG(tib, VTIB_CS)  & 0xFFFF;
    DWORD ip = VDM_REG(tib, VTIB_EIP) & 0xFFFF;
    volatile BYTE *code = (volatile BYTE *)((cs << 4) + ip);   /* absolute V86 */
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
    } else {
        val = (width == 1) ? (eax & 0xFF) : (width == 2) ? (eax & 0xFFFF) : eax;
        vdd_bus_io(bus, port, (uint8_t)width, 0, &val);
    }
    VDM_REG(tib, VTIB_EIP) = (ip + len) & 0xFFFF;          /* step past I/O    */
    return 1;
}

/* --- planar mode-12h: trap direct A0000 writes through the VGA write engine -- */
#define A000_LO 0xA0000u
#define A000_HI 0xB0000u
static int g_a000_prot = 0;

/* In mode 12h, mark the A0000 graphics window NOACCESS so direct guest writes
   fault to us; restore RW otherwise. */
static void a000_protect(int on)
{
    DWORD old;
    if (on == g_a000_prot) return;
    if (VirtualProtect((LPVOID)A000_LO, 0x10000,
                       on ? PAGE_NOACCESS : PAGE_EXECUTE_READWRITE, &old))
        g_a000_prot = on;
}

/* Decode a faulting store into the protected A0000 window and run it through the
   VGA planar write engine, then advance EIP. Handles STOSB/STOSW (incl. REP) --
   the dominant fast-fill path; other store forms fall through (return 0 -> the
   stop dump shows them, to grow the decoder later). */
static int host_try_mem(volatile BYTE *tib)
{
    DWORD cs = VDM_REG(tib, VTIB_CS) & 0xFFFF, ip = VDM_REG(tib, VTIB_EIP) & 0xFFFF;
    volatile BYTE *code = (volatile BYTE *)((cs << 4) + ip);
    int i = 0, rep = 0; BYTE op;
    if (!g_a000_prot) return 0;
    while (i < 4) {                                   /* skip prefixes            */
        BYTE b = code[i];
        if (b == 0xF3 || b == 0xF2) { rep = 1; ++i; }
        else if (b==0x26||b==0x2E||b==0x36||b==0x3E||b==0x64||b==0x65||b==0x66||b==0x67) ++i;
        else break;
    }
    op = code[i];
    if (op == 0xAA || op == 0xAB) {                   /* STOSB / STOSW (ES:DI)    */
        DWORD es = VDM_REG(tib, VTIB_ES) & 0xFFFF, di = VDM_REG(tib, VTIB_EDI) & 0xFFFF;
        DWORD lin = (es << 4) + di, cx = rep ? (VDM_REG(tib, VTIB_ECX) & 0xFFFF) : 1;
        DWORD eax = VDM_REG(tib, VTIB_EAX), n; int wsz = (op == 0xAB) ? 2 : 1;
        if (lin < A000_LO || lin >= A000_HI) return 0;
        EnterCriticalSection(&g_lock);
        for (n = 0; n < cx; ++n) {
            DWORD a = lin + n * wsz;
            if (a >= A000_HI) break;
            vga_planar_write(&g_vid, a - A000_LO, (BYTE)eax);
            if (wsz == 2 && a + 1 < A000_HI) vga_planar_write(&g_vid, a + 1 - A000_LO, (BYTE)(eax >> 8));
        }
        LeaveCriticalSection(&g_lock);
        VDM_SET16(tib, VTIB_EDI, (di + cx * wsz) & 0xFFFF);
        if (rep) VDM_SET16(tib, VTIB_ECX, 0);
        VDM_REG(tib, VTIB_EIP) = (ip + i + 1) & 0xFFFF;
        return 1;
    }
    return 0;
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
    static const BYTE bop10[] = { VDM_BOP0, VDM_BOP1, 0x10, 0xCF }; /* BOP 0x10 ; iret */
    static const BYTE bop16[] = { VDM_BOP0, VDM_BOP1, 0x16, 0xCF }; /* BOP 0x16 ; iret */
    HANDLE ui = NULL;

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

    /* status-bar program name = basename of progpath (if any) */
    { const char *bn = progpath, *q; int k = 0;
      for (q = progpath; *q; ++q) if (*q == '\\' || *q == '/') bn = q + 1;
      if (*bn) { while (bn[k] && k < 63) { g_progname[k] = bn[k]; ++k; } g_progname[k] = 0; } }
    set_window_title();                            /* "Virtual MS-DOS Prompt - PROG.EXE" */

    /* Build the DOS process in conventional memory (base=NULL => absolute V86). */
    img = dos_load(NULL, filebuf, nread, DOS_PSP_SEG);

    hdlr = (volatile BYTE *)(DOS_HDLR_SEG << 4);            /* INT 21h BOP handler */
    for (i = 0; i < sizeof(bop); ++i) hdlr[i] = bop[i];
    *(volatile WORD *)0x84 = 0x0000;                        /* IVT[0x21].offset    */
    *(volatile WORD *)0x86 = DOS_HDLR_SEG;                  /* IVT[0x21].segment   */
    hdlr[DOS_DBCS_OFF] = 0; hdlr[DOS_DBCS_OFF + 1] = 0;     /* empty DBCS table    */
    for (i = 0; i < sizeof(bop10); ++i) hdlr[0x20 + i] = bop10[i];  /* INT 10h stub */
    *(volatile WORD *)0x40 = 0x0020;                        /* IVT[0x10].offset    */
    *(volatile WORD *)0x42 = DOS_HDLR_SEG;                  /* IVT[0x10].segment   */
    for (i = 0; i < sizeof(bop16); ++i) hdlr[0x28 + i] = bop16[i];  /* INT 16h stub */
    *(volatile WORD *)0x58 = 0x0028;                        /* IVT[0x16].offset    */
    *(volatile WORD *)0x5A = DOS_HDLR_SEG;                  /* IVT[0x16].segment   */

    dos_psp_build(NULL, DOS_PSP_SEG, DOS_ENV_SEG, DOS_MEM_TOP);
    dos_env_build(NULL, DOS_ENV_SEG, progpath[0] ? progpath : "C:\\PROGRAM.COM");  /* M2.5: env */
    dos_cmdtail_build(NULL, DOS_PSP_SEG, args);                                    /* M2.5: args */
    dos_int21_init(&m, dos_mcb_init(NULL));

    /* Stand up the device bus (NULL base => absolute V86 addresses) with the PIT
       (ports 0x40-0x43, INT 08h/1Ah) and the video VDD (B8000 + INT 10h text +
       cell renderer). The present sink is DirectDraw via present_ddraw on the UI
       thread. I/O on claimed ports reflects as event 0 -> the bus; INT 10h comes
       in as a BOP routed below; DOS console output is routed via m.conout. */
    InitializeCriticalSection(&g_lock);
    vdd_bus_init(&g_bus, NULL);
    vdd_bus_set_sinks(&g_bus, host_irq_sink, NULL, NULL, NULL);  /* host presents directly */
    g_pit_dev = vdd_pit_device(&g_pit);
    vdd_bus_add(&g_bus, &g_pit_dev);
    g_vid.vmem = (uint8_t *)VID_APERTURE_BASE;  /* the mapped A0000 aperture (RAM) */
    g_vid_dev = vdd_video_device(&g_vid);
    vdd_bus_add(&g_bus, &g_vid_dev);
    g_in_dev = vdd_input_device(&g_in);
    vdd_bus_add(&g_bus, &g_in_dev);             /* keyboard: claims INT 16h      */
    m.conout = host_conout; m.conctx = NULL;    /* DOS console out -> video      */
    m.conin  = host_conin;  m.cinctx = NULL;    /* DOS console in  <- keyboard   */
    m.coninnb = host_coninnb;                   /* AH=06 DL=FF non-blocking read */
    m.conpeek = host_conpeek;                   /* AH=0B/06 non-blocking status  */

    /* Hide the inherited console (CSRSS already bound the VDM); the Luna window
       is now the display. Then start the UI thread that owns it. */
    g_key_event = CreateEventA(NULL, FALSE, FALSE, NULL);   /* auto-reset        */
    { HWND con = GetConsoleWindow(); if (con) ShowWindow(con, SW_HIDE); }
    ui = CreateThread(NULL, 0, ui_thread, NULL, 0, NULL);

    v86_set_entry(tib, img.cs, img.ip, img.ss, img.sp, DOS_PSP_SEG);
    if (!img.is_exe)                                        /* .COM near-ret guard */
        *(volatile WORD *)(((DWORD)DOS_PSP_SEG << 4) + 0xFFFE) = 0;

    p = zput(p, img.is_exe ? "STAGE2: running .EXE (entry 0x"
                           : "STAGE2: running .COM (entry 0x");
    p = zhex(p, img.cs); p = zput(p, ":0x"); p = zhex(p, img.ip); p = zput(p, ")...\r\n");
    log_write(LOG_PATH, report, p);
    base = p;                       /* preamble is on disk; the loop appends from here */

    SetCurrentDirectoryA(g_cur);    /* DOS relative paths resolve against CurDir */

    /* Service loop: run V86 until a BOP, dispatch INT 21h, step past the BOP, re-enter.
       Runs until the guest terminates, a hard stop, or the window closes (g_running);
       no iteration cap so interactive/animated programs keep going. */
    m.tib = tib; m.out = dosout; m.out_cap = sizeof(dosout); m.out_len = 0;
    (void)guard;
    while (g_running) {
        ev = v86_run(tib, &st);
        /* I/O port trap (event 0; VM-confirmed) or a generic GP fault (event 2):
           if the faulting instruction is an IN/OUT we can decode, service it via
           the VDD bus and resume; otherwise fall through to the stop dump. */
        if (ev == VDM_EVENT_IO || ev == VDM_EVENT_GPFAULT) {
            int handled;
            EnterCriticalSection(&g_lock);
            handled = host_try_io(tib, &g_bus);     /* no per-call logging (hot path) */
            LeaveCriticalSection(&g_lock);
            if (handled) continue;
            if (host_try_mem(tib)) continue;        /* direct A0000 planar write       */
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
        /* Route the BOP by its number: 0x10 -> INT 10h, 0x16 -> INT 16h (both via
           the bus), else INT 21h. */
        if ((VDM_REG(tib, VTIB_EVENT_INFO) & 0xFF) == 0x10) {
            ntvdd_regs r; uint8_t ah10, al10; regs_load(&r, tib);
            ah10 = r_ah(&r); al10 = r_al(&r);
            EnterCriticalSection(&g_lock);
            vdd_bus_deliver_int(&g_bus, 0x10, &r);
            LeaveCriticalSection(&g_lock);
            regs_store(&r, tib);
            p = zput(p, "  INT10 AH=0x"); p = zhex(p, ah10);
            p = zput(p, " AL=0x"); p = zhex(p, al10); p = zput(p, "\r\n");
            log_append(LOG_PATH, base, p); p = base;
            a000_protect(vdd_video_planar_active(&g_vid));   /* trap A0000 in mode 12h */
            VDM_REG(tib, VTIB_EIP) += 3;
            continue;
        }
        if ((VDM_REG(tib, VTIB_EVENT_INFO) & 0xFF) == 0x16) {
            ntvdd_regs r; uint8_t ah16; regs_load(&r, tib); ah16 = r_ah(&r);
            for (;;) {                          /* AH=00 blocks until a key       */
                EnterCriticalSection(&g_lock);
                vdd_bus_deliver_int(&g_bus, 0x16, &r);
                LeaveCriticalSection(&g_lock);
                if (ah16 != 0x00 || r.zf == 0 || !g_running) break;
                WaitForSingleObject(g_key_event, 50);
            }
            regs_store(&r, tib);
            host_set_flags(tib, r.cf, r.zf);
            VDM_REG(tib, VTIB_EIP) += 3;
            continue;
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

    /* Keep the Luna window open so the guest's final screen stays visible until
       the user closes it; then the UI thread's message loop returns. */
    if (ui) { WaitForSingleObject(ui, INFINITE); CloseHandle(ui); }
    return 0;
}
