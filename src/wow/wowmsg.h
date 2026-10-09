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

typedef struct _WOWMSG {
    WORD  Window, Message, WParam;
    DWORD LParam;
    DWORD Time;
    WORD  PointX, PointY;
} WOWMSG, *PWOWMSG;
typedef const WOWMSG *PCWOWMSG;

/* ⚠ ONE PENDING QUIT PER TASK, CLEARED WHEN IT IS TAKEN -- Win16's queue flag. A
     single slot lost Calc's: Calc posted its quit, WinHelp (closing because Calc
     told it to) posted its own before Calc collected, WinHelp got WM_QUIT and Calc
     waited forever with the host alive (#306's own symptom). Cleared on delivery
     so a later task on the same TDB selector does not inherit it. */
#define WOWMSG_MAXQUIT 8
typedef struct _WOWMSG_QUIT { WORD Task, Code; } WOWMSG_QUIT;

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

/* Defined in wowmsg.c (#335). */
INT WowMsgPost(WORD window, WORD message, WORD wParam, DWORD lParam, DWORD time, WORD pointX, WORD pointY);
extern DWORD g_WowMsgWaitMs;
extern volatile LONG g_WowMsgInWait;
extern INT g_WowMsgCount;
INT WowMsgTake(WORD window, WORD filterMin, WORD filterMax, INT isRemove, PWOWMSG output);
extern DWORD g_WowMsgPosted;
extern DWORD g_WowMsgTaken;
extern WORD g_WowMsgQuitCode;
extern WORD g_WowMsgTaker;
extern WORD (*g_WowMsgOwner)(WORD window);
VOID WowMsgPostQuit(WORD task, WORD code);
INT WowMsgQuitFor(WORD task);
WORD WowMsgTakeQuit(INT quitNumber);
extern WORD g_WowMsgFocus;
extern INT g_WowMsgIsReplayDue;
extern WOWMSG g_WowMsgReplay;
VOID WowMsgRead(const volatile BYTE *bytes, PWOWMSG message);
VOID WowMsgWrite(volatile BYTE *bytes, PCWOWMSG message);
INT WowMsgPostMove(WORD window, WORD message, WORD wParam, DWORD lParam, DWORD time, WORD pointX, WORD pointY);
extern INT g_WowMsgIsWaitAnnounced;
INT WowMsgCountFor(WORD task);
#endif /* NTVDMEX_WOWMSG_H */
