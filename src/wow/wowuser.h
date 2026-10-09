/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * USER.EXE's half of the WOW32 interface. GH #128, session 38.
 *
 * WHY THIS IS A SEPARATE FILE AND NOT MORE CASES IN wow32.h:
 * THE ID SPACE IS PER MODULE. Every id in wow32.h is one krnl386 sends; USER
 * sends its own 457 with its own numbering, and the numbers COLLIDE. `0x39` is `GetProfileInt` in krnl386's table and
 * `RegisterClass` in USER's -- and for one run this host serviced the second with
 * the first and handed WOWEXEC the answer. Two id spaces in one switch is how that
 * happens; two files with two dispatchers, chosen by the stub's own segment, is how
 * it stops happening.
 *
 * The surface is enumerated in docs/research/wow-user-surface.md -- 441 ids, 262 of
 * them named by USER's own export table, regenerable with
 *   tools/ne/wowmap.py guest/ne/user.exe --md
 *
 * THE Win16 WNDCLASS (documented; checked against what WOWEXEC passes):
 * 26 bytes. The block WOWEXEC hands RegisterClass reads back sensibly at every
 * offset -- hCursor is what LoadCursor returned, hbrBackground a stock object,
 * lpszMenuName NULL, lpszClassName a far pointer into its own DGROUP:
 *
 * +0x00 WORD  style          +0x0c WORD hIcon
 * +0x02 DWORD lpfnWndProc    +0x0e WORD hCursor
 * +0x06 WORD  cbClsExtra     +0x10 WORD hbrBackground
 * +0x08 WORD  cbWndExtra     +0x12 DWORD lpszMenuName
 * +0x0a WORD  hInstance      +0x16 DWORD lpszClassName
 *
 * [CAUTION]: The 0x82 seen at +0x16 is the CLASS NAME's OFFSET, not a style. An earlier
 * note read it as "style 0x82, hInstance=ds"; the 26-byte layout is what
 * settles it.
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#ifndef WOWUSER_H
#define WOWUSER_H

#include "host_state.h"
#include "wow32.h"
#include "wowcall.h"
#include "wowuser_calls.h"  /* the call table: thunk ids and argument offsets */
#include "wowconv.h"        /* the Win16/Win32 semantic deltas, pinned by tests/unit/wow_test.c */

#define WOWUSER_AD_KIND_PREDEFINED          1
#define WOWUSER_AD_KIND_MODULERES           3

/* s89 (#216): WHAT `kind` ACTUALLY IS. It arrives as 1 from every
 * LoadCursor -- NULL hInstance or a module's own -- and as 3 from every
 * LoadIcon. So `kind` is CURSOR (1) or ICON (3), and whether the object is a
 * PREDEFINED one or the MODULE's own is said by the hInstance at +18, which
 * arrives as 0 exactly when no module resolved the request. The token keeps the two names above for what they have always meant
 * (predefined / module), now derived from hInstance; the module arm also
 * passes the RT_CURSOR bytes it has just locked:
 * +2 GetExpWinVer   +4 hResData   +6 SizeofResource (DWORD)
 * +10 LockResource (far)   +14/+16 lpName   +18 hInstance
 */
#define WOWUSER_AD_KIND_CURSOR              1
#define WOWUSER_AD_KIND_ICON                3

/* A FOURTH KIND: AN ICON THAT IS ALREADY A REAL OBJECT. (session 57) (Importance = 1):
 * The two kinds above are both LAZY -- the token carries an ordinal or a name
 * and the resolver goes and gets the image when somebody uses it, because
 * until then we do not know whether the guest means a cursor or an icon. That
 * cannot describe SHELL's ExtractIcon, whose whole job is to reach into
 * ANOTHER FILE and come back with an image: there is no ordinal in this
 * module's resources to remember, and no name either. So this kind carries the
 * HICON itself, and the resolver hands it straight back.
 */
#define WOWUSER_AD_KIND_REALICON            4

/* Values every part of USER below shares. */
#define WOWUSER_LOWER_TO_UPPER              32                          /* 'a'..'z' minus this is 'A'..'Z' */
#define WOWUSER_SHORT_NAME_SIZE             32                          /* A resource or property name */
#define WOWUSER_NAME_SIZE                   64                          /* A class name, a menu name, a window title */
#define WOWUSER_CLASS32_SIZE                96                          /* The real Win32 class name behind a class */
#define WOWUSER_FULL_CLASS_NAME_SIZE        160                         /* WOWWIN_CLASS_PREFIX + a guest class name */
#define WOWUSER_TASK_NONE16                 0xFFFF                      /* krnl386's current-task word: no task */
#define WOWUSER_PAINT_WINDOW                0                           /* WowUserDefaultPaint: a window ... */
#define WOWUSER_PAINT_DIALOG                1                           /* ... or a dialog (the dialog colour) */
#define WOWUSER_KRNL_CURRENT_TASK           0x228                       /* That word's offset in krnl386's DGROUP */
#define WOW_TDB_INSTANCE                    0x1C /* A task database's hInstance (InitTask writes it) */
#define WOW_INSTANCE_FROM_SELECTOR          0xFFFE /* A task's SS with the low bit clear = its hInstance */
#define WOWUSER_MINUS_ONE16                 0xFFFF                      /* -1 as a Win16 WORD argument */

/* The Win16 owner-draw structures WM_DRAWITEM / MEASUREITEM / DELETEITEM / COMPAREITEM carry
 * (all WORD-sized handles and ids; itemState keeps only the first five ODS_ bits).
 */
#define WOWUSER_OWNERDRAW16_CTLTYPE         0
#define WOWUSER_OWNERDRAW16_CTLID           2
#define WOWUSER_DRAWITEM16_ITEMID           4
#define WOWUSER_DRAWITEM16_ITEMACTION       6
#define WOWUSER_DRAWITEM16_ITEMSTATE        8
#define WOWUSER_DRAWITEM16_HWNDITEM         10
#define WOWUSER_DRAWITEM16_HDC              12
#define WOWUSER_DRAWITEM16_RCITEM_LEFT      14
#define WOWUSER_DRAWITEM16_RCITEM_TOP       16
#define WOWUSER_DRAWITEM16_RCITEM_RIGHT     18
#define WOWUSER_DRAWITEM16_RCITEM_BOTTOM    20
#define WOWUSER_DRAWITEM16_ITEMDATA         22
#define WOWUSER_DRAWITEM16_SIZE             26
#define WOWUSER_ODS16_MASK                  0x1F
#define WOWUSER_MEASUREITEM16_ITEMID        4
#define WOWUSER_MEASUREITEM16_ITEMWIDTH     6
#define WOWUSER_MEASUREITEM16_ITEMHEIGHT    8
#define WOWUSER_MEASUREITEM16_ITEMDATA      10
#define WOWUSER_MEASUREITEM16_SIZE          14
#define WOWUSER_DELETEITEM16_ITEMID         4
#define WOWUSER_DELETEITEM16_HWNDITEM       6
#define WOWUSER_DELETEITEM16_ITEMDATA       8
#define WOWUSER_DELETEITEM16_SIZE           12
#define WOWUSER_COMPAREITEM16_HWNDITEM      4
#define WOWUSER_COMPAREITEM16_ITEMID1       6
#define WOWUSER_COMPAREITEM16_ITEMDATA1     8
#define WOWUSER_COMPAREITEM16_ITEMID2       12
#define WOWUSER_COMPAREITEM16_ITEMDATA2     14
#define WOWUSER_COMPAREITEM16_SIZE          18
#define WOWUSER_MAX_PARENT_DEPTH            8                           /* How far an instance is looked for upward */
#define WOWUSER_MAX_ALIASES                 48                          /* Other programs' windows given a handle */
#define WM_CTLCOLOR16                       0x0019                      /* Win16 only: Win32 split it per control */

/* Tokens live well above the window handles (0x0100 + n*0x20) so a stray one is
 * never mistaken for a window, and vice versa.
 */
#define WOWUSER_SYSRES_BASE                 0x8000
#define WOWUSER_SYSRES_STEP                 0x0008
#define WOWUSER_MAX_SYSRES                  16
#define CF_TEXT16                           1
#define CF_OEMTEXT16                        7
#define GMEM_MOVEABLE_DDESHARE16            0x2002

/* The host-side copy of the text in flight. One transfer at a time is all the
 * exec thread can have: a chain runs to completion before the guest's next call.
 */
#define WOWUSER_CLIPBOARD_SIZE              65536
#define WOWUSER_GCW16_HCURSOR               (-12)
#define WOWUSER_GWL16_WNDPROC               (-4)
#define WOWUSER_GWL16_STYLE                 (-16)
#define WOWUSER_GWL16_EXSTYLE               (-20)

#define WOWUSER_MAX_PROP                    64

#define WOWUSER_HOOKS                       8
#define WOWUSER_HOOK_RECORDED_ONLY          2                           /* WowUserHookSet: a kind with no Win32 hook */

/* Win16's EVENTMSG -- what a journal hook reads and writes. */
#define WOWUSER_EVENTMSG16_MESSAGE          0
#define WOWUSER_EVENTMSG16_PARAML           2
#define WOWUSER_EVENTMSG16_PARAMH           4
#define WOWUSER_EVENTMSG16_TIME             6
#define WOWUSER_EVENTMSG16_SIZE             10

/* [CAUTION]: ONE AT A TIME, IN ORDER. The nested run that calls the program pumps Win32
 * messages, and each input event retrieved there calls this hook again -- measured
 * six deep on the rig, which reaches the nesting limit and drops events. So an
 * event that arrives while one is being recorded is queued and handed over, in
 * order, when the outer call returns.
 */
#define WOWUSER_JREC_Q                      64

/* s92 (#306): WHOSE FILE A RESOURCE IS IN. Every menu, icon, cursor and
 * accelerator table used to be read from g_WowCommandProgram, the program on the
 * command line -- right while there was one Win16 program, wrong for the next:
 * WinHelp, started by Calc, came up with NO MENU because WINHELP.EXE's menu
 * #0fa0 was looked for in CALC.EXE. The running task's module says which file:
 * TDB+0x1E is hModule (TDB+0x1C, hInstance, is already read at (A)); the module
 * database starts "NE", and its word at +0x0A points at the OFSTRUCT krnl386
 * opened the file with, path at +8. Anything that does not check out falls back
 * to the command-line program, which is what every single-task run had.
 */
#define WOWUSER_TDB_HMODULE                 0x1E                        /* TDB: the task's module */
#define WOWUSER_NE_FILE_INFO                0x0A                        /* Module database: its OFSTRUCT */
#define WOWUSER_NE_FILE_INFO_MIN            0x40                        /* ...where a real one can lie */
#define WOWUSER_NE_FILE_INFO_MAX            0x8000
#define WOWUSER_OFSTRUCT_PATH               8                           /* OFSTRUCT.szPathName */
#define WOWUSER_MIN_FULL_PATH               4                           /* "C:\x" */
#define WOWUSER_PS16_HDC                    0
#define WOWUSER_PS16_ERASE                  2
#define WOWUSER_PS16_RECT                   4
#define WOWUSER_PS16_SIZE                   32

/* Menu tokens live between the window handles (0x0100 + n*0x20) and the
 * cursor/icon tokens (0x8000 + n*8), so a stray one of any kind is recognisable
 * on sight in a log rather than being mistaken for another kind of object.
 */
#define WOWUSER_MENU_BASE                   0x4000
#define WOWUSER_MENU_STEP                   0x0008
#define WOWUSER_MAX_MENU                    64

/* [CAUTION]: A Win16 RECT IS FOUR **WORDS**; A Win32 RECT IS FOUR **LONGS**. Eight bytes
 * against sixteen, and nothing about a wrong reading looks wrong -- it just
 * produces coordinates off by whatever the neighbouring field held. Anywhere a
 * RECT crosses this boundary it is converted field by field, and this is the
 * only place that says so.
 */
#define WOWUSER_RECT16_SIZE                 8

/* THE SAME TOKEN, RESOLVED AS A CURSOR (Importance = 1):
 * The mirror of the function above, and it exists for the same reason: only
 * the guest knows which of the two a token is, and it says so by which
 * WNDCLASS field it drops it into. [CAUTION] `fell` reports "the OS did not know that
 * predefined ordinal", which is a different failure from "this application has
 * no such named cursor" -- the caller logs them differently because one is our
 * assumption being wrong and the other is the guest's resource missing.
 */
/* s89 (#216): build a module's cursor from the RT_CURSOR bytes USER's
 * LoadCursor has just locked and passed to 0xad (+10, size at +6). Better than
 * the file: it is the resource of the module USER found, a DLL's included, and
 * it is the exact entry LookupIconIdFromDirectoryEx picked. Win 3.x layout -- a
 * 4-byte hotspot, then the DIB -- which is what CreateIconFromResourceEx takes
 * with fIcon FALSE at version 3.0.
 */
#define WOWUSER_CURSOR_HOTSPOT_SIZE         4
#define WOWUSER_BITMAPINFOHEADER_SIZE       40
#define WOWUSER_CURSOR_MAX_SIZE             0x10000
#define WOWUSER_ICON_RESOURCE_VERSION       0x00030000                  /* CreateIconFromResourceEx: 3.x */
#define WOWUSER_KRNL_LOCALALLOC_OFF         0x3ddb

/* [INFO]: AND ITS NEIGHBOURS -- the names from krnl386.exe's non-resident name table,
 * the offsets from its NE entry table (both documented NE structures):
 *    5 LOCALALLOC   0x3ddb      7 LOCALFREE    0x3df7
 *    6 LOCALREALLOC 0x3e1f      8 LOCALLOCK    0x3e0b
 *                               9 LOCALUNLOCK  0x3e55
 * Called with the documented signatures: `LocalLock(HLOCAL)` and
 * `LocalUnlock(HLOCAL)` take one WORD, far;
 * `HLOCAL LocalReAlloc(HLOCAL, WORD cbNew, WORD flags)` three words, far --
 * the order SYSEDIT's own call passes them in (above).
 */
#define WOWUSER_KRNL_LOCALREALLOC_OFF       0x3e1f
#define WOWUSER_KRNL_LOCALLOCK_OFF          0x3e0b
#define WOWUSER_KRNL_LOCALUNLOCK_OFF        0x3e55

/* [INFO]: #160: the GLOBAL trio, from the same entry table (all FIXED, segment 1) and
 * the same non-resident names -- 15 GLOBALALLOC, 18 GLOBALLOCK, 19 GLOBALUNLOCK
 * -- cross-checked by the three Local* offsets above coming out of the same parse.
 * Documented frames: GlobalAlloc(WORD flags, DWORD cb); GlobalLock/GlobalUnlock
 * take one WORD. GlobalLock answers a far pointer in DX:AX.
 */
#define WOWUSER_KRNL_GLOBALALLOC_OFF        0x3ac3
#define WOWUSER_KRNL_GLOBALLOCK_OFF         0x3b10
#define WOWUSER_KRNL_GLOBALUNLOCK_OFF       0x3b63

/* LMEM_MOVEABLE | LMEM_ZEROINIT -- the same flags SYSEDIT itself passes to
 * LocalReAlloc, so the block it grows is the kind it expects to be growing.
 */
#define LMEM_MOVEABLE_ZEROINIT              0x0042

/* Small on purpose: the guest reallocs it to the file's size before using it, so
 * anything bigger would be memory the application immediately replaces.
 */
#define WOWUSER_EDIT_INITIAL                0x20

/* NOTIFYWOW: "HERE IS A 16-BIT RESOURCE I HAVE JUST LOADED." (Importance = 3):
 * Named by USER's own export table (`wowmap.py`: id 0x217, 6 argument bytes,
 * NOTIFYWOW). It arrives inside every guest `LoadAccelerators`, with kind 3
 * and a far pointer to a block describing the RT_ACCELERATOR resource USER
 * has just found, loaded and locked. Answered 0, LoadAccelerators returns
 * NULL (observed); answered non-zero, it returns the resource's own handle.
 *
 * [INFO]: SO THE RETURN IS NOT A HANDLE. The application receives krnl386's global
 * handle for the resource (hResData below) whichever non-zero value this
 * answers. All this answer decides is whether LoadAccelerators SUCCEEDS.
 * Returning a fabricated handle here would be inventing a value nobody reads;
 * the honest answer is "noted", which is what the function's name says.
 *
 * The 12-byte block, as logged:
 *   +0x00 WORD  hInstance    ( the caller's module )
 *   +0x02 WORD  hResData     ( the resource's handle )
 *   +0x04 DWORD lpResource   ( 16:16 -- the bytes themselves )
 *   +0x08 DWORD cbResource   ( the resource's size )
 *
 * [CAUTION]: AND lpResource IS STALE THE MOMENT WE RETURN. USER unlocks the resource
 * as soon as this call returns, so a host that recorded that
 * pointer for a later TranslateAccelerator would be keeping an address the
 * guest has already released -- an instrument that lies later, which is this
 * project's most expensive shape. It is LOGGED, not kept. When accelerators
 * are actually implemented, the bytes must be COPIED here, while they are
 * locked, or asked for again through FindResource/LoadResource.
 */
#define WOWNOTIFY_ACCEL                     3

/* [WARNING]: wKind 4: USER'S START-UP CALL -- AND WHY NO WIN16 X BUTTON EVER WORKED (Importance =
 * 3): (s88, user: "some close buttons (X) don't work") XP's 16-bit DefWindowProc (USER.107)
 * forwards a message to WOW32 0x6b only if the message is no greater than a WORD maximum AND its
 * bit is set in a message bitmap USER keeps -- NOTHING IS FORWARDED UNLESS ITS BIT IS SET, and as
 * shipped the maximum is 0 and the bitmap is all zero. USER hands WOW32 pointers to both at
 * start-up through NotifyWow(4, &block), for the 32-bit side to fill. This host stepped that call
 * over, so DefWindowProc reached us ZERO times in 16 shelf apps, and every app that leaves WM_CLOSE
 * to DefWindowProc (Clock among them) could not end.
 *
 * [INFO]: The block, as it arrives: +0x0e/+0x10 far ptr to the WORD maximum,
 * +0x12/+0x14 far ptr to the bitmap, +0x16 its byte count (0x65 -> messages
 * 0..0x327).
 *
 * [CAUTION]: ONLY MESSAGES THE 0x6b HANDLER CAN TAKE RAW. It passes wParam/lParam straight
 * to DefWindowProcA; a message carrying a 16:16 pointer (WM_SETTEXT) or a GDI
 * token (WM_ERASEBKGND's HDC) would hand Windows a value it cannot use. Each
 * message joins this list when its parameters are translated AND a run needs it.
 */
#define WOWNOTIFY_USERINIT                  4
#define WOWNOTIFY_FINDCLASS                 6                           /* "where is the window of this class?" */
#define WOWUSER_NOTIFY_UI_MAX_OFF           0x0e
#define WOWUSER_NOTIFY_UI_BITS_OFF          0x12
#define WOWUSER_NOTIFY_UI_BITS_CB           0x16
#define WOWUSER_NOTIFY_HINSTANCE            0x00
#define WOWUSER_NOTIFY_HRESDATA             0x02
#define WOWUSER_NOTIFY_LPRESOURCE           0x04
#define WOWUSER_NOTIFY_CBRESOURCE           0x08

/* Win16's CW_USEDEFAULT, and what this host resolves it to.
 *
 * [INFO]: SESSION 42: IT RESOLVES TO Win32's. This used to be a stated placeholder --
 * "there is no desktop yet, so there is no honest answer" -- and the answer
 * turned out not to be a better number but a better question: the window is a
 * REAL Win32 window on the real desktop, so `CW_USEDEFAULT` is handed to the
 * OS's own window manager, which is what it means. [CAUTION] The two constants are NOT
 * the same value (`0x8000` here, `0x80000000` there); see WowWinCoordinate.
 * The DESK_* numbers survive only as the fallback for a rectangle asked about
 * before a window exists.
 */
/* [CAUTION]: CW_USEDEFAULT16 lives in wowwin.h, beside the Win32 value it is NOT. */
#define WOWUSER_DESK_CX                     640
#define WOWUSER_DESK_CY                     480

/* TWO STYLE BITS, BELIEVED BECAUSE FOUR WINDOWS AGREE:
 * WS_VISIBLE  `mpframe` (0x02cf0000) does NOT have it, and is the one window
 *             SYSEDIT calls ShowWindow on (as logged). `MDICLIENT`
 *             (0x42300000) and `EDIT` (0x513000c4) DO have it and are never
 *             shown explicitly, yet both must be visible.
 * WS_CHILD    `EDIT` and `MDICLIENT` have 0x40000000; `mpframe` does not.
 *
 * [INFO]: The Win16 and Win32 WS_* bits are the SAME VALUES -- Win32 inherited them --
 * which is why a style word can be handed straight to CreateWindowEx while a
 * CW_USEDEFAULT cannot.
 */
#define WS_VISIBLE16                        0x10000000u
#define WS_CHILD16                          0x40000000u

/* Win16 WNDCLASS field offsets -- see the note above. */
#define WOWUSER_WNDCLASS16_STYLE            0x00
#define WOWUSER_WNDCLASS16_WNDPROC          0x02
#define WOWUSER_WNDCLASS16_CLSEXTRA         0x06
#define WOWUSER_WNDCLASS16_WNDEXTRA         0x08
#define WOWUSER_WNDCLASS16_HINSTANCE        0x0a
#define WOWUSER_WNDCLASS16_HICON            0x0c
#define WOWUSER_WNDCLASS16_HCURSOR          0x0e
#define WOWUSER_WNDCLASS16_HBRBACKGROUND    0x10

/* The highest COLOR_* index a Win16 class can name in hbrBackground. Win 3.1 had
 * COLOR_BTNHIGHLIGHT = 20 as its last; anything above that is a real brush
 * handle, not a system colour. `mingw` has no COLOR_ENDCOLORS, and hard-coding
 * Win32's larger set would let a stray handle pass as a colour index.
 */
#define WOWUSER_COLOR_MAX                   20
#define WOWUSER_WNDCLASS16_MENUNAME         0x12
#define WOWUSER_WNDCLASS16_CLASSNAME        0x16
#define WOWUSER_WNDCLASS16_SIZE             0x1a

/* The control id Win32 gives an MDI client's first child. Any value works as long
 * as it is above the ids a program uses for its own controls; this is the one
 * Win32's own MDI samples use and it is above SYSEDIT's 0x0cac.
 */
#define WOWUSER_MDI_FIRSTCHILD              0xFF00

#define WOWUSER_MAX_CLASS                   32

/* A WINDOW, AS AN OBJECT, WITH NO PIXELS BEHIND IT (Importance = 2):
 * DELIBERATELY NOT A REAL HWND. A host window would drag in a real message
 * queue, a real WM_CREATE and the 16:16 thunk back into the class's wndproc --
 * none of which exists, and all of which would be half-built and lying by the
 * time the first CreateWindow returned. What the guest can actually observe at
 * this point is a handle that is non-zero, stable, and answers questions about
 * itself, so that is exactly what this is: the class it was made from, its
 * rectangle, its style, its text. WOWEXEC's own window is the WOW shell's and is
 * normally hidden, so for this guest there is nothing to draw in any case.
 *
 * When windows do get pixels, this struct is the thing that grows a host
 * window handle; nothing above it has to move.
 *
 * [CAUTION]: THE HANDLE SPACE IS SYNTHETIC AND SAYS SO. A real Win16 HWND is an offset
 * into USER's local heap; ours is a counter. Nothing may infer memory from it.
 */
/* [CAUTION]: 32 WAS ENOUGH UNTIL DIALOGS. A dialog is not one window, it is one window
 * PER CONTROL -- CALC's `SciCalc` template alone is a dialog plus 15 items,
 * and it opens that on top of the windows the program already has. At 32 the
 * table ran out mid-dialog, which does not fail loudly: the controls simply
 * stop being created and the dialog comes up half-built. (session 55)
 */
#define WOWUSER_MAX_WIN                     128
#define WOWUSER_MAX_EXTRA                   16                          /* Words -- 32 bytes of cbWndExtra */
#define WOWUSER_HWND_BASE                   0x0100                      /* First synthetic handle */
#define WOWUSER_HWND_STEP                   0x0020                      /* Spaced so a stray +n is not a hit */

/* #308 (s91): SUBCLASSING:
 *
 * [INFO]: THE SHAPE OF THE ANSWER. XP's USER.EXE exports one 16-bit procedure per
 * system control -- EDITWNDPROC (301), BUTTONWNDPROC (303), STATICWNDPROC (302),
 * SBWNDPROC (304), LBOXCTLWNDPROC (307), the combo box's (344), MDICLIENTWNDPROC
 * (444) -- at the segment-1 entry-table offsets in the table below. Calling one
 * behaves as CallWindowProc on ITSELF: it reaches us as CallWindowProc with its
 * own address as the procedure, and the 32-bit side knows from that to call the
 * real control. So that address is the right answer to GetWindowLong(
 * GWL_WNDPROC) for a system control: a subclass that chains --
 * CallWindowProc(old, ...) or a direct far call to `old` -- comes back here.
 * Each carries the marker bytes 'SCLS' and its class index at +0x34, which
 * this host checks before trusting the offset. This host:
 *   GetWindowLong  -> that export's own 16:16 address (marker checked first)
 *   SetWindowLong  -> the real control is subclassed with WowUserSubclassProcedure, which
 *                     SENDS its input/focus messages to the guest's procedure
 *                     through the nested run (g_WowUserCall16, main.c)
 *   CallWindowProc(<one of these>) -> the control's own Win32 procedure.
 *
 * [CAUTION]: WHAT THE GUEST'S PROCEDURE SEES: the messages whose parameters mean the same in
 * Win16 and Win32 -- keys, characters, mouse, focus, WM_GETDLGCODE, WM_SETCURSOR,
 * WM_NCHITTEST, WM_TIMER, WM_ENABLE, WM_CANCELMODE. Everything else (WM_PAINT,
 * text and EM_/LB_ messages with pointers) goes straight to the control. Those are
 * the ones subclassers filter -- an edit control that refuses letters, a list box
 * that drags -- and the rest would need the full 32->16 message translation.
 */
#define WOWUSER_STUB_SIGNATURE              0x34                        /* "SCLS" in USER's class-procedure stub */
#define WOWUSER_STUB_CLASS_INDEX            0x38                        /* ...then the class's index */
#define WOWUSER_SYSPROC_COUNT               ((INT)(sizeof g_WowUserSystemProcedures / sizeof g_WowUserSystemProcedures[0]))

/* s89 (#270): THE DESKTOP HAS A HANDLE. GetDesktopWindow used to answer 0,
 * on the grounds that GetDC(0) is the screen -- but a program that CENTRES a
 * dialog asks GetWindowRect(GetDesktopWindow()), and IsWindow of it must be
 * TRUE (the Win16 test `user.desktop.*`). One record outside the table: no
 * loop over g_WowUserWindows sees it, so it is never destroyed, enumerated or given a
 * message; every handler that takes an hWnd finds the real desktop behind it.
 * wndproc/dlgproc 0: nothing of the guest's is ever called for it. Below the
 * first synthetic window handle, on the same 0x20 spacing.
 */
#define WOWUSER_HWND_DESKTOP                0x00e0

/* THE Win16 DIALOG TEMPLATE, AND EVERY FIELD IS A DIFFERENT WIDTH
 * FROM ITS Win32 DESCENDANT. (session 55) ------------------------------------
 * This is the 16-bit DLGTEMPLATE: the item count is a BYTE, every coordinate
 * is a WORD, and the variable-length name fields come in three forms. Win32's
 * structure has a WORD count and a different field ORDER, so a reader written
 * from the modern layout produces a dialog with a plausible size and the
 * wrong number of controls -- which looks like a drawing bug, not a parsing one.
 *
 *     DWORD dtStyle;  BYTE dtItemCount;  WORD dtX, dtY, dtCX, dtCY;
 *     <menu>  <class>  <caption>
 *     if (dtStyle & DS_SETFONT):  WORD pointsize;  <typeface>
 *   then dtItemCount x:
 *     WORD x, y, cx, cy;  WORD id;  DWORD style;
 *     <class: ONE BYTE 0x80..0x85, or a string>  <text>  BYTE cbCreationData
 *
 * [INFO]: THE READING WAS CONFIRMED BEFORE ANY OF THIS EXISTED, by decoding CALC.EXE's
 * own resource offline (`tools/ne/neres.py dialog`) -- the same discipline the
 * menu decoder was held to in session 43. A wrong offset does not spell
 * 'Calculator', 'SciCalc', and buttons reading Hex/Dec/Oct/Bin/Hyp/Inv.
 */
#define WOWDLG_SETFONT                      0x40
#define WOWDLG_NAME_EMPTY                   0x00                        /* A name-or-ordinal field: nothing */
#define WOWDLG_NAME_ORDINAL                 0xFF                        /* ...an ordinal follows */
#define WOWDLG_NAME_ORDINAL_SIZE            3
#define WOWDLG_POINTS_PER_INCH              72
#define WOWDLG_SAMPLE_LENGTH                52                          /* "A..Za..z": the base-unit sample */

/* DLGTEMPLATE (Win16): style, item count, x, y, cx, cy, then the menu name. */
#define WOWDLG_TEMPLATE_COUNT               4
#define WOWDLG_TEMPLATE_X                   5
#define WOWDLG_TEMPLATE_Y                   7
#define WOWDLG_TEMPLATE_WIDTH               9
#define WOWDLG_TEMPLATE_HEIGHT              11
#define WOWDLG_TEMPLATE_MENU                13

/* DLGITEMTEMPLATE (Win16): x, y, cx, cy, id, style, then the class. */
#define WOWDLG_ITEM_Y                       2
#define WOWDLG_ITEM_WIDTH                   4
#define WOWDLG_ITEM_HEIGHT                  6
#define WOWDLG_ITEM_ID                      8
#define WOWDLG_ITEM_STYLE                   10
#define WOWDLG_ITEM_SIZE                    14
#define WOWDLG_UNITS_PER_BASE_X             4                           /* Dialog units: x*baseX/4, y*baseY/8 */
#define WOWDLG_UNITS_PER_BASE_Y             8

/* s89 (#283): A DIALOG IS LAID OUT IN ITS OWN FONT'S UNITS:
 * Dialog units are quarters of the dialog font's average character width and
 * eighths of its height. We used the SYSTEM font's (GetDialogBaseUnits), so
 * every Win16 dialog came out ~13% too big against stock on the same desktop
 * (Terminal's port dialog 218x152 vs 192x130, Calc 294 vs 275 tall, Charmap 702
 * vs 611 wide) and its controls drew in the system font. Measured the way USER32
 * measures it: the alphabet's average width (rounded), and tmHeight. BOLD: a
 * 3.x application's dialog font is bold under stock's WOW (its labels are, in
 * the same screenshots). Fonts are kept for the run, one per face+size -- a
 * shelf program opens a handful.
 */
#define WOWDLG_MAXFONT                      8

/* ASK FOR THE WM_CREATE. One helper, because there are now TWO places that
 * make a window with a 16-bit procedure behind it (CreateWindow, and the MDI
 * client's WM_MDICREATE) and they must send the same message with the same
 * entry conditions. See wowcall.h for what the fields mean and for why the
 * return mode is KEEP: the caller has already written the handle it made, and
 * the procedure's answer only gets a veto.
 */
/* THE TIMER TABLE, AND WHY A TIMERPROC IS NOT A NESTED CALL (Importance = 3):
 * Solitaire's first run named this: it arms `SetTimer(hWnd, 0x029a, 250ms,
 * lpTimerFunc)` with a REAL procedure at 0x0b9f:0x00ba, and a host that
 * refuses the call gets **"Out of memory"** -- Win16 timers were a scarce
 * system-wide resource, so failing to get one is genuinely how a program of
 * this era reports it.
 *
 * [INFO]: THE TRAP TO AVOID: calling that procedure from inside a Win32 timer
 * callback, which would mean re-entering the guest from a place the host is
 * not running it -- the nested run this project has not built. It is not
 * needed, because WIN16 DOES NOT CALL A TIMERPROC FROM THE TIMER EITHER. It
 * posts WM_TIMER with the procedure in lParam, and **DispatchMessage** calls
 * it instead of the window procedure. DispatchMessage is already a place this
 * host calls 16-bit code from, on the guest's own thread, with its own stack.
 * So the faithful implementation and the safe one are the same implementation.
 *
 * This table exists only so the WM_TIMER relay in wowwin.h can put the right
 * procedure in lParam; the OS keeps the actual timing.
 */
#define WOWUSER_MAXTIMER                    32

/* THE CREATESTRUCT, AND IT WAS READ OFF A RUN, NOT A HEADER (Importance = 5):
 * WM_CREATE's lParam is an LPCREATESTRUCT. This host passed 0 and said so, and
 * that stayed harmless until MS PAINT: its canvas procedure GP-faults on the
 * spot in WM_CREATE, reading through the null pointer at +0x0c (the host's
 * fault frame: ES:BX = lParam = 0) -- CREATESTRUCT.cx, i.e. how big it is.
 *
 * THE LAYOUT IS THE CreateWindow ARGUMENT BLOCK, UNCHANGED (Importance = 3):
 * Which is why this needs no header and no guesswork. Paint's own call carried
 *   (0000 0000 | 09c6 | 0001 | 0160 | 0002 | 0002 | 0002 | 00ac | 0000 40b0
 *    | 0000 0000 | 08bd 09c7)
 * and the CW_ARG_* offsets this file already uses -- named from earlier runs and
 * cross-checked against the 30 bytes the stub declares -- read that as
 * lpParam@0, hInstance@4, hMenu@6, hwndParent@8, cy@10, cx@12, y@14, x@16,
 * style@18 (0x40b00000, exactly what the log printed), lpszName@22,
 * lpszClass@26 ("pbPaint"). Those are the documented CREATESTRUCT's members, in
 * order, at those offsets -- and PBRUSH faulting on cx at +0x0c agrees with it
 * independently. So the structure is the argument block COPIED, plus a
 * `dwExStyle` of 0 at +30 to make up the 34 bytes.
 *
 * Nothing here is taken on trust: two independent observations agree.
 *
 * [CAUTION]: CW_USEDEFAULT IS SUBSTITUTED, NOT COPIED. A guest may pass 0x8000 for any of
 * x/y/cx/cy and then read the field expecting a number it can compute with --
 * Paint passes real values, so this is not what fixed it, but Notepad does not
 * and a copied 0x8000 would be a size of -32768. The real window's own
 * geometry is used instead, which is what the guest would have got on Windows.
 *
 * [CAUTION]: ONE THING IS STILL NOT MEASURED: whether real Windows shows a guest the
 * REQUESTED or the RESOLVED geometry for the fields it did not default. This
 * copies what was requested. No run has yet distinguished the two.
 */
#define WOWUSER_CW_GEOMETRY                 4                           /* Cy, cx, y, x */
#define WOWUSER_CW_BLOB_EXSTYLE             30                          /* ...then dwExStyle */
#define WOWUSER_CW_BLOB_SIZE                34

/* THE MDICREATESTRUCT, CHECKED AGAINST WHAT SYSEDIT SENDS (Importance = 3):
 * SYSEDIT builds one on its stack and hands it to
 * `SendMessage(hwndMDIClient, WM_MDICREATE, 0, &it)` (`USER.111 SENDMESSAGE`).
 * The documented Win16 layout, each field confirmed by the values that arrive:
 *
 * +0x00 szClass (far)    +0x0a x    +0x0c y    (CW_USEDEFAULT, 0x8000)
 * +0x04 szTitle (far)    +0x0e cx   +0x10 cy
 * +0x08 hOwner           +0x12 style DWORD
 *
 * [INFO]: szClass decodes to `"mpchild"` -- the class SYSEDIT registered two calls
 * earlier. A wrong offset for szClass does not decode to a class this program
 * has registered.
 *
 * [CAUTION]: THE STRUCT ENDS AT +0x16. `+0x16` (where a `lParam` member would sit) is NOT
 * set by this program -- the stack word there holds unrelated data -- and must
 * not be read.
 */
#define WOWUSER_MCS_SZCLASS                 0x00
#define WOWUSER_MCS_SZTITLE                 0x04
#define WOWUSER_MCS_HOWNER                  0x08
#define WOWUSER_MCS_X                       0x0a
#define WOWUSER_MCS_Y                       0x0c
#define WOWUSER_MCS_CX                      0x0e
#define WOWUSER_MCS_CY                      0x10
#define WOWUSER_MCS_STYLE                   0x12

/* Win16 numbers a control's messages from WM_USER, per class (see below). */
#define WOWUSER_CONTROL_MESSAGE_LAST16      0x0430
#define WOWUSER_CONTROL_CLASS_SIZE          16
#define WOWUSER_BM_BASE16                   WM_USER                     /* BM_GETCHECK */
#define WOWUSER_BM_LAST16                   0x0404                      /* BM_SETSTYLE */
#define WOWUSER_CB_BASE16                   WM_USER                     /* CB_GETEDITSEL */
#define WOWUSER_CB_COUNT16                  25
#define WOWUSER_LB_BASE16                   0x0401                      /* LB_ADDSTRING */
#define WOWUSER_LB_COUNT16                  0x23
#define WOWUSER_EM_BASE16                   WM_USER                     /* EM_GETSEL */
#define WOWUSER_EM_LAST16                   0x041D
#define WOWUSER_EM_COUNT16                  30
#define WOWUSER_EM_INDEX(message32)         ((message32) - EM_GETSEL)   /* N, from Win32's number */
#define WOWUSER_EM_SELECT_TO_END16          0x7FFF                      /* EM_SETSEL's end: to the end */
#define WOWUSER_RECT16_WORDS                4
#define WOWUSER_LIST_TEXT_SIZE              256
#define WOWUSER_LIST_MAX_SELECTION          1024
#define WOWUSER_MAX_TAB_STOPS               256
#define WOWUSER_EDIT_TEXT_SIZE              4096

/* What a list message's lParam/wParam carry -- the kinds in the tables below. */
#define WOWUSER_LIST_VALUES                 0
#define WOWUSER_LIST_IN_STRING              1
#define WOWUSER_LIST_OUT_BUFFER             2
#define WOWUSER_LIST_STRUCTURE              3
#define WOWUSER_LIST_INDEX                  4

#define WOWUSER_DEF_WINDOW                  0                           /* WowUserDef32's kind: DefWindowProc */
#define WOWUSER_DEF_FRAME                   1                           /* ...DefFrameProc */
#define WOWUSER_DEF_MDICHILD                2                           /* ...DefMDIChildProc */
#define WOWUSER_MINMAXINFO16_POINTS         5
#define WOWUSER_POINT16_SIZE                4
#define WOWUSER_POINT16_Y                   2

/* The dispatcher's named values: */
#define WOWUSER_TEXT_SIZE                   128

/* CREATESTRUCT (Win16), as WM_CREATE carries it -- see WowUserWantCreate. */
#define WOWUSER_CS16_INSTANCE               4
#define WOWUSER_CS16_MENU                   6
#define WOWUSER_CS16_PARENT                 8
#define WOWUSER_CS16_HEIGHT                 10
#define WOWUSER_CS16_WIDTH                  12
#define WOWUSER_CS16_Y                      14
#define WOWUSER_CS16_X                      16
#define WOWUSER_CS16_STYLE                  18
#define WOWUSER_CS16_NAME                   22                          /* lpszName: a far pointer, fixed up */
#define WOWUSER_CS16_CLASS                  26                          /* lpszClass: likewise */
#define WOWUSER_CS16_SIZE                   34
#define WOWUSER_CS16_FIXUPS                 2

/* GetWindowWord/SetWindowWord's negative indexes (Win16). */
#define WOWUSER_GWW16_HINSTANCE             (-6)
#define WOWUSER_GWW16_HWNDPARENT            (-8)
#define WOWUSER_GWW16_ID                    (-12)

/* An icon/cursor resource directory: idReserved, idType, idCount, then entries. */
#define WOWUSER_ICONDIR_COUNT               4
#define WOWUSER_ICONDIR_HEADER_SIZE         6
#define WOWUSER_ICONDIR_ENTRY_SIZE          14
#define WOWUSER_ICONDIR_MAX_ENTRIES         64
#define WOWUSER_NOTIFY_UI_BITS_MAX          0x100                       /* The forward bitmap's largest size */
#define WOWUSER_NOTIFY_FOUND                0x00010000u                 /* DX non-zero: found */
#define WOWUSER_TDB_HINSTANCE               0x1C                        /* TDB: the task's instance */
#define WOWUSER_KEY_DOWN                    0x8000                      /* GetKeyState: the key is down */
#define WOWUSER_COMMAND_FROM_ACCELERATOR    0x00010000u                 /* WM_COMMAND: HIWORD 1 */

/* DIB headers, as a resource holds them. */
#define WOWUSER_BITMAPCOREHEADER_SIZE       12
#define WOWUSER_BCH_WIDTH                   4
#define WOWUSER_BCH_HEIGHT                  6
#define WOWUSER_BCH_BITCOUNT                10
#define WOWUSER_BIH_WIDTH                   4
#define WOWUSER_BIH_HEIGHT                  8
#define WOWUSER_BIH_BITCOUNT                14
#define WOWUSER_BIH_SIZEIMAGE               20
#define WOWUSER_BIH_CLRUSED                 32
#define WOWUSER_RGBQUAD_SIZE                4
#define WOWUSER_RGBTRIPLE_SIZE              3
#define WOWUSER_MAX_PALETTE                 256
#define WOWUSER_MAX_PALETTE_BITS            8
#define WOWUSER_BITMAP_MAX_SIZE             0x10000
#define WOWUSER_STRING_SIZE                 256
#define WOWUSER_LONG_TEXT_SIZE              512
#define WOWUSER_MAX_TEXT_TABS               64                          /* TabbedTextOut's tab stops */
#define WOWUSER_KEY_STATE_SIZE              256                         /* Get/SetKeyboardState's table */
#define WOWUSER_WNDCLASS16_WORDS            13                          /* GetClassInfo's WNDCLASS, in words */
#define WOWUSER_ATOM_NAME_LENGTH            5                           /* "#xxxx": an integer atom's name */
#define WOWUSER_HEX_LETTER_VALUE            10                          /* 'a' in a hex digit */

/* RECT16 and POINT16 fields, as offsets. */
#define WOWUSER_RECT16_TOP                  2
#define WOWUSER_RECT16_RIGHT                4
#define WOWUSER_RECT16_BOTTOM               6

/* WINDOWPLACEMENT (Win16). */
#define WOWUSER_WP16_LENGTH                 0
#define WOWUSER_WP16_FLAGS                  2
#define WOWUSER_WP16_SHOWCMD                4
#define WOWUSER_WP16_MIN_X                  6
#define WOWUSER_WP16_MIN_Y                  8
#define WOWUSER_WP16_MAX_X                  10
#define WOWUSER_WP16_MAX_Y                  12
#define WOWUSER_WP16_NORMAL_LEFT            14
#define WOWUSER_WP16_NORMAL_TOP             16
#define WOWUSER_WP16_NORMAL_RIGHT           18
#define WOWUSER_WP16_NORMAL_BOTTOM          20
#define WOWUSER_WP16_SIZE                   22

/* The comm services: what wowcomm_* and the Win16 answers carry. */
#define WOWUSER_COMM_BUFFER_SIZE            512
#define WOWUSER_COMM_ALREADY_OPEN           (-5)                        /* WowCommOpen: the port is open */
#define WOWUSER_COMM_FAILED                 (-2)                        /* The answer these arms give on error */
#define WOWUSER_COMSTAT16_INQUEUE           1                           /* COMSTAT (Win16): cbInQue */
#define WOWUSER_COMSTAT16_OUTQUEUE          3                           /* ...cbOutQue */
#define WOWUSER_MINUS_ONE32                 0xFFFFFFFF
#define WOWUSER_DWORD_BYTES                 4
#define WOWUSER_CAPTION_SIZE                96
#define WOWUSER_MAX_WINDOW_WALK             4096                        /* GetWindow: windows stepped past */
#define WOWUSER_ATOM_TOP_DIGIT_SHIFT        12                          /* An atom's first hex digit */
#define WOWUSER_SEB_BUTTONS                 3
#define WOWUSER_SEB_BUTTON_MASK             0x7FFF                      /* The button, without SEB_DEFBUTTON */

typedef struct _WOWUSER_SYSRES
{
    WORD Handle16;                   /* 0 = free */
    WORD Ordinal;                    /* the ordinal the guest asked for, or 0 */
    WORD Kind;                       /* 1 = predefined system object, 3 = the
                                        MODULE's own resource */
    /* [INFO]: A RESOURCE CAN BE NAMED, AND MS PAINT'S ALL ARE. (session 47) Its icon
     * group is "PBRUSH" and its seven cursors are "FLOOD", "CROSSH", "PICK"...,
     * so a token that can only carry an ORDINAL cannot name any of them --
     * which is why Paint had no icon at all and never changed its pointer.
     * `Ordinal` and `Name` are alternatives: exactly one is set.
     */
    char Name[WOWUSER_SHORT_NAME_SIZE];   /* char, not CHAR: the spelling moves code (#333) */
    /* Set only for WOWUSER_AD_KIND_REALICON: the object itself, because there is nothing
     * to look it up BY -- it came out of a file that is not this module.
     */
    HICON RealIcon;
    /* s89 (#216): the cursor, once built -- from the bytes USER handed 0xad, or
     * on first use. SetCursor runs on every mouse move; building there each time
     * would leak an object per move.
     */
    HCURSOR Cursor;
} WOWUSER_SYSRES, *PWOWUSER_SYSRES; typedef const WOWUSER_SYSRES *PCWOWUSER_SYSRES;
typedef struct _WOWUSER_PROP
{
    WORD Window;
    WORD Data;
    char Name[WOWUSER_SHORT_NAME_SIZE];
} WOWUSER_PROP;
typedef struct _WOWUSER_HOOK
{
    SHORT Id;
    DWORD Procedure;
    WORD DataSelector;
    HHOOK Hook32;
} WOWUSER_HOOK;

typedef struct _WOWUSER_MENU
{
    WORD Handle16;
    HMENU Menu;
} WOWUSER_MENU;

typedef struct _WOWUSER_CLASS
{
    char  Name[WOWUSER_NAME_SIZE];                   /* char, not CHAR: the spelling moves code (#333) */
    WORD  Atom;
    WORD  Style;
    DWORD WindowProcedure;           /* 16:16 far pointer into the guest */
    WORD  Instance;
    WORD Icon16;
    WORD Cursor16;
    WORD Background16;
    WORD ClassExtra;
    WORD WindowExtra;
    INT   IsSystemClass;             /* 1 = the SYSTEM provides it, not a program */
    /* [INFO]: The REAL Win32 class this one is made from. For a program's class that is
     * a prefixed clone of its name registered against our own window procedure;
     * for a system class it is the OS's own name, because MDICLIENT and EDIT
     * already exist and reimplementing them would be the whole mistake again.
     */
    char  Class32[WOWUSER_CLASS32_SIZE];
    INT   IsRegistered32;            /* 1 = a real Win32 class is behind this */
    /* The predefined ordinals resolved out of hCursor/hIcon at registration --
     * kept only so the log can say what the class actually got.
     */
    WORD CursorOrdinal;
    WORD IconOrdinal;
    WORD IconKind;
    INT   IsCursorUnknown;           /* the OS did not know that cursor ordinal */
    INT   IconBits;                  /* colour depth of the icon actually built */
    /* What the class said its menu was -- a name, or an ordinal. Recorded and
     * logged; not yet turned into a real HMENU (that needs the guest's own MENU
     * resource, which lives in its NE file).
     */
    char  MenuName[WOWUSER_NAME_SIZE];
    WORD  MenuOrdinal;
} WOWUSER_CLASS, *PWOWUSER_CLASS; typedef const WOWUSER_CLASS *PCWOWUSER_CLASS;

typedef struct _WOWUSER_WINDOW
{
    WORD  Window16;                  /* 0 = free slot */
    WORD  Class;                     /* index into g_WowUserClasses */
    DWORD Style;
    DWORD WindowProcedure;           /* copied from the class AT CREATION -- Win16
                                        keeps it per window, so a later
                                        RegisterClass cannot retarget this one */
    /* AND THE OTHER PROCEDURE A WINDOW CAN HAVE. (session 57) (Importance = 3):
     * A dialog created from a template that names no class is a `#32770`
     * window -- a SYSTEM class, so `WindowProcedure` above is 0 and always was, which
     * is correct and is also why a dialog could not be told anything. The
     * thing that drives it is the DLGPROC the guest passed to
     * DialogBox/CreateDialog, which is not a window procedure and does not
     * live in a class: it belongs to this one window, for its lifetime.
     *
     * [CAUTION]: NOT A FALLBACK FOR WindowProcedure AT LARGE. Where a template DOES name the
     * application's own class (CALC's `SciCalc`) the class procedure is the
     * one Windows calls, and this stays 0 unless the guest supplied one. The
     * order is settled in WowUserWindowProcedureOf() and nowhere else.
     */
    DWORD DialogProcedure;
    INT PositionX;
    INT PositionY;
    INT Width;
    INT Height;
    WORD Parent;
    WORD Menu;
    WORD Instance;
    char  Text[WOWUSER_NAME_SIZE];                   /* char, not CHAR: the spelling moves code (#333) */
    /* THE WINDOW'S EXTRA BYTES -- cbWndExtra, AND THEY ARE LOAD-BEARING.
     * Not storage for its own sake: SYSEDIT keeps its EDIT control's handle
     * and its file state in them. `mpchild`'s WNDCLASS declares
     * `cbWndExtra = 8` (as it arrives in RegisterClass, the same block that
     * carries `"mpchild"` at +0x16), its WM_CREATE writes indices 0/2/4/6, and
     * it reads them back to address the control.
     * With no store behind them every read answered 0 and the run reached
     * `SendMessage: no such window 0x0000 msg 0x040d` -- EM_SETHANDLE to a
     * window handle the program had just been told to forget.
     */
    WORD  Extra[WOWUSER_MAX_EXTRA];
    /* An EDIT control's text: a Win16 LOCAL handle in the APPLICATION's own
     * heap, allocated by the guest's own KERNEL. See EM_GETHANDLE16.
     */
    WORD  Memory16;
    INT   MenuItems;                 /* how many entries its class menu produced */
    /* [INFO]: THE REAL WINDOW. Session 42: a Win16 window IS a Win32 window on the
     * XP desktop, and this is it. The Win16 handle above stays synthetic and
     * 16-bit because that is what the guest can hold; this is what the OS
     * holds, and the pair of them is the whole of the bridge.
     */
    HWND  Window32;
    /* [INFO]: DESTROYED, BUT NOT YET TOLD. Set between DestroyWindow tearing the real
     * window down and the guest's own procedure receiving WM_DESTROY -- the
     * record has to outlive the window by exactly that long, because
     * DispatchMessage finds the procedure THROUGH it. Cleared when that
     * message is dispatched. See both arms.
     */
    BYTE  IsDying;
    /* s89 (#283/#282): a dialog's base units, from its TEMPLATE font -- what
     * its controls were laid out with, and what MapDialogRect must use. 0 = not
     * a dialog (the system's base units apply).
     */
    WORD DialogBaseUnitX;
    WORD DialogBaseUnitY;
    /* s89: the template named a font (DS_SETFONT). Stock gives such a dialog the 3-D
     * look -- its static text defaults to the button face (Charmap) -- and a dialog
     * without one the window colour (Calc's display). Measured on those two.
     */
    BYTE  IsDialog3D;
    /* #308 (s91): A SUBCLASSED SYSTEM CONTROL. `SubclassProcedure` is the guest's 16:16
     * procedure installed by SetWindowLong(GWL_WNDPROC) on a real Win32 control
     * (EDIT, LISTBOX...), kept apart from `WindowProcedure` on purpose: every check of
     * `WindowProcedure` in this file means "a window whose CLASS is 16-bit", and this one is
     * not. `OriginalProcedure32` is the control's own Win32 procedure, displaced by
     * WowUserSubclassProcedure. 0/NULL = not subclassed.
     */
    DWORD   SubclassProcedure;
    WNDPROC OriginalProcedure32;
    /* s91: an ALIAS -- a Win16 handle for a window that is NOT the guest's (another
     * program's top-level window), minted by WowUserAlias16 for GetWindow. No
     * procedure, never destroyed by us, skipped by everything that means "ours".
     */
    BYTE    IsForeign;
    /* s92 (#306): the task that created it -- whose queue its posted messages are
     * in. 0 = not known; such a window's messages go to whichever task asks.
     */
    WORD    Task;
} WOWUSER_WINDOW, *PWOWUSER_WINDOW; typedef const WOWUSER_WINDOW *PCWOWUSER_WINDOW;
typedef struct _WOWUSER_SYSPROC
{
    PCSTR ClassName;
    WORD Offset;
    BYTE Index;
} WOWUSER_SYSPROC, *PWOWUSER_SYSPROC; typedef const WOWUSER_SYSPROC *PCWOWUSER_SYSPROC;
typedef struct _WOWDLG_FONT
{
    char FaceName[LF_FACESIZE];
    INT PointSize;
    HFONT Font;
    INT BaseX;
    INT BaseY;
} WOWDLG_FONT;
typedef struct _WOWUSER_TIMER
{
    WORD Window;
    WORD Id;
    DWORD Procedure;
    INT IsUsed;
} WOWUSER_TIMER;

/* s88: the DIALOG MANAGER'S DEFAULT for one message, once the DLGPROC has
 * answered FALSE (or there is none). See WOWUSER_DEFDLGPROC. The two arms are
 * the ones this host always had:
 *
 * [INFO]: #162: a dialog's WM_CLOSE is a Cancel. Win16's DefDlgProc posts
 * WM_COMMAND(IDCANCEL, BN_CLICKED) to the dialog, and the program's own
 * IDCANCEL handling decides what closing means (Charmap: end the program).
 *
 * [CAUTION]: Everything else goes to DefWindowProc on the real window: calling the OS's
 * DefDlgProc on a window WE created with CreateWindow is undefined (it reads
 * the dialog class's extra bytes), so a guest dialog keeps the ordinary
 * defaults and loses the dialog-specific keyboard ones (default button, ESC,
 * tab order) until real dialog creation lands.
 */
typedef struct _WOWUSER_DLGDEF
{
    WORD WParam;
    DWORD LParam;
} WOWUSER_DLGDEF;
/* [CAUTION]: AND THE LAST WINDOW WHOSE RECORD DispatchMessage RELEASED. It frees the slot
 * as it dispatches WM_DESTROY (see there), so when that WM_DESTROY reaches
 * DefDlgProc the window can no longer be found -- and WM_DESTROY is where a
 * dialog-as-main-window program (Charmap) calls PostQuitMessage. Measured s88:
 * "DefDlgProc 0x0140 msg=0x0002 -- no real window", the DLGPROC never ran, and
 * the program outlived its window. Its DLGPROC is kept here for that one call.
 */
typedef struct _WOWUSER_GONE
{
    WORD Window;
    DWORD DialogProcedure;
} WOWUSER_GONE;

/* Defined in wowuser.c (#335). */
extern WOWUSER_CLASS g_WowUserClasses[WOWUSER_MAX_CLASS];
extern WOWUSER_WINDOW g_WowUserWindows[WOWUSER_MAX_WIN];
extern WORD g_WowUserEnumTask;
extern WOWUSER_DLGDEF g_WowUserDlgDefaults[WOWCALL_MAX_DEPTH];
extern CHAR g_WowUserClipboard[WOWUSER_CLIPBOARD_SIZE];
extern INT g_WowUserClipboardLength;
extern WORD g_WowUserClipboardFormat;
extern WORD g_WowUserCurrentTask;
extern WORD g_WowUserKernelSegment;
extern INT (*g_WowUserCall16)(DWORD procedure, WORD dataSelector, PCWORD arguments, INT argumentCount, WORD window16, WORD message, PWORD result);
extern INT (*g_WowUserSend16)(WORD window16, WORD message, WORD wParam, DWORD lParam, PWORD result);
extern INT (*g_WowUserSend16Blob)(WORD window16, WORD message, WORD wParam, PBYTE blob, INT blobLength, const INT *fix, INT fixCount, PWORD result);

/* BATCH FOUR: what CARDFILE and WINFILE still want that is answerable (Importance = 2):
 *
 * [CAUTION]: THE COMM FAMILY ANSWERS "NO SUCH PORT", AND THAT IS THE TRUTH, NOT A STUB.
 * This host's BIOS equipment word claims ONE parallel port and NO serial ports
 * -- deliberately, because writing COM1..COM4 into the BDA would be inventing
 * hardware nothing answers for, and COMM.DRV's LibMain returns whatever is at
 * that BDA slot (docs/STATE.md, session 36). So there is no port to open, and
 * IE_BADID (-2) is exactly what Windows returns for a port id that does not
 * exist. CARDFILE uses these to AUTODIAL; it gets a clean refusal and works
 * without a modem, which is the same thing a real machine with no COM port
 * would give it.
 */
/* Provided by the host (main.c) over vdd_comm -- DECLARATIONS ONLY, because this
 * header must not see host internals. See the note beside them.
 */
INT  WowCommOpen(PCSTR dev);
INT  WowCommClose(INT id);
INT  WowCommRead(INT port, PBYTE buffer, INT count);
INT  WowCommWrite(INT port, PCBYTE buffer, INT count);
INT  WowCommInqueue(INT id);
VOID WowCommDtr(INT id, INT on);
VOID WowCommRts(INT id, INT on);
DWORD DpmiSelectorBase(WORD selector);
PWOWUSER_WINDOW WowUserNewWindow(VOID);
PWOWUSER_WINDOW WowUserFindWindow(WORD window16);
PWOWUSER_CLASS WowUserFindClass(PCSTR name);
WORD WowUserSystemResourceMintIcon(HICON icon);
HICON WowUserSystemResourceIcon(WORD token, PINT picked, INT width, INT height);
DWORD WowUserWindowProcedureOf(PCWOWUSER_WINDOW window);
WORD WowWinHwnd16(HWND window);
HWND WowUserHwnd32(WORD window16);
WORD WowUserMenu16(HMENU menu);
HMENU WowUserMenu32(WORD handle16);
INT WowUserIsDialog16(WORD window16);
WORD WowUserOwner16(WORD window16);
INT WowUserIsMdiChild(PCWOWUSER_WINDOW window);
HWND WowUserMdiClientOf(PCWOWUSER_WINDOW window);
DWORD WowUserTimerProcedure(WORD window16, WORD timerId);
LRESULT WowUserDlgDefault(
    PWOWUSER_WINDOW window,
    WORD dialog16,
    WORD message,
    WORD wParam16,
    DWORD lParam32,
    PSTR note,
    INT noteCapacity,
    PINT noteLengthInOut);
INT WowUserCall(PWOW32_FRAME frame, PSTR note, INT noteCapacity);

#endif /* WOWUSER_H */
