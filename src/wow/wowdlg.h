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

/* ⚠ NO FORWARD DECLARATIONS HERE, unlike wowwin.h. That file is included BEFORE
     wowuser.h and has to declare what it borrows; this one is included AFTER, so
     the window table, `WowUserFindWindow` and `WowUserWindowProcedureOf` are already
     complete -- and re-declaring the struct would be a duplicate typedef. The
     include order in main.c is what makes that true; it is commented there. */

/*
 * ── ★★★ THE PUMP, WITH ITS MOUTH OPEN. (session 57, second run) ─────────────
 * The heartbeat refuted the obvious explanation -- `this thread 0x334, the
 * windows' thread 0x334`, so PeekMessage is looking at the right queue -- and
 * left a sharper question: SIX Win32 messages were dispatched across a
 * foreground change and a click on a real Cancel button, and NOT ONE of them
 * turned into a Win16 message. Those are two different facts and the totals
 * cannot tell them apart:
 *     the click never reached this queue          -> an input problem
 *     it arrived and WowWinProc did not post it  -> a translation problem
 * So the pump says what it dispatched, to which window, and whether that window
 * is one of ours. `h16 == 0` is the whole answer if it is 0.
 * ⚠ BOUNDED, and the bound is the instrument's own discipline: a modal dialog
 *   left up for a minute would otherwise write a line per mouse move. The first
 *   WOWDLG_TRACE messages of each wait are traced and the rest are counted.
 */
#define WOWDLG_TRACE 48
#define WOWDLG_TRACE_LINE_MAX 200
#define WOWDLG_WAIT_LINE_MAX  192
#define WOWDLG_BEAT_LINE_MAX  256
#define WOWDLG_CLASS_NAME_MAX 16
#define WOWDLG_PUMP_BUDGET    64     /* Win32 messages per pump              */
#define WOWDLG_WAIT_SLICE_MS  50
#define WOWDLG_FAST_BEATS     20     /* heartbeats every 2 s, then a minute  */
#define WOWDLG_FAST_BEAT_MS   2000
#define WOWDLG_SLOW_BEAT_MS   60000
#define WOWDLG_PROCEDURE_ARGUMENTS 5 /* hwnd, msg, wParam, lParam (2 words)  */

#endif /* NTVDMEX_WOWDLG_H */
