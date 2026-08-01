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
#include "dpmi.h"
#include "csrss.h"
#include "log.h"
#include "dos_mcb.h"
#include "dos_loader.h"
#include "dos_psp.h"
#include "dos_env.h"
#include "dos_int21.h"
#include "dos_layout.h"
#include "dos_xms.h"
#include "dos_ems.h"
#include "vdd_bus.h"
#include "vdd_pit.h"
#include "vdd_video.h"
#include "vdd_input.h"
#include "vdd_speaker.h"
#include "present_ddraw.h"

#define LOG_PATH    "C:\\ntvdmex\\ntvdmhost.log"
#define TARGET_PATH "C:\\ntvdmex\\target.txt"

/* Offset, within DOS_HDLR_SEG, of the XMS API far-call entry stub (BOP 0x43; RETF).
   Lives just past the INT 1Ah stub (which ends at 0x40) and the INT 2Fh stub (4 bytes
   at 0x40). INT 2Fh AX=4310 hands the guest DOS_HDLR_SEG:XMS_ENTRY_OFF to far-call. */
#define XMS_ENTRY_OFF 0x0044

/* DPMI (M4 slice 3, spike): the mode-switch entry far-called by a client after it
   detects DPMI via INT 2Fh AX=1687h. Lives past the INT 67h stub (0x48..0x4B).
   The stub is `BOP 0x50 ; RETF`: the host services the BOP by switching the VDM to
   protected mode (rewriting CS:IP), so the RETF only runs if the switch FAILS (it
   returns to the client in real mode with CF=1). See src/vdm/dpmi.c. */
#define DPMI_ENTRY_OFF 0x0050
#define DPMI_BOP       0x50

/* EMS (M4): the LIM page frame is a 64KB RAM window in the UMA. v86_map_ems_frame
   scans the conventional page-frame segments AFTER VdmInitialize for a free 64KB
   hole and maps it there; g_ems_frame_lin holds the linear base actually chosen
   (0 => none found, EMS unavailable). The guest learns the segment via AH=41. */
#define EMS_POOL_PAGES 512                           /* 512 * 16KB = 8MB of EMS */
static DWORD g_ems_frame_lin;                        /* set by v86_map_ems_frame */

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
static speaker_state g_spk;      static ntvdd g_spk_dev;
static present_ddraw g_pd;
static xms_state    g_xms;       /* M4: XMS extended-memory manager           */
static ems_state    g_ems;       /* M4: EMS expanded-memory manager           */
static volatile LONG g_irq0_pending = 0;    /* PIT raised IRQ0 (UI thread sets, V86 thread delivers) */
static CRITICAL_SECTION g_lock;             /* serialises all bus dispatch       */
static HWND         g_hwnd;
static HANDLE       g_key_event;            /* signalled when a key is pushed     */
static volatile LONG g_running = 1;         /* 0 once the window is closed         */
static int g_dpmi_pm = 0;                   /* set once the guest is switched to PM (spike) */
static DWORD g_dpmi_code_base = 0;          /* linear base of the guest PM code seg (retcs<<4) */
static volatile BYTE *g_tib_dbg = 0;        /* VDM_TIB, for the crash VEH to dump guest state */

/* Serial debug sink (DPMI harness): COM1 is captured by QEMU (-serial file:vm/serial.log)
   so the host reads the log directly -- no GUI screendump, no stale-host ambiguity. */
static HANDLE g_serial = INVALID_HANDLE_VALUE;
static void serial_init(void)
{
    DCB dcb;
    g_serial = CreateFileA("\\\\.\\COM1", GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (g_serial == INVALID_HANDLE_VALUE) return;
    { unsigned i; char *q = (char *)&dcb; for (i = 0; i < sizeof dcb; ++i) q[i] = 0; }
    dcb.DCBlength = sizeof dcb;
    if (GetCommState(g_serial, &dcb)) {
        dcb.BaudRate = 115200; dcb.ByteSize = 8; dcb.Parity = 0; dcb.StopBits = 0;
        SetCommState(g_serial, &dcb);
    }
}
/* Write [buf..end) to COM1 (and it's already in the file log via log_*). */
static void serial_out(const char *buf, const char *end)
{
    DWORD wrote;
    if (g_serial != INVALID_HANDLE_VALUE && end > buf) {
        WriteFile(g_serial, buf, (DWORD)(end - buf), &wrote, NULL);
        FlushFileBuffers(g_serial);
    }
}
static HMENU        g_savedmenu;            /* stashed menu while hidden           */

static void host_irq_sink(void *ctx, uint8_t irq) { (void)ctx; if (irq == 0) g_irq0_pending = 1; }

/* Real/synthesised real-mode interrupt dispatch. The guest runs cooperatively
   (we only regain control at event boundaries), so when a hardware IRQ becomes
   pending and the guest's IF is set, we do here exactly what the CPU does on a
   hardware interrupt: push FLAGS/CS/IP, clear IF+TF, and vector CS:IP through the
   real-mode IVT. The guest's handler IRETs back normally. (`CD nn` software ints
   still vector natively via VME; this is only for asynchronous IRQ delivery.) */
static void pokew(DWORD lin, WORD v)
{ volatile BYTE *m = (volatile BYTE *)0; m[lin] = (BYTE)v; m[lin + 1] = (BYTE)(v >> 8); }
static WORD peekw(DWORD lin)
{ const volatile BYTE *m = (const volatile BYTE *)0; return (WORD)(m[lin] | (m[lin + 1] << 8)); }

static void inject_int(volatile BYTE *tib, unsigned vec)
{
    WORD ss = (WORD)VDM_REG(tib, VTIB_SS),  sp = (WORD)VDM_REG(tib, VTIB_ESP);
    WORD cs = (WORD)VDM_REG(tib, VTIB_CS),  ip = (WORD)VDM_REG(tib, VTIB_EIP);
    WORD fl = (WORD)VDM_REG(tib, VTIB_EFLAGS);     /* push the live frame's FLAGS */
    sp -= 2; pokew(((DWORD)ss << 4) + sp, fl);     /* push FLAGS */
    sp -= 2; pokew(((DWORD)ss << 4) + sp, cs);     /* push CS    */
    sp -= 2; pokew(((DWORD)ss << 4) + sp, ip);     /* push IP    */
    VDM_SET16(tib, VTIB_ESP, sp);
    VDM_REG(tib, VTIB_EFLAGS) &= ~0x300u;          /* clear IF + TF (CPU does this) */
    VDM_SET16(tib, VTIB_EIP, peekw(vec * 4));      /* IVT[vec].offset */
    VDM_SET16(tib, VTIB_CS,  peekw(vec * 4 + 2));  /* IVT[vec].segment */
}

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

/* --- XMS (M4) -------------------------------------------------------------- *
 * Extended memory lives on the host heap (above the 1MB the V86 map covers), so
 * each EMB is a VirtualAlloc block; xms_move() memcpys between it and the guest's
 * conventional window. The XMS entry point is a BOP stub reached by FAR CALL (so
 * it ends in RETF, not IRET); INT 2Fh AX=4300/4310 advertise it. */
static void *xms_host_alloc(void *ctx, uint32_t kb)
{
    (void)ctx;
    return VirtualAlloc(NULL, (SIZE_T)kb * 1024, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
}
static void xms_host_free(void *ctx, void *p, uint32_t kb)
{
    (void)ctx; (void)kb;
    if (p) VirtualFree(p, 0, MEM_RELEASE);
}

/* Service one XMS far-call (function in AH). XMS returns AX=1 success / AX=0 fail
   with BL=error code -- it does NOT use the carry flag, so no pushed-FLAGS edit. */
static void host_xms(volatile BYTE *tib)
{
    DWORD ah = (VDM_REG(tib, VTIB_EAX) >> 8) & 0xFF;
    uint8_t err = XMSERR_NOTIMPL;
    uint16_t handle, nh;
    uint32_t largest, totfree, lin;
    uint8_t lock, freeh; uint32_t size_kb;

    #define X_SETAX(v) VDM_SET16(tib, VTIB_EAX, (v))
    #define X_SETBX(v) VDM_SET16(tib, VTIB_EBX, (v))
    #define X_SETDX(v) VDM_SET16(tib, VTIB_EDX, (v))
    #define X_SETBL(v) VDM_REG(tib, VTIB_EBX) = (VDM_REG(tib, VTIB_EBX) & 0xFFFFFF00u) | ((v) & 0xFF)
    #define X_SETBH(v) VDM_REG(tib, VTIB_EBX) = (VDM_REG(tib, VTIB_EBX) & 0xFFFF00FFu) | (((DWORD)(v) & 0xFF) << 8)
    #define X_FAIL(e)  do { X_SETAX(0); X_SETBL(e); } while (0)

    switch (ah) {
    case 0x00:                                  /* get version */
        X_SETAX(XMS_VERSION); X_SETBX(XMS_REVISION); X_SETDX(0);  /* DX=0: no HMA */
        break;
    case 0x01: X_FAIL(XMSERR_HMA_NONE);   break; /* request HMA (none provided) */
    case 0x02: X_FAIL(XMSERR_HMA_NOTALL); break; /* release HMA */
    case 0x03: case 0x05: g_xms.a20 = 1; X_SETAX(1); break;  /* enable A20 (global/local) */
    case 0x04: case 0x06: g_xms.a20 = 0; X_SETAX(1); break;  /* disable A20 */
    case 0x07: X_SETAX(g_xms.a20 ? 1 : 0); X_SETBL(0); break;/* query A20 */
    case 0x08:                                  /* query free extended memory */
        xms_query_free(&g_xms, &largest, &totfree);
        X_SETAX(largest > 0xFFFF ? 0xFFFF : largest);
        X_SETDX(totfree > 0xFFFF ? 0xFFFF : totfree);
        X_SETBL(largest ? 0 : XMSERR_NOMEM);
        break;
    case 0x09:                                  /* allocate EMB: DX=KB */
        if (xms_alloc(&g_xms, VDM_REG(tib, VTIB_EDX) & 0xFFFF, &nh, &err)) { X_SETAX(1); X_SETDX(nh); }
        else X_FAIL(err);
        break;
    case 0x0A:                                  /* free EMB: DX=handle */
        if (xms_free(&g_xms, (uint16_t)(VDM_REG(tib, VTIB_EDX) & 0xFFFF), &err)) X_SETAX(1);
        else X_FAIL(err);
        break;
    case 0x0B: {                                /* move EMB: DS:SI -> move struct */
        DWORD ds = VDM_REG(tib, VTIB_DS) & 0xFFFF, si = VDM_REG(tib, VTIB_ESI) & 0xFFFF;
        const volatile BYTE *s = (const volatile BYTE *)((ds << 4) + si);
        xms_move_t mv;
        mv.length     = (DWORD)s[0] | ((DWORD)s[1] << 8) | ((DWORD)s[2] << 16) | ((DWORD)s[3] << 24);
        mv.src_handle = (uint16_t)(s[4] | (s[5] << 8));
        mv.src_offset = (DWORD)s[6] | ((DWORD)s[7] << 8) | ((DWORD)s[8] << 16) | ((DWORD)s[9] << 24);
        mv.dst_handle = (uint16_t)(s[10] | (s[11] << 8));
        mv.dst_offset = (DWORD)s[12] | ((DWORD)s[13] << 8) | ((DWORD)s[14] << 16) | ((DWORD)s[15] << 24);
        if (xms_move(&g_xms, NULL, &mv, &err)) X_SETAX(1); else X_FAIL(err);
        break; }
    case 0x0C:                                  /* lock EMB: DX=handle -> DX:BX linear */
        handle = (uint16_t)(VDM_REG(tib, VTIB_EDX) & 0xFFFF);
        if (xms_lock(&g_xms, handle, &lin, &err)) { X_SETAX(1); X_SETDX(lin >> 16); X_SETBX(lin & 0xFFFF); }
        else X_FAIL(err);
        break;
    case 0x0D:                                  /* unlock EMB: DX=handle */
        if (xms_unlock(&g_xms, (uint16_t)(VDM_REG(tib, VTIB_EDX) & 0xFFFF), &err)) X_SETAX(1);
        else X_FAIL(err);
        break;
    case 0x0E:                                  /* get handle info: DX=handle */
        handle = (uint16_t)(VDM_REG(tib, VTIB_EDX) & 0xFFFF);
        if (xms_info(&g_xms, handle, &lock, &freeh, &size_kb, &err)) {
            X_SETAX(1); X_SETBH(lock); X_SETBL(freeh); X_SETDX(size_kb > 0xFFFF ? 0xFFFF : size_kb);
        } else X_FAIL(err);
        break;
    case 0x0F:                                  /* reallocate EMB: BX=new KB, DX=handle */
        handle = (uint16_t)(VDM_REG(tib, VTIB_EDX) & 0xFFFF);
        if (xms_realloc(&g_xms, handle, VDM_REG(tib, VTIB_EBX) & 0xFFFF, &err)) X_SETAX(1);
        else X_FAIL(err);
        break;
    case 0x10: X_SETAX(0); X_SETBL(0xB1); X_SETDX(0); break;  /* request UMB: none */
    case 0x11: X_FAIL(0xB2); break;                          /* release UMB */
    default:   X_FAIL(XMSERR_NOTIMPL); break;
    }
    #undef X_SETAX
    #undef X_SETBX
    #undef X_SETDX
    #undef X_SETBL
    #undef X_SETBH
    #undef X_FAIL
}

/* --- EMS (M4) -------------------------------------------------------------- *
 * Expanded memory lives on the host heap (pages * 16KB per handle); the 64KB
 * page frame at E000:0 is real V86 RAM (v86 Map 5). ems_map memcpys logical
 * pages in/out of the frame windows (page-frame shadowing). INT 67h carries the
 * function in AH and returns status in AH (0 = ok). */
static void *ems_host_alloc(void *ctx, uint32_t pages)
{
    (void)ctx;
    return VirtualAlloc(NULL, (SIZE_T)pages * EMS_PAGE_SIZE, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
}
static void ems_host_free(void *ctx, void *p, uint32_t pages)
{
    (void)ctx; (void)pages;
    if (p) VirtualFree(p, 0, MEM_RELEASE);
}

/* Service one INT 67h (EMM) call (function in AH; status back in AH). */
static void host_ems(volatile BYTE *tib)
{
    DWORD ah = (VDM_REG(tib, VTIB_EAX) >> 8) & 0xFF;
    uint8_t err = EMSERR_UNDEFFUNC;
    uint16_t handle = 0, p16 = 0, p16b = 0;

    #define E_SETAH(v) VDM_REG(tib, VTIB_EAX) = (VDM_REG(tib, VTIB_EAX) & 0xFFFF00FFu) | (((DWORD)(v) & 0xFF) << 8)
    #define E_SETAL(v) VDM_REG(tib, VTIB_EAX) = (VDM_REG(tib, VTIB_EAX) & 0xFFFFFF00u) | ((v) & 0xFF)
    #define E_SETBX(v) VDM_SET16(tib, VTIB_EBX, (v))
    #define E_SETDX(v) VDM_SET16(tib, VTIB_EDX, (v))

    switch (ah) {
    case 0x40: E_SETAH(EMS_OK); break;                  /* get manager status   */
    case 0x41: E_SETBX(g_ems.frame_seg); E_SETAH(EMS_OK); break;  /* page frame seg */
    case 0x42:                                          /* unallocated/total pages */
        ems_counts(&g_ems, &p16, &p16b);
        E_SETBX(p16); E_SETDX(p16b); E_SETAH(EMS_OK);
        break;
    case 0x43:                                          /* allocate BX pages -> DX handle */
        if (ems_alloc(&g_ems, VDM_REG(tib, VTIB_EBX) & 0xFFFF, &handle, &err)) { E_SETDX(handle); E_SETAH(EMS_OK); }
        else E_SETAH(err);
        break;
    case 0x44:                                          /* map: AL=phys BX=logical DX=handle */
        if (ems_map(&g_ems, (uint8_t)(VDM_REG(tib, VTIB_EAX) & 0xFF),
                    (uint16_t)(VDM_REG(tib, VTIB_EBX) & 0xFFFF),
                    (uint16_t)(VDM_REG(tib, VTIB_EDX) & 0xFFFF), &err)) E_SETAH(EMS_OK);
        else E_SETAH(err);
        break;
    case 0x45:                                          /* deallocate DX handle */
        if (ems_free(&g_ems, (uint16_t)(VDM_REG(tib, VTIB_EDX) & 0xFFFF), &err)) E_SETAH(EMS_OK);
        else E_SETAH(err);
        break;
    case 0x46: E_SETAL(EMS_VERSION); E_SETAH(EMS_OK); break;  /* EMM version 4.0 */
    case 0x47:                                          /* save page map: DX handle */
        if (ems_save_map(&g_ems, (uint16_t)(VDM_REG(tib, VTIB_EDX) & 0xFFFF), &err)) E_SETAH(EMS_OK);
        else E_SETAH(err);
        break;
    case 0x48:                                          /* restore page map: DX handle */
        if (ems_restore_map(&g_ems, (uint16_t)(VDM_REG(tib, VTIB_EDX) & 0xFFFF), &err)) E_SETAH(EMS_OK);
        else E_SETAH(err);
        break;
    case 0x4B: E_SETBX(ems_handle_count(&g_ems)); E_SETAH(EMS_OK); break;  /* # handles */
    case 0x4C:                                          /* pages owned by DX handle */
        if (ems_handle_pages(&g_ems, (uint16_t)(VDM_REG(tib, VTIB_EDX) & 0xFFFF), &p16, &err)) { E_SETBX(p16); E_SETAH(EMS_OK); }
        else E_SETAH(err);
        break;
    case 0x51:                                          /* reallocate: BX pages, DX handle */
        if (ems_realloc(&g_ems, (uint16_t)(VDM_REG(tib, VTIB_EDX) & 0xFFFF),
                        (uint16_t)(VDM_REG(tib, VTIB_EBX) & 0xFFFF), &err)) {
            ems_handle_pages(&g_ems, (uint16_t)(VDM_REG(tib, VTIB_EDX) & 0xFFFF), &p16, &err);
            E_SETBX(p16); E_SETAH(EMS_OK);
        } else E_SETAH(err);
        break;
    default: E_SETAH(EMSERR_UNDEFFUNC); break;
    }
    #undef E_SETAH
    #undef E_SETAL
    #undef E_SETBX
    #undef E_SETDX
}

/* --- menu + status bar (scaffold; most items are stubs for now) ------------ */
static char g_progname[64] = "(none)";      /* shown in the status bar          */
static volatile int g_title_dirty = 1;      /* UI thread re-applies the caption  */

/* Mouse state shared UI thread -> V86 thread (INT 33h). Position is in guest
   pixels (mapped from the window client); buttons: bit0 L, bit1 R, bit2 M. */
static volatile LONG g_ms_x = 320, g_ms_y = 240, g_ms_btn = 0;
static volatile LONG g_ms_hidden = 1;       /* INT 33h cursor hide-count; 0 => visible */

/* INT 33h mouse driver (functions DOS apps actually use). The host draws the
   cursor (overlay in the present path) when the hide-count is 0, so apps that
   rely on the driver cursor (the common case) get a visible pointer. */
static void mouse_int33(volatile BYTE *tib)
{
    static LONG last_x = 320, last_y = 240;             /* for AX=0B motion (V86 thread only) */
    DWORD ax = VDM_REG(tib, VTIB_EAX) & 0xFFFF;
    LONG x = g_ms_x, y = g_ms_y, b = g_ms_btn;
    switch (ax) {
    case 0x0000:                                        /* reset + get status      */
        VDM_SET16(tib, VTIB_EAX, 0xFFFF);               /* driver installed        */
        VDM_SET16(tib, VTIB_EBX, 0x0002);               /* 2 buttons               */
        InterlockedExchange(&g_ms_hidden, 1);           /* hidden until Show       */
        break;
    case 0x0001:                                        /* show cursor (dec count) */
        if (g_ms_hidden > 0) InterlockedDecrement(&g_ms_hidden);
        break;
    case 0x0002:                                        /* hide cursor (inc count) */
        InterlockedIncrement(&g_ms_hidden);
        break;
    case 0x0003:                                        /* get position + buttons  */
        VDM_SET16(tib, VTIB_ECX, (WORD)x);
        VDM_SET16(tib, VTIB_EDX, (WORD)y);
        VDM_SET16(tib, VTIB_EBX, (WORD)b);
        break;
    case 0x0004:                                        /* set cursor position     */
        InterlockedExchange(&g_ms_x, (LONG)(VDM_REG(tib, VTIB_ECX) & 0xFFFF));
        InterlockedExchange(&g_ms_y, (LONG)(VDM_REG(tib, VTIB_EDX) & 0xFFFF));
        break;
    case 0x0005: case 0x0006:                           /* button press/release info */
        VDM_SET16(tib, VTIB_EAX, (WORD)b);
        VDM_SET16(tib, VTIB_EBX, 0);                    /* 0 presses since last call */
        VDM_SET16(tib, VTIB_ECX, (WORD)x);
        VDM_SET16(tib, VTIB_EDX, (WORD)y);
        break;
    case 0x000B:                                        /* read relative motion    */
        VDM_SET16(tib, VTIB_ECX, (WORD)((x - last_x) * 8));   /* ~8 mickeys / pixel */
        VDM_SET16(tib, VTIB_EDX, (WORD)((y - last_y) * 8));
        last_x = x; last_y = y;
        break;
    default: break;                                     /* 07/08 range, 0C handler, ...: accept */
    }
}

/* Classic arrow cursor: 'o' = black outline (index 0), 'X' = white fill (15),
   ' ' = transparent; hotspot at the top-left tip. Drawn into the 8bpp frame each
   present -- the frame is re-rendered from VRAM every tick, so it leaves no trail. */
static const char *const MS_CURSOR[16] = {
    "o",          "oo",         "oXo",        "oXXo",
    "oXXXo",      "oXXXXo",     "oXXXXXo",    "oXXXXXXo",
    "oXXXXXXXo",  "oXXXXoooo",  "oXXoXo",     "oXo oXo",
    "ooo  oXo",   "oo    oXo",  "      oXo",  "       oo",
};
static void overlay_cursor(uint8_t *px, int W, int H, int stride, int mx, int my)
{
    int row;
    for (row = 0; row < 16; ++row) {
        const char *s = MS_CURSOR[row]; int col, y = my + row;
        if (y < 0 || y >= H) continue;
        for (col = 0; s[col]; ++col) {
            int x = mx + col; char c = s[col];
            if (c == ' ' || x < 0 || x >= W) continue;
            px[y * stride + x] = (c == 'o') ? 0 : 15;     /* black outline / white fill */
        }
    }
}

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
/* Window caption: "Windows XP Virtual DOS Machine" idle, "... - PROG.EXE" while a
   program runs. Safe to call from the V86 thread (SetWindowText posts WM_SETTEXT). */
#define VDM_WIN_TITLE "Windows XP Virtual DOS Machine"
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
        if (g_title_dirty) { set_window_title(); g_title_dirty = 0; }  /* apply on UI thread */
        /* Drive the PIT from REAL elapsed time so the BIOS tick (0040:006C) and
           INT 1Ah track wall-clock regardless of WM_TIMER jitter; clamp after a
           stall so we don't flood a catch-up burst of ticks. */
        { static DWORD last = 0; DWORD now = GetTickCount();
          DWORD dms = last ? (now - last) : 16; last = now;
          if (dms > 500) dms = 500;
          g_pit.frame_us = dms * 1000u; }
        EnterCriticalSection(&g_lock);
        vdd_bus_frame(&g_bus);          /* tick PIT + render into g_vid.frame       */
        if (g_ms_hidden == 0 && g_vid.frame.bpp == 8 && g_vid.frame.pixels)
            overlay_cursor((uint8_t *)g_vid.frame.pixels, g_vid.frame.w, g_vid.frame.h,
                           (int)g_vid.frame.stride, g_ms_x, g_ms_y);   /* driver mouse cursor */
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
    case WM_MOUSEMOVE:                   /* map client -> guest pixels, + buttons */
    case WM_LBUTTONDOWN: case WM_LBUTTONUP:
    case WM_RBUTTONDOWN: case WM_RBUTTONUP:
    case WM_MBUTTONDOWN: case WM_MBUTTONUP: {
        RECT rc; int cw, ch, fw, fh; LONG b = 0;
        fw = g_vid.frame.w ? (int)g_vid.frame.w : 640;
        fh = g_vid.frame.h ? (int)g_vid.frame.h : 480;
        GetClientRect(h, &rc);
        cw = rc.right; ch = rc.bottom - (g_pd.status_h ? g_pd.status_h : PRESENT_STATUS_H);
        if (cw < 1) cw = 1;
        if (ch < 1) ch = 1;
        { int fx = (short)LOWORD(lp) * fw / cw, fy = (short)HIWORD(lp) * fh / ch;
          if (fx < 0) fx = 0;
          else if (fx >= fw) fx = fw - 1;
          if (fy < 0) fy = 0;
          else if (fy >= fh) fy = fh - 1;
          InterlockedExchange(&g_ms_x, fx); InterlockedExchange(&g_ms_y, fy); }
        if (wp & MK_LBUTTON) b |= 1;
        if (wp & MK_RBUTTON) b |= 2;
        if (wp & MK_MBUTTON) b |= 4;
        InterlockedExchange(&g_ms_btn, b);
        return 0; }
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
    wc.hIcon = LoadIconA(hi, MAKEINTRESOURCEA(101));    /* IDI_MAINICON: title bar + taskbar */
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

/* ====================================================================== *
 *  Mode-12h fill-loop fast path: a small, bounded, flags-accurate 8086    *
 *  interpreter.                                                           *
 *                                                                         *
 *  In mode 12h the A0000 window is PAGE_NOACCESS, so every guest pixel    *
 *  touch faults to us. QuickBASIC's PAINT/LINE fills are tight per-pixel  *
 *  loops (e.g. `MOV AL,ES:[SI] / OR AL,AL / JNZ / DEC DI / JNZ`), so one  *
 *  fill = hundreds of thousands of V86 round-trips and never finishes.    *
 *                                                                         *
 *  Fix: when an A0000 access faults, run the *whole* inner loop here --   *
 *  loads/stores (planar engine for A0000, flat for normal RAM), the       *
 *  arithmetic/logic group (computing CF/PF/AF/ZF/SF/OF), INC/DEC, string  *
 *  ops (REP, honouring DF), MOV, TEST, the flag ops, and Jcc/JMP/LOOP --  *
 *  until we hit an opcode we don't model or an iteration cap. Then we     *
 *  write the architectural state back and return to V86. It NEVER         *
 *  derails: any unmodeled byte stops the interpreter with EIP exactly on  *
 *  that instruction, so V86 re-executes it. 16-bit only (0x66/0x67/LOCK   *
 *  bail). One fault now drives the entire fill instead of one-per-pixel.  *
 * ====================================================================== */

/* Flat/planar guest memory for the interpreter: A0000 goes through the VGA
   engine (a read loads the latches), everything else is the directly-mapped
   V86 address space. These are the host hooks v86interp.h requires. */
static uint8_t imem_r8(uint32_t lin)
{ return (lin >= A000_LO && lin < A000_HI) ? vga_planar_read(&g_vid, lin - A000_LO)
                                           : *(volatile BYTE *)lin; }
static void imem_w8(uint32_t lin, uint8_t v)
{ if (lin >= A000_LO && lin < A000_HI) vga_planar_write(&g_vid, lin - A000_LO, v);
  else *(volatile BYTE *)lin = v; }

/* Port I/O dispatched to the device bus (same path as host_try_io). The
   interpreter already runs under g_lock, which is what the bus needs. */
static uint32_t iio_in(uint16_t port, int width)
{ uint32_t v = 0; vdd_bus_io(&g_bus, port, (uint8_t)width, 1, &v); return v; }
static void iio_out(uint16_t port, int width, uint32_t val)
{ uint32_t v = val; vdd_bus_io(&g_bus, port, (uint8_t)width, 0, &v); }

#include "v86interp.h"

/* The mode-12h trap-storm escape hatch. By default V86 runs on the real CPU and
   each VGA access (memory OR port) is emulated one-at-a-time as a device access
   -- pure device virtualization. But QuickBasic plots pixels one at a time and
   reprograms a VGA register via OUT *between* pixels, so a fill is hundreds of
   thousands of fault round-trips (port faults AND memory faults) and crawls.

   host_interp() is the opt-in batching interpreter: load the V86 register file,
   run up to `cap` instructions in the host (the inner loop -- planar A0000
   access, IN/OUT through the bus, ALU, CALL/RET, branches), then write the
   architectural state back. The caller (the service loop) engages it ONLY on a
   detected trap-storm (the same tight PC window faulting repeatedly), so it's a
   measured fallback for proven pathological video loops, not a blanket policy.
   Returns the number of instructions executed (0 if the faulting instruction
   itself is unmodeled -> caller falls through). */
#define STORM_WINDOW   128       /* faults within this PC span count as "the same loop" */
#define STORM_GATE      8        /* consecutive in-window faults -> escalate to the interpreter */
#define TIER1_CAP  2000000L      /* interpreter iteration ceiling once escalated        */

static long host_interp(volatile BYTE *tib, long cap)
{
    icpu c; long iters;

    c.r[0] = (uint16_t)VDM_REG(tib, VTIB_EAX); c.r[1] = (uint16_t)VDM_REG(tib, VTIB_ECX);
    c.r[2] = (uint16_t)VDM_REG(tib, VTIB_EDX); c.r[3] = (uint16_t)VDM_REG(tib, VTIB_EBX);
    c.r[4] = (uint16_t)VDM_REG(tib, VTIB_ESP); c.r[5] = (uint16_t)VDM_REG(tib, VTIB_EBP);
    c.r[6] = (uint16_t)VDM_REG(tib, VTIB_ESI); c.r[7] = (uint16_t)VDM_REG(tib, VTIB_EDI);
    c.seg[0] = (uint16_t)VDM_REG(tib, VTIB_ES); c.seg[1] = (uint16_t)VDM_REG(tib, VTIB_CS);
    c.seg[2] = (uint16_t)VDM_REG(tib, VTIB_SS); c.seg[3] = (uint16_t)VDM_REG(tib, VTIB_DS);
    c.seg[4] = (uint16_t)VDM_REG(tib, VTIB_FS); c.seg[5] = (uint16_t)VDM_REG(tib, VTIB_GS);
    c.ip = (uint16_t)VDM_REG(tib, VTIB_EIP); c.flags = VDM_REG(tib, VTIB_EFLAGS);

    EnterCriticalSection(&g_lock);
    for (iters = 0; iters < cap; ++iters)
        if (!istep(&c)) break;
    LeaveCriticalSection(&g_lock);

    if (iters == 0) return 0;                          /* first opcode unmodeled */

    VDM_SET16(tib, VTIB_EAX, c.r[0]); VDM_SET16(tib, VTIB_ECX, c.r[1]);
    VDM_SET16(tib, VTIB_EDX, c.r[2]); VDM_SET16(tib, VTIB_EBX, c.r[3]);
    VDM_SET16(tib, VTIB_ESP, c.r[4]); VDM_SET16(tib, VTIB_EBP, c.r[5]);
    VDM_SET16(tib, VTIB_ESI, c.r[6]); VDM_SET16(tib, VTIB_EDI, c.r[7]);
    VDM_SET16(tib, VTIB_EIP, c.ip);
    /* update only the low 16 flag bits (arith + DF); keep VM/IOPL/IF etc. */
    VDM_REG(tib, VTIB_EFLAGS) = (VDM_REG(tib, VTIB_EFLAGS) & 0xFFFF0000u) | (c.flags & 0xFFFFu);
    return iters;
}

/* Crash diagnostic (DPMI spike): the PM switch works but VdmStartExecution faults
   inside the monitor when it runs PM, crashing the host with no info. This VEH
   catches the fault, dumps the exception (code/addr/params) + the host CONTEXT +
   the guest PM CONTEXT to the log, then exits CLEANLY (no WER dialog) so the batch
   still prints the log. Only meaningful once g_dpmi_pm is set. */
static int s_veh_count = 0;
static LONG CALLBACK dpmi_crash_veh(EXCEPTION_POINTERS *ep)
{
    static char cb[1024]; char *p = cb;
    EXCEPTION_RECORD *er = ep->ExceptionRecord;
    CONTEXT *cx = ep->ContextRecord;
    if (!g_dpmi_pm) return EXCEPTION_CONTINUE_SEARCH;   /* only handle PM events */

    /* --- Reflected PM software interrupt = the DPMI INT 31h dispatch (run 26) ---------
       Under VdmStartExecution the kernel reflects a PM INT nn by advancing EIP past it and
       reloading FLAT CS/SS (0x1B/0x23), leaving the int OFFSET in EDX and the guest's other
       regs intact. So: CS flat + in PM => a reflected guest INT. Read the vector from the
       instruction bytes at [code_base+EDX] (the guest issues only INT 31h), SERVICE it as a
       DPMI call (returns in the CONTEXT, CF in EFlags), restore the LDT CS/SS, and resume the
       guest past the INT. The VEH IS the protected-mode DPMI handler, on the real CPU. */
    if (cx->SegCs == 0x1B && s_veh_count < 256) {
        DWORD site = g_dpmi_code_base + (cx->Edx & 0xFFFF);
        const BYTE *ib = (const BYTE *)(ULONG_PTR)site;
        DWORD func = cx->Eax & 0xFFFF;
        BYTE  vec  = (ib[0] == 0xCD) ? ib[1] : 0x31;    /* CD nn -> vector; default 31h    */
        ++s_veh_count;
        p = zput(p, "DPMI INT"); p = zhex(p, vec); p = zput(p, "h #"); p = zhex(p, (unsigned)s_veh_count);
        p = zput(p, ": AX=0x"); p = zhex(p, func);
        p = zput(p, " BX=0x"); p = zhex(p, cx->Ebx & 0xFFFF);
        p = zput(p, " CX=0x"); p = zhex(p, cx->Ecx & 0xFFFF);
        p = zput(p, " [site EDX=0x"); p = zhex(p, cx->Edx & 0xFFFF);
        p = zput(p, " EIP=0x"); p = zhex(p, cx->Eip);
        p = zput(p, " b@site="); p = zdump(p, ib, 4); p = zput(p, "]");
        { const BYTE *sent = (const BYTE *)(ULONG_PTR)0x1600;   /* guest sentinel DS:0x600 */
          p = zput(p, " sentinel@0x1600="); p = zdump(p, sent, 4); }
        cx->EFlags &= ~1u;                              /* default: CF=0 (success)          */
        switch (func) {
        case 0x0400:                                    /* get DPMI version                 */
            cx->Eax = (cx->Eax & 0xFFFF0000) | 0x005A;  /* AH=0 major, AL=0x5A (90) minor    */
            cx->Ebx = (cx->Ebx & 0xFFFF0000) | 0x0000;  /* BX flags: 16-bit host             */
            cx->Ecx = (cx->Ecx & 0xFFFF0000) | 0x0003;  /* CL=3 (80386)                      */
            cx->Edx = (cx->Edx & 0xFFFF0000) | 0x7008;  /* DH=slave 0x70, DL=master 0x08 PIC  */
            p = zput(p, " -> DPMI 0.90 (386)");
            break;
        case 0x0000:                                    /* allocate LDT descriptors (CX=count) */
            cx->Eax = (cx->Eax & 0xFFFF0000) | 0x001F;  /* base selector 0x1F (spike stub)    */
            p = zput(p, " -> alloc base sel 0x1F");
            break;
        default:
            cx->EFlags |= 1u;                           /* CF=1: unsupported function        */
            p = zput(p, " -> UNSUPPORTED (CF=1)");
            break;
        }
        p = zput(p, "\r\n");
        log_append(LOG_PATH, cb, p); serial_out(cb, p);
        cx->SegCs = 0x0F; cx->SegSs = 0x17;             /* restore the guest's LDT selectors  */
        return EXCEPTION_CONTINUE_EXECUTION;            /* resume the guest past the INT      */
    }

    /* --- genuine (non-reflected) PM fault: full dump + clean exit -------------------- */
    p = zput(p, "\r\nDPMI FATAL: exception code=0x"); p = zhex(p, er->ExceptionCode);
    p = zput(p, " at 0x"); p = zhex(p, (unsigned)(ULONG_PTR)er->ExceptionAddress);
    p = zput(p, "\r\n  CS:EIP=0x"); p = zhex(p, cx->SegCs); p = zput(p, ":0x"); p = zhex(p, cx->Eip);
    p = zput(p, " SS:ESP=0x"); p = zhex(p, cx->SegSs); p = zput(p, ":0x"); p = zhex(p, cx->Esp);
    p = zput(p, " EFL=0x"); p = zhex(p, cx->EFlags); p = zput(p, "\r\n");
    p = zput(p, "  DS=0x"); p = zhex(p, cx->SegDs); p = zput(p, " ES=0x"); p = zhex(p, cx->SegEs);
    p = zput(p, " FS=0x"); p = zhex(p, cx->SegFs); p = zput(p, " GS=0x"); p = zhex(p, cx->SegGs);
    p = zput(p, "\r\n  EAX=0x"); p = zhex(p, cx->Eax); p = zput(p, " EBX=0x"); p = zhex(p, cx->Ebx);
    p = zput(p, " ECX=0x"); p = zhex(p, cx->Ecx); p = zput(p, " EDX=0x"); p = zhex(p, cx->Edx);
    p = zput(p, "\r\n");
    { const BYTE *fb = (const BYTE *)(ULONG_PTR)(er->ExceptionAddress);
      p = zput(p, "  bytes@fault: "); p = zdump(p, fb, 16); }
    log_append(LOG_PATH, cb, p);
    serial_out(cb, p);
    ExitProcess(0xDE0);                                 /* clean exit; batch dumps the log */
    return EXCEPTION_CONTINUE_SEARCH;                   /* not reached */
}

/* DPMI test watchdog: if the PM guest neither faults to the VEH nor exits within a few
   seconds (the kernel skip+resumes PM faults, so the guest spins), terminate cleanly so
   the batch dumps the log and locks release. Makes every DPMI run self-terminating. */
static DWORD WINAPI dpmi_watchdog(LPVOID param)
{
    static char wb[256]; char *q = wb; unsigned i; (void)param;
    const void *sent = (const void *)(ULONG_PTR)0x1600;   /* guest sentinel DS:0x600 */
    q = zput(q, "STAGE3-DPMI: watchdog started; sampling sentinel@0x1600 (proves PM exec)\r\n");
    serial_out(wb, q); q = wb;
    /* Sample the guest's sentinel concurrently: if the PM guest executed its opening
       'mov [0x600],BEEF/CAFE', we see it here even though dpmi_enter_pm silently
       terminates on a later fault. BEEF/CAFE => PM code genuinely ran on the real CPU. */
    for (i = 0; i < 10; ++i) {
        Sleep(250);
        q = zput(q, "  wd["); q = zhex(q, i); q = zput(q, "] sentinel="); q = zdump(q, sent, 4);
        serial_out(wb, q); q = wb;
    }
    q = zput(q, "STAGE3-DPMI: watchdog terminating\r\n");
    log_append(LOG_PATH, wb, q);
    serial_out(wb, q);
    /* TerminateProcess (forceful) -- ExitProcess hangs trying to unwind the PM engine
       thread (un-terminable LDT context). */
    TerminateProcess(GetCurrentProcess(), 0xDD0);
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
    static const BYTE bop33[] = { VDM_BOP0, VDM_BOP1, 0x33, 0xCF }; /* BOP 0x33 ; iret */
    /* INT 08h (timer): tick via BOP, then chain INT 1Ch, then iret. INT 1Ch is a
       bare iret by default (the user-timer hook a program may repoint). INT 1Ah
       (BIOS time-of-day) is a plain BOP. */
    static const BYTE bop08[] = { VDM_BOP0, VDM_BOP1, 0x08, 0xCD, 0x1C, 0xCF };
    static const BYTE bop1c[] = { 0xCF };                           /* iret stub       */
    static const BYTE bop1a[] = { VDM_BOP0, VDM_BOP1, 0x1A, 0xCF }; /* BOP 0x1A ; iret */
    static const BYTE bop2f[] = { VDM_BOP0, VDM_BOP1, 0x2F, 0xCF }; /* INT 2Fh ; iret  */
    /* XMS API entry: reached by FAR CALL (INT 2Fh AX=4310 hands back ES:BX), so it
       ends in RETF (0xCB), not IRET. */
    static const BYTE bopxms[] = { VDM_BOP0, VDM_BOP1, 0x43, 0xCB };
    static const BYTE bop67[] = { VDM_BOP0, VDM_BOP1, 0x67, 0xCF }; /* INT 67h ; iret  */
    static const BYTE emmname[] = { 'E','M','M','X','X','X','X','0' };  /* EMS device header name */
    HANDLE ui = NULL;

    (void)hInst; (void)hPrev; (void)lpCmd; (void)nShow;
    progpath[0] = 0; args[0] = 0;

    p = zput(p, "NTVDMEX clean host\r\nSTAGE0: WinMain entered [build dpmi-harness-v25]\r\n");
    log_write(LOG_PATH, report, p);
    serial_init();                                      /* DPMI harness: COM1 log sink */
    serial_out(report, p);
    AddVectoredExceptionHandler(1, dpmi_crash_veh);     /* DPMI spike crash diagnostic */

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
    /* EMS page frame must be mapped AFTER VdmInitialize (see v86_map_ems_frame). */
    g_ems_frame_lin = v86_map_ems_frame();
    p = zput(p, "STAGE1: ems_frame lin=0x"); p = zhex(p, g_ems_frame_lin);
    p = zput(p, " seg=0x"); p = zhex(p, g_ems_frame_lin >> 4); p = zput(p, "\r\n");

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
    g_tib_dbg = tib;                                    /* let the crash VEH dump guest state */
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
    g_title_dirty = 1;                             /* UI thread sets the caption + prog name */

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
    for (i = 0; i < sizeof(bop33); ++i) hdlr[0x30 + i] = bop33[i];  /* INT 33h stub */
    *(volatile WORD *)(0x33 * 4)     = 0x0030;              /* IVT[0x33].offset    */
    *(volatile WORD *)(0x33 * 4 + 2) = DOS_HDLR_SEG;        /* IVT[0x33].segment   */
    for (i = 0; i < sizeof(bop08); ++i) hdlr[0x34 + i] = bop08[i];  /* INT 08h stub */
    *(volatile WORD *)(0x08 * 4)     = 0x0034;              /* IVT[0x08].offset    */
    *(volatile WORD *)(0x08 * 4 + 2) = DOS_HDLR_SEG;        /* IVT[0x08].segment   */
    for (i = 0; i < sizeof(bop1c); ++i) hdlr[0x3A + i] = bop1c[i];  /* INT 1Ch iret */
    *(volatile WORD *)(0x1C * 4)     = 0x003A;              /* IVT[0x1C].offset    */
    *(volatile WORD *)(0x1C * 4 + 2) = DOS_HDLR_SEG;        /* IVT[0x1C].segment   */
    for (i = 0; i < sizeof(bop1a); ++i) hdlr[0x3C + i] = bop1a[i];  /* INT 1Ah stub */
    *(volatile WORD *)(0x1A * 4)     = 0x003C;              /* IVT[0x1A].offset    */
    *(volatile WORD *)(0x1A * 4 + 2) = DOS_HDLR_SEG;        /* IVT[0x1A].segment   */
    for (i = 0; i < sizeof(bop2f); ++i) hdlr[0x40 + i] = bop2f[i];  /* INT 2Fh stub */
    *(volatile WORD *)(0x2F * 4)     = 0x0040;              /* IVT[0x2F].offset    */
    *(volatile WORD *)(0x2F * 4 + 2) = DOS_HDLR_SEG;        /* IVT[0x2F].segment   */
    for (i = 0; i < sizeof(bopxms); ++i) hdlr[XMS_ENTRY_OFF + i] = bopxms[i];  /* XMS far-call entry */
    for (i = 0; i < sizeof(bop67); ++i) hdlr[0x48 + i] = bop67[i];  /* INT 67h (EMM) stub */
    *(volatile WORD *)(0x67 * 4)     = 0x0048;              /* IVT[0x67].offset    */
    *(volatile WORD *)(0x67 * 4 + 2) = DOS_HDLR_SEG;        /* IVT[0x67].segment   */
    /* DPMI mode-switch entry (far-called): BOP 0x50 ; RETF. The host services the
       BOP by switching to PM; the RETF only executes if the switch fails. */
    hdlr[DPMI_ENTRY_OFF + 0] = VDM_BOP0; hdlr[DPMI_ENTRY_OFF + 1] = VDM_BOP1;
    hdlr[DPMI_ENTRY_OFF + 2] = DPMI_BOP; hdlr[DPMI_ENTRY_OFF + 3] = 0xCB; /* RETF */
    /* EMS detection method 2: programs read the INT 67h vector's segment:000Ah for
       the device-driver name "EMMXXXX0". Park it in the handler segment. */
    for (i = 0; i < sizeof(emmname); ++i) hdlr[DOS_EMM_NAME_OFF + i] = emmname[i];

    dos_psp_build(NULL, DOS_PSP_SEG, DOS_ENV_SEG, DOS_MEM_TOP);
    dos_env_build(NULL, DOS_ENV_SEG, progpath[0] ? progpath : "C:\\PROGRAM.COM");  /* M2.5: env */
    dos_cmdtail_build(NULL, DOS_PSP_SEG, args);                                    /* M2.5: args */
    dos_int21_init(&m, dos_mcb_init(NULL));
    xms_init(&g_xms, 16384, xms_host_alloc, xms_host_free, NULL);  /* M4: 16MB XMS pool */
    ems_init(&g_ems, (uint16_t)(g_ems_frame_lin >> 4), EMS_POOL_PAGES,
             (volatile BYTE *)g_ems_frame_lin,
             ems_host_alloc, ems_host_free, NULL);             /* M4: 8MB EMS pool   */

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
    g_spk.pit = &g_pit;                         /* speaker tone <- PIT channel 2 */
    g_spk_dev = vdd_speaker_device(&g_spk);
    vdd_bus_add(&g_bus, &g_spk_dev);            /* PC speaker: claims port 0x61  */
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
    { static uint32_t s_last_fault = 0; static int s_storm = 0;
    while (g_running) {
        /* Deliver a pending PIT IRQ0 as INT 08h when the guest's main-line
           interrupts are enabled. We regain control at event boundaries, almost
           always inside a BOP stub (CS == DOS_HDLR_SEG) where the LIVE IF is the
           handler's (cleared by the CD nn that vectored in) -- the guest's real
           IF is the FLAGS the stub will IRET to, at SS:SP+4. Outside a stub
           (e.g. an I/O fault from main-line) the live EFLAGS IF applies. Skip if
           we're inside our own INT 08h stub, to avoid timer re-entrancy. */
        if (g_irq0_pending) {
            DWORD cs = VDM_REG(tib, VTIB_CS) & 0xFFFF, ip = VDM_REG(tib, VTIB_EIP) & 0xFFFF;
            DWORD fl;
            if (cs == DOS_HDLR_SEG) {
                DWORD ss = VDM_REG(tib, VTIB_SS) & 0xFFFF, sp = VDM_REG(tib, VTIB_ESP) & 0xFFFF;
                fl = peekw((ss << 4) + ((sp + 4) & 0xFFFF));   /* main-line FLAGS the stub returns to */
            } else {
                fl = VDM_REG(tib, VTIB_EFLAGS);
            }
            if ((fl & 0x200) && !(cs == DOS_HDLR_SEG && ip >= 0x34 && ip < 0x3A)) {
                g_irq0_pending = 0;
                inject_int(tib, 0x08);
            }
        }
        ev = v86_run(tib, &st);
        /* SPIKE: once in protected mode, stop at the FIRST PM event and dump the raw
           taxonomy (event/info/selectors) -- this is how the spike learns how the
           monitor reflects a PM INT 31h / fault. Later increments replace this with
           a real INT 31h dispatch. */
        if (g_dpmi_pm) {
            DWORD csv = VDM_REG(tib, VTIB_CS) & 0xFFFF, ipv = VDM_REG(tib, VTIB_EIP) & 0xFFFF;
            p = zput(p, "STAGE3-DPMI: PM stop event=0x"); p = zhex(p, ev);
            p = zput(p, " status=0x"); p = zhex(p, (unsigned)st);
            p = zput(p, " info=0x"); p = zhex(p, VDM_REG(tib, VTIB_EVENT_INFO));
            p = zput(p, " CS:IP=0x"); p = zhex(p, csv); p = zput(p, ":0x"); p = zhex(p, ipv);
            p = zput(p, " EFL=0x"); p = zhex(p, VDM_REG(tib, VTIB_EFLAGS));
            p = zput(p, " SS:SP=0x"); p = zhex(p, VDM_REG(tib, VTIB_SS) & 0xFFFF);
            p = zput(p, ":0x"); p = zhex(p, VDM_REG(tib, VTIB_ESP) & 0xFFFF);
            p = zput(p, "\r\n  VTIB[5A8..]: "); p = zdump(p, (const void *)(tib + 0x5A8), 0x20);
            p = zput(p, "\r\n");
            log_append(LOG_PATH, base, p); serial_out(base, p); p = base;
            break;
        }
        /* I/O port trap (event 0; VM-confirmed) or a generic GP fault (event 2):
           if the faulting instruction is an IN/OUT we can decode, service it via
           the VDD bus and resume; otherwise fall through to the stop dump. */
        if (ev == VDM_EVENT_IO || ev == VDM_EVENT_GPFAULT) {
            int handled;
            /* Trap-storm detection over ALL faults (port + A0000 memory): the
               per-pixel VGA loop faults repeatedly in a tight PC window. Once a
               storm is established in mode 12h, escalate to the batching
               interpreter so the whole inner loop (OUTs + pixel writes) runs in
               one shot; otherwise emulate the single faulting access. */
            DWORD fcs = VDM_REG(tib, VTIB_CS) & 0xFFFF, fip = VDM_REG(tib, VTIB_EIP) & 0xFFFF;
            uint32_t cur = (fcs << 4) + fip;
            uint32_t d = (cur > s_last_fault) ? (cur - s_last_fault) : (s_last_fault - cur);
            s_storm = (d <= STORM_WINDOW) ? (s_storm + 1) : 0;
            s_last_fault = cur;
            if (g_a000_prot && s_storm >= STORM_GATE && host_interp(tib, TIER1_CAP) > 0)
                continue;                           /* batched the hot loop            */
            EnterCriticalSection(&g_lock);
            handled = host_try_io(tib, &g_bus);     /* single port op (no logging)     */
            LeaveCriticalSection(&g_lock);
            if (handled) continue;
            if (g_a000_prot && host_interp(tib, 1) > 0) continue;  /* single A0000 access */
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
            ntvdd_regs r; regs_load(&r, tib);
            EnterCriticalSection(&g_lock);
            vdd_bus_deliver_int(&g_bus, 0x10, &r);
            LeaveCriticalSection(&g_lock);
            regs_store(&r, tib);
            a000_protect(vdd_video_planar_active(&g_vid));   /* trap A0000 in mode 12h */
            VDM_REG(tib, VTIB_EIP) += 3;
            continue;
        }
        if ((VDM_REG(tib, VTIB_EVENT_INFO) & 0xFF) == 0x16) {
            ntvdd_regs r; uint8_t ah16; regs_load(&r, tib); ah16 = r_ah(&r);
            for (;;) {                          /* AH=00/10 block until a key     */
                EnterCriticalSection(&g_lock);
                vdd_bus_deliver_int(&g_bus, 0x16, &r);
                LeaveCriticalSection(&g_lock);
                if ((ah16 != 0x00 && ah16 != 0x10) || r.zf == 0 || !g_running) break;
                WaitForSingleObject(g_key_event, 50);
            }
            regs_store(&r, tib);
            host_set_flags(tib, r.cf, r.zf);
            VDM_REG(tib, VTIB_EIP) += 3;
            continue;
        }
        if ((VDM_REG(tib, VTIB_EVENT_INFO) & 0xFF) == 0x33) {   /* INT 33h mouse  */
            mouse_int33(tib);
            VDM_REG(tib, VTIB_EIP) += 3;
            continue;
        }
        if ((VDM_REG(tib, VTIB_EVENT_INFO) & 0xFF) == 0x08) {   /* INT 08h timer tick */
            ntvdd_regs r; regs_load(&r, tib);
            EnterCriticalSection(&g_lock);
            vdd_bus_deliver_int(&g_bus, 0x08, &r);  /* bump BIOS tick at 0040:006C */
            LeaveCriticalSection(&g_lock);
            regs_store(&r, tib);
            VDM_REG(tib, VTIB_EIP) += 3;            /* -> CD 1C (chain user timer) */
            continue;
        }
        if ((VDM_REG(tib, VTIB_EVENT_INFO) & 0xFF) == 0x1A) {   /* INT 1Ah BIOS time */
            ntvdd_regs r; regs_load(&r, tib);
            EnterCriticalSection(&g_lock);
            vdd_bus_deliver_int(&g_bus, 0x1A, &r);
            LeaveCriticalSection(&g_lock);
            regs_store(&r, tib);
            host_set_flags(tib, r.cf, r.zf);
            VDM_REG(tib, VTIB_EIP) += 3;
            continue;
        }
        if ((VDM_REG(tib, VTIB_EVENT_INFO) & 0xFF) == 0x2F) {   /* INT 2Fh multiplex */
            DWORD ax = VDM_REG(tib, VTIB_EAX) & 0xFFFF;
            p = zput(p, "STAGE2: BOP2F ax=0x"); p = zhex(p, ax); p = zput(p, "\r\n");
            log_append(LOG_PATH, base, p); serial_out(base, p); p = base;
            if (ax == 0x4300) {                                 /* XMS installation check */
                VDM_SET16(tib, VTIB_EAX, (VDM_REG(tib, VTIB_EAX) & 0xFF00) | 0x80);  /* AL=80h installed */
            } else if (ax == 0x4310) {                          /* get XMS entry -> ES:BX */
                VDM_SET16(tib, VTIB_ES, DOS_HDLR_SEG);
                VDM_SET16(tib, VTIB_EBX, XMS_ENTRY_OFF);
            } else if (ax == 0x1687) {                           /* DPMI installation check (SPIKE) */
                /* AX=0 present; BX=0 (16-bit only for now); CL=3 (386); DX=0.90;
                   SI=0 private paras; ES:DI = mode-switch entry to FAR-CALL. */
                VDM_SET16(tib, VTIB_EAX, 0);
                VDM_SET16(tib, VTIB_EBX, 0);
                VDM_SET16(tib, VTIB_ECX, (VDM_REG(tib, VTIB_ECX) & 0xFF00) | 0x03);
                VDM_SET16(tib, VTIB_EDX, 0x005A);               /* DPMI 0.90        */
                VDM_SET16(tib, VTIB_ESI, 0);
                VDM_SET16(tib, VTIB_ES,  DOS_HDLR_SEG);
                VDM_SET16(tib, VTIB_EDI, DPMI_ENTRY_OFF);
                p = zput(p, "STAGE2: DPMI 1687 -> AX=0 ES:DI=0x"); p = zhex(p, DOS_HDLR_SEG);
                p = zput(p, ":0x"); p = zhex(p, DPMI_ENTRY_OFF); p = zput(p, " (guest must far-call this)\r\n");
                log_append(LOG_PATH, base, p); serial_out(base, p); p = base;
            }
            /* other INT 2Fh multiplex functions: left to the guest (no-op pass) */
            VDM_REG(tib, VTIB_EIP) += 3;                        /* -> the IRET (CF) */
            continue;
        }
        if ((VDM_REG(tib, VTIB_EVENT_INFO) & 0xFF) == 0x43) {   /* XMS API far-call entry */
            p = zput(p, " XMS AH=0x"); p = zhex(p, (VDM_REG(tib, VTIB_EAX) >> 8) & 0xFF); p = zput(p, "\r\n");
            log_append(LOG_PATH, base, p); p = base;
            host_xms(tib);
            VDM_REG(tib, VTIB_EIP) += 3;                        /* -> the RETF      */
            continue;
        }
        if ((VDM_REG(tib, VTIB_EVENT_INFO) & 0xFF) == 0x67) {   /* INT 67h EMM (EMS) */
            p = zput(p, " EMS AH=0x"); p = zhex(p, (VDM_REG(tib, VTIB_EAX) >> 8) & 0xFF); p = zput(p, "\r\n");
            log_append(LOG_PATH, base, p); p = base;
            EnterCriticalSection(&g_lock);
            host_ems(tib);
            LeaveCriticalSection(&g_lock);
            VDM_REG(tib, VTIB_EIP) += 3;                        /* -> the IRET      */
            continue;
        }
        if ((VDM_REG(tib, VTIB_EVENT_INFO) & 0xFF) == DPMI_BOP) {  /* DPMI real->PM switch */
            DWORD csv = VDM_REG(tib, VTIB_CS) & 0xFFFF, ipv = VDM_REG(tib, VTIB_EIP) & 0xFFFF;
            LONG reg_st = 0, set_st = 0; int sw;
            p = zput(p, "STAGE3: DPMI_BOP far-call LANDED @ 0x"); p = zhex(p, csv);
            p = zput(p, ":0x"); p = zhex(p, ipv); p = zput(p, " -- switching to PM\r\n");
            log_append(LOG_PATH, base, p); serial_out(base, p); p = base;
            sw = dpmi_switch_to_pm(tib, 0, &reg_st, &set_st);
            p = zput(p, " [svc11=0x"); p = zhex(p, (unsigned)reg_st);
            p = zput(p, " svc10=0x"); p = zhex(p, (unsigned)set_st); p = zput(p, "]");
            p = zput(p, " retcs=0x"); p = zhex(p, g_dpmi_dbg[0]);
            p = zput(p, " clo=0x"); p = zhex(p, g_dpmi_dbg[2]);
            p = zput(p, " chi=0x"); p = zhex(p, g_dpmi_dbg[3]);
            if (sw == 0) {
                unsigned steps;
                g_dpmi_pm = 1;
                g_dpmi_code_base = g_dpmi_dbg[0] << 4;   /* retcs<<4 = guest PM code linear base */
                p = zput(p, " -> PM ok (CS=0x"); p = zhex(p, VDM_REG(tib, VTIB_CS) & 0xFFFF);
                p = zput(p, ":0x"); p = zhex(p, VDM_REG(tib, VTIB_EIP) & 0xFFFF);
                p = zput(p, ") -> DPMI PM loop\r\n");
                log_append(LOG_PATH, base, p); serial_out(base, p); p = base;
                /* Safety watchdog: an un-terminable spin still self-kills after ~3s. */
                { HANDLE wd = CreateThread(NULL, 0, dpmi_watchdog, NULL, 0, NULL);
                  if (wd) CloseHandle(wd); }
                /* --- DPMI protected-mode execution loop -----------------------------------
                   Run the client in PM via dpmi_enter_pm (real ntvdm far-jmp). It returns when
                   the guest hits a BOP (kernel reflects C4 C4 as VTIB_EVENT=4 -- proven run 32).
                   The client issues DPMI calls as `C4 C4 31` (BOP 0x31) and exits via
                   `C4 C4 4C`. We service the call by AX, write returns into the guest CONTEXT,
                   advance past the 3-byte BOP, and re-enter PM. */
                for (steps = 0; steps < 256; ++steps) {
                    DWORD ev, info, ax;
                    dpmi_enter_pm(tib);
                    ev   = VDM_REG(tib, VTIB_EVENT);
                    info = VDM_REG(tib, VTIB_EVENT_INFO) & 0xFF;
                    ax   = VDM_REG(tib, VTIB_EAX) & 0xFFFF;
                    if (ev == 4 && info == 0x31) {                 /* DPMI service call */
                        p = zput(p, "DPMI31 AX=0x"); p = zhex(p, ax);
                        p = zput(p, " CX=0x"); p = zhex(p, VDM_REG(tib, VTIB_ECX) & 0xFFFF);
                        VDM_REG(tib, VTIB_EFLAGS) &= ~1u;          /* default CF=0 (success) */
                        switch (ax) {
                        case 0x0400:                               /* get DPMI version */
                            VDM_SET16(tib, VTIB_EAX, 0x005A);      /* 0.90 (AH=0,AL=90) */
                            VDM_SET16(tib, VTIB_EBX, 0x0001);      /* flags: 32-bit? no -> 16-bit host */
                            VDM_SET16(tib, VTIB_ECX, 0x0003);      /* CL=3 (80386) */
                            VDM_SET16(tib, VTIB_EDX, 0x0870);      /* DH/DL PIC bases */
                            p = zput(p, " -> ver 0.90");
                            break;
                        case 0x0000:                               /* allocate LDT descriptors */
                            VDM_SET16(tib, VTIB_EAX, 0x001F);      /* base selector (spike stub) */
                            p = zput(p, " -> sel 0x1F");
                            break;
                        default:
                            VDM_REG(tib, VTIB_EFLAGS) |= 1u;       /* CF=1: unsupported */
                            p = zput(p, " -> UNSUP");
                            break;
                        }
                        p = zput(p, "\r\n"); log_append(LOG_PATH, base, p); serial_out(base, p); p = base;
                        VDM_REG(tib, VTIB_EIP) += 3;               /* past C4 C4 31 -> resume client */
                        continue;
                    }
                    if (ev == 4 && info == 0x4C) {                 /* DPMI client exit */
                        DWORD r1 = *(volatile WORD *)(ULONG_PTR)0x1600;
                        DWORD r2 = *(volatile WORD *)(ULONG_PTR)0x1602;
                        p = zput(p, "DPMI: client EXIT after "); p = zhex(p, steps);
                        p = zput(p, " calls. results@0x1600=0x"); p = zhex(p, r1);
                        p = zput(p, " @0x1602=0x"); p = zhex(p, r2);
                        p = zput(p, (r1 == 0x005A && r2 == 0x001F)
                                    ? "  <<< DPMI ROUND-TRIP OK >>>\r\n" : "\r\n");
                        log_append(LOG_PATH, base, p); serial_out(base, p); p = base;
                        break;
                    }
                    p = zput(p, "DPMI: unexpected PM stop event=0x"); p = zhex(p, ev);
                    p = zput(p, " info=0x"); p = zhex(p, info);
                    p = zput(p, " CS:EIP=0x"); p = zhex(p, VDM_REG(tib, VTIB_CS) & 0xFFFF);
                    p = zput(p, ":0x"); p = zhex(p, VDM_REG(tib, VTIB_EIP) & 0xFFFF); p = zput(p, "\r\n");
                    log_append(LOG_PATH, base, p); serial_out(base, p); p = base;
                    break;
                }
                break;
            }
            p = zput(p, " -> SWITCH FAILED (staying real mode, CF=1)\r\n");
            log_append(LOG_PATH, base, p); serial_out(base, p); p = base;
            VDM_REG(tib, VTIB_EFLAGS) |= 1;         /* CF=1 signals failure to the client */
            VDM_REG(tib, VTIB_EIP) += 3;            /* -> the RETF, returns real mode     */
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
    }   /* storm-state block */

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
