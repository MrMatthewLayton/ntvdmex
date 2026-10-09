/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * A Win16 WINDOW IS A REAL Win32 WINDOW. GH #128, session 42.
 *
 * The code of wowwin.h (#335): its functions and state, in their original order;
 * its own translation unit, declared in wowwin.h.
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

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
#include "wowenum.h"
#include "wowshell.h"
#include "wowcommdlg.h"
#include "host_dpmi.h"

/* Forward declarations, from when this file was part of main.c's unit (they were in wowwin.h). */
WORD  WowWinHwnd16(HWND window);
PWOWUSER_WINDOW WowUserFindWindow(WORD window16);
INT   WowUserIsMdiChild(PCWOWUSER_WINDOW window);
HWND  WowUserMdiClientOf(PCWOWUSER_WINDOW window);
HWND  WowUserHwnd32(WORD window16);
WORD  WowUserMenu16(HMENU menu);  /* the 16-bit name for a real menu */
/* #294: COMMDLG's modeless Find/Replace dialogs -- wowcommdlg.h, included later. */
INT   WowCdlgRelay(UINT message, LPARAM lParam);
INT   WowCdlgIsDialogMessage(PMSG message);
DWORD WowUserTimerProcedure(WORD window16, WORD timerId);  /* 0 if none installed */

INT WowUserIsDialog16(WORD h16);          /* wowuser.h: a dialog procedure? */

/* Set once the exec thread has a window: the thread id that owns them all, so a
 * pump on the wrong thread can be refused rather than silently doing nothing.
 */
DWORD g_WowWinThread = 0;
DWORD g_WowWinCreated = 0, g_WowWinMessages = 0;
/* #160: menus held back until the guest set them up, and the replay's re-entry flag. */
static DWORD g_WowWinMenuDeferred = 0;
static INT   g_WowWinIsReplaying = 0;
/* Times Alt/F10 had to take the mouse capture off a guest window so the menu
 * could open. Non-zero is normal for a paint program; zero on a session where
 * the menu is dead means the cause is something else.
 */
static DWORD g_WowWinMenuUncaptures = 0;
/* Win32 messages this thread has dispatched for the guest's windows. The answer to
 * "is the window hung", which cannot be read off anything else.
 */
DWORD g_WowWinPumped = 0;

static WOWWIN_PAINT g_WowWinPaints[WOWWIN_MAXPAINT];
/* Tick at which the most recent WM_PAINT was posted to the Win16 queue. */
DWORD g_WowWinPaintMs = 0;

static VOID WowWinPaintWant(WORD window16, const RECT *rect, INT isErase)
{
    INT index, freeSlot = -1;

    for (index = 0; index < WOWWIN_MAXPAINT; ++index)
    {
        if (g_WowWinPaints[index].IsPending && g_WowWinPaints[index].Window16 == window16)
        {
            if (rect->left   < g_WowWinPaints[index].Rect.left)
                g_WowWinPaints[index].Rect.left   = rect->left;
            if (rect->top    < g_WowWinPaints[index].Rect.top)
                g_WowWinPaints[index].Rect.top    = rect->top;
            if (rect->right  > g_WowWinPaints[index].Rect.right)
                g_WowWinPaints[index].Rect.right  = rect->right;
            if (rect->bottom > g_WowWinPaints[index].Rect.bottom)
                g_WowWinPaints[index].Rect.bottom = rect->bottom;
            if (isErase)
                g_WowWinPaints[index].IsErase = 1;
            return;
        }
        if (!g_WowWinPaints[index].IsPending && freeSlot < 0)
            freeSlot = index;
    }
    if (freeSlot < 0)
        return;                  /* full: the guest still gets the
                                              message, just no rectangle */
    g_WowWinPaints[freeSlot].Window16 = window16;
    g_WowWinPaints[freeSlot].Rect = *rect;
    g_WowWinPaints[freeSlot].IsErase = isErase;
    g_WowWinPaints[freeSlot].IsPending = 1;
}

/* Take the pending rectangle for a window, or 0 if there is none. */
INT WowWinPaintTake(WORD window16, PRECT output, PINT isErase)
{
    INT index;

    for (index = 0; index < WOWWIN_MAXPAINT; ++index)
        if (g_WowWinPaints[index].IsPending && g_WowWinPaints[index].Window16 == window16)
        {
            *output = g_WowWinPaints[index].Rect;
            if (isErase)
                *isErase = g_WowWinPaints[index].IsErase;
            g_WowWinPaints[index].IsPending = 0;
            return 1;
        }
    return 0;
}

/* -- MSG.pt IS THE CURSOR IN *SCREEN* COORDINATES, AND IT WAS ALWAYS 0,0.
 * Every WowMsgPost here passed `0, 0` for it, because nothing this host had
 * watched read the field -- wowmsg.h says exactly that, and says it was filled
 * with the cursor position, which it was not.
 *
 * [INFO]: MINESWEEPER'S SMILEY IS THE PROGRAM THAT READS IT. Pressing the face does
 * SetCapture and then converts the button's rectangle to SCREEN coordinates
 * (two ClientToScreen calls, 0x385..0x39d = 901..925 on this display). Its
 * tracking loop then hit-tests the release against that rectangle using the
 * message's OWN `pt` -- and (0,0) is outside it, so the game concluded the
 * button was released off the face and correctly declined to start a new one.
 * The release WAS delivered and the code WAS reached; the message was just
 * carrying a position no cursor has ever been at.
 *
 * [CAUTION]: SCREEN, NOT CLIENT. lParam already carries the client point; `pt` is the
 * other one, and filling it from lParam would be a plausible-looking value
 * that fails the same test.
 */
/* #162: A MESSAGE THE GUEST JUST TOOK MUST NOT BE HANDED BACK TO IT:
 * USER's IsDialogMessage (wowuser.h) lets the REAL dialog manager see the guest's
 * message, and that manager DISPATCHES what it does not use -- to the real window,
 * i.e. to this procedure, which relays it to the guest queue. For the very message
 * the guest had just taken out of that queue, that is a loop: Charmap's main window
 * is a dialog, and its WM_PAINT/WM_SETFOCUS pair went round ~4,000 times a
 * millisecond, as did the X button's WM_CLOSE (~99,000 in ten seconds), and the
 * program never saw any of them. While IsDialogMessage runs, the SAME message for the
 * SAME window is recorded as bounced and not relayed; IsDialogMessage then answers
 * FALSE, so the guest's loop dispatches it to its own 16-bit procedure, which is where
 * it belongs. What the dialog manager GENERATES (Enter -> WM_COMMAND for the default
 * button, focus moving to a real control) still flows as before.
 */
INT  g_WowWinInDialogMessage, g_WowWinIsDialogBounced;
HWND g_WowWinDialogWindow;
UINT g_WowWinDialogMessage;

/* s89 (#162): MESSAGES WINDOWS SENDS AND NEEDS AN ANSWER TO, NOW:
 * WM_CTLCOLOR* arrives from inside a control's paint and wants a brush back
 * before the control can draw. Answering it means running the 16-bit window
 * procedure SYNCHRONOUSLY -- the nested run (main.c, wow_call16_sync). main.c
 * wires this hook; NULL (or a refusal) leaves Windows' own default.
 */
LRESULT (*g_WowWinCtlColor)(HWND window, WORD window16, UINT message, WPARAM wParam, LPARAM lParam,
                                PINT isHandled);
/* s89 (#300): any message SENT to a guest window now, through the same nested run
 * (main.c: wow_send16_now). 0 = it could not run; the caller then posts.
 */
INT (*g_WowWinSend16)(WORD window16, WORD message, WORD wParam, DWORD lParam, PWORD result);
/* s89 (#302): owner-draw (WM_DRAWITEM/MEASUREITEM/DELETEITEM/COMPAREITEM), the
 * structures converted and the program asked through the nested run (main.c).
 */
LRESULT (*g_WowWinOwnerDraw)(HWND window, WORD window16, UINT message, WPARAM wParam, LPARAM lParam,
                                 PINT isHandled);

static UINT g_WowWinMultimediaLogged;   /* s90: first MM notifications logged */
/* s92 (#305 M12): krnl386's OWN global heap, through the nested run (main.c:
 * shim_global16 -- 0 GlobalAlloc(flags, cb) 1 GlobalFree 2 GlobalLock -> 16:16
 * 3 GlobalUnlock). NULL until main.c wires it.
 */
DWORD (*g_WowWinGlobal16)(INT operation, DWORD first, DWORD second);

/* s92 (#305 M12): WM_DROPFILES -- A WIN16 HDROP IS A REAL GLOBAL BLOCK:
 * DragQueryPoint (SHELL ord 13) and DragFinish (ord 12) never reach us -- they
 * run in 16-bit SHELL.DLL against the handle as a global block (point, then
 * fNC; DragFinish frees it). Only DragQueryFile (ord 11) thunks to us. So the
 * drop is copied, whole, into a block krnl386 allocates, laid out as Windows
 * 3.1's DROPFILESTRUCT:
 *   +0 WORD pFiles (= 8)   +2 POINT pt (client)   +6 WORD fNC   +8 the names,
 *   each NUL-terminated, the list ended by an empty one.
 * The names are the SHORT (8.3) forms -- a Win16 program opens what it is given
 * through DOS. The Win32 HDROP is finished here; the guest's DragFinish frees ours.
 * Returns the 16-bit handle, or 0 (nothing posted).
 */
static WORD WowWinDrop16(HDROP drop, PSTR reason, INT reasonCapacity)
{
    BYTE buffer[WOWWIN_DROP_BUFFER];
    UINT count, index, offset = WOWWIN_DROPFILES_HEADER;
    POINT point;
    BOOL isInside;
    WORD handle16;
    DWORD farPointer;
    volatile BYTE *bytes;

    if (!g_WowWinGlobal16)
    {
        lstrcpynA(reason, "no 16-bit heap entry", reasonCapacity);
        return 0;
    }
    count = DragQueryFileA(drop, WOWWIN_DRAGQUERY_COUNT, NULL, 0);
    isInside = DragQueryPoint(drop, &point);
    for (index = 0; index < count; ++index)
    {
        CHAR longPath[MAX_PATH], shortPath[MAX_PATH];
        INT  length;
        if (!DragQueryFileA(drop, index, longPath, sizeof longPath))
            continue;
        if (!GetShortPathNameA(longPath, shortPath, sizeof shortPath))
            lstrcpynA(shortPath, longPath, sizeof shortPath);
        length = lstrlenA(shortPath);
        if (offset + (UINT)length + WOWWIN_DROP_TERMINATORS > sizeof buffer)
            break;                                                                        /* a 2 KB list; the rest dropped */
        CopyMemory(buffer + offset, shortPath, (SIZE_T)length + 1);
        offset += (UINT)length + 1;
    }
    buffer[offset++] = 0;
    buffer[0] = WOWWIN_DROPFILES_HEADER;
    buffer[1] = 0;
    buffer[WOWWIN_DROPFILES_POINT_X] = (BYTE)point.x;
    buffer[WOWWIN_DROPFILES_POINT_X + 1] = (BYTE)((WORD)point.x >> BYTE_SHIFT);
    buffer[WOWWIN_DROPFILES_POINT_Y] = (BYTE)point.y;
    buffer[WOWWIN_DROPFILES_POINT_Y + 1] = (BYTE)((WORD)point.y >> BYTE_SHIFT);
    buffer[WOWWIN_DROPFILES_NONCLIENT] = (BYTE)(isInside ? 0 : 1);
    buffer[WOWWIN_DROPFILES_NONCLIENT + 1] = 0;
    handle16 = (WORD)g_WowWinGlobal16(WOWWIN_GLOBAL16_ALLOC, WOWWIN_GMEM_SHARE_MOVEABLE_ZEROINIT, offset);
    if (!handle16)
    {
        lstrcpynA(reason, "GlobalAlloc refused", reasonCapacity);
        return 0;
    }
    farPointer = g_WowWinGlobal16(WOWWIN_GLOBAL16_LOCK, handle16, 0);
    bytes = (farPointer >> WORD_SHIFT) ? (volatile BYTE *)(ULONG_PTR)(DpmiSelectorBase((WORD)(farPointer >> WORD_SHIFT)) + (farPointer & WORD_MASK)) : NULL;
    if (!bytes || !(farPointer >> WORD_SHIFT) || !DpmiSelectorBase((WORD)(farPointer >> WORD_SHIFT)))
    {
        g_WowWinGlobal16(WOWWIN_GLOBAL16_FREE, handle16, 0);
        lstrcpynA(reason, "GlobalLock refused", reasonCapacity);
        return 0;
    }
    for (index = 0; index < offset; ++index)
        bytes[index] = buffer[index];
    g_WowWinGlobal16(WOWWIN_GLOBAL16_UNLOCK, handle16, 0);
    wsprintfA(reason, "%u file(s), %u bytes, pt=(%d,%d)%s", count, offset, (INT)point.x, (INT)point.y,
              isInside ? "" : " non-client");
    return handle16;
}

/* s91 (#305 M9): a message with a STRUCTURE, sent now (main.c: wow_send16_blob) --
 * WM_GETMINMAXINFO's 16-bit MINMAXINFO, copied back. 0 = it could not run.
 */
INT (*g_WowWinSend16Blob)(WORD window16, WORD message, WORD wParam, PBYTE blob, INT blobLength,
                           const INT *fixups, INT fixupCount, PWORD result);

static WOWWIN_SENDING g_WowWinSending[WOWWIN_MAX_SENDING];
static INT g_WowWinSendingCount;
/* s92 (#289): inside USER32's move/size loop (WM_ENTERSIZEMOVE..WM_EXITSIZEMOVE).
 * The guest cannot run there, so a POSTED WM_PAINT waits for the mouse to come up
 * and the vacated areas are never erased -- Packager's panes left a trail of
 * scrollbars across the window. Win16's size loop dispatches WM_PAINT as it goes,
 * so inside it the relay SENDS the paint (see WM_PAINT below).
 */
static INT g_WowWinSizeMove;
static UINT g_WowWinPaintsLogged;

static WOWWIN_HELD_CHAR g_WowWinHeldChars[WOWWIN_MAX_HELD_CHARS];
static DWORD g_WowWinHeldCharSequence;
static VOID WowWinHoldChar(WORD window16, WORD character, DWORD lParam)
{
    INT index, oldest = 0;

    for (index = 0; index < WOWWIN_MAX_HELD_CHARS; ++index)
    {
        if (!g_WowWinHeldChars[index].Sequence)
        {
            oldest = index;
            break;
        }
        if (g_WowWinHeldChars[index].Sequence < g_WowWinHeldChars[oldest].Sequence)
            oldest = index;
    }
    g_WowWinHeldChars[oldest].Window16 = window16;
    g_WowWinHeldChars[oldest].Character = character;
    g_WowWinHeldChars[oldest].LParam = lParam;
    g_WowWinHeldChars[oldest].Sequence = ++g_WowWinHeldCharSequence;
}

/* Post, oldest first, every held character for this key-down; returns how many. */
INT WowWinReleaseChars(WORD window16, DWORD keyLParam)
{
    INT count = 0;

    for (;;)
    {
        INT index, best = -1;
        for (index = 0; index < WOWWIN_MAX_HELD_CHARS; ++index)
            if (g_WowWinHeldChars[index].Sequence && g_WowWinHeldChars[index].Window16 == window16
                && ((g_WowWinHeldChars[index].LParam >> WOWWIN_SCAN_CODE_SHIFT) & WOWWIN_SCAN_CODE_MASK) == ((keyLParam >> WOWWIN_SCAN_CODE_SHIFT) & WOWWIN_SCAN_CODE_MASK)
                && (best < 0 || g_WowWinHeldChars[index].Sequence < g_WowWinHeldChars[best].Sequence))
                best = index;
        if (best < 0)
            return count;
        WowMsgPost(window16, WM_CHAR16, g_WowWinHeldChars[best].Character, g_WowWinHeldChars[best].LParam,
                    GetTickCount(), 0, 0);
        g_WowWinHeldChars[best].Sequence = 0;
        ++count;
    }
}

static WOWWIN_THREAD_TIMER g_WowWinThreadTimers[WOWWIN_MAX_THREAD_TIMERS];
INT WowWinThreadTimerAdd(UINT_PTR id32, DWORD procedure)
{
    INT index;

    for (index = 0; index < WOWWIN_MAX_THREAD_TIMERS; ++index)
        if (!g_WowWinThreadTimers[index].Id32 || g_WowWinThreadTimers[index].Id32 == id32)
        {
            g_WowWinThreadTimers[index].Id32 = id32;
            g_WowWinThreadTimers[index].Procedure = procedure;
            return 1;
        }
    return 0;
}

INT WowWinThreadTimerKill(UINT_PTR id32)
{
    INT index;

    for (index = 0; index < WOWWIN_MAX_THREAD_TIMERS; ++index)
        if (g_WowWinThreadTimers[index].Id32 == id32)
        {
            g_WowWinThreadTimers[index].Id32 = 0;
            g_WowWinThreadTimers[index].Procedure = 0;
            return 1;
        }
    return 0;
}

/* A Win32 thread WM_TIMER: 1 if it was one of ours (and is now queued for the guest). */
INT WowWinThreadTimerFire(const MSG *message)
{
    INT index;

    if (message->message != WM_TIMER || message->hwnd)
        return 0;
    for (index = 0; index < WOWWIN_MAX_THREAD_TIMERS; ++index)
        if (g_WowWinThreadTimers[index].Id32 && g_WowWinThreadTimers[index].Id32 == message->wParam)
        {
            /* one pending per timer, as Windows coalesces them */
            WowMsgPostMove(0, WM_TIMER16, (WORD)message->wParam, g_WowWinThreadTimers[index].Procedure, message->time, 0, 0)
                || WowMsgPost(0, WM_TIMER16, (WORD)message->wParam, g_WowWinThreadTimers[index].Procedure, message->time, 0, 0);
            return 1;
        }
    return 0;
}

/* s93: the guest's SetFocus calls, counted, and the real window of the last one --
 * so WM_ACTIVATE can tell that the program placed the focus itself (wowuser.h).
 */
UINT g_WowWinSetFocusCount;
HWND     g_WowWinSetFocusWindow;

static VOID WowWinSendOrPost(
    WORD window16,
    WORD message,
    WORD wParam,
    DWORD lParam,
    WORD pointX,
    WORD pointY)
{
    WORD result;
    INT index, busyCount = 0;
    /* s92 (#284): ONE nested WM_SIZE is Windows' own order. Cardfile's card hides its
     * scrollbars inside its WM_SIZE; the client grows by the scrollbar's width and
     * Windows sends WM_SIZE again, nested. Posted, it arrived after the first paint and
     * the card was laid out twice -- the first card's header stayed on screen. A
     * second level is allowed; deeper is the MDI loop above, and is still posted.
     */
    INT limit = (message == WM_SIZE) ? WOWWIN_SIZE_REENTRY : 1;

    for (index = 0; index < g_WowWinSendingCount; ++index)
        if (g_WowWinSending[index].Window16 == window16 && g_WowWinSending[index].Message == message)
            ++busyCount;
    if (busyCount < limit && g_WowWinSend16 && g_WowWinSendingCount < WOWWIN_MAX_SENDING)
    {
        INT isSent;
        g_WowWinSending[g_WowWinSendingCount].Window16 = window16;
        g_WowWinSending[g_WowWinSendingCount].Message = message;
        ++g_WowWinSendingCount;
        isSent = g_WowWinSend16(window16, message, wParam, lParam, &result);
        --g_WowWinSendingCount;
        if (isSent)
            return;
    }
    WowMsgPost(window16, message, wParam, lParam, GetTickCount(), pointX, pointY);
}

/* The default procedure this window needs -- frame, MDI child, or plain; see the
 * note at the end of WowWinProc.
 */
static LRESULT WowWinDefProc(HWND window, WORD window16, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (window16)
    {
        PWOWUSER_WINDOW record = WowUserFindWindow(window16);
        if (record)
        {
            HWND client = WowUserMdiClientOf(record);
            if (client)
                return DefFrameProcA(window, client, message, wParam, lParam);
            if (WowUserIsMdiChild(record))
                return DefMDIChildProcA(window, message, wParam, lParam);
        }
    }
    return DefWindowProcA(window, message, wParam, lParam);
}

static LRESULT CALLBACK WowWinProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    WORD window16 = WowWinHwnd16(window);
    POINT cursor;
    WORD  pointX, pointY;

    if (g_WowWinInDialogMessage && window == g_WowWinDialogWindow && message == g_WowWinDialogMessage)
    {
        g_WowWinIsDialogBounced = 1;
        return 0;
    }
    GetCursorPos(&cursor);
    pointX = (WORD)(SHORT)cursor.x;
    pointY = (WORD)(SHORT)cursor.y;
    switch (message)
    {
    case WM_CHAR:
        if (window16)
        {
            WowWinHoldChar(window16, (WORD)wParam, (DWORD)lParam);
            return 0;
        }
        break;

    case WM_KEYDOWN:
    case WM_KEYUP:
        /* [INFO]: RELAYED VERBATIM. Win16 and Win32 agree on the message number, on
         * wParam being the virtual key, and on the lParam bit field -- Win32
         * inherited all three -- so the honest thing is to hand across exactly
         * what the OS handed us rather than compose anything.
         */
        if (window16)
        {
            WowMsgPost(window16, (WORD)message, (WORD)wParam, (DWORD)lParam, GetTickCount(), pointX, pointY);
            ++g_WowWinMessages;
            return 0;
        }
        break;

    /* -- THE SYSTEM KEYS ARE THE SYSTEM'S, AND SWALLOWING THEM BROKE THE MENU.
     * (session 44) These were in the case above, relayed to the guest and then
     * returned as HANDLED -- so DefWindowProc never saw them. But "Sys" in
     * WM_SYSKEYDOWN means exactly *"this key belongs to the system"*: it is the
     * message Alt arrives in, and DefWindowProc's response to Alt is TO OPEN THE
     * MENU BAR. With it swallowed, an application could have a perfect menu and
     * no keyboard would ever reach it -- Alt did nothing, so Alt+H, Alt+F4 and
     * F10 did nothing either. Win16's own DefWindowProc does the same job, so
     * handing these to the real one is not a Win32 concession; it is the same
     * behaviour, implemented by the OS we are already running on.
     *
     * [CAUTION]: STILL POSTED TO THE GUEST AS WELL, because a Win16 program may look at
     * WM_SYSKEYDOWN before passing it on, and this host cannot ask it whether it
     * did. The duplication is the price of an asynchronous queue and is written
     * down rather than left to be discovered: an application that ACTS on a
     * system key will see the OS act too. Nothing measured does.
     */
    case WM_SYSKEYDOWN:
    case WM_SYSKEYUP:
        /* -- AND ALT MUST TAKE THE MOUSE CAPTURE, OR THE MENU NEVER OPENS.
         * (session 53, the "Alt stops working after a few canvas drags" defect)
         * MS Paint takes the capture on button-down and DOES NOT GIVE IT BACK --
         * measured, nine SetCapture and zero ReleaseCapture in one session, and
         * it re-takes it after every button-up. That is legal Win16: capture was
         * the app's to hold. But USER32 REFUSES SC_KEYMENU WHILE THE CALLING
         * THREAD HOLDS A CAPTURE -- menus deliberately do not open mid-drag --
         * so DefWindowProc did nothing with Alt and the whole menu bar went dead
         * for the rest of the session.
         *
         * [INFO]: STOCK ntvdm WAS MEASURED DOING EXACTLY THIS. Same program, same three
         * drags, cross-process GetGUIThreadInfo (`rigshot capture`):
         *   after the drags   stock hwndCapture = the CANVAS   (same as ours)
         *   after Alt         stock hwndCapture = the TOP-LEVEL, menuowner set,
         *                     GUI_INMENUMODE   (ours: unchanged, no menu)
         * So stock ALSO leaves it held, and Alt moves it. This is not us being
         * unfaithful by releasing it -- it is us reproducing what the reference
         * implementation is observed to do.
         *
         * [CAUTION]: WHY STOCK GETS IT FREE AND WE DO NOT: its thread reports
         * GUI_16BITTASK (flags 0x20) and ours reports 0. USER32 knows stock's
         * thread is a WOW task and gives it WOW-specific menu handling; that
         * flag is internal to USER32's WOW support and there is no supported way
         * for us to set it. Reproducing the BEHAVIOUR is the available route.
         *
         * [CAUTION]: ONLY ON ALT/F10 GOING DOWN, and only when one of OUR windows holds
         * it. Releasing on the way up would fight the menu loop for the capture
         * it has just taken, and releasing a capture that belongs to some other
         * application would be reaching outside this VDM entirely.
         */
        if (message == WM_SYSKEYDOWN && (wParam == VK_MENU || wParam == VK_F10))
        {
            HWND capture = GetCapture();
            if (capture && WowWinHwnd16(capture))
            {
                ReleaseCapture();
                ++g_WowWinMenuUncaptures;
            }
        }
        if (window16)
        {
            WowMsgPost(window16, (WORD)message, (WORD)wParam, (DWORD)lParam, GetTickCount(), pointX, pointY);
            ++g_WowWinMessages;
        }
        break;

    /* -- WM_PAINT, WHICH THIS FILE SAID IT WOULD RELAY "THE DAY" GDI'S ID
     * SPACE WAS DISPATCHED. That day is session 45: GDI is anchored, USER's
     * GetDC issues real device contexts, and MS Paint's window is on screen
     * and empty because nothing has ever asked it to draw.
     *
     * [CAUTION]: THE REGION IS VALIDATED HERE, AND NOT DOING SO IS A LIVE-LOCK. Win32
     * does not queue WM_PAINT -- it SYNTHESISES one for as long as the window
     * has an update region. Relaying it to the guest and returning 0 leaves
     * that region dirty, so the real pump hands us another WM_PAINT
     * immediately and the host spins at 100% delivering paints the guest never
     * gets a turn to answer. So the OS's BeginPaint/EndPaint pair runs here:
     * it erases the background and clears the region, which stops the storm,
     * and the rectangle it reports is carried to the guest.
     *
     * [CAUTION]: WHAT THAT COSTS, STATED RATHER THAN DISCOVERED: the guest's own
     * BeginPaint can no longer inherit a real update region, because this
     * already consumed it. The rectangle is therefore remembered per window
     * and handed back when the guest asks -- see the paint record below. The
     * guest's BeginPaint DC is clipped to that rectangle (#287, wowuser.h), as
     * a Win16 paint DC is; only a true non-rectangular update region is lost.
     */
    /* -- s89 (#162, Clock): THE GUEST ERASES FIRST. In Win16 WM_ERASEBKGND goes to
     * the window's procedure, and the class brush is only DefWindowProc's
     * answer for a procedure that passes it on. Letting the OS erase here
     * painted Clock WHITE (its class brush) before Clock -- which fills the
     * button face itself in that handler -- got a turn, and told BeginPaint the
     * background was done. Returning 0 leaves fErase set: the guest's
     * BeginPaint sends it WM_ERASEBKGND (wowuser.h), and DefWindowProc16
     * forwards it to us with its DC (0x6b), where the class brush is applied.
     */
    case WM_ERASEBKGND:
        if (window16)
            return 0;
        break;

    case WM_CTLCOLORMSGBOX:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
    case WM_CTLCOLORBTN:
    case WM_CTLCOLORDLG:
    case WM_CTLCOLORSCROLLBAR:
    case WM_CTLCOLORSTATIC:
        if (window16 && g_WowWinCtlColor)
        {
            INT isHandled = 0;
            LRESULT result = g_WowWinCtlColor(window, window16, message, wParam, lParam, &isHandled);
            if (isHandled)
                return result;
        }
        break;

    case WM_PAINT:
        if (window16)
        {
            PAINTSTRUCT paint;
            HDC dc = BeginPaint(window, &paint);
            if (dc)
            {
                WowWinPaintWant(window16, &paint.rcPaint, paint.fErase);
                /* [INFO]: WHEN the paint was handed to the guest, so BeginPaint can say
                 * how long it sat there. Throughput was measured and is fine
                 * (~260 GDI calls, ~10 ms, for a full Solitaire repaint); if a
                 * redraw still LOOKS slow the cost is the wait, not the work,
                 * and this is the only number that separates them.
                 */
                g_WowWinPaintMs = GetTickCount();
                EndPaint(window, &paint);
            }
            if (g_WowWinPaintsLogged < WOWWIN_PAINT_LOG_MAX)       /* s92: what the OS reported, and how it went */
            {
                CHAR paintLog[WOWWIN_LOG_LINE_MAX], *paintCursor = paintLog;
                ++g_WowWinPaintsLogged;
                paintCursor = LogPut(paintCursor, "WOWWIN: WM_PAINT h16=0x"); paintCursor = LogHex(paintCursor, window16);
                paintCursor = LogPut(paintCursor, " rc="); paintCursor = LogHex(paintCursor, (DWORD)paint.rcPaint.left);
                paintCursor = LogPut(paintCursor, ","); paintCursor = LogHex(paintCursor, (DWORD)paint.rcPaint.top);
                paintCursor = LogPut(paintCursor, ","); paintCursor = LogHex(paintCursor, (DWORD)paint.rcPaint.right);
                paintCursor = LogPut(paintCursor, ","); paintCursor = LogHex(paintCursor, (DWORD)paint.rcPaint.bottom);
                paintCursor = LogPut(paintCursor, paint.fErase ? " erase" : " noerase");
                paintCursor = LogPut(paintCursor, g_WowWinSizeMove ? " SENT\r\n" : " posted\r\n");
                LogAppend(LOG_PATH, paintLog, paintCursor);
            }
            if (g_WowWinSizeMove)
                WowWinSendOrPost(window16, WM_PAINT16, 0, 0, pointX, pointY);
            else
                WowMsgPost(window16, WM_PAINT16, 0, 0, GetTickCount(), pointX, pointY);
            ++g_WowWinMessages;
            return 0;
        }
        break;

    case WM_ENTERSIZEMOVE:
        ++g_WowWinSizeMove;
    break;

    /* ...and when the loop ends, the whole window is repainted once. A child the guest
     * moved from inside its WM_SIZE (Packager's "View:" label) left the strip it
     * vacated on screen: its erase ran (measured: the strip visible in the DC, the
     * class brush applied) and USER32's own move/size machinery still put the old
     * pixels back afterwards (runs/s92/drv). One full repaint at the end is what
     * makes the final picture right whatever happened mid-drag.
     */
    case WM_EXITSIZEMOVE:
        if (g_WowWinSizeMove > 0)
            --g_WowWinSizeMove;
        if (window16)
            RedrawWindow(window, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
        break;

    /* THE MOUSE. WITHOUT THIS A PAINT PROGRAM CANNOT PAINT (Importance = 5):
     * This procedure relayed keys, system keys, close, size, focus and paint,
     * and NOTHING from the mouse -- so MS Paint could be looked at but not
     * used: no stroke on the canvas, no tool picked out of the toolbox, no
     * colour picked out of the palette. The header note above explains why it
     * was left out ("posting every message would fill the ring with mouse
     * moves the guest never asked for and would hide the ones it did"), and
     * that hazard is real. The answer is not to drop the mouse, it is to
     * COALESCE the moves the way Windows does -- see WowMsgPostMove.
     *
     * [INFO]: RELAYED VERBATIM, like the keyboard. Win16 and Win32 agree on the
     * message numbers (0x200..0x209), on wParam being the MK_* button/modifier
     * bits (MK_LBUTTON 1, MK_RBUTTON 2, MK_SHIFT 4, MK_CONTROL 8, MK_MBUTTON
     * 0x10 in both), and on lParam being the x in the low word and y in the
     * high, in CLIENT coordinates. Composing anything here would be inventing.
     *
     * [CAUTION]: THESE MUST STILL REACH DefWindowProc for the non-client cases, but the
     * ones handled here are all CLIENT-area messages, which DefWindowProc does
     * nothing with. Returning 0 is what a window procedure that handled them
     * does.
     *
     * [CAUTION]: A DOUBLE-CLICK ONLY ARRIVES IF THE CLASS ASKED FOR IT (CS_DBLCLKS). We
     * register the guest's own class style, so a guest that did not ask gets
     * two ordinary clicks -- which is correct, not a gap.
     */
    /* -- s90 (#278): THE MULTIMEDIA NOTIFICATIONS. MM_MCINOTIFY (0x3B9), MM_WOM_*
     * (0x3BB-0x3BD), MM_WIM_* (0x3BE-0x3C0), MM_MIM_ and MM_MOM_ (0x3C1-0x3C9), the
     * joystick ones (0x3A0-0x3B8). A Win16 program that opens a device with
     * CALLBACK_WINDOW gets them POSTED BY WINMM to its real window: winmm's WOW
     * layer maps the 16-bit HWND with WOWHandle32 (bin\wowshim\WOW32.DLL) and
     * posts the 16-bit device handle and 16:16 header itself, so they are relayed
     * VERBATIM. Before this they fell to DefWindowProc and Sound Recorder never
     * learned that a buffer had finished playing.
     */
    case 0x3A0:
    case 0x3A1:
    case 0x3A2:
    case 0x3A3:
    case 0x3A4:
    case 0x3A5:
    case 0x3A6:
    case 0x3A7:
    case 0x3B5:
    case 0x3B6:
    case 0x3B7:
    case 0x3B8:
    case 0x3B9:
    case 0x3BA:
    case 0x3BB:
    case 0x3BC:
    case 0x3BD:
    case 0x3BE:
    case 0x3BF:
    case 0x3C0:
    case 0x3C1:
    case 0x3C2:
    case 0x3C3:
    case 0x3C4:
    case 0x3C5:
    case 0x3C6:
    case 0x3C7:
    case 0x3C8:
    case 0x3C9:
        if (window16)
        {
            if (g_WowWinMultimediaLogged < WOWWIN_MM_LOG_MAX)
            {
                CHAR buffer[WOWWIN_LOG_LINE_MAX], *bufferCursor = buffer;
                ++g_WowWinMultimediaLogged;
                bufferCursor = LogPut(bufferCursor, "WOWWIN: MM notification 0x"); bufferCursor = LogHex(bufferCursor, message);
                bufferCursor = LogPut(bufferCursor, " -> hwnd16 0x"); bufferCursor = LogHex(bufferCursor, window16);
                bufferCursor = LogPut(bufferCursor, " wp=0x"); bufferCursor = LogHex(bufferCursor, (DWORD)wParam);
                bufferCursor = LogPut(bufferCursor, " lp=0x"); bufferCursor = LogHex(bufferCursor, (DWORD)lParam); bufferCursor = LogPut(bufferCursor, "\r\n");
                LogAppend(LOG_PATH, buffer, bufferCursor);
            }
            WowMsgPost(window16, (WORD)message, (WORD)wParam, (DWORD)lParam, GetTickCount(), pointX, pointY);
            ++g_WowWinMessages;
            return 0;
        }
        break;

    case WM_MOUSEMOVE:
        if (window16)
        {
            if (!WowMsgPostMove(window16, (WORD)message, (WORD)wParam, (DWORD)lParam,
                                  GetTickCount(), pointX, pointY))
                WowMsgPost(window16, (WORD)message, (WORD)wParam, (DWORD)lParam,
                            GetTickCount(), pointX, pointY);
            ++g_WowWinMessages;
            return 0;
        }
        break;

    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_LBUTTONDBLCLK:
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
    case WM_RBUTTONDBLCLK:
    case WM_MBUTTONDOWN:
    case WM_MBUTTONUP:
    case WM_MBUTTONDBLCLK:
        if (window16)
        {
            WowMsgPost(window16, (WORD)message, (WORD)wParam, (DWORD)lParam, GetTickCount(), pointX, pointY);
            ++g_WowWinMessages;
            return 0;
        }
        break;

    case WM_CLOSE:
        /* [CAUTION]: LOGGED BOTH WAYS. (s73) User-reported: the X does not close Charmap or
         * WinMine. Measured headlessly -- the guest's loop is alive and dispatching
         * WM_TIMER, but msg 0x0010 never reaches it, and NOTHING in the log said
         * whether this case ran at all or whether h16 resolved. An absence in the
         * report means nothing; say which branch was taken.
         */
        {   CHAR closeLog[WOWWIN_LOG_LINE_MAX], *closeCursor = closeLog;
            closeCursor = LogPut(closeCursor, "WOWWIN: WM_CLOSE on hwnd=0x"); closeCursor = LogHex(closeCursor, (DWORD)(ULONG_PTR)window);
            closeCursor = LogPut(closeCursor, " -> h16=0x"); closeCursor = LogHex(closeCursor, window16);
            closeCursor = LogPut(closeCursor, window16 ? " -- posted to the guest\r\n"
                              : " -- NO Win16 window for it, falling through to DefWindowProc\r\n");
            LogAppend(LOG_PATH, closeLog, closeCursor); }
        if (window16) { WowMsgPost(window16, (WORD)message, 0, 0, GetTickCount(), pointX, pointY);
                   ++g_WowWinMessages;
                   return 0; }
        break;

    /* s92 (#305 M12): a drop on a window that called DragAcceptFiles. POSTED, as the
     * shell posts it, with a Win16 HDROP built by WowWinDrop16 (see there).
     */
    case WM_DROPFILES:
        if (window16)
        {
            CHAR reason[WOWWIN_REASON_MAX], dropLog[WOWWIN_DROP_LOG_MAX], *dropCursor = dropLog;
            WORD drop16 = WowWinDrop16((HDROP)wParam, reason, sizeof reason);
            DragFinish((HDROP)wParam);
            dropCursor = LogPut(dropCursor, "WOWWIN: WM_DROPFILES on h16=0x"); dropCursor = LogHex(dropCursor, window16);
            dropCursor = LogPut(dropCursor, drop16 ? " -> HDROP16 0x" : " -- ★ NOT DELIVERED: ");
            if (drop16)
            {
                dropCursor = LogHex(dropCursor, drop16);
                dropCursor = LogPut(dropCursor, " ");
            }
            dropCursor = LogPut(dropCursor, reason); dropCursor = LogPut(dropCursor, "\r\n");
            LogAppend(LOG_PATH, dropLog, dropCursor);
            if (drop16)
            {
                WowMsgPost(window16, WM_DROPFILES16, drop16, 0, GetTickCount(), pointX, pointY);
                ++g_WowWinMessages;
            }
            return 0;
        }
        break;

    /* WM_TIMER. THE OS IS THE TIMER ENGINE; THIS IS THE WHOLE RELAY (Importance = 2):
     * The real HWND belongs to this thread, so the OS's own timer already
     * delivers WM_TIMER here on schedule with the id in wParam, exactly where
     * Win16 puts it.
     *
     * [INFO]: lParam CARRIES THE 16-BIT TIMERPROC WHEN THE GUEST INSTALLED ONE, which
     * is what Win16 does -- and DispatchMessage, not this relay, is what calls
     * it. See the timer table in wowuser.h for why that ordering is the
     * difference between a faithful implementation and re-entering the guest
     * from inside a Win32 callback.
     *
     * [CAUTION]: A guest that installed NO proc gets lParam 0 and must: it would
     * otherwise receive a pointer it never supplied.
     */
    case WM_TIMER:
        if (window16)
        {
            WowMsgPost(window16, (WORD)message, (WORD)wParam,
                        WowUserTimerProcedure(window16, (WORD)wParam),
                        GetTickCount(), pointX, pointY);
            ++g_WowWinMessages;
            return 0;
        }
        break;

    /* FOCUS AND SIZE, BECAUSE THE GUEST ACTS ON THEM (Importance = 1):
     * A Win16 application puts the caret where it belongs by handling
     * WM_SETFOCUS -- Notepad's answer to it is `SetFocus(its edit control)` --
     * so a host that never delivers one leaves a window nobody can type into
     * and no caret anywhere. WM_SIZE is the same shape: the guest lays its
     * children out in response to it.
     *
     * [CAUTION]: ON REAL WINDOWS THESE ARE **SENT**, NOT POSTED, and here they are posted:
     * the guest sees them at its next GetMessage rather than immediately.
     * Delivering them synchronously means re-entering the guest from inside a
     * Win32 callback, which needs the nested run this host has not built yet.
     * The ordering is therefore slightly wrong and is written down rather than
     * discovered -- it is invisible for a program that only uses them to move
     * focus and lay out children, which is what these two do.
     *
     * [CAUTION]: DefWindowProc still runs afterwards, so the OS keeps its own idea of
     * focus and size; the guest is being told, not put in charge.
     */
    /* [CAUTION]: REFUTED, session 45: "MS Paint lays its children out too big because it
     * is never told its size" -- WM_SIZE has been relayed here since session
     * 43, in this very case. Do not re-add it below; it is a duplicate case
     * value and the compiler says so. The over-sized toolbox is something
     * else.
     */
    /* [CAUTION]: #303: for the two FOCUS messages wParam is the OTHER window -- the one
     * losing focus to this one, or gaining it from it -- and it is a real HWND.
     * `(WORD)wp` handed the guest the low word of a Win32 handle, which names
     * some unrelated Win16 window or none. Mapped through the table; a window
     * that is not a guest window is 0, as Win16 reports one from another task.
     * WM_SIZE's wParam is a SIZE_* code and passes as it is.
     */
    /* -- #305 M9 (s91): ACTIVATION, MOVEMENT, VISIBILITY, MENU SELECTION AND THE
     * SIZE LIMITS reach the guest's procedure, each in the Win16 packing:
     * WM_MOVE (0003) / WM_SHOWWINDOW (0018)  same parameters in both
     * WM_ACTIVATE (0006)   Win32 wParam=MAKELONG(state, fMinimized), lParam=hwnd
     *                      Win16 wParam=state, lParam=MAKELONG(hwnd, fMinimized)
     * WM_ACTIVATEAPP (001C) Win32 lParam = a thread id; Win16 = an hTask --
     *                      0 here, "another task", which is what it always is
     * WM_MENUSELECT (011F) Win32 wParam=MAKELONG(item, flags), lParam=hMenu
     *                      Win16 wParam=item, lParam=MAKELONG(flags, hMenu) --
     *                      hMenu 0: the guest's menus are real Win32 ones here
     * WM_GETMINMAXINFO (0024) the 16-bit MINMAXINFO, 5 POINTs of INT16s,
     *                      sent with the structure and COPIED BACK, so a
     *                      program's minimum size holds
     * SENT through the nested run where it can run (the order Windows gives),
     * posted otherwise; DefWindowProc runs afterwards as before.
     */
    case WM_MOVE:
    case WM_SHOWWINDOW:
        if (window16)
            WowWinSendOrPost(window16, (WORD)message, (WORD)wParam, (DWORD)lParam, pointX, pointY);
        break;

    case WM_ACTIVATE:
        if (window16)
        {
            UINT setFocusCountBefore = g_WowWinSetFocusCount;
            WowWinSendOrPost(window16, (WORD)message, LOWORD(wParam),
                                ((DWORD)(HIWORD(wParam) ? 1 : 0) << WORD_SHIFT)
                                | WowWinHwnd16((HWND)lParam), pointX, pointY);
            /* -- s93: A PROGRAM THAT PLACES THE FOCUS ITSELF KEEPS IT. Win32's
             * DefWindowProc gives an activated window the focus -- after WRITE's
             * own WM_ACTIVATE had just put it on its document window, so every
             * key went to the frame, which ignores them: the user typed into Write
             * and nothing appeared. If the program called SetFocus while handling
             * this, the default still runs (caption, z-order) and the focus goes
             * back where the program put it.
             */
            if (LOWORD(wParam) != WA_INACTIVE && g_WowWinSetFocusCount != setFocusCountBefore
                && g_WowWinSetFocusWindow && g_WowWinSetFocusWindow != window
                && IsChild(window, g_WowWinSetFocusWindow))
            {
                LRESULT result = WowWinDefProc(window, window16, message, wParam, lParam);
                SetFocus(g_WowWinSetFocusWindow);
                return result;
            }
            /* -- s93: A DIALOG KEEPS ITS CONTROL'S FOCUS ACROSS ACTIVATION, as
             * DefDlgProc does (it saves the focus on deactivation and restores it
             * on activation). Our dialogs are our own class on DefWindowProc, which
             * gives the focus to the dialog WINDOW -- so Program Manager's "Program
             * Item Properties" had its keys going to the dialog itself: the first
             * typed letters vanished and Tab only then reached a control.
             */
            {   if (WowUserIsDialog16(window16))
            {
                    if (LOWORD(wParam) == WA_INACTIVE)
                    {
                        HWND focus = GetFocus();
                        if (focus && IsChild(window, focus))
                            SetPropA(window, "NTVDMEX16.DlgFocus", (HANDLE)focus);
                    }
                    else
                    {
                        LRESULT result = WowWinDefProc(window, window16, message, wParam, lParam);
                        HWND focus = GetFocus();
                        if (!focus || focus == window || !IsChild(window, focus))
                        {
                            HWND saved = (HWND)GetPropA(window, "NTVDMEX16.DlgFocus");
                            if (!saved || !IsWindow(saved) || !IsChild(window, saved))
                                saved = GetNextDlgTabItem(window, NULL, FALSE);
                            if (saved)
                                SetFocus(saved);
                        }
                        return result;
                    }
                }
            }
        }
        break;

    case WM_NCDESTROY:
        RemovePropA(window, "NTVDMEX16.DlgFocus");          /* s93: see WM_ACTIVATE */
        break;

    case WM_ACTIVATEAPP:
        if (window16)
            WowWinSendOrPost(window16, (WORD)message, (WORD)(wParam ? 1 : 0), 0, pointX, pointY);
        break;

    /* -- s93: WM_MDIACTIVATE, TO THE CHILD -- AND THE TWO PACKINGS DIFFER. Win32 gives
     * the child (wParam = the one losing, lParam = the one gaining); Win16 gives it
     * (wParam = TRUE if IT is gaining, lParam = MAKELONG(gaining, losing)). It was
     * not relayed at all, so SYSEDIT -- which keeps "the active file" from this
     * message and greys File > Save, Print... without one -- had Save greyed for
     * good: the user's "Save is disabled". SENT, as Windows sends it.
     */
    case WM_MDIACTIVATE:
        if (window16)
        {
            WORD gaining = lParam ? WowWinHwnd16((HWND)lParam) : 0;
            WORD losing = wParam ? WowWinHwnd16((HWND)wParam) : 0;
            WowWinSendOrPost(window16, (WORD)message, (WORD)((HWND)lParam == window ? 1 : 0),
                                ((DWORD)losing << WORD_SHIFT) | gaining, pointX, pointY);
        }
        break;

    case WM_MENUSELECT:
        if (window16) WowWinSendOrPost(window16, (WORD)message, LOWORD(wParam),
                                     (DWORD)HIWORD(wParam), pointX, pointY);
        break;

    case WM_GETMINMAXINFO:
        if (window16 && g_WowWinSend16Blob && lParam && g_WowWinSendingCount < WOWWIN_MAX_SENDING)
        {
            PMINMAXINFO minMaxInfo = (PMINMAXINFO)lParam;
            PPOINT points = &minMaxInfo->ptReserved;
            BYTE buffer[WOWWIN_MINMAXINFO16_SIZE];
            WORD result;
            INT index;
            for (index = 0; index < WOWWIN_MINMAXINFO_POINTS; ++index)
            {
                buffer[index * WOWWIN_POINT16_SIZE] = (BYTE)points[index].x;
                buffer[index * WOWWIN_POINT16_SIZE + 1] = (BYTE)(points[index].x >> BYTE_SHIFT);
                buffer[index * WOWWIN_POINT16_SIZE + WOWWIN_POINT16_Y] = (BYTE)points[index].y;
                buffer[index * WOWWIN_POINT16_SIZE + WOWWIN_POINT16_Y + 1] = (BYTE)(points[index].y >> BYTE_SHIFT);
            }
            INT isSent, slot, isBusy = 0;
            for (slot = 0; slot < g_WowWinSendingCount; ++slot)
                if (g_WowWinSending[slot].Window16 == window16 && g_WowWinSending[slot].Message == (WORD)message)
                    isBusy = 1;
            if (isBusy)
                break;
            g_WowWinSending[g_WowWinSendingCount].Window16 = window16;
            g_WowWinSending[g_WowWinSendingCount].Message = (WORD)message;
            ++g_WowWinSendingCount;
            isSent = g_WowWinSend16Blob(window16, (WORD)message, 0, buffer, WOWWIN_MINMAXINFO16_SIZE, NULL, 0, &result);
            --g_WowWinSendingCount;
            if (isSent)
            {
                for (index = 1; index < WOWWIN_MINMAXINFO_POINTS; ++index)     /* ptReserved is not the guest's to set */
                {
                    points[index].x = (LONG)(SHORT)(buffer[index * WOWWIN_POINT16_SIZE] | (buffer[index * WOWWIN_POINT16_SIZE + 1] << BYTE_SHIFT));
                    points[index].y = (LONG)(SHORT)(buffer[index * WOWWIN_POINT16_SIZE + WOWWIN_POINT16_Y] | (buffer[index * WOWWIN_POINT16_SIZE + WOWWIN_POINT16_Y + 1] << BYTE_SHIFT));
                }
                return 0;
            }
        }
        break;

    case WM_SYSCHAR:
        if (window16) { WowMsgPost(window16, (WORD)message, (WORD)wParam, (DWORD)lParam, GetTickCount(), pointX, pointY);
                   ++g_WowWinMessages; }
        break;

    case WM_SETFOCUS:
    case WM_KILLFOCUS:
    case WM_SIZE:
        if (window16)
        {
            WORD wParam16 = (message == WM_SIZE) ? (WORD)wParam
                                         : WowWinHwnd16((HWND)wParam);
            /* #305 M10 (s91): WM_SIZE is SENT, as Windows sends it -- a program that
             * lays its children out there has done so before the MoveWindow /
             * ShowWindow that caused it returns (w_msgs size.before.return = stock).
             * Focus stays posted (the note above).
             */
            if (message == WM_SIZE)
                WowWinSendOrPost(window16, (WORD)message, wParam16, (DWORD)lParam, pointX, pointY);
            else
                WowMsgPost(window16, (WORD)message, wParam16, (DWORD)lParam, GetTickCount(), pointX, pointY);
            ++g_WowWinMessages;
            /* -- s93: A DIALOG PASSES THE FOCUS ON, as DefDlgProc does on
             * WM_SETFOCUS -- to the control that last had it, else its first tab
             * stop. A dialog window with controls never keeps the focus itself;
             * ours did (DefWindowProc), so Program Manager's Program Item
             * Properties took the first typed letters into nothing and its Tab
             * went to IsDialogMessage with no control focused.
             */
            if (message == WM_SETFOCUS && WowUserIsDialog16(window16))
            {
                HWND saved = (HWND)GetPropA(window, "NTVDMEX16.DlgFocus");
                if (!saved || !IsWindow(saved) || !IsChild(window, saved))
                    saved = GetNextDlgTabItem(window, NULL, FALSE);
                if (saved && saved != window)
                {
                    SetFocus(saved);
                    return 0;
                }
            }
        }
        break;

    /* WM_COMMAND -- THE MENU STOPS BEING DECORATION. (session 44) (Importance = 3):
     * The menu bar is the application's OWN resource on a real Win32 window,
     * so clicking it already produces a real WM_COMMAND carrying the
     * application's own item id -- `0x000b` is `&About Notepad...` out of
     * NOTEPAD.EXE's `MENU 1`. Until now this procedure dropped it, so the menu
     * was real and inert.
     *
     * [CAUTION]: THE TWO PACKINGS ARE DIFFERENT, AND TRANSLATING THEM IS THE WHOLE JOB.
     * Win32 puts the notification code in the HIGH half of wParam and the
     * control's window handle in lParam; Win16 puts the id alone in wParam and
     * packs (hwndCtl, notifyCode) into lParam. Relaying a Win32 WM_COMMAND
     * unchanged gets a MENU command right by luck -- both are "id in the low
     * half, everything else zero" -- and gets every CONTROL notification wrong
     * in both parameters. So it is composed, not relayed.
     *
     * [INFO]: And the control's handle has to become a WIN16 one: a guest comparing it
     * against the handle its own CreateWindow returned must find them equal.
     *
     * [CAUTION]: THE lParam FORM FOR A CONTROL IS NOT CONFIRMED BY A RUN. The menu form
     * is -- SYSEDIT sends itself `(0x111, <id>, 0)`, as logged -- and that
     * is the form Help > About travels. The control form is written here
     * because leaving it as the Win32 packing would be knowingly wrong, and
     * the log prints both halves so the first guest that uses it can say.
     *
     * [CAUTION]: FALLS THROUGH TO THE DEFAULT PROCEDURE ON PURPOSE. The guest is told
     * asynchronously and has no way to answer "I did not handle that", and on
     * an MDI frame DefFrameProc's own WM_COMMAND arm is what activates a child
     * from the Window menu. Posting and then letting the OS have it keeps both
     * -- DefWindowProc does nothing with a WM_COMMAND, so the non-MDI case
     * costs nothing.
     */
    /* WM_INITMENU / WM_INITMENUPOPUP -- WHERE A MENU GETS ITS STATE (Importance = 3):
     * (session 44) An application does not grey and check its menu items when
     * it feels like it; it does so when the OS tells it a menu is ABOUT TO BE
     * SHOWN. Notepad greys Edit > Undo, Cut, Copy, Paste and checks Word Wrap
     * from here -- so with these dropped it never called GetMenu at all, and
     * the whole menu-state cluster looked unused when it was simply never
     * asked for. Found by driving Alt+E on the live guest and watching nothing
     * happen.
     *
     * [CAUTION]: wParam IS AN HMENU AND MUST BECOME A TOKEN. A real menu handle is 32
     * bits and a Win16 program has 16 to hold it in; worse, it hands that
     * handle straight back to EnableMenuItem, which is 16-bit code inside
     * USER, so the value has to survive a round trip and still name the right
     * menu. Truncating a pointer would do neither, and would not fail loudly.
     *
     * [INFO]: lParam is the same shape in both: the popup's index in the low half and
     * "this is the system menu" in the high half. Composed rather than
     * relayed, because the Win32 value is what we have and the Win16 value is
     * what the guest reads.
     */
    case WM_INITMENU:
    case WM_INITMENUPOPUP:
        if (window16)
        {
            WORD menu16 = WowUserMenu16((HMENU)wParam);
            WowMsgPost(window16, (WORD)message, menu16,
                        (message == WM_INITMENUPOPUP)
                            ? ((DWORD)LOWORD(lParam) | ((DWORD)HIWORD(lParam) << WORD_SHIFT))
                            : 0,
                        GetTickCount(), pointX, pointY);
            ++g_WowWinMessages;
        }
        break;

    /* #160: THE MENU MUST WAIT FOR THE APPLICATION TO SET IT UP (Importance = 3):
     * WM_INITMENUPOPUP is where a Win16 program greys and ungreys its items --
     * Notepad enables Cut/Copy/Delete there from EM_GETSEL. But the real menu's
     * modal loop runs INSIDE this thread's pump, where the guest cannot run, so
     * the posted init was handled only after the menu had CLOSED: Copy was shown
     * grey on every first open and its mnemonic did nothing (measured, clip16).
     *
     * Hold the menu back one turn of the guest's loop: post WM_INITMENU and a
     * WM_INITMENUPOPUP for every popup on the bar, then a marker, and open the
     * menu only when the guest's GetMessage reaches the marker -- by which time
     * it has handled every init (wowmsg.h, WOWMSG_MENUREPLAY; the replay itself
     * is WowWinMenuReplay, run from the GetMessage service).
     *
     * [CAUTION]: The real inits still arrive while the menu is open and are posted as
     * before; handled afterwards, they set the state the menu already has.
     */
    case WM_SYSCOMMAND:
        if (window16 && !g_WowWinIsReplaying
            && ((wParam & WOWWIN_SC_MASK) == SC_KEYMENU || (wParam & WOWWIN_SC_MASK) == SC_MOUSEMENU)
            && GetMenu(window))
        {
            HMENU menuBar = GetMenu(window);
            INT index, itemCount = GetMenuItemCount(menuBar);
            DWORD time = GetTickCount();
            WowMsgPost(window16, WM_INITMENU16, WowUserMenu16(menuBar), 0, time, pointX, pointY);
            for (index = 0; index < itemCount && index < WOWWIN_MENU_POPUPS_MAX; ++index)
            {
                HMENU submenu = GetSubMenu(menuBar, index);
                if (submenu) WowMsgPost(window16, WM_INITMENUPOPUP16,
                                     WowUserMenu16(submenu), (DWORD)index, time, pointX, pointY);
            }
            if (WowMsgPost(window16, (WORD)WOWMSG_MENUREPLAY, (WORD)wParam, (DWORD)lParam, time, pointX, pointY))
            {
                ++g_WowWinMenuDeferred;
                return 0;                       /* opened later, by the replay */
            }
        }
        break;

    /* -- #300 (M1): SCROLL BARS. Never relayed before, so a program's own scroll
     * bars -- Write's page, Cardfile's list, Charmap's grid -- did nothing when
     * clicked or dragged. The packing differs:
     *     Win32  wParam = MAKELONG(code, pos)   lParam = scroll-bar HWND (0 =
     *                                                    the window's own bar)
     *     Win16  wParam = code                  lParam = MAKELONG(pos, hwndCtl16)
     *
     * [INFO]: SENT, through the nested run, because Windows sends them from INSIDE its
     * own tracking loop: while an arrow is held or the thumb is dragged the
     * program must answer each one (SetScrollPos, redraw) before the next, or
     * the bar snaps back and the content moves only on release. Posted only if
     * a nested call cannot run here. DefWindowProc does nothing with them.
     */
    case WM_DRAWITEM:
    case WM_MEASUREITEM:
    case WM_DELETEITEM:
    case WM_COMPAREITEM:
        if (window16 && g_WowWinOwnerDraw)
        {
            INT isHandled = 0;
            LRESULT result = g_WowWinOwnerDraw(window, window16, message, wParam, lParam, &isHandled);
            ++g_WowWinMessages;
            if (isHandled)
                return result;
        }
        break;

    case WM_HSCROLL:
    case WM_VSCROLL:
        if (window16)
        {
            WORD code  = (WORD)LOWORD(wParam);
            WORD position   = (WORD)HIWORD(wParam);
            WORD control16 = lParam ? WowWinHwnd16((HWND)(ULONG_PTR)lParam) : 0;
            DWORD lParam16 = (DWORD)position | ((DWORD)control16 << WORD_SHIFT);
            WORD  result16;
            ++g_WowWinMessages;
            if (g_WowWinSend16 && g_WowWinSend16(window16, (WORD)message, code, lParam16, &result16))
                return 0;
            WowMsgPost(window16, (WORD)message, code, lParam16, GetTickCount(), pointX, pointY);
            return 0;
        }
        break;

    case WM_COMMAND:
        if (window16)
        {
            WORD id     = (WORD)LOWORD(wParam);
            WORD notifyCode = (WORD)HIWORD(wParam);
            WORD control16  = lParam ? WowWinHwnd16((HWND)(ULONG_PTR)lParam) : 0;
            WowMsgPost(window16, (WORD)WM_COMMAND16, id,
                        (DWORD)control16 | ((DWORD)notifyCode << WORD_SHIFT),
                        GetTickCount(), pointX, pointY);
            ++g_WowWinMessages;
        }
        break;

    default:
        break;
    }
    /* THE RIGHT DEFAULT PROCEDURE, WHICH IS WHAT MAKES MDI WORK (Importance = 1):
     * Win32 has three, and which one a window needs is a property of where it
     * sits in the tree, not of anything the guest tells us: an MDI FRAME must
     * pass its client to DefFrameProc (that is how Alt+F4, the child system
     * menu and the window list get handled), an MDI CHILD needs
     * DefMDIChildProc, and everything else DefWindowProc. Getting this wrong is
     * not cosmetic -- an MDI frame on DefWindowProc loses its children's
     * non-client behaviour entirely.
     */
    /* #294: a Find/Replace dialog's notification ("commdlg_FindReplace") to its
     * owner -- relayed with the guest's own FINDREPLACE pointer.
     */
    if (window16 && message >= WOWWIN_REGISTERED_MESSAGE_FIRST && WowCdlgRelay(message, lParam))
        return 0;
    return WowWinDefProc(window, window16, message, wParam, lParam);
}

/* PUMP THE EXEC THREAD'S Win32 QUEUE (Importance = 2):
 * The windows belong to this thread, so nothing about them happens -- no paint,
 * no move, no click, no title bar -- unless this thread dispatches. Called at
 * every WOW32 BOP, which is the only regular moment the exec thread is not
 * inside the guest.
 *
 * [CAUTION]: BOUNDED. A pump that drained without limit would let a flood of mouse moves
 * starve the guest, and the guest is the thing we are here to run.
 */
/* Ticks spent in here, and how many times it was entered -- the other half of
 * the per-BOP cost. See the note in log.h.
 */
LONGLONG g_WowWinPumpTicks = 0;
DWORD    g_WowWinPumpCalls = 0;

INT WowWinPump(INT budget)
{
    MSG message;
    INT count = 0;
    LARGE_INTEGER startTicks, endTicks;

    if (!g_WowWinCreated)
        return 0;
    QueryPerformanceCounter(&startTicks);
    ++g_WowWinPumpCalls;
    while (count < budget && PeekMessageA(&message, NULL, 0, 0, PM_REMOVE))
    {
        ++count;
        ++g_WowWinPumped;
        if (WowWinThreadTimerFire(&message))
            continue;                                           /* s93: a windowless Win16 timer */
        if (WowCdlgIsDialogMessage(&message))
            continue;                                          /* #294: Find dialog's Tab/Enter */
        TranslateMessage(&message);
        DispatchMessageA(&message);
    }
    QueryPerformanceCounter(&endTicks);
    g_WowWinPumpTicks += endTicks.QuadPart - startTicks.QuadPart;
    return count;
}

/* #160: open the menu a WM_SYSCOMMAND was held back for (see that case). Runs the
 * real modal menu loop here, on this thread, exactly where the pump would have.
 */
VOID WowWinMenuReplay(PCWOWMSG replay)
{
    HWND window = WowUserHwnd32(replay->Window);

    if (!window || !IsWindow(window))
        return;
    g_WowWinIsReplaying = 1;
    SendMessageA(window, WM_SYSCOMMAND, (WPARAM)replay->WParam, (LPARAM)replay->LParam);
    g_WowWinIsReplaying = 0;
}

/* Register a real Win32 class for a Win16 one. Returns 1 if the class is usable.
 *
 * [CAUTION]: cbWndExtra is ZERO on purpose: the guest's window words are kept in
 * WOWUSER_WINDOW (session 40), bounded by the class's own declaration, and
 * giving Win32 a second copy would create two answers to one question.
 *
 * [INFO]: `curord` / `icoord` are the PREDEFINED ORDINALS the guest put in its
 * WNDCLASS, or 0. This is the moment the token minted by USER id 0xad becomes a
 * real object: the guest has just said which field it belongs in, so cursor and
 * icon can finally be told apart -- see the note by WOWUSER_LOADSYSOBJ.
 *
 * [CAUTION]: THE ORDINAL IS PASSED TO THE OS UNCHANGED, on the same claim as the WS_*
 * bits: Win32 inherited the predefined cursor and icon ordinals from Win16. If
 * that is ever wrong the OS returns NULL, so the fallback below is not belt and
 * braces -- it is what turns a wrong assumption into a visible line instead of
 * a window with no cursor.
 */
/* [CAUTION]: THE CURSOR IS NOW BUILT BY THE CALLER TOO, like the icon. (session 47) It
 * used to be an ordinal resolved here, which could only ever name one of the
 * OS's predefined cursors -- and MS Paint's seven cursors are all NAMED
 * resources in its own file, so a paint program's pointer never changed shape.
 * One asymmetry removed: whoever knows the token resolves it, and this only
 * decides what to do when there is nothing.
 */
INT WowWinRegister(
    PCSTR name16,
    PSTR className32,
    INT capacity,
    HCURSOR cursor,
    HICON icon,
    HICON smallIcon,
    PINT isCursorDefaulted,
    HBRUSH background)
{
    /* [CAUTION]: WNDCLASSEX, NOT WNDCLASS, AND FOR ONE REASON: `hIconSm`. A class with no
     * small icon makes Windows derive one, and the derived one measured
     * MONOCHROME against stock ntvdm on the taskbar while the caption was
     * correct -- same window, same HICON, two renderings. See wowres.h.
     */
    WNDCLASSEXA windowClass;
    INT length = 0, index;

    for (index = 0; WOWWIN_CLASS_PREFIX[index] && length < capacity - 1; ++index)
        className32[length++] = WOWWIN_CLASS_PREFIX[index];
    for (index = 0; name16[index] && length < capacity - 1; ++index)
        className32[length++] = name16[index];
    className32[length] = 0;
    ZeroMemory(&windowClass, sizeof windowClass);
    windowClass.cbSize        = sizeof windowClass;
    windowClass.lpfnWndProc   = WowWinProc;
    windowClass.hInstance     = GetModuleHandleA(NULL);
    windowClass.hCursor       = cursor;
    if (!windowClass.hCursor)
    {
        if (isCursorDefaulted)
            *isCursorDefaulted = 1;
        windowClass.hCursor = LoadCursorA(NULL, IDC_ARROW);
    }
    windowClass.hIcon   = icon;        /* built by the caller: predefined, or the app's own */
    windowClass.hIconSm = smallIcon;         /* and the 16x16 built at 16x16, not shrunk later */
    /* -- THE CLASS'S OWN BACKGROUND BRUSH, AND IT USED TO BE THROWN AWAY.
     * This was hard-coded to COLOR_WINDOW+1 -- WHITE -- for every Win16 class
     * ever registered, while the guest's real `hbrBackground` was read into
     * `c->hbrback` and then ignored. Windows erases with this brush on every
     * BeginPaint that asks for one, so any region the guest did not repaint
     * itself came out WHITE.
     *
     * [INFO]: IT LOOKED FINE FOR EIGHT SESSIONS BECAUSE ANOTHER BUG WAS HIDING IT.
     * InvalidateRect had its `lpRect` offset clobbered (session 50) and so
     * invalidated the WHOLE CLIENT AREA on every call -- the guest therefore
     * repainted everything, every time, and painted over the white. Fixing
     * InvalidateRect to honour the rectangle is what exposed this: SOLITAIRE's
     * green table grew white patches wherever a card had been moved.
     *
     * [CAUTION]: TWO BUGS, ONE SYMPTOM, AND THE SECOND ONE WAS OLDER. Recorded because
     * the obvious reading -- "the InvalidateRect fix broke Solitaire" -- is
     * wrong and would have led to reverting a correct fix.
     */
    windowClass.hbrBackground = background ? background : (HBRUSH)(COLOR_WINDOW + 1);
    windowClass.lpszClassName = className32;
    if (RegisterClassExA(&windowClass))
        return 1;
    /* Already registered is success: a program may register a class name twice
     * across two instances, and Win32 says so with ERROR_CLASS_ALREADY_EXISTS.
     */
    return GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

/* Win16 CW_USEDEFAULT -> Win32's. Different values; see the header note. */
INT WowWinCoordinate(WORD value)
{
    return (value == CW_USEDEFAULT16) ? CW_USEDEFAULT32 : (INT)(SHORT)value;
}
