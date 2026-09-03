#ifndef WOWUSER_H
#define WOWUSER_H
/*
 * wowuser.h -- USER.EXE's half of the WOW32 interface. GH #128, session 38.
 *
 * ── WHY THIS IS A SEPARATE FILE AND NOT MORE CASES IN wow32.h ────────────────
 * THE ID SPACE IS PER MODULE. Every id in wow32.h was read out of krnl386's own
 * stub table; USER has its own table of 457 stubs with its own numbering, and the
 * numbers COLLIDE. `0x39` is `GetProfileInt` in krnl386's table and
 * `RegisterClass` in USER's -- and for one run this host serviced the second with
 * the first and handed WOWEXEC the answer. Two id spaces in one switch is how that
 * happens; two files with two dispatchers, chosen by the stub's own segment, is how
 * it stops happening.
 *
 * The surface is enumerated in docs/research/wow-user-surface.md -- 441 ids, 262 of
 * them named by USER's own export table, regenerable with
 *     tools/ne/wowmap.py guest/ne/user.exe --md
 *
 * ── THE Win16 WNDCLASS, CONFIRMED FIELD BY FIELD AGAINST WOWEXEC's OWN CODE ──
 * Not taken from a header. WOWEXEC builds one on its stack and every store lands
 * where this layout says it should:
 *
 *     083b  lea ax,[bp-0x1a]        -> the struct is 0x1A = 26 bytes
 *     0819  lcall LoadCursor
 *     081e  mov [bp-0xc],ax          -> +0x0e  hCursor
 *     0823  lcall <stock object>
 *     0828  mov [bp-0xa],ax          -> +0x10  hbrBackground
 *     082b  mov word [bp-4],0x82     -> +0x16  lpszClassName offset
 *     0830  mov [bp-2],ds            -> +0x18  lpszClassName segment
 *     0833  sub ax,ax / mov [bp-6],ax / mov [bp-8],ax  -> +0x12/+0x14 lpszMenuName = NULL
 *
 *   +0x00 WORD  style          +0x0c WORD hIcon
 *   +0x02 DWORD lpfnWndProc    +0x0e WORD hCursor
 *   +0x06 WORD  cbClsExtra     +0x10 WORD hbrBackground
 *   +0x08 WORD  cbWndExtra     +0x12 DWORD lpszMenuName
 *   +0x0a WORD  hInstance      +0x16 DWORD lpszClassName
 *
 * ⚠ `mov word [bp-4],0x82` is the CLASS NAME POINTER, not a style. An earlier note
 *   read it as "style 0x82, hInstance=ds" off the same two instructions; the offsets
 *   above are what settle it, and they come from the struct size the program itself
 *   declares with that `lea`.
 */

#define WOWUSER_REGISTERCLASS   0x39
#define WOWUSER_CREATEWINDOW    0x29
#define WOWUSER_NOTIFYWOW       0x217
#define WOWUSER_SENDMESSAGE     0x6f
#define WOWUSER_GETWINDOWWORD   0x85
#define WOWUSER_SETWINDOWWORD   0x86
/* The message loop. Every id here is named by USER's own export table
   (docs/research/wow-user-surface.md) and every one of them is a call this run
   already makes -- see the frontier note in src/wow/wowmsg.h. */
#define WOWUSER_POSTQUITMESSAGE 0x06
#define WOWUSER_SETFOCUS        0x16
#define WOWUSER_GETMESSAGE      0x6c
#define WOWUSER_PEEKMESSAGE     0x6d
#define WOWUSER_POSTMESSAGE     0x6e
/* ★ These four the export table could NOT name -- they are four of its 56
   unnamed ids, and they are named here by their call sites in SYSEDIT's own
   message loop, which the relocation chain names in turn. See wowmsg.h. */
#define WOWUSER_TRANSLATEMESSAGE 0x71
#define WOWUSER_DISPATCHMESSAGE  0x72
#define WOWUSER_TRANSLATEACCEL   0xb2
#define WOWUSER_TRANSLATEMDISYS  0x1c3
/* Visibility -- and with a real window behind it, this is the OS's job. */
#define WOWUSER_SHOWWINDOW       0x2a
#define WOWUSER_UPDATEWINDOW     0x7c
/* A name in the system-wide clipboard atom table. CARDFILE and WRITE both stop
   without it -- see the case. One argument: a far pointer to the name. */
#define WOWUSER_REGCLIPFORMAT    0x91
#define RCF_ARG_NAME             0

/* ── ★★★ 0xad -- "BUILD ME A CURSOR OR AN ICON". ─────────────────────────────
     One of the 56 ids USER's export table cannot name, and the reason it matters
     is NOTEPAD: `notepad seg2:0x02d0` calls `LoadCursor(NULL, 0x7f02)` and
     `seg2:0x02ee cmp [0xb14],0 / je` returns 0 from its whole initialisation if
     the answer is 0 -- so `WinMain` (`seg1:0x0c38 or ax,ax / jne`) returns
     without ever registering a class. No cursor, no Notepad.

     USER's 16-bit LoadCursor/LoadIcon do the resource lookup themselves and then
     ask the 32-bit side to build the object. The call site is exact
     (`user seg1:0x49e2`, the arm taken when the module handle came back 0, i.e.
     hInstance was NULL):

         49e2  push ax(0)      -> +18
         49e3  push [bp+8]     -> +16   lpName HIGH word (0 => MAKEINTRESOURCE)
         49e6  push [bp+6]     -> +14   lpName LOW word  = the ORDINAL
         49e9  push ax(0) x5   -> +12..+4
         49ee  push [bp-0x14]  -> +2
         49f1  push 1          -> +0    ★ KIND: a PREDEFINED system object
         49f3  lcall <id 0xad>

     and the run agrees field for field: Notepad's call arrives as
     `(1 0 0 0 0 0 0 0x7f02 0 0)`. The sibling site (`seg1:0x4998`) pushes kind 3
     with a real resource pointer -- a module's OWN icon -- and is NOT answered
     here; the log names the kind so the next run can read that site instead of
     this one being widened by guesswork.

   ★★ WHY THE HOST DOES NOT HAVE TO KNOW WHETHER IT IS A CURSOR OR AN ICON.
     Win16's predefined cursor and icon ordinals share one numeric range, and both
     LoadCursor and LoadIcon reach this same stub -- so the id and its arguments
     genuinely cannot tell them apart, and the two USER exports that would are
     reached through relocation chains rather than fixed call targets. But the
     GUEST says which it is a moment later, when it puts the handle into
     `WNDCLASS.hCursor` or `WNDCLASS.hIcon`. So this returns a TOKEN that remembers
     the ordinal, and the real `LoadCursorA`/`LoadIconA` happens at the point of
     use, where the answer is not a guess. Same shape as every other handle here:
     synthetic to the guest, a real OS object behind it. */
#define WOWUSER_LOADSYSOBJ       0xad
#define AD_ARG_KIND              0
#define AD_ARG_NAMELO            14
#define AD_ARG_NAMEHI            16
#define AD_KIND_PREDEFINED       1

/* Tokens live well above the window handles (0x0100 + n*0x20) so a stray one is
   never mistaken for a window, and vice versa. */
#define WOWUSER_SYSRES_BASE      0x8000
#define WOWUSER_SYSRES_STEP      0x0008
#define WOWUSER_MAX_SYSRES       16

typedef struct {
    WORD h;                          /* 0 = free */
    WORD ord;                        /* the predefined ordinal the guest asked for */
} wowuser_sysres_t;

static wowuser_sysres_t g_wu_sysres[WOWUSER_MAX_SYSRES];
static int              g_wu_nsysres = 0;

/* The ordinal behind a token, or 0 if this is not one of ours. */
static WORD wowuser_sysres_ord(WORD h)
{
    int i;
    if (!h) return 0;
    for (i = 0; i < g_wu_nsysres; ++i)
        if (g_wu_sysres[i].h == h) return g_wu_sysres[i].ord;
    return 0;
}

/* ShowWindow(hWnd, nCmdShow) -- 4 bytes, reversed as always, and confirmed by the
   run: `(0x0005 0x0160)` from sysedit seg1:0x01da and `(0x0001 0x0140)` from
   seg2:0x0149. UpdateWindow(hWnd) -- 2 bytes. */
#define SW_ARG_CMDSHOW  0
#define SW_ARG_HWND     2
#define UW_ARG_HWND     0

/* Get/SetWindowWord argument blocks, reversed as always (base = last push):
     GetWindowWord(hWnd, nIndex)               -> +0 nIndex, +2 hWnd
     SetWindowWord(hWnd, nIndex, wNewWord)     -> +0 value, +2 nIndex, +4 hWnd
   Confirmed against the run: SYSEDIT's `mpchild` WM_CREATE calls
   `push si / push 2 / push 0` and the frame carries `(0x0000 0x0002 <hwnd>)`. */
#define GWW_ARG_INDEX   0
#define GWW_ARG_HWND    2
#define SWW_ARG_VALUE   0
#define SWW_ARG_INDEX   2
#define SWW_ARG_HWND    4

/* ── ★★★ SendMessage(hWnd, msg, wParam, lParam) -- 10 argument bytes ──────────
     `USER.111 SENDMESSAGE`, named from SYSEDIT's own relocation chain at
     `seg3:0x007e`. The block is REVERSED from the parameter list as always (the
     base is the LAST push), and the run confirms every offset in one line:
     `args=(0x2378 0x0a9f 0x0000 0x0220 0x0160)` against the pushes
     `push [0x22] / push 0x220 / push 0 / push ss / push ax`. */
#define SM_ARG_LPARAM   0
#define SM_ARG_WPARAM   4
#define SM_ARG_MSG      6
#define SM_ARG_HWND     8

/* The messages this host names. `0x0220` is what SYSEDIT pushes at
   `seg3:0x0074`, and its lParam is an MDICREATESTRUCT -- see below. */
#define WM_MDICREATE16  0x0220

/*
 * ── ★★★★ AN EDIT CONTROL'S TEXT IS A HANDLE IN THE APPLICATION'S OWN HEAP ────
 * `0x040C` and `0x040D` are `WM_USER + 12` and `WM_USER + 13`, and which is
 * which is settled by SYSEDIT's file-loading routine rather than from memory --
 * `sysedit seg3`, and every call in it is named from the relocation chain:
 *
 *   00cf  OPENFILE                       ; < 0 -> "Cannot open this file"
 *   00f8  _LLSEEK(h, 0, 2)  -> [bp-4]    ; ★ the file's SIZE
 *   010a  _LLSEEK(h, 0, 0)               ; back to the start
 *   011b  SendMessage(hEdit, 0x40D, 0,0) ; ★ so 0x40D takes NO parameters...
 *   0124  push ax / push [bp-4]+1 / push 0x42
 *   012c  LOCALREALLOC                   ; ★ ...and RETURNS A LOCAL HANDLE
 *   0148  LOCALLOCK -> [bp-0x96]         ; a far pointer to the bytes
 *   015a  _LREAD(h, that, size)          ; the file goes straight in
 *   017b  mov byte es:[bx+si],0          ; the guest NUL-terminates it itself
 *   0183  LOCALUNLOCK
 *   018b  SendMessage(hEdit, 0x40C, hMem, 0)  ; ★ 0x40C TAKES the handle
 *
 * ⇒ `0x040D` is **EM_GETHANDLE** and `0x040C` is **EM_SETHANDLE**, and the run
 *   that stopped here was stopping on the FIRST of the pair, not the second.
 *
 * ★★ AND THE HANDLE MUST BE VALID IN THE APPLICATION'S OWN LOCAL HEAP. That is
 *   not an assumption about how Windows implements edit controls -- it is what
 *   this program demonstrably requires: it hands the answer straight to
 *   `LocalReAlloc` and `LocalLock`, which operate on the local heap of the
 *   CURRENT DS, and DS throughout this routine is SYSEDIT's own DGROUP (its
 *   window procedure's prologue put it there). A handle this host invented would
 *   be a number `LocalReAlloc` rejects, and rejecting it is exactly what produced
 *   *"Cannot open this file."*
 * ⇒ The host cannot make this handle. The GUEST'S KERNEL has to, and since
 *   session 40 the host can ask it: `KERNEL.5 LOCALALLOC` is entry-table
 *   `FIXED, segment 1, offset 0x3ddb`, and krnl386's segment 1 is the segment
 *   every WOW32 BOP executes in -- so its runtime address is `<the BOP's CS>:
 *   0x3ddb`, with no resolution machinery at all. The disassembly there confirms
 *   the signature to the byte: `test ax,0xf08d` against `[bp+8]` (LocalAlloc's
 *   own flag validation) and `retf 4` -- two words, far.
 */
#define EM_SETHANDLE16  0x040C
#define EM_GETHANDLE16  0x040D
#define KRNL_LOCALALLOC_OFF 0x3ddb
/* ★ AND ITS NEIGHBOURS, NAMED BY krnl386's OWN NON-RESIDENT NAME TABLE -- so
     these are its names for its own ordinals, not a list from memory:
        5 LOCALALLOC   0x3ddb      7 LOCALFREE    0x3df7
        6 LOCALREALLOC 0x3e1f      8 LOCALLOCK    0x3e0b
                                   9 LOCALUNLOCK  0x3e55
   Both of the ones used here disassemble to `push bp / mov bp,sp / mov bx,[bp+6]
   / call 0x406f / ... / retf 2` -- one WORD argument, far, which is what
   `LocalLock(HLOCAL)` and `LocalUnlock(HLOCAL)` take. */
#define KRNL_LOCALLOCK_OFF   0x3e0b
#define KRNL_LOCALUNLOCK_OFF 0x3e55
/* LMEM_MOVEABLE | LMEM_ZEROINIT -- the same flags SYSEDIT itself passes to
   LocalReAlloc at `seg3:0x012a` (`push 0x42`), so the block it grows is the kind
   it expects to be growing. */
#define LMEM_MOVEABLE_ZEROINIT 0x0042
/* Small on purpose: the guest reallocs it to the file's size before using it, so
   anything bigger would be memory the application immediately replaces. */
#define WOWUSER_EDIT_INITIAL   0x20

/* ── ★★★ NOTIFYWOW: "HERE IS A 16-BIT RESOURCE I HAVE JUST LOADED." ───────────
     Named by USER's own export table (`wowmap.py`: id 0x217, 6 argument bytes,
     stub `seg1:0x12dd`, NOTIFYWOW) and pinned to the byte by its one caller,
     which is USER's whole implementation of `LoadAccelerators`:

       3dcc  push 0 / push 9      \
       3dce  lcall FindResource   /  ★ lpType = 9 = RT_ACCELERATOR
       3dde  lcall LoadResource      -> [bp-4] = hResData
       3df4  lcall LockResource      -> [bp-0xc] = the bytes, 16:16
       3e05  lcall SizeofResource    -> [bp-8]  = how many
       3e10  push 3 / lea ax,[bp-0x10] / push ss / push ax
       3e17  lcall <the 0x217 stub>  ★ THIS CALL
       3e1c  or dx,ax / je 0x3e2a    -> 0 frees the resource and returns NULL
       3e37  mov ax,[bp-4] / retf 6  ★ AND THE HANDLE IT RETURNS IS ITS OWN

   ★ SO THE RETURN IS NOT A HANDLE. `seg1:0x3e37` hands the application
     `[bp-4]` -- krnl386's global handle for the resource -- whichever way this
     call goes. All this answer decides is whether LoadAccelerators SUCCEEDS.
     Returning a fabricated handle here would be inventing a value nobody reads;
     the honest answer is "noted", which is what the function's name says.

   ── The 12-byte block at ss:[bp-0x10], every field from a store above ────────
       +0x00 WORD  hInstance    ( [bp+0x0a], the caller's module )
       +0x02 WORD  hResData     ( LoadResource's handle )
       +0x04 DWORD lpResource   ( LockResource's 16:16 -- the bytes themselves )
       +0x08 DWORD cbResource   ( SizeofResource )

   ⚠⚠ AND lpResource IS STALE THE MOMENT WE RETURN. `seg1:0x3e23` calls
     GlobalUnlock on the very next instruction, so a host that recorded that
     pointer for a later TranslateAccelerator would be keeping an address the
     guest has already released -- an instrument that lies later, which is this
     project's most expensive shape. It is LOGGED, not kept. When accelerators
     are actually implemented, the bytes must be COPIED here, while they are
     locked, or asked for again through FindResource/LoadResource. */
#define WOWNOTIFY_ACCEL         3
#define NOTIFY_ARG_BLOCK        0
#define NOTIFY_ARG_KIND         4
#define NOTIFY_HINSTANCE        0x00
#define NOTIFY_HRESDATA         0x02
#define NOTIFY_LPRESOURCE       0x04
#define NOTIFY_CBRESOURCE       0x08

/*
 * ── ★★★ CreateWindow's ARGUMENT BLOCK, READ OFF WOWEXEC'S OWN PUSHES ─────────
 * Not from a header, and not from the parameter order in the documentation: the
 * arg block grows the opposite way from the pushes, so "lpClassName is the first
 * parameter" says nothing about where it lands. The block base (bp+16) is the
 * LOWEST address, which holds the LAST word pushed -- so the parameter list is
 * reversed, and a DWORD's high word is at the LOWER offset because Pascal pushes
 * it first.
 *
 * WOWEXEC's call site is `wowexec seg1:0x08b2`, and the fifteen pushes ahead of
 * it are exactly 30 bytes -- the `push 0x1e` the USER stub at `seg1:0x038d`
 * declares. Import-table resolution names it outright: `USER.41 CREATEWINDOW`.
 *
 *   push ds      -> +28 |  push 0x8000 -> +14 (y)
 *   push 0xae    -> +26 |  push 0x8000 -> +12 (nWidth)
 *   push [0x16]  -> +24 |  push 0x8000 -> +10 (nHeight)
 *   push [0x14]  -> +22 |  push 0      -> +8  (hWndParent)
 *   push 0x2cf   -> +20 |  push 0      -> +6  (hMenu)
 *   push 0       -> +18 |  push [0x206]-> +4  (hInstance)
 *   push 0x8000  -> +16 |  push 0 / push 0 -> +2/+0 (lpParam)
 *
 * ★ AND THE DATA CROSS-VALIDATES THE LAYOUT, which is why it is trusted:
 *     +26/+28  ds:0x00ae is the string "WOWExecClass" -- the class WOWEXEC has
 *              just registered (a second copy of the literal; the WNDCLASS used
 *              ds:0x0082, and both decode to the same name).
 *     +18/+20  0x02CF0000 = WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN. Read the other
 *              way round it would be 0x000002CF, which is not a window style.
 *     +10..+16 four copies of 0x8000 = CW_USEDEFAULT, exactly where x/y/w/h are.
 *     +4       [0x206], the SAME word WOWEXEC stored into WNDCLASS.hInstance at
 *              `seg1:0x0803`.
 *   Four independent agreements. A wrong offset assignment produces none of them.
 */
#define CW_ARG_LPPARAM      0
#define CW_ARG_HINSTANCE    4
#define CW_ARG_HMENU        6
#define CW_ARG_HWNDPARENT   8
#define CW_ARG_HEIGHT       10
#define CW_ARG_WIDTH        12
#define CW_ARG_Y            14
#define CW_ARG_X            16
#define CW_ARG_STYLE        18
#define CW_ARG_WINDOWNAME   22
#define CW_ARG_CLASSNAME    26

/* Win16's CW_USEDEFAULT, and what this host resolves it to.
   ★ SESSION 42: IT RESOLVES TO Win32's. This used to be a stated placeholder --
     "there is no desktop yet, so there is no honest answer" -- and the answer
     turned out not to be a better number but a better question: the window is a
     REAL Win32 window on the real desktop, so `CW_USEDEFAULT` is handed to the
     OS's own window manager, which is what it means. ⚠ The two constants are NOT
     the same value (`0x8000` here, `0x80000000` there); see wowwin_coord.
   The DESK_* numbers survive only as the fallback for a rectangle asked about
   before a window exists. */
/* ⚠ CW_USEDEFAULT16 lives in wowwin.h, beside the Win32 value it is NOT. */
#define WOWUSER_DESK_CX     640
#define WOWUSER_DESK_CY     480

/* ── TWO STYLE BITS, BELIEVED BECAUSE FOUR WINDOWS AGREE ──────────────────────
     WS_VISIBLE  `mpframe` (0x02cf0000) does NOT have it, and is the one window
                 SYSEDIT calls ShowWindow on (`seg2:0x0149`). `MDICLIENT`
                 (0x42300000) and `EDIT` (0x513000c4) DO have it and are never
                 shown explicitly, yet both must be visible.
     WS_CHILD    `EDIT` and `MDICLIENT` have 0x40000000; `mpframe` does not.
   ★ The Win16 and Win32 WS_* bits are the SAME VALUES -- Win32 inherited them --
     which is why a style word can be handed straight to CreateWindowEx while a
     CW_USEDEFAULT cannot. */
#define WS_VISIBLE16        0x10000000u
#define WS_CHILD16          0x40000000u

/* Win16 WNDCLASS field offsets -- see the note above. */
#define WNDCLASS_STYLE          0x00
#define WNDCLASS_WNDPROC        0x02
#define WNDCLASS_CLSEXTRA       0x06
#define WNDCLASS_WNDEXTRA       0x08
#define WNDCLASS_HINSTANCE      0x0a
#define WNDCLASS_HICON          0x0c
#define WNDCLASS_HCURSOR        0x0e
#define WNDCLASS_HBRBACKGROUND  0x10
#define WNDCLASS_MENUNAME       0x12
#define WNDCLASS_CLASSNAME      0x16
#define WNDCLASS_SIZE           0x1a

/* The control id Win32 gives an MDI client's first child. Any value works as long
   as it is above the ids a program uses for its own controls; this is the one
   Win32's own MDI samples use and it is above SYSEDIT's 0x0cac. */
#define WOWUSER_MDI_FIRSTCHILD 0xFF00

#define WOWUSER_MAX_CLASS 32

typedef struct {
    char  name[64];
    WORD  atom;
    WORD  style;
    DWORD wndproc;                   /* 16:16 far pointer into the guest */
    WORD  hinst;
    WORD  hicon, hcursor, hbrback;
    WORD  clsextra, wndextra;
    int   sysclass;                  /* 1 = the SYSTEM provides it, not a program */
    /* ★ The REAL Win32 class this one is made from. For a program's class that is
         a prefixed clone of its name registered against our own window procedure;
         for a system class it is the OS's own name, because MDICLIENT and EDIT
         already exist and reimplementing them would be the whole mistake again. */
    char  cls32[96];
    int   reg32;                     /* 1 = a real Win32 class is behind this */
    /* The predefined ordinals resolved out of hCursor/hIcon at registration --
       kept only so the log can say what the class actually got. */
    WORD  curord, icoord;
    int   curfell;                   /* the OS did not know that cursor ordinal */
} wowuser_class_t;

static wowuser_class_t g_wu_class[WOWUSER_MAX_CLASS];
static int             g_wu_nclass = 0;

/*
 * ── ★★★ THE SYSTEM CLASSES ARE THE 32-BIT SIDE'S, WHICH MEANS THEY ARE OURS ──
 * Measured, not assumed. Under WOW, `USER.EXE` is a THUNK MODULE: every export
 * funnels to the 32-bit half, which is why `RegisterClass` reaches this host at
 * all. So the classes Windows itself provides -- the ones no application ever
 * registers because they are already there -- have nowhere else to come from.
 * The run says so directly: in a whole SYSEDIT launch there are exactly four
 * RegisterClass calls, and all four are a program's own (`WOWExecClass`,
 * `WOWFaxClass`, `mpframe`, `mpchild`). USER never registers one, because in
 * this architecture it cannot.
 *
 * ★ AND THIS IS THE WALL THE FIRST 16-BIT CALLBACK UNCOVERED. With WM_CREATE
 *   delivered, SYSEDIT's frame procedure runs and does the one thing it exists
 *   to do -- `CreateWindow("MDICLIENT", ...)` at `sysedit seg1:0x01ca` -- and
 *   this host answered "no such class", because nothing had ever registered it.
 *   `[0x22]` stayed zero for a NEW reason, one step further on.
 *
 * ⚠ THE LIST IS WHAT THE RUN ASKED FOR, NOT A LIST OF SYSTEM CLASSES. Windows
 *   provides BUTTON, EDIT, STATIC, LISTBOX, COMBOBOX, SCROLLBAR and the numbered
 *   dialog/menu classes too, and seeding all of them would be answering questions
 *   nothing has asked -- every one would be a class that exists and does nothing,
 *   which is the "runs but lies" shape. They go in when a run names them.
 * ⚠ AND A SYSTEM CLASS HAS NO 16-BIT WINDOW PROCEDURE HERE, deliberately. On real
 *   Windows MDICLIENT's procedure lives in USER; ours is a host object with no
 *   behaviour, so a window made from it gets a handle and no WM_CREATE -- there is
 *   nothing to send it to. That is a stated gap, and it is the next thing an MDI
 *   application will feel: WM_MDICREATE has nowhere to go yet.
 */
/* ⚠ `EDIT` IS HERE BECAUSE THE GUEST BINARY NAMES IT, not because it is on a
     list of system classes. `sysedit seg1:0x0281` -- inside `mpchild`'s own
     WM_CREATE handler, four instructions after the message dispatch reaches
     msg == 1 -- pushes `ds:0x003a` as a CreateWindow class name, and segment 6
     (SYSEDIT's DGROUP) holds `"edit"` at that offset in the file on disk. Two
     more offsets from the same read confirm the reading rather than resting on
     it: `ds:0x0030` is `"mdiclient"`, which the frame procedure creates and a
     run has already been seen to ask for, and `ds:0x004a` is `"mpchild"`, which
     is the class named in the MDICREATESTRUCT below. Reading the guest binary
     is stronger evidence than waiting for the run line, not weaker. */
static const char *const g_wu_sysclass[] = { "MDICLIENT", "EDIT" };
static int               g_wu_sysdone = 0;

static void wowuser_ensure_sysclasses(void)
{
    unsigned i;
    int k;
    if (g_wu_sysdone) return;
    g_wu_sysdone = 1;
    for (i = 0; i < sizeof g_wu_sysclass / sizeof g_wu_sysclass[0]; ++i) {
        wowuser_class_t *c;
        if (g_wu_nclass >= WOWUSER_MAX_CLASS) return;
        c = &g_wu_class[g_wu_nclass++];
        for (k = 0; k < (int)sizeof c->name - 1 && g_wu_sysclass[i][k]; ++k)
            c->name[k] = g_wu_sysclass[i][k];
        c->name[k]   = 0;
        c->atom      = (WORD)(0xC000 + g_wu_nclass);
        c->sysclass  = 1;
        /* ★ A SYSTEM CLASS IS THE OS's OWN, AND WE USE IT AS-IS. `MDICLIENT` and
             `EDIT` are real Win32 classes on this machine; registering clones of
             them against our procedure would be reimplementing an edit control
             and an MDI client that already exist -- which is the same mistake as
             drawing our own window frames. No prefix, no registration. */
        for (k = 0; k < (int)sizeof c->cls32 - 1 && c->name[k]; ++k)
            c->cls32[k] = c->name[k];
        c->cls32[k] = 0;
        c->reg32 = 1;
    }
}

/*
 * ── ★★ A WINDOW, AS AN OBJECT, WITH NO PIXELS BEHIND IT ─────────────────────
 * DELIBERATELY NOT A REAL HWND. A host window would drag in a real message
 * queue, a real WM_CREATE and the 16:16 thunk back into the class's wndproc --
 * none of which exists, and all of which would be half-built and lying by the
 * time the first CreateWindow returned. What the guest can actually observe at
 * this point is a handle that is non-zero, stable, and answers questions about
 * itself, so that is exactly what this is: the class it was made from, its
 * rectangle, its style, its text. WOWEXEC's own window is the WOW shell's and is
 * normally hidden, so for this guest there is nothing to draw in any case.
 * ⇒ When windows do get pixels, this struct is the thing that grows a host
 *   window handle; nothing above it has to move.
 *
 * ⚠ THE HANDLE SPACE IS SYNTHETIC AND SAYS SO. A real Win16 HWND is an offset
 *   into USER's local heap; ours is a counter. Nothing may infer memory from it.
 */
#define WOWUSER_MAX_WIN   32
#define WOWUSER_MAX_EXTRA 16            /* words -- 32 bytes of cbWndExtra */
#define WOWUSER_HWND_BASE 0x0100        /* first synthetic handle */
#define WOWUSER_HWND_STEP 0x0020        /* spaced so a stray +n is not a hit    */

typedef struct wowuser_win_s {
    WORD  hwnd;                      /* 0 = free slot */
    WORD  cls;                       /* index into g_wu_class */
    DWORD style;
    DWORD wndproc;                   /* copied from the class AT CREATION -- Win16
                                        keeps it per window, so a later
                                        RegisterClass cannot retarget this one */
    int   x, y, cx, cy;
    WORD  parent, menu, hinst;
    char  text[64];
    /* ── ★★★ THE WINDOW'S EXTRA BYTES -- cbWndExtra, AND THEY ARE LOAD-BEARING.
         Not storage for its own sake: SYSEDIT keeps its EDIT control's handle
         and its file state in them. `mpchild`'s WNDCLASS declares
         `cbWndExtra = 8` (`sysedit seg2:0x0091 mov word [bp-0x14],8`, which is
         +0x08 from the struct base at `bp-0x1c` -- the same read that puts
         `"mpchild"` at +0x16, so the layout confirms itself), its WM_CREATE
         writes indices 0/2/4/6, and it reads them back to address the control.
         With no store behind them every read answered 0 and the run reached
         `SendMessage: no such window 0x0000 msg 0x040d` -- EM_SETHANDLE to a
         window handle the program had just been told to forget. */
    WORD  extra[WOWUSER_MAX_EXTRA];
    /* An EDIT control's text: a Win16 LOCAL handle in the APPLICATION's own
       heap, allocated by the guest's own KERNEL. See EM_GETHANDLE16. */
    WORD  hmem;
    /* ★★★★★ THE REAL WINDOW. Session 42: a Win16 window IS a Win32 window on the
         XP desktop, and this is it. The Win16 handle above stays synthetic and
         16-bit because that is what the guest can hold; this is what the OS
         holds, and the pair of them is the whole of the bridge. */
    HWND  hwnd32;
} wowuser_win_t;

static wowuser_win_t g_wu_win[WOWUSER_MAX_WIN];
static int           g_wu_nwin = 0;

/* ── krnl386's SEGMENT 1, AS A LIVE SELECTOR. ─────────────────────────────────
     Needed to call KERNEL exports (see EM_GETHANDLE16), and it costs nothing to
     know: the WOW32 common thunk lives in krnl386's segment 1, so the CS at
     every WOW32 BOP IS that selector. The host records it there rather than
     looking it up -- `wow_module_of_sel()` is a bind-stage table and cannot name
     a selector krnl386 allocated at run time, which is the same trap that made
     the id-space label print `?` about a segment the dispatcher had identified. */
static WORD g_wu_krnl_seg = 0;

/* A free window slot with its synthetic handle assigned, or NULL. Factored out
   of CreateWindow the moment a SECOND thing started making windows -- the MDI
   client's WM_MDICREATE -- because two copies of a handle formula is how two
   windows come to share a handle. */
static wowuser_win_t *wowuser_newwin(void)
{
    wowuser_win_t *w = NULL;
    int i;
    for (i = 0; i < g_wu_nwin; ++i) if (!g_wu_win[i].hwnd) { w = &g_wu_win[i]; break; }
    if (!w) {
        if (g_wu_nwin >= WOWUSER_MAX_WIN) return NULL;
        w = &g_wu_win[g_wu_nwin++];
    }
    w->hwnd = (WORD)(WOWUSER_HWND_BASE + (w - g_wu_win) * WOWUSER_HWND_STEP);
    return w;
}

static wowuser_win_t *wowuser_findwin(WORD hwnd)
{
    int i;
    if (!hwnd) return NULL;
    for (i = 0; i < g_wu_nwin; ++i)
        if (g_wu_win[i].hwnd == hwnd) return &g_wu_win[i];
    return NULL;
}

/* The other direction: the OS hands our window procedure a real HWND and we have
   to say which Win16 window that is. Declared in wowwin.h, defined here because
   this is where the table lives. 0 means "not one of ours", which is not an error
   -- DefWindowProc gets it, as it should. */
static WORD wowwin_hwnd16(HWND h)
{
    int i;
    if (!h) return 0;
    for (i = 0; i < g_wu_nwin; ++i)
        if (g_wu_win[i].hwnd && g_wu_win[i].hwnd32 == h) return g_wu_win[i].hwnd;
    return 0;
}

/* The real window behind a Win16 handle, or NULL. */
static HWND wowuser_hwnd32(WORD hwnd)
{
    const wowuser_win_t *w = wowuser_findwin(hwnd);
    return w ? w->hwnd32 : NULL;
}

/* Is this window's parent an MDI client? Decides which default procedure the OS
   should run for it -- see wowwin_proc. */
static int wowuser_is_mdichild(const wowuser_win_t *w)
{
    const wowuser_win_t *p = w->parent ? wowuser_findwin(w->parent) : NULL;
    return p && g_wu_class[p->cls].sysclass
             && g_wu_class[p->cls].name[0] == 'M';       /* MDICLIENT */
}

/* The MDI client owned by this window, if it has one -- a frame window has to
   pass it to DefFrameProc, which is how Win32 makes an MDI frame behave. */
static HWND wowuser_mdiclient_of(const wowuser_win_t *w)
{
    int i;
    for (i = 0; i < g_wu_nwin; ++i) {
        const wowuser_win_t *k = &g_wu_win[i];
        if (k->hwnd && k->parent == w->hwnd && k->hwnd32
            && g_wu_class[k->cls].sysclass && g_wu_class[k->cls].name[0] == 'M')
            return k->hwnd32;
    }
    return NULL;
}

/* Resolve a 16:16 far pointer that is NOT an argument -- one we found inside a
   structure the guest handed us. Same null-selector rule as wow32_argptr: 0
   rather than the LDT base, so a missing check cannot scribble at the bottom of
   the address space. */
static volatile BYTE *wowuser_farp(const wow32_frame_t *f, DWORD fp)
{
    WORD sel = (WORD)(fp >> 16);
    DWORD base;
    if (!sel || !f->sel2lin) return NULL;
    base = f->sel2lin(sel, f->ctx);
    if (!base) return NULL;
    return (volatile BYTE *)(ULONG_PTR)(base + (fp & 0xFFFF));
}

/* Read a NUL-terminated guest string through a 16:16 far pointer. */
static int wowuser_farstr(const wow32_frame_t *f, DWORD fp, char *out, int cap)
{
    WORD sel = (WORD)(fp >> 16);
    DWORD base;
    const volatile BYTE *s;
    int k = 0;
    if (cap) out[0] = 0;
    if (!sel || !f->sel2lin) return 0;
    base = f->sel2lin(sel, f->ctx);
    if (!base) return 0;
    s = (const volatile BYTE *)(ULONG_PTR)(base + (fp & 0xFFFF));
    while (k < cap - 1 && s[k]) { out[k] = (char)s[k]; ++k; }
    out[k] = 0;
    return 1;
}

static WORD wowuser_peek(const volatile BYTE *p, int off)
{
    return (WORD)(p[off] | (p[off + 1] << 8));
}

/* The class a name is registered under, or NULL. Win16 class names are
   case-insensitive, and a lookup that is not would silently register duplicates. */
static wowuser_class_t *wowuser_find(const char *name)
{
    int i, k;
    for (i = 0; i < g_wu_nclass; ++i) {
        const char *a = g_wu_class[i].name, *b = name;
        for (k = 0; ; ++k) {
            char ca = a[k], cb = b[k];
            if (ca >= 'a' && ca <= 'z') ca = (char)(ca - 32);
            if (cb >= 'a' && cb <= 'z') cb = (char)(cb - 32);
            if (ca != cb) break;
            if (!ca) return &g_wu_class[i];
        }
    }
    return NULL;
}

/* The class registered under an ATOM, or NULL. ⚠ Win16 lets lpClassName be an
   atom rather than a string -- a null selector with the atom in the offset -- and
   a host that only understood strings would fail every CreateWindow made from
   RegisterClass's own return value, which is the idiomatic way to write it. */
static wowuser_class_t *wowuser_find_atom(WORD atom)
{
    int i;
    if (!atom) return NULL;
    for (i = 0; i < g_wu_nclass; ++i)
        if (g_wu_class[i].atom == atom) return &g_wu_class[i];
    return NULL;
}

/* ---- note building ------------------------------------------------------
   The note is what the run log says a call DID, and it is the only channel this
   project has for reading a Win16 program's intent. It is built with a running
   cursor rather than a formatter because there is no CRT here (see runtime.c). */
static void wu_puts(char *b, int cap, int *k, const char *s)
{
    while (*s && *k < cap - 1) b[(*k)++] = *s++;
    b[*k] = 0;
}

static void wu_puthex(char *b, int cap, int *k, DWORD v, int digits)
{
    static const char hx[] = "0123456789abcdef";
    int i;
    for (i = digits - 1; i >= 0; --i) {
        if (*k >= cap - 1) break;
        b[(*k)++] = hx[(v >> (i * 4)) & 0xF];
    }
    b[*k] = 0;
}

/* A quoted string, with the quotes, truncated rather than dropped. */
static void wu_putq(char *b, int cap, int *k, const char *s)
{
    wu_puts(b, cap, k, "\"");
    wu_puts(b, cap, k, s);
    wu_puts(b, cap, k, "\"");
}

/* ── ★ ASK FOR THE WM_CREATE. One helper, because there are now TWO places that
     make a window with a 16-bit procedure behind it (CreateWindow, and the MDI
     client's WM_MDICREATE) and they must send the same message with the same
     entry conditions. See wowcall.h for what the fields mean and for why the
     return mode is KEEP: the caller has already written the handle it made, and
     the procedure's answer only gets a veto. */
/* Fill in a window-procedure call: five words, in DECLARED order. */
static void wowuser_want_msg(wow32_frame_t *f, const wowuser_win_t *w, WORD ds,
                             WORD msg, WORD wparam, DWORD lparam, int retmode)
{
    f->cbproc   = w->wndproc;
    f->cbds     = ds;
    f->cbarg[0] = w->hwnd;
    f->cbarg[1] = msg;
    f->cbarg[2] = wparam;
    f->cbarg[3] = (WORD)(lparam >> 16);
    f->cbarg[4] = (WORD)(lparam & 0xFFFF);
    f->cbnarg   = 5;
    f->cbret    = retmode;
    f->cbhwnd   = w->hwnd;
    f->cbmsg    = msg;
}

static void wowuser_want_create(wow32_frame_t *f, const wowuser_class_t *c,
                                const wowuser_win_t *w)
{
    if (!f->cbok || !w->wndproc) return;
    /* ⚠ lParam 0: no CREATESTRUCT yet -- see wowcall.h. */
    wowuser_want_msg(f, w, w->hinst ? w->hinst : c->hinst,
                     WM_CREATE16, 0, 0, WOWCALL_RET_KEEP);
}

/*
 * ── ★★★ THE MDICREATESTRUCT, READ OFF SYSEDIT'S OWN STORES ───────────────────
 * `sysedit seg3:0x0046` builds one on its stack at `ss:[bp-0x1e]` and hands it
 * to `SendMessage(hwndMDIClient, WM_MDICREATE, 0, &it)` at `seg3:0x007e` --
 * `USER.111 SENDMESSAGE`, named from the relocation chain, not inferred. Every
 * offset below is a store in that window, so nothing here is taken from a
 * header:
 *
 *   0046  mov [bp-0x1e],0x4a  / 004b mov [bp-0x1c],ds   -> +0x00 szClass  ds:0x4a
 *   0040  mov [bp-0x1a],ax    / 0043 mov [bp-0x18],ds   -> +0x04 szTitle
 *   004e  mov ax,[0x2e0] / 0051 mov [bp-0x16],ax        -> +0x08 hOwner
 *   0054  mov ax,0x8000  ... [bp-0x14]/[bp-0x12]/       -> +0x0a x   +0x0c y
 *                            [bp-0x10]/[bp-0x0e]        -> +0x0e cx  +0x10 cy
 *   0063  mov ax,[0x28] / mov dx,[0x2a] -> [bp-0xc]/[bp-0xa] -> +0x12 style DWORD
 *
 * ★ AND THE READING IS CONFIRMED FROM OUTSIDE THE CODE. `ds:0x004a` in
 *   SYSEDIT's DGROUP (segment 6, in the file on disk) is the string `"mpchild"`
 *   -- the class SYSEDIT registered two calls earlier. A wrong offset for
 *   szClass does not decode to a class this program has registered.
 * ⚠ THE STRUCT ENDS AT +0x16. The slot at `[bp-8]` is the SendMessage result,
 *   not a field, so `+0x16` (where a `lParam` member would sit) is NOT written
 *   by this program and must not be read.
 */
#define MCS_SZCLASS   0x00
#define MCS_SZTITLE   0x04
#define MCS_HOWNER    0x08
#define MCS_X         0x0a
#define MCS_Y         0x0c
#define MCS_CX        0x0e
#define MCS_CY        0x10
#define MCS_STYLE     0x12

/*
 * ── ★★★★ THE DEFAULT WINDOW PROCEDURE FOR A SYSTEM-CLASS WINDOW ─────────────
 * A window made from `MDICLIENT` has no 16-bit procedure, because under WOW the
 * system classes belong to the 32-bit side -- so when something sends it a
 * message, WE are the procedure. This is that procedure, and it is deliberately
 * tiny: the ONE message any run has ever sent, and 0 for everything else.
 *
 * ⚠ 0 IS NOT A STUB HERE, IT IS THE HONEST ANSWER for a message we do not
 *   implement, and the log names the message so the next one can be read off a
 *   run rather than guessed at from a list of MDI messages.
 */
static LONG wowuser_defproc(wow32_frame_t *f, wowuser_win_t *w, WORD msg,
                            WORD wparam, DWORD lparam, char *note, int notecap)
{
    int k = 0;
    switch (msg) {

    /* ── ★★★★★ WM_MDICREATE: MAKE THE CHILD WINDOW ────────────────────────────
         The message SYSEDIT stops on. lParam is a far pointer to the
         MDICREATESTRUCT above; the answer is the child's handle, and 0 means
         "no child", which is what the program has been correctly reporting as
         *"Cannot open this file."* */
    case WM_MDICREATE16: {
        volatile BYTE *m = wowuser_farp(f, lparam);
        char cname[64], tname[64];
        wowuser_class_t *c;
        wowuser_win_t *ch;
        int i;
        if (!m) { wu_puts(note, notecap, &k, "WM_MDICREATE: unreadable "
                                             "MDICREATESTRUCT"); return 0; }
        wowuser_farstr(f, (DWORD)wowuser_peek(m, MCS_SZCLASS)
                          | ((DWORD)wowuser_peek(m, MCS_SZCLASS + 2) << 16),
                       cname, sizeof cname);
        wowuser_farstr(f, (DWORD)wowuser_peek(m, MCS_SZTITLE)
                          | ((DWORD)wowuser_peek(m, MCS_SZTITLE + 2) << 16),
                       tname, sizeof tname);
        c = cname[0] ? wowuser_find(cname) : NULL;
        if (!c) {
            wu_puts(note, notecap, &k, "WM_MDICREATE: no such class ");
            wu_putq(note, notecap, &k, cname);
            return 0;
        }
        ch = wowuser_newwin();
        if (!ch) { wu_puts(note, notecap, &k, "WM_MDICREATE: no window slot");
                   return 0; }
        ch->cls     = (WORD)(c - g_wu_class);
        ch->wndproc = c->wndproc;             /* per WINDOW, as at CreateWindow */
        ch->style   = (DWORD)wowuser_peek(m, MCS_STYLE)
                    | ((DWORD)wowuser_peek(m, MCS_STYLE + 2) << 16);
        ch->parent  = w->hwnd;                /* ★ the MDI CLIENT is the parent */
        ch->menu    = 0;
        ch->hinst   = wowuser_peek(m, MCS_HOWNER);
        {   WORD x  = wowuser_peek(m, MCS_X),  y  = wowuser_peek(m, MCS_Y);
            WORD cx = wowuser_peek(m, MCS_CX), cy = wowuser_peek(m, MCS_CY);
            ch->x  = (x  == CW_USEDEFAULT16) ? 0 : (int)(short)x;
            ch->y  = (y  == CW_USEDEFAULT16) ? 0 : (int)(short)y;
            ch->cx = (cx == CW_USEDEFAULT16) ? WOWUSER_DESK_CX : (int)(short)cx;
            ch->cy = (cy == CW_USEDEFAULT16) ? WOWUSER_DESK_CY : (int)(short)cy;
        }
        for (i = 0; i < (int)sizeof ch->text; ++i) ch->text[i] = tname[i];
        /* ── ★★★★★ AND THE REAL MDI CLIENT MAKES THE REAL CHILD. (session 42) ──
             The first cut created it with a plain CreateWindowEx, and the run
             said why that is not the same thing: SYSEDIT passes **style 0**, and
             `MCS_STYLE == 0` is not "no style", it is *"give me the MDI
             defaults"* -- which only an MDI client can supply. Four borderless
             children stacked at (0,0) filling the whole client area is what
             "style 0" means to CreateWindowEx, and it is what the desktop showed.
           ⇒ Forward the message. Win32's MDI client then applies the default
             child style, CASCADES the children, gives each one a caption, a
             system menu and a place in the window list, and hands back the HWND
             -- all of which is the same argument as using the real `EDIT` class
             rather than drawing a text box.
           ⚠ The Win32 MDICREATESTRUCT is NOT the Win16 one -- different field
             widths and a different `lParam` -- so it is BUILT here from the
             fields read above rather than passed through.
           ⚠ Win32 sends its own WM_CREATE to `wowwin_proc` from inside this
             SendMessage, before `ch->hwnd32` is set, so the child is momentarily
             unknown to `wowwin_hwnd16`. That is correct and harmless: an unknown
             HWND falls through to DefWindowProc, and the message that matters to
             the guest is the WIN16 WM_CREATE requested below. */
        if (c->reg32 && w->hwnd32) {
            MDICREATESTRUCTA mcs;
            ZeroMemory(&mcs, sizeof mcs);
            mcs.szClass = c->cls32;
            mcs.szTitle = ch->text;
            mcs.hOwner  = GetModuleHandleA(NULL);
            mcs.x       = wowwin_coord(wowuser_peek(m, MCS_X));
            mcs.y       = wowwin_coord(wowuser_peek(m, MCS_Y));
            mcs.cx      = wowwin_coord(wowuser_peek(m, MCS_CX));
            mcs.cy      = wowwin_coord(wowuser_peek(m, MCS_CY));
            mcs.style   = ch->style;
            ch->hwnd32  = (HWND)(ULONG_PTR)SendMessageA(w->hwnd32, WM_MDICREATE16,
                                                        0, (LPARAM)&mcs);
            if (ch->hwnd32) {
                RECT r;
                ++g_ww_created;
                /* Take the rectangle back FROM the MDI client rather than keeping
                   the one we asked for: it chose, and every later answer this
                   host gives about this window has to agree with the screen. */
                if (GetWindowRect(ch->hwnd32, &r)) {
                    POINT p; p.x = r.left; p.y = r.top;
                    ScreenToClient(w->hwnd32, &p);
                    ch->x = p.x; ch->y = p.y;
                    ch->cx = r.right - r.left; ch->cy = r.bottom - r.top;
                }
            }
        }
        wu_puts(note, notecap, &k, "WM_MDICREATE ");
        wu_putq(note, notecap, &k, c->name);
        wu_puts(note, notecap, &k, " ");
        wu_putq(note, notecap, &k, ch->text);
        wu_puts(note, notecap, &k, " in client 0x");
        wu_puthex(note, notecap, &k, w->hwnd, 4);
        wu_puts(note, notecap, &k, " -> hwnd=0x");
        wu_puthex(note, notecap, &k, ch->hwnd, 4);
        /* ★ SAY WHETHER THE REAL WINDOW EXISTS, for the same reason CreateWindow
             does: a Win16 handle and a window on the desktop are two different
             achievements and only one of them can be seen. */
        if (ch->hwnd32) {
            wu_puts(note, notecap, &k, " HWND=0x");
            wu_puthex(note, notecap, &k, (DWORD)(ULONG_PTR)ch->hwnd32, 8);
            wu_puts(note, notecap, &k, " @");
            wu_puthex(note, notecap, &k, (DWORD)(short)ch->x, 4);
            wu_puts(note, notecap, &k, ",");
            wu_puthex(note, notecap, &k, (DWORD)(short)ch->y, 4);
            wu_puts(note, notecap, &k, " ");
            wu_puthex(note, notecap, &k, (DWORD)(short)ch->cx, 4);
            wu_puts(note, notecap, &k, "x");
            wu_puthex(note, notecap, &k, (DWORD)(short)ch->cy, 4);
            wu_puts(note, notecap, &k, " style=0x");
            wu_puthex(note, notecap, &k, ch->style, 8);
        } else {
            wu_puts(note, notecap, &k, " -- ★ NO REAL WINDOW (Win32 gle=0x");
            wu_puthex(note, notecap, &k, GetLastError(), 8);
            wu_puts(note, notecap, &k, ")");
        }
        wowuser_want_create(f, c, ch);
        return (LONG)ch->hwnd;
    }

    /* ── ★★★★ EM_GETHANDLE: THE HOST ASKS THE GUEST'S KERNEL FOR MEMORY ───────
         See the note on EM_GETHANDLE16 for why the answer has to be a real local
         handle in the application's own heap, and why that means calling
         `KERNEL.5 LocalAlloc` rather than inventing a number.
       ★ THE CALL'S RESULT IS THIS MESSAGE'S RESULT, so it goes back through
         WOWCALL_RET_RESULT -- the same path SendMessage-to-a-window-procedure
         uses, and for the same reason: the host must not invent the value.
       ★ AND THE HOST KEEPS A COPY, through the sink, because the control owns
         this handle from now on and the next EM_GETHANDLE must return the SAME
         one rather than allocating again.
       ⚠ `f->cbds` is the CONTROL'S OWN hInstance, not the caller's. LocalAlloc
         allocates from the local heap of whatever DS it is entered with, and the
         heap this handle has to be valid in is the one the application will call
         LocalReAlloc against -- which is its own. */
    case EM_GETHANDLE16: {
        if (w->hmem) {
            wu_puts(note, notecap, &k, "EM_GETHANDLE -> 0x");
            wu_puthex(note, notecap, &k, w->hmem, 4);
            wu_puts(note, notecap, &k, " (already allocated)");
            return (LONG)w->hmem;
        }
        if (!f->cbok || !w->hinst || !g_wu_krnl_seg) {
            wu_puts(note, notecap, &k, "EM_GETHANDLE: cannot allocate (no callback,"
                                       " no instance, or krnl386's segment is not"
                                       " known yet), answered 0");
            return 0;
        }
        wu_puts(note, notecap, &k, "EM_GETHANDLE -> asking the guest's own "
                                   "KERNEL.5 LocalAlloc in DGROUP 0x");
        wu_puthex(note, notecap, &k, w->hinst, 4);
        f->cbproc   = ((DWORD)g_wu_krnl_seg << 16) | KRNL_LOCALALLOC_OFF;
        f->cbds     = w->hinst;
        f->cbarg[0] = LMEM_MOVEABLE_ZEROINIT;
        f->cbarg[1] = WOWUSER_EDIT_INITIAL;
        f->cbnarg   = 2;
        f->cbret    = WOWCALL_RET_RESULTW;   /* LocalAlloc returns a WORD in AX */
        f->cbsink   = &w->hmem;
        f->cbhwnd   = w->hwnd;
        f->cbmsg    = msg;
        return 0;                    /* replaced by LocalAlloc's own answer */
    }

    /* ── EM_SETHANDLE: the application hands the control its text. ────────────
         The block is the GUEST'S -- it allocated the growth, locked it, read the
         file into it and NUL-terminated it itself -- so there is nothing to copy
         and nothing to own. Recording which handle the control now holds is the
         whole of the work, and it is what a later EM_GETHANDLE must return.
       ⚠ THIS IS WHERE THE TEXT BECOMES READABLE, and the day windows have pixels
         it is the hook: LocalLock that handle through the app's DGROUP and the
         file's contents are right there. */
    case EM_SETHANDLE16:
        w->hmem = wparam;
        wu_puts(note, notecap, &k, "EM_SETHANDLE 0x");
        wu_puthex(note, notecap, &k, wparam, 4);
        /* ── ★★★★★ AND THE REAL CONTROL HAS TO BE GIVEN THE TEXT. (session 42) ─
             The control is a real Win32 `EDIT` now, so "the control holds the
             text" stopped being a thing this host could just record. Real WOW has
             the same problem and solves it the same way: the Win16 handle names a
             block in the APPLICATION's local heap, which the 32-bit side cannot
             address, so the text is READ OUT of it and given to the real control.
           ★ Reading it means locking it, and only the guest's KERNEL can:
             `KERNEL.8 LOCALLOCK` at `<the BOP's CS>:0x3e0b`, entered with
             DS = the control's own hInstance, exactly as LocalAlloc was.
           ⚠ The answer is a near OFFSET, and following it is work that can only
             happen after the guest returns -- hence WOWCALL_ACT_EDITTEXT rather
             than a sink. The BOP handler does the read, the SetWindowText and the
             matching LocalUnlock.
           ⚠ EM_SETHANDLE's own answer stays 0: RET_KEEP, because the value the
             application gets back is the message's, not LocalLock's. */
        if (f->cbok && w->hwnd32 && w->hinst && g_wu_krnl_seg && wparam) {
            f->cbproc   = ((DWORD)g_wu_krnl_seg << 16) | KRNL_LOCALLOCK_OFF;
            f->cbds     = w->hinst;
            f->cbarg[0] = wparam;
            f->cbnarg   = 1;
            f->cbret    = WOWCALL_RET_KEEP;
            f->cbsink   = NULL;
            f->cbact    = WOWCALL_ACT_EDITTEXT;
            f->cbactarg = w->hwnd;
            f->cbhwnd   = w->hwnd;
            f->cbmsg    = msg;
            wu_puts(note, notecap, &k, " -- asking the guest's KERNEL.8 LocalLock"
                                       " for its text");
        } else {
            wu_puts(note, notecap, &k, " -- recorded, but nothing can read it"
                                       " (no callback, no real control, or"
                                       " krnl386's segment is unknown)");
        }
        return 0;

    default:
        wu_puts(note, notecap, &k, "default procedure: msg 0x");
        wu_puthex(note, notecap, &k, msg, 4);
        wu_puts(note, notecap, &k, " not implemented, answered 0");
        (void)wparam;
        return 0;
    }
}

/*
 * Returns 1 if serviced (the caller advances EIP past the BOP), 0 if not.
 * `note` receives a short human-readable description of what happened, so the
 * caller's log line can say what was registered rather than just that something was.
 *
 * ⚠ CALLED ONLY WHEN THE STUB IS USER'S. The caller checks; this file must never
 *   be reachable from krnl386's id space or the whole point of splitting it is lost.
 */
static int wowuser_call(wow32_frame_t *f, char *note, int notecap)
{
    if (notecap) note[0] = 0;
    wowuser_ensure_sysclasses();
    switch (f->id) {

    /* ── ★★★ 0x39 RegisterClass(const WNDCLASS FAR*) ──────────────────────────
         Named by USER's own export table: ordinal 57 `REGISTERCLASS` is a wrapper
         at seg1:0x1dbd that TAIL-JUMPS to the stub at seg1:0x0c18, and that stub
         pushes id 0x39 with 4 argument bytes -- one far pointer. The caller tests
         `or ax,ax`, so 0 is failure and any non-zero value is the class ATOM.
       ★ REGISTERING IS REAL WORK, NOT A STUB. The window procedure, the class
         styles and the extra-bytes counts are what CreateWindow and DefWindowProc
         will need, and they are only available here -- the guest hands them over
         once and then refers to the class by name. So keep them.
       ⚠ THE ATOM MUST BE STABLE AND NON-ZERO. Win16 atoms live at 0xC000 and up;
         a second RegisterClass of the same name returns the SAME atom rather than
         allocating another, because a program that re-registers (WOWEXEC does, once
         per relaunch) must not exhaust the table. */
    case WOWUSER_REGISTERCLASS: {
        volatile BYTE *wc = wow32_argptr(f, 0);
        char cname[64];
        wowuser_class_t *c;
        int n;
        if (!wc) { wow32_setret(f, 0); return 1; }      /* an unreadable WNDCLASS fails */
        wowuser_farstr(f, (DWORD)wowuser_peek(wc, WNDCLASS_CLASSNAME)
                          | ((DWORD)wowuser_peek(wc, WNDCLASS_CLASSNAME + 2) << 16),
                       cname, sizeof cname);
        if (!cname[0]) { wow32_setret(f, 0); return 1; } /* no name, no class */
        c = wowuser_find(cname);
        /* ⚠ A SYSTEM CLASS MAY NOT BE OVERWRITTEN. Re-registering an app's own
             class is allowed above (WOWEXEC does it once per relaunch), but the
             same code path would let a program point MDICLIENT at its own
             procedure and quietly take the system's class away from every other
             window made from it. Real Windows fails the call; so do we. */
        if (c && c->sysclass) {
            int k2 = 0;
            wu_puts(note, notecap, &k2, "RegisterClass REFUSED (system class) ");
            wu_putq(note, notecap, &k2, cname);
            wow32_setret(f, 0);
            return 1;
        }
        if (!c) {
            if (g_wu_nclass >= WOWUSER_MAX_CLASS) { wow32_setret(f, 0); return 1; }
            c = &g_wu_class[g_wu_nclass++];
            for (n = 0; n < (int)sizeof c->name; ++n) c->name[n] = cname[n];
            c->atom = (WORD)(0xC000 + g_wu_nclass);
        }
        c->style    = wowuser_peek(wc, WNDCLASS_STYLE);
        c->wndproc  = (DWORD)wowuser_peek(wc, WNDCLASS_WNDPROC)
                    | ((DWORD)wowuser_peek(wc, WNDCLASS_WNDPROC + 2) << 16);
        c->clsextra = wowuser_peek(wc, WNDCLASS_CLSEXTRA);
        c->wndextra = wowuser_peek(wc, WNDCLASS_WNDEXTRA);
        c->hinst    = wowuser_peek(wc, WNDCLASS_HINSTANCE);
        c->hicon    = wowuser_peek(wc, WNDCLASS_HICON);
        c->hcursor  = wowuser_peek(wc, WNDCLASS_HCURSOR);
        c->hbrback  = wowuser_peek(wc, WNDCLASS_HBRBACKGROUND);
        /* ★★★★★ AND REGISTER A REAL Win32 CLASS BEHIND IT. (session 42) A Win16
             window is a real window on the real desktop, so its class has to be a
             real class -- and it is OUR window procedure that goes in it, because
             the guest's is 16-bit and can only be entered through wowcall.h. */
        {   /* ★ THE TOKEN BECOMES A REAL OBJECT HERE, because this is where the
                 guest says which field it belongs in. A value that is not one of
                 our tokens yields 0 and the OS default is used -- an app's OWN
                 icon is a module resource (kind 3) and is not built yet. */
            WORD curord = wowuser_sysres_ord(c->hcursor);
            WORD icoord = wowuser_sysres_ord(c->hicon);
            int  fell   = 0;
            if (!c->reg32)
                c->reg32 = wowwin_register(c->name, c->cls32, sizeof c->cls32,
                                           curord, icoord, &fell);
            c->curord = curord; c->icoord = icoord; c->curfell = fell;
        }
        /* The note is the whole point of servicing this: it is the first time this
           project can say WHAT a Win16 program is trying to put on the screen. */
        {   int k = 0;
            wu_puts(note, notecap, &k, "RegisterClass ");
            wu_putq(note, notecap, &k, cname);
            wu_puts(note, notecap, &k, c->reg32 ? " -> Win32 class " : " -- ★ Win32 "
                                                  "RegisterClass FAILED for ");
            wu_puts(note, notecap, &k, c->cls32);
            if (c->curord) { wu_puts(note, notecap, &k, " cursor=0x");
                             wu_puthex(note, notecap, &k, c->curord, 4); }
            if (c->icoord) { wu_puts(note, notecap, &k, " icon=0x");
                             wu_puthex(note, notecap, &k, c->icoord, 4); }
            if (c->curfell)
                wu_puts(note, notecap, &k, " -- ★ THE OS DID NOT KNOW THAT CURSOR"
                                           " ORDINAL; fell back to IDC_ARROW");
        }
        wow32_setret(f, c->atom);
        return 1;
    }

    /* ── ★★★ 0x29 CreateWindow(...) -- 30 argument bytes ──────────────────────
         `USER.41 CREATEWINDOW`, named by resolving WOWEXEC's own import chain at
         its call site rather than inferred: the stub at USER `seg1:0x038d`
         pushes id 0x29 and `0x1e` argument bytes, and Win16's eleven parameters
         come to exactly 30. The block layout is derived and cross-validated in
         the comment on CW_ARG_* above.
       ★ AN UNREGISTERED CLASS MUST FAIL. Real Windows returns NULL, and a host
         that made a window for any name at all would hide a broken RegisterClass
         behind a working CreateWindow -- the "runs but lies" class.
       ⚠ The caller tests `or ax,ax / je`, so 0 is the failure the guest expects
         and any non-zero value is taken as the window handle. */
    case WOWUSER_CREATEWINDOW: {
        DWORD clsfp = wow32_argd(f, CW_ARG_CLASSNAME);
        char  cname[64], wname[64];
        wowuser_class_t *c;
        wowuser_win_t *w;
        int i, k = 0;

        cname[0] = 0;
        if ((WORD)(clsfp >> 16) == 0)                    /* an ATOM, not a string */
            c = wowuser_find_atom((WORD)clsfp);
        else {
            wowuser_farstr(f, clsfp, cname, sizeof cname);
            c = cname[0] ? wowuser_find(cname) : NULL;
        }
        /* ⚠ SAY WHAT WAS ASKED FOR. This read "CreateWindow: no such class" and
             nothing else, and the one run where it mattered -- SYSEDIT's frame
             procedure asking for MDICLIENT -- was therefore a line that named the
             class of failure and withheld the instance, which is the exact shape
             this project has been caught by before. An atom prints as an atom,
             because a lookup that failed on an atom did not have a name to fail on. */
        if (!c) {
            wow32_setret(f, 0);
            wu_puts(note, notecap, &k, "CreateWindow: no such class ");
            if (cname[0]) wu_putq(note, notecap, &k, cname);
            else { wu_puts(note, notecap, &k, "atom 0x");
                   wu_puthex(note, notecap, &k, clsfp & 0xFFFF, 4); }
            return 1;
        }

        w = wowuser_newwin();
        if (!w) { wow32_setret(f, 0); return 1; }
        w->cls     = (WORD)(c - g_wu_class);
        w->style   = wow32_argd(f, CW_ARG_STYLE);
        w->wndproc = c->wndproc;
        w->parent  = wow32_argw(f, CW_ARG_HWNDPARENT);
        w->menu    = wow32_argw(f, CW_ARG_HMENU);
        w->hinst   = wow32_argw(f, CW_ARG_HINSTANCE);
        {   WORD x  = wow32_argw(f, CW_ARG_X),  y  = wow32_argw(f, CW_ARG_Y);
            WORD cx = wow32_argw(f, CW_ARG_WIDTH), cy = wow32_argw(f, CW_ARG_HEIGHT);
            /* CW_USEDEFAULT is only meaningful before there is a rectangle; every
               later reader wants numbers, so resolve it once, here. */
            w->x  = (x  == CW_USEDEFAULT16) ? 0 : (int)(short)x;
            w->y  = (y  == CW_USEDEFAULT16) ? 0 : (int)(short)y;
            w->cx = (cx == CW_USEDEFAULT16) ? WOWUSER_DESK_CX : (int)(short)cx;
            w->cy = (cy == CW_USEDEFAULT16) ? WOWUSER_DESK_CY : (int)(short)cy;
        }
        w->text[0] = 0;
        if (wowuser_farstr(f, wow32_argd(f, CW_ARG_WINDOWNAME), wname, sizeof wname))
            for (i = 0; i < (int)sizeof w->text; ++i) w->text[i] = wname[i];

        /* ── ★★★★★ AND NOW MAKE A REAL WINDOW ON THE REAL DESKTOP. (session 42)
             This is what WOW is. Everything above stays -- the guest needs a
             16-bit handle it can hold, its window words, its class -- and the
             thing the USER sees is now the OS's own window, with the OS's title
             bar, border, taskbar button, focus and clipping.
           ⚠ THE COORDINATES ARE TRANSLATED, NOT PASSED. Win16's CW_USEDEFAULT is
             0x8000 and Win32's is 0x80000000. The STYLE word IS passed straight
             across, because Win32 inherited the WS_* values unchanged.
           ⚠ `hMenu` is a control ID for a child and a menu handle for a
             top-level, and this host has no menus, so it is only used for the
             child case -- a top-level gets NULL rather than a 16-bit number cast
             to a HMENU, which would be a handle from another address space.
           ⚠ MDICLIENT REQUIRES A CLIENTCREATESTRUCT and fails without one. That
             is not a workaround: it is the documented contract of the class we
             just chose to use rather than reimplement. */
        {   const wowuser_class_t *cc = &g_wu_class[w->cls];
            HWND parent32 = w->parent ? wowuser_hwnd32(w->parent) : NULL;
            CLIENTCREATESTRUCT ccs;
            void *param = NULL;
            HMENU hm = NULL;
            if (cc->sysclass && cc->name[0] == 'M') {     /* MDICLIENT */
                ccs.hWindowMenu  = NULL;
                ccs.idFirstChild = WOWUSER_MDI_FIRSTCHILD;
                param = &ccs;
            }
            if ((w->style & WS_CHILD16) && w->menu)
                hm = (HMENU)(ULONG_PTR)w->menu;
            if (cc->reg32) {
                w->hwnd32 = CreateWindowExA(0, cc->cls32, w->text, w->style,
                                            wowwin_coord(wow32_argw(f, CW_ARG_X)),
                                            wowwin_coord(wow32_argw(f, CW_ARG_Y)),
                                            wowwin_coord(wow32_argw(f, CW_ARG_WIDTH)),
                                            wowwin_coord(wow32_argw(f, CW_ARG_HEIGHT)),
                                            parent32, hm, GetModuleHandleA(NULL),
                                            param);
                if (w->hwnd32) { ++g_ww_created;
                                 if (!g_ww_thread) g_ww_thread = GetCurrentThreadId(); }
            }
        }

        wu_puts(note, notecap, &k, "CreateWindow ");
        wu_putq(note, notecap, &k, c->name);
        wu_puts(note, notecap, &k, " ");
        wu_putq(note, notecap, &k, w->text);
        wu_puts(note, notecap, &k, " style=0x");
        wu_puthex(note, notecap, &k, w->style, 8);
        wu_puts(note, notecap, &k, " -> hwnd=0x");
        wu_puthex(note, notecap, &k, w->hwnd, 4);
        /* A window made from a system class has no 16-bit procedure behind it, so
           it gets no WM_CREATE. Say which kind of window this is on the line that
           creates it, or the ABSENCE of the callback below reads like a defect. */
        if (c->sysclass) wu_puts(note, notecap, &k, " [system class: no wndproc]");
        /* ★ SAY WHETHER IT IS REALLY THERE. A Win16 handle that answers questions
             about itself and a WINDOW ON THE DESKTOP are different achievements,
             and only one of them is visible -- so the line has to distinguish
             them, or a failed CreateWindowEx reads as a success. */
        if (w->hwnd32) {
            wu_puts(note, notecap, &k, " HWND=0x");
            wu_puthex(note, notecap, &k, (DWORD)(ULONG_PTR)w->hwnd32, 8);
        } else {
            wu_puts(note, notecap, &k, " -- ★ NO REAL WINDOW (Win32 gle=0x");
            wu_puthex(note, notecap, &k, GetLastError(), 8);
            wu_puts(note, notecap, &k, ")");
        }
        wow32_setret(f, w->hwnd);

        /* ── ★★★★★ AND NOW SEND IT WM_CREATE. (GH #128, session 40) ───────────
             This is the whole point of the window, and until this session it was
             the one thing this host had never done in either direction. SYSEDIT's
             frame procedure creates its MDI client while handling WM_CREATE
             (`sysedit seg1:0x01ca`, then `mov [0x22],ax`), and `[0x22]` is what
             `seg2:0x0114` tests before deciding whether it has a usable window.
             So a CreateWindow that does not send WM_CREATE is not a window that
             is missing a message -- it is a window the application will correctly
             refuse to use.
           ★ THE PROCEDURE IS THE WINDOW'S, NOT THE CLASS'S. Win16 copies
             lpfnWndProc into the window at creation, which is why w->wndproc
             exists; sending to the class would be wrong the moment anything
             subclasses.
           ★ AND DS IS THE WINDOW'S hInstance. sysedit.exe is MULTIPLEDATA, so its
             exported prologue is the unpatched `push ds / pop ax`, which takes DS
             from its caller -- see the contract in wowcall.h. `hinst` is the same
             word the program put in WNDCLASS.hInstance and passed to
             CreateWindow, so it is the guest's own statement about its data
             segment rather than ours.
           ⚠ lParam SHOULD BE AN LPCREATESTRUCT and is 0 -- a gap this host names
             rather than fakes. See wowcall.h. */
        wowuser_want_create(f, c, w);
        return 1;
    }

    /* ── ★★★★★ 0x6f SendMessage(hWnd, msg, wParam, lParam) ────────────────────
         The call that moved the frontier, and the two halves of it are two
         different mechanisms rather than two cases of one:

           to a window with a 16-bit procedure  -> ★ THIS IS THE CALL. Hand it
             straight to wowcall.h with the caller's own arguments, and the
             procedure's return value IS SendMessage's (WOWCALL_RET_RESULT) --
             the host must not invent one, because "whatever the window
             procedure returned" is the entire definition of this function.
           to a SYSTEM-class window            -> the procedure is OURS, because
             under WOW the system classes belong to the 32-bit side. See
             wowuser_defproc.

       ⚠ GATED ON cbok, ALL OF IT. With callbacks off this falls through to the
         honest "unimplemented" rather than half-servicing: a WM_MDICREATE that
         made a child window whose WM_CREATE never ran would be a half-built
         object that the guest would then use, which is worse than a missing
         answer. It also keeps a default run byte-identical.
       ⚠ AN UNKNOWN hWnd IS AN ERROR, NOT A NO-OP. Our handles are synthetic and
         we issued every one of them, so a handle we do not recognise means the
         guest is talking about a window we never made -- and 0 with a named
         handle in the log is how that gets found. */
    case WOWUSER_SENDMESSAGE: {
        WORD  hwnd = wow32_argw(f, SM_ARG_HWND);
        WORD  msg  = wow32_argw(f, SM_ARG_MSG);
        WORD  wp   = wow32_argw(f, SM_ARG_WPARAM);
        DWORD lp   = wow32_argd(f, SM_ARG_LPARAM);
        wowuser_win_t *w;
        int k = 0;
        if (!f->cbok) return 0;
        w = wowuser_findwin(hwnd);
        if (!w) {
            wu_puts(note, notecap, &k, "SendMessage: no such window 0x");
            wu_puthex(note, notecap, &k, hwnd, 4);
            wu_puts(note, notecap, &k, " msg 0x");
            wu_puthex(note, notecap, &k, msg, 4);
            wow32_setret(f, 0);
            return 1;
        }
        if (w->wndproc) {
            wu_puts(note, notecap, &k, "SendMessage 0x");
            wu_puthex(note, notecap, &k, hwnd, 4);
            wu_puts(note, notecap, &k, " msg 0x");
            wu_puthex(note, notecap, &k, msg, 4);
            wu_puts(note, notecap, &k, " -> its own window procedure");
            /* Written so the hole is never uninitialised if the call is
               refused; wowcall.h overwrites it with the real answer. */
            wow32_setret(f, 0);
            wowuser_want_msg(f, w, w->hinst ? w->hinst : g_wu_class[w->cls].hinst,
                             msg, wp, lp, WOWCALL_RET_RESULT);
            return 1;
        }
        wow32_setret(f, (DWORD)wowuser_defproc(f, w, msg, wp, lp, note, notecap));
        return 1;
    }

    /* ── ★★★ 0x85 GetWindowWord / 0x86 SetWindowWord -- cbWndExtra ────────────
         Both named by USER's own export table. A window's extra words are where
         a Win16 program keeps its per-window state, and SYSEDIT keeps the handle
         of the EDIT control it created there -- so without these, its own child
         cannot find its own control, which is what
         `SendMessage: no such window 0x0000 msg 0x040d` was.
       ★ THE BOUND IS THE GUEST'S OWN DECLARATION. `cbWndExtra` comes from the
         WNDCLASS the program registered, so an out-of-range index is the program
         asking for storage it never asked to have -- refuse it, and say the
         declared size on the line so a refusal is self-explaining rather than a
         silent zero.
       ⚠ A NEGATIVE INDEX IS A STANDARD FIELD (Win16's GWW_*), and this host does
         NOT answer those. The constants would be written from memory, which is
         the one thing this project has a cardinal rule against; a run that needs
         one will name the index in the log, and then it can be read off the
         guest that asked. Until then it is an honest 0 that says so. */
    case WOWUSER_GETWINDOWWORD:
    case WOWUSER_SETWINDOWWORD: {
        int set = (f->id == WOWUSER_SETWINDOWWORD);
        WORD hwnd = wow32_argw(f, set ? SWW_ARG_HWND  : GWW_ARG_HWND);
        short idx = (short)wow32_argw(f, set ? SWW_ARG_INDEX : GWW_ARG_INDEX);
        WORD val  = set ? wow32_argw(f, SWW_ARG_VALUE) : 0;
        wowuser_win_t *w = wowuser_findwin(hwnd);
        int k = 0;
        wu_puts(note, notecap, &k, set ? "SetWindowWord 0x" : "GetWindowWord 0x");
        wu_puthex(note, notecap, &k, hwnd, 4);
        wu_puts(note, notecap, &k, " index ");
        if (idx < 0) { wu_puts(note, notecap, &k, "-0x");
                       wu_puthex(note, notecap, &k, (DWORD)(-idx), 2); }
        else         { wu_puts(note, notecap, &k, "0x");
                       wu_puthex(note, notecap, &k, (DWORD)idx, 2); }
        if (!w) {
            wu_puts(note, notecap, &k, " -- NO SUCH WINDOW");
            wow32_setret(f, 0);
            return 1;
        }
        if (idx < 0) {
            wu_puts(note, notecap, &k, " -- a STANDARD field; this host does not"
                                       " answer those yet, returning 0");
            wow32_setret(f, 0);
            return 1;
        }
        {   const wowuser_class_t *c = &g_wu_class[w->cls];
            if (idx + 2 > (int)c->wndextra || idx + 2 > (int)sizeof w->extra) {
                wu_puts(note, notecap, &k, " -- OUT OF RANGE, class declared"
                                           " cbWndExtra=0x");
                wu_puthex(note, notecap, &k, c->wndextra, 4);
                wow32_setret(f, 0);
                return 1;
            }
        }
        if (set) {
            wu_puts(note, notecap, &k, " <- 0x");
            wu_puthex(note, notecap, &k, val, 4);
            /* Win16 returns the PREVIOUS value, so read before writing. */
            wow32_setret(f, w->extra[idx / 2]);
            w->extra[idx / 2] = val;
        } else {
            wu_puts(note, notecap, &k, " -> 0x");
            wu_puthex(note, notecap, &k, w->extra[idx / 2], 4);
            wow32_setret(f, w->extra[idx / 2]);
        }
        return 1;
    }

    /* ── ★★★ 0x217 NotifyWow(lpBlock, wKind) -- see the note above. ───────────
         ⚠ ONLY THE KIND THAT HAS BEEN READ. `wKind == 3` is what LoadAccelerators
           passes, and that call site is the only one this run has ever taken.
           Answering every kind would be claiming to have understood a namespace
           of which exactly one member has been measured -- so anything else falls
           through to the honest "unimplemented", and the log says which kind. */
    case WOWUSER_NOTIFYWOW: {
        volatile BYTE *b = wow32_argptr(f, NOTIFY_ARG_BLOCK);
        WORD kind = wow32_argw(f, NOTIFY_ARG_KIND);
        int k = 0;
        if (kind != WOWNOTIFY_ACCEL || !b) return 0;
        wu_puts(note, notecap, &k, "NotifyWow(RT_ACCELERATOR) hInst=0x");
        wu_puthex(note, notecap, &k, wowuser_peek(b, NOTIFY_HINSTANCE), 4);
        wu_puts(note, notecap, &k, " hRes=0x");
        wu_puthex(note, notecap, &k, wowuser_peek(b, NOTIFY_HRESDATA), 4);
        wu_puts(note, notecap, &k, " at 0x");
        wu_puthex(note, notecap, &k, wowuser_peek(b, NOTIFY_LPRESOURCE + 2), 4);
        wu_puts(note, notecap, &k, ":0x");
        wu_puthex(note, notecap, &k, wowuser_peek(b, NOTIFY_LPRESOURCE), 4);
        wu_puts(note, notecap, &k, " cb=0x");
        wu_puthex(note, notecap, &k, (DWORD)wowuser_peek(b, NOTIFY_CBRESOURCE)
                  | ((DWORD)wowuser_peek(b, NOTIFY_CBRESOURCE + 2) << 16), 8);
        /* Non-zero is "noted"; the handle the application gets is the guest's
           own (seg1:0x3e37). 1 rather than a number that looks like a handle,
           so nothing downstream can mistake it for one. */
        wow32_setret(f, 1);
        return 1;
    }

    /* ── ★★★★★ 0x6c GetMessage / 0x6d PeekMessage -- THE LOOP TURNS ───────────
         The host's job here is exactly to FILL AN 18-BYTE STRUCTURE. It does not
         dispatch anything, because USER.EXE's own 16-bit `DispatchMessage` walks
         the MSG and makes the call itself -- see the note at the top of
         src/wow/wowmsg.h, where both the layout and that fact are read out of
         `user.exe seg1:0x1c37`.

       ★ THE RETURN VALUES ARE READ OFF THE CALL SITE, NOT FROM A HEADER.
         `sysedit seg1:0x0112 or ax,ax / jne 0x00c6`: non-zero keeps the loop, and
         **0 is WM_QUIT**. So 0 is not "nothing to report" -- there is no such
         answer to GetMessage, which blocks -- it is "this application is over".
         PeekMessage's 0 IS "nothing to report", and that is the whole difference
         between them.
       ⚠ WHEN THE QUEUE IS EMPTY, GetMessage BLOCKS, and the host does the waiting
         BEFORE this service is entered (wowmsg_host_wait in main.c, where the
         keyboard event handle lives). By the time we are here the wait is over,
         so an empty queue at this point means the wait expired -- nothing is ever
         going to arrive -- and answering WM_QUIT is then the truthful answer as
         well as the one that lets a harness run end. The log says which it was. */
    case WOWUSER_GETMESSAGE:
    case WOWUSER_PEEKMESSAGE: {
        int peek = (f->id == WOWUSER_PEEKMESSAGE);
        volatile BYTE *lp = wow32_argptr(f, peek ? PM_ARG_LPMSG : GM_ARG_LPMSG);
        WORD hwndf = wow32_argw(f, peek ? PM_ARG_HWND : GM_ARG_HWND);
        WORD minf  = wow32_argw(f, peek ? PM_ARG_MIN  : GM_ARG_MIN);
        WORD maxf  = wow32_argw(f, peek ? PM_ARG_MAX  : GM_ARG_MAX);
        WORD rem   = peek ? wow32_argw(f, PM_ARG_REMOVE) : PM_REMOVE16;
        wowmsg_t m;
        int k = 0;
        if (!lp) {
            wu_puts(note, notecap, &k, peek ? "PeekMessage" : "GetMessage");
            wu_puts(note, notecap, &k, ": unreadable lpMsg -- answered 0");
            wow32_setret(f, 0);
            return 1;
        }
        if (wowmsg_take(hwndf, minf, maxf, (rem & PM_REMOVE16) != 0, &m)) {
            wowmsg_write(lp, &m);
            wu_puts(note, notecap, &k, peek ? "PeekMessage -> hwnd=0x"
                                            : "GetMessage -> hwnd=0x");
            wu_puthex(note, notecap, &k, m.hwnd, 4);
            wu_puts(note, notecap, &k, " msg=0x");
            wu_puthex(note, notecap, &k, m.msg, 4);
            wu_puts(note, notecap, &k, " wParam=0x");
            wu_puthex(note, notecap, &k, m.wparam, 4);
            wu_puts(note, notecap, &k, " lParam=0x");
            wu_puthex(note, notecap, &k, m.lparam, 8);
            wu_puts(note, notecap, &k, (rem & PM_REMOVE16) ? " [removed]" : " [left]");
            wu_puts(note, notecap, &k, ", ");
            wu_puthex(note, notecap, &k, (DWORD)g_wm_count, 2);
            wu_puts(note, notecap, &k, " still queued");
            /* ⚠ WM_QUIT is delivered AND reported as the end. A loop that got a
                 non-zero for it would dispatch a message meant to stop it. */
            wow32_setret(f, (m.msg == WM_QUIT16 && !peek) ? 0 : 1);
            return 1;
        }
        if (!peek && g_wm_quit) {
            m.hwnd = 0; m.msg = WM_QUIT16; m.wparam = g_wm_quitcode;
            m.lparam = 0; m.time = 0; m.ptx = m.pty = 0;
            wowmsg_write(lp, &m);
            wu_puts(note, notecap, &k, "GetMessage -> WM_QUIT (PostQuitMessage 0x");
            wu_puthex(note, notecap, &k, g_wm_quitcode, 4);
            wu_puts(note, notecap, &k, ") -- the loop ends");
            wow32_setret(f, 0);
            return 1;
        }
        if (peek) {
            wu_puts(note, notecap, &k, "PeekMessage: queue empty -> 0 (correct:"
                                       " peek does not block)");
            wow32_setret(f, 0);
            return 1;
        }
        /* Nothing, and the host has already waited. Say so in full: a reader who
           sees `GetMessage -> 0` without this sentence would read it as WM_QUIT
           having been posted, which is a different fact entirely. */
        wu_puts(note, notecap, &k, "GetMessage: the queue is EMPTY and the host's"
                                   " input wait expired -- no message can arrive,"
                                   " so the application is told to quit. Posted 0x");
        wu_puthex(note, notecap, &k, g_wm_posted, 4);
        wu_puts(note, notecap, &k, " taken 0x");
        wu_puthex(note, notecap, &k, g_wm_taken, 4);
        wu_puts(note, notecap, &k, " this run");
        wow32_setret(f, 0);
        return 1;
    }

    /* ── 0x6e PostMessage(hWnd, msg, wParam, lParam) ───────────────────────────
         The same 10-byte block SendMessage uses, and the difference between them
         is the whole point of having a queue: SendMessage IS the call and returns
         the procedure's answer; PostMessage returns whether it got into the ring.
       ⚠ hWnd 0 is a THREAD message, not an error -- USER's DispatchMessage
         `jcxz`es it and the loop drops it, which is correct behaviour and not
         ours to prevent. */
    case WOWUSER_POSTMESSAGE: {
        WORD  hwnd = wow32_argw(f, PSM_ARG_HWND);
        WORD  msg  = wow32_argw(f, PSM_ARG_MSG);
        WORD  wp   = wow32_argw(f, PSM_ARG_WPARAM);
        DWORD lp   = wow32_argd(f, PSM_ARG_LPARAM);
        int ok, k = 0;
        ok = wowmsg_post(hwnd, msg, wp, lp, 0, 0, 0);
        wu_puts(note, notecap, &k, "PostMessage 0x");
        wu_puthex(note, notecap, &k, hwnd, 4);
        wu_puts(note, notecap, &k, " msg=0x");
        wu_puthex(note, notecap, &k, msg, 4);
        wu_puts(note, notecap, &k, ok ? " -> queued" : " -> ★ RING FULL, DROPPED");
        wow32_setret(f, (DWORD)ok);
        return 1;
    }

    /* ── 0x06 PostQuitMessage(nExitCode) ───────────────────────────────────────
         Not a queue entry: Win16 sets a flag on the task and GetMessage
         manufactures WM_QUIT only once everything else has drained. Queueing it
         would let it overtake messages already posted. */
    case WOWUSER_POSTQUITMESSAGE: {
        int k = 0;
        g_wm_quit = 1;
        g_wm_quitcode = wow32_argw(f, PQM_ARG_EXITCODE);
        wu_puts(note, notecap, &k, "PostQuitMessage 0x");
        wu_puthex(note, notecap, &k, g_wm_quitcode, 4);
        wu_puts(note, notecap, &k, " -- the next drained queue ends the loop");
        wow32_setret(f, 0);
        return 1;
    }

    /* ── ★★★★★ 0x72 DispatchMessage(lpMsg) -- THE LAST LINK IN THE LOOP ───────
         The message the host queued, the application took out of GetMessage and
         is now handing back to be delivered. Everything a window procedure needs
         is in those 18 bytes, which is why this call takes nothing else -- and
         the delivery itself is `wowcall_enter`, built in session 40 and used here
         without a line of new machinery.
       ★ THE RETURN IS THE PROCEDURE'S, exactly as for SendMessage, so it goes
         back through WOWCALL_RET_RESULT rather than being invented.
       ⚠ hwnd 0 IS NOT AN ERROR -- it is a thread message, and USER's own
         DispatchMessage `jcxz`es one. Answering 0 is what that means. */
    case WOWUSER_DISPATCHMESSAGE: {
        volatile BYTE *lp = wow32_argptr(f, DM_ARG_LPMSG);
        wowmsg_t m;
        wowuser_win_t *w;
        int k = 0;
        if (!lp) {
            wu_puts(note, notecap, &k, "DispatchMessage: unreadable lpMsg");
            wow32_setret(f, 0);
            return 1;
        }
        wowmsg_read(lp, &m);
        wu_puts(note, notecap, &k, "DispatchMessage hwnd=0x");
        wu_puthex(note, notecap, &k, m.hwnd, 4);
        wu_puts(note, notecap, &k, " msg=0x");
        wu_puthex(note, notecap, &k, m.msg, 4);
        if (!m.hwnd) {
            wu_puts(note, notecap, &k, " -- a thread message, nowhere to dispatch");
            wow32_setret(f, 0);
            return 1;
        }
        w = wowuser_findwin(m.hwnd);
        if (!w) {
            wu_puts(note, notecap, &k, " -- NO SUCH WINDOW");
            wow32_setret(f, 0);
            return 1;
        }
        if (w->wndproc) {
            if (!f->cbok) {
                wu_puts(note, notecap, &k, " -- its own window procedure, but"
                                           " callbacks are not armed");
                wow32_setret(f, 0);
                return 1;
            }
            wu_puts(note, notecap, &k, " -> its own window procedure");
            wow32_setret(f, 0);
            wowuser_want_msg(f, w, w->hinst ? w->hinst : g_wu_class[w->cls].hinst,
                             m.msg, m.wparam, m.lparam, WOWCALL_RET_RESULT);
            return 1;
        }
        wu_puts(note, notecap, &k, " -> ");
        wow32_setret(f, (DWORD)wowuser_defproc(f, w, m.msg, m.wparam, m.lparam,
                                               note + k, notecap - k));
        return 1;
    }

    /* ── 0x71 TranslateMessage(lpMsg) ──────────────────────────────────────────
         Its job is to turn a WM_KEYDOWN into a WM_CHAR and post that, and its
         return says whether it did. ⚠ THIS HOST DOES NOT, and 0 is therefore the
         TRUE answer rather than a stub: producing a character from a virtual key
         means a keyboard STATE (shift, caps, the dead-key buffer) that nothing
         here keeps, and inventing one would put wrong characters into an edit
         control -- the "runs but lies" class, in the one place a user would see
         it. Answered explicitly rather than left unimplemented so the line says
         so, and so the caller never reads the harness sentinel for a decision.
       ⇒ The day this returns 1 it will be because the host keeps that state and
         calls the OS (`ToAscii`) with it, the same way the virtual key itself
         comes from `MapVirtualKey` rather than from a table. */
    case WOWUSER_TRANSLATEMESSAGE: {
        volatile BYTE *lp = wow32_argptr(f, TA_ARG_LPMSG);
        wowmsg_t m;
        int k = 0;
        wu_puts(note, notecap, &k, "TranslateMessage msg=0x");
        if (lp) { wowmsg_read(lp, &m); wu_puthex(note, notecap, &k, m.msg, 4); }
        else      wu_puts(note, notecap, &k, "?");
        wu_puts(note, notecap, &k, " -> 0: this host produces no WM_CHAR (no"
                                   " keyboard state to translate with)");
        wow32_setret(f, 0);
        return 1;
    }

    /* ── 0xb2 TranslateAccelerator / 0x1c3 TranslateMDISysAccel ────────────────
         Both are asked BEFORE TranslateMessage and both are tested (`or ax,ax /
         jne`), so their answer decides whether the message reaches the window at
         all. 0 = "no accelerator matched", which is the truth: this host has no
         accelerator table -- `NotifyWow` deliberately does not keep the resource
         it is shown, because `GlobalUnlock` is the next instruction after it.
       ⚠ They were previously answered by the harness sentinel, which happens to
         be the same 0. Same value, different status: this one is a decision. */
    case WOWUSER_TRANSLATEACCEL:
    case WOWUSER_TRANSLATEMDISYS: {
        int mdi = (f->id == WOWUSER_TRANSLATEMDISYS);
        int k = 0;
        wu_puts(note, notecap, &k, mdi ? "TranslateMDISysAccel hwnd=0x"
                                       : "TranslateAccelerator hwnd=0x");
        wu_puthex(note, notecap, &k,
                  wow32_argw(f, mdi ? TMSA_ARG_HWND : TA_ARG_HWND), 4);
        if (!mdi) {
            wu_puts(note, notecap, &k, " hAccel=0x");
            wu_puthex(note, notecap, &k, wow32_argw(f, TA_ARG_HACCEL), 4);
        }
        wu_puts(note, notecap, &k, " -> 0 (no accelerator table in this host)");
        wow32_setret(f, 0);
        return 1;
    }

    /* ── ★★★ 0x2a ShowWindow(hWnd, nCmdShow) / 0x7c UpdateWindow(hWnd) ────────
         Both go straight to the OS, because the window is the OS's. That is the
         whole difference between this and the version of this host that drew its
         own frames: there is nothing here to decide.
       ★ THE ARGUMENTS ARE CONFIRMED BY THE RUN: `(0x0005 0x0160)` from
         `sysedit seg1:0x01da` -- the MDI client, from inside the frame's
         WM_CREATE -- and `(0x0001 0x0140)` from `seg2:0x0149`, the frame window,
         from WinMain. So `+0` is nCmdShow and `+2` is hWnd.
       ★ AND THE `1` IS OUR OWN VALUE COMING BACK: WinMain's `nCmdShow` is what
         this host put in the WOW command structure (`WOWCMD_NCMDSHOW`), handed to
         the application at launch and handed straight back here.
       ⚠ nCmdShow IS PASSED THROUGH UNTRANSLATED, and that is a claim worth
         making explicitly: the SW_* values are the same in Win16 and Win32, as
         the WS_* bits are. If a run ever shows a window doing the wrong thing,
         this is the line to doubt. */
    case WOWUSER_SHOWWINDOW: {
        WORD hwnd = wow32_argw(f, SW_ARG_HWND);
        WORD cmd  = wow32_argw(f, SW_ARG_CMDSHOW);
        wowuser_win_t *w = wowuser_findwin(hwnd);
        int k = 0;
        wu_puts(note, notecap, &k, "ShowWindow 0x");
        wu_puthex(note, notecap, &k, hwnd, 4);
        wu_puts(note, notecap, &k, " nCmdShow=0x");
        wu_puthex(note, notecap, &k, cmd, 4);
        if (!w) {
            wu_puts(note, notecap, &k, " -- NO SUCH WINDOW");
            wow32_setret(f, 0);
            return 1;
        }
        if (!w->hwnd32) {
            wu_puts(note, notecap, &k, " -- no real window behind it");
            wow32_setret(f, 0);
            return 1;
        }
        wow32_setret(f, ShowWindow(w->hwnd32, (int)(short)cmd) ? 1 : 0);
        wu_puts(note, notecap, &k, " -> the OS's ShowWindow on HWND=0x");
        wu_puthex(note, notecap, &k, (DWORD)(ULONG_PTR)w->hwnd32, 8);
        return 1;
    }

    case WOWUSER_UPDATEWINDOW: {
        WORD hwnd = wow32_argw(f, UW_ARG_HWND);
        wowuser_win_t *w = wowuser_findwin(hwnd);
        int k = 0;
        wu_puts(note, notecap, &k, "UpdateWindow 0x");
        wu_puthex(note, notecap, &k, hwnd, 4);
        if (w && w->hwnd32) { UpdateWindow(w->hwnd32);
                              wu_puts(note, notecap, &k, " -> the OS's"); }
        else                  wu_puts(note, notecap, &k, " -- no real window");
        wow32_setret(f, 0);
        return 1;
    }

    /* ── ★★ 0x91 RegisterClipboardFormat(lpszName) ────────────────────────────
         Named by USER's own export table, and the run names the callers: CARDFILE
         and WRITE both register the OLE 1.0 set -- "ObjectLink", "OwnerLink",
         "Native", "Binary", "FileName", "NetworkName" -- and then EXIT. Answered
         with the harness sentinel they get 0, which is the documented failure
         value, so a program that checks is entitled to give up. Two guests are
         stopped by one missing service.
       ★ THE ANSWER IS THE OS's. A clipboard format is a name in a system-wide
         atom table, and Win32 has that exact table with that exact call --
         including the same "an existing name returns the SAME id" contract, which
         matters because two Win16 programs must agree about "Native" the way two
         Win32 ones do. Registering our own would be a second table that agrees
         with nothing.
       ⚠ Win16's return is a WORD, Win32's a UINT. Registered formats live at
         0xC000..0xFFFF, so the value fits -- but the mask is explicit, because a
         silent truncation is how a host starts handing out ids that collide. */
    case WOWUSER_REGCLIPFORMAT: {
        char name[128];
        UINT fmt = 0;
        int k = 0;
        wu_puts(note, notecap, &k, "RegisterClipboardFormat ");
        if (!wow32_argstr(f, RCF_ARG_NAME, name, sizeof name) || !name[0]) {
            wu_puts(note, notecap, &k, "-- no name, answered 0");
            wow32_setret(f, 0);
            return 1;
        }
        wu_putq(note, notecap, &k, name);
        fmt = RegisterClipboardFormatA(name);
        wu_puts(note, notecap, &k, " -> 0x");
        wu_puthex(note, notecap, &k, fmt, 4);
        if (fmt > 0xFFFF) {
            wu_puts(note, notecap, &k, " -- ★ THE OS RETURNED AN ID THAT DOES NOT"
                                       " FIT IN A WORD; answered 0 rather than a"
                                       " truncation");
            wow32_setret(f, 0);
            return 1;
        }
        wow32_setret(f, fmt);
        return 1;
    }

    /* ── ★★★★ 0xad LoadSystemObject -- see the long note above. ───────────────
         ⚠ ONLY THE KIND THAT HAS BEEN READ. `1` is what a NULL-instance
           LoadCursor/LoadIcon passes and it is the only call site any run has
           taken; kind 3 carries a module's own resource and needs its own site
           read rather than this one widened. Anything else falls through to the
           honest 0 and the log says which kind asked. */
    case WOWUSER_LOADSYSOBJ: {
        WORD kind = wow32_argw(f, AD_ARG_KIND);
        WORD lo   = wow32_argw(f, AD_ARG_NAMELO);
        WORD hi   = wow32_argw(f, AD_ARG_NAMEHI);
        int k = 0, i;
        wu_puts(note, notecap, &k, "LoadSystemObject kind=0x");
        wu_puthex(note, notecap, &k, kind, 4);
        if (kind != AD_KIND_PREDEFINED) {
            wu_puts(note, notecap, &k, " -- not the predefined kind; this host has"
                                       " only read that call site, answered 0");
            wow32_setret(f, 0);
            return 1;
        }
        if (hi) {
            /* A far pointer to a NAME rather than MAKEINTRESOURCE. Real, and not
               something a run has shown us, so it is refused by name. */
            wu_puts(note, notecap, &k, " -- a NAMED resource (0x");
            wu_puthex(note, notecap, &k, hi, 4);
            wu_puts(note, notecap, &k, ":0x");
            wu_puthex(note, notecap, &k, lo, 4);
            wu_puts(note, notecap, &k, "), not an ordinal; answered 0");
            wow32_setret(f, 0);
            return 1;
        }
        wu_puts(note, notecap, &k, " ordinal=0x");
        wu_puthex(note, notecap, &k, lo, 4);
        /* One token per ordinal: the guest asks for IDC_ARROW in twenty classes
           and should get one answer, the way the OS gives one HCURSOR. */
        for (i = 0; i < g_wu_nsysres; ++i)
            if (g_wu_sysres[i].ord == lo) {
                wu_puts(note, notecap, &k, " -> 0x");
                wu_puthex(note, notecap, &k, g_wu_sysres[i].h, 4);
                wu_puts(note, notecap, &k, " (already issued)");
                wow32_setret(f, g_wu_sysres[i].h);
                return 1;
            }
        if (g_wu_nsysres >= WOWUSER_MAX_SYSRES) {
            wu_puts(note, notecap, &k, " -- ★ NO TOKEN LEFT, answered 0");
            wow32_setret(f, 0);
            return 1;
        }
        i = g_wu_nsysres++;
        g_wu_sysres[i].ord = lo;
        g_wu_sysres[i].h   = (WORD)(WOWUSER_SYSRES_BASE + i * WOWUSER_SYSRES_STEP);
        wu_puts(note, notecap, &k, " -> token 0x");
        wu_puthex(note, notecap, &k, g_wu_sysres[i].h, 4);
        wu_puts(note, notecap, &k, "; the OS object is fetched when the guest says"
                                   " whether it is a cursor or an icon");
        wow32_setret(f, g_wu_sysres[i].h);
        return 1;
    }

    /* ── 0x16 SetFocus(hWnd) ───────────────────────────────────────────────────
         Implemented because a keystroke has to be ADDRESSED, and Win16 addresses
         it to the focus window. SYSEDIT calls this once per MDI child it builds
         (`sysedit seg1:0x02d8`), so the target of a key is the guest's own
         decision rather than a choice this host makes.
       ⚠ An unknown handle is refused rather than recorded: focus on a window we
         never made would send every later key into nothing, silently. */
    case WOWUSER_SETFOCUS: {
        WORD hwnd = wow32_argw(f, SF_ARG_HWND);
        WORD prev = g_wm_focus;
        int k = 0;
        wu_puts(note, notecap, &k, "SetFocus 0x");
        wu_puthex(note, notecap, &k, hwnd, 4);
        if (hwnd && !wowuser_findwin(hwnd)) {
            wu_puts(note, notecap, &k, " -- NO SUCH WINDOW, focus unchanged");
            wow32_setret(f, prev);
            return 1;
        }
        g_wm_focus = hwnd;
        wu_puts(note, notecap, &k, " (was 0x");
        wu_puthex(note, notecap, &k, prev, 4);
        wu_puts(note, notecap, &k, ") -- keyboard messages now go here");
        wow32_setret(f, prev);
        return 1;
    }

    default:
        return 0;
    }
}

#endif /* WOWUSER_H */
