#ifndef NTVDMEX_WOWWIN_H
#define NTVDMEX_WOWWIN_H
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
 *   RegisterClass   a real Win32 class whose lpfnWndProc is OURS (WowWinProc)
 *   CreateWindow    a real CreateWindowExA; the HWND is kept in WOWUSER_WINDOW
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
 *   - cheaply at every WOW32 BOP (`WowWinPump`), so the window stays alive while
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
 *   to `WowWinProc` during `CreateWindowEx`, about the real window; the guest's
 *   own `WM_CREATE`, about its object, is delivered afterwards by
 *   `WowUserWantCreate` through the callback machinery. Conflating them would
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
#define CW_USEDEFAULT32     ((INT)0x80000000)

/* Win16 message numbers relayed here (Win16 and Win32 agree on each). */
#define WM_CHAR16             0x0102
#define WM_INITMENU16         0x0116
#define WM_INITMENUPOPUP16    0x0117
#define WM_DROPFILES16        0x0233
#define WOWWIN_REGISTERED_MESSAGE_FIRST 0xC000   /* RegisterWindowMessage's range */
#define WOWWIN_SC_MASK        0xFFF0             /* WM_SYSCOMMAND: the low four bits are the system's */
#define WOWWIN_SCAN_CODE_SHIFT 16                /* a key message's lParam: bits 16..23 */
#define WOWWIN_SCAN_CODE_MASK  0xFF
#define WOWWIN_MAX_SENDING    8
#define WOWWIN_SIZE_REENTRY   2                  /* WM_SIZE may nest once (see WowWinSendOrPost) */
#define WOWWIN_MENU_POPUPS_MAX 32
#define WOWWIN_LOG_LINE_MAX   160
#define WOWWIN_REASON_MAX     96
#define WOWWIN_DROP_LOG_MAX   200
#define WOWWIN_PAINT_LOG_MAX  600
#define WOWWIN_MM_LOG_MAX     12

/* A Win16 DROPFILES block (krnl386 global memory): pFiles, pt, fNC, then the names. */
#define WOWWIN_DROP_BUFFER        2048
#define WOWWIN_DROPFILES_HEADER   8
#define WOWWIN_DROPFILES_POINT_X  2
#define WOWWIN_DROPFILES_POINT_Y  4
#define WOWWIN_DROPFILES_NONCLIENT 6
#define WOWWIN_DROP_TERMINATORS   2      /* the name's NUL and the list's */
#define WOWWIN_DRAGQUERY_COUNT    0xFFFFFFFFu
#define WOWWIN_GMEM_SHARE_MOVEABLE_ZEROINIT 0x2042

/* A Win16 MINMAXINFO: five 16-bit POINTs, the first reserved. */
#define WOWWIN_MINMAXINFO16_SIZE  20
#define WOWWIN_MINMAXINFO_POINTS  5
#define WOWWIN_POINT16_SIZE       4
#define WOWWIN_POINT16_Y          2

/* Forward: the window table this proc maps through. All defined in wowuser.h,
   which owns the table and is included after this file. */
typedef struct _WOWUSER_WINDOW WOWUSER_WINDOW, *PWOWUSER_WINDOW; typedef const WOWUSER_WINDOW *PCWOWUSER_WINDOW;

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
 *   pending move per window is kept (WowMsgPostMove). Leaving the mouse out
 *   was why a paint program could be looked at but not used.
 * ★ WM_PAINT IS NOW TRANSLATED (session 45) -- this note used to say it was left
 *   to DefWindowProc "until GDI's id space is dispatched", and that day came.
 *   ⚠⚠ But the region is STILL validated here, and that is not optional: Win32
 *   SYNTHESISES WM_PAINT for as long as the window has an update region, so
 *   relaying it and returning 0 live-locks the host. See the case itself.
 */
/* ── ★ THE PENDING-PAINT RECORD ──────────────────────────────────────────────
     One rectangle per window, because the OS's update region is consumed in
     WowWinProc (see WM_PAINT there) and the guest asks for it later, out of its
     own message loop. A second WM_PAINT arriving before the guest has answered
     the first UNIONS with what is already pending rather than replacing it --
     replacing would silently drop the area from the earlier one, which is the
     kind of loss that shows up as "it only redraws sometimes". */
#define WOWWIN_MAXPAINT 32
typedef struct _WOWWIN_PAINT { WORD Window16; RECT Rect; INT IsErase; INT IsPending; } WOWWIN_PAINT, *PWOWWIN_PAINT;

#define WOWWIN_GLOBAL16_ALLOC  0
#define WOWWIN_GLOBAL16_FREE   1
#define WOWWIN_GLOBAL16_LOCK   2
#define WOWWIN_GLOBAL16_UNLOCK 3
DWORD DpmiSelectorBase(WORD selector);            /* main.c: a selector's linear base */

/* Send if the nested run can, else post -- the M9 messages' delivery. */
/* ⚠ NOT RE-ENTRANTLY (s91, the final regression run): maximizing an MDI child SENT
     WM_SIZE / WM_GETMINMAXINFO into its procedure, which chains through DefMDIChildProc
     to the real MDI client, which re-sizes the child, which came back here for the
     same window and message -- w_mdi stopped dead at WM_MDIMAXIMIZE. A message already
     being sent to a window is POSTED instead, as before s91. */
typedef struct _WOWWIN_SENDING { WORD Window16, Message; } WOWWIN_SENDING;
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
#define WOWWIN_MAX_HELD_CHARS 16
typedef struct _WOWWIN_HELD_CHAR { WORD Window16, Character; DWORD LParam; DWORD Sequence; } WOWWIN_HELD_CHAR;

/* ── s93: TIMERS WITH NO WINDOW. Win16's SetTimer(NULL, 0, ms, proc) is legal and
     common in a program that has no window of its own to time with -- RECORDER, while
     recording, times itself this way, and was answered 0 ("refused"). Win32 has the
     same thing: a THREAD timer, whose WM_TIMER arrives with hwnd NULL. The pumps turn
     that into a Win16 WM_TIMER with hwnd 0, the timer's id in wParam and the TIMERPROC
     in lParam -- exactly what Win16 queues -- and DispatchMessage calls the proc. */
#define WOWWIN_MAX_THREAD_TIMERS 16
typedef struct _WOWWIN_THREAD_TIMER { UINT_PTR Id32; DWORD Procedure; } WOWWIN_THREAD_TIMER;

/* Defined in wowwin.c (#335). */
extern DWORD (*g_WowWinGlobal16)(INT operation, DWORD first, DWORD second);
extern DWORD g_WowWinThread;
extern DWORD g_WowWinPumped;
INT WowWinThreadTimerFire(const MSG *message);
extern DWORD g_WowWinMessages;
extern DWORD g_WowWinCreated;
extern DWORD g_WowWinPaintMs;
INT WowWinPaintTake(WORD window16, PRECT output, PINT isErase);
extern INT g_WowWinIsDialogBounced;
extern INT g_WowWinInDialogMessage;
extern HWND g_WowWinDialogWindow;
extern UINT g_WowWinDialogMessage;
INT WowWinReleaseChars(WORD window16, DWORD keyLParam);
INT WowWinThreadTimerAdd(UINT_PTR id32, DWORD procedure);
INT WowWinThreadTimerKill(UINT_PTR id32);
extern UINT g_WowWinSetFocusCount;
extern HWND g_WowWinSetFocusWindow;
VOID WowWinMenuReplay(PCWOWMSG replay);
INT WowWinRegister(PCSTR name16, PSTR className32, INT capacity, HCURSOR cursor, HICON icon, HICON smallIcon, PINT isCursorDefaulted, HBRUSH background);
INT WowWinCoordinate(WORD value);
extern LRESULT (*g_WowWinCtlColor)(HWND window, WORD window16, UINT message, WPARAM wParam, LPARAM lParam, PINT isHandled);
extern INT (*g_WowWinSend16)(WORD window16, WORD message, WORD wParam, DWORD lParam, PWORD result);
extern LRESULT (*g_WowWinOwnerDraw)(HWND window, WORD window16, UINT message, WPARAM wParam, LPARAM lParam, PINT isHandled);
extern INT (*g_WowWinSend16Blob)(WORD window16, WORD message, WORD wParam, PBYTE blob, INT blobLength, const INT *fixups, INT fixupCount, PWORD result);
extern LONGLONG g_WowWinPumpTicks;
extern DWORD g_WowWinPumpCalls;
INT WowWinPump(INT budget);
#endif /* NTVDMEX_WOWWIN_H */
