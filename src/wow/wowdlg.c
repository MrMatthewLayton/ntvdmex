/* wowdlg.c -- ★★★★★ THE MODAL MESSAGE LOOP. GH #128, session 57.
 *
 * The code of wowdlg.h (#335): its functions and state, in their original order;
 * its own translation unit, declared in wowdlg.h. */
#include "host_state.h"
#include "log.h"
#include "ne.h"
#include "wow32.h"
#include "wowanchors.h"
#include "wowsched.h"
#include "wowcall.h"
#include "wowmsg.h"
#include "wowres.h"
#include "wowwin.h"
#include "wowgdi.h"
#include "wowuser.h"
#include "wowdlg.h"
#include "host_wow.h"


/* Forward declarations, from when this file was part of main.c's unit (they were in wowdlg.h). */
/* Defined in main.c, which owns the LDT: is this code selector NOT PRESENT?
   See the call site, and WOWCALL_RETF_OFF in wowcall.h for what it decides. */
INT WowDlgIsSelectorAbsent(WORD selector);

/* s88: per callback depth -- did the modal loop's call go to the dialog's own
   DLGPROC, and with which message. Read when the call returns (main.c). */
INT  g_WowDlgIsDialogCall[WOWCALL_MAX_DEPTH];
WORD g_WowDlgMessage[WOWCALL_MAX_DEPTH];

static WOWDLG_MODAL g_WowDlgModals[WOWDLG_MAX_MODAL];
static INT   g_WowDlgDepth   = 0;
static DWORD g_WowDlgRan     = 0;   /* modal dialogs run to completion this run    */
static DWORD g_WowDlgRefused = 0;   /* ...and ones the host could not drive        */

static INT WowDlgPump(INT budget, PINT traceBudget)
{
    MSG message;
    INT count = 0;
    while (count < budget && PeekMessageA(&message, NULL, 0, 0, PM_REMOVE)) {
        if (traceBudget && *traceBudget > 0) {
            CHAR traceBuffer[WOWDLG_TRACE_LINE_MAX], *traceCursor = traceBuffer;
            WORD window16 = WowWinHwnd16(message.hwnd);
            --*traceBudget;
            traceCursor = LogPut(traceCursor, "       WOWDLG/win32: msg=0x"); traceCursor = LogHex(traceCursor, message.message);
            traceCursor = LogPut(traceCursor, " hwnd=0x");   traceCursor = LogHex(traceCursor, (DWORD)(ULONG_PTR)message.hwnd);
            traceCursor = LogPut(traceCursor, " wp=0x");     traceCursor = LogHex(traceCursor, (DWORD)message.wParam);
            traceCursor = LogPut(traceCursor, " lp=0x");     traceCursor = LogHex(traceCursor, (DWORD)message.lParam);
            traceCursor = LogPut(traceCursor, " -> win16 0x"); traceCursor = LogHex(traceCursor, window16);
            if (!window16) traceCursor = LogPut(traceCursor, " (★ NOT ONE OF OURS -- nothing will be"
                                    " posted for it)");
            traceCursor = LogPut(traceCursor, "\r\n");
            LogAppend(LOG_PATH, traceBuffer, traceCursor);
        }
        if (WowWinThreadTimerFire(&message)) { ++count; ++g_WowWinPumped; continue; }   /* s93 */
        /* #305 M11 (s91): THE DIALOG MANAGER'S KEYS. DialogBox's own loop gives a modal
             dialog Tab / Shift+Tab between its controls, Enter = the default button and
             Esc = IDCANCEL -- without the program asking. This loop dispatched keys raw,
             so Esc did nothing in TASKMAN's Task List (runs/s91/chain25). The controls are
             real windows, so the OS's IsDialogMessage does the work; what it generates
             (WM_COMMAND IDCANCEL/IDOK) reaches the dialog procedure through WowWinProc
             like a click does. Keyboard messages only, and only for the topmost modal
             dialog or one of its children. */
        if (message.message >= WM_KEYFIRST && message.message <= WM_KEYLAST && g_WowDlgDepth > 0) {
            HWND dialog = WowUserHwnd32(g_WowDlgModals[g_WowDlgDepth - 1].Window);
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
                ++count; ++g_WowWinPumped;
                continue;
            }
        }
        TranslateMessage(&message);
        DispatchMessageA(&message);
        ++count; ++g_WowWinPumped;
    }
    return count;
}

static PWOWDLG_MODAL WowDlgTop(VOID)
{
    return g_WowDlgDepth > 0 ? &g_WowDlgModals[g_WowDlgDepth - 1] : NULL;
}

/* Is any modal dialog up? Read by the DialogBox service, so that a nested one
   is described honestly in the trace rather than looking like the first. */
INT WowDlgActive(VOID)
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
INT WowDlgPush(WORD window, DWORD returnLinear, DWORD dialogProcedure, DWORD windowProcedure,
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
         its X included, so nothing reaches WowWinProc to be posted. The
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
VOID WowDlgSetInit(DWORD initParameter, WORD firstFocus)
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
INT WowDlgEnd(WORD window, WORD result)
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
    hole[0] = (BYTE)(value & BYTE_MASK);         hole[1] = (BYTE)((value >> BYTE_SHIFT)  & BYTE_MASK);
    hole[2] = (BYTE)((value >> WORD_SHIFT) & BYTE_MASK); hole[3] = (BYTE)((value >> TOP_BYTE_SHIFT) & BYTE_MASK);
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
INT WowDlgStep(volatile BYTE *tib, DWORD stackBase, WORD returnSelector,
                       const volatile LONG *running, PSTR note, INT noteCapacity)
{
    INT noteLength = 0;
    for (;;) {
        PWOWDLG_MODAL dialog = WowDlgTop();
        PWOWUSER_WINDOW window;
        WOWMSG message;
        DWORD procedure = 0;
        WORD  arguments[WOWDLG_PROCEDURE_ARGUMENTS];
        WORD  messageNumber, wParam;
        WORD  target;                      /* s93: the window the call is FOR */
        DWORD lParam;
        INT   isAbsent = 0;
        INT   verdict;

        if (!dialog) return 0;
        window = WowUserFindWindow(dialog->Window);
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
                                     window && window->Window32 && IsWindow(window->Window32),
                                     dialog->DialogProcedure || dialog->WindowProcedure, FALSE);

        /* ── EXIT 1: THE REAL ONE. EndDialog was called for this dialog. ───── */
        if (verdict == WOWCONV_MODAL_END) {
            ++g_WowDlgRan;
            WowNotePut(note, noteCapacity, &noteLength, "MODAL 0x");
            WowNoteHex(note, noteCapacity, &noteLength, dialog->Window, WOW_HEX_WORD_DIGITS);
            WowNotePut(note, noteCapacity, &noteLength, " ENDED -- DialogBox returns 0x");
            WowNoteHex(note, noteCapacity, &noteLength, dialog->Result, WOW_HEX_WORD_DIGITS);
            WowNotePut(note, noteCapacity, &noteLength, " after 0x");
            WowNoteHex(note, noteCapacity, &noteLength, dialog->Messages, WOW_HEX_WORD_DIGITS);
            WowNotePut(note, noteCapacity, &noteLength, " message(s), 0x");
            WowNoteHex(note, noteCapacity, &noteLength, GetTickCount() - dialog->StartTime, WOW_HEX_DWORD_DIGITS);
            WowNotePut(note, noteCapacity, &noteLength, " ms. ");
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
            WowNotePut(note, noteCapacity, &noteLength, "MODAL 0x");
            WowNoteHex(note, noteCapacity, &noteLength, dialog->Window, WOW_HEX_WORD_DIGITS);
            WowNotePut(note, noteCapacity, &noteLength, " -- ★ ITS WINDOW IS GONE (destroyed while"
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
            WowNotePut(note, noteCapacity, &noteLength, "MODAL 0x");
            WowNoteHex(note, noteCapacity, &noteLength, dialog->Window, WOW_HEX_WORD_DIGITS);
            WowNotePut(note, noteCapacity, &noteLength, " -- ★ NO DIALOG PROCEDURE AND NO CLASS WINDOW"
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
            WowNotePut(note, noteCapacity, &noteLength, "MODAL 0x");
            WowNoteHex(note, noteCapacity, &noteLength, dialog->Window, WOW_HEX_WORD_DIGITS);
            WowNotePut(note, noteCapacity, &noteLength, " -> WM_INITDIALOG ");
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
            if (dialog->IsShowDeferred && window->Window32) {
                dialog->IsShowDeferred = 0;
                ShowWindow(window->Window32, SW_SHOW);
                WowNotePut(note, noteCapacity, &noteLength, "MODAL 0x");
                WowNoteHex(note, noteCapacity, &noteLength, dialog->Window, WOW_HEX_WORD_DIGITS);
                WowNotePut(note, noteCapacity, &noteLength, " SHOWN (WM_INITDIALOG is done, so the"
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
                    if (!focus || focus == window->Window32 || !IsChild(window->Window32, focus)) {
                        HWND firstTabStop = GetNextDlgTabItem(window->Window32, NULL, FALSE);
                        if (firstTabStop) {
                            /* ⚠ NOT WM_NEXTDLGCTL: only the real DefDlgProc acts on
                                 it, and this window runs OUR procedure (see #32770
                                 in wowuser.h) -- measured, it did nothing. */
                            CHAR className[WOWDLG_CLASS_NAME_MAX];
                            SetFocus(firstTabStop);
                            if (GetClassNameA(firstTabStop, className, sizeof className) && !lstrcmpiA(className, "Edit"))
                                SendMessageA(firstTabStop, EM_SETSEL, 0, -1);
                            WowNotePut(note, noteCapacity, &noteLength, "Focus -> its first tab stop. ");
                        }
                    }
                }
            }
            /* ── ★★ WAIT FOR SOMETHING TO HAPPEN, WHICH IS WHAT MODAL MEANS.
                 The dialog's controls are real Win32 windows on this thread, so
                 a click on one becomes a Win32 message here, which WowWinProc
                 turns into a Win16 WM_COMMAND for the dialog. Pumping and
                 waiting is therefore the same statement as "wait for the user",
                 and it is the identical loop a blocked GetMessage already runs
                 -- see main.c. Sharing the KNOB (wowidle.txt) matters more than
                 sharing the code: a modal dialog and an idle message loop are
                 the same kind of wait and a user who set one meant both. */
            DWORD startTime = GetTickCount(), lastBeat = startTime;
            DWORD pumpedAtStart = g_WowWinPumped;
            UINT beatCount = 0;
            /* ── ★★ AND IT SAYS SO BEFORE IT BLOCKS, NOT AFTER. ───────────────
                 A log that goes silent at the moment a modal dialog appears is
                 indistinguishable from a host that died there, and this project
                 has already read one that way twice (the MessageBox arm in
                 main.c carries the same note for the same reason). So the line
                 goes out FIRST, and a heartbeat follows it, bounded so that a
                 dialog left up overnight cannot fill the disk. */
            {   CHAR waitBuffer[WOWDLG_WAIT_LINE_MAX], *waitCursor = waitBuffer;
                waitCursor = LogPut(waitCursor, "     WOWDLG: modal 0x"); waitCursor = LogHex(waitCursor, dialog->Window);
                waitCursor = LogPut(waitCursor, " is WAITING for input -- the guest is parked"
                              " inside DialogBox on purpose, ");
                if (g_WowMsgWaitMs) { waitCursor = LogPut(waitCursor, "for at most 0x");
                                        waitCursor = LogHex(waitCursor, g_WowMsgWaitMs);
                                        waitCursor = LogPut(waitCursor, " ms"); }
                else                    waitCursor = LogPut(waitCursor, "for as long as it takes"
                                                      " (wowidle.txt = 0)");
                waitCursor = LogPut(waitCursor, "\r\n");
                LogAppend(LOG_PATH, waitBuffer, waitCursor);
            }
            g_WowMsgInWait = 1;
            while ((!running || *running) && !g_WowMsgCount
                   && (!g_WowMsgWaitMs || GetTickCount() - startTime < g_WowMsgWaitMs)) {
                if (!WowDlgPump(WOWDLG_PUMP_BUDGET, &dialog->TraceBudget))
                    MsgWaitForMultipleObjects(0, NULL, FALSE, WOWDLG_WAIT_SLICE_MS, QS_ALLINPUT);
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
                {   DWORD interval = (beatCount < WOWDLG_FAST_BEATS) ? WOWDLG_FAST_BEAT_MS : WOWDLG_SLOW_BEAT_MS;
                    if (GetTickCount() - lastBeat >= interval) {
                        CHAR beatBuffer[WOWDLG_BEAT_LINE_MAX], *beatCursor = beatBuffer;
                        lastBeat = GetTickCount(); ++beatCount;
                        beatCursor = LogPut(beatCursor, "     WOWDLG: modal 0x"); beatCursor = LogHex(beatCursor, dialog->Window);
                        beatCursor = LogPut(beatCursor, " waiting 0x");   beatCursor = LogHex(beatCursor, lastBeat - startTime);
                        beatCursor = LogPut(beatCursor, " ms; pumped 0x"); beatCursor = LogHex(beatCursor, g_WowWinPumped);
                        beatCursor = LogPut(beatCursor, " (+0x");         beatCursor = LogHex(beatCursor, g_WowWinPumped - pumpedAtStart);
                        beatCursor = LogPut(beatCursor, " since blocking), Win16 queued 0x");
                        beatCursor = LogHex(beatCursor, (DWORD)g_WowMsgCount);
                        beatCursor = LogPut(beatCursor, "; queue status 0x");
                        beatCursor = LogHex(beatCursor, GetQueueStatus(QS_ALLINPUT));
                        beatCursor = LogPut(beatCursor, "; this thread 0x");
                        beatCursor = LogHex(beatCursor, GetCurrentThreadId());
                        beatCursor = LogPut(beatCursor, ", the windows' thread 0x");
                        beatCursor = LogHex(beatCursor, g_WowWinThread);
                        if (g_WowWinThread && g_WowWinThread != GetCurrentThreadId())
                            beatCursor = LogPut(beatCursor, " -- ★★ DIFFERENT: PeekMessage is"
                                          " PER-THREAD, so this loop can never see"
                                          " their input");
                        beatCursor = LogPut(beatCursor, "\r\n");
                        LogAppend(LOG_PATH, beatBuffer, beatCursor);
                    }
                }
                /* ⚠ AND WATCH THE DIALOG ITSELF, NOT ONLY THE QUEUE. A dialog
                     dismissed by its own procedure during a message we
                     dispatched INSIDE this pump (a WM_COMMAND handled by a
                     nested SendMessage, say) is over, and so is one whose
                     window the OS has destroyed. Either way there is nothing
                     left to wait for, and the checks at the top of the loop are
                     the ones that say so -- this just stops waiting. */
                if (dialog->IsEnded || !IsWindow(window->Window32)) break;
            }
            g_WowMsgInWait = 0;
            if (dialog->IsEnded || !IsWindow(window->Window32)) continue;

            /* ── EXIT 4: THE WAIT EXPIRED. Only reachable with a bounded
                 wowidle.txt, which is the unattended-harness setting; an
                 interactive session waits forever and never gets here. The same
                 tested decision as the three above, asked again now that the
                 fourth fact is known. */
            if (!WowMsgTake(0, 0, 0, PM_REMOVE, &message)) {
                /* ⚠ AND ONLY IF NOTHING ELSE ENDED IT WHILE WE WAITED. The
                     answer for a dialog that was dismissed during the wait is
                     EndDialog's result, not 0 -- so anything but EXPIRED goes
                     back to the top of the loop, where that case is handled. */
                if (WowConvModalExit(dialog->IsEnded, TRUE, TRUE, TRUE)
                        != WOWCONV_MODAL_EXPIRED)
                    continue;
                ++g_WowDlgRefused;
                WowNotePut(note, noteCapacity, &noteLength, "MODAL 0x");
                WowNoteHex(note, noteCapacity, &noteLength, dialog->Window, WOW_HEX_WORD_DIGITS);
                WowNotePut(note, noteCapacity, &noteLength, " -- the host's input wait expired with an"
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
                PWOWUSER_WINDOW targetWindow = message.Window ? WowUserFindWindow(message.Window) : NULL;
                DWORD targetProcedure = targetWindow ? WowUserWindowProcedureOf(targetWindow) : 0;
                if (!targetProcedure) {
                    ++dialog->Messages;
                    continue;            /* nowhere to put it; take the next one */
                }
                procedure = targetProcedure;
                target  = message.Window;
            }
            WowNotePut(note, noteCapacity, &noteLength, "MODAL 0x");
            WowNoteHex(note, noteCapacity, &noteLength, dialog->Window, WOW_HEX_WORD_DIGITS);
            WowNotePut(note, noteCapacity, &noteLength, " -> hwnd=0x");
            WowNoteHex(note, noteCapacity, &noteLength, message.Window, WOW_HEX_WORD_DIGITS);
            WowNotePut(note, noteCapacity, &noteLength, " msg=0x");
            WowNoteHex(note, noteCapacity, &noteLength, messageNumber, WOW_HEX_WORD_DIGITS);
            WowNotePut(note, noteCapacity, &noteLength, " ");
        }

        /* ── THE CALL ITSELF. Five words in declared order, the shape every
             window and dialog procedure takes -- WowUserWantMessage builds the
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
        arguments[3] = (WORD)(lParam >> WORD_SHIFT);
        arguments[4] = (WORD)(lParam & WORD_MASK);

        /* Is the procedure's code segment loaded? Not present means we must go
           in through the RETF trampoline so krnl386's own #NP handler loads it
           -- writing that selector into CS kills the VDM silently, which cost a
           session on CARDFILE. See WOWCALL_RETF_OFF in wowcall.h.
           ⚠ ASKED, NOT COMPUTED HERE: the LDT is the host's, and this file has
             no business reaching into it. main.c answers. */
        isAbsent = WowDlgIsSelectorAbsent((WORD)(procedure >> WORD_SHIFT));

        if (!returnSelector || !stackBase
            || !WowCallEnter(tib, stackBase, returnSelector, procedure, dialog->DataSelector, arguments, WOWDLG_PROCEDURE_ARGUMENTS,
                              /* returnLinear */ 0, WOWCALL_RET_KEEP, NULL,
                              target, messageNumber, NULL, 0, -1, isAbsent)) {
            /* The call could not be made -- depth, or no return selector. That
               is not a reason to spin: without a call there is no EndDialog. */
            ++g_WowDlgRefused;
            WowNotePut(note, noteCapacity, &noteLength, "-- ★ THE CALL WAS REFUSED (depth, or no return"
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
            g_WowUserDlgDefaults[g_WowCallDepth - 1].WParam = wParam;
            g_WowUserDlgDefaults[g_WowCallDepth - 1].LParam = lParam;
        }
        ++dialog->Messages;
        WowNotePut(note, noteCapacity, &noteLength, "-> 0x");
        WowNoteHex(note, noteCapacity, &noteLength, procedure >> WORD_SHIFT, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, ":0x");
        WowNoteHex(note, noteCapacity, &noteLength, procedure & WORD_MASK, WOW_HEX_WORD_DIGITS);
        if (isAbsent) WowNotePut(note, noteCapacity, &noteLength, " [segment not present -- via the RETF"
                                           " trampoline]");
        return 1;
    }
}
