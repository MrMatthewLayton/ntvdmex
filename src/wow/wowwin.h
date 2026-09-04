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
/* Win32 messages this thread has dispatched for the guest's windows. The answer to
   "is the window hung", which cannot be read off anything else. */
static DWORD g_ww_pumped = 0;

/* Forward: the window table this proc maps through. All defined in wowuser.h,
   which owns the table and is included after this file. */
typedef struct wowuser_win_s wowuser_win_t;
static WORD  wowwin_hwnd16(HWND h);
static wowuser_win_t *wowuser_findwin(WORD hwnd);
static int   wowuser_is_mdichild(const wowuser_win_t *w);
static HWND  wowuser_mdiclient_of(const wowuser_win_t *w);
static WORD  wowuser_menu16(HMENU m);   /* the 16-bit name for a real menu */
static DWORD wowuser_timer_proc(WORD hwnd, WORD id);  /* 0 if none installed */

/*
 * ── OUR WINDOW PROCEDURE FOR EVERY Win16 WINDOW ─────────────────────────────
 * Deliberately thin. The chrome -- border, caption, sizing, minimise, close --
 * is DefWindowProc's, i.e. the OS's, which is the entire reason for doing this
 * the right way round. What we do here is TRANSLATE: a real Win32 message that
 * the guest's own window procedure would expect becomes a Win16 queue entry, and
 * the guest takes it out of its own GetMessage.
 *
 * ⚠ ONLY WHAT THE GUEST CAN ACTUALLY USE. Posting every message would fill the
 *   ring and hide the ones that matter, so the set here is deliberate and the
 *   log names anything that turns out to be missing.
 * ★ THE MOUSE IS RELAYED (session 45) and the flooding hazard above is answered
 *   the way Windows answers it: WM_MOUSEMOVE COALESCES -- only the newest
 *   pending move per window is kept (wowmsg_post_move). Leaving the mouse out
 *   was why a paint program could be looked at but not used.
 * ★ WM_PAINT IS NOW TRANSLATED (session 45) -- this note used to say it was left
 *   to DefWindowProc "until GDI's id space is dispatched", and that day came.
 *   ⚠⚠ But the region is STILL validated here, and that is not optional: Win32
 *   SYNTHESISES WM_PAINT for as long as the window has an update region, so
 *   relaying it and returning 0 live-locks the host. See the case itself.
 */
/* ── ★ THE PENDING-PAINT RECORD ──────────────────────────────────────────────
     One rectangle per window, because the OS's update region is consumed in
     wowwin_proc (see WM_PAINT there) and the guest asks for it later, out of its
     own message loop. A second WM_PAINT arriving before the guest has answered
     the first UNIONS with what is already pending rather than replacing it --
     replacing would silently drop the area from the earlier one, which is the
     kind of loss that shows up as "it only redraws sometimes". */
#define WOWWIN_MAXPAINT 32
typedef struct { WORD h16; RECT r; int erase; int pending; } wowwin_paint_t;
static wowwin_paint_t g_ww_paint[WOWWIN_MAXPAINT];

static void wowwin_paint_want(WORD h16, const RECT *r, int erase)
{
    int i, free = -1;
    for (i = 0; i < WOWWIN_MAXPAINT; ++i) {
        if (g_ww_paint[i].pending && g_ww_paint[i].h16 == h16) {
            if (r->left   < g_ww_paint[i].r.left)   g_ww_paint[i].r.left   = r->left;
            if (r->top    < g_ww_paint[i].r.top)    g_ww_paint[i].r.top    = r->top;
            if (r->right  > g_ww_paint[i].r.right)  g_ww_paint[i].r.right  = r->right;
            if (r->bottom > g_ww_paint[i].r.bottom) g_ww_paint[i].r.bottom = r->bottom;
            if (erase) g_ww_paint[i].erase = 1;
            return;
        }
        if (!g_ww_paint[i].pending && free < 0) free = i;
    }
    if (free < 0) return;                  /* full: the guest still gets the
                                              message, just no rectangle */
    g_ww_paint[free].h16 = h16;
    g_ww_paint[free].r = *r;
    g_ww_paint[free].erase = erase;
    g_ww_paint[free].pending = 1;
}

/* Take the pending rectangle for a window, or 0 if there is none. */
static int wowwin_paint_take(WORD h16, RECT *out, int *erase)
{
    int i;
    for (i = 0; i < WOWWIN_MAXPAINT; ++i)
        if (g_ww_paint[i].pending && g_ww_paint[i].h16 == h16) {
            *out = g_ww_paint[i].r;
            if (erase) *erase = g_ww_paint[i].erase;
            g_ww_paint[i].pending = 0;
            return 1;
        }
    return 0;
}

static LRESULT CALLBACK wowwin_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    WORD h16 = wowwin_hwnd16(h);
    switch (msg) {
    case WM_KEYDOWN: case WM_KEYUP: case WM_CHAR:
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
    /* ── ★★ THE SYSTEM KEYS ARE THE SYSTEM'S, AND SWALLOWING THEM BROKE THE MENU.
         (session 44) These were in the case above, relayed to the guest and then
         returned as HANDLED -- so DefWindowProc never saw them. But "Sys" in
         WM_SYSKEYDOWN means exactly *"this key belongs to the system"*: it is the
         message Alt arrives in, and DefWindowProc's response to Alt is TO OPEN THE
         MENU BAR. With it swallowed, an application could have a perfect menu and
         no keyboard would ever reach it -- Alt did nothing, so Alt+H, Alt+F4 and
         F10 did nothing either. Win16's own DefWindowProc does the same job, so
         handing these to the real one is not a Win32 concession; it is the same
         behaviour, implemented by the OS we are already running on.
       ⚠ STILL POSTED TO THE GUEST AS WELL, because a Win16 program may look at
         WM_SYSKEYDOWN before passing it on, and this host cannot ask it whether it
         did. The duplication is the price of an asynchronous queue and is written
         down rather than left to be discovered: an application that ACTS on a
         system key will see the OS act too. Nothing measured does. */
    case WM_SYSKEYDOWN: case WM_SYSKEYUP:
        if (h16) {
            wowmsg_post(h16, (WORD)msg, (WORD)wp, (DWORD)lp, GetTickCount(), 0, 0);
            ++g_ww_msgs;
        }
        break;
    /* ── ★★★ WM_PAINT, WHICH THIS FILE SAID IT WOULD RELAY "THE DAY" GDI'S ID
         SPACE WAS DISPATCHED. That day is session 45: GDI is anchored, USER's
         GetDC issues real device contexts, and MS Paint's window is on screen
         and empty because nothing has ever asked it to draw.

       ⚠⚠ THE REGION IS VALIDATED HERE, AND NOT DOING SO IS A LIVE-LOCK. Win32
         does not queue WM_PAINT -- it SYNTHESISES one for as long as the window
         has an update region. Relaying it to the guest and returning 0 leaves
         that region dirty, so the real pump hands us another WM_PAINT
         immediately and the host spins at 100% delivering paints the guest never
         gets a turn to answer. So the OS's BeginPaint/EndPaint pair runs here:
         it erases the background and clears the region, which stops the storm,
         and the rectangle it reports is carried to the guest.
       ⚠ WHAT THAT COSTS, STATED RATHER THAN DISCOVERED: the guest's own
         BeginPaint can no longer inherit a real update region, because this
         already consumed it. The rectangle is therefore remembered per window
         and handed back when the guest asks -- see the paint record below. A
         guest that paints only what it is told to will paint the right area; one
         that relies on the DC being CLIPPED to that area is relying on something
         this does not yet reproduce, and that is a known gap rather than a
         surprise waiting to happen. */
    case WM_PAINT:
        if (h16) {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(h, &ps);
            if (dc) {
                wowwin_paint_want(h16, &ps.rcPaint, ps.fErase);
                EndPaint(h, &ps);
            }
            wowmsg_post(h16, WM_PAINT16, 0, 0, GetTickCount(), 0, 0);
            ++g_ww_msgs;
            return 0;
        }
        break;
    /* ── ★★★★★ THE MOUSE. WITHOUT THIS A PAINT PROGRAM CANNOT PAINT. ────────
         This procedure relayed keys, system keys, close, size, focus and paint,
         and NOTHING from the mouse -- so MS Paint could be looked at but not
         used: no stroke on the canvas, no tool picked out of the toolbox, no
         colour picked out of the palette. The header note above explains why it
         was left out ("posting every message would fill the ring with mouse
         moves the guest never asked for and would hide the ones it did"), and
         that hazard is real. The answer is not to drop the mouse, it is to
         COALESCE the moves the way Windows does -- see wowmsg_post_move.

       ★ RELAYED VERBATIM, like the keyboard. Win16 and Win32 agree on the
         message numbers (0x200..0x209), on wParam being the MK_* button/modifier
         bits (MK_LBUTTON 1, MK_RBUTTON 2, MK_SHIFT 4, MK_CONTROL 8, MK_MBUTTON
         0x10 in both), and on lParam being the x in the low word and y in the
         high, in CLIENT coordinates. Composing anything here would be inventing.
       ⚠ THESE MUST STILL REACH DefWindowProc for the non-client cases, but the
         ones handled here are all CLIENT-area messages, which DefWindowProc does
         nothing with. Returning 0 is what a window procedure that handled them
         does.
       ⚠ A DOUBLE-CLICK ONLY ARRIVES IF THE CLASS ASKED FOR IT (CS_DBLCLKS). We
         register the guest's own class style, so a guest that did not ask gets
         two ordinary clicks -- which is correct, not a gap. */
    case WM_MOUSEMOVE:
        if (h16) {
            if (!wowmsg_post_move(h16, (WORD)msg, (WORD)wp, (DWORD)lp,
                                  GetTickCount(), 0, 0))
                wowmsg_post(h16, (WORD)msg, (WORD)wp, (DWORD)lp,
                            GetTickCount(), 0, 0);
            ++g_ww_msgs;
            return 0;
        }
        break;
    case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK:
    case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_RBUTTONDBLCLK:
    case WM_MBUTTONDOWN: case WM_MBUTTONUP: case WM_MBUTTONDBLCLK:
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
    /* ── ★★ WM_TIMER. THE OS IS THE TIMER ENGINE; THIS IS THE WHOLE RELAY. ───
         The real HWND belongs to this thread, so the OS's own timer already
         delivers WM_TIMER here on schedule with the id in wParam, exactly where
         Win16 puts it.
       ★ lParam CARRIES THE 16-BIT TIMERPROC WHEN THE GUEST INSTALLED ONE, which
         is what Win16 does -- and DispatchMessage, not this relay, is what calls
         it. See the timer table in wowuser.h for why that ordering is the
         difference between a faithful implementation and re-entering the guest
         from inside a Win32 callback.
       ⚠ A guest that installed NO proc gets lParam 0 and must: it would
         otherwise receive a pointer it never supplied. */
    case WM_TIMER:
        if (h16) {
            wowmsg_post(h16, (WORD)msg, (WORD)wp,
                        wowuser_timer_proc(h16, (WORD)wp),
                        GetTickCount(), 0, 0);
            ++g_ww_msgs;
            return 0;
        }
        break;
    /* ── ★ FOCUS AND SIZE, BECAUSE THE GUEST ACTS ON THEM. ────────────────────
         A Win16 application puts the caret where it belongs by handling
         WM_SETFOCUS -- Notepad's answer to it is `SetFocus(its edit control)` --
         so a host that never delivers one leaves a window nobody can type into
         and no caret anywhere. WM_SIZE is the same shape: the guest lays its
         children out in response to it.
       ⚠ ON REAL WINDOWS THESE ARE **SENT**, NOT POSTED, and here they are posted:
         the guest sees them at its next GetMessage rather than immediately.
         Delivering them synchronously means re-entering the guest from inside a
         Win32 callback, which needs the nested run this host has not built yet.
         The ordering is therefore slightly wrong and is written down rather than
         discovered -- it is invisible for a program that only uses them to move
         focus and lay out children, which is what these two do.
       ⚠ DefWindowProc still runs afterwards, so the OS keeps its own idea of
         focus and size; the guest is being told, not put in charge. */
    /* ⚠ REFUTED, session 45: "MS Paint lays its children out too big because it
         is never told its size" -- WM_SIZE has been relayed here since session
         43, in this very case. Do not re-add it below; it is a duplicate case
         value and the compiler says so. The over-sized toolbox is something
         else. */
    case WM_SETFOCUS: case WM_KILLFOCUS: case WM_SIZE:
        if (h16) { wowmsg_post(h16, (WORD)msg, (WORD)wp, (DWORD)lp,
                               GetTickCount(), 0, 0); ++g_ww_msgs; }
        break;
    /* ── ★★★ WM_COMMAND -- THE MENU STOPS BEING DECORATION. (session 44) ──────
         The menu bar is the application's OWN resource on a real Win32 window,
         so clicking it already produces a real WM_COMMAND carrying the
         application's own item id -- `0x000b` is `&About Notepad...` out of
         NOTEPAD.EXE's `MENU 1`. Until now this procedure dropped it, so the menu
         was real and inert.

       ⚠ THE TWO PACKINGS ARE DIFFERENT, AND TRANSLATING THEM IS THE WHOLE JOB.
         Win32 puts the notification code in the HIGH half of wParam and the
         control's window handle in lParam; Win16 puts the id alone in wParam and
         packs (hwndCtl, notifyCode) into lParam. Relaying a Win32 WM_COMMAND
         unchanged gets a MENU command right by luck -- both are "id in the low
         half, everything else zero" -- and gets every CONTROL notification wrong
         in both parameters. So it is composed, not relayed.
       ★ And the control's handle has to become a WIN16 one: a guest comparing it
         against the handle its own CreateWindow returned must find them equal.
       ⚠ THE lParam FORM FOR A CONTROL IS NOT CONFIRMED BY A RUN. The menu form
         is -- `sysedit seg1:0x0477` sends itself `(0x111, <id>, 0)` -- and that
         is the form Help > About travels. The control form is written here
         because leaving it as the Win32 packing would be knowingly wrong, and
         the log prints both halves so the first guest that uses it can say.

       ⚠ FALLS THROUGH TO THE DEFAULT PROCEDURE ON PURPOSE. The guest is told
         asynchronously and has no way to answer "I did not handle that", and on
         an MDI frame DefFrameProc's own WM_COMMAND arm is what activates a child
         from the Window menu. Posting and then letting the OS have it keeps both
         -- DefWindowProc does nothing with a WM_COMMAND, so the non-MDI case
         costs nothing. */
    /* ── ★★★ WM_INITMENU / WM_INITMENUPOPUP -- WHERE A MENU GETS ITS STATE. ──
         (session 44) An application does not grey and check its menu items when
         it feels like it; it does so when the OS tells it a menu is ABOUT TO BE
         SHOWN. Notepad greys Edit > Undo, Cut, Copy, Paste and checks Word Wrap
         from here -- so with these dropped it never called GetMenu at all, and
         the whole menu-state cluster looked unused when it was simply never
         asked for. Found by driving Alt+E on the live guest and watching nothing
         happen.
       ⚠⚠ wParam IS AN HMENU AND MUST BECOME A TOKEN. A real menu handle is 32
         bits and a Win16 program has 16 to hold it in; worse, it hands that
         handle straight back to EnableMenuItem, which is 16-bit code inside
         USER, so the value has to survive a round trip and still name the right
         menu. Truncating a pointer would do neither, and would not fail loudly.
       ★ lParam is the same shape in both: the popup's index in the low half and
         "this is the system menu" in the high half. Composed rather than
         relayed, because the Win32 value is what we have and the Win16 value is
         what the guest reads. */
    case WM_INITMENU:
    case WM_INITMENUPOPUP:
        if (h16) {
            WORD hm = wowuser_menu16((HMENU)wp);
            wowmsg_post(h16, (WORD)msg, hm,
                        (msg == WM_INITMENUPOPUP)
                            ? ((DWORD)LOWORD(lp) | ((DWORD)HIWORD(lp) << 16))
                            : 0,
                        GetTickCount(), 0, 0);
            ++g_ww_msgs;
        }
        break;
    case WM_COMMAND:
        if (h16) {
            WORD id     = (WORD)LOWORD(wp);
            WORD notify = (WORD)HIWORD(wp);
            WORD ctl16  = lp ? wowwin_hwnd16((HWND)(ULONG_PTR)lp) : 0;
            wowmsg_post(h16, (WORD)WM_COMMAND16, id,
                        (DWORD)ctl16 | ((DWORD)notify << 16),
                        GetTickCount(), 0, 0);
            ++g_ww_msgs;
        }
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
        ++n; ++g_ww_pumped;
    }
    return n;
}

/* Register a real Win32 class for a Win16 one. Returns 1 if the class is usable.
   ⚠ cbWndExtra is ZERO on purpose: the guest's window words are kept in
     wowuser_win_t (session 40), bounded by the class's own declaration, and
     giving Win32 a second copy would create two answers to one question.
   ★ `curord` / `icoord` are the PREDEFINED ORDINALS the guest put in its
     WNDCLASS, or 0. This is the moment the token minted by USER id 0xad becomes a
     real object: the guest has just said which field it belongs in, so cursor and
     icon can finally be told apart -- see the note by WOWUSER_LOADSYSOBJ.
   ⚠ THE ORDINAL IS PASSED TO THE OS UNCHANGED, on the same claim as the WS_*
     bits: Win32 inherited the predefined cursor and icon ordinals from Win16. If
     that is ever wrong the OS returns NULL, so the fallback below is not belt and
     braces -- it is what turns a wrong assumption into a visible line instead of
     a window with no cursor. */
/* ⚠ THE CURSOR IS NOW BUILT BY THE CALLER TOO, like the icon. (session 47) It
     used to be an ordinal resolved here, which could only ever name one of the
     OS's predefined cursors -- and MS Paint's seven cursors are all NAMED
     resources in its own file, so a paint program's pointer never changed shape.
     One asymmetry removed: whoever knows the token resolves it, and this only
     decides what to do when there is nothing. */
static int wowwin_register(const char *name16, char *out32, int cap,
                           HCURSOR hcur, HICON hico, HICON hsm, int *curfell)
{
    /* ⚠ WNDCLASSEX, NOT WNDCLASS, AND FOR ONE REASON: `hIconSm`. A class with no
         small icon makes Windows derive one, and the derived one measured
         MONOCHROME against stock ntvdm on the taskbar while the caption was
         correct -- same window, same HICON, two renderings. See wowres.h. */
    WNDCLASSEXA wc;
    int i = 0, k;
    for (k = 0; WOWWIN_CLASS_PREFIX[k] && i < cap - 1; ++k) out32[i++] = WOWWIN_CLASS_PREFIX[k];
    for (k = 0; name16[k] && i < cap - 1; ++k) out32[i++] = name16[k];
    out32[i] = 0;
    ZeroMemory(&wc, sizeof wc);
    wc.cbSize        = sizeof wc;
    wc.lpfnWndProc   = wowwin_proc;
    wc.hInstance     = GetModuleHandleA(NULL);
    wc.hCursor       = hcur;
    if (!wc.hCursor) {
        if (curfell) *curfell = 1;
        wc.hCursor = LoadCursorA(NULL, IDC_ARROW);
    }
    wc.hIcon   = hico;        /* built by the caller: predefined, or the app's own */
    wc.hIconSm = hsm;         /* ★ and the 16x16 built at 16x16, not shrunk later */
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = out32;
    if (RegisterClassExA(&wc)) return 1;
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
