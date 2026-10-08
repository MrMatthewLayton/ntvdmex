#ifndef NTVDMEX_WOWMSG_H
#define NTVDMEX_WOWMSG_H
/*
 * wowmsg.h -- ★ THE WIN16 MESSAGE QUEUE. GH #128, session 41.
 *
 * ── WHY THIS IS THE FRONTIER ────────────────────────────────────────────────
 * SYSEDIT builds its whole interface, loads its four files, and then sits in the
 * documented Win16 message loop -- its NE import table (`tools/ne/neimports.py`)
 * names every call in it:
 *
 *     GetMessage(&msg, 0, 0, 0)                 ; USER.108  -- ★ 0 IS WM_QUIT
 *     TranslateMDISysAccel(hwndClient, &msg)    ; USER.451
 *     TranslateAccelerator(hwnd, hAccel, &msg)
 *     TranslateMessage(&msg)                    ; USER.113
 *     DispatchMessage(&msg)                     ; USER.114
 *
 * Answered with the harness sentinel, the application is told to quit and does,
 * cleanly -- not failing, dismissed. Everything a Win16 program does after its
 * startup arrives through that loop, so until it can turn, nothing else about
 * this half of the project can be measured at all.
 *
 * ── ★★ WHO IMPLEMENTS THE REST OF THE LOOP -- SETTLED BY A RUN ──────────────
 * `docs/research/wow-user-surface.md` names 385 of USER's 441 ids and neither
 * `TranslateMessage` nor `DispatchMessage` is among them, which reads like "they
 * are 16-bit code inside USER.EXE". ⚠ THE RUN REFUTES THAT. With GetMessage
 * answered, the loop turns and the two calls arrive here as ordinary WOW32 BOPs.
 * Each id was named by matching the return address the BOP carried against the
 * import relocations in SYSEDIT's NE header, so nothing here is inferred:
 *
 *     id 0x071  4 arg bytes  = TRANSLATEMESSAGE
 *     id 0x072  4 arg bytes  = DISPATCHMESSAGE
 *     id 0x0b2  8 arg bytes  = TRANSLATEACCELERATOR
 *     id 0x1c3  6 arg bytes  = TRANSLATEMDISYSACCEL
 *
 * They are four of the 56 ids the map could not name from the export table.
 * ⇒ So the host fills the MSG **and** dispatches it. `DispatchMessage` is
 *   `WowCallEnter` into the window's procedure, i.e. machinery session 40
 *   already built.
 *
 * ── THE MSG ─────────────────────────────────────────────────────────────────
 * `DispatchMessage` takes nothing but `lpMsg`, so everything a window procedure
 * is called with is in those 18 bytes -- the documented Win16 MSG, whose size
 * matches the 18-byte stack buffer whose address SYSEDIT passes (see the
 * GetMessage arguments below). ★ A MSG with hwnd 0 dispatches to no window.
 *
 *   +0x00 WORD  hwnd        +0x06 DWORD lParam
 *   +0x02 WORD  message     +0x0a DWORD time
 *   +0x04 WORD  wParam      +0x0e POINT pt (two WORDs)
 *
 * ⚠ `time` and `pt` are NOT pinned by any guest -- nothing this host has
 *   watched reads them. They are the remaining 8 bytes of an 18-byte structure
 *   whose first 10 are known, and they are filled with the tick count and the
 *   cursor position because leaving 8 bytes of the guest's stack untouched is
 *   worse than filling them: a program that reads them would read litter. The
 *   log says what went in, so a guest that disagrees can correct it.
 *
 * ── ONE QUEUE, AND THE CODE SAYS SO RATHER THAN PRETENDING ──────────────────
 * Win16 gives every task its own queue. This is one ring, because the host has
 * exactly one input source and every message in it is addressed to a window
 * this host issued the handle for -- so "whose message is it" has a single
 * answer today and needs none of the machinery. ⚠ The moment a SECOND task runs
 * an interactive message loop that stops being true, and the fix is a task field
 * on the entry plus krnl386's own task list, not a second copy of this file.
 */

/* Win16 message numbers this host names. Every one is either read out of a
   guest (WM_CREATE, WM_MDICREATE, EM_*) or is the one that makes GetMessage
   return 0 and end the loop. */
#define WM_QUIT16       0x0012
/* ★ WM_DESTROY, WITHOUT WHICH A WIN16 TASK NEVER ENDS. (session 56) See the
     DestroyWindow arm in wowuser.h: it is the message whose handler calls
     PostQuitMessage, and until it was delivered every guest sat in GetMessage
     forever after its window closed. */
#define WM_DESTROY16    0x0002
#define WM_KEYDOWN16    0x0100
#define WM_KEYUP16      0x0101
/* ── ★★★ WM_COMMAND (0x111), CONFIRMED ON THE GUESTS ─────────────────────────
     Documented, and observed at run time: SYSEDIT sends ITSELF
     SendMessage(hwnd, 0x111, <menu id>, 0) -- the log shows the call's
     arguments -- and COMMDLG's dialog procedures handle 0x110 (WM_INITDIALOG)
     and 0x111 together.
   ★ THAT SEND PINS THE PACKING TOO, which is the part that differs between
     Win16 and Win32: for a MENU command Win16 puts the id alone in wParam and
     ZERO in lParam. See the translation in wowwin.h. */
#define WM_COMMAND16    0x0111
/* ⚠ WM_TIMER's lParam is the guest's TIMERPROC when it installed one, and
   DispatchMessage calls that INSTEAD of the window procedure -- see wowuser.h. */
#define WM_TIMER16      0x0113

/* MSG field offsets -- see the note above. */
#define WOWMSG_FIELD_HWND    0x00
#define WOWMSG_FIELD_MESSAGE 0x02
#define WOWMSG_FIELD_WPARAM  0x04
#define WOWMSG_FIELD_LPARAM  0x06
#define WOWMSG_FIELD_TIME    0x0a
#define WOWMSG_FIELD_POINT   0x0e
#define WOWMSG_SIZE          0x12

/* ── The argument blocks. Reversed as always (the base is the LAST push), and
     GetMessage's is confirmed against a line this host has already printed for
     SYSEDIT's GetMessage(&msg, 0, 0, 0): `args=0x0a b=(0x0000 0x0000 0x0000
     0x248a 0x0a9f)`. +6/+8 is the far pointer to its stack MSG, and +4/+2/+0
     are the three zeroes. A wrong assignment does not produce a readable
     pointer. */
#define WOWMSG_GETMESSAGE_ARG_MAX   0
#define WOWMSG_GETMESSAGE_ARG_MIN   2
#define WOWMSG_GETMESSAGE_ARG_HWND  4
#define WOWMSG_GETMESSAGE_ARG_LPMSG 6

#define WOWMSG_PEEKMESSAGE_ARG_REMOVE 0
#define WOWMSG_PEEKMESSAGE_ARG_MAX    2
#define WOWMSG_PEEKMESSAGE_ARG_MIN    4
#define WOWMSG_PEEKMESSAGE_ARG_HWND   6
#define WOWMSG_PEEKMESSAGE_ARG_LPMSG  8

/* PostMessage(hWnd, msg, wParam, lParam) -- the same 10 bytes, in the same
   order, as SendMessage's block, which this host already reads. */
#define WOWMSG_POSTMESSAGE_ARG_LPARAM 0
#define WOWMSG_POSTMESSAGE_ARG_WPARAM 4
#define WOWMSG_POSTMESSAGE_ARG_MSG    6
#define WOWMSG_POSTMESSAGE_ARG_HWND   8

#define WOWMSG_POSTQUITMESSAGE_ARG_EXITCODE 0
#define WOWMSG_SETFOCUS_ARG_HWND            0

/* The rest of the loop, every offset confirmed against the args this host has
   already printed for the call:
     DispatchMessage(lpMsg)                       (0x248a 0x0a9f)
     TranslateMessage(lpMsg)                      (0x248a 0x0a9f)
     TranslateAccelerator(hWnd, hAccel, lpMsg)    (0x248a 0x0a9f 0x0a8e 0x0140)
     TranslateMDISysAccel(hWndClient, lpMsg)      (0x248a 0x0a9f 0x0160)
   -- 0x0a8e is the handle LoadAccelerators returned and 0x0140/0x0160 are the
   frame and MDI-client windows this host issued, so three of the four values are
   ones we can recognise. */
#define WOWMSG_DISPATCHMESSAGE_ARG_LPMSG       0
#define WOWMSG_TRANSLATEACCELERATOR_ARG_LPMSG  0
#define WOWMSG_TRANSLATEACCELERATOR_ARG_HACCEL 4
#define WOWMSG_TRANSLATEACCELERATOR_ARG_HWND   6
#define WOWMSG_TRANSLATEMDISYSACCEL_ARG_LPMSG  0
#define WOWMSG_TRANSLATEMDISYSACCEL_ARG_HWND   4

#define PM_REMOVE16     0x0001

/* 64 is not a guess about Windows, it is a bound on a host with one keyboard:
   a full ring means the guest is not draining, which is a fact worth printing
   rather than a queue worth growing. */
#define WOWMSG_MAX      64

/* How long a task blocking in GetMessage waits before the host gives up on it.
   ⚠ A REAL Win16 TASK WAITS FOREVER. This bound exists because a harness run has
     to end, and because a host that hangs on an empty queue looks exactly like a
     host that has crashed. Long enough that a scripted keystroke (keys.txt) can
     land inside it, short enough that a run without one still finishes. */
#define WOWMSG_WAIT_MS  6000
/* ★ AND IT IS A KNOB NOW (session 43, `wowidle.txt`): **0 means forever**, which
     is what a real Win16 task does and what an interactive session needs. A
     program sitting in GetMessage with its window on the desktop is not stuck, it
     is waiting for the user -- and quitting it after six seconds makes it
     impossible to type into. The bound stays the default so an unattended run
     still finishes. */
static DWORD g_WowMsgWaitMs = WOWMSG_WAIT_MS;
static INT   g_WowMsgIsWaitAnnounced    = 0;   /* the setting is announced once, at first use */

/* ── ★★★ "THE GUEST IS PARKED HERE ON PURPOSE", FOR THE FREEZE WATCHDOG. ──────
     Non-zero while the exec thread is inside the blocking GetMessage wait.
   ⚠⚠ THIS EXISTS BECAUSE THE WATCHDOG KILLED AN IDLE Win16 APPLICATION. The
     DPMI watchdog samples `g_dpmi_iter` every 250 ms and calls a run WEDGED when
     it stops advancing -- 600 samples, i.e. **150 seconds**, on a WOW run. That
     bound was written when `wowrun.bat` bounded every run at 75 s, and session
     43's `wowidle.txt`=0 ("a blocked GetMessage waits FOREVER") broke the
     premise without anyone telling the watchdog. So a guest left on the desktop
     for a human to use was TerminateProcess'd 150 s after it went idle, and the
     only note went to `wdprobe.log` -- the main log simply stopped mid-heartbeat.
     Found because the user looked at the box and said "it's only running stock".
   ★ AND A PARKED GUEST GENUINELY LOOKS FROZEN: it sits at one EIP inside our own
     wait, which is exactly what a Win16 task waiting for input IS. The watchdog
     cannot tell that from a wedge by sampling, and it does not have to -- the
     host put it there and can simply say so. */
static volatile LONG g_WowMsgInWait = 0;

typedef struct _WOWMSG {
    WORD  Window, Message, WParam;
    DWORD LParam;
    DWORD Time;
    WORD  PointX, PointY;
} WOWMSG, *PWOWMSG;
typedef const WOWMSG *PCWOWMSG;

static WOWMSG g_WowMsgRing[WOWMSG_MAX];
static INT      g_WowMsgHead = 0;      /* next to take */
static INT      g_WowMsgTail = 0;      /* next to fill */
static INT      g_WowMsgCount = 0;
static DWORD    g_WowMsgPosted = 0;    /* how many went in, for the run summary   */
static DWORD    g_WowMsgTaken  = 0;    /* ...and how many came out                */
static DWORD    g_WowMsgDropped = 0;   /* ring full -- LOUD, see WOWMSG_MAX       */
static INT      g_WowMsgIsQuit = 0;      /* PostQuitMessage was called              */
static WORD     g_WowMsgQuitCode = 0;
/* ── s92 (#306): ONE RING, BUT EVERY TASK SEES ONLY ITS OWN MESSAGES. ─────────
     The note at the top came true: with the run queue, Calc's GetMessage took a
     message WinHelp had posted to ITS OWN window, and DispatchMessage ran WinHelp's
     window procedure on Calc's stack -- SS != DS, a near pointer to a local read
     garbage, #GP. Win16 queues are per task, so a take is too: `g_WowMsgTaker` is
     the task asking (0 = no filter: the modal loops and BeginPaint keep their
     old behaviour), `g_WowMsgOwner` names a window's task (0 = unknown, anyone's).
     A thread message (hwnd 0) is anyone's too. And PostQuitMessage ends the loop
     of the task that called it, not every task's. */
static WORD     g_WowMsgTaker = 0;
static WORD   (*g_WowMsgOwner)(WORD window) = 0;
/* ⚠ ONE PENDING QUIT PER TASK, CLEARED WHEN IT IS TAKEN -- Win16's queue flag. A
     single slot lost Calc's: Calc posted its quit, WinHelp (closing because Calc
     told it to) posted its own before Calc collected, WinHelp got WM_QUIT and Calc
     waited forever with the host alive (#306's own symptom). Cleared on delivery
     so a later task on the same TDB selector does not inherit it. */
#define WOWMSG_MAXQUIT 8
typedef struct _WOWMSG_QUIT { WORD Task, Code; } WOWMSG_QUIT;
static WOWMSG_QUIT g_WowMsgQuits[WOWMSG_MAXQUIT];
static INT      g_WowMsgQuitCount = 0;

static INT WowMsgIsFor(WORD window, WORD task)
{
    WORD owner;
    if (!task || !window || !g_WowMsgOwner) return 1;
    owner = g_WowMsgOwner(window);
    return !owner || owner == task;
}
static VOID WowMsgPostQuit(WORD task, WORD code)
{
    INT index;
    for (index = 0; index < g_WowMsgQuitCount; ++index) if (g_WowMsgQuits[index].Task == task) break;
    if (index == g_WowMsgQuitCount) {
        if (g_WowMsgQuitCount == WOWMSG_MAXQUIT) {          /* full: the oldest goes */
            for (index = 1; index < g_WowMsgQuitCount; ++index) g_WowMsgQuits[index - 1] = g_WowMsgQuits[index];
            --g_WowMsgQuitCount;
        }
        index = g_WowMsgQuitCount++;
    }
    g_WowMsgQuits[index].Task = task; g_WowMsgQuits[index].Code = code;
    g_WowMsgIsQuit = 1; g_WowMsgQuitCode = code;
}
/* 1 + the index of `task`'s pending quit (a task-0 quit is anyone's), or 0. */
static INT WowMsgQuitFor(WORD task)
{
    INT index;
    for (index = 0; index < g_WowMsgQuitCount; ++index)
        if (!task || !g_WowMsgQuits[index].Task || g_WowMsgQuits[index].Task == task) return index + 1;
    return 0;
}
/* Deliver it: the exit code, and the flag is cleared. */
static WORD WowMsgTakeQuit(INT quitNumber)
{
    WORD code;
    INT index;
    if (quitNumber < 1 || quitNumber > g_WowMsgQuitCount) return 0;
    code = g_WowMsgQuits[quitNumber - 1].Code;
    for (index = quitNumber; index < g_WowMsgQuitCount; ++index) g_WowMsgQuits[index - 1] = g_WowMsgQuits[index];
    --g_WowMsgQuitCount;
    g_WowMsgIsQuit = g_WowMsgQuitCount > 0;
    return code;
}
/* ★ WHO A KEYSTROKE IS FOR. Win16 sends keyboard input to the focus window, and
     SYSEDIT sets one (USER 0x16 SETFOCUS, four times in a launch). With no
     focus the target is 0, and USER's own DispatchMessage `jcxz`es a null hwnd
     -- so a key with nowhere to go is discarded BY THE GUEST, correctly, and
     this host does not have to invent a destination. */
static WORD     g_WowMsgFocus = 0;

/* Put one message in the queue. ⚠ CALLED FROM THE UI THREAD as well as the exec
   thread (a keystroke arrives on whichever thread owns the host window), so
   every caller must hold the host lock -- there is no lock in here, on purpose,
   because this file must not know how the host serialises itself. */
static INT WowMsgPost(WORD window, WORD message, WORD wParam, DWORD lParam,
                       DWORD time, WORD pointX, WORD pointY)
{
    PWOWMSG entry;
    if (g_WowMsgCount >= WOWMSG_MAX) { ++g_WowMsgDropped; return 0; }
    entry = &g_WowMsgRing[g_WowMsgTail];
    entry->Window = window; entry->Message = message; entry->WParam = wParam; entry->LParam = lParam;
    entry->Time = time; entry->PointX = pointX; entry->PointY = pointY;
    g_WowMsgTail = (g_WowMsgTail + 1) % WOWMSG_MAX;
    ++g_WowMsgCount; ++g_WowMsgPosted;
    return 1;
}

/* ── ★★★★★ MOUSE MOVES COALESCE. ────────────────────────────────────────────
     wowwin.h used to relay no mouse input at all, and said why: "posting every
     message would fill the ring with mouse moves the guest never asked for and
     would hide the ones it did". That is a real hazard and it is the reason a
     Win16 program cannot draw without this -- but the answer is not to drop the
     mouse, it is what Windows itself does: keep only the NEWEST pending
     WM_MOUSEMOVE per window. A position is not a history; an old one is
     worthless the moment a newer one exists, and a button press is never
     coalesced away because only moves are folded.
   ⚠ ONLY A MOVE ALREADY AT THE TAIL IS REPLACED. Folding a move that sits
     BEHIND a button press would reorder input -- the guest would see the click
     at a position the pointer had not reached yet -- so the scan stops at the
     newest entry for that window.
   Returns 1 if it folded into an existing entry. */
static INT WowMsgPostMove(WORD window, WORD message, WORD wParam, DWORD lParam,
                            DWORD time, WORD pointX, WORD pointY)
{
    INT newest;
    if (g_WowMsgCount) {
        newest = (g_WowMsgTail + WOWMSG_MAX - 1) % WOWMSG_MAX;    /* the newest entry */
        if (g_WowMsgRing[newest].Window == window && g_WowMsgRing[newest].Message == message) {
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

/* Take the oldest message matching the filter, or return 0.
   `window` 0 means "any window", which is what every loop this host has read
   passes. `isRemove` 0 is PeekMessage's look-without-taking.
   ⚠ THE FILTER IS FIRST-MATCH, NOT SCAN-AND-COMPACT: with a filter set and a
     non-matching message at the head, this answers "nothing", which is what a
     single-consumer queue with one producer can honestly say. A run that needs
     better will show a filtered call in the log, and there is not one yet. */
/* ── ★★★★★ A FILTERED TAKE MUST SCAN THE WHOLE QUEUE, NOT JUST THE HEAD. ─────
     This used to look at `g_WowMsgRing[g_WowMsgHead]` ALONE and give up if it did not
     match the filter -- which is not a slow PeekMessage, it is a DEADLOCK. A
     Win16 program that captures the mouse pumps
     `PeekMessage(hwnd, WM_MOUSEFIRST, WM_MOUSELAST)` waiting for its button-up,
     and nothing else is draining the queue; so if the head holds anything that
     does not match -- a WM_TIMER, a WM_PAINT, a move for another window -- the
     head can only be removed by a matching take, and no take can ever match.
     The loop spins forever.
   ★ MINESWEEPER'S SMILEY IS EXACTLY THAT LOOP. It presses (SetCapture, draws the
     pressed face, ClientToScreen twice) and then peeks for the button-up that is
     sitting in the ring behind a message it will not accept. The game never
     resets, which is what the user reported; the host looked idle and the log
     said "queue empty" while `g_WowMsgCount` was not zero.
   ⚠ AND THAT LOG LINE WAS PART OF THE PROBLEM -- it asserted "empty" for what
     was really "nothing matched", so the one number that would have named this
     was never printed. It says both now.
     Removing from the middle keeps ORDER: shift the entries ahead of the match
     up one slot and advance the head, which is a rotate of the prefix rather
     than a swap -- a swap would deliver messages out of order, and message order
     is the one thing a Win16 program is entitled to assume. */
/* ── #160: A MENU THAT WAITS FOR ITS APPLICATION. See wowwin.h, WM_SYSCOMMAND.
     The marker is posted behind the WM_INITMENU/WM_INITMENUPOPUP it follows; when
     a take reaches it, every one of those has been handed to the guest, so the
     menu can open. It is ours, never the guest's: whichever take passes it
     consumes it -- filtered or not, peek or not -- and leaves the replay due for
     the GetMessage/PeekMessage service to run (not here: the menu loop posts into
     this very ring). 0xBF7E sits in a range Win16 never allocates. */
#define WOWMSG_MENUREPLAY 0xBF7Eu
static INT      g_WowMsgIsReplayDue = 0;
static WOWMSG g_WowMsgReplay;

static INT WowMsgTake(WORD window, WORD filterMin, WORD filterMax, INT isRemove, PWOWMSG output)
{
    INT position, index;
    if (!g_WowMsgCount) return 0;
    for (position = 0; position < g_WowMsgCount; ++position) {
        PWOWMSG entry = &g_WowMsgRing[(g_WowMsgHead + position) % WOWMSG_MAX];
        if (entry->Message == WOWMSG_MENUREPLAY) {
            g_WowMsgReplay = *entry; g_WowMsgIsReplayDue = 1;
            for (index = position; index > 0; --index)
                g_WowMsgRing[(g_WowMsgHead + index) % WOWMSG_MAX] =
                    g_WowMsgRing[(g_WowMsgHead + index - 1) % WOWMSG_MAX];
            g_WowMsgHead = (g_WowMsgHead + 1) % WOWMSG_MAX;
            --g_WowMsgCount;
            --position;                          /* the next entry now sits at n */
            continue;
        }
        if (window && entry->Window != window) continue;
        if ((filterMin || filterMax) && (entry->Message < filterMin || entry->Message > filterMax)) continue;
        if (!WowMsgIsFor(entry->Window, g_WowMsgTaker)) continue;          /* s92 #306 */
        *output = *entry;
        if (isRemove) {
            for (index = position; index > 0; --index)
                g_WowMsgRing[(g_WowMsgHead + index) % WOWMSG_MAX] =
                    g_WowMsgRing[(g_WowMsgHead + index - 1) % WOWMSG_MAX];
            g_WowMsgHead = (g_WowMsgHead + 1) % WOWMSG_MAX;
            --g_WowMsgCount; ++g_WowMsgTaken;
        }
        return 1;
    }
    return 0;
}

/* s92 (#306): how many queued messages are `task`'s (all of them for task 0). */
static INT WowMsgCountFor(WORD task)
{
    INT position, count = 0;
    for (position = 0; position < g_WowMsgCount; ++position)
        if (WowMsgIsFor(g_WowMsgRing[(g_WowMsgHead + position) % WOWMSG_MAX].Window, task)) ++count;
    return count;
}

/* Read an 18-byte MSG back out of guest memory -- the guest owns this one; it is
   the buffer GetMessage filled and the loop then handed to DispatchMessage. Only
   the four fields a window procedure is called with are taken. */
static VOID WowMsgRead(const volatile BYTE *bytes, PWOWMSG message)
{
    message->Window   = (WORD)(bytes[WOWMSG_FIELD_HWND]    | (bytes[WOWMSG_FIELD_HWND + 1]    << WOW_BYTE_SHIFT));
    message->Message    = (WORD)(bytes[WOWMSG_FIELD_MESSAGE] | (bytes[WOWMSG_FIELD_MESSAGE + 1] << WOW_BYTE_SHIFT));
    message->WParam = (WORD)(bytes[WOWMSG_FIELD_WPARAM]  | (bytes[WOWMSG_FIELD_WPARAM + 1]  << WOW_BYTE_SHIFT));
    message->LParam = (DWORD)(bytes[WOWMSG_FIELD_LPARAM] | (bytes[WOWMSG_FIELD_LPARAM + 1] << WOW_BYTE_SHIFT))
              | ((DWORD)(bytes[WOWMSG_FIELD_LPARAM + WOW_WORD_BYTES] | (bytes[WOWMSG_FIELD_LPARAM + WOW_WORD_BYTES + 1] << WOW_BYTE_SHIFT)) << WOW_WORD_SHIFT);
    message->Time   = 0; message->PointX = 0; message->PointY = 0;
}

/* Write an 18-byte MSG through the far pointer the guest handed us. */
static VOID WowMsgWrite(volatile BYTE *bytes, PCWOWMSG message)
{
    Wow32PokeWord(bytes + WOWMSG_FIELD_HWND,    message->Window);
    Wow32PokeWord(bytes + WOWMSG_FIELD_MESSAGE, message->Message);
    Wow32PokeWord(bytes + WOWMSG_FIELD_WPARAM,  message->WParam);
    Wow32PokeWord(bytes + WOWMSG_FIELD_LPARAM,     (WORD)(message->LParam & WORD_MASK));
    Wow32PokeWord(bytes + WOWMSG_FIELD_LPARAM + WOW_WORD_BYTES, (WORD)(message->LParam >> WOW_WORD_SHIFT));
    Wow32PokeWord(bytes + WOWMSG_FIELD_TIME,       (WORD)(message->Time & WORD_MASK));
    Wow32PokeWord(bytes + WOWMSG_FIELD_TIME + WOW_WORD_BYTES,   (WORD)(message->Time >> WOW_WORD_SHIFT));
    Wow32PokeWord(bytes + WOWMSG_FIELD_POINT,     message->PointX);
    Wow32PokeWord(bytes + WOWMSG_FIELD_POINT + WOW_WORD_BYTES, message->PointY);
}

#endif /* NTVDMEX_WOWMSG_H */
