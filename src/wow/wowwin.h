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
/* #160: menus held back until the guest set them up, and the replay's re-entry flag. */
static DWORD g_ww_menudefer = 0;
static int   g_ww_replaying = 0;
/* Times Alt/F10 had to take the mouse capture off a guest window so the menu
   could open. Non-zero is normal for a paint program; zero on a session where
   the menu is dead means the cause is something else. */
static DWORD g_ww_menu_uncapture = 0;
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
static HWND  wowuser_hwnd32(WORD hwnd);
static WORD  wowuser_menu16(HMENU m);   /* the 16-bit name for a real menu */
/* #294: COMMDLG's modeless Find/Replace dialogs -- wowcommdlg.h, included later. */
static int   wowcdlg_relay(UINT msg, LPARAM lp);
static int   wowcdlg_isdlgmsg(MSG *m);
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
/* Tick at which the most recent WM_PAINT was posted to the Win16 queue. */
static DWORD g_ww_paint_ms = 0;

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

/* ── ★★★★ MSG.pt IS THE CURSOR IN *SCREEN* COORDINATES, AND IT WAS ALWAYS 0,0.
     Every wowmsg_post here passed `0, 0` for it, because nothing this host had
     watched read the field -- wowmsg.h says exactly that, and says it was filled
     with the cursor position, which it was not.
   ★ MINESWEEPER'S SMILEY IS THE PROGRAM THAT READS IT. Pressing the face does
     SetCapture and then converts the button's rectangle to SCREEN coordinates
     (two ClientToScreen calls, 0x385..0x39d = 901..925 on this display). Its
     tracking loop then hit-tests the release against that rectangle using the
     message's OWN `pt` -- and (0,0) is outside it, so the game concluded the
     button was released off the face and correctly declined to start a new one.
     The release WAS delivered and the code WAS reached; the message was just
     carrying a position no cursor has ever been at.
   ⚠ SCREEN, NOT CLIENT. lParam already carries the client point; `pt` is the
     other one, and filling it from lParam would be a plausible-looking value
     that fails the same test. */
/* ── #162: A MESSAGE THE GUEST JUST TOOK MUST NOT BE HANDED BACK TO IT. ─────────────
     USER's IsDialogMessage (wowuser.h) lets the REAL dialog manager see the guest's
     message, and that manager DISPATCHES what it does not use -- to the real window,
     i.e. to this procedure, which relays it to the guest queue. For the very message
     the guest had just taken out of that queue, that is a loop: Charmap's main window
     is a dialog, and its WM_PAINT/WM_SETFOCUS pair went round ~4,000 times a
     millisecond, as did the X button's WM_CLOSE (~99,000 in ten seconds), and the
     program never saw any of them. While IsDialogMessage runs, the SAME message for the
     SAME window is recorded as bounced and not relayed; IsDialogMessage then answers
     FALSE, so the guest's loop dispatches it to its own 16-bit procedure, which is where
     it belongs. What the dialog manager GENERATES (Enter -> WM_COMMAND for the default
     button, focus moving to a real control) still flows as before. */
static int  g_ww_isdlg, g_ww_isdlg_bounced;
static HWND g_ww_isdlg_hwnd;
static UINT g_ww_isdlg_msg;

/* ── s89 (#162): MESSAGES WINDOWS SENDS AND NEEDS AN ANSWER TO, NOW. ─────────────
     WM_CTLCOLOR* arrives from inside a control's paint and wants a brush back
     before the control can draw. Answering it means running the 16-bit window
     procedure SYNCHRONOUSLY -- the nested run (main.c, wow_call16_sync). main.c
     wires this hook; NULL (or a refusal) leaves Windows' own default. */
static LRESULT (*g_ww_ctlcolor)(HWND h, WORD h16, UINT msg, WPARAM wp, LPARAM lp,
                                int *handled);
/* s89 (#300): any message SENT to a guest window now, through the same nested run
   (main.c: wow_send16_now). 0 = it could not run; the caller then posts. */
static int (*g_ww_send16)(WORD h16, WORD msg, WORD wp, DWORD lp, WORD *res);
/* s89 (#302): owner-draw (WM_DRAWITEM/MEASUREITEM/DELETEITEM/COMPAREITEM), the
   structures converted and the program asked through the nested run (main.c). */
static LRESULT (*g_ww_ownerdraw)(HWND h, WORD h16, UINT msg, WPARAM wp, LPARAM lp,
                                 int *handled);

static unsigned g_ww_mmlog;   /* s90: first MM notifications logged */
/* s92 (#305 M12): krnl386's OWN global heap, through the nested run (main.c:
   shim_global16 -- 0 GlobalAlloc(flags, cb) 1 GlobalFree 2 GlobalLock -> 16:16
   3 GlobalUnlock). NULL until main.c wires it. */
static DWORD (*g_ww_global16)(int op, DWORD a, DWORD b);
static DWORD dpmi_sel_base(WORD sel);            /* main.c: a selector's linear base */

/* ── s92 (#305 M12): WM_DROPFILES -- A WIN16 HDROP IS A REAL GLOBAL BLOCK. ─────────
     DragQueryPoint (SHELL ord 13) and DragFinish (ord 12) never reach us -- they
     run in 16-bit SHELL.DLL against the handle as a global block (point, then
     fNC; DragFinish frees it). Only DragQueryFile (ord 11) thunks to us. So the
     drop is copied, whole, into a block krnl386 allocates, laid out as Windows
     3.1's DROPFILESTRUCT:
         +0 WORD pFiles (= 8)   +2 POINT pt (client)   +6 WORD fNC   +8 the names,
         each NUL-terminated, the list ended by an empty one.
     The names are the SHORT (8.3) forms -- a Win16 program opens what it is given
     through DOS. The Win32 HDROP is finished here; the guest's DragFinish frees ours.
     Returns the 16-bit handle, or 0 (nothing posted). */
static WORD wowwin_drop16(HDROP hd, char *why, int whycap)
{
    BYTE buf[2048];
    UINT n, i, at = 8;
    POINT pt;
    BOOL inside;
    WORD h;
    DWORD fp;
    volatile BYTE *d;
    if (!g_ww_global16) { lstrcpynA(why, "no 16-bit heap entry", whycap); return 0; }
    n = DragQueryFileA(hd, 0xFFFFFFFFu, NULL, 0);
    inside = DragQueryPoint(hd, &pt);
    for (i = 0; i < n; ++i) {
        char lng[MAX_PATH], sht[MAX_PATH];
        int  len;
        if (!DragQueryFileA(hd, i, lng, sizeof lng)) continue;
        if (!GetShortPathNameA(lng, sht, sizeof sht)) lstrcpynA(sht, lng, sizeof sht);
        len = lstrlenA(sht);
        if (at + (UINT)len + 2 > sizeof buf) break;       /* a 2 KB list; the rest dropped */
        CopyMemory(buf + at, sht, (SIZE_T)len + 1);
        at += (UINT)len + 1;
    }
    buf[at++] = 0;
    buf[0] = 8; buf[1] = 0;
    buf[2] = (BYTE)pt.x; buf[3] = (BYTE)((WORD)pt.x >> 8);
    buf[4] = (BYTE)pt.y; buf[5] = (BYTE)((WORD)pt.y >> 8);
    buf[6] = (BYTE)(inside ? 0 : 1); buf[7] = 0;
    h = (WORD)g_ww_global16(0, 0x2042 /* GMEM_SHARE|GMEM_MOVEABLE|GMEM_ZEROINIT */, at);
    if (!h) { lstrcpynA(why, "GlobalAlloc refused", whycap); return 0; }
    fp = g_ww_global16(2, h, 0);
    d = (fp >> 16) ? (volatile BYTE *)(ULONG_PTR)(dpmi_sel_base((WORD)(fp >> 16)) + (fp & 0xFFFF)) : NULL;
    if (!d || !(fp >> 16) || !dpmi_sel_base((WORD)(fp >> 16))) {
        g_ww_global16(1, h, 0);
        lstrcpynA(why, "GlobalLock refused", whycap);
        return 0;
    }
    for (i = 0; i < at; ++i) d[i] = buf[i];
    g_ww_global16(3, h, 0);
    wsprintfA(why, "%u file(s), %u bytes, pt=(%d,%d)%s", n, at, (int)pt.x, (int)pt.y,
              inside ? "" : " non-client");
    return h;
}
/* s91 (#305 M9): a message with a STRUCTURE, sent now (main.c: wow_send16_blob) --
   WM_GETMINMAXINFO's 16-bit MINMAXINFO, copied back. 0 = it could not run. */
static int (*g_ww_send16b)(WORD h16, WORD msg, WORD wp, BYTE *blob, int n,
                           const int *fix, int nfix, WORD *res);
/* Send if the nested run can, else post -- the M9 messages' delivery. */
/* ⚠ NOT RE-ENTRANTLY (s91, the final regression run): maximizing an MDI child SENT
     WM_SIZE / WM_GETMINMAXINFO into its procedure, which chains through DefMDIChildProc
     to the real MDI client, which re-sizes the child, which came back here for the
     same window and message -- w_mdi stopped dead at WM_MDIMAXIMIZE. A message already
     being sent to a window is POSTED instead, as before s91. */
static struct { WORD h16, msg; } g_ww_sending[8];
static int g_ww_nsending;
/* s92 (#289): inside USER32's move/size loop (WM_ENTERSIZEMOVE..WM_EXITSIZEMOVE).
   The guest cannot run there, so a POSTED WM_PAINT waits for the mouse to come up
   and the vacated areas are never erased -- Packager's panes left a trail of
   scrollbars across the window. Win16's size loop dispatches WM_PAINT as it goes,
   so inside it the relay SENDS the paint (see WM_PAINT below). */
static int g_ww_sizemove;
static unsigned g_ww_paintlog;
/* ── s93: A WM_CHAR EXISTS ONLY IF THE PROGRAM ASKS FOR IT. On Win16 the character
     comes from TranslateMessage, which the program calls -- or does not, for a key it
     handles itself. Win32 had already translated every key on this thread, and the
     relay posted that WM_CHAR unconditionally: WRITE handles Backspace in WM_KEYDOWN
     and never translates it, so it ALSO received a WM_CHAR 08h and inserted it -- the
     user's "Backspace enters a square". So a WM_CHAR is held here and released by the
     guest's own TranslateMessage on the matching key-down (same window, same scan
     code), into the queue where Win16's TranslateMessage would post it. 16 held,
     oldest overwritten: a program that never translates loses nothing it would have
     had on Win16. */
#define WOWWIN_PCHAR 16
static struct { WORD h16, ch; DWORD lp; DWORD seq; } g_ww_pchar[WOWWIN_PCHAR];
static DWORD g_ww_pchar_seq;
static void wowwin_hold_char(WORD h16, WORD ch, DWORD lp)
{
    int i, o = 0;
    for (i = 0; i < WOWWIN_PCHAR; ++i) {
        if (!g_ww_pchar[i].seq) { o = i; break; }
        if (g_ww_pchar[i].seq < g_ww_pchar[o].seq) o = i;
    }
    g_ww_pchar[o].h16 = h16; g_ww_pchar[o].ch = ch; g_ww_pchar[o].lp = lp;
    g_ww_pchar[o].seq = ++g_ww_pchar_seq;
}
/* Post, oldest first, every held character for this key-down; returns how many. */
static int wowwin_release_chars(WORD h16, DWORD keylp)
{
    int n = 0;
    for (;;) {
        int i, best = -1;
        for (i = 0; i < WOWWIN_PCHAR; ++i)
            if (g_ww_pchar[i].seq && g_ww_pchar[i].h16 == h16
                && ((g_ww_pchar[i].lp >> 16) & 0xFF) == ((keylp >> 16) & 0xFF)
                && (best < 0 || g_ww_pchar[i].seq < g_ww_pchar[best].seq)) best = i;
        if (best < 0) return n;
        wowmsg_post(h16, 0x0102, g_ww_pchar[best].ch, g_ww_pchar[best].lp,
                    GetTickCount(), 0, 0);
        g_ww_pchar[best].seq = 0;
        ++n;
    }
}

/* ── s93: TIMERS WITH NO WINDOW. Win16's SetTimer(NULL, 0, ms, proc) is legal and
     common in a program that has no window of its own to time with -- RECORDER, while
     recording, times itself this way, and was answered 0 ("refused"). Win32 has the
     same thing: a THREAD timer, whose WM_TIMER arrives with hwnd NULL. The pumps turn
     that into a Win16 WM_TIMER with hwnd 0, the timer's id in wParam and the TIMERPROC
     in lParam -- exactly what Win16 queues -- and DispatchMessage calls the proc. */
#define WOWWIN_TT 16
static struct { UINT_PTR id32; DWORD proc; } g_ww_tt[WOWWIN_TT];
static int wowwin_tt_add(UINT_PTR id32, DWORD proc)
{
    int i;
    for (i = 0; i < WOWWIN_TT; ++i)
        if (!g_ww_tt[i].id32 || g_ww_tt[i].id32 == id32) {
            g_ww_tt[i].id32 = id32; g_ww_tt[i].proc = proc; return 1;
        }
    return 0;
}
static int wowwin_tt_kill(UINT_PTR id32)
{
    int i;
    for (i = 0; i < WOWWIN_TT; ++i)
        if (g_ww_tt[i].id32 == id32) { g_ww_tt[i].id32 = 0; g_ww_tt[i].proc = 0; return 1; }
    return 0;
}
/* A Win32 thread WM_TIMER: 1 if it was one of ours (and is now queued for the guest). */
static int wowwin_tt_fire(const MSG *m)
{
    int i;
    if (m->message != WM_TIMER || m->hwnd) return 0;
    for (i = 0; i < WOWWIN_TT; ++i)
        if (g_ww_tt[i].id32 && g_ww_tt[i].id32 == m->wParam) {
            /* one pending per timer, as Windows coalesces them */
            wowmsg_post_move(0, 0x0113, (WORD)m->wParam, g_ww_tt[i].proc, m->time, 0, 0)
                || wowmsg_post(0, 0x0113, (WORD)m->wParam, g_ww_tt[i].proc, m->time, 0, 0);
            return 1;
        }
    return 0;
}

static int wowuser_is_dialog16(WORD h16);        /* wowuser.h: a dialog procedure? */

/* s93: the guest's SetFocus calls, counted, and the real window of the last one --
   so WM_ACTIVATE can tell that the program placed the focus itself (wowuser.h). */
static unsigned g_ww_setfocus_n;
static HWND     g_ww_setfocus_h32;

static void wowwin_send_or_post(WORD h16, WORD msg, WORD wp, DWORD lp, WORD ptx, WORD pty)
{
    WORD r;
    int i, busy = 0;
    /* s92 (#284): ONE nested WM_SIZE is Windows' own order. Cardfile's card hides its
       scrollbars inside its WM_SIZE; the client grows by the scrollbar's width and
       Windows sends WM_SIZE again, nested. Posted, it arrived after the first paint and
       the card was laid out twice -- the first card's header stayed on screen. A
       second level is allowed; deeper is the MDI loop above, and is still posted. */
    int limit = (msg == WM_SIZE) ? 2 : 1;
    for (i = 0; i < g_ww_nsending; ++i)
        if (g_ww_sending[i].h16 == h16 && g_ww_sending[i].msg == msg) ++busy;
    if (busy < limit && g_ww_send16 && g_ww_nsending < 8) {
        int ok;
        g_ww_sending[g_ww_nsending].h16 = h16; g_ww_sending[g_ww_nsending].msg = msg;
        ++g_ww_nsending;
        ok = g_ww_send16(h16, msg, wp, lp, &r);
        --g_ww_nsending;
        if (ok) return;
    }
    wowmsg_post(h16, msg, wp, lp, GetTickCount(), ptx, pty);
}
/* The default procedure this window needs -- frame, MDI child, or plain; see the
   note at the end of wowwin_proc. */
static LRESULT wowwin_defproc(HWND h, WORD h16, UINT msg, WPARAM wp, LPARAM lp)
{
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

static LRESULT CALLBACK wowwin_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    WORD h16 = wowwin_hwnd16(h);
    POINT wwpt;
    WORD  ptx, pty;
    if (g_ww_isdlg && h == g_ww_isdlg_hwnd && msg == g_ww_isdlg_msg) {
        g_ww_isdlg_bounced = 1;
        return 0;
    }
    GetCursorPos(&wwpt);
    ptx = (WORD)(short)wwpt.x;
    pty = (WORD)(short)wwpt.y;
    switch (msg) {
    case WM_CHAR:
        if (h16) { wowwin_hold_char(h16, (WORD)wp, (DWORD)lp); return 0; }
        break;
    case WM_KEYDOWN: case WM_KEYUP:
        /* ★ RELAYED VERBATIM. Win16 and Win32 agree on the message number, on
             wParam being the virtual key, and on the lParam bit field -- Win32
             inherited all three -- so the honest thing is to hand across exactly
             what the OS handed us rather than compose anything. */
        if (h16) {
            wowmsg_post(h16, (WORD)msg, (WORD)wp, (DWORD)lp, GetTickCount(), ptx, pty);
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
        /* ── ★★★ AND ALT MUST TAKE THE MOUSE CAPTURE, OR THE MENU NEVER OPENS.
             (session 53, the "Alt stops working after a few canvas drags" defect)
             MS Paint takes the capture on button-down and DOES NOT GIVE IT BACK --
             measured, nine SetCapture and zero ReleaseCapture in one session, and
             it re-takes it after every button-up. That is legal Win16: capture was
             the app's to hold. But USER32 REFUSES SC_KEYMENU WHILE THE CALLING
             THREAD HOLDS A CAPTURE -- menus deliberately do not open mid-drag --
             so DefWindowProc did nothing with Alt and the whole menu bar went dead
             for the rest of the session.
           ★ STOCK ntvdm WAS MEASURED DOING EXACTLY THIS. Same program, same three
             drags, cross-process GetGUIThreadInfo (`rigshot capture`):
               after the drags   stock hwndCapture = the CANVAS   (same as ours)
               after Alt         stock hwndCapture = the TOP-LEVEL, menuowner set,
                                 GUI_INMENUMODE   (ours: unchanged, no menu)
             So stock ALSO leaves it held, and Alt moves it. This is not us being
             unfaithful by releasing it -- it is us reproducing what the reference
             implementation is observed to do.
           ⚠ WHY STOCK GETS IT FREE AND WE DO NOT: its thread reports
             GUI_16BITTASK (flags 0x20) and ours reports 0. USER32 knows stock's
             thread is a WOW task and gives it WOW-specific menu handling; that
             flag is internal to USER32's WOW support and there is no supported way
             for us to set it. Reproducing the BEHAVIOUR is the available route.
           ⚠ ONLY ON ALT/F10 GOING DOWN, and only when one of OUR windows holds
             it. Releasing on the way up would fight the menu loop for the capture
             it has just taken, and releasing a capture that belongs to some other
             application would be reaching outside this VDM entirely. */
        if (msg == WM_SYSKEYDOWN && (wp == VK_MENU || wp == VK_F10)) {
            HWND cap = GetCapture();
            if (cap && wowwin_hwnd16(cap)) {
                ReleaseCapture();
                ++g_ww_menu_uncapture;
            }
        }
        if (h16) {
            wowmsg_post(h16, (WORD)msg, (WORD)wp, (DWORD)lp, GetTickCount(), ptx, pty);
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
         and handed back when the guest asks -- see the paint record below. The
         guest's BeginPaint DC is clipped to that rectangle (#287, wowuser.h), as
         a Win16 paint DC is; only a true non-rectangular update region is lost. */
    /* ── s89 (#162, Clock): THE GUEST ERASES FIRST. In Win16 WM_ERASEBKGND goes to
         the window's procedure, and the class brush is only DefWindowProc's
         answer for a procedure that passes it on. Letting the OS erase here
         painted Clock WHITE (its class brush) before Clock -- which fills the
         button face itself in that handler -- got a turn, and told BeginPaint the
         background was done. Returning 0 leaves fErase set: the guest's
         BeginPaint sends it WM_ERASEBKGND (wowuser.h), and DefWindowProc16
         forwards it to us with its DC (0x6b), where the class brush is applied. */
    case WM_ERASEBKGND:
        if (h16) return 0;
        break;
    case WM_CTLCOLORMSGBOX: case WM_CTLCOLOREDIT: case WM_CTLCOLORLISTBOX:
    case WM_CTLCOLORBTN:    case WM_CTLCOLORDLG:  case WM_CTLCOLORSCROLLBAR:
    case WM_CTLCOLORSTATIC:
        if (h16 && g_ww_ctlcolor) {
            int handled = 0;
            LRESULT r = g_ww_ctlcolor(h, h16, msg, wp, lp, &handled);
            if (handled) return r;
        }
        break;
    case WM_PAINT:
        if (h16) {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(h, &ps);
            if (dc) {
                wowwin_paint_want(h16, &ps.rcPaint, ps.fErase);
                /* ★ WHEN the paint was handed to the guest, so BeginPaint can say
                     how long it sat there. Throughput was measured and is fine
                     (~260 GDI calls, ~10 ms, for a full Solitaire repaint); if a
                     redraw still LOOKS slow the cost is the wait, not the work,
                     and this is the only number that separates them. */
                g_ww_paint_ms = GetTickCount();
                EndPaint(h, &ps);
            }
            if (g_ww_paintlog < 600) {     /* s92: what the OS reported, and how it went */
                char pb[160], *pq = pb;
                ++g_ww_paintlog;
                pq = zput(pq, "WOWWIN: WM_PAINT h16=0x"); pq = zhex(pq, h16);
                pq = zput(pq, " rc="); pq = zhex(pq, (DWORD)ps.rcPaint.left);
                pq = zput(pq, ","); pq = zhex(pq, (DWORD)ps.rcPaint.top);
                pq = zput(pq, ","); pq = zhex(pq, (DWORD)ps.rcPaint.right);
                pq = zput(pq, ","); pq = zhex(pq, (DWORD)ps.rcPaint.bottom);
                pq = zput(pq, ps.fErase ? " erase" : " noerase");
                pq = zput(pq, g_ww_sizemove ? " SENT\r\n" : " posted\r\n");
                log_append(LOG_PATH, pb, pq);
            }
            if (g_ww_sizemove) wowwin_send_or_post(h16, WM_PAINT16, 0, 0, ptx, pty);
            else               wowmsg_post(h16, WM_PAINT16, 0, 0, GetTickCount(), ptx, pty);
            ++g_ww_msgs;
            return 0;
        }
        break;
    case WM_ENTERSIZEMOVE: ++g_ww_sizemove; break;
    /* ...and when the loop ends, the whole window is repainted once. A child the guest
       moved from inside its WM_SIZE (Packager's "View:" label) left the strip it
       vacated on screen: its erase ran (measured: the strip visible in the DC, the
       class brush applied) and USER32's own move/size machinery still put the old
       pixels back afterwards (runs/s92/drv). One full repaint at the end is what
       makes the final picture right whatever happened mid-drag. */
    case WM_EXITSIZEMOVE:
        if (g_ww_sizemove > 0) --g_ww_sizemove;
        if (h16) RedrawWindow(h, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
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
    /* ── s90 (#278): THE MULTIMEDIA NOTIFICATIONS. MM_MCINOTIFY (0x3B9), MM_WOM_*
         (0x3BB-0x3BD), MM_WIM_* (0x3BE-0x3C0), MM_MIM_ and MM_MOM_ (0x3C1-0x3C9), the
         joystick ones (0x3A0-0x3B8). A Win16 program that opens a device with
         CALLBACK_WINDOW gets them POSTED BY WINMM to its real window: winmm's WOW
         layer maps the 16-bit HWND with WOWHandle32 (bin\wowshim\WOW32.DLL) and
         posts the 16-bit device handle and 16:16 header itself, so they are relayed
         VERBATIM. Before this they fell to DefWindowProc and Sound Recorder never
         learned that a buffer had finished playing. */
    case 0x3A0: case 0x3A1: case 0x3A2: case 0x3A3: case 0x3A4: case 0x3A5:
    case 0x3A6: case 0x3A7: case 0x3B5: case 0x3B6: case 0x3B7: case 0x3B8:
    case 0x3B9: case 0x3BA: case 0x3BB: case 0x3BC: case 0x3BD: case 0x3BE:
    case 0x3BF: case 0x3C0: case 0x3C1: case 0x3C2: case 0x3C3: case 0x3C4:
    case 0x3C5: case 0x3C6: case 0x3C7: case 0x3C8: case 0x3C9:
        if (h16) {
            if (g_ww_mmlog < 12) {
                char b[160], *q = b;
                ++g_ww_mmlog;
                q = zput(q, "WOWWIN: MM notification 0x"); q = zhex(q, msg);
                q = zput(q, " -> hwnd16 0x"); q = zhex(q, h16);
                q = zput(q, " wp=0x"); q = zhex(q, (DWORD)wp);
                q = zput(q, " lp=0x"); q = zhex(q, (DWORD)lp); q = zput(q, "\r\n");
                log_append(LOG_PATH, b, q);
            }
            wowmsg_post(h16, (WORD)msg, (WORD)wp, (DWORD)lp, GetTickCount(), ptx, pty);
            ++g_ww_msgs;
            return 0;
        }
        break;
    case WM_MOUSEMOVE:
        if (h16) {
            if (!wowmsg_post_move(h16, (WORD)msg, (WORD)wp, (DWORD)lp,
                                  GetTickCount(), ptx, pty))
                wowmsg_post(h16, (WORD)msg, (WORD)wp, (DWORD)lp,
                            GetTickCount(), ptx, pty);
            ++g_ww_msgs;
            return 0;
        }
        break;
    case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK:
    case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_RBUTTONDBLCLK:
    case WM_MBUTTONDOWN: case WM_MBUTTONUP: case WM_MBUTTONDBLCLK:
        if (h16) {
            wowmsg_post(h16, (WORD)msg, (WORD)wp, (DWORD)lp, GetTickCount(), ptx, pty);
            ++g_ww_msgs;
            return 0;
        }
        break;
    case WM_CLOSE:
        /* ⚠ LOGGED BOTH WAYS. (s73) User-reported: the X does not close Charmap or
             WinMine. Measured headlessly -- the guest's loop is alive and dispatching
             WM_TIMER, but msg 0x0010 never reaches it, and NOTHING in the log said
             whether this case ran at all or whether h16 resolved. An absence in the
             report means nothing; say which branch was taken. */
        {   char cb[160], *cq = cb;
            cq = zput(cq, "WOWWIN: WM_CLOSE on hwnd=0x"); cq = zhex(cq, (DWORD)(ULONG_PTR)h);
            cq = zput(cq, " -> h16=0x"); cq = zhex(cq, h16);
            cq = zput(cq, h16 ? " -- posted to the guest\r\n"
                              : " -- NO Win16 window for it, falling through to DefWindowProc\r\n");
            log_append(LOG_PATH, cb, cq); }
        if (h16) { wowmsg_post(h16, (WORD)msg, 0, 0, GetTickCount(), ptx, pty);
                   ++g_ww_msgs; return 0; }
        break;
    /* s92 (#305 M12): a drop on a window that called DragAcceptFiles. POSTED, as the
       shell posts it, with a Win16 HDROP built by wowwin_drop16 (see there). */
    case WM_DROPFILES:
        if (h16) {
            char why[96], db[200], *dq = db;
            WORD hd = wowwin_drop16((HDROP)wp, why, sizeof why);
            DragFinish((HDROP)wp);
            dq = zput(dq, "WOWWIN: WM_DROPFILES on h16=0x"); dq = zhex(dq, h16);
            dq = zput(dq, hd ? " -> HDROP16 0x" : " -- ★ NOT DELIVERED: ");
            if (hd) { dq = zhex(dq, hd); dq = zput(dq, " "); }
            dq = zput(dq, why); dq = zput(dq, "\r\n");
            log_append(LOG_PATH, db, dq);
            if (hd) { wowmsg_post(h16, 0x0233, hd, 0, GetTickCount(), ptx, pty); ++g_ww_msgs; }
            return 0;
        }
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
                        GetTickCount(), ptx, pty);
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
    /* ⚠ #303: for the two FOCUS messages wParam is the OTHER window -- the one
         losing focus to this one, or gaining it from it -- and it is a real HWND.
         `(WORD)wp` handed the guest the low word of a Win32 handle, which names
         some unrelated Win16 window or none. Mapped through the table; a window
         that is not a guest window is 0, as Win16 reports one from another task.
         WM_SIZE's wParam is a SIZE_* code and passes as it is. */
    /* ── #305 M9 (s91): ACTIVATION, MOVEMENT, VISIBILITY, MENU SELECTION AND THE
         SIZE LIMITS reach the guest's procedure, each in the Win16 packing:
           WM_MOVE (0003) / WM_SHOWWINDOW (0018)  same parameters in both
           WM_ACTIVATE (0006)   Win32 wParam=MAKELONG(state, fMinimized), lParam=hwnd
                                Win16 wParam=state, lParam=MAKELONG(hwnd, fMinimized)
           WM_ACTIVATEAPP (001C) Win32 lParam = a thread id; Win16 = an hTask --
                                0 here, "another task", which is what it always is
           WM_MENUSELECT (011F) Win32 wParam=MAKELONG(item, flags), lParam=hMenu
                                Win16 wParam=item, lParam=MAKELONG(flags, hMenu) --
                                hMenu 0: the guest's menus are real Win32 ones here
           WM_GETMINMAXINFO (0024) the 16-bit MINMAXINFO, 5 POINTs of INT16s,
                                sent with the structure and COPIED BACK, so a
                                program's minimum size holds
         SENT through the nested run where it can run (the order Windows gives),
         posted otherwise; DefWindowProc runs afterwards as before. */
    case WM_MOVE: case WM_SHOWWINDOW:
        if (h16) wowwin_send_or_post(h16, (WORD)msg, (WORD)wp, (DWORD)lp, ptx, pty);
        break;
    case WM_ACTIVATE:
        if (h16) {
            unsigned n0 = g_ww_setfocus_n;
            wowwin_send_or_post(h16, (WORD)msg, LOWORD(wp),
                                ((DWORD)(HIWORD(wp) ? 1 : 0) << 16)
                                | wowwin_hwnd16((HWND)lp), ptx, pty);
            /* ── s93: A PROGRAM THAT PLACES THE FOCUS ITSELF KEEPS IT. Win32's
                 DefWindowProc gives an activated window the focus -- after WRITE's
                 own WM_ACTIVATE had just put it on its document window, so every
                 key went to the frame, which ignores them: the user typed into Write
                 and nothing appeared. If the program called SetFocus while handling
                 this, the default still runs (caption, z-order) and the focus goes
                 back where the program put it. */
            if (LOWORD(wp) != WA_INACTIVE && g_ww_setfocus_n != n0
                && g_ww_setfocus_h32 && g_ww_setfocus_h32 != h
                && IsChild(h, g_ww_setfocus_h32)) {
                LRESULT r = wowwin_defproc(h, h16, msg, wp, lp);
                SetFocus(g_ww_setfocus_h32);
                return r;
            }
            /* ── s93: A DIALOG KEEPS ITS CONTROL'S FOCUS ACROSS ACTIVATION, as
                 DefDlgProc does (it saves the focus on deactivation and restores it
                 on activation). Our dialogs are our own class on DefWindowProc, which
                 gives the focus to the dialog WINDOW -- so Program Manager's "Program
                 Item Properties" had its keys going to the dialog itself: the first
                 typed letters vanished and Tab only then reached a control. */
            {   if (wowuser_is_dialog16(h16)) {
                    if (LOWORD(wp) == WA_INACTIVE) {
                        HWND fo = GetFocus();
                        if (fo && IsChild(h, fo)) SetPropA(h, "NTVDMEX16.DlgFocus", (HANDLE)fo);
                    } else {
                        LRESULT r = wowwin_defproc(h, h16, msg, wp, lp);
                        HWND fo = GetFocus();
                        if (!fo || fo == h || !IsChild(h, fo)) {
                            HWND sv = (HWND)GetPropA(h, "NTVDMEX16.DlgFocus");
                            if (!sv || !IsWindow(sv) || !IsChild(h, sv))
                                sv = GetNextDlgTabItem(h, NULL, FALSE);
                            if (sv) SetFocus(sv);
                        }
                        return r;
                    }
                }
            }
        }
        break;
    case WM_NCDESTROY:
        RemovePropA(h, "NTVDMEX16.DlgFocus");          /* s93: see WM_ACTIVATE */
        break;
    case WM_ACTIVATEAPP:
        if (h16) wowwin_send_or_post(h16, (WORD)msg, (WORD)(wp ? 1 : 0), 0, ptx, pty);
        break;
    /* ── s93: WM_MDIACTIVATE, TO THE CHILD -- AND THE TWO PACKINGS DIFFER. Win32 gives
         the child (wParam = the one losing, lParam = the one gaining); Win16 gives it
         (wParam = TRUE if IT is gaining, lParam = MAKELONG(gaining, losing)). It was
         not relayed at all, so SYSEDIT -- which keeps "the active file" from this
         message and greys File > Save, Print... without one -- had Save greyed for
         good: the user's "Save is disabled". SENT, as Windows sends it. */
    case WM_MDIACTIVATE:
        if (h16) {
            WORD gain = lp ? wowwin_hwnd16((HWND)lp) : 0;
            WORD lose = wp ? wowwin_hwnd16((HWND)wp) : 0;
            wowwin_send_or_post(h16, (WORD)msg, (WORD)((HWND)lp == h ? 1 : 0),
                                ((DWORD)lose << 16) | gain, ptx, pty);
        }
        break;
    case WM_MENUSELECT:
        if (h16) wowwin_send_or_post(h16, (WORD)msg, LOWORD(wp),
                                     (DWORD)HIWORD(wp), ptx, pty);
        break;
    case WM_GETMINMAXINFO:
        if (h16 && g_ww_send16b && lp && g_ww_nsending < 8) {
            MINMAXINFO *mm = (MINMAXINFO *)lp;
            POINT *pt = &mm->ptReserved;
            BYTE b[20];
            WORD r;
            int i;
            for (i = 0; i < 5; ++i) {
                b[i * 4 + 0] = (BYTE)pt[i].x; b[i * 4 + 1] = (BYTE)(pt[i].x >> 8);
                b[i * 4 + 2] = (BYTE)pt[i].y; b[i * 4 + 3] = (BYTE)(pt[i].y >> 8);
            }
            int ok, j, busy = 0;
            for (j = 0; j < g_ww_nsending; ++j)
                if (g_ww_sending[j].h16 == h16 && g_ww_sending[j].msg == (WORD)msg) busy = 1;
            if (busy) break;
            g_ww_sending[g_ww_nsending].h16 = h16; g_ww_sending[g_ww_nsending].msg = (WORD)msg;
            ++g_ww_nsending;
            ok = g_ww_send16b(h16, (WORD)msg, 0, b, 20, NULL, 0, &r);
            --g_ww_nsending;
            if (ok) {
                for (i = 1; i < 5; ++i) {   /* ptReserved is not the guest's to set */
                    pt[i].x = (LONG)(short)(b[i * 4 + 0] | (b[i * 4 + 1] << 8));
                    pt[i].y = (LONG)(short)(b[i * 4 + 2] | (b[i * 4 + 3] << 8));
                }
                return 0;
            }
        }
        break;
    case WM_SYSCHAR:
        if (h16) { wowmsg_post(h16, (WORD)msg, (WORD)wp, (DWORD)lp, GetTickCount(), ptx, pty);
                   ++g_ww_msgs; }
        break;
    case WM_SETFOCUS: case WM_KILLFOCUS: case WM_SIZE:
        if (h16) {
            WORD wp16 = (msg == WM_SIZE) ? (WORD)wp
                                         : wowwin_hwnd16((HWND)wp);
            /* #305 M10 (s91): WM_SIZE is SENT, as Windows sends it -- a program that
               lays its children out there has done so before the MoveWindow /
               ShowWindow that caused it returns (w_msgs size.before.return = stock).
               Focus stays posted (the note above). */
            if (msg == WM_SIZE) wowwin_send_or_post(h16, (WORD)msg, wp16, (DWORD)lp, ptx, pty);
            else wowmsg_post(h16, (WORD)msg, wp16, (DWORD)lp, GetTickCount(), ptx, pty);
            ++g_ww_msgs;
            /* ── s93: A DIALOG PASSES THE FOCUS ON, as DefDlgProc does on
                 WM_SETFOCUS -- to the control that last had it, else its first tab
                 stop. A dialog window with controls never keeps the focus itself;
                 ours did (DefWindowProc), so Program Manager's Program Item
                 Properties took the first typed letters into nothing and its Tab
                 went to IsDialogMessage with no control focused. */
            if (msg == WM_SETFOCUS && wowuser_is_dialog16(h16)) {
                HWND sv = (HWND)GetPropA(h, "NTVDMEX16.DlgFocus");
                if (!sv || !IsWindow(sv) || !IsChild(h, sv))
                    sv = GetNextDlgTabItem(h, NULL, FALSE);
                if (sv && sv != h) { SetFocus(sv); return 0; }
            }
        }
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
         is -- SYSEDIT sends itself `(0x111, <id>, 0)`, as logged -- and that
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
                        GetTickCount(), ptx, pty);
            ++g_ww_msgs;
        }
        break;
    /* ── ★★★ #160: THE MENU MUST WAIT FOR THE APPLICATION TO SET IT UP. ─────────
         WM_INITMENUPOPUP is where a Win16 program greys and ungreys its items --
         Notepad enables Cut/Copy/Delete there from EM_GETSEL. But the real menu's
         modal loop runs INSIDE this thread's pump, where the guest cannot run, so
         the posted init was handled only after the menu had CLOSED: Copy was shown
         grey on every first open and its mnemonic did nothing (measured, clip16).
       ⇒ Hold the menu back one turn of the guest's loop: post WM_INITMENU and a
         WM_INITMENUPOPUP for every popup on the bar, then a marker, and open the
         menu only when the guest's GetMessage reaches the marker -- by which time
         it has handled every init (wowmsg.h, WOWMSG_MENUREPLAY; the replay itself
         is wowwin_menu_replay, run from the GetMessage service).
       ⚠ The real inits still arrive while the menu is open and are posted as
         before; handled afterwards, they set the state the menu already has. */
    case WM_SYSCOMMAND:
        if (h16 && !g_ww_replaying
            && ((wp & 0xFFF0) == SC_KEYMENU || (wp & 0xFFF0) == SC_MOUSEMENU)
            && GetMenu(h)) {
            HMENU bar = GetMenu(h);
            int i, nb = GetMenuItemCount(bar);
            DWORD t = GetTickCount();
            wowmsg_post(h16, 0x0116 /* WM_INITMENU */, wowuser_menu16(bar), 0, t, ptx, pty);
            for (i = 0; i < nb && i < 32; ++i) {
                HMENU sub = GetSubMenu(bar, i);
                if (sub) wowmsg_post(h16, 0x0117 /* WM_INITMENUPOPUP */,
                                     wowuser_menu16(sub), (DWORD)i, t, ptx, pty);
            }
            if (wowmsg_post(h16, (WORD)WOWMSG_MENUREPLAY, (WORD)wp, (DWORD)lp, t, ptx, pty)) {
                ++g_ww_menudefer;
                return 0;                       /* opened later, by the replay */
            }
        }
        break;
    /* ── #300 (M1): SCROLL BARS. Never relayed before, so a program's own scroll
         bars -- Write's page, Cardfile's list, Charmap's grid -- did nothing when
         clicked or dragged. The packing differs:
             Win32  wParam = MAKELONG(code, pos)   lParam = scroll-bar HWND (0 =
                                                            the window's own bar)
             Win16  wParam = code                  lParam = MAKELONG(pos, hwndCtl16)
       ★ SENT, through the nested run, because Windows sends them from INSIDE its
         own tracking loop: while an arrow is held or the thumb is dragged the
         program must answer each one (SetScrollPos, redraw) before the next, or
         the bar snaps back and the content moves only on release. Posted only if
         a nested call cannot run here. DefWindowProc does nothing with them. */
    case WM_DRAWITEM: case WM_MEASUREITEM: case WM_DELETEITEM: case WM_COMPAREITEM:
        if (h16 && g_ww_ownerdraw) {
            int handled = 0;
            LRESULT r = g_ww_ownerdraw(h, h16, msg, wp, lp, &handled);
            ++g_ww_msgs;
            if (handled) return r;
        }
        break;
    case WM_HSCROLL: case WM_VSCROLL:
        if (h16) {
            WORD code  = (WORD)LOWORD(wp);
            WORD pos   = (WORD)HIWORD(wp);
            WORD ctl16 = lp ? wowwin_hwnd16((HWND)(ULONG_PTR)lp) : 0;
            DWORD lp16 = (DWORD)pos | ((DWORD)ctl16 << 16);
            WORD  r16;
            ++g_ww_msgs;
            if (g_ww_send16 && g_ww_send16(h16, (WORD)msg, code, lp16, &r16)) return 0;
            wowmsg_post(h16, (WORD)msg, code, lp16, GetTickCount(), ptx, pty);
            return 0;
        }
        break;
    case WM_COMMAND:
        if (h16) {
            WORD id     = (WORD)LOWORD(wp);
            WORD notify = (WORD)HIWORD(wp);
            WORD ctl16  = lp ? wowwin_hwnd16((HWND)(ULONG_PTR)lp) : 0;
            wowmsg_post(h16, (WORD)WM_COMMAND16, id,
                        (DWORD)ctl16 | ((DWORD)notify << 16),
                        GetTickCount(), ptx, pty);
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
    /* #294: a Find/Replace dialog's notification ("commdlg_FindReplace") to its
         owner -- relayed with the guest's own FINDREPLACE pointer. */
    if (h16 && msg >= 0xC000 && wowcdlg_relay(msg, lp)) return 0;
    return wowwin_defproc(h, h16, msg, wp, lp);
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
/* Ticks spent in here, and how many times it was entered -- the other half of
   the per-BOP cost. See the note in log.h. */
static LONGLONG g_ww_qpc = 0;
static DWORD    g_ww_pumpcalls = 0;

static int wowwin_pump(int budget)
{
    MSG m;
    int n = 0;
    LARGE_INTEGER t0, t1;
    if (!g_ww_created) return 0;
    QueryPerformanceCounter(&t0);
    ++g_ww_pumpcalls;
    while (n < budget && PeekMessageA(&m, NULL, 0, 0, PM_REMOVE)) {
        ++n; ++g_ww_pumped;
        if (wowwin_tt_fire(&m)) continue;          /* s93: a windowless Win16 timer */
        if (wowcdlg_isdlgmsg(&m)) continue;        /* #294: Find dialog's Tab/Enter */
        TranslateMessage(&m);
        DispatchMessageA(&m);
    }
    QueryPerformanceCounter(&t1);
    g_ww_qpc += t1.QuadPart - t0.QuadPart;
    return n;
}

/* #160: open the menu a WM_SYSCOMMAND was held back for (see that case). Runs the
   real modal menu loop here, on this thread, exactly where the pump would have. */
static void wowwin_menu_replay(const wowmsg_t *r)
{
    HWND h = wowuser_hwnd32(r->hwnd);
    if (!h || !IsWindow(h)) return;
    g_ww_replaying = 1;
    SendMessageA(h, WM_SYSCOMMAND, (WPARAM)r->wparam, (LPARAM)r->lparam);
    g_ww_replaying = 0;
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
                           HCURSOR hcur, HICON hico, HICON hsm, int *curfell,
                           HBRUSH hbr)
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
    /* ── ★★★★ THE CLASS'S OWN BACKGROUND BRUSH, AND IT USED TO BE THROWN AWAY.
         This was hard-coded to COLOR_WINDOW+1 -- WHITE -- for every Win16 class
         ever registered, while the guest's real `hbrBackground` was read into
         `c->hbrback` and then ignored. Windows erases with this brush on every
         BeginPaint that asks for one, so any region the guest did not repaint
         itself came out WHITE.
       ★ IT LOOKED FINE FOR EIGHT SESSIONS BECAUSE ANOTHER BUG WAS HIDING IT.
         InvalidateRect had its `lpRect` offset clobbered (session 50) and so
         invalidated the WHOLE CLIENT AREA on every call -- the guest therefore
         repainted everything, every time, and painted over the white. Fixing
         InvalidateRect to honour the rectangle is what exposed this: SOLITAIRE's
         green table grew white patches wherever a card had been moved.
       ⚠ TWO BUGS, ONE SYMPTOM, AND THE SECOND ONE WAS OLDER. Recorded because
         the obvious reading -- "the InvalidateRect fix broke Solitaire" -- is
         wrong and would have led to reverting a correct fix. */
    wc.hbrBackground = hbr ? hbr : (HBRUSH)(COLOR_WINDOW + 1);
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
