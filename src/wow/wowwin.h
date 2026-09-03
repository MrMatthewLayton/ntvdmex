#ifndef WOWWIN_H
#define WOWWIN_H
/*
 * wowwin.h -- ★★★★★ A Win16 WINDOW IS A REAL Win32 WINDOW. GH #128, session 42.
 *
 * ── THE CORRECTION THIS FILE EXISTS BECAUSE OF ───────────────────────────────
 * The first attempt at pixels drew a Windows 3.x desktop -- teal background, grey
 * frames, navy caption bars -- into the VGA framebuffer, so the Win16 window tree
 * appeared INSIDE the NTVDMEX host window. It worked, and it was the wrong
 * architecture, and the user stopped the session to say so:
 *
 *     "WOW16 apps should NOT draw inside the NTVDMEX window. They should draw to
 *      the Windows XP desktop, which is what Windows XP already does."
 *
 * That is exactly right, and it is what WOW IS. `wow32.dll` gives every Win16
 * window a real Win32 `HWND`, which is why a 16-bit program on XP gets a taskbar
 * button, a real title bar, real focus, real clipping against other applications,
 * and the OS's own input and painting. A VDM *console* window is what a DOS
 * session gets; a Win16 program is not a DOS session. Reimplementing a window
 * manager when we are running on one is both more code and less faithful -- and
 * it is the DOSBox-shaped answer, which this project is explicitly not.
 *
 * ── WHAT MAPS TO WHAT ───────────────────────────────────────────────────────
 *   RegisterClass   a real Win32 class whose lpfnWndProc is OURS (wowwin_proc)
 *   CreateWindow    a real CreateWindowExA; the HWND is kept in wowuser_win_t
 *   ShowWindow      the real one
 *   MDICLIENT/EDIT  the REAL Win32 system classes -- they exist, so use them
 *   input           the real window's own messages, translated into the Win16
 *                   queue, taken by the guest's own GetMessage
 *
 * ⚠ `CW_USEDEFAULT` MUST BE TRANSLATED, NOT PASSED THROUGH. Win16's is `0x8000`
 *   and Win32's is `0x80000000`; the `WS_*` style bits, by contrast, ARE the same
 *   values in both, which is why the style word can go straight across.
 *
 * ── ⚠⚠ THREADING IS THE DESIGN, NOT A DETAIL ────────────────────────────────
 * A Win32 window belongs to the thread that created it: its window procedure runs
 * on that thread, and `BeginPaint` and every GDI call against its DC must be made
 * from it. Guest code runs on the EXEC thread, and a guest that paints will call
 * `BeginPaint` from there -- so the real HWNDs are created and owned by the exec
 * thread, and that thread's Win32 queue is pumped:
 *
 *   - cheaply at every WOW32 BOP (`wowwin_pump`), so the window stays alive while
 *     the guest is working;
 *   - blocking inside Win16 `GetMessage` when the Win16 queue is empty, which is
 *     exactly where a Win16 task is supposed to wait.
 *
 * ⚠ A guest that runs a long BOP-free stretch will make its window unresponsive,
 *   because nothing is pumping. That is a real limitation of having one exec
 *   thread and it is stated rather than discovered later. Real WOW gives each
 *   Win16 task its own Win32 thread; we have the cooperative scheduler instead.
 *
 * ⚠ WIN32's `WM_CREATE` AND WIN16's ARE TWO DIFFERENT MESSAGES. Win32 sends one
 *   to `wowwin_proc` during `CreateWindowEx`, about the real window; the guest's
 *   own `WM_CREATE`, about its object, is delivered afterwards by
 *   `wowuser_want_create` through the callback machinery. Conflating them would
 *   re-enter the guest from inside `CreateWindowEx`.
 */

/* The prefix keeps a guest's class name out of the process-global Win32 class
   namespace -- `mpframe` is SYSEDIT's, not ours to occupy. */
#define WOWWIN_CLASS_PREFIX "NTVDMEX16."
/* ⚠ THE TWO CW_USEDEFAULTs ARE DIFFERENT VALUES. Both live here, next to the one
   function that converts between them, so the pair can be read in one glance --
   which is the only way a reader can see that passing one where the other is
   expected creates a window 32768 pixels from the origin rather than a default. */
#define CW_USEDEFAULT16     0x8000
#define CW_USEDEFAULT32     ((int)0x80000000)

/* Set once the exec thread has a window: the thread id that owns them all, so a
   pump on the wrong thread can be refused rather than silently doing nothing. */
static DWORD g_ww_thread = 0;
static DWORD g_ww_created = 0, g_ww_msgs = 0;

/* Forward: the window table this proc maps through. All defined in wowuser.h,
   which owns the table and is included after this file. */
typedef struct wowuser_win_s wowuser_win_t;
static WORD  wowwin_hwnd16(HWND h);
static wowuser_win_t *wowuser_findwin(WORD hwnd);
static int   wowuser_is_mdichild(const wowuser_win_t *w);
static HWND  wowuser_mdiclient_of(const wowuser_win_t *w);

/*
 * ── OUR WINDOW PROCEDURE FOR EVERY Win16 WINDOW ─────────────────────────────
 * Deliberately thin. The chrome -- border, caption, sizing, minimise, close --
 * is DefWindowProc's, i.e. the OS's, which is the entire reason for doing this
 * the right way round. What we do here is TRANSLATE: a real Win32 message that
 * the guest's own window procedure would expect becomes a Win16 queue entry, and
 * the guest takes it out of its own GetMessage.
 *
 * ⚠ ONLY WHAT THE GUEST CAN ACTUALLY USE. Posting every message would fill the
 *   ring with mouse moves the guest never asked for and would hide the ones it
 *   did. Keyboard and close are what a run has needed; the rest goes to
 *   DefWindowProc, and the log names anything that turns out to matter.
 * ⚠ WM_PAINT IS LEFT TO DefWindowProc ON PURPOSE. It validates the region, so
 *   there is no repaint storm; the guest is not asked to paint because it has no
 *   way to yet -- GDI's id space is not dispatched. The day it is, WM_PAINT
 *   starts being translated here and the guest's BeginPaint gets the real HDC.
 */
static LRESULT CALLBACK wowwin_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    WORD h16 = wowwin_hwnd16(h);
    switch (msg) {
    case WM_KEYDOWN: case WM_KEYUP: case WM_CHAR:
    case WM_SYSKEYDOWN: case WM_SYSKEYUP:
        /* ★ RELAYED VERBATIM. Win16 and Win32 agree on the message number, on
             wParam being the virtual key, and on the lParam bit field -- Win32
             inherited all three -- so the honest thing is to hand across exactly
             what the OS handed us rather than compose anything. */
        if (h16) {
            wowmsg_post(h16, (WORD)msg, (WORD)wp, (DWORD)lp, GetTickCount(), 0, 0);
            ++g_ww_msgs;
            return 0;
        }
        break;
    case WM_CLOSE:
        if (h16) { wowmsg_post(h16, (WORD)msg, 0, 0, GetTickCount(), 0, 0);
                   ++g_ww_msgs; return 0; }
        break;
    default: break;
    }
    /* ── ★ THE RIGHT DEFAULT PROCEDURE, WHICH IS WHAT MAKES MDI WORK ──────────
         Win32 has three, and which one a window needs is a property of where it
         sits in the tree, not of anything the guest tells us: an MDI FRAME must
         pass its client to DefFrameProc (that is how Alt+F4, the child system
         menu and the window list get handled), an MDI CHILD needs
         DefMDIChildProc, and everything else DefWindowProc. Getting this wrong is
         not cosmetic -- an MDI frame on DefWindowProc loses its children's
         non-client behaviour entirely. */
    if (h16) {
        wowuser_win_t *w = wowuser_findwin(h16);
        if (w) {
            HWND cli = wowuser_mdiclient_of(w);
            if (cli) return DefFrameProcA(h, cli, msg, wp, lp);
            if (wowuser_is_mdichild(w)) return DefMDIChildProcA(h, msg, wp, lp);
        }
    }
    return DefWindowProcA(h, msg, wp, lp);
}

/*
 * ── ★★ PUMP THE EXEC THREAD'S Win32 QUEUE. ──────────────────────────────────
 * The windows belong to this thread, so nothing about them happens -- no paint,
 * no move, no click, no title bar -- unless this thread dispatches. Called at
 * every WOW32 BOP, which is the only regular moment the exec thread is not
 * inside the guest.
 * ⚠ BOUNDED. A pump that drained without limit would let a flood of mouse moves
 *   starve the guest, and the guest is the thing we are here to run.
 */
static int wowwin_pump(int budget)
{
    MSG m;
    int n = 0;
    if (!g_ww_created) return 0;
    while (n < budget && PeekMessageA(&m, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&m);
        DispatchMessageA(&m);
        ++n;
    }
    return n;
}

/* Register a real Win32 class for a Win16 one. Returns 1 if the class is usable.
   ⚠ cbWndExtra is ZERO on purpose: the guest's window words are kept in
     wowuser_win_t (session 40), bounded by the class's own declaration, and
     giving Win32 a second copy would create two answers to one question. */
static int wowwin_register(const char *name16, char *out32, int cap)
{
    WNDCLASSA wc;
    int i = 0, k;
    for (k = 0; WOWWIN_CLASS_PREFIX[k] && i < cap - 1; ++k) out32[i++] = WOWWIN_CLASS_PREFIX[k];
    for (k = 0; name16[k] && i < cap - 1; ++k) out32[i++] = name16[k];
    out32[i] = 0;
    ZeroMemory(&wc, sizeof wc);
    wc.lpfnWndProc   = wowwin_proc;
    wc.hInstance     = GetModuleHandleA(NULL);
    wc.hCursor       = LoadCursorA(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = out32;
    if (RegisterClassA(&wc)) return 1;
    /* Already registered is success: a program may register a class name twice
       across two instances, and Win32 says so with ERROR_CLASS_ALREADY_EXISTS. */
    return GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

/* Win16 CW_USEDEFAULT -> Win32's. Different values; see the header note. */
static int wowwin_coord(WORD v)
{
    return (v == CW_USEDEFAULT16) ? CW_USEDEFAULT32 : (int)(short)v;
}

#endif /* WOWWIN_H */
