/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * USER.EXE's call table: the thunk ids WOW32's USER dispatcher answers, and their argument blocks.
 *
 * Split out of wowuser.h (STYLE.md 5a): the call table only -- every thunk id the
 * dispatcher answers and every argument offset, each with its note. wowuser.h holds the
 * host's own constants, types and prototypes, and includes this file.
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#ifndef NTVDMEX_WOWUSER_CALLS_H
#define NTVDMEX_WOWUSER_CALLS_H

#define WOWUSER_REGISTERCLASS               0x39
#define WOWUSER_CREATEWINDOW                0x29

/* 0xEF -- THE DIALOG HELPER, AND NO EXPORT MAPS TO IT (Importance = 5):
 * USER's wow32 id space is derived from its export table, and this id is in
 * one of the GAPS: `CreateDialog`, `DialogBoxParam` and `CreateDialogParam`
 * are implemented in USER's OWN 16-bit code, so `neneeds.py` classifies them
 * `native16` and reports them as costing us nothing. THEY DO NOT. That
 * 16-bit code does the FindResource/LoadResource/LockResource itself and then
 * calls OUT to the 32-bit side -- here -- to build the window and its
 * controls, because under WOW the windows are the 32-bit side's.
 *
 * `native16` IS NOT `free`, and this id is the proof: the tool said CALC had
 * 0 services to do, and CALC produced no window at all. (session 55)
 */
#define WOWUSER_CREATEDIALOG                0xEF
#define WOWUSER_NOTIFYWOW                   0x217

/* 0x16c LookupIconIdFromDirectoryEx(lpDir, fIcon, cx, cy, flags) = 12 (Importance = 3):
 * (s89, #216) Observed arriving inside every guest LoadCursor/LoadIcon,
 * between the GROUP resource being loaded and the entry it names being
 * looked up (fIcon 0 from LoadCursor, 1 from LoadIcon; both cx = cy = 0,
 * flags 0x40 = LR_DEFAULTSIZE). Stepped over, it answered 0, FindResource(hInst, 0,
 * RT_CURSOR) failed and LoadCursor returned NULL for EVERY cursor a program
 * ships -- Paintbrush's zoom rectangle among them. The group directory is the
 * same 6-byte header + 14-byte entries in Win16 and Win32, so the bytes go to
 * the OS's own LookupIconIdFromDirectoryEx as they are. Frame, reversed:
 * +0 flags, +2 cy, +4 cx, +6 fIcon, +8 the directory, far.
 */
#define WOWUSER_LOOKUPICONID                0x16c
#define WOWUSER_LII_ARG_FLAGS               0
#define WOWUSER_LII_ARG_CY                  2
#define WOWUSER_LII_ARG_CX                  4
#define WOWUSER_LII_ARG_FICON               6
#define WOWUSER_LII_ARG_DIR                 8
#define WOWUSER_SENDMESSAGE                 0x6f
#define WOWUSER_GETWINDOWWORD               0x85
#define WOWUSER_SETWINDOWWORD               0x86

/* The message loop. Every id here is named by USER's own export table,
 * and every one of them is a call this run
 * already makes -- see the frontier note in src/wow/wowmsg.h.
 */
#define WOWUSER_POSTQUITMESSAGE             0x06
#define WOWUSER_SETFOCUS                    0x16
#define WOWUSER_GETMESSAGE                  0x6c
#define WOWUSER_PEEKMESSAGE                 0x6d
#define WOWUSER_POSTMESSAGE                 0x6e

/* [INFO]: These four the export table could NOT name -- they are four of its 56
 * unnamed ids, and they are named here by where they arrive from in SYSEDIT's
 * message loop, matched against its NE import relocations. See wowmsg.h.
 */
#define WOWUSER_TRANSLATEMESSAGE            0x71
#define WOWUSER_DISPATCHMESSAGE             0x72
#define WOWUSER_TRANSLATEACCEL              0xb2
#define WOWUSER_TRANSLATEMDISYS             0x1c3

/* Visibility -- and with a real window behind it, this is the OS's job. */
#define WOWUSER_SHOWWINDOW                  0x2a
#define WOWUSER_UPDATEWINDOW                0x7c

/* A name in the system-wide clipboard atom table. CARDFILE and WRITE both stop
 * without it -- see the case. One argument: a far pointer to the name.
 */
#define WOWUSER_REGCLIPFORMAT               0x91
#define WOWUSER_RCF_ARG_NAME                0

/* 0x76 RegisterWindowMessage(lpString) (Importance = 3):
 * Another of the ids USER's export table cannot name, and NOTEPAD names it by
 * what it passes: it arrives twice at start-up, with the strings
 * "commdlg_FindReplace" and "commdlg_help" -- the two message names the common
 * dialogs are documented to register. Answered 0, Notepad abandons its whole
 * initialisation (observed): it shows its window and then throws it away, for
 * want of a message id.
 *
 * [INFO]: THE ANSWER IS THE OS's, for the same reason RegisterClipboardFormat's is: a
 * registered window message is a name in a SYSTEM-WIDE atom table, and its
 * whole purpose is that two programs which register the same string get the
 * same number. Our own table would agree with nothing.
 */
#define WOWUSER_REGWINMSG                   0x76
#define WOWUSER_RWM_ARG_NAME                0

/* SetWindowText(hWnd, lpString) -- 6 bytes, reversed: +0/+2 the string's far
 * pointer, +4 the window. Without it a window's caption stays whatever
 * CreateWindow was given, which for NOTEPAD is the empty string -- so its title
 * bar and its taskbar button are blank and it looks like a window with no name
 * rather than a program that has not been told to say one.
 */
#define WOWUSER_SETWINDOWTEXT               0x25
#define WOWUSER_SWT_ARG_TEXT                0
#define WOWUSER_SWT_ARG_HWND                4

/* 0xad -- "BUILD ME A CURSOR OR AN ICON" (Importance = 3):
 * One of the 56 ids USER's export table cannot name, and the reason it matters
 * is NOTEPAD: it calls `LoadCursor(NULL, 0x7f02)` at start-up, and if the
 * answer is 0 it abandons its whole initialisation and WinMain returns without
 * ever registering a class (observed). No cursor, no Notepad.
 *
 * USER's 16-bit LoadCursor/LoadIcon do the resource lookup themselves and then
 * ask the 32-bit side, through this id, to build the object. For a NULL
 * hInstance the block is (reversed, as logged):
 *
 *     +18  0                 (the hInstance -- NULL)
 *     +16  lpName HIGH word  (0 => MAKEINTRESOURCE)
 *     +14  lpName LOW word   = the ORDINAL
 *     +12..+4  0
 *     +2   a version word
 *     +0   KIND
 *
 * Notepad's call arrives as `(1 0 0 0 0 0 0 0x7f02 0 0)`. A call with kind 3
 * and a real resource pointer -- a module's OWN icon -- was NOT answered here at
 * first; the log names the kind so a run can say what it carries instead of
 * this being widened by guesswork.
 *
 * [INFO]: WHY THE HOST DOES NOT HAVE TO KNOW WHETHER IT IS A CURSOR OR AN ICON.
 * Win16's predefined cursor and icon ordinals share one numeric range, and both
 * LoadCursor and LoadIcon reach this same id -- and the GUEST says which it is
 * a moment later, when it puts the handle into
 * `WNDCLASS.hCursor` or `WNDCLASS.hIcon`. So this returns a TOKEN that remembers
 * the ordinal, and the real `LoadCursorA`/`LoadIconA` happens at the point of
 * use, where the answer is not a guess. Same shape as every other handle here:
 * synthetic to the guest, a real OS object behind it.
 */
#define WOWUSER_LOADSYSOBJ                  0xad
#define WOWUSER_AD_ARG_KIND                 0
#define WOWUSER_AD_ARG_NAMELO               14
#define WOWUSER_AD_ARG_NAMEHI               16
#define WOWUSER_AD_ARG_SIZE                 6
#define WOWUSER_AD_ARG_BITS                 10
#define WOWUSER_AD_ARG_HINST                18

/* 0xaf -- "BUILD ME A BITMAP FROM THESE RESOURCE BYTES" (Importance = 5):
 * MS Paint's second-to-last wall: with the registration, the DCs and the
 * CREATESTRUCT in place it gets as far as building its toolbox, fails to load
 * the bitmap for it, and says "Not enough memory to edit image."
 *
 * Another id USER's export table cannot name. It arrives with 14 argument
 * bytes, laid out (reversed, as logged) as:
 *
 *     +12  hInstance
 *     +8   lpszName, far  (SEG at +10, OFF at +8)
 *     +4   the resource's BYTES, far
 *     +0   its SIZE, a DWORD
 *
 * [INFO]: AND IT IS `LoadBitmap`, PROVEN THREE WAYS RATHER THAN INFERRED:
 * 1. It arrives exactly when the guest calls `USER.175 LOADBITMAP` (an
 *    export that does not thunk by itself -- `native16`), whose documented
 *    parameters are (HINSTANCE, LPCSTR) -- the hInstance and name above.
 * 2. The names that arrive are "pToolbox" and "pArrow".
 * 3. PBRUSH's own resource table has `BITMAP PTOOLBOX 9040` and
 *    `BITMAP PARROW 208` -- and the SIZE at +0 arrives as 0x2350 and 0x00d0.
 * A wrong reading does not produce three agreements.
 *
 * [INFO]: WHAT THE BYTES ARE, MEASURED AND NOT ASSUMED. Read straight out of
 * PBRUSH.EXE at the offsets `neres.py list` gives:
 *    PTOOLBOX  28 00 00 00 | 3a 00 00 00 | 17 01 00 00 | 01 00 | 04 00 | ...
 *    PARROW    28 00 00 00 | 0c 00 00 00 | 0d 00 00 00 | 01 00 | 04 00 | ...
 * `biSize` = 0x28 = 40, so these are BITMAPINFOHEADER (Windows 3.0) DIBs --
 * 58x279 and 12x13, 4bpp -- NOT the 12-byte BITMAPCOREHEADER form. A packed
 * DIB is header + palette + pixels, which is precisely what `CreateDIBitmap`
 * consumes, so USER's 16-bit half has already done all the resource work and
 * this is the one call it cannot make.
 *
 * [CAUTION]: PARROW's `biSizeImage` is 8, which is junk (12px at 4bpp padded is 8 bytes
 * per ROW, and 13 rows is 104 -- and 40 + 16*4 + 104 = 208, the whole
 * resource). It is ignored for BI_RGB, and this zeroes it in its own copy of
 * the header rather than trusting GDI to overlook it.
 *
 * [CAUTION]: THE CORE-HEADER FORM IS REFUSED, NOT GUESSED AT. No guest has produced one
 * here, so there is nothing to check a reading against.
 */
#define WOWUSER_LOADBITMAPRES               0x00af
#define WOWUSER_LBM_ARG_SIZE                0
#define WOWUSER_LBM_ARG_BITS                4
#define WOWUSER_LBM_ARG_NAME                8
#define WOWUSER_LBM_ARG_HINST               12
#define WOWUSER_WNDPROC_ARGUMENTS           5       /* Hwnd, msg, wParam, lParam high, low */
#define WOWUSER_WNDPROC_ARG_LPARAM          3       /* lParam's high word: a blob's far pointer */

/* THE LAYOUT CLUSTER -- WHAT MAKES NOTEPAD USABLE. (session 44) (Importance = 3):
 * Every id here was named by the RUN and confirmed against USER's own export
 * table, not chosen from a list: the host
 * log records each unimplemented USER call with the return address it came
 * from, and `tools/ne/neimports.py` matches that against NOTEPAD.EXE's own
 * NE import relocations. Four agreed both ways:
 *
 *    0x38  12 args  MOVEWINDOW
 *    0x7d   8 args  INVALIDATERECT
 *    0xb3   2 args  GETSYSTEMMETRICS
 *    0x1f   2 args  ISICONIC
 *
 * [INFO]: Notepad's resize arrives as InvalidateRect(hEdit, NULL, TRUE) followed by
 * MoveWindow(hEdit, 8, 2, cx - 15, cy - 4, TRUE) -- 8 bytes and 12 bytes
 * exactly, as each call carries, and the logged values put each field where
 * the block below says. The block is REVERSED as always (the base is the LAST
 * push).
 *
 * [CAUTION]: `DefWindowProc` is NOT here and must not be added: `USER.107` never arrives
 * as a BOP -- a whole run of Notepad produced none -- because USER handles it
 * in its own 16-bit code. `DefFrameProc` (0x1bd) and `DefMDIChildProc` (0x1bf)
 * DO thunk; nothing had called them when this was written.
 */
#define WOWUSER_MOVEWINDOW                  0x0038
#define WOWUSER_MW_ARG_REPAINT              0
#define WOWUSER_MW_ARG_CY                   2
#define WOWUSER_MW_ARG_CX                   4
#define WOWUSER_MW_ARG_Y                    6
#define WOWUSER_MW_ARG_X                    8
#define WOWUSER_MW_ARG_HWND                 10

#define WOWUSER_INVALIDATERECT              0x007d
#define WOWUSER_IR_ARG_ERASE                0
#define WOWUSER_IR_ARG_RECT                 2
#define WOWUSER_IR_ARG_HWND                 6

/* THE SMALL USER CALLS THE SHELF STILL ASKS FOR. (session 55):
 * Each is a Win32 function of the same name and meaning, so the body is a
 * handle translation and a call. They are listed together because they were
 * found together -- `neneeds.py --todo` over the whole shelf -- and because
 * doing them one at a time is how a batch of one-liners turns into a session.
 */
#define WOWUSER_ISCHILD                     0x0030  /* Ord 48, 4 args */
#define WOWUSER_ICH_ARG_HWND                0
#define WOWUSER_ICH_ARG_PARENT              2

#define WOWUSER_VALIDATERECT                0x007f  /* Ord 127, 6 args */
#define WOWUSER_VR_ARG_RECT                 0
#define WOWUSER_VR_ARG_HWND                 4

#define WOWUSER_INVALIDATERGN               0x007e  /* Ord 126, 6 args */
#define WOWUSER_IRG_ARG_ERASE               0
#define WOWUSER_IRG_ARG_RGN                 2
#define WOWUSER_IRG_ARG_HWND                4

#define WOWUSER_GETCARETBLINK               0x00a9  /* Ord 169, 0 args */
#define WOWUSER_INSENDMESSAGE               0x00c0  /* Ord 192, 0 args */

#define WOWUSER_GETNEXTWINDOW               0x00e6  /* Ord 230, 4 args */
#define WOWUSER_GNW_ARG_FLAG                0
#define WOWUSER_GNW_ARG_HWND                2

#define WOWUSER_HILITEMENUITEM              0x00a2  /* Ord 162, 8 args */
#define WOWUSER_HMI_ARG_FLAGS               0
#define WOWUSER_HMI_ARG_ITEM                2
#define WOWUSER_HMI_ARG_MENU                4
#define WOWUSER_HMI_ARG_HWND                6

/* 0x7a CallWindowProc -- AND IT IS ONE OF THE FOUR SINGLE CALLS THAT
 * EACH BLOCK A GUEST. (CARDFILE) -----------------------------------------
 * A subclassing program keeps the procedure it displaced and calls it for
 * everything it does not handle. The displaced procedure here can be either
 * kind, and that is the whole difficulty: a 16-bit one has to go through
 * wowcall.h, and a window of one of OUR system classes has no 16-bit
 * procedure at all and must go to the OS.
 */
#define WOWUSER_CALLWINDOWPROC              0x007a  /* Ord 122, 14 args */
#define WOWUSER_CWP_ARG_LPARAM              0
#define WOWUSER_CWP_ARG_WPARAM              4
#define WOWUSER_CWP_ARG_MSG                 6
#define WOWUSER_CWP_ARG_HWND                8
#define WOWUSER_CWP_ARG_PROC                10

#define WOWUSER_GETSYSTEMMETRICS            0x00b3
#define WOWUSER_GSM_ARG_INDEX               0

#define WOWUSER_ISICONIC                    0x001f
#define WOWUSER_II_ARG_HWND                 0

/* THE ENUMERATED BATCH -- FROM `tools/ne/neneeds.py`, NOT FROM A RUN (Importance = 3):
 * Everything above was named by a guest stopping on it. These were named by
 * reading NOTEPAD.EXE's import table -- so they are calls the program provably
 * can make, including the ones that would have failed QUIETLY. See the tool for
 * why an import is not automatically work.
 *
 * Every argument block below is the standard reversal (the base is the LAST
 * word pushed), and every one is cross-checked against the argument-byte count
 * the call carries:
 *
 * 0x17  GETFOCUS               0 bytes   ()
 * 0x22  ENABLEWINDOW           4 bytes   (hWnd, bEnable)          2+2
 * 0x35  DESTROYWINDOW          2 bytes   (hWnd)                   2
 * 0x3b  SETACTIVEWINDOW        2 bytes   (hWnd)                   2
 * 0x68  MESSAGEBEEP            2 bytes   (uType)                  2
 * 0x89  OPENCLIPBOARD          2 bytes   (hWnd)                   2
 * 0x8a  CLOSECLIPBOARD         0 bytes   ()
 * 0x90  ENUMCLIPBOARDFORMATS   2 bytes   (wFormat)                2
 * A parameter list that did not add up to the declared count would mean the
 * reading is wrong, and all eight add up.
 */
/* 0x01 MessageBox -- AND IT IS NOT IN THE IMPORT-DERIVED LIST (Importance = 4):
 * `neneeds.py` classifies `USER.1 MESSAGEBOX` as native16 -- the export does
 * not thunk by itself -- yet a call to it still reaches us, as this id, from
 * inside USER's 16-bit side: exactly the `LoadIcon`/`0xad` shape the tool's own
 * header warns about. So this is the first thing the enumeration could not
 * see, found the old way: a run stopped on it.
 *
 * [INFO]: AND IT IS THE MOST VALUABLE ONE IN THE FILE, because it is how the program
 * TALKS. Notepad reached this immediately after reading the file it was asked
 * to open, with `uType = 0x30` (an exclamation) and the caption "Notepad" --
 * i.e. it had already decided something was wrong and was trying to say what.
 * Dropping the call threw the sentence away and left "nothing happened", which
 * is why two sessions of this have been guesswork. An implemented MessageBox
 * turns every future failure of this kind into a sentence on the screen and in
 * the log.
 *
 * [INFO]: The block, from the arguments the run itself printed (`0x0030 0x265a 0x0a9e
 * ...`, where `0x0a9e:0x265a` decoded to "Notepad"): reversed as always, so
 * uType is at +0 and hWnd -- pushed first -- is at +10. 2+4+4+2 = 12, which is
 * what the call carries.
 *
 * [CAUTION]: MODAL, on the exec thread, like ShellAbout and the file dialog.
 */
#define WOWUSER_MESSAGEBOX                  0x0001
#define WOWUSER_MSGB_ARG_TYPE               0
#define WOWUSER_MSGB_ARG_CAPTION            2
#define WOWUSER_MSGB_ARG_TEXT               6
#define WOWUSER_MSGB_ARG_HWND               10

#define WOWUSER_GETFOCUS                    0x0017
#define WOWUSER_ENABLEWINDOW                0x0022
#define WOWUSER_EW_ARG_ENABLE               0
#define WOWUSER_EW_ARG_HWND                 2
#define WOWUSER_DESTROYWINDOW               0x0035
#define WOWUSER_DW_ARG_HWND                 0
#define WOWUSER_SETACTIVEWINDOW             0x003b
#define WOWUSER_SAW_ARG_HWND                0
#define WOWUSER_MESSAGEBEEP                 0x0068
#define WOWUSER_MB_ARG_TYPE                 0
#define WOWUSER_OPENCLIPBOARD               0x0089
#define WOWUSER_OC_ARG_HWND                 0
#define WOWUSER_CLOSECLIPBOARD              0x008a
#define WOWUSER_ENUMCLIPFMT                 0x0090
#define WOWUSER_ECF_ARG_FORMAT              0

/* THE DEVICE-CONTEXT TRIO -- WHERE MS PAINT ACTUALLY STOPS (Importance = 5):
 * Session 44 left a note saying the next thing for Paint was GDI's producers
 * (`CreateDC`, `CreateCompatibleDC`, `GetStockObject`). A run of PBRUSH.EXE
 * says otherwise, and the program said it in English: it puts up
 *
 *     Paintbrush: "Not enough memory to perform this operation."
 *
 * and the call immediately before that box is USER id 0x42, 2 argument bytes,
 * argument 0x0140 -- the hwnd Paint had just created. That is `GetDC`, it was
 * answered 0, and Paint reads a null DC as being out of memory.
 *
 * [INFO]: SO THE FIRST DC THIS HOST EVER ISSUES COMES OUT OF **USER**, NOT GDI. That
 * is worth stating plainly because it inverts the plan that was written down:
 * the three GDI calls already implemented (`GetDeviceCaps`, `DeleteDC`,
 * `DeleteObject`) could only ever answer "not one of our GDI tokens" until
 * something produced one, and the producer was in another id space all along.
 *
 * [INFO]: The ids, argument counts and return-stub offsets (the value each call
 * carries at OFF_FROM), from `tools/ne/neneeds.py --stubs`; the run printed
 * the same value to the digit where it reached one:
 *     66 GETDC        id 0x42   2 args  retstub 0x076a   (the run: 0x076a)
 *     67 GETWINDOWDC  id 0x43   2 args  retstub 0x090a
 *     68 RELEASEDC    id 0x44   4 args  retstub 0x0c5c
 * 2 = (HWND); 4 = (HWND, HDC). Both add up to what the calls carry.
 *
 * [CAUTION]: 0x44 IS `DeleteDC` IN GDI'S NUMBERING AND `ReleaseDC` HERE -- the same
 * collision this file exists to prevent, and a reason the dispatcher must
 * never reach this switch with another module's stub segment.
 */
/* 0x21 GetClientRect -- WHY MS PAINT LAID ITSELF OUT WRONG (Importance = 5):
 * The single call behind "the way it paints is completely wrong". Measured
 * against STOCK ntvdm running the SAME PBRUSH.EXE on the SAME box, via
 * `tree`, with both frames at an identical 1252x688 client:
 *
 *     child      stock (the oracle)     ours
 *     pbPaint    at(128,2) 1100x604     at(7,4)    1682x976
 *     pbTool     at(3,2)    121x516     at(4,2)     163x731
 *     pbSize     at(3,521)  121x164     at(4,736)   163x235
 *     pbColor    at(128,608)1099x66     at(172,860)1475x94
 *
 * Ours are laid out for a 1680x974 client -- which is exactly the `width` and
 * `height` Paint reads from WIN.INI's [Paintbrush] section. It falls back to
 * those because it asked how big it actually was and nobody answered: USER
 * id 0x21, 6 argument bytes, `(lpRect far, hWnd=0x0160)` = GetClientRect on
 * its own frame, stepped over. So the palette and the line-size box were laid
 * out BELOW THE BOTTOM of the window and the toolbox was taller than its own
 * parent -- which is why the only thing on screen was a column of stray lines.
 *
 * [INFO]: Nothing was wrong with the drawing. The drawing was faithful; it was drawing
 * the right picture at the wrong size, in a window the guest had been given no
 * way to measure.
 *
 * [CAUTION]: A Win16 RECT IS FOUR `int`s = 8 BYTES, against Win32's four LONGs = 16.
 *
 * AND THE THREE THAT CAME WITH IT, each named from its own call:
 * 0x3e  8 args  (bRedraw=1, nPos=0, nBar=0, hWnd=0x180)      SetScrollPos
 * 0x40 10 args  (0, nMax=0x68e, nMin=0, nBar=0, hWnd=0x180)  SetScrollRange
 * 0x45  2 args  (hCursor)                                     SetCursor
 *
 * [INFO]: `nMax = 0x68e` = 1678 is the canvas width, which is what identifies 0x40:
 * a scroll RANGE over the image. These two are the missing scrollbars the
 * oracle shows on the canvas and ours does not have. SB_HORZ/SB_VERT/SB_CTL
 * are 0/1/2 in both worlds.
 */
#define WOWUSER_GETCLIENTRECT               0x0021
#define WOWUSER_GCR_ARG_RECT                0
#define WOWUSER_GCR_ARG_HWND                4

/* SOLITAIRE AND MINESWEEPER -- ENUMERATED, NOT DISCOVERED (Importance = 1):
 * Every id below came out of `tools/ne/neneeds.py --todo` run on the two
 * binaries, so each is a call one of them really makes, and the argument-BYTE
 * count the tool prints is what pins each offset table. The rule, and it is
 * the one that has been got wrong most often here:
 *
 * THE ARGUMENT BLOCK IS REVERSED. Win16 is FAR PASCAL, so arguments are
 * pushed LEFT TO RIGHT and the block's base is the LAST push -- offset 0 is
 * the RIGHTMOST parameter. A far pointer is 4 bytes, an int/HWND/HDC is 2.
 *
 * Each table below is annotated with the prototype it was derived from and
 * adds up to the byte count neneeds reported; if a call misbehaves, check the
 * sum first -- an offset table that does not total the reported width is
 * wrong by construction.
 */

/* UINT SetTimer(HWND, UINT nIDEvent, UINT wElapse, FARPROC lpTimerFunc) = 10 */
#define WOWUSER_SETTIMER                    0x000a
#define WOWUSER_ST_ARG_PROC                 0       /* Far */
#define WOWUSER_ST_ARG_ELAPSE               4
#define WOWUSER_ST_ARG_ID                   6
#define WOWUSER_ST_ARG_HWND                 8

/* BOOL KillTimer(HWND, UINT nIDEvent) = 4
 *
 * [CAUTION]: NOT in either program's TO-DO list -- KillTimer resolves to 16-bit code in
 * USER, which then calls DOWN to this id. neneeds cannot see that call (it is
 * not an import), which is exactly the `native16 does not mean free` trap in its
 * own header. Arming a timer with no way to disarm it is a leak per game, so it
 * is implemented alongside SetTimer rather than waiting for a run to show it.
 */
#define WOWUSER_KILLTIMER                   0x000b
#define WOWUSER_KT_ARG_ID                   0
#define WOWUSER_KT_ARG_HWND                 2

/* DWORD GetCurrentTime(void) = 0 */
#define WOWUSER_GETCURRENTTIME              0x000f

/* HWND FindWindow(LPCSTR lpClassName, LPCSTR lpWindowName) = 8 */
#define WOWUSER_FINDWINDOW                  0x0032
#define WOWUSER_FW_ARG_NAME                 0       /* Far */
#define WOWUSER_FW_ARG_CLASS                4       /* Far */

/* int FrameRect(HDC, LPRECT, HBRUSH) = 8
 *
 * [CAUTION]: `FRAMER_`, NOT `FR_`: FillRect (0x0051) already owns that prefix further down
 * this file. The two prototypes happen to be identical, so the values coincide
 * and this collision was harmless -- which is exactly why it survived. If either
 * ever gained an argument, the later definition would silently win and one of
 * them would read the other's layout. Caught by tests/unit/wow_test.c on its
 * first run, not by anybody reading the build output.
 */
#define WOWUSER_FRAMERECT                   0x0053
#define WOWUSER_FRAMER_ARG_BRUSH            0
#define WOWUSER_FRAMER_ARG_RECT             2       /* Far */
#define WOWUSER_FRAMER_ARG_HDC              6

/* int DrawText(HDC, LPCSTR, int nCount, LPRECT, UINT uFormat) = 14 */
#define WOWUSER_DRAWTEXT                    0x0055
#define WOWUSER_DT_ARG_FORMAT               0
#define WOWUSER_DT_ARG_RECT                 2       /* Far */
#define WOWUSER_DT_ARG_COUNT                6
#define WOWUSER_DT_ARG_STR                  8       /* Far */
#define WOWUSER_DT_ARG_HDC                  12

/* void SetDlgItemText(HWND hDlg, int nIDDlgItem, LPCSTR) = 8 */
#define WOWUSER_SETDLGITEMTEXT              0x005c
#define WOWUSER_SDIT_ARG_TEXT               0       /* Far */
#define WOWUSER_SDIT_ARG_ID                 4
#define WOWUSER_SDIT_ARG_HDLG               6

/* UINT GetDlgItemInt(HWND, int nIDDlgItem, BOOL FAR *lpTranslated, BOOL) = 10 */
#define WOWUSER_GETDLGITEMINT               0x005f
#define WOWUSER_GDII_ARG_SIGNED             0
#define WOWUSER_GDII_ARG_XLATED             2       /* Far */
#define WOWUSER_GDII_ARG_ID                 6
#define WOWUSER_GDII_ARG_HDLG               8

/* void CheckRadioButton(HWND, int nIDFirst, int nIDLast, int nIDCheck) = 8 */
#define WOWUSER_CHECKRADIOBUTTON            0x0060
#define WOWUSER_CRB_ARG_CHECK               0
#define WOWUSER_CRB_ARG_LAST                2
#define WOWUSER_CRB_ARG_FIRST               4
#define WOWUSER_CRB_ARG_HDLG                6

/* void CheckDlgButton(HWND, int nIDButton, UINT uCheck) = 6 */
#define WOWUSER_CHECKDLGBUTTON              0x0061
#define WOWUSER_CDB_ARG_CHECK               0
#define WOWUSER_CDB_ARG_ID                  2
#define WOWUSER_CDB_ARG_HDLG                4

/* UINT IsDlgButtonChecked(HWND, int nIDButton) = 4 */
#define WOWUSER_ISDLGBUTTONCHECKED          0x0062
#define WOWUSER_IDBC_ARG_ID                 0
#define WOWUSER_IDBC_ARG_HDLG               2

/* void AdjustWindowRect(LPRECT, DWORD dwStyle, BOOL bMenu) = 10 */
#define WOWUSER_ADJUSTWINDOWRECT            0x0066
#define WOWUSER_AWR_ARG_MENU                0
#define WOWUSER_AWR_ARG_STYLE               2       /* DWORD */
#define WOWUSER_AWR_ARG_RECT                6       /* Far */

/* 0x96 -- USER's OWN DOWNCALL FOR LoadMenu. 16 arg bytes (Importance = 3):
 * NOT the exported LoadMenu, which is 16-bit code and never reaches us. This
 * is what that code calls DOWN to once it has found and locked the resource,
 * the same shape as 0xad for icons -- so it is invisible to neneeds.py, which
 * only sees a program's imports. WINMINE's USER surface reads 41/41 COMPLETE
 * and it still had no menu. **native16 does not mean free.**
 *
 * The block, as it arrives (eight words, reversed):
 *
 *     offset 14  hInstance
 *     offset 10  lpMenuName (high word at 12)
 *     offset 6   the locked resource, far (segment at 8)
 *     offsets 4, 2, 0  -- not identified
 *
 * Minesweeper's live call carries hInstance 0x0b86 and lpMenuName 0x000001f4
 * -- a MAKEINTRESOURCE whose high word is 0, so an ORDINAL -- and
 * `neres.py list WINMINE.EXE` says `MENU 500`. 0x1f4 IS 500. The id was in the
 * arguments the whole time.
 *
 * [CAUTION]: Offsets 0/2/4 carry values this host does not need and are NOT
 * named as anything: guessing at them would put three inventions in a header
 * that is otherwise all measurement. They are covered so the block tiles.
 */
#define WOWUSER_LOADMENU                    0x0096
#define WOWUSER_LOADMENU_ARG_LOCAL0         0
#define WOWUSER_LOADMENU_ARG_LOCAL2         2
#define WOWUSER_LOADMENU_ARG_LOCAL4         4
#define WOWUSER_LOADMENU_ARG_RES            6       /* Far -- the locked resource */
#define WOWUSER_LOADMENU_ARG_NAME           10      /* MAKEINTRESOURCE, or a far string */
#define WOWUSER_LOADMENU_ARG_HINST          14

/* BOOL SetMenu(HWND, HMENU) = 4
 *
 * [CAUTION]: NOT `SM_ARG_*`: SendMessage already owns that prefix further down this file
 * and redefines it to 8. The LAST definition before the use wins, so a SetMenu
 * written with `WOWUSER_SM_ARG_HWND` silently reads SendMessage's offset and the
 * compiler says only "redefined". Prefixes here are a namespace, not a habit.
 */
#define WOWUSER_SETMENU                     0x009e
#define WOWUSER_SETMENU_ARG_MENU            0
#define WOWUSER_SETMENU_ARG_HWND            2

/* HWND GetLastActivePopup(HWND hwndOwner) = 2 */
#define WOWUSER_GETLASTACTIVEPOPUP          0x011f
#define WOWUSER_GLAP_ARG_HWND               0

/* 0x2f IsWindow / 0x31 IsWindowVisible -- WHY THE TOOLBOX NEVER MOVED.
 * Two of the smallest calls in USER, and between them they were the whole of
 * the second half of "the way it paints is completely wrong".
 *
 * After `GetClientRect` went in, Paint's WM_SIZE handler did exactly one thing:
 * `MoveWindow(pbPaint, (3,2) 1251x687)` -- it gave the CANVAS the entire client
 * and moved nothing else, while stock puts the canvas at (128,2) 1100x604 and
 * leaves room around it. Paint had decided there was no toolbox and no palette
 * to leave room for, and it decided that by ASKING:
 *
 *     IsWindowVisible(0x01a0)   x2   -- pbTool,  the toolbox
 *     IsWindowVisible(0x01e0)        -- pbColor, the palette
 *     IsWindow(0x0180)          x2   -- pbPaint, the canvas
 *
 * all stepped over, all answered 0. => Paint believed its own toolbox and
 * palette were hidden -- so it never resized them, and they kept the size they
 * were CREATED at, which came from Paint's fallback window height of 974
 * (`SM_CYFULLSCREEN - SM_CYMENU`, the default it passes to `GetProfileInt`
 * because this test machine's WIN.INI has no `[Paintbrush] height`). That is why they
 * were ~1.35x too large and fell below the bottom of a 688-tall client.
 *
 * [INFO]: Windows 3.1 Paintbrush can genuinely hide both (View > Tools and Linesize,
 * View > Palette), so "is it visible" is a real question with a real answer,
 * and answering 0 was not a harmless default -- it was a lie about the state
 * of windows this host had itself created and shown.
 *
 * [CAUTION]: TWO REFUTED HYPOTHESES ARE BURIED HERE, both plausible and both wrong:
 * `GetDeviceCaps(HORZSIZE/VERTSIZE)` (forcing the ratio to stock's 2.0 changed
 * the toolbox by nothing) and "the guest is never told its size" (WM_SIZE has
 * been relayed since session 43, and the log shows it arriving with the
 * correct 1252x688). The layout was not mis-computed; it was never
 * re-computed.
 */
#define WOWUSER_ISWINDOW                    0x002f

/* THE SHELF, BATCH ONE: TASKMAN, CLOCK AND CALC. (session 53) (Importance = 3):
 * `tools/ne/neneeds.py` says these three programs are 2, 5 and 4 services away
 * from launching -- not "roughly", but by name, from their own import tables.
 * Ten of them are here and ExtTextOut is in wowgdi.h.
 *
 * [CAUTION]: EVERY ONE OF THESE IS THE REAL WIN32 CALL ON THE GUEST'S REAL WINDOW, for the
 * same reason the menu and the caption are: these ARE Win32 windows, and an
 * answer composed here would be a second opinion about state the OS already
 * owns. Where that is NOT possible it is said so at the call, loudly, rather
 * than answered plausibly -- see the clipboard pair.
 */

/* UINT ArrangeIconicWindows(HWND) -- TASKMAN's whole reason to exist. */
#define WOWUSER_ARRANGEICONICWINDOWS        0x00aa
#define WOWUSER_AIW_ARG_HWND                0

/* void SwitchToThisWindow(HWND, BOOL fAltTab) -- TASKMAN's "Switch To" button.
 *
 * [CAUTION]: USER32 exports it undocumented and no import library declares it, so this is
 * the documented pair that does the same job: restore it if minimised, then put
 * it in front. A guest cannot tell the difference; a linker can.
 */
#define WOWUSER_SWITCHTOTHISWINDOW          0x00ac
#define WOWUSER_STW_ARG_ALTTAB              0
#define WOWUSER_STW_ARG_HWND                2

/* DWORD GetDialogBaseUnits(void) -- LOWORD x, HIWORD y. */
#define WOWUSER_GETDIALOGBASEUNITS          0x00f3

/* SHORT GetAsyncKeyState(int vk) -- Win16 and Win32 agree on the shape: bit 15
 * is "down now", bit 0 "pressed since the last call".
 */
#define WOWUSER_GETASYNCKEYSTATE            0x00f9
#define WOWUSER_GAKS_ARG_VK                 0

/* BOOL IsZoomed(HWND). Same family as IsIconic, and the same reason to ask the
 * OS rather than to answer 0: the wrong answer is right most of the time.
 */
#define WOWUSER_ISZOOMED                    0x0110
#define WOWUSER_IZ_ARG_HWND                 0

/* BOOL AppendMenu(HMENU, UINT flags, UINT id, LPCSTR item) -- CLOCK builds its
 * own menu at run time rather than from a resource.
 */
#define WOWUSER_APPENDMENU                  0x019b
#define WOWUSER_AM_ARG_ITEM                 0
#define WOWUSER_AM_ARG_ID                   4
#define WOWUSER_AM_ARG_FLAGS                6
#define WOWUSER_AM_ARG_HMENU                8

/* BOOL IsDialogMessage(HWND hDlg, LPMSG lpMsg) -- CALC's message loop. */
#define WOWUSER_ISDIALOGMESSAGE             0x005a
#define WOWUSER_IDM_ARG_MSG                 0
#define WOWUSER_IDM_ARG_HDLG                4

/* void MapDialogRect(HWND hDlg, LPRECT lprc) -- dialog units to client pixels. */
#define WOWUSER_MAPDIALOGRECT               0x0067
#define WOWUSER_MDR_ARG_RECT                0
#define WOWUSER_MDR_ARG_HDLG                4

/* THE CLIPBOARD PAIR -- AND THE BRIDGE THAT LETS BOTH TELL THE TRUTH. (#160):
 * GetClipboardData must hand back a handle the GUEST can lock -- a 16-bit
 * global handle in its own address space. The real Win32 handle is not that, so
 * for s40-s81 the honest pair was "nothing available, nothing returned".
 *
 * [INFO]: The bridge: for TEXT (CF_TEXT, CF_OEMTEXT) the host asks krnl386 itself for a
 * global block (KERNEL.15 GlobalAlloc through wowcall), locks it (KERNEL.18),
 * copies the host clipboard's text in and unlocks it (KERNEL.19) -- the chain in
 * WOWCALL_ACT_CLIP*. The guest gets a handle its own KERNEL made.
 *
 * [CAUTION]: ONE VOICE STILL: availability is answered from the host ONLY for the formats
 * the bridge can deliver, and only when it can run (callbacks on, krnl386's
 * segment known). Available-but-empty is a state no real clipboard is ever in.
 *
 * [CAUTION]: The block is GMEM_DDESHARE and belongs to the asking task; it is freed when
 * that task ends, so each paste costs one block for the task's lifetime.
 */
#define WOWUSER_GETCLIPBOARDDATA            0x008e
#define WOWUSER_ISCLIPBOARDFORMATAVAILABLE  0x00c1
#define WOWUSER_CB_ARG_FORMAT               0

/* HANDLE SetClipboardData(UINT fmt, HANDLE hMem) -- USER.141, between Empty (139),
 * GetClipboardOwner (140) and Get (142). Pascal: hMem is the last argument, so it
 * sits lowest.
 */
#define WOWUSER_SETCLIPBOARDDATA            0x008d
#define WOWUSER_SCD_ARG_HMEM                0
#define WOWUSER_SCD_ARG_FORMAT              2

/* THE SHELF, BATCH TWO: RECORDER, MPLAYER AND CHARMAP. (session 53) (Importance = 3):
 * Priced by neneeds.py at 9, 11 and 13 services. Most are a Win32 call with a
 * handle translated at each end, and they are written out plainly rather than
 * wrapped in a macro: the ones that are NOT plain (a 256-byte key array, a
 * RECT that is 8 bytes not 16, a hook chain we do not own) are the ones worth
 * seeing, and a macro would hide them among the ones that are.
 *
 * [CAUTION]: NONE of these composes an answer. Where the OS cannot be asked, the call
 * says so at the site instead of returning something plausible.
 */
#define WOWUSER_ISWINDOWENABLED             0x0023
#define WOWUSER_GETWINDOWTEXTLENGTH         0x0026
#define WOWUSER_GETWINDOWTEXT               0x0024  /* USER.36, 8 args: see its case */
#define WOWUSER_GWT_ARG_MAX                 0
#define WOWUSER_GWT_ARG_BUF                 2
#define WOWUSER_GWT_ARG_HWND                6
#define WOWUSER_WINDOWFROMPOINT             0x001e
#define WOWUSER_FLASHWINDOW                 0x0069
#define WOWUSER_GETCAPTURE                  0x00ec
#define WOWUSER_GETKEYBOARDSTATE            0x00de
#define WOWUSER_SETKEYBOARDSTATE            0x00df
#define WOWUSER_VKKEYSCAN                   0x0081
#define WOWUSER_MAPVIRTUALKEY               0x0083
#define WOWUSER_EMPTYCLIPBOARD              0x008b
#define WOWUSER_GETUPDATERECT               0x00be
#define WOWUSER_GETNEXTDLGTABITEM           0x00e4
#define WOWUSER_GETDLGCTRLID                0x0115
#define WOWUSER_DRAWFOCUSRECT               0x01d2
#define WOWUSER_DELETEMENU                  0x019d
#define WOWUSER_GETWINDOWPLACEMENT          0x0172
#define WOWUSER_UNHOOKWINDOWSHOOK           0x00ea

/* s93: USER.121 SetWindowsHook arrives as an 8-byte block: lpfn (far) at +0,
 * nFilterType at +4, and at +6 the module handle owning the hook procedure (as
 * logged); the DX:AX answered is what SetWindowsHook returns ("the previous
 * hook").
 */
#define WOWUSER_SETWINDOWSHOOK              0x0079
#define WOWUSER_SWH_ARG_PROC                0
#define WOWUSER_SWH_ARG_ID                  4
#define WOWUSER_SWH_ARG_HMOD                6
#define WOWUSER_DEFHOOKPROC                 0x00eb
#define WOWUSER_SYSTEMPARAMETERSINFO        0x01e3

#define WOWUSER_W1_ARG_HWND                 0       /* Every one-word (HWND) call */

/* [CAUTION]: s90: WAS y-at-+0 FROM s53 TO s90, WRITTEN FROM REASONING AND NEVER MEASURED.
 * w_misc `wfp.visible` (a visible popup at (300,500), point (350,520)) read the
 * window under stock and NOT under ours. The POINT lies in field order: x, y.
 */
#define WOWUSER_WFP_ARG_X                   0       /* WindowFromPoint(POINT) */
#define WOWUSER_WFP_ARG_Y                   2

/* s90 (#297): the USER singles. Frames reversed as always (+0 = last param). */
#define WOWUSER_GETCLIPBOARDFORMATNAME      0x0092  /* (fmt, buf, cch) 8 */
#define WOWUSER_GCFN_ARG_CCH                0
#define WOWUSER_GCFN_ARG_BUF                2
#define WOWUSER_GCFN_ARG_FMT                6
#define WOWUSER_DLGDIRSELECT                0x0063  /* (hDlg, lpString, nIDListBox) 8 */
#define WOWUSER_DDS_ARG_ID                  0
#define WOWUSER_DDS_ARG_STR                 2
#define WOWUSER_DDS_ARG_HDLG                6
#define WOWUSER_SETPARENT                   0x00e9  /* (hwndChild, hwndNewParent) 4 */
#define WOWUSER_SPA_ARG_NEW                 0
#define WOWUSER_SPA_ARG_CHILD               2
#define WOWUSER_GETCLASSINFO                0x0194  /* (hInst, lpszClass, lpWndClass) 10 */
#define WOWUSER_GCI_ARG_WC                  0
#define WOWUSER_GCI_ARG_NAME                4
#define WOWUSER_GCI_ARG_HINST               8
#define WOWUSER_CHILDWINDOWFROMPOINT        0x00bf  /* (hwnd, POINT) -- POINT as WFP_ 6 */

/* [CAUTION]: MEASURED (w_misc, s90): x at +0, y at +2 -- the POINT lies in the frame in
 * field order. The first cut copied WFP_'s y-first reading and stock disagreed
 * on both asymmetric cases.
 */
#define WOWUSER_CWFP_ARG_X                  0
#define WOWUSER_CWFP_ARG_Y                  2
#define WOWUSER_CWFP_ARG_HWND               4
#define WOWUSER_CALLMSGFILTER               0x007b  /* (lpMsg, nCode) 6 */

/* s90 (#296): EnumProps(hwnd, proc) = 6, reversed: +0 proc, +4 hwnd. */
#define WOWUSER_ENUMPROPS                   0x001b
#define WOWUSER_EPR_ARG_PROC                0
#define WOWUSER_EPR_ARG_HWND                4

/* GetInternalIconHeader(lp, lp) -- undocumented; stock answers 0 (w_misc `giih`). */
#define WOWUSER_GETINTERNALICONHEADER       0x0174
#define WOWUSER_CMF16_ARG_CODE              0
#define WOWUSER_CMF16_ARG_MSG               2
#define WOWUSER_FW_ARG_INVERT               0
#define WOWUSER_FW_ARG_HWND                 2
#define WOWUSER_KS_ARG_BUF                  0       /* Far pointer to 256 bytes */
#define WOWUSER_VKS_ARG_CHAR                0
#define WOWUSER_MVK_ARG_TYPE                0
#define WOWUSER_MVK_ARG_CODE                2
#define WOWUSER_GUR_ARG_ERASE               0
#define WOWUSER_GUR_ARG_RECT                2
#define WOWUSER_GUR_ARG_HWND                6
#define WOWUSER_GNDTI_ARG_PREV              0
#define WOWUSER_GNDTI_ARG_CTL               2
#define WOWUSER_GNDTI_ARG_HDLG              4
#define WOWUSER_DFR_ARG_RECT                0
#define WOWUSER_DFR_ARG_HDC                 4
#define WOWUSER_DM_ARG_FLAGS                0
#define WOWUSER_DM_ARG_POS                  2
#define WOWUSER_DM_ARG_HMENU                4
#define WOWUSER_GWP_ARG_PL                  0
#define WOWUSER_GWP_ARG_HWND                4
#define WOWUSER_SPI_ARG_WINI                0
#define WOWUSER_SPI_ARG_PARAM               2
#define WOWUSER_SPI_ARG_UIPARAM             6
#define WOWUSER_SPI_ARG_ACTION              8

#define WOWUSER_DLGDIRLIST                  0x0064
#define WOWUSER_DDL_ARG_FILETYPE            0
#define WOWUSER_DDL_ARG_IDSTATIC            2
#define WOWUSER_DDL_ARG_IDLIST              4
#define WOWUSER_DDL_ARG_SPEC                6
#define WOWUSER_DDL_ARG_HDLG                10

/* [CAUTION]: ChangeMenu IS FIVE FUNCTIONS BEHIND ONE ORDINAL -- the Win16 legacy
 * multiplexer that predates Append/Insert/Modify/Delete/Remove. The action is
 * in the FLAGS, not in the name, so dispatching on them is the whole job; a
 * version that only appended would silently do the wrong thing four times out
 * of five.
 */
#define WOWUSER_CHANGEMENU                  0x0099
#define WOWUSER_CM_ARG_CHANGE               0
#define WOWUSER_CM_ARG_IDNEW                2
#define WOWUSER_CM_ARG_ITEM                 4
#define WOWUSER_CM_ARG_IDCHANGE             8
#define WOWUSER_CM_ARG_HMENU                10

#define WOWUSER_GRAYSTRING                  0x00b9
#define WOWUSER_GS_ARG_HEIGHT               0
#define WOWUSER_GS_ARG_WIDTH                2
#define WOWUSER_GS_ARG_Y                    4
#define WOWUSER_GS_ARG_X                    6
#define WOWUSER_GS_ARG_COUNT                8
#define WOWUSER_GS_ARG_DATA                 10
#define WOWUSER_GS_ARG_OUTFUNC              14
#define WOWUSER_GS_ARG_HBRUSH               18
#define WOWUSER_GS_ARG_HDC                  20

#define WOWUSER_DEFDLGPROC                  0x0134
#define WOWUSER_DDP_ARG_LPARAM              0
#define WOWUSER_DDP_ARG_WPARAM              4
#define WOWUSER_DDP_ARG_MSG                 6
#define WOWUSER_DDP_ARG_HDLG                8

/* THE SHELF, BATCH THREE. (session 53) (Importance = 3):
 * SOUNDREC is six services away and four of the remaining guests share most of
 * them -- the menu family in particular is wanted by CARDFILE, PACKAGER and
 * WINFILE as well, so these are chosen for OVERLAP rather than for one guest.
 */
#define WOWUSER_DRAWICON                    0x0054
#define WOWUSER_DI2_ARG_HICON               0
#define WOWUSER_DI2_ARG_Y                   2
#define WOWUSER_DI2_ARG_X                   4
#define WOWUSER_DI2_ARG_HDC                 6

#define WOWUSER_GETCLIPBOARDOWNER           0x008c
#define WOWUSER_GETDOUBLECLICKTIME          0x0015
#define WOWUSER_CREATEPOPUPMENU             0x019f
#define WOWUSER_DESTROYMENU                 0x0098
#define WOWUSER_DESTROYICON                 0x01c9
#define WOWUSER_GETMENUITEMCOUNT            0x0107
#define WOWUSER_GETTOPWINDOW                0x00e5

/* ModifyMenu and InsertMenu share a signature exactly, so they share a case;
 * the id decides which of the two the OS is asked for.
 */
#define WOWUSER_MODIFYMENU                  0x019e
#define WOWUSER_INSERTMENU                  0x019a
#define WOWUSER_MI2_ARG_ITEM                0
#define WOWUSER_MI2_ARG_IDNEW               4
#define WOWUSER_MI2_ARG_FLAGS               6
#define WOWUSER_MI2_ARG_POS                 8
#define WOWUSER_MI2_ARG_HMENU               10

#define WOWUSER_GETMENUITEMID               0x0108
#define WOWUSER_GMII_ARG_POS                0
#define WOWUSER_GMII_ARG_HMENU              2

#define WOWUSER_GETMENUSTATE                0x00fa
#define WOWUSER_GMS_ARG_FLAGS               0
#define WOWUSER_GMS_ARG_ID                  2
#define WOWUSER_GMS_ARG_HMENU               4

#define WOWUSER_GETMENUSTRING               0x00a1
#define WOWUSER_GMSTR_ARG_FLAGS             0
#define WOWUSER_GMSTR_ARG_MAX               2
#define WOWUSER_GMSTR_ARG_BUF               4
#define WOWUSER_GMSTR_ARG_ID                8
#define WOWUSER_GMSTR_ARG_HMENU             10

#define WOWUSER_SCROLLWINDOW                0x003d
#define WOWUSER_SW2_ARG_CLIP                0
#define WOWUSER_SW2_ARG_RECT                4
#define WOWUSER_SW2_ARG_DY                  8
#define WOWUSER_SW2_ARG_DX                  10
#define WOWUSER_SW2_ARG_HWND                12

#define WOWUSER_GETSCROLLRANGE              0x0041
#define WOWUSER_GSR_ARG_MAX                 0
#define WOWUSER_GSR_ARG_MIN                 4
#define WOWUSER_GSR_ARG_BAR                 8
#define WOWUSER_GSR_ARG_HWND                10

#define WOWUSER_SHOWSCROLLBAR               0x010b

/* #300: EnableScrollBar(hWnd, wSBflags, wArrows) -- 6 arg bytes, Pascal: wArrows@0,
 * wSBflags@2, hWnd@4. Stepped over 8 times in s89's logs (USER reaches it from its
 * own code). Same constants in Win16 and Win32 (SB_*, ESB_*).
 */
#define WOWUSER_ENABLESCROLLBAR             0x01e2
#define WOWUSER_SSB_ARG_SHOW                0
#define WOWUSER_SSB_ARG_BAR                 2
#define WOWUSER_SSB_ARG_HWND                4

#define WOWUSER_OPENCOMM                    0x00c8

/* Pascal order: base = the LAST argument pushed. OpenComm(dev, cbIn, cbOut). */
#define WOWUSER_OC_ARG_DEV                  4
#define WOWUSER_CC_ARG_ID                   0
#define WOWUSER_RC_ARG_CB                   0
#define WOWUSER_RC_ARG_BUF                  2
#define WOWUSER_RC_ARG_ID                   6
#define WOWUSER_TC_ARG_CH                   0
#define WOWUSER_TC_ARG_ID                   2
#define WOWUSER_GCE_ARG_STAT                0
#define WOWUSER_GCE_ARG_ID                  4
#define WOWUSER_ECF_ARG_FN                  0
#define WOWUSER_ECF_ARG_ID                  2
#define WOWUSER_CLOSECOMM                   0x00cf
#define WOWUSER_TRANSMITCHAR                0x00ce
#define WOWUSER_SETCOMMSTATE                0x00c9
#define WOWUSER_GETCOMMSTATE                0x00ca
#define WOWUSER_GETCOMMERROR                0x00cb
#define WOWUSER_WRITECOMM                   0x00cd
#define WOWUSER_FLUSHCOMM                   0x00d7

/* The rest of the Win16 comm surface the shelf asks for -- TERMINAL.EXE wants
 * all five. They join the honest refusal below rather than getting stubs.
 *
 * [CAUTION]: SetCommEventMask IS THE ODD ONE: it returns a FAR POINTER to the event
 * word, not a status, so IE_BADID would be a pointer to 0xFFFE. Its failure
 * value is a null pointer, and it is answered separately for that reason.
 */
#define WOWUSER_READCOMM                    0x00cc
#define WOWUSER_SETCOMMEVTMASK              0x00d0
#define WOWUSER_SETCOMMBREAK                0x00d2
#define WOWUSER_CLEARCOMMBREAK              0x00d3
#define WOWUSER_ESCAPECOMMFN                0x00d6

#define WOWUSER_SETWINDOWPLACEMENT          0x0173
#define WOWUSER_EXITWINDOWS                 0x0007
#define WOWUSER_DEFFRAMEPROC                0x01bd
#define WOWUSER_DFP_ARG_LPARAM              0
#define WOWUSER_DFP_ARG_WPARAM              4
#define WOWUSER_DFP_ARG_MSG                 6
#define WOWUSER_DFP_ARG_HCLIENT             8
#define WOWUSER_DFP_ARG_HWND                10
#define WOWUSER_DEFMDICHILDPROC             0x01bf

#define WOWUSER_TABBEDTEXTOUT               0x00c4
#define WOWUSER_TTO_ARG_TABORG              0
#define WOWUSER_TTO_ARG_TABPOS              2
#define WOWUSER_TTO_ARG_TABCNT              6
#define WOWUSER_TTO_ARG_COUNT               8
#define WOWUSER_TTO_ARG_STR                 10
#define WOWUSER_TTO_ARG_Y                   14
#define WOWUSER_TTO_ARG_X                   16
#define WOWUSER_TTO_ARG_HDC                 18

#define WOWUSER_ISWINDOWVISIBLE             0x0031
#define WOWUSER_IW_ARG_HWND                 0

/* 0x12 SetCapture / 0x13 ReleaseCapture -- WHAT MAKES A DRAG A STROKE.
 * From `neneeds.py`'s list for PBRUSH (ord 18 and 19, 2 and 0 argument bytes).
 * A paint program takes the capture on button-down so that the rest of the
 * stroke arrives even when the pointer leaves the canvas, and gives it back on
 * button-up. Without it a stroke ends at the window edge -- or, worse, the
 * button-up is delivered to whatever window the pointer happens to be over and
 * the guest never learns the stroke finished, so it keeps drawing.
 *
 * [INFO]: The real Win32 capture is the right mechanism, exactly as the real menu and
 * the real caption are: these windows ARE Win32 windows.
 *
 * [CAUTION]: SetCapture RETURNS THE PREVIOUS CAPTURE WINDOW, and it has to come back as a
 * 16-bit handle -- a guest that restores it would otherwise be handed a
 * truncated HWND. A previous window that is not one of ours answers 0, which
 * is what "nobody had it" looks like to the guest.
 */
#define WOWUSER_SETCAPTURE                  0x0012
#define WOWUSER_RELEASECAPTURE              0x0013
#define WOWUSER_CAP_ARG_HWND                0

/* THE REST OF THE DRAWING PATH (Importance = 4):
 * Named from the run in which the mouse first reached MS Paint. Paint answers
 * a button-down on its canvas with `SetCapture`, `GetDC`, `CreateSolidBrush`,
 * `GetClientRect` -- all of which worked -- and then a loop that was stepped
 * over 21 times per stroke. Their ids track USER's ordinals, and the
 * neighbours prove the mapping rather than assuming it: ord 30 -> 0x1e, ord 31
 * -> 0x1f, and ord 33 GETCLIENTRECT -> 0x21, which is the id this file already
 * implements.
 *   ord 16 CLIPCURSOR      id 0x10   4 args  (LPRECT)
 *   ord 28 CLIENTTOSCREEN  id 0x1c   6 args  (HWND, LPPOINT)
 *   ord 32 GETWINDOWRECT   id 0x20   6 args  (HWND, LPRECT)
 *   ord 60 GETACTIVEWINDOW id 0x3c   0 args  ()
 *
 * [CAUTION]: A Win16 POINT is two `int`s = 4 bytes and a RECT is four = 8, against
 * Win32's 8 and 16. Same conversion as everywhere else in this file.
 *
 * [CAUTION]: CLIPCURSOR IS DELIBERATELY NOT APPLIED, and this is a stated deviation
 * rather than an oversight. Paint uses it to pen the pointer inside its canvas
 * for the duration of a stroke, which is a nicety it does not need to draw
 * correctly -- but `ClipCursor` is SYSTEM-WIDE, and this host is a VDM the
 * harness kills with `taskkill` several times a session. A guest that is
 * terminated mid-stroke while holding a clip would leave the user's real
 * pointer confined to a rectangle on their own desktop. The call is accepted
 * and logged; a later session that wants it can apply it and release it on
 * WM_KILLFOCUS, capture loss and task exit.
 */
#define WOWUSER_CLIPCURSOR                  0x0010
#define WOWUSER_CC_ARG_RECT                 0
#define WOWUSER_CLIENTTOSCREEN              0x001c
#define WOWUSER_C2S_ARG_POINT               0
#define WOWUSER_C2S_ARG_HWND                4
#define WOWUSER_GETWINDOWRECT               0x0020
#define WOWUSER_GWR_ARG_RECT                0
#define WOWUSER_GWR_ARG_HWND                4
#define WOWUSER_GETACTIVEWINDOW             0x003c

/* [INFO]: 0x51 FillRect(hDC, lprc, hbr) -- USER ordinal 81, 8 argument bytes
 * (2 + 4 + 2), and `neimports.py` names the call site in PBRUSH.EXE outright.
 * The run confirms the order: `(2020 | 6ed6 09c7 | 20c0)` is a stock-object
 * BRUSH token at +0, a far RECT at +2 and one of our DC tokens at +6.
 */
/* THE SECOND USER SWEEP (session 47) (Importance = 5):
 * With `neneeds.py` able to see through USER's validating export wrappers, the
 * surface these two programs actually reach went from 52 to 92 calls, and
 * everything below is on that list with its id and argument count. The
 * constants are the ones the GUEST passes at run time, not out of a header:
 *
 *   SetClassWord  index -12 = GCW_HCURSOR, and the value is
 *                 LoadCursor(0, 0x7f00) = IDC_ARROW. **This is how MS Paint
 *                 changes its pointer per tool** -- eighteen calls a run,
 *                 every one of them stepped over until now.
 *   GetWindowLong index -16 = GWL_STYLE, read and written back through
 *                 SetWindowLong.
 *   GetKeyState   Paint tests the HIGH BIT, i.e. "is the key down now", which
 *                 is Win32's convention unchanged.
 *
 * [CAUTION]: Win16's negative indices are the same numbers as Win32's for the fields that
 * exist in both, which is what the two observations above confirm --
 * but only for those two. Anything else is refused by name rather than passed
 * through on the strength of a pattern.
 */
#define WOWUSER_DEFWINDOWPROC               0x006b
#define WOWUSER_DWP_ARG_LPARAM              0       /* DWORD */
#define WOWUSER_DWP_ARG_WPARAM              4
#define WOWUSER_DWP_ARG_MSG                 6
#define WOWUSER_DWP_ARG_HWND                8

#define WOWUSER_SETCLASSWORD                0x0082
#define WOWUSER_SCW_ARG_VALUE               0
#define WOWUSER_SCW_ARG_INDEX               2
#define WOWUSER_SCW_ARG_HWND                4

#define WOWUSER_GETWINDOWLONG               0x0087
#define WOWUSER_GWL_ARG_INDEX               0
#define WOWUSER_GWL_ARG_HWND                2
#define WOWUSER_SETWINDOWLONG               0x0088
#define WOWUSER_SWL_ARG_VALUE               0       /* DWORD */
#define WOWUSER_SWL_ARG_INDEX               4
#define WOWUSER_SWL_ARG_HWND                6

#define WOWUSER_GETKEYSTATE                 0x006a
#define WOWUSER_GETSYSCOLOR                 0x00b4
#define WOWUSER_GETMESSAGEPOS               0x0077
#define WOWUSER_GETMSGEXTRAINFO             0x0120
#define WOWUSER_GETDESKTOPWINDOW            0x011e
#define WOWUSER_BRINGWINDOWTOTOP            0x002d
#define WOWUSER_DRAWMENUBAR                 0x00a0
#define WOWUSER_SHOWCURSOR                  0x0047
#define WOWUSER_GETCURSORPOS                0x0011  /* 4 args, far LPPOINT */
#define WOWUSER_SETCURSORPOS                0x0046  /* 4 args (x, y) */
#define WOWUSER_SCREENTOCLIENT              0x001d  /* 6 args */
#define WOWUSER_STC_ARG_POINT               0
#define WOWUSER_STC_ARG_HWND                4

/* [CAUTION]: `INVR_`, NOT `IR_` -- AND THAT RENAME IS A BUG FIX, NOT TIDYING.
 * InvalidateRect above already owns `WOWUSER_IR_ARG_RECT` and sets it to **2**. This
 * block used to redefine it to **0**, and because the preprocessor takes the
 * last definition before the use, BOTH handlers read offset 0 -- so every
 * InvalidateRect in this host has been fetching its `lpRect` out of `bErase`,
 * getting a junk far pointer, and falling into the NULL path that invalidates
 * THE WHOLE CLIENT AREA. It never looked broken because over-invalidating
 * still repaints correctly; it just repaints everything, every time.
 *
 * [INFO]: The only evidence was a `warning: 'WOWUSER_IR_ARG_RECT' redefined` that had been in
 * the build output all along. A prefix here is a namespace, and a collision in
 * it is a silent wrong answer -- exactly the class this project treats as most
 * expensive. (found while adding SetMenu, which collided the same way)
 */
#define WOWUSER_INVERTRECT                  0x0052  /* 6 args (hDC, lpRect) */
#define WOWUSER_INVR_ARG_RECT               0
#define WOWUSER_INVR_ARG_HDC                4

#define WOWUSER_GLOBALADDATOM               0x010c  /* 4 args, far LPCSTR */
#define WOWUSER_GLOBALDELATOM               0x010d  /* 2 args */

#define WOWUSER_SELECTPALETTE               0x011a  /* 6 args (hDC, hPal, bForce) */
#define WOWUSER_SPL_ARG_FORCE               0
#define WOWUSER_SPL_ARG_PAL                 2
#define WOWUSER_SPL_ARG_HDC                 4
#define WOWUSER_REALIZEPALETTE              0x011b  /* 2 args (hDC) */

/* The caret -- five calls that stand or fall together, which is why they are one
 * group here. MS Paint's Text tool needs all of them.
 */
#define WOWUSER_CREATECARET                 0x00a3  /* 8 args */
#define WOWUSER_CC_ARG_HEIGHT               0
#define WOWUSER_CC_ARG_WIDTH                2
#define WOWUSER_CC_ARG_BITMAP               4
#define WOWUSER_CC_ARG_HWND                 6
#define WOWUSER_DESTROYCARET                0x00a4  /* 0 args */
#define WOWUSER_SETCARETPOS                 0x00a5  /* 4 args */
#define WOWUSER_HIDECARET                   0x00a6  /* 2 args */
#define WOWUSER_SHOWCARET                   0x00a7  /* 2 args */

#define WOWUSER_SETWINDOWPOS                0x00e8  /* 14 args */
#define WOWUSER_SWP_ARG_FLAGS               0
#define WOWUSER_SWP_ARG_CY                  2
#define WOWUSER_SWP_ARG_CX                  4
#define WOWUSER_SWP_ARG_Y                   6
#define WOWUSER_SWP_ARG_X                   8
#define WOWUSER_SWP_ARG_AFTER               10
#define WOWUSER_SWP_ARG_HWND                12

#define WOWUSER_GETSCROLLPOS                0x003f  /* 4 args (hWnd, nBar) */
#define WOWUSER_GSP_ARG_BAR                 0
#define WOWUSER_GSP_ARG_HWND                2

/* THE OLE CLUSTER -- WHAT `File > Save As` DIES ON NOW. (session 49):
 * With the LDT collision fixed, MS Paint's save runs, reads its whole canvas
 * with `GetDIBits`, and then faults inside **OLESVR.DLL** -- because Paint
 * registers itself as an OLE server and OLESVR notifies its clients that the
 * document changed. The host's fault frame gives the reason: ES = 0x0000 {NO
 * DESCRIPTOR}, BX = 0, i.e. a NULL far pointer dereferenced without a check.
 * Three lines of log before it say where the null came from:
 *
 *   FUNC=0x2e from=OLESVR (0x0200) -> UNIMPLEMENTED, answered 0
 *   FUNC=0x35 ... DestroyWindow 0x0200 -> destroyed
 *   FUNC=0x87 from=OLESVR (0,0) -> GetWindowLong(0x0000,0)
 *                                  [INFO]: NOT ONE OF OUR WINDOWS; 0
 *
 * `USER.46 GetParent` was stepped over, so OLESVR asked window 0 for its
 * window long, got 0, and dereferenced it. => **`GetParent` is the whole bug.**
 *
 * [INFO]: AND THE ANSWER IS KNOWN TO BE RIGHT BEFORE THE RUN. Window `0x0200` is
 * `CreateWindow("DocWndClass","Doc", style=0x40000000 = WS_CHILD)` and its
 * argument block carries `hwndParent = 0x0140` -- and thirty lines earlier the
 * log has `SetWindowLong(0x0140, 0000, 0x09b70000)`, which is OLESVR storing
 * its server object on exactly that window. So `GetParent(0x200) -> 0x140`
 * makes `GetWindowLong(0x140,0)` hand back the pointer it stored itself.
 *
 * [CAUTION]: The rest of this cluster is everything else OLESVR reaches that is still
 * stepped over, because a null from any of them lands the same way.
 */
#define WOWUSER_GETPARENT                   0x002e  /* Ord 46, 2 args THE FIX */
#define WOWUSER_GETWINDOW                   0x0106  /* Ord 262, 4 args */
#define WOWUSER_GW_ARG_CMD                  0
#define WOWUSER_GW_ARG_HWND                 2
#define WOWUSER_GETCLASSNAME                0x003a  /* Ord 58, 8 args */
#define WOWUSER_GCN_ARG_MAX                 0
#define WOWUSER_GCN_ARG_BUF                 2       /* Far */
#define WOWUSER_GCN_ARG_HWND                6
#define WOWUSER_GETWINDOWTASK               0x00e0  /* Ord 224, 2 args */

/* The window property list. [CAUTION] A property NAME is either a far string or an ATOM
 * in the low word of a far pointer whose SELECTOR IS ZERO (MAKEINTATOM) -- so a
 * lookup that only understands strings finds nothing and a store that only
 * understands strings keeps nothing, which is the same null-pointer ending. Both
 * forms are canonicalised to one key here, atoms as "#nnnn".
 */
#define WOWUSER_REMOVEPROP                  0x0018  /* Ord 24, 6 args */
#define WOWUSER_GETPROP                     0x0019  /* Ord 25, 6 args */
#define WOWUSER_SETPROP                     0x001a  /* Ord 26, 8 args */
#define WOWUSER_PROP_ARG_NAME_G             0       /* Get/Remove: far name @0, hWnd @4 */
#define WOWUSER_PROP_ARG_HWND_G             4
#define WOWUSER_PROP_ARG_DATA_S             0       /* Set: data @0, far name @2, hWnd @6 */
#define WOWUSER_PROP_ARG_NAME_S             2
#define WOWUSER_PROP_ARG_HWND_S             6

#define WOWUSER_GLOBALFINDATOM              0x010e  /* Ord 270, 4 args */
#define WOWUSER_GLOBALATOMNAME              0x010f  /* Ord 271, 8 args */
#define WOWUSER_GAN_ARG_SIZE                0
#define WOWUSER_GAN_ARG_BUF                 2       /* Far */
#define WOWUSER_GAN_ARG_ATOM                6
#define WOWUSER_HOOK_ARGUMENTS              4       /* nCode, wParam, lParam high, low */
#define WOWUSER_HOOK_ARG_LPARAM             2       /* lParam's high word: the EVENTMSG's pointer */

#define WOWUSER_FILLRECT                    0x0051
#define WOWUSER_FR_ARG_BRUSH                0
#define WOWUSER_FR_ARG_RECT                 2
#define WOWUSER_FR_ARG_HDC                  6

#define WOWUSER_SETSCROLLPOS                0x003e
#define WOWUSER_SSP_ARG_REDRAW              0
#define WOWUSER_SSP_ARG_POS                 2
#define WOWUSER_SSP_ARG_BAR                 4
#define WOWUSER_SSP_ARG_HWND                6

#define WOWUSER_SETSCROLLRANGE              0x0040
#define WOWUSER_SSR_ARG_REDRAW              0
#define WOWUSER_SSR_ARG_MAX                 2
#define WOWUSER_SSR_ARG_MIN                 4
#define WOWUSER_SSR_ARG_BAR                 6
#define WOWUSER_SSR_ARG_HWND                8

#define WOWUSER_SETCURSOR                   0x0045
#define WOWUSER_SC_ARG_HCURSOR              0

/* 0x27 BeginPaint / 0x28 EndPaint -- WHERE A GUEST DRAWS (Importance = 5):
 * Both are internal stubs (their exports are native16), so the ids came from
 * the run that first relayed WM_PAINT to a guest -- see wowwin.h. Each carries
 * 6 argument bytes and the same block: a far `lpPaint` at +0 and the hWnd at
 * +4, and the hWnd that arrived was 0x0180, MS Paint's CANVAS window.
 *
 * [INFO]: THE PAINTSTRUCT LAYOUT -- the documented Win16 one: hdc at +0, fErase at
 * +2, rcPaint at +4 as four `int`s. Checked on Paint: the HDC its GDI calls
 * carry after BeginPaint is the word we wrote at +0.
 *   Win16's PAINTSTRUCT is 32 bytes (16 of them a reserved tail); Win32's is
 *   64 with LONGs, so this is a conversion like every other structure here.
 *
 * [CAUTION]: THE UPDATE REGION HAS ALREADY BEEN CONSUMED by the time the guest gets
 * here -- WowWinProc had to validate it to stop Win32 re-synthesising
 * WM_PAINT forever. So the rectangle comes out of the pending-paint record
 * that kept it. A window with no record still gets a DC and its whole client
 * rectangle, which is correct-but-wasteful rather than wrong.
 */
#define WOWUSER_BEGINPAINT                  0x0027
#define WOWUSER_ENDPAINT                    0x0028
#define WOWUSER_BP_ARG_PS                   0
#define WOWUSER_BP_ARG_HWND                 4

#define WOWUSER_GETDC                       0x0042
#define WOWUSER_GETWINDOWDC                 0x0043
#define WOWUSER_GDC_ARG_HWND16              0
#define WOWUSER_RELEASEDC                   0x0044
#define WOWUSER_RDC_ARG_HDC                 0
#define WOWUSER_RDC_ARG_HWND                2

/* [INFO]: USER's DC calls MINT A GDI TOKEN out of the map in wowgdi.h, which main.c
 * now includes BEFORE this file for exactly that reason.
 */

/* MENUS AND DIALOG ITEMS -- THE LAST TWO CLUSTERS (Importance = 3):
 * Both from `neneeds.py`'s list, and both blocked on the same missing piece: a
 * 16-bit name for an `HMENU`. A Win32 menu handle is 32 bits and a Win16
 * program has 16 to hold it in, so the same answer as everywhere else in this
 * file -- a TOKEN the guest can carry, with the real object behind it.
 *
 * [INFO]: AND IT HAS TO BE A TOKEN, NOT A TRUNCATION, BECAUSE THE HANDLE GOES BACK
 * THROUGH 16-BIT CODE. `USER.154 CHECKMENUITEM` and `USER.155 ENABLEMENUITEM`
 * are `native16` -- USER implements them in its own code, which will hand the
 * handle to a stub of its own later. So whatever `GetMenu` returns must
 * survive a round trip through USER and still name the right menu when it
 * comes back. A truncated pointer would not, and would not fail loudly either.
 *
 * 0x9c  GETSYSTEMMENU   4 bytes  (hWnd, bRevert)
 * 0x9d  GETMENU         2 bytes  (hWnd)
 * 0x9f  GETSUBMENU      4 bytes  (hMenu, nPos)
 * 0x58  ENDDIALOG       4 bytes  (hDlg, nResult)
 * 0x5b  GETDLGITEM      4 bytes  (hDlg, nIDDlgItem)
 * 0x5d  GETDLGITEMTEXT 10 bytes  (hDlg, nID, lpString, nMaxCount)  2+2+4+2
 * 0x5e  SETDLGITEMINT   8 bytes  (hDlg, nID, wValue, bSigned)      2+2+2+2
 * 0x65  SENDDLGITEMMSG 12 bytes  (hDlg, nID, wMsg, wParam, lParam) 2+2+2+2+4
 * Every one adds up to the byte count its own stub declares.
 */
/* [INFO]: AND THE TWO THAT ACTUALLY CHANGE A MENU, found the way MessageBox was:
 * `USER.154 CHECKMENUITEM` and `USER.155 ENABLEMENUITEM` are `native16`, so
 * neneeds.py cannot see them -- but they are WRAPPERS that reach stubs of
 * their own, and the run named both the moment WM_INITMENUPOPUP started
 * arriving: `0x9a` and `0x9b`, 6 argument bytes each, six calls in one opening
 * of Notepad's Edit menu. That is the THIRD time today a native16 wrapper has
 * turned out to be real work (MessageBox and LoadIcon are the others), which
 * is exactly the limit the tool documents.
 * (hMenu, wID, wFlags) = 2+2+2 = 6, reversed as always.
 */
#define WOWUSER_CHECKMENUITEM               0x009a
#define WOWUSER_ENABLEMENUITEM              0x009b
#define WOWUSER_MI_ARG_FLAGS                0
#define WOWUSER_MI_ARG_ID                   2
#define WOWUSER_MI_ARG_HMENU                4

#define WOWUSER_GETSYSTEMMENU               0x009c
#define WOWUSER_GSYM_ARG_REVERT             0
#define WOWUSER_GSYM_ARG_HWND               2
#define WOWUSER_GETMENU                     0x009d
#define WOWUSER_GM2_ARG_HWND                0
#define WOWUSER_GETSUBMENU                  0x009f
#define WOWUSER_GSM2_ARG_POS                0
#define WOWUSER_GSM2_ARG_HMENU              2

#define WOWUSER_ENDDIALOG                   0x0058
#define WOWUSER_ED_ARG_RESULT               0
#define WOWUSER_ED_ARG_HDLG                 2
#define WOWUSER_GETDLGITEM                  0x005b
#define WOWUSER_GDI2_ARG_ID                 0
#define WOWUSER_GDI2_ARG_HDLG               2
#define WOWUSER_GETDLGITEMTEXT              0x005d
#define WOWUSER_GDIT_ARG_MAX                0
#define WOWUSER_GDIT_ARG_BUF                2
#define WOWUSER_GDIT_ARG_ID                 6
#define WOWUSER_GDIT_ARG_HDLG               8
#define WOWUSER_SETDLGITEMINT               0x005e
#define WOWUSER_SDII_ARG_SIGNED             0
#define WOWUSER_SDII_ARG_VALUE              2
#define WOWUSER_SDII_ARG_ID                 4
#define WOWUSER_SDII_ARG_HDLG               6
#define WOWUSER_SENDDLGITEMMSG              0x0065
#define WOWUSER_SDIM_ARG_LPARAM             0
#define WOWUSER_SDIM_ARG_WPARAM             4
#define WOWUSER_SDIM_ARG_MSG                6
#define WOWUSER_SDIM_ARG_ID                 8
#define WOWUSER_SDIM_ARG_HDLG               10

/* BOOL ScrollDC(HDC, int dx, int dy, LPRECT scroll, LPRECT clip,
 *                 HRGN update, LPRECT lprcUpdate) = 20 --------------------
 * Win32 has the same call with the same seven arguments and the same meaning.
 * What is NOT the same is the RECTANGLES: a Win16 RECT is 8 bytes and Win32's
 * is 16 (see wowconv.h), so each one is read field by field and rebuilt, and
 * the update rectangle is written back the same way. Passing a guest's RECT
 * straight to Win32 would read this rectangle plus eight bytes of whatever
 * follows it.
 */
#define WOWUSER_SCROLLDC                    0x00dd
#define WOWUSER_SCRDC_ARG_LPRCUPDATE        0       /* Far */
#define WOWUSER_SCRDC_ARG_HRGNUPDATE        4
#define WOWUSER_SCRDC_ARG_LPRCCLIP          6       /* Far */
#define WOWUSER_SCRDC_ARG_LPRCSCROLL        10      /* Far */
#define WOWUSER_SCRDC_ARG_DY                14
#define WOWUSER_SCRDC_ARG_DX                16
#define WOWUSER_SCRDC_ARG_HDC               18

/* 0x1ce CalcChildScroll(HWND hwnd, WORD wScroll) = 4 (Importance = 1):
 * An undocumented USER internal that PROGMAN calls: "recompute the scroll bars
 * of this MDI client". [CAUTION] THE HONEST ANSWER HERE IS THAT THE OS ALREADY DID IT.
 * Our MDICLIENT is the REAL Win32 system class (see WowUserEnsureSystemClasses),
 * so its scroll bars are managed by Win32's own MDI client, not by anything
 * this host keeps -- there is no state of ours to recalculate. That is a
 * statement about our architecture, not a stub, and the log says it every
 * time so a guest that visibly disagrees is a line to grep for.
 */
#define WOWUSER_CALCCHILDSCROLL             0x01ce
#define WOWUSER_CCS_ARG_SCROLL              0
#define WOWUSER_CCS_ARG_HWND                2

/* THE ENUMERATIONS. (session 57) One callback per item; the mechanism is
 * in src/wow/wowenum.h and the argument blocks are reversed as always.
 * BOOL EnumWindows(FARPROC lpEnumFunc, LPARAM lParam)              = 8
 * BOOL EnumChildWindows(HWND hParent, FARPROC, LPARAM)             = 10
 * BOOL EnumTaskWindows(HTASK hTask, FARPROC, LPARAM)               = 10
 *
 * [CAUTION]: THE CALLBACK'S SIGNATURE IS (HWND, LPARAM) -- three words -- and its answer
 * is a veto: 0 ends the enumeration and the function returns FALSE.
 */
#define WOWUSER_ENUMWINDOWS                 0x0036
#define WOWUSER_EW_ARG_LPARAM               0       /* DWORD */
#define WOWUSER_EW_ARG_PROC                 4       /* Far */
#define WOWUSER_ENUMCHILDWINDOWS            0x0037
#define WOWUSER_ECW_ARG_LPARAM              0
#define WOWUSER_ECW_ARG_PROC                4
#define WOWUSER_ECW_ARG_PARENT              8
#define WOWUSER_ENUMTASKWINDOWS             0x00e1
#define WOWUSER_ETW_ARG_LPARAM              0
#define WOWUSER_ETW_ARG_PROC                4
#define WOWUSER_ETW_ARG_TASK                8

/* ShowWindow(hWnd, nCmdShow) -- 4 bytes, reversed as always, and confirmed by the
 * run: SYSEDIT's two calls carry `(0x0005 0x0160)` (the MDI client) and
 * `(0x0001 0x0140)` (the frame). UpdateWindow(hWnd) -- 2 bytes.
 */
#define WOWUSER_SW_ARG_CMDSHOW              0
#define WOWUSER_SW_ARG_HWND                 2
#define WOWUSER_UW_ARG_HWND                 0

/* Get/SetWindowWord argument blocks, reversed as always (base = last push):
 *   GetWindowWord(hWnd, nIndex)               -> +0 nIndex, +2 hWnd
 *   SetWindowWord(hWnd, nIndex, wNewWord)     -> +0 value, +2 nIndex, +4 hWnd
 * Confirmed against the run: SYSEDIT's `mpchild` WM_CREATE calls
 * SetWindowWord(hwnd, 2, 0) and the frame carries `(0x0000 0x0002 <hwnd>)`.
 */
#define WOWUSER_GWW_ARG_INDEX               0
#define WOWUSER_GWW_ARG_HWND                2
#define WOWUSER_SWW_ARG_VALUE               0
#define WOWUSER_SWW_ARG_INDEX               2
#define WOWUSER_SWW_ARG_HWND                4

/* SendMessage(hWnd, msg, wParam, lParam) -- 10 argument bytes (Importance = 3):
 * `USER.111 SENDMESSAGE`, named from SYSEDIT's NE import relocations. The
 * block is REVERSED from the parameter list as always (the base is the LAST
 * push), and the run confirms every offset in one line: SYSEDIT's
 * SendMessage(hwndMDIClient, WM_MDICREATE, 0, &stackStruct) arrives as
 * `args=(0x2378 0x0a9f 0x0000 0x0220 0x0160)`.
 */
#define WOWUSER_SM_ARG_LPARAM               0
#define WOWUSER_SM_ARG_WPARAM               4
#define WOWUSER_SM_ARG_MSG                  6
#define WOWUSER_SM_ARG_HWND                 8

/* The messages this host names. `0x0220` is what SYSEDIT sends its MDI client,
 * and its lParam is an MDICREATESTRUCT -- see below.
 */
#define WM_MDICREATE16                      0x0220

/* AN EDIT CONTROL'S TEXT IS A HANDLE IN THE APPLICATION'S OWN HEAP (Importance = 4):
 * `0x040C` and `0x040D` are `WM_USER + 12` and `WM_USER + 13`, and which is
 * which is settled by what SYSEDIT does with them when it loads a file
 * (observed call sequence):
 *
 * OpenFile, _llseek to the end (the file's SIZE), _llseek back to the start
 * SendMessage(hEdit, 0x40D, 0, 0)           ; so 0x40D takes NO parameters...
 * LocalReAlloc(<that answer>, size + 1, 0x42) ; ...and RETURNS A LOCAL HANDLE
 * LocalLock, _lread the file straight in, NUL-terminate it, LocalUnlock
 * SendMessage(hEdit, 0x40C, hMem, 0)        ; 0x40C TAKES the handle
 *
 * `0x040D` is **EM_GETHANDLE** and `0x040C` is **EM_SETHANDLE** (as documented),
 * and the run that stopped here was stopping on the FIRST of the pair.
 *
 * [INFO]: AND THE HANDLE MUST BE VALID IN THE APPLICATION'S OWN LOCAL HEAP. That is
 * not an assumption about how Windows implements edit controls -- it is what
 * this program demonstrably requires: it hands the answer straight to
 * `LocalReAlloc` and `LocalLock`, which operate on the local heap of the
 * CURRENT DS, and DS throughout is SYSEDIT's own DGROUP. A handle this host
 * invented would be a number `LocalReAlloc` rejects, and rejecting it is
 * exactly what produced *"Cannot open this file."*
 *
 * The host cannot make this handle. The GUEST'S KERNEL has to, and since
 * session 40 the host can ask it: `KERNEL.5 LOCALALLOC` is, in krnl386.exe's
 * NE entry table (documented format), `FIXED, segment 1, offset 0x3ddb`, and
 * segment 1 is the code segment every WOW32 BOP arrives from -- so its runtime
 * address is `<the BOP's CS>:0x3ddb`, with no resolution machinery at all.
 * Called with the documented LocalAlloc(flags, cb) -- two words, far.
 */
#define EM_SETHANDLE16                      0x040C
#define EM_GETHANDLE16                      0x040D
#define WOWUSER_LOCALALLOC_ARGUMENTS        2       /* Flags, cb */
#define WOWUSER_LOCALREALLOC_ARGUMENTS      3       /* hMem, cbNew, flags */
#define WOWUSER_NOTIFY_ARG_BLOCK            0
#define WOWUSER_NOTIFY_ARG_KIND             4

/* CreateWindow's ARGUMENT BLOCK, CHECKED AGAINST WOWEXEC'S CALL (Importance = 3):
 * The arg block grows the opposite way from the pushes, so "lpClassName is the
 * first parameter" says nothing about where it lands. The block base is the
 * LOWEST address, which holds the LAST word pushed -- so the parameter list is
 * reversed, and a DWORD's high word is at the LOWER offset because Pascal pushes
 * it first. `USER.41 CREATEWINDOW` arrives with exactly 30 argument bytes.
 *
 * +26/+28 lpClassName (far)  |  +14 y
 * +22/+24 lpWindowName (far) |  +12 nWidth
 * +18/+20 dwStyle            |  +10 nHeight
 * +16 x                      |  +8  hWndParent
 *                            |  +6  hMenu
 *                            |  +4  hInstance
 *                            |  +0/+2 lpParam
 *
 * [INFO]: AND THE DATA WOWEXEC's CALL CARRIES CROSS-VALIDATES THE LAYOUT (as logged):
 *   +26/+28  the string "WOWExecClass" -- the class WOWEXEC has just
 *            registered (a second copy of the literal from the one in its
 *            WNDCLASS; both decode to the same name).
 *   +18/+20  0x02CF0000 = WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN. Read the other
 *            way round it would be 0x000002CF, which is not a window style.
 *   +10..+16 four copies of 0x8000 = CW_USEDEFAULT, exactly where x/y/w/h are.
 *   +4       the SAME word WOWEXEC put into WNDCLASS.hInstance.
 * Four independent agreements. A wrong offset assignment produces none of them.
 */
#define WOWUSER_CW_ARG_LPPARAM              0
#define WOWUSER_CW_ARG_HINSTANCE            4
#define WOWUSER_CW_ARG_HMENU                6
#define WOWUSER_CW_ARG_HWNDPARENT           8
#define WOWUSER_CW_ARG_HEIGHT               10
#define WOWUSER_CW_ARG_WIDTH                12
#define WOWUSER_CW_ARG_Y                    14
#define WOWUSER_CW_ARG_X                    16
#define WOWUSER_CW_ARG_STYLE                18
#define WOWUSER_CW_ARG_WINDOWNAME           22
#define WOWUSER_CW_ARG_CLASSNAME            26

/* 0x1c4 CreateWindowEx -- AND ITS BLOCK IS CreateWindow's WITH ONE FIELD
 * ON THE END. (session 55) WINFILE.EXE imports it and creates no window at
 * all without it.
 * CreateWindowEx pushes dwExStyle FIRST, and the first push sits at the
 * HIGHEST offset -- so every one of the eleven offsets above is unchanged and
 * the extra DWORD lands at +30. 4+4+4+4+2+2+2+2+2+2+2+4 = 34, which is what
 * the stub declares. That is why this shares the CreateWindow body outright
 * rather than getting a copy of it: two copies of a window builder is how two
 * window builders come to disagree.
 */
#define WOWUSER_CREATEWINDOWEX              0x01c4  /* Ord 452, 34 args */
#define WOWUSER_CWX_ARG_EXSTYLE             30
#define WOWDLG_CLASS_BUTTON                 0x80    /* A control's predefined class byte */
#define WOWDLG_CLASS_EDIT                   0x81
#define WOWDLG_CLASS_STATIC                 0x82
#define WOWDLG_CLASS_LISTBOX                0x83
#define WOWDLG_CLASS_SCROLLBAR              0x84
#define WOWDLG_CLASS_COMBOBOX               0x85
#define WOWUSER_CW_ARGUMENT_BYTES           30      /* CreateWindow's arguments, as passed */

/* THE DEFAULT WINDOW PROCEDURE FOR A SYSTEM-CLASS WINDOW (Importance = 4):
 * A window made from `MDICLIENT` has no 16-bit procedure, because under WOW the
 * system classes belong to the 32-bit side -- so when something sends it a
 * message, WE are the procedure. This is that procedure, and it is deliberately
 * tiny: the ONE message any run has ever sent, and 0 for everything else.
 *
 * [CAUTION]: 0 IS NOT A STUB HERE, IT IS THE HONEST ANSWER for a message we do not
 * implement, and the log names the message so the next one can be read off a
 * run rather than guessed at from a list of MDI messages.
 */
/* The three text messages, named because this host now handles them. Win16 and
 * Win32 agree on all three numbers -- Win32 inherited them, the same claim the
 * keyboard messages already travel on.
 */
#define WM_SETTEXT16                        0x000C
#define WM_GETTEXT16                        0x000D
#define WM_GETTEXTLENGTH16                  0x000E

/* #160: the edit commands -- also shared numbers. */
#define WM_CUT16                            0x0300
#define WM_COPY16                           0x0301
#define WM_PASTE16                          0x0302
#define WM_CLEAR16                          0x0303
#define WM_UNDO16                           0x0304

/* USER's internal CreateDialog/DialogBox call: its argument block. */
#define WOWUSER_CD_ARG_MODAL                0       /* 0 = CreateDialog, 1 = DialogBox */
#define WOWUSER_CD_ARG_LENGTH               2       /* The template's length */
#define WOWUSER_CD_ARG_PARAM                6       /* DialogBoxParam's lParam */
#define WOWUSER_CD_ARG_PROC                 10
#define WOWUSER_CD_ARG_PARENT               14
#define WOWUSER_CD_ARG_TEMPLATE             16
#define WOWUSER_CD_ARG_INSTANCE             20
#define WOWUSER_GLOBALALLOC_ARGUMENTS       3       /* Flags, cb high, cb low */
#define WOWUSER_UWH_ARG_PROC                0       /* UnhookWindowsHook(nCode, lpfn) */
#define WOWUSER_UWH_ARG_ID                  4
#define WOWUSER_ESB_ARG_ARROWS              0       /* EnableScrollBar(hwnd, bar, arrows) */
#define WOWUSER_ESB_ARG_BAR                 2
#define WOWUSER_ESB_ARG_HWND                4
#define WOWUSER_SCP_ARG_Y                   0       /* SetCursorPos/SetCaretPos(x, y) */
#define WOWUSER_SCP_ARG_X                   2

/* USER ids krnl386 calls from its own code (see the arms). */
#define WOWUSER_SIGNALPROC                  0x013a
#define WOWUSER_SIGNALPROC_ARG_CODE         6
#define WOWUSER_FINALUSERINIT               0x0190
#define WOWUSER_SYSERRORBOX                 0x0140
#define WOWUSER_SEB_ARG_BUTTON3             0       /* SysErrorBox(text, caption, b1, b2, b3) */
#define WOWUSER_SEB_ARG_BUTTON2             2
#define WOWUSER_SEB_ARG_BUTTON1             4
#define WOWUSER_SEB_ARG_CAPTION             6
#define WOWUSER_SEB_ARG_TEXT                10

#endif /* NTVDMEX_WOWUSER_CALLS_H */
