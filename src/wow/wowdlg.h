#ifndef WOWDLG_H
#define WOWDLG_H
/*
 * wowdlg.h -- ★★★★★ THE MODAL MESSAGE LOOP. GH #128, session 57.
 *
 * ── THE ONE-SENTENCE VERSION ────────────────────────────────────────────────
 * `DialogBox` does not return until `EndDialog`, and the loop that makes that
 * true is OURS -- not USER.EXE's -- so until this file existed a modal dialog
 * ENDED THE PROGRAM THAT PUT IT UP.
 *
 * ── HOW THAT WAS SETTLED, AND BY WHOM ───────────────────────────────────────
 * Session 55 left the question written down; session 56 answered it at run time:
 * CreateDialog and DialogBox both arrive here as ONE thunk (id 0xEF), and the
 * word at argument offset 0 is the MODAL FLAG -- 0 from CreateDialog, 1 from
 * DialogBox. Whatever the BOP answers, the 16-bit caller has its return value at
 * once: no 16-bit modal loop runs on the far side to fall back on. The 32-bit
 * side is expected to park the caller, run the dialog, and complete the
 * original call with EndDialog's result. That is exactly what this file does.
 *
 * ⇒ MEASURED CONSEQUENCE OF NOT DOING IT (s56, TASKMAN on the rig): the dialog
 *   and all eight of its controls are built, `STAGE2: complete` follows, and
 *   there is NO WINDOW -- because TASKMAN's WinMain is `DialogBox(...); return;`
 *   and we handed control straight back to it.
 *
 * ── THE MECHANISM, AND WHY IT NEEDED NO NEW MACHINERY ───────────────────────
 * The temptation is to plant a 16-bit message-loop stub in guest memory and let
 * the guest run it. That is a second implementation of something this host
 * already has. What it has is the EDITLOCK -> EDITFILL -> LocalUnlock chain
 * (session 44): a service asks for a 16-bit call, the host makes it, and when it
 * RETURNS the host decides -- from the frame's `action` -- what to do next,
 * including issuing another call. A modal loop is that chain with a different
 * continuation rule:
 *
 *     DialogBox BOP           park the caller; do NOT write its return value yet
 *       -> WM_INITDIALOG      call the dialog procedure    (wowcall_enter)
 *       <- it returns         ACT_MODALPUMP: is it over? no
 *       -> wait for input     pump Win32, take a Win16 message
 *       -> WM_COMMAND         call the dialog procedure
 *       <- it returns         ACT_MODALPUMP: EndDialog was seen
 *       -> UNWIND             write nResult into the DialogBox return hole and
 *                             let the guest resume past its BOP at last
 *
 * ★ THE PARKED CONTEXT IS FREE, AND THAT IS THE WHOLE TRICK. `wowcall_enter`
 *   saves the guest exactly as it will be resumed -- which, at the DialogBox
 *   BOP, is "the caller, one instruction after DialogBox". Every iteration
 *   restores that same context and re-parks it, so when the loop finally does
 *   nothing, the guest is standing precisely where DialogBox should return to.
 *   No stack grows, no EIP is written by hand, no context is invented.
 * ⚠ SO THE PER-MESSAGE CALLS PASS retlin = 0. Their return values are the
 *   dialog procedure's BOOLs and must not touch the hole; the ONLY writer of the
 *   DialogBox return value is the unwind below. Letting WOWCALL_RET_RESULT near
 *   it would fill a caller's `if (DialogBox(...) == IDOK)` with whatever the
 *   last message handler happened to answer.
 *
 * ── ⚠⚠ THE HAZARD THIS FILE IS BUILT AGAINST ────────────────────────────────
 * Session 56 declined to write this loop, and its reason is the specification
 * for the safety rules below: "a half-built modal loop that never returns is
 * worse than an honest immediate return, because it hangs the guest instead of
 * ending it". A hang is worse than an exit because an exit is legible -- the
 * program is gone and the log says why -- whereas a hang looks exactly like the
 * host having crashed, and this project has already spent a session on a Win16
 * guest that "was only running stock" when in fact a watchdog had killed it.
 * ⇒ THEREFORE THE LOOP HAS FOUR EXITS, and every one of them writes a line
 *   saying which fired:
 *     1. EndDialog          -- the real one; nResult goes to the caller
 *     2. THE WINDOW IS GONE -- IsWindow() is false, so nothing can ever drive
 *                              the dialog again; return 0, which is what real
 *                              Windows returns for a dialog that could not run
 *     3. NOTHING TO CALL    -- no dialog procedure AND no class window procedure.
 *                              A dialog nobody can be told about cannot be
 *                              dismissed, so this degrades to session 56's
 *                              behaviour DELIBERATELY rather than spinning
 *     4. THE WAIT EXPIRED   -- the same bound GetMessage already honours
 *                              (wowidle.txt; 0 = forever), so an unattended
 *                              harness run still finishes and an interactive
 *                              session still waits for the human
 * ⚠ AND THE WAIT IS THE SAME WAIT. A modal dialog sitting for four minutes while
 *   a human reads it is NOT a hang, and a timeout short enough to "catch a hang"
 *   would break the only case this feature exists for. So the bound is the knob
 *   the message loop already has, and `g_wm_inwait` is set across it for the
 *   freeze watchdog -- see the long note on that flag in wowmsg.h, which exists
 *   because the watchdog once killed an idle Win16 guest at 150 seconds.
 *
 * ── WHAT IS MEASURED HERE AND WHAT IS REASONED ──────────────────────────────
 * MEASURED, session 56: the modal flag; that TASKMAN builds its dialog and all 8
 *   controls and then exits; that clicking a control on a dialog produces a
 *   Win32 WM_COMMAND on the dialog's own HWND (CALC's buttons do exactly this
 *   and its arithmetic is correct because of it).
 * REASONED, and to be settled by the first run: that WM_INITDIALOG must be sent
 *   BEFORE the loop turns (it is where a dialog fills its own controls -- for
 *   TASKMAN, the task list itself); and that a dialog whose template names no
 *   class reaches its procedure through `dlgproc` rather than through a class
 *   window procedure. The log names both, so a run can correct either.
 */

/* WM_INITDIALOG. The documented Win16 value, the message immediately before
   WM_COMMAND (0x111); COMMDLG's dialog procedures handle the pair together
   (see wowmsg.h). */
#define WM_INITDIALOG16   0x0110

/* Four is not a guess: a modal dialog may put up another one (a File > Open
   inside an Options dialog, a message box inside a validation handler), and a
   host that nested deeper than this would be looping rather than working. The
   same reasoning, and the same number's worth of it, as WOWCALL_MAX_DEPTH. */
#define WOWDLG_MAX_MODAL  4

/* s88: per callback depth -- did the modal loop's call go to the dialog's own
   DLGPROC, and with which message. Read when the call returns (main.c). */
static int  g_wd_dlgcall[WOWCALL_MAX_DEPTH];
static WORD g_wd_dlgmsg[WOWCALL_MAX_DEPTH];

typedef struct {
    WORD  hwnd;        /* the dialog's Win16 handle -- the loop's identity      */
    DWORD retlin;      /* THE RETURN HOLE of the DialogBox call we parked       */
    DWORD dlgproc;     /* the guest's dialog procedure, or 0                    */
    DWORD wndproc;     /* its class's window procedure, or 0                    */
    WORD  ds;          /* the DS/AX both must be entered with                   */
    int   inited;      /* WM_INITDIALOG has been sent                           */
    int   toshow;      /* the template said WS_VISIBLE and we DEFERRED it       */
    int   ended;       /* EndDialog was called for this dialog                  */
    WORD  result;      /* ...and this is the nResult it passed                  */
    DWORD msgs;        /* messages dispatched into it, for the log              */
    /* ⚠ THE WIN32 TRACE BUDGET IS PER DIALOG, NOT PER WAIT, and that distinction
         is the difference between an instrument and a flood: the wait below is
         re-entered after EVERY message, so a budget living there would re-arm
         48 lines each time and a mouse crossing the dialog would write a line
         per move. TERMINAL's 158 MB log is what that looks like. */
    int   trace;
    DWORD t0;          /* when it went up -- so the log can say how long it ran */
    /* GH #279: the top-level window DialogBox disabled, re-enabled at unwind.
       NULL when there was no owner or it was ALREADY disabled -- a nested dialog
       whose owner is the outer dialog must not enable a window it did not
       disable. */
    HWND  owner32;
    DWORD initparam;   /* DialogBoxParam's lParam, for WM_INITDIALOG (s89)  */
    WORD  firstfocus;  /* the first WS_TABSTOP control, WM_INITDIALOG's wParam */
} wowdlg_modal_t;

static wowdlg_modal_t g_wd[WOWDLG_MAX_MODAL];
static int   g_wd_depth   = 0;
static DWORD g_wd_ran     = 0;   /* modal dialogs run to completion this run    */
static DWORD g_wd_refused = 0;   /* ...and ones the host could not drive        */

/* ⚠ NO FORWARD DECLARATIONS HERE, unlike wowwin.h. That file is included BEFORE
     wowuser.h and has to declare what it borrows; this one is included AFTER, so
     the window table, `wowuser_findwin` and `wowuser_winproc_of` are already
     complete -- and re-declaring the struct would be a duplicate typedef. The
     include order in main.c is what makes that true; it is commented there. */

/* Defined in main.c, which owns the LDT: is this code selector NOT PRESENT?
   See the call site, and WOWCALL_RETF_OFF in wowcall.h for what it decides. */
static int wowdlg_sel_absent(WORD sel);

/*
 * ── ★★★ THE PUMP, WITH ITS MOUTH OPEN. (session 57, second run) ─────────────
 * The heartbeat refuted the obvious explanation -- `this thread 0x334, the
 * windows' thread 0x334`, so PeekMessage is looking at the right queue -- and
 * left a sharper question: SIX Win32 messages were dispatched across a
 * foreground change and a click on a real Cancel button, and NOT ONE of them
 * turned into a Win16 message. Those are two different facts and the totals
 * cannot tell them apart:
 *     the click never reached this queue          -> an input problem
 *     it arrived and wowwin_proc did not post it  -> a translation problem
 * So the pump says what it dispatched, to which window, and whether that window
 * is one of ours. `h16 == 0` is the whole answer if it is 0.
 * ⚠ BOUNDED, and the bound is the instrument's own discipline: a modal dialog
 *   left up for a minute would otherwise write a line per mouse move. The first
 *   WOWDLG_TRACE messages of each wait are traced and the rest are counted.
 */
#define WOWDLG_TRACE 48

static int wowdlg_pump(int budget, int *trace)
{
    MSG m;
    int n = 0;
    while (n < budget && PeekMessageA(&m, NULL, 0, 0, PM_REMOVE)) {
        if (trace && *trace > 0) {
            char tb[200], *tq = tb;
            WORD h16 = wowwin_hwnd16(m.hwnd);
            --*trace;
            tq = zput(tq, "       WOWDLG/win32: msg=0x"); tq = zhex(tq, m.message);
            tq = zput(tq, " hwnd=0x");   tq = zhex(tq, (DWORD)(ULONG_PTR)m.hwnd);
            tq = zput(tq, " wp=0x");     tq = zhex(tq, (DWORD)m.wParam);
            tq = zput(tq, " lp=0x");     tq = zhex(tq, (DWORD)m.lParam);
            tq = zput(tq, " -> win16 0x"); tq = zhex(tq, h16);
            if (!h16) tq = zput(tq, " (★ NOT ONE OF OURS -- nothing will be"
                                    " posted for it)");
            tq = zput(tq, "\r\n");
            log_append(LOG_PATH, tb, tq);
        }
        if (wowwin_tt_fire(&m)) { ++n; ++g_ww_pumped; continue; }   /* s93 */
        /* #305 M11 (s91): THE DIALOG MANAGER'S KEYS. DialogBox's own loop gives a modal
             dialog Tab / Shift+Tab between its controls, Enter = the default button and
             Esc = IDCANCEL -- without the program asking. This loop dispatched keys raw,
             so Esc did nothing in TASKMAN's Task List (runs/s91/chain25). The controls are
             real windows, so the OS's IsDialogMessage does the work; what it generates
             (WM_COMMAND IDCANCEL/IDOK) reaches the dialog procedure through wowwin_proc
             like a click does. Keyboard messages only, and only for the topmost modal
             dialog or one of its children. */
        if (m.message >= WM_KEYFIRST && m.message <= WM_KEYLAST && g_wd_depth > 0) {
            HWND dlg = wowuser_hwnd32(g_wd[g_wd_depth - 1].hwnd);
            /* ── s93: A KEY FOR THE DIALOG WINDOW ITSELF belongs to a control -- a
                 dialog with controls never holds the focus (DefDlgProc passes it on).
                 Ours could end up holding it (Program Item Properties: the first
                 letters typed vanished, measured); the key and the focus go to the
                 control that last had it, else the first tab stop. */
            if (dlg && m.hwnd == dlg && m.message == WM_KEYDOWN) {
                HWND sv = (HWND)GetPropA(dlg, "NTVDMEX16.DlgFocus");
                if (!sv || !IsWindow(sv) || !IsChild(dlg, sv))
                    sv = GetNextDlgTabItem(dlg, NULL, FALSE);
                if (sv && sv != dlg) { SetFocus(sv); m.hwnd = sv; }
            }
            if (dlg && IsWindow(dlg) && (m.hwnd == dlg || IsChild(dlg, m.hwnd))
                && IsDialogMessageA(dlg, &m)) {
                ++n; ++g_ww_pumped;
                continue;
            }
        }
        TranslateMessage(&m);
        DispatchMessageA(&m);
        ++n; ++g_ww_pumped;
    }
    return n;
}

static wowdlg_modal_t *wowdlg_top(void)
{
    return g_wd_depth > 0 ? &g_wd[g_wd_depth - 1] : NULL;
}

/* Is any modal dialog up? Read by the DialogBox service, so that a nested one
   is described honestly in the trace rather than looking like the first. */
static int wowdlg_active(void)
{
    return g_wd_depth;
}

/*
 * Park a DialogBox call. `retlin` is the linear address of ITS return hole --
 * the four bytes the caller will read as DialogBox's result -- and nothing
 * writes them until this dialog ends.
 * Returns 0 if the stack is full, in which case the caller must complete the
 * call the old way rather than pretend.
 */
static int wowdlg_push(WORD hwnd, DWORD retlin, DWORD dlgproc, DWORD wndproc,
                       WORD ds, int defer_show, HWND owner32)
{
    wowdlg_modal_t *d;
    if (g_wd_depth >= WOWDLG_MAX_MODAL) return 0;
    if (!hwnd || !retlin) return 0;
    d = &g_wd[g_wd_depth++];
    d->hwnd    = hwnd;
    d->retlin  = retlin;
    d->dlgproc = dlgproc;
    d->wndproc = wndproc;
    d->ds      = ds;
    d->inited  = 0;
    d->toshow  = defer_show;
    d->ended   = 0;
    d->result  = 0;
    d->msgs    = 0;
    d->trace   = WOWDLG_TRACE;
    d->t0      = GetTickCount();
    /* ── GH #279: A MODAL DIALOG DISABLES ITS OWNER, as USER's DialogBox does.
         Without it the main window's X stayed live under Terminal's first-run
         "Default Serial Port" dialog and took WM_CLOSE. Disabling the REAL
         window is the whole fix: Windows itself then ignores clicks on it,
         its X included, so nothing reaches wowwin_proc to be posted. The
         owner is the top-level window: a dialog owned by a child control
         disables the frame around it, as USER's does. */
    d->owner32 = NULL;
    d->initparam = 0; d->firstfocus = 0;     /* the caller sets them after the push */
    if (owner32) {
        HWND top = GetAncestor(owner32, GA_ROOT);
        if (!top) top = owner32;
        if (IsWindowEnabled(top)) { EnableWindow(top, FALSE); d->owner32 = top; }
    }
    return 1;
}

/* s89: WM_INITDIALOG's wParam/lParam for the dialog just pushed. */
static void wowdlg_set_init(DWORD initparam, WORD firstfocus)
{
    if (g_wd_depth <= 0) return;
    g_wd[g_wd_depth - 1].initparam  = initparam;
    g_wd[g_wd_depth - 1].firstfocus = firstfocus;
}

/*
 * ★ EndDialog, from inside the dialog procedure we are running. This does not
 * unwind anything by itself -- it cannot: we are several frames down, inside
 * 16-bit code, and the guest has to return through our stub before its context
 * can be put back. It records the answer, and the pump notices on the way out.
 * ⚠ IT SEARCHES THE STACK rather than assuming the top. A dialog procedure may
 *   legally end an OUTER dialog (an Options page whose Cancel closes the whole
 *   sheet), and ending the wrong one would leave a loop with no way out.
 * Returns 1 if this handle names a modal dialog we are running.
 */
static int wowdlg_end(WORD hwnd, WORD result)
{
    int i;
    for (i = g_wd_depth - 1; i >= 0; --i)
        if (g_wd[i].hwnd == hwnd) {
            g_wd[i].ended  = 1;
            g_wd[i].result = result;
            return 1;
        }
    return 0;
}

/* Write the answer into the parked call's return hole and drop the frame. The
   hole is guest memory reached by linear address, exactly as wowcall.h revises
   a refused WM_CREATE, so it outlives every context switch in between. */
static void wowdlg_unwind(wowdlg_modal_t *d, DWORD value)
{
    volatile BYTE *h = (volatile BYTE *)(ULONG_PTR)d->retlin;
    h[0] = (BYTE)(value & 0xFF);         h[1] = (BYTE)((value >> 8)  & 0xFF);
    h[2] = (BYTE)((value >> 16) & 0xFF); h[3] = (BYTE)((value >> 24) & 0xFF);
    /* GH #279: give the owner back BEFORE the dialog goes, so activation returns
       to it rather than to whatever window Windows picks next. */
    if (d->owner32 && IsWindow(d->owner32)) EnableWindow(d->owner32, TRUE);
    d->owner32 = NULL;
    if (g_wd_depth > 0) --g_wd_depth;
}

/*
 * ── ★★★★★ ONE TURN OF THE MODAL LOOP ────────────────────────────────────────
 * Called twice from the BOP handler and identically from both: once when
 * DialogBox parks its caller, and once every time a dialog procedure we called
 * returns (WOWCALL_ACT_MODALPUMP).
 *
 *   returns 1  a 16-bit call is IN FLIGHT -- the guest is now inside the dialog
 *              procedure and must be resumed there
 *   returns 0  no modal dialog is left running; the guest resumes past the
 *              DialogBox BOP with its answer already in the hole
 *
 * `running` is the host's own shutdown flag, passed in rather than reached for:
 * this file is included before main.c defines it, and a wait that ignores it
 * would hold a closing process open for the length of the idle timeout.
 */
static int wowdlg_step(volatile BYTE *tib, DWORD ssbase, WORD rsel,
                       const volatile LONG *running, char *note, int cap)
{
    int k = 0;
    for (;;) {
        wowdlg_modal_t *d = wowdlg_top();
        wowuser_win_t  *w;
        wowmsg_t m;
        DWORD proc = 0;
        WORD  arg[5];
        WORD  msg, wparam;
        WORD  tgt;                      /* s93: the window the call is FOR */
        DWORD lparam;
        int   absent = 0;
        int   verdict;

        if (!d) return 0;
        w = wowuser_findwin(d->hwnd);
        tgt = d->hwnd;

        /* ── ★★★ THE FOUR EXITS ARE ONE DECISION, AND IT IS A TESTED FUNCTION.
             `WowConvModalExit` in wowconv.h is total -- every combination of
             the four facts returns something -- so there is no state in which
             this loop neither runs nor leaves. That is the property that makes
             a modal loop safe to ship, and it is pinned off-VM in wow_test.c
             rather than argued for here. `wait_expired` is 0 at this point
             because the wait has not happened yet; the branch that runs it asks
             again with 1. */
        verdict = WowConvModalExit(d->ended,
                                     w && w->hwnd32 && IsWindow(w->hwnd32),
                                     d->dlgproc || d->wndproc, 0);

        /* ── EXIT 1: THE REAL ONE. EndDialog was called for this dialog. ───── */
        if (verdict == WOWCONV_MODAL_END) {
            ++g_wd_ran;
            wu_puts(note, cap, &k, "MODAL 0x");
            wu_puthex(note, cap, &k, d->hwnd, 4);
            wu_puts(note, cap, &k, " ENDED -- DialogBox returns 0x");
            wu_puthex(note, cap, &k, d->result, 4);
            wu_puts(note, cap, &k, " after 0x");
            wu_puthex(note, cap, &k, d->msgs, 4);
            wu_puts(note, cap, &k, " message(s), 0x");
            wu_puthex(note, cap, &k, GetTickCount() - d->t0, 8);
            wu_puts(note, cap, &k, " ms. ");
            /* The window itself is USER's to destroy and ours to stop showing:
               EndDialog's own arm already hid it when Win32 declined to end a
               window that is not a real dialog. Nothing to do here but leave. */
            wowdlg_unwind(d, (DWORD)d->result);
            continue;              /* an OUTER modal dialog may still be running */
        }

        /* ── EXIT 2: THE WINDOW IS GONE. Nothing can ever drive this dialog
             again, so waiting for it would be the hang this file exists to
             avoid. Real Windows answers a dialog that could not run with 0. ── */
        if (verdict == WOWCONV_MODAL_GONE) {
            ++g_wd_refused;
            wu_puts(note, cap, &k, "MODAL 0x");
            wu_puthex(note, cap, &k, d->hwnd, 4);
            wu_puts(note, cap, &k, " -- ★ ITS WINDOW IS GONE (destroyed while"
                                   " modal); DialogBox returns 0 rather than"
                                   " waiting for input that can no longer"
                                   " arrive. ");
            wowdlg_unwind(d, 0);
            continue;
        }

        /* ── EXIT 3: THERE IS NOTHING TO CALL. A dialog is driven by a
             procedure; with neither a DLGPROC nor a class window procedure
             there is no way to tell the guest anything, so no EndDialog can
             ever happen. Degrading to session 56's behaviour here is
             deliberate: an immediate return ends the program legibly, a wait
             would hang it. ⚠ THIS IS THE LINE TO GREP FOR if a guest still
             exits at a modal dialog. */
        if (verdict == WOWCONV_MODAL_NOPROC) {
            ++g_wd_refused;
            wu_puts(note, cap, &k, "MODAL 0x");
            wu_puthex(note, cap, &k, d->hwnd, 4);
            wu_puts(note, cap, &k, " -- ★ NO DIALOG PROCEDURE AND NO CLASS WINDOW"
                                   " PROCEDURE: nothing to dispatch to, so the"
                                   " dialog could never be dismissed. Returning 0"
                                   " immediately (session 56's behaviour) rather"
                                   " than waiting forever. ");
            wowdlg_unwind(d, 0);
            continue;
        }

        /* Same rule as every other delivery path: the class's procedure where
           there is one, the dialog's own otherwise. See WowConvWindowProcedure(). */
        proc = (DWORD)WowConvWindowProcedure((unsigned)d->wndproc, (unsigned)d->dlgproc);

        /* ── ★★★ WM_INITDIALOG COMES FIRST, AND IT IS WHERE THE DIALOG FILLS
             ITSELF IN. TASKMAN's task list, a Preferences page's current
             settings, a Find dialog's last search string -- all of it is put
             there by the procedure's WM_INITDIALOG arm, so a loop that started
             at the first mouse click would show an empty dialog.
           ⚠ wParam IS THE CONTROL THAT SHOULD TAKE FOCUS and we pass 0: the
             control list is the template's and this host does not yet track
             which item carries WS_TABSTOP first. A procedure that returns TRUE
             (the usual answer) is telling USER "put the focus where the
             template says", which is the case we are already in. Named rather
             than left as a silent zero. */
        if (!d->inited) {
            d->inited = 1;
            msg = WM_INITDIALOG16; wparam = d->firstfocus; lparam = d->initparam;
            wu_puts(note, cap, &k, "MODAL 0x");
            wu_puthex(note, cap, &k, d->hwnd, 4);
            wu_puts(note, cap, &k, " -> WM_INITDIALOG ");
        } else {
            /* ── ★★★★ AND *NOW* IT APPEARS. WM_INITDIALOG has returned, so the
                 dialog has finished arranging itself -- which for TASKMAN means
                 it has centred itself on the screen and filled its list -- and
                 this is the moment real USER makes it visible. Showing it here
                 rather than at CreateWindow is what stops a guest's own
                 `MoveWindow(..., bRepaint=FALSE)` from stranding a painted
                 window at the position it started from; see the long note at the
                 CreateWindowEx call in wowuser.h, and the rig screenshot that
                 had one Task List's pixels in the corner and another's wallpaper
                 showing through its client.
               ⚠ ONCE. `toshow` is cleared, because ShowWindow on every turn of
                 the loop would fight a guest that hides its own dialog. */
            if (d->toshow && w->hwnd32) {
                d->toshow = 0;
                ShowWindow(w->hwnd32, SW_SHOW);
                wu_puts(note, cap, &k, "MODAL 0x");
                wu_puthex(note, cap, &k, d->hwnd, 4);
                wu_puts(note, cap, &k, " SHOWN (WM_INITDIALOG is done, so the"
                                       " dialog appears where it put itself). ");
                /* ── s93: AND THE FOCUS GOES TO THE FIRST TAB STOP, unless the
                     procedure placed it itself (it then returns FALSE, and the focus
                     is already inside the dialog). The note above assumed this was
                     the case we were in; the rig said otherwise: Program Manager's
                     "Program Item Properties" kept the focus on the dialog window,
                     so the first keys typed were lost and Tab only then reached
                     Description. WM_NEXTDLGCTL is the dialog manager's own way in,
                     and selects an edit's text as DialogBox does. */
                {   HWND fo = GetFocus();
                    if (!fo || fo == w->hwnd32 || !IsChild(w->hwnd32, fo)) {
                        HWND first = GetNextDlgTabItem(w->hwnd32, NULL, FALSE);
                        if (first) {
                            /* ⚠ NOT WM_NEXTDLGCTL: only the real DefDlgProc acts on
                                 it, and this window runs OUR procedure (see #32770
                                 in wowuser.h) -- measured, it did nothing. */
                            char fcn[16];
                            SetFocus(first);
                            if (GetClassNameA(first, fcn, sizeof fcn) && !lstrcmpiA(fcn, "Edit"))
                                SendMessageA(first, EM_SETSEL, 0, -1);
                            wu_puts(note, cap, &k, "Focus -> its first tab stop. ");
                        }
                    }
                }
            }
            /* ── ★★ WAIT FOR SOMETHING TO HAPPEN, WHICH IS WHAT MODAL MEANS.
                 The dialog's controls are real Win32 windows on this thread, so
                 a click on one becomes a Win32 message here, which wowwin_proc
                 turns into a Win16 WM_COMMAND for the dialog. Pumping and
                 waiting is therefore the same statement as "wait for the user",
                 and it is the identical loop a blocked GetMessage already runs
                 -- see main.c. Sharing the KNOB (wowidle.txt) matters more than
                 sharing the code: a modal dialog and an idle message loop are
                 the same kind of wait and a user who set one meant both. */
            DWORD t0 = GetTickCount(), beat = t0;
            DWORD pumped0 = g_ww_pumped;
            unsigned beats = 0;
            /* ── ★★ AND IT SAYS SO BEFORE IT BLOCKS, NOT AFTER. ───────────────
                 A log that goes silent at the moment a modal dialog appears is
                 indistinguishable from a host that died there, and this project
                 has already read one that way twice (the MessageBox arm in
                 main.c carries the same note for the same reason). So the line
                 goes out FIRST, and a heartbeat follows it, bounded so that a
                 dialog left up overnight cannot fill the disk. */
            {   char wb[192], *wq = wb;
                wq = zput(wq, "     WOWDLG: modal 0x"); wq = zhex(wq, d->hwnd);
                wq = zput(wq, " is WAITING for input -- the guest is parked"
                              " inside DialogBox on purpose, ");
                if (g_wowmsg_wait_ms) { wq = zput(wq, "for at most 0x");
                                        wq = zhex(wq, g_wowmsg_wait_ms);
                                        wq = zput(wq, " ms"); }
                else                    wq = zput(wq, "for as long as it takes"
                                                      " (wowidle.txt = 0)");
                wq = zput(wq, "\r\n");
                log_append(LOG_PATH, wb, wq);
            }
            g_wm_inwait = 1;
            while ((!running || *running) && !g_wm_count
                   && (!g_wowmsg_wait_ms || GetTickCount() - t0 < g_wowmsg_wait_ms)) {
                if (!wowdlg_pump(64, &d->trace))
                    MsgWaitForMultipleObjects(0, NULL, FALSE, 50, QS_ALLINPUT);
                /* ── ★★★ THE HEARTBEAT NAMES THE THREAD, AND THAT IS THE POINT.
                     (session 57, first run) The first cut of this loop printed a
                     running total and a queue depth, and both were FROZEN --
                     `Win32 dispatched 0x0a` for forty seconds while a dialog sat
                     unpainted and a click on its Cancel button produced nothing.
                     A frozen total says "nothing arrived"; it does NOT say why,
                     and the two candidate whys need opposite fixes:
                       - the messages are on ANOTHER THREAD's queue, because the
                         windows were created there (PeekMessage is per-thread and
                         would sit here reading empty forever), or
                       - they are on THIS queue and something else is wrong.
                     `GetQueueStatus` answers the second and the thread ids answer
                     the first, so the next run cannot be ambiguous the way this
                     one was. ⚠ AND IT NO LONGER STOPS AT TWENTY: it slows to one
                     line a minute instead. A log that goes quiet while a guest is
                     parked is the exact instrument failure this file's header
                     warns about, and I shipped it anyway. */
                {   DWORD every = (beats < 20) ? 2000 : 60000;
                    if (GetTickCount() - beat >= every) {
                        char hb[256], *hq = hb;
                        beat = GetTickCount(); ++beats;
                        hq = zput(hq, "     WOWDLG: modal 0x"); hq = zhex(hq, d->hwnd);
                        hq = zput(hq, " waiting 0x");   hq = zhex(hq, beat - t0);
                        hq = zput(hq, " ms; pumped 0x"); hq = zhex(hq, g_ww_pumped);
                        hq = zput(hq, " (+0x");         hq = zhex(hq, g_ww_pumped - pumped0);
                        hq = zput(hq, " since blocking), Win16 queued 0x");
                        hq = zhex(hq, (DWORD)g_wm_count);
                        hq = zput(hq, "; queue status 0x");
                        hq = zhex(hq, GetQueueStatus(QS_ALLINPUT));
                        hq = zput(hq, "; this thread 0x");
                        hq = zhex(hq, GetCurrentThreadId());
                        hq = zput(hq, ", the windows' thread 0x");
                        hq = zhex(hq, g_ww_thread);
                        if (g_ww_thread && g_ww_thread != GetCurrentThreadId())
                            hq = zput(hq, " -- ★★ DIFFERENT: PeekMessage is"
                                          " PER-THREAD, so this loop can never see"
                                          " their input");
                        hq = zput(hq, "\r\n");
                        log_append(LOG_PATH, hb, hq);
                    }
                }
                /* ⚠ AND WATCH THE DIALOG ITSELF, NOT ONLY THE QUEUE. A dialog
                     dismissed by its own procedure during a message we
                     dispatched INSIDE this pump (a WM_COMMAND handled by a
                     nested SendMessage, say) is over, and so is one whose
                     window the OS has destroyed. Either way there is nothing
                     left to wait for, and the checks at the top of the loop are
                     the ones that say so -- this just stops waiting. */
                if (d->ended || !IsWindow(w->hwnd32)) break;
            }
            g_wm_inwait = 0;
            if (d->ended || !IsWindow(w->hwnd32)) continue;

            /* ── EXIT 4: THE WAIT EXPIRED. Only reachable with a bounded
                 wowidle.txt, which is the unattended-harness setting; an
                 interactive session waits forever and never gets here. The same
                 tested decision as the three above, asked again now that the
                 fourth fact is known. */
            if (!wowmsg_take(0, 0, 0, 1, &m)) {
                /* ⚠ AND ONLY IF NOTHING ELSE ENDED IT WHILE WE WAITED. The
                     answer for a dialog that was dismissed during the wait is
                     EndDialog's result, not 0 -- so anything but EXPIRED goes
                     back to the top of the loop, where that case is handled. */
                if (WowConvModalExit(d->ended, 1, 1, 1)
                        != WOWCONV_MODAL_EXPIRED)
                    continue;
                ++g_wd_refused;
                wu_puts(note, cap, &k, "MODAL 0x");
                wu_puthex(note, cap, &k, d->hwnd, 4);
                wu_puts(note, cap, &k, " -- the host's input wait expired with an"
                                       " empty queue (wowidle.txt is bounded), so"
                                       " nobody is going to dismiss this dialog;"
                                       " DialogBox returns 0. ");
                wowdlg_unwind(d, 0);
                continue;
            }

            /* ── ★ WHOSE MESSAGE IS IT? A modal dialog's loop is the TASK's
                 loop for as long as it runs, so messages for other windows
                 arrive here too -- and they must still be delivered, or a
                 repaint of the window behind the dialog never happens. So the
                 target is the message's own window, and only the *procedure* is
                 looked up per window; the dialog's own dlgproc is used only for
                 the dialog itself.
               ⚠ A MESSAGE FOR A WINDOW WE NO LONGER HAVE IS DROPPED, not
                 dispatched to the dialog. Handing a stale hwnd's message to the
                 wrong procedure is the "answered by an unrelated function"
                 shape this project treats as worse than not answering. */
            msg = m.msg; wparam = m.wparam; lparam = m.lparam;
            if (m.hwnd == d->hwnd) {
                proc = (DWORD)WowConvWindowProcedure((unsigned)d->wndproc,
                                              (unsigned)d->dlgproc);
            } else {
                wowuser_win_t *tw = m.hwnd ? wowuser_findwin(m.hwnd) : NULL;
                DWORD tp = tw ? wowuser_winproc_of(tw) : 0;
                if (!tp) {
                    ++d->msgs;
                    continue;            /* nowhere to put it; take the next one */
                }
                proc = tp;
                tgt  = m.hwnd;
            }
            wu_puts(note, cap, &k, "MODAL 0x");
            wu_puthex(note, cap, &k, d->hwnd, 4);
            wu_puts(note, cap, &k, " -> hwnd=0x");
            wu_puthex(note, cap, &k, m.hwnd, 4);
            wu_puts(note, cap, &k, " msg=0x");
            wu_puthex(note, cap, &k, msg, 4);
            wu_puts(note, cap, &k, " ");
        }

        /* ── THE CALL ITSELF. Five words in declared order, the shape every
             window and dialog procedure takes -- wowuser_want_msg builds the
             same block for DispatchMessage, and this is that block built by
             hand because there is no service frame here to hang it on. */
        /* ⚠ THE DIALOG'S handle for the dialog's own messages: a dialog procedure
             is called with the dialog, and a control's own message arrives at the
             dialog carrying the control in lParam.
           ★ s93: BUT ANOTHER WINDOW'S MESSAGE CARRIES THAT WINDOW. This said "the
             dialog's handle, always" -- so TERMINAL's own WM_PAINT, arriving while its
             "Default Serial Port" dialog was up, reached Terminal's procedure with the
             DIALOG's hwnd: Terminal's client was never painted (the desktop showed
             through, the user's "captures the desktop") and the dialog was painted
             with Terminal's code. The right procedure was already looked up per
             window; the handle has to travel with it. */
        arg[0] = tgt;
        arg[1] = msg;
        arg[2] = wparam;
        arg[3] = (WORD)(lparam >> 16);
        arg[4] = (WORD)(lparam & 0xFFFF);

        /* Is the procedure's code segment loaded? Not present means we must go
           in through the RETF trampoline so krnl386's own #NP handler loads it
           -- writing that selector into CS kills the VDM silently, which cost a
           session on CARDFILE. See WOWCALL_RETF_OFF in wowcall.h.
           ⚠ ASKED, NOT COMPUTED HERE: the LDT is the host's, and this file has
             no business reaching into it. main.c answers. */
        absent = wowdlg_sel_absent((WORD)(proc >> 16));

        if (!rsel || !ssbase
            || !wowcall_enter(tib, ssbase, rsel, proc, d->ds, arg, 5,
                              /* retlin */ 0, WOWCALL_RET_KEEP, NULL,
                              tgt, msg, NULL, 0, -1, absent)) {
            /* The call could not be made -- depth, or no return selector. That
               is not a reason to spin: without a call there is no EndDialog. */
            ++g_wd_refused;
            wu_puts(note, cap, &k, "-- ★ THE CALL WAS REFUSED (depth, or no return"
                                   " selector); DialogBox returns 0 rather than"
                                   " looping with no way to be dismissed. ");
            wowdlg_unwind(d, 0);
            continue;
        }
        if (g_wc_depth > 0) {
            g_wc[g_wc_depth - 1].action = WOWCALL_ACT_MODALPUMP;
            g_wc[g_wc_depth - 1].actarg = d->hwnd;
            /* s88: say whether THIS call went to the dialog's own DLGPROC, so the
               return can apply DefDlgProc's WM_CLOSE default (see main.c). */
            g_wd_dlgcall[g_wc_depth - 1] = (m.hwnd == d->hwnd && proc == d->dlgproc
                                            && !d->wndproc);
            g_wd_dlgmsg[g_wc_depth - 1]  = msg;
            g_wu_dlgdef[g_wc_depth - 1].wp = wparam;
            g_wu_dlgdef[g_wc_depth - 1].lp = lparam;
        }
        ++d->msgs;
        wu_puts(note, cap, &k, "-> 0x");
        wu_puthex(note, cap, &k, proc >> 16, 4);
        wu_puts(note, cap, &k, ":0x");
        wu_puthex(note, cap, &k, proc & 0xFFFF, 4);
        if (absent) wu_puts(note, cap, &k, " [segment not present -- via the RETF"
                                           " trampoline]");
        return 1;
    }
}

#endif /* WOWDLG_H */
