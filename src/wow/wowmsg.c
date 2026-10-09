/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * THE WIN16 MESSAGE QUEUE. GH #128, session 41.
 *
 * The code of wowmsg.h (#335): its functions and state, in their original order;
 * its own translation unit, declared in wowmsg.h.
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#include "wowmsg.h"
#include "host_state.h"
#include "wow32.h"

/* [INFO]: AND IT IS A KNOB NOW (session 43, `wowidle.txt`): **0 means forever**, which
 * is what a real Win16 task does and what an interactive session needs. A
 * program sitting in GetMessage with its window on the desktop is not stuck, it
 * is waiting for the user -- and quitting it after six seconds makes it
 * impossible to type into. The bound stays the default so an unattended run
 * still finishes.
 */
DWORD g_WowMsgWaitMs = WOWMSG_WAIT_MS;
INT   g_WowMsgIsWaitAnnounced    = 0;   /* the setting is announced once, at first use */

/* "THE GUEST IS PARKED HERE ON PURPOSE", FOR THE FREEZE WATCHDOG (Importance = 3):
 * Non-zero while the exec thread is inside the blocking GetMessage wait.
 *
 * [CAUTION]: THIS EXISTS BECAUSE THE WATCHDOG KILLED AN IDLE Win16 APPLICATION. The
 * DPMI watchdog samples `g_dpmi_iter` every 250 ms and calls a run WEDGED when
 * it stops advancing -- 600 samples, i.e. **150 seconds**, on a WOW run. That
 * bound was written when `wowrun.bat` bounded every run at 75 s, and session
 * 43's `wowidle.txt`=0 ("a blocked GetMessage waits FOREVER") broke the
 * premise without anyone telling the watchdog. So a guest left on the desktop
 * for a human to use was TerminateProcess'd 150 s after it went idle, and the
 * only note went to `wdprobe.log` -- the main log simply stopped mid-heartbeat.
 * Found because the user looked at the box and said "it's only running stock".
 *
 * [INFO]: AND A PARKED GUEST GENUINELY LOOKS FROZEN: it sits at one EIP inside our own
 * wait, which is exactly what a Win16 task waiting for input IS. The watchdog
 * cannot tell that from a wedge by sampling, and it does not have to -- the
 * host put it there and can simply say so.
 */
volatile LONG g_WowMsgInWait = 0;
INT      g_WowMsgCount = 0;
DWORD    g_WowMsgPosted = 0;    /* how many went in, for the run summary */
DWORD    g_WowMsgTaken  = 0;    /* ...and how many came out */
WORD     g_WowMsgQuitCode = 0;
/* s92 (#306): ONE RING, BUT EVERY TASK SEES ONLY ITS OWN MESSAGES:
 * The note at the top came true: with the run queue, Calc's GetMessage took a
 * message WinHelp had posted to ITS OWN window, and DispatchMessage ran WinHelp's
 * window procedure on Calc's stack -- SS != DS, a near pointer to a local read
 * garbage, #GP. Win16 queues are per task, so a take is too: `g_WowMsgTaker` is
 * the task asking (0 = no filter: the modal loops and BeginPaint keep their
 * old behaviour), `g_WowMsgOwner` names a window's task (0 = unknown, anyone's).
 * A thread message (hwnd 0) is anyone's too. And PostQuitMessage ends the loop
 * of the task that called it, not every task's.
 */
WORD     g_WowMsgTaker = 0;
WORD   (*g_WowMsgOwner)(WORD window) = 0;

/* [INFO]: WHO A KEYSTROKE IS FOR. Win16 sends keyboard input to the focus window, and
 * SYSEDIT sets one (USER 0x16 SETFOCUS, four times in a launch). With no
 * focus the target is 0, and USER's own DispatchMessage `jcxz`es a null hwnd
 * -- so a key with nowhere to go is discarded BY THE GUEST, correctly, and
 * this host does not have to invent a destination.
 */
WORD     g_WowMsgFocus = 0;

INT      g_WowMsgIsReplayDue = 0;
WOWMSG g_WowMsgReplay;

static WOWMSG g_WowMsgRing[WOWMSG_MAX];
static INT      g_WowMsgHead = 0;      /* next to take */
static INT      g_WowMsgTail = 0;      /* next to fill */
static DWORD    g_WowMsgDropped = 0;   /* ring full -- LOUD, see WOWMSG_MAX */
static INT      g_WowMsgIsQuit = 0;      /* PostQuitMessage was called */

static WOWMSG_QUIT g_WowMsgQuits[WOWMSG_MAXQUIT];
static INT      g_WowMsgQuitCount = 0;

static INT WowMsgIsFor(WORD window, WORD task)
{
    WORD owner;

    if (!task || !window || !g_WowMsgOwner)
        return 1;

    owner = g_WowMsgOwner(window);
    return !owner || owner == task;
}

VOID WowMsgPostQuit(WORD task, WORD code)
{
    INT index;

    for (index = 0; index < g_WowMsgQuitCount; ++index)
        if (g_WowMsgQuits[index].Task == task)
            break;

    if (index == g_WowMsgQuitCount)
    {
        if (g_WowMsgQuitCount == WOWMSG_MAXQUIT)            /* full: the oldest goes */
        {
            for (index = 1; index < g_WowMsgQuitCount; ++index)
                g_WowMsgQuits[index - 1] = g_WowMsgQuits[index];

            --g_WowMsgQuitCount;
        }

        index = g_WowMsgQuitCount++;
    }

    g_WowMsgQuits[index].Task = task;
    g_WowMsgQuits[index].Code = code;
    g_WowMsgIsQuit = 1;
    g_WowMsgQuitCode = code;
}

/* 1 + the index of `task`'s pending quit (a task-0 quit is anyone's), or 0. */
INT WowMsgQuitFor(WORD task)
{
    INT index;

    for (index = 0; index < g_WowMsgQuitCount; ++index)
        if (!task || !g_WowMsgQuits[index].Task || g_WowMsgQuits[index].Task == task)
            return index + 1;

    return 0;
}

/* Deliver it: the exit code, and the flag is cleared. */
WORD WowMsgTakeQuit(INT quitNumber)
{
    WORD code;
    INT index;

    if (quitNumber < 1 || quitNumber > g_WowMsgQuitCount)
        return 0;

    code = g_WowMsgQuits[quitNumber - 1].Code;

    for (index = quitNumber; index < g_WowMsgQuitCount; ++index)
        g_WowMsgQuits[index - 1] = g_WowMsgQuits[index];

    --g_WowMsgQuitCount;
    g_WowMsgIsQuit = g_WowMsgQuitCount > 0;
    return code;
}

/* Put one message in the queue. [CAUTION] CALLED FROM THE UI THREAD as well as the exec
 * thread (a keystroke arrives on whichever thread owns the host window), so
 * every caller must hold the host lock -- there is no lock in here, on purpose,
 * because this file must not know how the host serialises itself.
 */
INT WowMsgPost(
    WORD window,
    WORD message,
    WORD wParam,
    DWORD lParam,
    DWORD time,
    WORD pointX,
    WORD pointY)
{
    PWOWMSG entry;

    if (g_WowMsgCount >= WOWMSG_MAX)
    {
        ++g_WowMsgDropped;
        return 0;
    }

    entry = &g_WowMsgRing[g_WowMsgTail];
    entry->Window = window;
    entry->Message = message;
    entry->WParam = wParam;
    entry->LParam = lParam;
    entry->Time = time;
    entry->PointX = pointX;
    entry->PointY = pointY;
    g_WowMsgTail = (g_WowMsgTail + 1) % WOWMSG_MAX;
    ++g_WowMsgCount;
    ++g_WowMsgPosted;
    return 1;
}

/* MOUSE MOVES COALESCE (Importance = 5):
 * wowwin.h used to relay no mouse input at all, and said why: "posting every
 * message would fill the ring with mouse moves the guest never asked for and
 * would hide the ones it did". That is a real hazard and it is the reason a
 * Win16 program cannot draw without this -- but the answer is not to drop the
 * mouse, it is what Windows itself does: keep only the NEWEST pending
 * WM_MOUSEMOVE per window. A position is not a history; an old one is
 * worthless the moment a newer one exists, and a button press is never
 * coalesced away because only moves are folded.
 *
 * [CAUTION]: ONLY A MOVE ALREADY AT THE TAIL IS REPLACED. Folding a move that sits
 * BEHIND a button press would reorder input -- the guest would see the click
 * at a position the pointer had not reached yet -- so the scan stops at the
 * newest entry for that window.
 * Returns 1 if it folded into an existing entry.
 */
INT WowMsgPostMove(
    WORD window,
    WORD message,
    WORD wParam,
    DWORD lParam,
    DWORD time,
    WORD pointX,
    WORD pointY)
{
    INT newest;

    if (g_WowMsgCount)
    {
        newest = (g_WowMsgTail + WOWMSG_MAX - 1) % WOWMSG_MAX;    /* the newest entry */

        if (g_WowMsgRing[newest].Window == window && g_WowMsgRing[newest].Message == message)
        {
            g_WowMsgRing[newest].WParam = wParam;
            g_WowMsgRing[newest].LParam = lParam;
            g_WowMsgRing[newest].Time = time;
            g_WowMsgRing[newest].PointX = pointX;
            g_WowMsgRing[newest].PointY = pointY;
            return 1;
        }
    }

    return 0;
}

INT WowMsgTake(WORD window, WORD filterMin, WORD filterMax, INT isRemove, PWOWMSG output)
{
    INT position;
    INT index;

    if (!g_WowMsgCount)
        return 0;

    for (position = 0; position < g_WowMsgCount; ++position)
    {
        PWOWMSG entry = &g_WowMsgRing[(g_WowMsgHead + position) % WOWMSG_MAX];

        if (entry->Message == WOWMSG_MENUREPLAY)
        {
            g_WowMsgReplay = *entry;
            g_WowMsgIsReplayDue = 1;

            for (index = position; index > 0; --index)
                g_WowMsgRing[(g_WowMsgHead + index) % WOWMSG_MAX] =
                    g_WowMsgRing[(g_WowMsgHead + index - 1) % WOWMSG_MAX];

            g_WowMsgHead = (g_WowMsgHead + 1) % WOWMSG_MAX;
            --g_WowMsgCount;
            --position;                          /* the next entry now sits at n */
            continue;
        }

        if (window && entry->Window != window)
            continue;

        if ((filterMin || filterMax) && (entry->Message < filterMin || entry->Message > filterMax))
            continue;

        if (!WowMsgIsFor(entry->Window, g_WowMsgTaker))
            continue;                                                      /* s92 #306 */

        *output = *entry;

        if (isRemove)
        {
            for (index = position; index > 0; --index)
                g_WowMsgRing[(g_WowMsgHead + index) % WOWMSG_MAX] =
                    g_WowMsgRing[(g_WowMsgHead + index - 1) % WOWMSG_MAX];

            g_WowMsgHead = (g_WowMsgHead + 1) % WOWMSG_MAX;
            --g_WowMsgCount;
            ++g_WowMsgTaken;
        }

        return 1;
    }

    return 0;
}

/* s92 (#306): how many queued messages are `task`'s (all of them for task 0). */
INT WowMsgCountFor(WORD task)
{
    INT position;
    INT count = 0;

    for (position = 0; position < g_WowMsgCount; ++position)
        if (WowMsgIsFor(g_WowMsgRing[(g_WowMsgHead + position) % WOWMSG_MAX].Window, task))
            ++count;

    return count;
}

/* Read an 18-byte MSG back out of guest memory -- the guest owns this one; it is
 * the buffer GetMessage filled and the loop then handed to DispatchMessage. Only
 * the four fields a window procedure is called with are taken.
 */
VOID WowMsgRead(const volatile BYTE *bytes, PWOWMSG message)
{
    message->Window   = (WORD)(bytes[WOWMSG_FIELD_HWND]    | (bytes[WOWMSG_FIELD_HWND + 1]    << BYTE_SHIFT));
    message->Message    = (WORD)(bytes[WOWMSG_FIELD_MESSAGE] | (bytes[WOWMSG_FIELD_MESSAGE + 1] << BYTE_SHIFT));
    message->WParam = (WORD)(bytes[WOWMSG_FIELD_WPARAM]  | (bytes[WOWMSG_FIELD_WPARAM + 1]  << BYTE_SHIFT));
    message->LParam = (DWORD)(bytes[WOWMSG_FIELD_LPARAM] | (bytes[WOWMSG_FIELD_LPARAM + 1] << BYTE_SHIFT))
              | ((DWORD)(bytes[WOWMSG_FIELD_LPARAM + WOW_WORD_BYTES] | (bytes[WOWMSG_FIELD_LPARAM + WOW_WORD_BYTES + 1] << BYTE_SHIFT)) << WORD_SHIFT);
    message->Time   = 0;
    message->PointX = 0;
    message->PointY = 0;
}

/* Write an 18-byte MSG through the far pointer the guest handed us. */
VOID WowMsgWrite(volatile BYTE *bytes, PCWOWMSG message)
{
    Wow32PokeWord(bytes + WOWMSG_FIELD_HWND,    message->Window);
    Wow32PokeWord(bytes + WOWMSG_FIELD_MESSAGE, message->Message);
    Wow32PokeWord(bytes + WOWMSG_FIELD_WPARAM,  message->WParam);
    Wow32PokeWord(bytes + WOWMSG_FIELD_LPARAM,     (WORD)(message->LParam & WORD_MASK));
    Wow32PokeWord(bytes + WOWMSG_FIELD_LPARAM + WOW_WORD_BYTES, (WORD)(message->LParam >> WORD_SHIFT));
    Wow32PokeWord(bytes + WOWMSG_FIELD_TIME,       (WORD)(message->Time & WORD_MASK));
    Wow32PokeWord(bytes + WOWMSG_FIELD_TIME + WOW_WORD_BYTES,   (WORD)(message->Time >> WORD_SHIFT));
    Wow32PokeWord(bytes + WOWMSG_FIELD_POINT,     message->PointX);
    Wow32PokeWord(bytes + WOWMSG_FIELD_POINT + WOW_WORD_BYTES, message->PointY);
}
