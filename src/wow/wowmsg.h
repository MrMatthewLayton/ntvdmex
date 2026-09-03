#ifndef WOWMSG_H
#define WOWMSG_H
/*
 * wowmsg.h -- ★ THE WIN16 MESSAGE QUEUE. GH #128, session 41.
 *
 * ── WHY THIS IS THE FRONTIER ────────────────────────────────────────────────
 * SYSEDIT builds its whole interface, loads its four files, and then does this,
 * every call in it named from its own relocation chain (`tools/ne/neimports.py`):
 *
 *     seg1:0x0102  GetMessage(&msg, 0, 0, 0)          ; USER.108
 *          0x0112  or ax,ax / jne 0x00c6              ; ★ 0 IS WM_QUIT
 *          0x00c6  TranslateMDISysAccel([0x22], &msg) ; USER.451
 *          0x00d8  TranslateAccelerator([0x20], [0x4ac], &msg)
 *          0x00ee  TranslateMessage(&msg)             ; USER.113
 *          0x00f8  DispatchMessage(&msg)              ; USER.114
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
 * answered, the loop turns and the two calls arrive here as ordinary WOW32 BOPs,
 * naming themselves by their call sites -- which are named in turn from SYSEDIT's
 * own relocation chain, so nothing in this chain is inferred:
 *
 *     id 0x071  4 arg bytes  from sysedit seg1:0x00f8  = TRANSLATEMESSAGE
 *     id 0x072  4 arg bytes  from sysedit seg1:0x0102  = DISPATCHMESSAGE
 *     id 0x0b2  8 arg bytes  from sysedit seg1:0x00ea  = TRANSLATEACCELERATOR
 *     id 0x1c3  6 arg bytes  from sysedit seg1:0x00d4  = TRANSLATEMDISYSACCEL
 *
 * They are four of the 56 ids the map could not name from the export table, and
 * the reason the `from` address is the APPLICATION's rather than USER's is that
 * USER's exports reach their stubs by TAIL-JUMP, not by call -- which the map
 * already says, and which is what makes a call site name a stub at all.
 * ⇒ So the host fills the MSG **and** dispatches it. `DispatchMessage` is
 *   `wowcall_enter` into the window's procedure, i.e. machinery session 40
 *   already built.
 *
 * ── THE MSG, READ OUT OF CODE RATHER THAN A HEADER ──────────────────────────
 * `DispatchMessage` takes nothing but `lpMsg`, so everything a window procedure
 * is called with is in those 18 bytes. The size is the application's own
 * declaration -- `sysedit seg1:0x0102 lea ax,[bp-0x12]` reserves exactly 18 --
 * and the field order is USER.EXE's, in a body that walks one:
 *
 *     user seg1:0x1c43  mov bx,0x12          ; ★ sizeof(MSG) == 18, twice over
 *              0x1c4d   mov cx, es:[bx]      ; hwnd
 *              0x1c50   jcxz 0x1c78          ; ★ hwnd 0 -> dispatch nothing
 *              0x1c52   push cx / es:[bx+2] / es:[bx+4] / es:[bx+8] / es:[bx+6]
 *                                            ;   message, wParam, lParam hi, lo
 *
 *   +0x00 WORD  hwnd        +0x06 DWORD lParam
 *   +0x02 WORD  message     +0x0a DWORD time
 *   +0x04 WORD  wParam      +0x0e POINT pt (two WORDs)
 *
 * ⚠ `time` and `pt` are NOT pinned by the code above -- nothing this host has
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
   guest (WM_CREATE, WM_MDICREATE, EM_*) or is the one the loop's own `or ax,ax`
   is testing for. */
#define WM_QUIT16       0x0012
#define WM_KEYDOWN16    0x0100
#define WM_KEYUP16      0x0101
/* ── ★★★ WM_COMMAND, READ OUT OF THE GUESTS RATHER THAN OUT OF A HEADER ───────
     Two independent readings, neither of them Notepad's:
       `commdlg seg3:0x0966`  cmp ax,0x110 / jne / jmp
                     0x096e   cmp ax,0x111 / jne / jmp
         -- a dialog procedure's message chain, and 0x110 immediately before
            0x111 is WM_INITDIALOG immediately before WM_COMMAND. No other pair
            of adjacent numbers is the pair every dialog procedure handles.
       `sysedit seg1:0x0477`  push [0x24] / push 0x111 / push 0x7d8
                              / push 0 / push 0 / lcall <SendMessage>
         -- a program sending ITSELF one, with a menu item id in wParam.
   ★ AND THAT SECOND SITE PINS THE PACKING TOO, which is the part that differs
     between Win16 and Win32: for a MENU command Win16 puts the id alone in
     wParam and ZERO in lParam. See the translation in wowwin.h. */
#define WM_COMMAND16    0x0111

/* MSG field offsets -- see the note above. */
#define MSG_HWND        0x00
#define MSG_MESSAGE     0x02
#define MSG_WPARAM      0x04
#define MSG_LPARAM      0x06
#define MSG_TIME        0x0a
#define MSG_PT          0x0e
#define MSG_SIZE        0x12

/* ── The argument blocks. Reversed as always (the base is the LAST push), and
     GetMessage's is confirmed against a line this host has already printed:
     `args=0x0a b=(0x0000 0x0000 0x0000 0x248a 0x0a9f)` from `sysedit
     seg1:0x0112`, whose pushes are `lea ax,[bp-0x12] / push ss / push ax /
     push 0 / push 0 / push 0`. +6/+8 is that stack MSG, and +4/+2/+0 are the
     three zeroes. A wrong assignment does not produce a readable pointer. */
#define GM_ARG_MAX      0
#define GM_ARG_MIN      2
#define GM_ARG_HWND     4
#define GM_ARG_LPMSG    6

#define PM_ARG_REMOVE   0
#define PM_ARG_MAX      2
#define PM_ARG_MIN      4
#define PM_ARG_HWND     6
#define PM_ARG_LPMSG    8

/* PostMessage(hWnd, msg, wParam, lParam) -- the same 10 bytes, in the same
   order, as SendMessage's block, which this host already reads. */
#define PSM_ARG_LPARAM  0
#define PSM_ARG_WPARAM  4
#define PSM_ARG_MSG     6
#define PSM_ARG_HWND    8

#define PQM_ARG_EXITCODE 0
#define SF_ARG_HWND      0

/* The rest of the loop, every offset confirmed against the args this host has
   already printed for the call:
     DispatchMessage(lpMsg)                       (0x248a 0x0a9f)
     TranslateMessage(lpMsg)                      (0x248a 0x0a9f)
     TranslateAccelerator(hWnd, hAccel, lpMsg)    (0x248a 0x0a9f 0x0a8e 0x0140)
     TranslateMDISysAccel(hWndClient, lpMsg)      (0x248a 0x0a9f 0x0160)
   -- 0x0a8e is the handle LoadAccelerators returned and 0x0140/0x0160 are the
   frame and MDI-client windows this host issued, so three of the four values are
   ones we can recognise. */
#define DM_ARG_LPMSG     0
#define TA_ARG_LPMSG     0
#define TA_ARG_HACCEL    4
#define TA_ARG_HWND      6
#define TMSA_ARG_LPMSG   0
#define TMSA_ARG_HWND    4

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
static DWORD g_wowmsg_wait_ms = WOWMSG_WAIT_MS;
static int   g_wm_saidwait    = 0;   /* the setting is announced once, at first use */

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
static volatile LONG g_wm_inwait = 0;

typedef struct {
    WORD  hwnd, msg, wparam;
    DWORD lparam;
    DWORD time;
    WORD  ptx, pty;
} wowmsg_t;

static wowmsg_t g_wm_ring[WOWMSG_MAX];
static int      g_wm_head = 0;      /* next to take */
static int      g_wm_tail = 0;      /* next to fill */
static int      g_wm_count = 0;
static DWORD    g_wm_posted = 0;    /* how many went in, for the run summary   */
static DWORD    g_wm_taken  = 0;    /* ...and how many came out                */
static DWORD    g_wm_dropped = 0;   /* ring full -- LOUD, see WOWMSG_MAX       */
static int      g_wm_quit = 0;      /* PostQuitMessage was called              */
static WORD     g_wm_quitcode = 0;
/* ★ WHO A KEYSTROKE IS FOR. Win16 sends keyboard input to the focus window, and
     SYSEDIT sets one (USER 0x16 SETFOCUS, four times in a launch). With no
     focus the target is 0, and USER's own DispatchMessage `jcxz`es a null hwnd
     -- so a key with nowhere to go is discarded BY THE GUEST, correctly, and
     this host does not have to invent a destination. */
static WORD     g_wm_focus = 0;

/* Put one message in the queue. ⚠ CALLED FROM THE UI THREAD as well as the exec
   thread (a keystroke arrives on whichever thread owns the host window), so
   every caller must hold the host lock -- there is no lock in here, on purpose,
   because this file must not know how the host serialises itself. */
static int wowmsg_post(WORD hwnd, WORD msg, WORD wparam, DWORD lparam,
                       DWORD time, WORD ptx, WORD pty)
{
    wowmsg_t *e;
    if (g_wm_count >= WOWMSG_MAX) { ++g_wm_dropped; return 0; }
    e = &g_wm_ring[g_wm_tail];
    e->hwnd = hwnd; e->msg = msg; e->wparam = wparam; e->lparam = lparam;
    e->time = time; e->ptx = ptx; e->pty = pty;
    g_wm_tail = (g_wm_tail + 1) % WOWMSG_MAX;
    ++g_wm_count; ++g_wm_posted;
    return 1;
}

/* Take the oldest message matching the filter, or return 0.
   `hwnd` 0 means "any window", which is what every loop this host has read
   passes. `remove` 0 is PeekMessage's look-without-taking.
   ⚠ THE FILTER IS FIRST-MATCH, NOT SCAN-AND-COMPACT: with a filter set and a
     non-matching message at the head, this answers "nothing", which is what a
     single-consumer queue with one producer can honestly say. A run that needs
     better will show a filtered call in the log, and there is not one yet. */
static int wowmsg_take(WORD hwnd, WORD minf, WORD maxf, int remove, wowmsg_t *out)
{
    wowmsg_t *e;
    if (!g_wm_count) return 0;
    e = &g_wm_ring[g_wm_head];
    if (hwnd && e->hwnd != hwnd) return 0;
    if ((minf || maxf) && (e->msg < minf || e->msg > maxf)) return 0;
    *out = *e;
    if (remove) {
        g_wm_head = (g_wm_head + 1) % WOWMSG_MAX;
        --g_wm_count; ++g_wm_taken;
    }
    return 1;
}

/* Read an 18-byte MSG back out of guest memory -- the guest owns this one; it is
   the buffer GetMessage filled and the loop then handed to DispatchMessage. Only
   the four fields a window procedure is called with are taken. */
static void wowmsg_read(const volatile BYTE *p, wowmsg_t *m)
{
    m->hwnd   = (WORD)(p[MSG_HWND]    | (p[MSG_HWND + 1]    << 8));
    m->msg    = (WORD)(p[MSG_MESSAGE] | (p[MSG_MESSAGE + 1] << 8));
    m->wparam = (WORD)(p[MSG_WPARAM]  | (p[MSG_WPARAM + 1]  << 8));
    m->lparam = (DWORD)(p[MSG_LPARAM] | (p[MSG_LPARAM + 1] << 8))
              | ((DWORD)(p[MSG_LPARAM + 2] | (p[MSG_LPARAM + 3] << 8)) << 16);
    m->time   = 0; m->ptx = 0; m->pty = 0;
}

/* Write an 18-byte MSG through the far pointer the guest handed us. */
static void wowmsg_write(volatile BYTE *p, const wowmsg_t *m)
{
    wow32_pokew(p + MSG_HWND,    m->hwnd);
    wow32_pokew(p + MSG_MESSAGE, m->msg);
    wow32_pokew(p + MSG_WPARAM,  m->wparam);
    wow32_pokew(p + MSG_LPARAM,     (WORD)(m->lparam & 0xFFFF));
    wow32_pokew(p + MSG_LPARAM + 2, (WORD)(m->lparam >> 16));
    wow32_pokew(p + MSG_TIME,       (WORD)(m->time & 0xFFFF));
    wow32_pokew(p + MSG_TIME + 2,   (WORD)(m->time >> 16));
    wow32_pokew(p + MSG_PT,     m->ptx);
    wow32_pokew(p + MSG_PT + 2, m->pty);
}

#endif /* WOWMSG_H */
