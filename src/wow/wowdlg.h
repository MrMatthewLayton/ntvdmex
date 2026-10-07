#ifndef NTVDMEX_WOWDLG_H
#define NTVDMEX_WOWDLG_H
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
 * RETURNS the host decides -- from the frame's `Action` -- what to do next,
 * including issuing another call. A modal loop is that chain with a different
 * continuation rule:
 *
 *     DialogBox BOP           park the caller; do NOT write its return value yet
 *       -> WM_INITDIALOG      call the dialog procedure    (WowCallEnter)
 *       <- it returns         ACT_MODALPUMP: is it over? no
 *       -> wait for input     pump Win32, take a Win16 message
 *       -> WM_COMMAND         call the dialog procedure
 *       <- it returns         ACT_MODALPUMP: EndDialog was seen
 *       -> UNWIND             write nResult into the DialogBox return hole and
 *                             let the guest resume past its BOP at last
 *
 * ★ THE PARKED CONTEXT IS FREE, AND THAT IS THE WHOLE TRICK. `WowCallEnter`
 *   saves the guest exactly as it will be resumed -- which, at the DialogBox
 *   BOP, is "the caller, one instruction after DialogBox". Every iteration
 *   restores that same context and re-parks it, so when the loop finally does
 *   nothing, the guest is standing precisely where DialogBox should return to.
 *   No stack grows, no EIP is written by hand, no context is invented.
 * ⚠ SO THE PER-MESSAGE CALLS PASS returnLinear = 0. Their return values are the
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
 *   the message loop already has, and `g_WowMsgInWait` is set across it for the
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
 *   class reaches its procedure through `DialogProcedure` rather than through a class
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
static INT  g_WowDlgIsDialogCall[WOWCALL_MAX_DEPTH];
static WORD g_WowDlgMessage[WOWCALL_MAX_DEPTH];

typedef struct _WOWDLG_MODAL {
    WORD  Window;          /* the dialog's Win16 handle -- the loop's identity      */
    DWORD ReturnLinear;    /* THE RETURN HOLE of the DialogBox call we parked       */
    DWORD DialogProcedure; /* the guest's dialog procedure, or 0                    */
    DWORD WindowProcedure; /* its class's window procedure, or 0                    */
    WORD  DataSelector;    /* the DS/AX both must be entered with                   */
    INT   IsInitialised;   /* WM_INITDIALOG has been sent                           */
    INT   IsShowDeferred;  /* the template said WS_VISIBLE and we DEFERRED it       */
    INT   IsEnded;         /* EndDialog was called for this dialog                  */
    WORD  Result;          /* ...and this is the nResult it passed                  */
    DWORD Messages;        /* messages dispatched into it, for the log              */
    /* ⚠ THE WIN32 TRACE BUDGET IS PER DIALOG, NOT PER WAIT, and that distinction
         is the difference between an instrument and a flood: the wait below is
         re-entered after EVERY message, so a budget living there would re-arm
         48 lines each time and a mouse crossing the dialog would write a line
         per move. TERMINAL's 158 MB log is what that looks like. */
    INT   TraceBudget;
    DWORD StartTime;       /* when it went up -- so the log can say how long it ran */
    /* GH #279: the top-level window DialogBox disabled, re-enabled at unwind.
       NULL when there was no owner or it was ALREADY disabled -- a nested dialog
       whose owner is the outer dialog must not enable a window it did not
       disable. */
    HWND  Owner32;
    DWORD InitParameter;   /* DialogBoxParam's lParam, for WM_INITDIALOG (s89)  */
    WORD  FirstFocus;      /* the first WS_TABSTOP control, WM_INITDIALOG's wParam */
} WOWDLG_MODAL, *PWOWDLG_MODAL;

static WOWDLG_MODAL g_WowDlgModals[WOWDLG_MAX_MODAL];
static INT   g_WowDlgDepth   = 0;
static DWORD g_WowDlgRan     = 0;   /* modal dialogs run to completion this run    */
static DWORD g_WowDlgRefused = 0;   /* ...and ones the host could not drive        */

/* ⚠ NO FORWARD DECLARATIONS HERE, unlike wowwin.h. That file is included BEFORE
     wowuser.h and has to declare what it borrows; this one is included AFTER, so
     the window table, `wowuser_findwin` and `wowuser_winproc_of` are already
     complete -- and re-declaring the struct would be a duplicate typedef. The
     include order in main.c is what makes that true; it is commented there. */

/* Defined in main.c, which owns the LDT: is this code selector NOT PRESENT?
   See the call site, and WOWCALL_RETF_OFF in wowcall.h for what it decides. */
static INT WowDlgIsSelectorAbsent(WORD sel);

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

static INT WowDlgPump(INT budget, PINT traceBudget)
{
    MSG message;
    INT count = 0;
    while (count < budget && PeekMessageA(&message, NULL, 0, 0, PM_REMOVE)) {
        if (traceBudget && *traceBudget > 0) {
            CHAR traceBuffer[200], *traceCursor = traceBuffer;
            WORD window16 = wowwin_hwnd16(message.hwnd);
            --*traceBudget;
            traceCursor = zput(traceCursor, "       WOWDLG/win32: msg=0x"); traceCursor = zhex(traceCursor, message.message);
            traceCursor = zput(traceCursor, " hwnd=0x");   traceCursor = zhex(traceCursor, (DWORD)(ULONG_PTR)message.hwnd);
            traceCursor = zput(traceCursor, " wp=0x");     traceCursor = zhex(traceCursor, (DWORD)message.wParam);
            traceCursor = zput(traceCursor, " lp=0x");     traceCursor = zhex(traceCursor, (DWORD)message.lParam);
            traceCursor = zput(traceCursor, " -> win16 0x"); traceCursor = zhex(traceCursor, window16);
            if (!window16) traceCursor = zput(traceCursor, " (★ NOT ONE OF OURS -- nothing will be"
                                    " posted for it)");
            traceCursor = zput(traceCursor, "\r\n");
            log_append(LOG_PATH, traceBuffer, traceCursor);
        }
        if (wowwin_tt_fire(&message)) { ++count; ++g_ww_pumped; continue; }   /* s93 */
        /* #305 M11 (s91): THE DIALOG MANAGER'S KEYS. DialogBox's own loop gives a modal
             dialog Tab / Shift+Tab between its controls, Enter = the default button and
             Esc = IDCANCEL -- without the program asking. This loop dispatched keys raw,
             so Esc did nothing in TASKMAN's Task List (runs/s91/chain25). The controls are
             real windows, so the OS's IsDialogMessage does the work; what it generates
             (WM_COMMAND IDCANCEL/IDOK) reaches the dialog procedure through wowwin_proc
             like a click does. Keyboard messages only, and only for the topmost modal
             dialog or one of its children. */
        if (message.message >= WM_KEYFIRST && message.message <= WM_KEYLAST && g_WowDlgDepth > 0) {
            HWND dialog = wowuser_hwnd32(g_WowDlgModals[g_WowDlgDepth - 1].Window);
            /* ── s93: A KEY FOR THE DIALOG WINDOW ITSELF belongs to a control -- a
                 dialog with controls never holds the focus (DefDlgProc passes it on).
                 Ours could end up holding it (Program Item Properties: the first
                 letters typed vanished, measured); the key and the focus go to the
                 control that last had it, else the first tab stop. */
            if (dialog && message.hwnd == dialog && message.message == WM_KEYDOWN) {
                HWND saved = (HWND)GetPropA(dialog, "NTVDMEX16.DlgFocus");
                if (!saved || !IsWindow(saved) || !IsChild(dialog, saved))
                    saved = GetNextDlgTabItem(dialog, NULL, FALSE);
                if (saved && saved != dialog) { SetFocus(saved); message.hwnd = saved; }
            }
            if (dialog && IsWindow(dialog) && (message.hwnd == dialog || IsChild(dialog, message.hwnd))
                && IsDialogMessageA(dialog, &message)) {
                ++count; ++g_ww_pumped;
                continue;
            }
        }
        TranslateMessage(&message);
        DispatchMessageA(&message);
        ++count; ++g_ww_pumped;
    }
    return count;
}

static PWOWDLG_MODAL WowDlgTop(VOID)
{
    return g_WowDlgDepth > 0 ? &g_WowDlgModals[g_WowDlgDepth - 1] : NULL;
}

/* Is any modal dialog up? Read by the DialogBox service, so that a nested one
   is described honestly in the trace rather than looking like the first. */
static INT WowDlgActive(VOID)
{
    return g_WowDlgDepth;
}

/*
 * Park a DialogBox call. `retlin` is the linear address of ITS return hole --
 * the four bytes the caller will read as DialogBox's result -- and nothing
 * writes them until this dialog ends.
 * Returns 0 if the stack is full, in which case the caller must complete the
 * call the old way rather than pretend.
 */
static INT WowDlgPush(WORD window, DWORD returnLinear, DWORD dialogProcedure, DWORD windowProcedure,
                       WORD dataSelector, INT isShowDeferred, HWND owner32)
{
    PWOWDLG_MODAL dialog;
    if (g_WowDlgDepth >= WOWDLG_MAX_MODAL) return 0;
    if (!window || !returnLinear) return 0;
    dialog = &g_WowDlgModals[g_WowDlgDepth++];
    dialog->Window    = window;
    dialog->ReturnLinear  = returnLinear;
    dialog->DialogProcedure = dialogProcedure;
    dialog->WindowProcedure = windowProcedure;
    dialog->DataSelector      = dataSelector;
    dialog->IsInitialised  = 0;
    dialog->IsShowDeferred  = isShowDeferred;
    dialog->IsEnded   = 0;
    dialog->Result  = 0;
    dialog->Messages    = 0;
    dialog->TraceBudget   = WOWDLG_TRACE;
    dialog->StartTime      = GetTickCount();
    /* ── GH #279: A MODAL DIALOG DISABLES ITS OWNER, as USER's DialogBox does.
         Without it the main window's X stayed live under Terminal's first-run
         "Default Serial Port" dialog and took WM_CLOSE. Disabling the REAL
         window is the whole fix: Windows itself then ignores clicks on it,
         its X included, so nothing reaches wowwin_proc to be posted. The
         owner is the top-level window: a dialog owned by a child control
         disables the frame around it, as USER's does. */
    dialog->Owner32 = NULL;
    dialog->InitParameter = 0; dialog->FirstFocus = 0;     /* the caller sets them after the push */
    if (owner32) {
        HWND top = GetAncestor(owner32, GA_ROOT);
        if (!top) top = owner32;
        if (IsWindowEnabled(top)) { EnableWindow(top, FALSE); dialog->Owner32 = top; }
    }
    return 1;
}

/* s89: WM_INITDIALOG's wParam/lParam for the dialog just pushed. */
static VOID WowDlgSetInit(DWORD initParameter, WORD firstFocus)
{
    if (g_WowDlgDepth <= 0) return;
    g_WowDlgModals[g_WowDlgDepth - 1].InitParameter  = initParameter;
    g_WowDlgModals[g_WowDlgDepth - 1].FirstFocus = firstFocus;
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
static INT WowDlgEnd(WORD window, WORD result)
{
    INT index;
    for (index = g_WowDlgDepth - 1; index >= 0; --index)
        if (g_WowDlgModals[index].Window == window) {
            g_WowDlgModals[index].IsEnded  = 1;
            g_WowDlgModals[index].Result = result;
            return 1;
        }
    return 0;
}

/* Write the answer into the parked call's return hole and drop the frame. The
   hole is guest memory reached by linear address, exactly as wowcall.h revises
   a refused WM_CREATE, so it outlives every context switch in between. */
static VOID WowDlgUnwind(PWOWDLG_MODAL dialog, DWORD value)
{
    volatile BYTE *hole = (volatile BYTE *)(ULONG_PTR)dialog->ReturnLinear;
    hole[0] = (BYTE)(value & 0xFF);         hole[1] = (BYTE)((value >> 8)  & 0xFF);
    hole[2] = (BYTE)((value >> 16) & 0xFF); hole[3] = (BYTE)((value >> 24) & 0xFF);
    /* GH #279: give the owner back BEFORE the dialog goes, so activation returns
       to it rather than to whatever window Windows picks next. */
    if (dialog->Owner32 && IsWindow(dialog->Owner32)) EnableWindow(dialog->Owner32, TRUE);
    dialog->Owner32 = NULL;
    if (g_WowDlgDepth > 0) --g_WowDlgDepth;
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
static INT WowDlgStep(volatile BYTE *tib, DWORD stackBase, WORD returnSelector,
                       const volatile LONG *running, PSTR note, INT noteCapacity)
{
    INT noteLength = 0;
    for (;;) {
        PWOWDLG_MODAL dialog = WowDlgTop();
        wowuser_win_t  *window;
        WOWMSG message;
        DWORD procedure = 0;
        WORD  arguments[5];
        WORD  messageNumber, wParam;
        WORD  target;                      /* s93: the window the call is FOR */
        DWORD lParam;
        INT   isAbsent = 0;
        INT   verdict;

        if (!dialog) return 0;
        window = wowuser_findwin(dialog->Window);
        target = dialog->Window;

        /* ── ★★★ THE FOUR EXITS ARE ONE DECISION, AND IT IS A TESTED FUNCTION.
             `WowConvModalExit` in wowconv.h is total -- every combination of
             the four facts returns something -- so there is no state in which
             this loop neither runs nor leaves. That is the property that makes
             a modal loop safe to ship, and it is pinned off-VM in wow_test.c
             rather than argued for here. `isWaitExpired` is 0 at this point
             because the wait has not happened yet; the branch that runs it asks
             again with 1. */
        verdict = WowConvModalExit(dialog->IsEnded,
                                     window && window->hwnd32 && IsWindow(window->hwnd32),
                                     dialog->DialogProcedure || dialog->WindowProcedure, 0);

        /* ── EXIT 1: THE REAL ONE. EndDialog was called for this dialog. ───── */
        if (verdict == WOWCONV_MODAL_END) {
            ++g_WowDlgRan;
            wu_puts(note, noteCapacity, &noteLength, "MODAL 0x");
            wu_puthex(note, noteCapacity, &noteLength, dialog->Window, 4);
            wu_puts(note, noteCapacity, &noteLength, " ENDED -- DialogBox returns 0x");
            wu_puthex(note, noteCapacity, &noteLength, dialog->Result, 4);
            wu_puts(note, noteCapacity, &noteLength, " after 0x");
            wu_puthex(note, noteCapacity, &noteLength, dialog->Messages, 4);
            wu_puts(note, noteCapacity, &noteLength, " message(s), 0x");
            wu_puthex(note, noteCapacity, &noteLength, GetTickCount() - dialog->StartTime, 8);
            wu_puts(note, noteCapacity, &noteLength, " ms. ");
            /* The window itself is USER's to destroy and ours to stop showing:
               EndDialog's own arm already hid it when Win32 declined to end a
               window that is not a real dialog. Nothing to do here but leave. */
            WowDlgUnwind(dialog, (DWORD)dialog->Result);
            continue;              /* an OUTER modal dialog may still be running */
        }

        /* ── EXIT 2: THE WINDOW IS GONE. Nothing can ever drive this dialog
             again, so waiting for it would be the hang this file exists to
             avoid. Real Windows answers a dialog that could not run with 0. ── */
        if (verdict == WOWCONV_MODAL_GONE) {
            ++g_WowDlgRefused;
            wu_puts(note, noteCapacity, &noteLength, "MODAL 0x");
            wu_puthex(note, noteCapacity, &noteLength, dialog->Window, 4);
            wu_puts(note, noteCapacity, &noteLength, " -- ★ ITS WINDOW IS GONE (destroyed while"
                                   " modal); DialogBox returns 0 rather than"
                                   " waiting for input that can no longer"
                                   " arrive. ");
            WowDlgUnwind(dialog, 0);
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
            ++g_WowDlgRefused;
            wu_puts(note, noteCapacity, &noteLength, "MODAL 0x");
            wu_puthex(note, noteCapacity, &noteLength, dialog->Window, 4);
            wu_puts(note, noteCapacity, &noteLength, " -- ★ NO DIALOG PROCEDURE AND NO CLASS WINDOW"
                                   " PROCEDURE: nothing to dispatch to, so the"
                                   " dialog could never be dismissed. Returning 0"
                                   " immediately (session 56's behaviour) rather"
                                   " than waiting forever. ");
            WowDlgUnwind(dialog, 0);
            continue;
        }

        /* Same rule as every other delivery path: the class's procedure where
           there is one, the dialog's own otherwise. See WowConvWindowProcedure(). */
        procedure = (DWORD)WowConvWindowProcedure((UINT)dialog->WindowProcedure, (UINT)dialog->DialogProcedure);

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
        if (!dialog->IsInitialised) {
            dialog->IsInitialised = 1;
            messageNumber = WM_INITDIALOG16; wParam = dialog->FirstFocus; lParam = dialog->InitParameter;
            wu_puts(note, noteCapacity, &noteLength, "MODAL 0x");
            wu_puthex(note, noteCapacity, &noteLength, dialog->Window, 4);
            wu_puts(note, noteCapacity, &noteLength, " -> WM_INITDIALOG ");
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
               ⚠ ONCE. `IsShowDeferred` is cleared, because ShowWindow on every turn of
                 the loop would fight a guest that hides its own dialog. */
            if (dialog->IsShowDeferred && window->hwnd32) {
                dialog->IsShowDeferred = 0;
                ShowWindow(window->hwnd32, SW_SHOW);
                wu_puts(note, noteCapacity, &noteLength, "MODAL 0x");
                wu_puthex(note, noteCapacity, &noteLength, dialog->Window, 4);
                wu_puts(note, noteCapacity, &noteLength, " SHOWN (WM_INITDIALOG is done, so the"
                                       " dialog appears where it put itself). ");
                /* ── s93: AND THE FOCUS GOES TO THE FIRST TAB STOP, unless the
                     procedure placed it itself (it then returns FALSE, and the focus
                     is already inside the dialog). The note above assumed this was
                     the case we were in; the rig said otherwise: Program Manager's
                     "Program Item Properties" kept the focus on the dialog window,
                     so the first keys typed were lost and Tab only then reached
                     Description. WM_NEXTDLGCTL is the dialog manager's own way in,
                     and selects an edit's text as DialogBox does. */
                {   HWND focus = GetFocus();
                    if (!focus || focus == window->hwnd32 || !IsChild(window->hwnd32, focus)) {
                        HWND firstTabStop = GetNextDlgTabItem(window->hwnd32, NULL, FALSE);
                        if (firstTabStop) {
                            /* ⚠ NOT WM_NEXTDLGCTL: only the real DefDlgProc acts on
                                 it, and this window runs OUR procedure (see #32770
                                 in wowuser.h) -- measured, it did nothing. */
                            CHAR className[16];
                            SetFocus(firstTabStop);
                            if (GetClassNameA(firstTabStop, className, sizeof className) && !lstrcmpiA(className, "Edit"))
                                SendMessageA(firstTabStop, EM_SETSEL, 0, -1);
                            wu_puts(note, noteCapacity, &noteLength, "Focus -> its first tab stop. ");
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
            DWORD startTime = GetTickCount(), lastBeat = startTime;
            DWORD pumpedAtStart = g_ww_pumped;
            UINT beatCount = 0;
            /* ── ★★ AND IT SAYS SO BEFORE IT BLOCKS, NOT AFTER. ───────────────
                 A log that goes silent at the moment a modal dialog appears is
                 indistinguishable from a host that died there, and this project
                 has already read one that way twice (the MessageBox arm in
                 main.c carries the same note for the same reason). So the line
                 goes out FIRST, and a heartbeat follows it, bounded so that a
                 dialog left up overnight cannot fill the disk. */
            {   CHAR waitBuffer[192], *waitCursor = waitBuffer;
                waitCursor = zput(waitCursor, "     WOWDLG: modal 0x"); waitCursor = zhex(waitCursor, dialog->Window);
                waitCursor = zput(waitCursor, " is WAITING for input -- the guest is parked"
                              " inside DialogBox on purpose, ");
                if (g_WowMsgWaitMs) { waitCursor = zput(waitCursor, "for at most 0x");
                                        waitCursor = zhex(waitCursor, g_WowMsgWaitMs);
                                        waitCursor = zput(waitCursor, " ms"); }
                else                    waitCursor = zput(waitCursor, "for as long as it takes"
                                                      " (wowidle.txt = 0)");
                waitCursor = zput(waitCursor, "\r\n");
                log_append(LOG_PATH, waitBuffer, waitCursor);
            }
            g_WowMsgInWait = 1;
            while ((!running || *running) && !g_WowMsgCount
                   && (!g_WowMsgWaitMs || GetTickCount() - startTime < g_WowMsgWaitMs)) {
                if (!WowDlgPump(64, &dialog->TraceBudget))
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
                {   DWORD interval = (beatCount < 20) ? 2000 : 60000;
                    if (GetTickCount() - lastBeat >= interval) {
                        CHAR beatBuffer[256], *beatCursor = beatBuffer;
                        lastBeat = GetTickCount(); ++beatCount;
                        beatCursor = zput(beatCursor, "     WOWDLG: modal 0x"); beatCursor = zhex(beatCursor, dialog->Window);
                        beatCursor = zput(beatCursor, " waiting 0x");   beatCursor = zhex(beatCursor, lastBeat - startTime);
                        beatCursor = zput(beatCursor, " ms; pumped 0x"); beatCursor = zhex(beatCursor, g_ww_pumped);
                        beatCursor = zput(beatCursor, " (+0x");         beatCursor = zhex(beatCursor, g_ww_pumped - pumpedAtStart);
                        beatCursor = zput(beatCursor, " since blocking), Win16 queued 0x");
                        beatCursor = zhex(beatCursor, (DWORD)g_WowMsgCount);
                        beatCursor = zput(beatCursor, "; queue status 0x");
                        beatCursor = zhex(beatCursor, GetQueueStatus(QS_ALLINPUT));
                        beatCursor = zput(beatCursor, "; this thread 0x");
                        beatCursor = zhex(beatCursor, GetCurrentThreadId());
                        beatCursor = zput(beatCursor, ", the windows' thread 0x");
                        beatCursor = zhex(beatCursor, g_ww_thread);
                        if (g_ww_thread && g_ww_thread != GetCurrentThreadId())
                            beatCursor = zput(beatCursor, " -- ★★ DIFFERENT: PeekMessage is"
                                          " PER-THREAD, so this loop can never see"
                                          " their input");
                        beatCursor = zput(beatCursor, "\r\n");
                        log_append(LOG_PATH, beatBuffer, beatCursor);
                    }
                }
                /* ⚠ AND WATCH THE DIALOG ITSELF, NOT ONLY THE QUEUE. A dialog
                     dismissed by its own procedure during a message we
                     dispatched INSIDE this pump (a WM_COMMAND handled by a
                     nested SendMessage, say) is over, and so is one whose
                     window the OS has destroyed. Either way there is nothing
                     left to wait for, and the checks at the top of the loop are
                     the ones that say so -- this just stops waiting. */
                if (dialog->IsEnded || !IsWindow(window->hwnd32)) break;
            }
            g_WowMsgInWait = 0;
            if (dialog->IsEnded || !IsWindow(window->hwnd32)) continue;

            /* ── EXIT 4: THE WAIT EXPIRED. Only reachable with a bounded
                 wowidle.txt, which is the unattended-harness setting; an
                 interactive session waits forever and never gets here. The same
                 tested decision as the three above, asked again now that the
                 fourth fact is known. */
            if (!WowMsgTake(0, 0, 0, 1, &message)) {
                /* ⚠ AND ONLY IF NOTHING ELSE ENDED IT WHILE WE WAITED. The
                     answer for a dialog that was dismissed during the wait is
                     EndDialog's result, not 0 -- so anything but EXPIRED goes
                     back to the top of the loop, where that case is handled. */
                if (WowConvModalExit(dialog->IsEnded, 1, 1, 1)
                        != WOWCONV_MODAL_EXPIRED)
                    continue;
                ++g_WowDlgRefused;
                wu_puts(note, noteCapacity, &noteLength, "MODAL 0x");
                wu_puthex(note, noteCapacity, &noteLength, dialog->Window, 4);
                wu_puts(note, noteCapacity, &noteLength, " -- the host's input wait expired with an"
                                       " empty queue (wowidle.txt is bounded), so"
                                       " nobody is going to dismiss this dialog;"
                                       " DialogBox returns 0. ");
                WowDlgUnwind(dialog, 0);
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
            messageNumber = message.Message; wParam = message.WParam; lParam = message.LParam;
            if (message.Window == dialog->Window) {
                procedure = (DWORD)WowConvWindowProcedure((UINT)dialog->WindowProcedure,
                                              (UINT)dialog->DialogProcedure);
            } else {
                wowuser_win_t *targetWindow = message.Window ? wowuser_findwin(message.Window) : NULL;
                DWORD targetProcedure = targetWindow ? wowuser_winproc_of(targetWindow) : 0;
                if (!targetProcedure) {
                    ++dialog->Messages;
                    continue;            /* nowhere to put it; take the next one */
                }
                procedure = targetProcedure;
                target  = message.Window;
            }
            wu_puts(note, noteCapacity, &noteLength, "MODAL 0x");
            wu_puthex(note, noteCapacity, &noteLength, dialog->Window, 4);
            wu_puts(note, noteCapacity, &noteLength, " -> hwnd=0x");
            wu_puthex(note, noteCapacity, &noteLength, message.Window, 4);
            wu_puts(note, noteCapacity, &noteLength, " msg=0x");
            wu_puthex(note, noteCapacity, &noteLength, messageNumber, 4);
            wu_puts(note, noteCapacity, &noteLength, " ");
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
        arguments[0] = target;
        arguments[1] = messageNumber;
        arguments[2] = wParam;
        arguments[3] = (WORD)(lParam >> 16);
        arguments[4] = (WORD)(lParam & 0xFFFF);

        /* Is the procedure's code segment loaded? Not present means we must go
           in through the RETF trampoline so krnl386's own #NP handler loads it
           -- writing that selector into CS kills the VDM silently, which cost a
           session on CARDFILE. See WOWCALL_RETF_OFF in wowcall.h.
           ⚠ ASKED, NOT COMPUTED HERE: the LDT is the host's, and this file has
             no business reaching into it. main.c answers. */
        isAbsent = WowDlgIsSelectorAbsent((WORD)(procedure >> 16));

        if (!returnSelector || !stackBase
            || !WowCallEnter(tib, stackBase, returnSelector, procedure, dialog->DataSelector, arguments, 5,
                              /* returnLinear */ 0, WOWCALL_RET_KEEP, NULL,
                              target, messageNumber, NULL, 0, -1, isAbsent)) {
            /* The call could not be made -- depth, or no return selector. That
               is not a reason to spin: without a call there is no EndDialog. */
            ++g_WowDlgRefused;
            wu_puts(note, noteCapacity, &noteLength, "-- ★ THE CALL WAS REFUSED (depth, or no return"
                                   " selector); DialogBox returns 0 rather than"
                                   " looping with no way to be dismissed. ");
            WowDlgUnwind(dialog, 0);
            continue;
        }
        if (g_WowCallDepth > 0) {
            g_WowCallFrames[g_WowCallDepth - 1].Action = WOWCALL_ACT_MODALPUMP;
            g_WowCallFrames[g_WowCallDepth - 1].ActionArgument = dialog->Window;
            /* s88: say whether THIS call went to the dialog's own DLGPROC, so the
               return can apply DefDlgProc's WM_CLOSE default (see main.c). */
            g_WowDlgIsDialogCall[g_WowCallDepth - 1] = (message.Window == dialog->Window && procedure == dialog->DialogProcedure
                                            && !dialog->WindowProcedure);
            g_WowDlgMessage[g_WowCallDepth - 1]  = messageNumber;
            g_wu_dlgdef[g_WowCallDepth - 1].wp = wParam;
            g_wu_dlgdef[g_WowCallDepth - 1].lp = lParam;
        }
        ++dialog->Messages;
        wu_puts(note, noteCapacity, &noteLength, "-> 0x");
        wu_puthex(note, noteCapacity, &noteLength, procedure >> 16, 4);
        wu_puts(note, noteCapacity, &noteLength, ":0x");
        wu_puthex(note, noteCapacity, &noteLength, procedure & 0xFFFF, 4);
        if (isAbsent) wu_puts(note, noteCapacity, &noteLength, " [segment not present -- via the RETF"
                                           " trampoline]");
        return 1;
    }
}

#endif /* NTVDMEX_WOWDLG_H */
