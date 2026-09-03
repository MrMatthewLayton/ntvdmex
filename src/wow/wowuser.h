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

/* ── ★★★ 0x76 RegisterWindowMessage(lpString) ────────────────────────────────
     Another of the ids USER's export table cannot name, and NOTEPAD names it by
     what it passes. `notepad seg2:0x05d1` and `seg2:0x05e4` call it twice with
     strings out of its own DGROUP --

         ds:0x0201 = "commdlg_FindReplace"
         ds:0x0215 = "commdlg_help"

     -- which are the two message names the common dialogs are documented to
     register, and each call is followed by `or ax,ax / jne` falling through to
     `jmp 0x02cb`, the `sub ax,ax / ret` that abandons the whole initialisation.
     So Notepad shows its window and then throws it away, twice over, for want of
     a message id.
   ★ THE ANSWER IS THE OS's, for the same reason RegisterClipboardFormat's is: a
     registered window message is a name in a SYSTEM-WIDE atom table, and its
     whole purpose is that two programs which register the same string get the
     same number. Our own table would agree with nothing. */
#define WOWUSER_REGWINMSG        0x76
#define RWM_ARG_NAME             0

/* SetWindowText(hWnd, lpString) -- 6 bytes, reversed: +0/+2 the string's far
   pointer, +4 the window. Without it a window's caption stays whatever
   CreateWindow was given, which for NOTEPAD is the empty string -- so its title
   bar and its taskbar button are blank and it looks like a window with no name
   rather than a program that has not been told to say one. */
#define WOWUSER_SETWINDOWTEXT    0x25
#define SWT_ARG_TEXT             0
#define SWT_ARG_HWND             4

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
#define AD_KIND_MODULERES        3

/* ── ★★★★★ 0xaf -- "BUILD ME A BITMAP FROM THESE RESOURCE BYTES". ────────────
     MS Paint's second-to-last wall: with the registration, the DCs and the
     CREATESTRUCT in place it gets as far as building its toolbox, fails to load
     the bitmap for it, and says "Not enough memory to edit image."

     Another id USER's export table cannot name, found the same way `0xad` was --
     by reading USER's own call site. `user.exe seg1:0x4732`:

         4732  push [bp+0x0a]   -> +12   hInstance
         4735  push [bp+0x08]   -> +10 } lpszName, far  (SEG at +10, OFF at +8)
         4738  push [bp+0x06]   -> +8  }
         473b  push [bp-0x0a]   -> +6  } the resource's BYTES, far
         473e  push [bp-0x0c]   -> +4  }  (set from the DX:AX at 0x4728)
         4741  push [bp-0x06]   -> +2  } its SIZE, a DWORD
         4744  push [bp-0x08]   -> +0  }
         4747  lcall <id 0xaf>
     Seven pushes = the 14 argument bytes the stub declares.

   ★★★ AND IT IS `LoadBitmap`, PROVEN THREE WAYS RATHER THAN INFERRED:
     1. `USER.175 LOADBITMAP` (seg1:0x2e78) is `native16`, and it ENDS in
        `2e8f jmp 0x46b8` -- a tail-jump into the function this call site is in.
        Its return thunk is `retf 6`, i.e. exactly (HINSTANCE + LPCSTR).
     2. The names that arrive are "pToolbox" and "pArrow".
     3. PBRUSH's own resource table has `BITMAP PTOOLBOX 9040` and
        `BITMAP PARROW 208` -- and the SIZE at +0 arrives as 0x2350 and 0x00d0.
     A wrong reading does not produce three agreements.

   ★★ WHAT THE BYTES ARE, MEASURED AND NOT ASSUMED. Read straight out of
     PBRUSH.EXE at the offsets `neres.py list` gives:
        PTOOLBOX  28 00 00 00 | 3a 00 00 00 | 17 01 00 00 | 01 00 | 04 00 | ...
        PARROW    28 00 00 00 | 0c 00 00 00 | 0d 00 00 00 | 01 00 | 04 00 | ...
     `biSize` = 0x28 = 40, so these are BITMAPINFOHEADER (Windows 3.0) DIBs --
     58x279 and 12x13, 4bpp -- NOT the 12-byte BITMAPCOREHEADER form. A packed
     DIB is header + palette + pixels, which is precisely what `CreateDIBitmap`
     consumes, so USER's 16-bit half has already done all the resource work and
     this is the one call it cannot make.
     ⚠ PARROW's `biSizeImage` is 8, which is junk (12px at 4bpp padded is 8 bytes
       per ROW, and 13 rows is 104 -- and 40 + 16*4 + 104 = 208, the whole
       resource). It is ignored for BI_RGB, and this zeroes it in its own copy of
       the header rather than trusting GDI to overlook it.
   ⚠ THE CORE-HEADER FORM IS REFUSED, NOT GUESSED AT. No guest has produced one
     here, so there is nothing to check a reading against. */
#define WOWUSER_LOADBITMAPRES    0x00af
#define LBM_ARG_SIZE     0
#define LBM_ARG_BITS     4
#define LBM_ARG_NAME     8
#define LBM_ARG_HINST   12

/* Tokens live well above the window handles (0x0100 + n*0x20) so a stray one is
   never mistaken for a window, and vice versa. */
#define WOWUSER_SYSRES_BASE      0x8000
#define WOWUSER_SYSRES_STEP      0x0008
#define WOWUSER_MAX_SYSRES       16

typedef struct {
    WORD h;                          /* 0 = free */
    WORD ord;                        /* the ordinal the guest asked for */
    WORD kind;                       /* 1 = predefined system object, 3 = the
                                        MODULE's own resource */
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

/* Which kind of thing the token stands for -- 0 if it is not one of ours. */
static WORD wowuser_sysres_kind(WORD h)
{
    int i;
    if (!h) return 0;
    for (i = 0; i < g_wu_nsysres; ++i)
        if (g_wu_sysres[i].h == h) return g_wu_sysres[i].kind;
    return 0;
}

/* ── ★★★ THE LAYOUT CLUSTER -- WHAT MAKES NOTEPAD USABLE. (session 44) ────────
     Every id here was named by the RUN and confirmed against USER's own export
     table (`docs/research/wow-user-surface.md`), not chosen from a list: the host
     log records each unimplemented USER call with the return address it came
     from, `from - 5` is the call site, and `tools/ne/neimports.py` names it out
     of NOTEPAD.EXE's own relocation chain. Four agreed both ways:

        0x38  12 args  MOVEWINDOW          notepad seg1:0x0061
        0x7d   8 args  INVALIDATERECT      notepad seg1:0x0044
        0xb3   2 args  GETSYSTEMMETRICS    notepad seg1:0x0c04
        0x1f   2 args  ISICONIC            notepad seg1:0x0aba

   ★ AND NOTEPAD'S RESIZE HELPER READS STRAIGHT OFF THE DISASSEMBLY, which is
     where the argument order below comes from rather than from a parameter list:

        0037  push bp / mov bp,sp
        003a  push [0x12] / push 0 / push 0 / push 1
        0044  lcall  InvalidateRect(hEdit, NULL, TRUE)
        0049  push [0x12]                  ; hWnd  = its EDIT control
        004d  push 8 / push 2              ; X, Y
        0051  mov ax,[bp+6] / sub ax,0x0f / push ax   ; nWidth
        0058  mov ax,[bp+4] / sub ax,4    / push ax   ; nHeight
        005f  push 1                       ; bRepaint
        0061  lcall  MoveWindow
        0069  ret 4

     -- 12 bytes and 8 bytes exactly, which is what each stub declares. The block
     is REVERSED as always (the base is the LAST push).
   ⚠ `DefWindowProc` is NOT here and must not be added: `USER.107` resolves to
     entry-table `FIXED, segment 1, offset 0x1d5e`, and the bytes there are
     `55 8b ec 68 86 1d ...` -- ordinary 16-bit code, not a `6a XX 68 00 00 68`
     WOW32 stub. USER implements it ITSELF, which is why a whole run of Notepad
     never produced one as a BOP. `DefFrameProc` (0x1bd) and `DefMDIChildProc`
     (0x1bf) ARE stubs; nothing has called them yet. */
#define WOWUSER_MOVEWINDOW       0x0038
#define MW_ARG_REPAINT   0
#define MW_ARG_CY        2
#define MW_ARG_CX        4
#define MW_ARG_Y         6
#define MW_ARG_X         8
#define MW_ARG_HWND     10

#define WOWUSER_INVALIDATERECT   0x007d
#define IR_ARG_ERASE     0
#define IR_ARG_RECT      2
#define IR_ARG_HWND      6

#define WOWUSER_GETSYSTEMMETRICS 0x00b3
#define GSM_ARG_INDEX    0

#define WOWUSER_ISICONIC         0x001f
#define II_ARG_HWND      0

/* ── ★★★ THE ENUMERATED BATCH -- FROM `tools/ne/neneeds.py`, NOT FROM A RUN ───
     Everything above was named by a guest stopping on it. These were named by
     reading NOTEPAD.EXE's import table and resolving each ordinal through USER's
     own entry table to the bytes it lands on -- so they are calls the program
     provably can make, including the ones that would have failed QUIETLY. See
     the tool for why an import is not automatically work, and for the two export
     prologues that have to be seen through to tell a thunk from 16-bit code.

     Every argument block below is the standard reversal (the base is the LAST
     word pushed), and every one is cross-checked against the argument-byte count
     the stub itself declares -- which is what makes the layout a reading rather
     than a parameter list copied from somewhere:

       0x17  GETFOCUS               0 bytes   ()
       0x22  ENABLEWINDOW           4 bytes   (hWnd, bEnable)          2+2
       0x35  DESTROYWINDOW          2 bytes   (hWnd)                   2
       0x3b  SETACTIVEWINDOW        2 bytes   (hWnd)                   2
       0x68  MESSAGEBEEP            2 bytes   (uType)                  2
       0x89  OPENCLIPBOARD          2 bytes   (hWnd)                   2
       0x8a  CLOSECLIPBOARD         0 bytes   ()
       0x90  ENUMCLIPBOARDFORMATS   2 bytes   (wFormat)                2
     A parameter list that did not add up to the declared count would mean the
     reading is wrong, and all eight add up. */
/* ── ★★★★ 0x01 MessageBox -- AND IT IS NOT IN THE IMPORT-DERIVED LIST ────────
     `neneeds.py` classifies `USER.1 MESSAGEBOX` as native16, and it is RIGHT:
     the export at seg1:0x29e3 is 16-bit code. It is a WRAPPER, and it reaches
     the WOW32 stub at seg1:0x0b62 from inside its own body -- exactly the
     `LoadIcon`/`0xad` shape the tool's own header warns about. So this is the
     first thing the enumeration could not see, found the old way: a run stopped
     on it.
   ★★ AND IT IS THE MOST VALUABLE ONE IN THE FILE, because it is how the program
     TALKS. Notepad reached this immediately after reading the file it was asked
     to open, with `uType = 0x30` (an exclamation) and the caption "Notepad" --
     i.e. it had already decided something was wrong and was trying to say what.
     Dropping the call threw the sentence away and left "nothing happened", which
     is why two sessions of this have been guesswork. An implemented MessageBox
     turns every future failure of this kind into a sentence on the screen and in
     the log.
   ★ The block, from the arguments the run itself printed (`0x0030 0x265a 0x0a9e
     ...`, where `0x0a9e:0x265a` decoded to "Notepad"): reversed as always, so
     uType is at +0 and hWnd -- pushed first -- is at +10. 2+4+4+2 = 12, which is
     what the stub declares.
   ⚠ MODAL, on the exec thread, like ShellAbout and the file dialog. */
#define WOWUSER_MESSAGEBOX       0x0001
#define MSGB_ARG_TYPE    0
#define MSGB_ARG_CAPTION 2
#define MSGB_ARG_TEXT    6
#define MSGB_ARG_HWND   10

#define WOWUSER_GETFOCUS         0x0017
#define WOWUSER_ENABLEWINDOW     0x0022
#define EW_ARG_ENABLE    0
#define EW_ARG_HWND      2
#define WOWUSER_DESTROYWINDOW    0x0035
#define DW_ARG_HWND      0
#define WOWUSER_SETACTIVEWINDOW  0x003b
#define SAW_ARG_HWND     0
#define WOWUSER_MESSAGEBEEP      0x0068
#define MB_ARG_TYPE      0
#define WOWUSER_OPENCLIPBOARD    0x0089
#define OC_ARG_HWND      0
#define WOWUSER_CLOSECLIPBOARD   0x008a
#define WOWUSER_ENUMCLIPFMT      0x0090
#define ECF_ARG_FORMAT   0

/* ── ★★★★★ THE DEVICE-CONTEXT TRIO -- WHERE MS PAINT ACTUALLY STOPS. ─────────
     Session 44 left a note saying the next thing for Paint was GDI's producers
     (`CreateDC`, `CreateCompatibleDC`, `GetStockObject`). A run of PBRUSH.EXE
     says otherwise, and the program said it in English: it puts up

         Paintbrush: "Not enough memory to perform this operation."

     and the call immediately before that box is USER id 0x42, 2 argument bytes,
     argument 0x0140 -- the hwnd Paint had just created. That is `GetDC`, it was
     answered 0, and Paint reads a null DC as being out of memory.
   ★★ SO THE FIRST DC THIS HOST EVER ISSUES COMES OUT OF **USER**, NOT GDI. That
     is worth stating plainly because it inverts the plan that was written down:
     the three GDI calls already implemented (`GetDeviceCaps`, `DeleteDC`,
     `DeleteObject`) could only ever answer "not one of our GDI tokens" until
     something produced one, and the producer was in another id space all along.
   ★ The ids and their argument counts are from `tools/ne/neneeds.py --stubs`,
     resolved through USER's own entry table, and the retstubs it computes from
     the file match the ones the run printed to the digit:
         66 GETDC        id 0x42   2 args  retstub 0x076a   (the run: 0x076a)
         67 GETWINDOWDC  id 0x43   2 args  retstub 0x090a
         68 RELEASEDC    id 0x44   4 args  retstub 0x0c5c
     2 = (HWND); 4 = (HWND, HDC). Both add up to what the stubs declare.
   ⚠ 0x44 IS `DeleteDC` IN GDI'S NUMBERING AND `ReleaseDC` HERE -- the same
     collision this file exists to prevent, and a reason the dispatcher must
     never reach this switch with another module's stub segment. */
#define WOWUSER_GETDC            0x0042
#define WOWUSER_GETWINDOWDC      0x0043
#define GDC_ARG_HWND16   0
#define WOWUSER_RELEASEDC        0x0044
#define RDC_ARG_HDC      0
#define RDC_ARG_HWND     2

/* ★ USER's DC calls MINT A GDI TOKEN out of the map in wowgdi.h, which main.c
   now includes BEFORE this file for exactly that reason. */

/* ── ★★★ MENUS AND DIALOG ITEMS -- THE LAST TWO CLUSTERS. ────────────────────
     Both from `neneeds.py`'s list, and both blocked on the same missing piece: a
     16-bit name for an `HMENU`. A Win32 menu handle is 32 bits and a Win16
     program has 16 to hold it in, so the same answer as everywhere else in this
     file -- a TOKEN the guest can carry, with the real object behind it.

   ★★ AND IT HAS TO BE A TOKEN, NOT A TRUNCATION, BECAUSE THE HANDLE GOES BACK
     THROUGH 16-BIT CODE. `USER.154 CHECKMENUITEM` and `USER.155 ENABLEMENUITEM`
     are `native16` -- USER implements them in its own code, which will hand the
     handle to a stub of its own later. So whatever `GetMenu` returns must
     survive a round trip through USER and still name the right menu when it
     comes back. A truncated pointer would not, and would not fail loudly either.

     0x9c  GETSYSTEMMENU   4 bytes  (hWnd, bRevert)
     0x9d  GETMENU         2 bytes  (hWnd)
     0x9f  GETSUBMENU      4 bytes  (hMenu, nPos)
     0x58  ENDDIALOG       4 bytes  (hDlg, nResult)
     0x5b  GETDLGITEM      4 bytes  (hDlg, nIDDlgItem)
     0x5d  GETDLGITEMTEXT 10 bytes  (hDlg, nID, lpString, nMaxCount)  2+2+4+2
     0x5e  SETDLGITEMINT   8 bytes  (hDlg, nID, wValue, bSigned)      2+2+2+2
     0x65  SENDDLGITEMMSG 12 bytes  (hDlg, nID, wMsg, wParam, lParam) 2+2+2+2+4
   Every one adds up to the byte count its own stub declares. */
/* ★★ AND THE TWO THAT ACTUALLY CHANGE A MENU, found the way MessageBox was:
     `USER.154 CHECKMENUITEM` and `USER.155 ENABLEMENUITEM` are `native16`, so
     neneeds.py cannot see them -- but they are WRAPPERS that reach stubs of
     their own, and the run named both the moment WM_INITMENUPOPUP started
     arriving: `0x9a` and `0x9b`, 6 argument bytes each, six calls in one opening
     of Notepad's Edit menu. That is the THIRD time today a native16 wrapper has
     turned out to be real work (MessageBox and LoadIcon are the others), which
     is exactly the limit the tool documents.
     (hMenu, wID, wFlags) = 2+2+2 = 6, reversed as always. */
#define WOWUSER_CHECKMENUITEM    0x009a
#define WOWUSER_ENABLEMENUITEM   0x009b
#define MI_ARG_FLAGS     0
#define MI_ARG_ID        2
#define MI_ARG_HMENU     4

#define WOWUSER_GETSYSTEMMENU    0x009c
#define GSYM_ARG_REVERT  0
#define GSYM_ARG_HWND    2
#define WOWUSER_GETMENU          0x009d
#define GM2_ARG_HWND     0
#define WOWUSER_GETSUBMENU       0x009f
#define GSM2_ARG_POS     0
#define GSM2_ARG_HMENU   2

#define WOWUSER_ENDDIALOG        0x0058
#define ED_ARG_RESULT    0
#define ED_ARG_HDLG      2
#define WOWUSER_GETDLGITEM       0x005b
#define GDI2_ARG_ID      0
#define GDI2_ARG_HDLG    2
#define WOWUSER_GETDLGITEMTEXT   0x005d
#define GDIT_ARG_MAX     0
#define GDIT_ARG_BUF     2
#define GDIT_ARG_ID      6
#define GDIT_ARG_HDLG    8
#define WOWUSER_SETDLGITEMINT    0x005e
#define SDII_ARG_SIGNED  0
#define SDII_ARG_VALUE   2
#define SDII_ARG_ID      4
#define SDII_ARG_HDLG    6
#define WOWUSER_SENDDLGITEMMSG   0x0065
#define SDIM_ARG_LPARAM  0
#define SDIM_ARG_WPARAM  4
#define SDIM_ARG_MSG     6
#define SDIM_ARG_ID      8
#define SDIM_ARG_HDLG   10

/* Menu tokens live between the window handles (0x0100 + n*0x20) and the
   cursor/icon tokens (0x8000 + n*8), so a stray one of any kind is recognisable
   on sight in a log rather than being mistaken for another kind of object. */
#define WOWUSER_MENU_BASE   0x4000
#define WOWUSER_MENU_STEP   0x0008
#define WOWUSER_MAX_MENU    64

typedef struct { WORD h; HMENU m; } wowuser_menu_t;
static wowuser_menu_t g_wu_menu[WOWUSER_MAX_MENU];
static int            g_wu_nmenu = 0;

/* One token per HMENU: the OS hands back the same handle for the same menu, and
   a program that asks twice must get one answer, the way it does for a cursor. */
static WORD wowuser_menu16(HMENU m)
{
    int i;
    if (!m) return 0;
    for (i = 0; i < g_wu_nmenu; ++i)
        if (g_wu_menu[i].m == m) return g_wu_menu[i].h;
    if (g_wu_nmenu >= WOWUSER_MAX_MENU) return 0;
    i = g_wu_nmenu++;
    g_wu_menu[i].m = m;
    g_wu_menu[i].h = (WORD)(WOWUSER_MENU_BASE + i * WOWUSER_MENU_STEP);
    return g_wu_menu[i].h;
}

static HMENU wowuser_menu32(WORD h)
{
    int i;
    if (!h) return NULL;
    for (i = 0; i < g_wu_nmenu; ++i)
        if (g_wu_menu[i].h == h) return g_wu_menu[i].m;
    return NULL;
}

/* ⚠⚠ A Win16 RECT IS FOUR **WORDS**; A Win32 RECT IS FOUR **LONGS**. Eight bytes
     against sixteen, and nothing about a wrong reading looks wrong -- it just
     produces coordinates off by whatever the neighbouring field held. Anywhere a
     RECT crosses this boundary it is converted field by field, and this is the
     only place that says so. */
#define WOW_RECT16_SIZE  8

/* ── ★ AND HERE IS WHERE A TOKEN BECOMES A REAL HICON. ───────────────────────
     0xad can only hand back a token, because at that moment nothing knows whether
     the guest wants a cursor or an icon (see the long note above). The moment it
     USES one as an icon, this resolves it -- predefined ordinals through the OS,
     the application's own through its own file.
   ⚠ ONE IMPLEMENTATION, TWO CALLERS. `RegisterClass` puts the result in a Win32
     class and `ShellAbout` puts it in the About box, and the two resolving a token
     differently is exactly the kind of drift that shows up as "the icon is right
     in one place and wrong in the other".
   `picked` receives the colour depth chosen out of the application's own icon
     group, or 0, so a log line can say which image the OS was given. */
static HICON wowuser_sysres_hicon(WORD token, int *picked)
{
    WORD ord  = wowuser_sysres_ord(token);
    WORD kind = wowuser_sysres_kind(token);
    if (picked) *picked = 0;
    if (!ord) return NULL;
    if (kind == AD_KIND_MODULERES)
        return wowres_open(g_wow_cmd_prog) ? wowres_icon(ord, picked) : NULL;
    return LoadIconA(NULL, MAKEINTRESOURCEA(ord));
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
/* ★ And LocalReAlloc, from the same non-resident name table: `6 LOCALREALLOC`
     at 0x3e1f. `HLOCAL LocalReAlloc(HLOCAL, WORD cbNew, WORD flags)` -- three
     words, far, and SYSEDIT's own call site pushes them in exactly that order
     (`push ax / push [bp-4]+1 / push 0x42` at seg3:0x0124), which is what pins
     the argument order rather than a header. */
#define KRNL_LOCALREALLOC_OFF 0x3e1f
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
    WORD  curord, icoord, icokind;
    int   curfell;                   /* the OS did not know that cursor ordinal */
    int   icobits;                   /* colour depth of the icon actually built  */
    /* What the class said its menu was -- a name, or an ordinal. Recorded and
       logged; not yet turned into a real HMENU (that needs the guest's own MENU
       resource, which lives in its NE file). */
    char  menuname[64];
    WORD  menuord;
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
    int   menuitems;                 /* how many entries its class menu produced */
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

/*
 * ── ★★★★★ THE CREATESTRUCT, AND IT WAS READ OFF A RUN, NOT A HEADER ─────────
 * WM_CREATE's lParam is an LPCREATESTRUCT. This host passed 0 and said so, and
 * that stayed harmless until MS PAINT: its canvas procedure GP-faults on the
 * spot, reading through the null pointer to find out how big it is. From
 * `nedis.py guest/win16/PBRUSH.EXE 3 0x0720`, with `es:bx` = lParam:
 *
 *     0746  mov es, dx            ; es:bx <- lParam
 *     0748  mov bx, ax
 *     074a  mov cx, es:[bx+0xc]   ; ★ THE FAULT -- CREATESTRUCT.cx
 *     074e  mov [0x4e0a], cx
 *     0752  mov cx, es:[bx+0xa]   ;   CREATESTRUCT.cy
 *
 * ── ★★★ THE LAYOUT IS THE CreateWindow ARGUMENT BLOCK, UNCHANGED ────────────
 * Which is why this needs no header and no guesswork. Paint's own call carried
 *     (0000 0000 | 09c6 | 0001 | 0160 | 0002 | 0002 | 0002 | 00ac | 0000 40b0
 *      | 0000 0000 | 08bd 09c7)
 * and the CW_ARG_* offsets this file already uses -- named from earlier runs and
 * cross-checked against the 30 bytes the stub declares -- read that as
 * lpParam@0, hInstance@4, hMenu@6, hwndParent@8, cy@10, cx@12, y@14, x@16,
 * style@18 (0x40b00000, exactly what the log printed), lpszName@22,
 * lpszClass@26 ("pbPaint"). Those are the CREATESTRUCT's members, in order, at
 * those offsets -- and PBRUSH reading cx at +0x0c and cy at +0x0a agrees with it
 * independently. So the structure is the argument block COPIED, plus a
 * `dwExStyle` of 0 at +30 to make up the 34 bytes.
 * ⇒ Nothing here is taken on trust: two readings of two different binaries.
 *
 * ⚠ CW_USEDEFAULT IS SUBSTITUTED, NOT COPIED. A guest may pass 0x8000 for any of
 *   x/y/cx/cy and then read the field expecting a number it can compute with --
 *   Paint passes real values, so this is not what fixed it, but Notepad does not
 *   and a copied 0x8000 would be a size of -32768. The real window's own
 *   geometry is used instead, which is what the guest would have got on Windows.
 * ⚠ ONE THING IS STILL NOT MEASURED: whether real Windows shows a guest the
 *   REQUESTED or the RESOLVED geometry for the fields it did not default. This
 *   copies what was requested. No run has yet distinguished the two.
 */
static void wowuser_want_create(wow32_frame_t *f, const wowuser_class_t *c,
                                const wowuser_win_t *w)
{
    BYTE *b = f->cbblob;
    int i;
    WORD g[4];
    static const int GEO[4] = { CW_ARG_HEIGHT, CW_ARG_WIDTH,
                                CW_ARG_Y, CW_ARG_X };
    if (!f->cbok || !w->wndproc) return;

    /* The 30 argument bytes, verbatim -- they ARE the first eleven members. */
    for (i = 0; i < 30; ++i) b[i] = f->bp[WOW32_OFF_ARGS + i];
    b[30] = b[31] = b[32] = b[33] = 0;               /* dwExStyle, always 0 here */

    /* ⚠ CW_USEDEFAULT is 0x8000 in Win16 (it is 0x80000000 in Win32 -- the trap
         this host has already been caught by once). Where the guest defaulted a
         field, tell it what it actually got. */
    if (w->hwnd32) {
        RECT r;
        if (GetWindowRect(w->hwnd32, &r)) {
            g[0] = (WORD)(r.bottom - r.top);         /* cy, cx, y, x -- GEO order */
            g[1] = (WORD)(r.right - r.left);
            g[2] = (WORD)r.top;
            g[3] = (WORD)r.left;
            for (i = 0; i < 4; ++i)
                if (wow32_argw(f, GEO[i]) == 0x8000) {
                    b[GEO[i]]     = (BYTE)(g[i] & 0xFF);
                    b[GEO[i] + 1] = (BYTE)(g[i] >> 8);
                }
        }
    }
    f->cbblobn = 34;

    wowuser_want_msg(f, w, w->hinst ? w->hinst : c->hinst,
                     WM_CREATE16, 0, 0, WOWCALL_RET_KEEP);
    /* cbarg[3] is lParam's HIGH word (see wowuser_want_msg); wowcall_enter fills
       both halves once it knows where on the stack the structure landed. */
    f->cbblobarg = 3;
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
/* The three text messages, named because this host now handles them. Win16 and
   Win32 agree on all three numbers -- Win32 inherited them, the same claim the
   keyboard messages already travel on. */
#define WM_SETTEXT16        0x000C
#define WM_GETTEXT16        0x000D
#define WM_GETTEXTLENGTH16  0x000E

/* Resolve a 16:16 far pointer VALUE (as carried in an lParam) to a host address.
   ⚠ Null selector yields NULL rather than the LDT base, the same rule
     wow32_argptr follows, so a forgotten check cannot scribble at the bottom of
     the address space. */
static volatile BYTE *wowuser_lin(const wow32_frame_t *f, DWORD fp)
{
    WORD  sel = (WORD)(fp >> 16);
    DWORD base;
    if (!sel || !f->sel2lin) return NULL;
    base = f->sel2lin(sel, f->ctx);
    if (!base) return NULL;
    return (volatile BYTE *)(ULONG_PTR)(base + (fp & 0xFFFF));
}

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
    /* ── ★★★★★ AND IT MUST CARRY THE CONTROL'S **CURRENT** TEXT. (session 44) ──
         Returning the same handle a second time -- what this did until now, as
         "(already allocated)" -- is what made File > Save write the wrong bytes.
         The block holds whatever was put in it when the file was LOADED; every
         keystroke since then went into the real Win32 EDIT control, which is
         where the text actually lives. Notepad's save path is
         `WM_GETTEXTLENGTH` (answered correctly, 0x41) then EM_GETHANDLE,
         LocalLock and `_lwrite` of that many bytes -- so it wrote the right
         LENGTH from the wrong BUFFER, and the file came back as the old text
         followed by five bytes of heap litter. Measured, byte for byte.
       ⇒ EM_GETHANDLE now REFRESHES the block from the real control, which is
         the exact mirror of what EM_SETHANDLE already does in the other
         direction, and it is a three-call chain into the guest's own KERNEL
         because only the guest's KERNEL can touch the guest's local heap:
             LocalAlloc/LocalReAlloc  -> a block big enough for the text
               -> ACT_EDITLOCK: LocalLock  -> a near offset into its DGROUP
                 -> ACT_EDITFILL: write the text there, then LocalUnlock
         Chaining is not new machinery: EM_SETHANDLE's ACT_EDITTEXT already
         issues a follow-up LocalUnlock from inside an action.
       ⚠ THE HANDLE CAN MOVE. LocalReAlloc may return a different handle, so the
         sink updates `w->hmem` before the action runs (the return path applies
         the sink first) and every later step uses the NEW one.
       ⚠ `f->cbds` is the CONTROL'S OWN hInstance, not the caller's: LocalAlloc
         allocates from the local heap of whatever DS it is entered with, and the
         heap this handle must be valid in is the application's own. */
    case EM_GETHANDLE16: {
        int  n    = w->hwnd32 ? GetWindowTextLengthA(w->hwnd32) : 0;
        WORD need = (WORD)(n + 1);
        if (need < WOWUSER_EDIT_INITIAL) need = WOWUSER_EDIT_INITIAL;
        if (!f->cbok || !w->hinst || !g_wu_krnl_seg) {
            wu_puts(note, notecap, &k, "EM_GETHANDLE: cannot reach the guest's heap"
                                       " (no callback, no instance, or krnl386's"
                                       " segment is not known yet), answered 0x");
            wu_puthex(note, notecap, &k, w->hmem, 4);
            return (LONG)w->hmem;
        }
        wu_puts(note, notecap, &k, "EM_GETHANDLE: the real control holds 0x");
        wu_puthex(note, notecap, &k, (DWORD)n, 4);
        wu_puts(note, notecap, &k, " char(s); ");
        if (w->hmem) {
            wu_puts(note, notecap, &k, "LocalReAlloc 0x");
            wu_puthex(note, notecap, &k, w->hmem, 4);
            wu_puts(note, notecap, &k, " to 0x");
            wu_puthex(note, notecap, &k, need, 4);
            f->cbproc   = ((DWORD)g_wu_krnl_seg << 16) | KRNL_LOCALREALLOC_OFF;
            f->cbarg[0] = w->hmem;
            f->cbarg[1] = need;
            f->cbarg[2] = LMEM_MOVEABLE_ZEROINIT;
            f->cbnarg   = 3;
        } else {
            wu_puts(note, notecap, &k, "LocalAlloc 0x");
            wu_puthex(note, notecap, &k, need, 4);
            f->cbproc   = ((DWORD)g_wu_krnl_seg << 16) | KRNL_LOCALALLOC_OFF;
            f->cbarg[0] = LMEM_MOVEABLE_ZEROINIT;
            f->cbarg[1] = need;
            f->cbnarg   = 2;
        }
        wu_puts(note, notecap, &k, " in DGROUP 0x");
        wu_puthex(note, notecap, &k, w->hinst, 4);
        wu_puts(note, notecap, &k, ", then lock and fill it from the control");
        f->cbds     = w->hinst;
        f->cbret    = WOWCALL_RET_RESULTW;   /* a WORD handle in AX */
        f->cbsink   = &w->hmem;
        f->cbact    = WOWCALL_ACT_EDITLOCK;
        f->cbactarg = w->hwnd;
        f->cbhwnd   = w->hwnd;
        f->cbmsg    = msg;
        return 0;                    /* replaced by the allocator's own answer */
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

    /* ── ★★★★★ THE TEXT MESSAGES -- AND THIS IS WHY SAVE SAVED NOTHING. ──────
         Notepad's File > Save asks its edit control how much text it holds, and
         this procedure answered 0 for every message it did not know. So Notepad
         put up, in its own words:

           "C:\DOCUME~1\Matthew\MYDOCU~1\test.txt
            This file is empty and will be deleted. This file cannot be saved
            because it is empty."

         -- with the text plainly visible in the control on screen. The control
         is a REAL Win32 EDIT and its text lives in the OS, so the only thing
         that ever knew the answer was the OS, and we were not asking it.
       ★ These three forward to the real control, which is the same argument as
         everywhere else in this file: the window is real, so the OS's answer IS
         the answer. Nothing is cached here -- a copy would be a second version
         of the text that goes stale the moment the user types.
       ⚠⚠ THE POINTER ONES MUST BE TRANSLATED, WHICH IS WHY THIS IS NOT A BLANKET
         FORWARD OF EVERY UNKNOWN MESSAGE. `WM_GETTEXT`/`WM_SETTEXT` carry a
         16:16 far pointer in lParam; handing that to Win32 as a flat address
         would read or WRITE at an arbitrary place in our own address space. A
         message whose parameters this host has not read stays unimplemented and
         says so, exactly as before.
       ⚠ WM_GETTEXT's wParam is the buffer size the CALLER declared, and it is
         the only bound there is -- passed straight to the OS, which respects it. */
    /* ── ★★★★★ AND THIS IS WHERE THE GUEST'S COPY IS BROUGHT UP TO DATE. ─────
         ⚠ EM_GETHANDLE IS NOT ENOUGH, AND THE RUN SAYS SO: it is called ONCE, at
           LOAD time, when the control is still empty. Notepad then allocates its
           own block, fills it from the file, hands it over with EM_SETHANDLE --
           and KEEPS THE HANDLE. At save time it never asks again; it asks the
           LENGTH and writes that many bytes straight out of the block it
           remembers. So the refresh has to happen here, at the last moment the
           guest touches the control before writing.
         ⇒ Measured before this: the file came back the right LENGTH (0x40) and
           the WRONG BYTES -- the text as it was when loaded, plus five bytes of
           heap litter where the new characters should have been.
       ★ The answer goes back first and the chain runs behind it: the return hole
         is written before wowcall_enter is reached, and the chain uses RET_KEEP
         so nothing overwrites it. The sink still fires, because a handle that
         moved must still be recorded.
       ⚠ A Win16 LOCAL handle is STABLE across LocalReAlloc (the memory moves,
         the handle does not), which is what makes this safe -- Notepad is still
         holding that handle and will write through it a moment from now.
       ⚠ Only when there is a block to refresh. Before EM_SETHANDLE there is
         nothing the guest owns and nothing to update. */
    case WM_GETTEXTLENGTH16: {
        int n = w->hwnd32 ? GetWindowTextLengthA(w->hwnd32) : 0;
        wu_puts(note, notecap, &k, "WM_GETTEXTLENGTH -> 0x");
        wu_puthex(note, notecap, &k, (DWORD)n, 4);
        wu_puts(note, notecap, &k, w->hwnd32 ? " (the real control's)"
                                             : " -- no real control");
        if (n > 0 && w->hmem && w->hwnd32 && f->cbok && w->hinst
            && g_wu_krnl_seg) {
            f->cbproc   = ((DWORD)g_wu_krnl_seg << 16) | KRNL_LOCALREALLOC_OFF;
            f->cbds     = w->hinst;
            f->cbarg[0] = w->hmem;
            f->cbarg[1] = (WORD)(n + 1);
            f->cbarg[2] = LMEM_MOVEABLE_ZEROINIT;
            f->cbnarg   = 3;
            f->cbret    = WOWCALL_RET_KEEP;   /* the LENGTH is the answer */
            f->cbsink   = &w->hmem;
            f->cbact    = WOWCALL_ACT_EDITLOCK;
            f->cbactarg = w->hwnd;
            f->cbhwnd   = w->hwnd;
            f->cbmsg    = msg;
            wu_puts(note, notecap, &k, "; refreshing the guest's block 0x");
            wu_puthex(note, notecap, &k, w->hmem, 4);
            wu_puts(note, notecap, &k, " from the control before it writes");
        }
        return (LONG)n;
    }

    case WM_GETTEXT16: {
        volatile BYTE *dst = wowuser_lin(f, lparam);
        int n = 0;
        wu_puts(note, notecap, &k, "WM_GETTEXT max=0x");
        wu_puthex(note, notecap, &k, wparam, 4);
        if (!dst || !w->hwnd32 || !wparam) {
            wu_puts(note, notecap, &k, " -- no buffer or no real control;"
                                       " answered 0");
            return 0;
        }
        n = GetWindowTextA(w->hwnd32, (LPSTR)dst, (int)wparam);
        wu_puts(note, notecap, &k, " -> 0x");
        wu_puthex(note, notecap, &k, (DWORD)n, 4);
        wu_puts(note, notecap, &k, " char(s) into the guest's own buffer");
        return (LONG)n;
    }

    case WM_SETTEXT16: {
        volatile BYTE *src = wowuser_lin(f, lparam);
        wu_puts(note, notecap, &k, "WM_SETTEXT ");
        if (!src || !w->hwnd32) {
            wu_puts(note, notecap, &k, "-- no string or no real control;"
                                       " answered 0");
            return 0;
        }
        wu_putq(note, notecap, &k, (const char *)src);
        SetWindowTextA(w->hwnd32, (LPCSTR)src);
        return 1;
    }

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
        /* ★ THE MENU THE CLASS NAMES. A Win16 program does not have to call
             LoadMenu -- it can put the resource's NAME in the WNDCLASS and let
             CreateWindow attach it, which is what Notepad appears to do (it calls
             LoadMenu never, and it certainly has a File menu). Read and LOGGED
             before it is used, because "the class named a menu and we ignored it"
             and "the class named no menu" are different facts and the window looks
             identical either way. */
        c->menuname[0] = 0;
        {   DWORD mn = (DWORD)wowuser_peek(wc, WNDCLASS_MENUNAME)
                     | ((DWORD)wowuser_peek(wc, WNDCLASS_MENUNAME + 2) << 16);
            c->menuord = 0;
            if ((WORD)(mn >> 16) == 0) c->menuord = (WORD)mn;    /* MAKEINTRESOURCE */
            else wowuser_farstr(f, mn, c->menuname, sizeof c->menuname);
        }
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
            WORD icokind = wowuser_sysres_kind(c->hicon);
            int  fell   = 0, bits = 0;
            HICON hico  = wowuser_sysres_hicon(c->hicon, &bits);
            if (!c->reg32)
                c->reg32 = wowwin_register(c->name, c->cls32, sizeof c->cls32,
                                           curord, hico, &fell);
            c->curord = curord; c->icoord = icoord; c->curfell = fell;
            c->icobits = bits; c->icokind = icokind;
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
            if (c->menuname[0]) { wu_puts(note, notecap, &k, " MENU=");
                                  wu_putq(note, notecap, &k, c->menuname); }
            else if (c->menuord) { wu_puts(note, notecap, &k, " MENU=#");
                                   wu_puthex(note, notecap, &k, c->menuord, 4); }
            else wu_puts(note, notecap, &k, " (no menu named)");
            if (c->icoord) {
                wu_puts(note, notecap, &k, " icon=0x");
                wu_puthex(note, notecap, &k, c->icoord, 4);
                if (c->icokind == AD_KIND_MODULERES) {
                    wu_puts(note, notecap, &k, c->icobits ? " (the app's own, "
                                                            : " (the app's own -- "
                                                              "★ NOT BUILT");
                    if (c->icobits) { wu_puthex(note, notecap, &k,
                                                (DWORD)c->icobits, 2);
                                      wu_puts(note, notecap, &k, " bpp)"); }
                    else wu_puts(note, notecap, &k, ")");
                }
            }
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
            /* ── ★★★ THE CLASS'S OWN MENU, BUILT FROM THE GUEST'S RESOURCE.
                 A Win16 program does not have to call LoadMenu: it can name the
                 resource in its WNDCLASS and let CreateWindow attach it, which is
                 exactly what NOTEPAD does (`MENU=#0001`, and it calls LoadMenu
                 never). The bytes are in its own file, so the host reads them --
                 see wowres.h, whose decoding was confirmed against the data before
                 any of this existed.
               ⚠ A CHILD WINDOW HAS NO MENU: its hMenu slot is a control id, which
                 is why this is inside the else. */
            else if (!(w->style & WS_CHILD16) && cc->menuord) {
                int nitems = 0;
                if (wowres_open(g_wow_cmd_prog))
                    hm = wowres_menu(cc->menuord, &nitems);
                w->menuitems = nitems;
            }
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
            if (w->menuitems) { wu_puts(note, notecap, &k, " MENU=");
                                wu_puthex(note, notecap, &k, (DWORD)w->menuitems, 2);
                                wu_puts(note, notecap, &k, " items from the guest's"
                                                           " own resource"); }
            else if (g_wu_class[w->cls].menuord)
                wu_puts(note, notecap, &k, " -- \u2605 ITS CLASS NAMED A MENU AND"
                                           " NONE WAS BUILT");
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
    case WOWUSER_REGWINMSG:
    case WOWUSER_REGCLIPFORMAT: {
        int isclip = (f->id == WOWUSER_REGCLIPFORMAT);
        char name[128];
        UINT fmt = 0;
        int k = 0;
        wu_puts(note, notecap, &k, isclip ? "RegisterClipboardFormat "
                                          : "RegisterWindowMessage ");
        if (!wow32_argstr(f, RCF_ARG_NAME, name, sizeof name) || !name[0]) {
            wu_puts(note, notecap, &k, "-- no name, answered 0");
            wow32_setret(f, 0);
            return 1;
        }
        wu_putq(note, notecap, &k, name);
        fmt = isclip ? RegisterClipboardFormatA(name) : RegisterWindowMessageA(name);
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

    /* ── ★★★★★ 0xaf LoadBitmap's 32-bit half -- see the long note above. ─────
       ⚠ THE DIB IS READ WHERE THE GUEST PUT IT, but its HEADER is copied out
         first. Two reasons, and neither is tidiness: the copy is where
         `biSizeImage` gets its junk cleared without writing into guest memory,
         and it means the only thing handed to GDI as a pointer-into-the-guest is
         the PIXEL array, whose length this host can actually check against the
         size the guest declared.
       ⚠ EVERY FIELD IS BOUNDS-CHECKED AGAINST THAT DECLARED SIZE. The pointer
         comes from 16-bit code and the header is data from a file; a palette
         count or a header size read out of it could send the pixel pointer
         anywhere, and GDI would read it. A resource that does not add up is
         refused with the arithmetic in the log.
       ★ The handle is a GDI token, not a USER one -- the guest will hand it
         straight to SelectObject and DeleteObject. */
    case WOWUSER_LOADBITMAPRES: {
        static BYTE hdr[40 + 256 * 4];        /* header + palette, our own copy */
        DWORD size = wow32_argd(f, LBM_ARG_SIZE);
        volatile BYTE *p = wow32_argptr(f, LBM_ARG_BITS);
        char name[64];
        DWORD bisize, pal, off, i;
        int   bits, k = 0;
        HDC   dc;
        HBITMAP bm;
        WORD  tok;

        wow32_argstr(f, LBM_ARG_NAME, name, sizeof name);
        wu_puts(note, notecap, &k, "LoadBitmap ");
        wu_putq(note, notecap, &k, name);
        wu_puts(note, notecap, &k, " size=0x");
        wu_puthex(note, notecap, &k, size, 4);

        if (!p || size < 40 || size > 0x10000) {
            wu_puts(note, notecap, &k, " -- ★ NO BYTES, or a length that cannot be"
                                       " a packed DIB; answered 0");
            wow32_setret(f, 0);
            return 1;
        }
        bisize = (DWORD)wow32_peekw(p) | ((DWORD)wow32_peekw(p + 2) << 16);
        if (bisize != 40) {
            wu_puts(note, notecap, &k, " -- ★ biSize IS 0x");
            wu_puthex(note, notecap, &k, bisize, 4);
            wu_puts(note, notecap, &k, ", NOT 40. Only the BITMAPINFOHEADER form"
                                       " has been measured; the 12-byte core"
                                       " header is refused rather than guessed"
                                       " at; answered 0");
            wow32_setret(f, 0);
            return 1;
        }
        bits = (int)wow32_peekw(p + 14);                     /* biBitCount     */
        pal  = (DWORD)wow32_peekw(p + 32)                    /* biClrUsed      */
             | ((DWORD)wow32_peekw(p + 34) << 16);
        if (!pal && bits <= 8) pal = 1ul << bits;
        wu_puts(note, notecap, &k, " ");
        wu_puthex(note, notecap, &k, (DWORD)wow32_peekw(p + 4), 4);
        wu_puts(note, notecap, &k, "x");
        wu_puthex(note, notecap, &k, (DWORD)wow32_peekw(p + 8), 4);
        wu_puts(note, notecap, &k, " ");
        wu_puthex(note, notecap, &k, (DWORD)bits, 2);
        wu_puts(note, notecap, &k, "bpp pal=0x");
        wu_puthex(note, notecap, &k, pal, 4);

        off = 40 + pal * 4;                        /* where the pixels start   */
        if (pal > 256 || off >= size) {
            wu_puts(note, notecap, &k, " -- ★ THE HEADER DOES NOT ADD UP (pixels"
                                       " would start at 0x");
            wu_puthex(note, notecap, &k, off, 4);
            wu_puts(note, notecap, &k, " in 0x");
            wu_puthex(note, notecap, &k, size, 4);
            wu_puts(note, notecap, &k, " bytes); answered 0");
            wow32_setret(f, 0);
            return 1;
        }
        for (i = 0; i < off; ++i) hdr[i] = p[i];
        hdr[20] = hdr[21] = hdr[22] = hdr[23] = 0;           /* biSizeImage    */

        /* ⚠ A SCREEN DC, TAKEN AND GIVEN BACK HERE. CreateDIBitmap needs a DC to
             be compatible with, and this one is the host's own business -- it is
             never shown to the guest, so it takes no token. */
        dc = GetDC(NULL);
        if (!dc) {
            wu_puts(note, notecap, &k, " -- ★ NO SCREEN DC; answered 0");
            wow32_setret(f, 0);
            return 1;
        }
        bm = CreateDIBitmap(dc, (const BITMAPINFOHEADER *)hdr, CBM_INIT,
                            (const void *)(const BYTE *)(p + off),
                            (const BITMAPINFO *)hdr, DIB_RGB_COLORS);
        ReleaseDC(NULL, dc);
        tok = bm ? wowgdi_h16((HGDIOBJ)bm, WOWGDI_KIND_OBJ) : 0;
        if (!tok) {
            if (bm) DeleteObject((HGDIOBJ)bm);
            wu_puts(note, notecap, &k, bm ? " -- ★ THE GDI TOKEN MAP IS FULL; the"
                                            " bitmap was freed and 0 answered"
                                          : " -- ★ GDI REFUSED THE DIB;"
                                            " answered 0");
            wow32_setret(f, 0);
            return 1;
        }
        wu_puts(note, notecap, &k, " -> bitmap token 0x");
        wu_puthex(note, notecap, &k, tok, 4);
        wow32_setret(f, tok);
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
        /* ★ KIND 3 IS THE MODULE'S OWN RESOURCE, and it arrives with the same
             name fields at the same offsets -- the two call sites differ only in
             what they put in the middle. So an ordinal is enough to find the
             resource in the application's own file (see wowres.h), and the token
             carries the kind so RegisterClass knows which way to resolve it. */
        if (kind != AD_KIND_PREDEFINED && kind != AD_KIND_MODULERES) {
            wu_puts(note, notecap, &k, " -- a kind no call site has been read for;"
                                       " answered 0");
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
            if (g_wu_sysres[i].ord == lo && g_wu_sysres[i].kind == kind) {
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
        g_wu_sysres[i].ord  = lo;
        g_wu_sysres[i].kind = kind;
        g_wu_sysres[i].h   = (WORD)(WOWUSER_SYSRES_BASE + i * WOWUSER_SYSRES_STEP);
        wu_puts(note, notecap, &k, " -> token 0x");
        wu_puthex(note, notecap, &k, g_wu_sysres[i].h, 4);
        wu_puts(note, notecap, &k, "; the OS object is fetched when the guest says"
                                   " whether it is a cursor or an icon");
        wow32_setret(f, g_wu_sysres[i].h);
        return 1;
    }

    /* ── ★ 0x25 SetWindowText(hWnd, lpString) -- straight to the OS. ───────────
         The caption belongs to the real window, so there is nothing here to keep;
         the copy in wowuser_win_t is updated only so the host's own log keeps
         saying which window is which. */
    case WOWUSER_SETWINDOWTEXT: {
        WORD hwnd = wow32_argw(f, SWT_ARG_HWND);
        wowuser_win_t *w = wowuser_findwin(hwnd);
        char txt[128];
        int k = 0, i;
        wow32_argstr(f, SWT_ARG_TEXT, txt, sizeof txt);
        wu_puts(note, notecap, &k, "SetWindowText 0x");
        wu_puthex(note, notecap, &k, hwnd, 4);
        wu_puts(note, notecap, &k, " ");
        wu_putq(note, notecap, &k, txt);
        if (!w) { wu_puts(note, notecap, &k, " -- NO SUCH WINDOW");
                  wow32_setret(f, 0); return 1; }
        for (i = 0; i < (int)sizeof w->text - 1 && txt[i]; ++i) w->text[i] = txt[i];
        w->text[i] = 0;
        if (w->hwnd32) { SetWindowTextA(w->hwnd32, txt);
                         wu_puts(note, notecap, &k, " -> the OS's"); }
        else             wu_puts(note, notecap, &k, " -- no real window");
        wow32_setret(f, 0);
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
        wowuser_win_t *w = wowuser_findwin(hwnd);
        int k = 0;
        wu_puts(note, notecap, &k, "SetFocus 0x");
        wu_puthex(note, notecap, &k, hwnd, 4);
        if (hwnd && !w) {
            wu_puts(note, notecap, &k, " -- NO SUCH WINDOW, focus unchanged");
            wow32_setret(f, prev);
            return 1;
        }
        g_wm_focus = hwnd;
        /* ★ AND THE OS's FOCUS TOO. The Win16 handle decides where this host
             posts a keystroke, but the CARET belongs to the real control and only
             the real SetFocus creates one -- a window the OS has not focused is a
             window with no cursor blinking in it, however right our own table is. */
        if (w && w->hwnd32) SetFocus(w->hwnd32);
        wu_puts(note, notecap, &k, " (was 0x");
        wu_puthex(note, notecap, &k, prev, 4);
        wu_puts(note, notecap, &k, ") -- keyboard messages now go here");
        wow32_setret(f, prev);
        return 1;
    }

    /* ── ★★★★ 0x38 MoveWindow(hWnd, X, Y, nWidth, nHeight, bRepaint) ─────────
         THE call that makes Notepad usable: its window procedure answers every
         WM_SIZE by moving its EDIT control to fit, and with this unimplemented
         the control kept whatever size CreateWindow gave it -- which is why ours
         showed a stray scrollbar hard right and stock's edit control filled the
         frame. Straight through to the real one, because the window IS real.
       ⚠ COORDINATES ARE SIGNED. They arrive as WORDs and a window at x = -4 is
         ordinary (Windows positions a maximised frame slightly off-screen), so
         they are sign-extended rather than taken as unsigned -- otherwise a small
         negative becomes ~65000 and the control lands off the desktop.
       ⚠ NO CW_USEDEFAULT TRANSLATION HERE, deliberately: CW_USEDEFAULT is a
         CreateWindow convention and MoveWindow has no such value. 0x8000 is a
         legitimate (if large) coordinate to this call. */
    case WOWUSER_MOVEWINDOW: {
        WORD hwnd = wow32_argw(f, MW_ARG_HWND);
        int  x  = (int)(short)wow32_argw(f, MW_ARG_X);
        int  y  = (int)(short)wow32_argw(f, MW_ARG_Y);
        int  cx = (int)(short)wow32_argw(f, MW_ARG_CX);
        int  cy = (int)(short)wow32_argw(f, MW_ARG_CY);
        WORD rep = wow32_argw(f, MW_ARG_REPAINT);
        wowuser_win_t *w = wowuser_findwin(hwnd);
        int k = 0;
        wu_puts(note, notecap, &k, "MoveWindow 0x");
        wu_puthex(note, notecap, &k, hwnd, 4);
        wu_puts(note, notecap, &k, " to ("); wu_puthex(note, notecap, &k, (DWORD)x, 4);
        wu_puts(note, notecap, &k, ",");     wu_puthex(note, notecap, &k, (DWORD)y, 4);
        wu_puts(note, notecap, &k, ") ");    wu_puthex(note, notecap, &k, (DWORD)cx, 4);
        wu_puts(note, notecap, &k, "x");     wu_puthex(note, notecap, &k, (DWORD)cy, 4);
        if (!w) { wu_puts(note, notecap, &k, " -- NO SUCH WINDOW");
                  wow32_setret(f, 0); return 1; }
        w->x = x; w->y = y; w->cx = cx; w->cy = cy;
        if (w->hwnd32) {
            MoveWindow(w->hwnd32, x, y, cx, cy, rep ? TRUE : FALSE);
            wu_puts(note, notecap, &k, " -> the OS's");
        } else {
            wu_puts(note, notecap, &k, " -- no real window; recorded only");
        }
        wow32_setret(f, 1);
        return 1;
    }

    /* ── ★ 0x7d InvalidateRect(hWnd, lpRect, bErase) ─────────────────────────
         Notepad calls it on its EDIT control just before moving it. The rect is
         a FAR POINTER and NULL means "the whole client area" -- a real
         distinction, so a null pointer is passed through as NULL rather than
         turned into an empty rectangle, which would invalidate nothing.
       ⚠ THE Win16 RECT IS 8 BYTES, FOUR WORDS -- see WOW_RECT16_SIZE. Handing
         the guest's 8 bytes to Win32 as a RECT would read four LONGs, i.e. this
         rectangle and eight bytes of whatever follows it. */
    case WOWUSER_INVALIDATERECT: {
        WORD hwnd = wow32_argw(f, IR_ARG_HWND);
        WORD er   = wow32_argw(f, IR_ARG_ERASE);
        volatile BYTE *rp = wow32_argptr(f, IR_ARG_RECT);
        wowuser_win_t *w = wowuser_findwin(hwnd);
        RECT r; int k = 0, haver = 0;
        wu_puts(note, notecap, &k, "InvalidateRect 0x");
        wu_puthex(note, notecap, &k, hwnd, 4);
        if (rp) {
            r.left   = (LONG)(short)wow32_peekw(rp + 0);
            r.top    = (LONG)(short)wow32_peekw(rp + 2);
            r.right  = (LONG)(short)wow32_peekw(rp + 4);
            r.bottom = (LONG)(short)wow32_peekw(rp + 6);
            haver = 1;
            wu_puts(note, notecap, &k, " rect(");
            wu_puthex(note, notecap, &k, (DWORD)r.left, 4);  wu_puts(note, notecap, &k, ",");
            wu_puthex(note, notecap, &k, (DWORD)r.top, 4);   wu_puts(note, notecap, &k, ",");
            wu_puthex(note, notecap, &k, (DWORD)r.right, 4); wu_puts(note, notecap, &k, ",");
            wu_puthex(note, notecap, &k, (DWORD)r.bottom, 4);
            wu_puts(note, notecap, &k, ")");
        } else {
            wu_puts(note, notecap, &k, " whole client area (lpRect NULL)");
        }
        wu_puts(note, notecap, &k, er ? " erase" : " no erase");
        if (!w || !w->hwnd32) { wu_puts(note, notecap, &k, " -- no real window");
                                wow32_setret(f, 0); return 1; }
        InvalidateRect(w->hwnd32, haver ? &r : NULL, er ? TRUE : FALSE);
        wow32_setret(f, 1);
        return 1;
    }

    /* ── ★ 0xb3 GetSystemMetrics(nIndex) ─────────────────────────────────────
         Straight through to the OS, on the same claim the WS_* style bits and the
         predefined cursor ordinals are passed on: Win32 inherited the SM_*
         indices from Win16 unchanged.
       ⚠ THAT CLAIM IS NOT FREE HERE, and unlike a style bit a wrong metric does
         not fail visibly -- it lays a window out slightly wrong. So the index and
         the answer are BOTH logged on every call: if a guest's arithmetic ever
         looks wrong, the line says exactly what it was told. */
    case WOWUSER_GETSYSTEMMETRICS: {
        WORD idx = wow32_argw(f, GSM_ARG_INDEX);
        int  v   = GetSystemMetrics((int)idx);
        int  k = 0;
        wu_puts(note, notecap, &k, "GetSystemMetrics(0x");
        wu_puthex(note, notecap, &k, idx, 4);
        wu_puts(note, notecap, &k, ") = 0x");
        wu_puthex(note, notecap, &k, (DWORD)v, 4);
        wu_puts(note, notecap, &k, " (the OS's own, SM_* assumed common to Win16/32)");
        wow32_setret(f, (DWORD)(WORD)v);
        return 1;
    }

    /* ── ★ 0x1f IsIconic(hWnd) -- ask the real window. ───────────────────────
         Notepad asks before laying anything out, because a minimised window has
         no useful client area. Answering 0 unconditionally (what an unimplemented
         call did) is the "runs but lies" shape: it is the right answer most of the
         time, which is exactly why the wrong one would never be noticed. */
    case WOWUSER_ISICONIC: {
        WORD hwnd = wow32_argw(f, II_ARG_HWND);
        wowuser_win_t *w = wowuser_findwin(hwnd);
        int k = 0, ic = 0;
        wu_puts(note, notecap, &k, "IsIconic 0x");
        wu_puthex(note, notecap, &k, hwnd, 4);
        if (!w || !w->hwnd32) {
            wu_puts(note, notecap, &k, " -- no real window; answered 0");
            wow32_setret(f, 0);
            return 1;
        }
        ic = IsIconic(w->hwnd32) ? 1 : 0;
        wu_puts(note, notecap, &k, ic ? " -> MINIMISED" : " -> not minimised");
        wow32_setret(f, (DWORD)ic);
        return 1;
    }

    /* ── ★★★★ 0x01 MessageBox(hWnd, lpText, lpCaption, uType) ───────────────
         The real one, on the guest's own window. Win16 and Win32 agree on the
         MB_* bits and on the ID* return values, so the pass-through is exact --
         and unlike a metric or a style, a wrong answer here is impossible to
         miss, because the box is on the screen with the guest's own words in it.
       ★ The TEXT IS ALSO LOGGED, and that is half the point: a headless run
         cannot see a dialog, and this is how a program reports the failures it
         has already diagnosed for us. */
    case WOWUSER_MESSAGEBOX: {
        WORD hwnd = wow32_argw(f, MSGB_ARG_HWND);
        WORD type = wow32_argw(f, MSGB_ARG_TYPE);
        wowuser_win_t *w = hwnd ? wowuser_findwin(hwnd) : NULL;
        char text[512], cap[128];
        int k = 0, rc;
        wow32_argstr(f, MSGB_ARG_TEXT,    text, sizeof text);
        wow32_argstr(f, MSGB_ARG_CAPTION, cap,  sizeof cap);
        wu_puts(note, notecap, &k, "★ MessageBox ");
        wu_putq(note, notecap, &k, cap);
        wu_puts(note, notecap, &k, ": ");
        wu_putq(note, notecap, &k, text);
        wu_puts(note, notecap, &k, " type=0x");
        wu_puthex(note, notecap, &k, type, 4);
        wu_puts(note, notecap, &k, " -- ★ MODAL: the VDM stops until it is"
                                   " dismissed");
        rc = MessageBoxA(w ? w->hwnd32 : NULL, text, cap, (UINT)type);
        wu_puts(note, notecap, &k, "; answered 0x");
        wu_puthex(note, notecap, &k, (DWORD)rc, 4);
        wow32_setret(f, (DWORD)(WORD)rc);
        return 1;
    }

    /* ── ★★★★★ 0x42 GetDC(hWnd) / 0x43 GetWindowDC(hWnd) ────────────────────
         The call MS Paint stops on, and the first producer of a device context
         anywhere in this host. The real one, on the guest's real window: the DC
         a Win16 program draws through has to be a DC for the actual pixels on
         the actual desktop, and ours are real HWNDs (session 42).
       ★ THE ANSWER IS A TOKEN, NOT THE HDC. An HDC is 32 bits and the guest has
         16 to keep it in, and it travels back to us through ReleaseDC and every
         GDI call, so it goes through the same map the menus, icons and windows
         use. It is minted as WINDC, which is what makes a later `DeleteDC` on it
         refusable -- see wowgdi.h for why that distinction is not pedantry.
       ⚠ hWnd == NULL IS LEGAL AND MEANS THE SCREEN, so a null handle is passed
         through as NULL rather than rejected as "no such window". A guest that
         asks for the screen DC and is told no would be being lied to.
       ⚠ EVERY GetDC MUST BE MATCHED BY A ReleaseDC. Windows keeps a small cache
         of common DCs and a guest that leaks them will eventually be refused by
         the OS, not by us -- so the note carries the token and the count, and a
         run that stops painting can be read back to whichever call stopped
         returning one. */
    case WOWUSER_GETDC:
    case WOWUSER_GETWINDOWDC: {
        int  wantwin = (f->id == WOWUSER_GETWINDOWDC);
        WORD hwnd = wow32_argw(f, GDC_ARG_HWND16);
        wowuser_win_t *w = hwnd ? wowuser_findwin(hwnd) : NULL;
        HDC  dc;
        WORD tok;
        int  k = 0;
        wu_puts(note, notecap, &k, wantwin ? "GetWindowDC 0x" : "GetDC 0x");
        wu_puthex(note, notecap, &k, hwnd, 4);
        if (hwnd && (!w || !w->hwnd32)) {
            wu_puts(note, notecap, &k, " -- ★ NO SUCH WINDOW; answered 0 (a guest"
                                       " reads a null DC as out of memory)");
            wow32_setret(f, 0);
            return 1;
        }
        if (!hwnd) wu_puts(note, notecap, &k, " (the SCREEN)");
        dc = wantwin ? GetWindowDC(w ? w->hwnd32 : NULL)
                     : GetDC(w ? w->hwnd32 : NULL);
        if (!dc) {
            wu_puts(note, notecap, &k, " -- ★ THE OS REFUSED THE DC; answered 0");
            wow32_setret(f, 0);
            return 1;
        }
        tok = wowgdi_h16((HGDIOBJ)dc, WOWGDI_KIND_WINDC);
        if (!tok) {
            /* ⚠ Do not hand back a DC we cannot name later: it could never be
                 released, which is the leak this map exists to prevent.
                 GetWindowDC's result goes back through ReleaseDC too. */
            ReleaseDC(w ? w->hwnd32 : NULL, dc);
            wu_puts(note, notecap, &k, " -- ★ THE GDI TOKEN MAP IS FULL; the DC was"
                                       " given straight back and 0 answered");
            wow32_setret(f, 0);
            return 1;
        }
        wu_puts(note, notecap, &k, " -> DC token 0x");
        wu_puthex(note, notecap, &k, tok, 4);
        wow32_setret(f, tok);
        return 1;
    }

    /* ── ★★ 0x44 ReleaseDC(hWnd, hDC) ───────────────────────────────────────
       ⚠ 0x44 IS `DeleteDC` IN GDI'S TABLE. Same number, different call, which is
         why the dispatcher gates on the stub's segment before reaching here.
       ⚠ THE TOKEN IS FORGOTTEN AS WELL AS THE DC RELEASED -- a released DC goes
         straight back into the window's cache and will be handed out again, so a
         token left pointing at it would name somebody else's DC.
       ⚠ A DC THAT IS NOT BORROWED IS REFUSED rather than passed on: ReleaseDC on
         a CreateCompatibleDC result silently does nothing on Win32 and leaks it,
         and a guest doing that is worth seeing. */
    case WOWUSER_RELEASEDC: {
        WORD hdc  = wow32_argw(f, RDC_ARG_HDC);
        WORD hwnd = wow32_argw(f, RDC_ARG_HWND);
        wowuser_win_t *w = hwnd ? wowuser_findwin(hwnd) : NULL;
        int kind = -1;
        HGDIOBJ o = wowgdi_h32(hdc, &kind);
        int k = 0, ok;
        wu_puts(note, notecap, &k, "ReleaseDC dc=0x");
        wu_puthex(note, notecap, &k, hdc, 4);
        wu_puts(note, notecap, &k, " hwnd=0x");
        wu_puthex(note, notecap, &k, hwnd, 4);
        if (!o) {
            wu_puts(note, notecap, &k, " -- ★ NOT ONE OF OUR GDI TOKENS;"
                                       " answered 0");
            wow32_setret(f, 0);
            return 1;
        }
        if (kind != WOWGDI_KIND_WINDC) {
            wu_puts(note, notecap, &k, " -- ★ THAT DC WAS NOT BORROWED FROM A"
                                       " WINDOW; it must go through DeleteDC."
                                       " Refused");
            wow32_setret(f, 0);
            return 1;
        }
        ok = ReleaseDC(w ? w->hwnd32 : NULL, (HDC)o) ? 1 : 0;
        if (ok) wowgdi_forget(hdc);
        wu_puts(note, notecap, &k, ok ? " -> released, token freed"
                                      : " -- ★ the OS refused the release");
        wow32_setret(f, (DWORD)ok);
        return 1;
    }

    /* ── ★ 0x17 GetFocus() -- ask the OS, not our own bookkeeping. ───────────
         g_wm_focus is where this host POSTS a keystroke; the OS's focus is where
         one actually goes, and a caret only blinks in the second. They agree
         because SetFocus sets both, and if they ever disagree that is a defect
         worth seeing rather than papering over -- so the OS answers, and a
         mismatch is printed instead of being silently preferred either way. */
    case WOWUSER_GETFOCUS: {
        WORD h = wowwin_hwnd16(GetFocus());
        int k = 0;
        wu_puts(note, notecap, &k, "GetFocus -> 0x");
        wu_puthex(note, notecap, &k, h, 4);
        if (h != g_wm_focus) {
            wu_puts(note, notecap, &k, " -- ⚠ the OS says this and our queue says 0x");
            wu_puthex(note, notecap, &k, g_wm_focus, 4);
        }
        wow32_setret(f, h);
        return 1;
    }

    /* ── ★ 0x22 EnableWindow(hWnd, bEnable) ─────────────────────────────────
         Notepad disables its window while a modal thing is up. Straight through:
         a disabled real window stops taking real input, which is the whole
         behaviour being asked for and not something this host could imitate. */
    case WOWUSER_ENABLEWINDOW: {
        WORD hwnd = wow32_argw(f, EW_ARG_HWND);
        WORD en   = wow32_argw(f, EW_ARG_ENABLE);
        wowuser_win_t *w = wowuser_findwin(hwnd);
        int k = 0;
        wu_puts(note, notecap, &k, en ? "EnableWindow ENABLE 0x" : "EnableWindow DISABLE 0x");
        wu_puthex(note, notecap, &k, hwnd, 4);
        if (!w || !w->hwnd32) { wu_puts(note, notecap, &k, " -- no real window");
                                wow32_setret(f, 0); return 1; }
        wow32_setret(f, (DWORD)(EnableWindow(w->hwnd32, en ? TRUE : FALSE) ? 1 : 0));
        return 1;
    }

    /* ── ★★ 0x35 DestroyWindow(hWnd) ────────────────────────────────────────
       ⚠ THE SLOT MUST BE RELEASED, and that is the whole reason this is not a
         one-liner. Destroying the real window while leaving our record pointing
         at it leaves a dangling HWND that every later lookup would hand to
         Win32 -- and a destroyed HWND is not merely invalid, it can be REUSED,
         so the failure would not even be a clean one. Clearing `hwnd` frees the
         slot and makes a later reference fail honestly as "NO SUCH WINDOW".
       ⚠ The real DestroyWindow destroys child windows too, so their Win16
         records are stale the moment this returns. They are cleared here rather
         than left for whoever notices, and the count is logged. */
    case WOWUSER_DESTROYWINDOW: {
        WORD hwnd = wow32_argw(f, DW_ARG_HWND);
        wowuser_win_t *w = wowuser_findwin(hwnd);
        int k = 0, i, kids = 0;
        HWND h32;
        wu_puts(note, notecap, &k, "DestroyWindow 0x");
        wu_puthex(note, notecap, &k, hwnd, 4);
        if (!w) { wu_puts(note, notecap, &k, " -- NO SUCH WINDOW");
                  wow32_setret(f, 0); return 1; }
        h32 = w->hwnd32;
        for (i = 0; i < g_wu_nwin; ++i) {
            wowuser_win_t *c = &g_wu_win[i];
            if (c->hwnd && c != w && c->hwnd32 && h32 && IsChild(h32, c->hwnd32)) {
                c->hwnd = 0; c->hwnd32 = NULL; ++kids;
            }
        }
        w->hwnd = 0; w->hwnd32 = NULL;
        if (g_wm_focus == hwnd) g_wm_focus = 0;
        if (h32) DestroyWindow(h32);
        wu_puts(note, notecap, &k, " -> destroyed");
        if (kids) { wu_puts(note, notecap, &k, ", with 0x");
                    wu_puthex(note, notecap, &k, (DWORD)kids, 2);
                    wu_puts(note, notecap, &k, " child record(s) released too"); }
        wow32_setret(f, 1);
        return 1;
    }

    /* ── ★ 0x3b SetActiveWindow(hWnd) -- returns the PREVIOUS active window. ── */
    case WOWUSER_SETACTIVEWINDOW: {
        WORD hwnd = wow32_argw(f, SAW_ARG_HWND);
        wowuser_win_t *w = wowuser_findwin(hwnd);
        int k = 0;
        WORD prev = 0;
        wu_puts(note, notecap, &k, "SetActiveWindow 0x");
        wu_puthex(note, notecap, &k, hwnd, 4);
        if (!w || !w->hwnd32) { wu_puts(note, notecap, &k, " -- no real window");
                               wow32_setret(f, 0); return 1; }
        prev = wowwin_hwnd16(SetActiveWindow(w->hwnd32));
        wu_puts(note, notecap, &k, " (was 0x");
        wu_puthex(note, notecap, &k, prev, 4);
        wu_puts(note, notecap, &k, ")");
        wow32_setret(f, prev);
        return 1;
    }

    /* ── ★ 0x68 MessageBeep(uType) ──────────────────────────────────────────
       ⚠ Win16's only documented argument is 0 and Win32's MB_OK is also 0, so
         the pass-through is exact for the one value a Win16 program can pass.
         Notepad beeps at a failed search. */
    case WOWUSER_MESSAGEBEEP: {
        WORD t = wow32_argw(f, MB_ARG_TYPE);
        int k = 0;
        wu_puts(note, notecap, &k, "MessageBeep(0x");
        wu_puthex(note, notecap, &k, t, 4);
        wu_puts(note, notecap, &k, ")");
        MessageBeep((UINT)t);
        wow32_setret(f, 0);
        return 1;
    }

    /* ── ★★ THE CLIPBOARD -- 0x89 / 0x8a / 0x90 ─────────────────────────────
         The REAL clipboard, deliberately: this host already registers the
         guest's private formats in the OS's own atom table (0x91
         RegisterClipboardFormat), so a format number the guest holds IS a Win32
         format number and the two halves cannot disagree. A private clipboard
         here would be a second, invisible one that never talked to anything.
       ★ It also means Win16 Notepad and Win32 programs share a clipboard, which
         is what a user would expect of a program on this desktop and is what
         real WOW does. */
    case WOWUSER_OPENCLIPBOARD: {
        WORD hwnd = wow32_argw(f, OC_ARG_HWND);
        wowuser_win_t *w = hwnd ? wowuser_findwin(hwnd) : NULL;
        int k = 0, ok;
        ok = OpenClipboard(w ? w->hwnd32 : NULL) ? 1 : 0;
        wu_puts(note, notecap, &k, "OpenClipboard owner=0x");
        wu_puthex(note, notecap, &k, hwnd, 4);
        wu_puts(note, notecap, &k, ok ? " -> opened (the OS's own)"
                                      : " -> REFUSED (someone else has it open)");
        wow32_setret(f, (DWORD)ok);
        return 1;
    }

    case WOWUSER_CLOSECLIPBOARD: {
        int k = 0;
        int ok = CloseClipboard() ? 1 : 0;
        wu_puts(note, notecap, &k, ok ? "CloseClipboard -> closed"
                                      : "CloseClipboard -> it was not open");
        wow32_setret(f, (DWORD)ok);
        return 1;
    }

    /* Enumerate from `wFormat`, 0 to start. Returns 0 at the end, which is the
       loop's termination condition, so a wrong answer here spins a guest. */
    case WOWUSER_ENUMCLIPFMT: {
        WORD fmt = wow32_argw(f, ECF_ARG_FORMAT);
        UINT nxt = EnumClipboardFormats((UINT)fmt);
        int k = 0;
        wu_puts(note, notecap, &k, "EnumClipboardFormats(0x");
        wu_puthex(note, notecap, &k, fmt, 4);
        wu_puts(note, notecap, &k, ") -> 0x");
        wu_puthex(note, notecap, &k, (DWORD)nxt, 4);
        if (!nxt) wu_puts(note, notecap, &k, " (end)");
        wow32_setret(f, (DWORD)(WORD)nxt);
        return 1;
    }

    /* ── ★★★ EnableMenuItem / CheckMenuItem -- WHERE THE GREYING LANDS. ─────
         Without these the run looked finished: GetMenu and GetSubMenu answered,
         USER's own 16-bit code ran, no error anywhere -- and every item in the
         menu stayed enabled, because the calls that change one were being
         stepped over. A menu that renders is not a menu that is RIGHT.
       ⚠ MF_* ARE THE SAME VALUES IN BOTH (MF_BYCOMMAND 0, MF_BYPOSITION 0x400,
         MF_GRAYED 1, MF_DISABLED 2, MF_CHECKED 8), so the flags go straight
         across -- but they are LOGGED, because MF_BYCOMMAND vs MF_BYPOSITION
         decides whether the second argument is an id or an index, and getting
         that wrong greys the wrong line rather than failing. */
    case WOWUSER_ENABLEMENUITEM:
    case WOWUSER_CHECKMENUITEM: {
        int   chk  = (f->id == WOWUSER_CHECKMENUITEM);
        WORD  hm   = wow32_argw(f, MI_ARG_HMENU);
        WORD  id   = wow32_argw(f, MI_ARG_ID);
        WORD  fl   = wow32_argw(f, MI_ARG_FLAGS);
        HMENU m    = wowuser_menu32(hm);
        int k = 0;
        DWORD prev;
        wu_puts(note, notecap, &k, chk ? "CheckMenuItem 0x" : "EnableMenuItem 0x");
        wu_puthex(note, notecap, &k, hm, 4);
        wu_puts(note, notecap, &k, (fl & 0x400) ? " byPOSITION " : " byCOMMAND ");
        wu_puthex(note, notecap, &k, id, 4);
        wu_puts(note, notecap, &k, " flags=0x");
        wu_puthex(note, notecap, &k, fl, 4);
        if (!m) {
            wu_puts(note, notecap, &k, " -- ★ NOT ONE OF OUR MENU TOKENS;"
                                       " answered -1");
            wow32_setret(f, 0xFFFFFFFF);   /* Win16's "no such item" */
            return 1;
        }
        prev = chk ? (DWORD)CheckMenuItem(m, (UINT)id, (UINT)fl)
                   : (DWORD)EnableMenuItem(m, (UINT)id, (UINT)fl);
        wu_puts(note, notecap, &k, " -> previous state 0x");
        wu_puthex(note, notecap, &k, prev, 4);
        wow32_setret(f, prev);
        return 1;
    }

    /* ── ★★ THE MENU TRIO. Real menus, named by tokens. ─────────────────────
         Notepad greys and checks its own menu items (Edit > Undo, Word Wrap),
         and to do that it first has to GET the menu. It reads its own window's
         menu bar, walks into a popup, and works on that -- so all three are the
         same operation with the real HMENU behind a token. */
    case WOWUSER_GETMENU: {
        WORD hwnd = wow32_argw(f, GM2_ARG_HWND);
        wowuser_win_t *w = wowuser_findwin(hwnd);
        WORD t = 0;
        int k = 0;
        wu_puts(note, notecap, &k, "GetMenu 0x");
        wu_puthex(note, notecap, &k, hwnd, 4);
        if (!w || !w->hwnd32) { wu_puts(note, notecap, &k, " -- no real window");
                                wow32_setret(f, 0); return 1; }
        t = wowuser_menu16(GetMenu(w->hwnd32));
        wu_puts(note, notecap, &k, t ? " -> token 0x" : " -- NO MENU (or no token"
                                                        " left) 0x");
        wu_puthex(note, notecap, &k, t, 4);
        wow32_setret(f, t);
        return 1;
    }

    case WOWUSER_GETSUBMENU: {
        WORD hm  = wow32_argw(f, GSM2_ARG_HMENU);
        WORD pos = wow32_argw(f, GSM2_ARG_POS);
        HMENU m  = wowuser_menu32(hm);
        WORD t = 0;
        int k = 0;
        wu_puts(note, notecap, &k, "GetSubMenu 0x");
        wu_puthex(note, notecap, &k, hm, 4);
        wu_puts(note, notecap, &k, " pos 0x");
        wu_puthex(note, notecap, &k, pos, 4);
        if (!m) { wu_puts(note, notecap, &k, " -- ★ NOT ONE OF OUR MENU TOKENS");
                  wow32_setret(f, 0); return 1; }
        t = wowuser_menu16(GetSubMenu(m, (int)(short)pos));
        wu_puts(note, notecap, &k, " -> token 0x");
        wu_puthex(note, notecap, &k, t, 4);
        wow32_setret(f, t);
        return 1;
    }

    /* ⚠ bRevert TRUE DESTROYS the application's copy and rebuilds the default,
         which is a real side effect and not a query -- passed through as given,
         and logged, because a guest that passes TRUE by accident would otherwise
         lose its own system-menu customisations invisibly. */
    case WOWUSER_GETSYSTEMMENU: {
        WORD hwnd = wow32_argw(f, GSYM_ARG_HWND);
        WORD rev  = wow32_argw(f, GSYM_ARG_REVERT);
        wowuser_win_t *w = wowuser_findwin(hwnd);
        WORD t = 0;
        int k = 0;
        wu_puts(note, notecap, &k, "GetSystemMenu 0x");
        wu_puthex(note, notecap, &k, hwnd, 4);
        wu_puts(note, notecap, &k, rev ? " REVERT (rebuilds the default menu)"
                                       : " (query)");
        if (!w || !w->hwnd32) { wu_puts(note, notecap, &k, " -- no real window");
                                wow32_setret(f, 0); return 1; }
        t = wowuser_menu16(GetSystemMenu(w->hwnd32, rev ? TRUE : FALSE));
        wu_puts(note, notecap, &k, " -> token 0x");
        wu_puthex(note, notecap, &k, t, 4);
        wow32_setret(f, t);
        return 1;
    }

    /* ── ★★ THE DIALOG-ITEM HELPERS. ────────────────────────────────────────
         Every one of these is "find the child control with this id and do
         something to it", and the child IS a real Win32 window -- USER's own
         16-bit DialogBox builds a dialog by calling CreateWindow, which comes
         through this host, so the controls are ours and the OS can find them by
         id exactly as it would for any dialog.
       ⚠ The Win16 handle for a control that came back from the OS is looked up
         rather than invented; a control this host did not create yields 0 and
         says so. */
    case WOWUSER_GETDLGITEM: {
        WORD hdlg = wow32_argw(f, GDI2_ARG_HDLG);
        WORD id   = wow32_argw(f, GDI2_ARG_ID);
        wowuser_win_t *w = wowuser_findwin(hdlg);
        WORD h16 = 0;
        int k = 0;
        wu_puts(note, notecap, &k, "GetDlgItem dlg 0x");
        wu_puthex(note, notecap, &k, hdlg, 4);
        wu_puts(note, notecap, &k, " id 0x");
        wu_puthex(note, notecap, &k, id, 4);
        if (!w || !w->hwnd32) { wu_puts(note, notecap, &k, " -- no real window");
                                wow32_setret(f, 0); return 1; }
        h16 = wowwin_hwnd16(GetDlgItem(w->hwnd32, (int)(short)id));
        wu_puts(note, notecap, &k, h16 ? " -> 0x" : " -- NOT FOUND (or not a window"
                                                    " this host made) 0x");
        wu_puthex(note, notecap, &k, h16, 4);
        wow32_setret(f, h16);
        return 1;
    }

    case WOWUSER_GETDLGITEMTEXT: {
        WORD hdlg = wow32_argw(f, GDIT_ARG_HDLG);
        WORD id   = wow32_argw(f, GDIT_ARG_ID);
        WORD cap  = wow32_argw(f, GDIT_ARG_MAX);
        volatile BYTE *dst = wow32_argptr(f, GDIT_ARG_BUF);
        wowuser_win_t *w = wowuser_findwin(hdlg);
        UINT n = 0;
        int k = 0;
        wu_puts(note, notecap, &k, "GetDlgItemText dlg 0x");
        wu_puthex(note, notecap, &k, hdlg, 4);
        wu_puts(note, notecap, &k, " id 0x");
        wu_puthex(note, notecap, &k, id, 4);
        if (!w || !w->hwnd32 || !dst || !cap) {
            wu_puts(note, notecap, &k, " -- no window or no buffer; answered 0");
            wow32_setret(f, 0); return 1;
        }
        /* ⚠ `cap` is the caller's claim about its own buffer and the only bound
             there is -- handed to the OS, which respects it. */
        n = GetDlgItemTextA(w->hwnd32, (int)(short)id, (LPSTR)dst, (int)cap);
        wu_puts(note, notecap, &k, " -> ");
        wu_putq(note, notecap, &k, (const char *)dst);
        wow32_setret(f, (DWORD)(WORD)n);
        return 1;
    }

    case WOWUSER_SETDLGITEMINT: {
        WORD hdlg = wow32_argw(f, SDII_ARG_HDLG);
        WORD id   = wow32_argw(f, SDII_ARG_ID);
        WORD val  = wow32_argw(f, SDII_ARG_VALUE);
        WORD sgn  = wow32_argw(f, SDII_ARG_SIGNED);
        wowuser_win_t *w = wowuser_findwin(hdlg);
        int k = 0;
        wu_puts(note, notecap, &k, "SetDlgItemInt dlg 0x");
        wu_puthex(note, notecap, &k, hdlg, 4);
        wu_puts(note, notecap, &k, " id 0x");
        wu_puthex(note, notecap, &k, id, 4);
        wu_puts(note, notecap, &k, " = 0x");
        wu_puthex(note, notecap, &k, val, 4);
        if (!w || !w->hwnd32) { wu_puts(note, notecap, &k, " -- no real window");
                                wow32_setret(f, 0); return 1; }
        /* ⚠ SIGNEDNESS IS THE CALLER'S, and it changes the text: -1 or 65535.
             The Win16 value is a WORD, so it is widened the way the caller says
             rather than the way C would. */
        SetDlgItemInt(w->hwnd32, (int)(short)id,
                      sgn ? (UINT)(int)(short)val : (UINT)val,
                      sgn ? TRUE : FALSE);
        wow32_setret(f, 0);
        return 1;
    }

    case WOWUSER_SENDDLGITEMMSG: {
        WORD hdlg = wow32_argw(f, SDIM_ARG_HDLG);
        WORD id   = wow32_argw(f, SDIM_ARG_ID);
        WORD m    = wow32_argw(f, SDIM_ARG_MSG);
        WORD wp   = wow32_argw(f, SDIM_ARG_WPARAM);
        DWORD lp  = wow32_argd(f, SDIM_ARG_LPARAM);
        wowuser_win_t *w = wowuser_findwin(hdlg);
        WORD ch16 = 0;
        int k = 0;
        wu_puts(note, notecap, &k, "SendDlgItemMessage dlg 0x");
        wu_puthex(note, notecap, &k, hdlg, 4);
        wu_puts(note, notecap, &k, " id 0x");
        wu_puthex(note, notecap, &k, id, 4);
        wu_puts(note, notecap, &k, " msg 0x");
        wu_puthex(note, notecap, &k, m, 4);
        if (!w || !w->hwnd32) { wu_puts(note, notecap, &k, " -- no real window");
                                wow32_setret(f, 0); return 1; }
        /* ★ ROUTED THROUGH THIS HOST'S OWN SendMessage PATH, not straight to
             Win32: the control may be one whose window procedure is the GUEST'S,
             and it may carry a 16:16 pointer that Win32 must never see. Turning
             it into (hwnd16, msg, wParam, lParam) and reusing the machinery that
             already decides between those two worlds is the only answer that is
             right in both. */
        ch16 = wowwin_hwnd16(GetDlgItem(w->hwnd32, (int)(short)id));
        if (!ch16) {
            wu_puts(note, notecap, &k, " -- NO SUCH ITEM; answered 0");
            wow32_setret(f, 0);
            return 1;
        }
        wu_puts(note, notecap, &k, " -> control 0x");
        wu_puthex(note, notecap, &k, ch16, 4);
        {   wowuser_win_t *cw = wowuser_findwin(ch16);
            if (!cw) { wu_puts(note, notecap, &k, " -- not in our table");
                       wow32_setret(f, 0); return 1; }
            if (cw->wndproc) {
                if (!f->cbok) { wu_puts(note, notecap, &k, " -- its procedure is"
                                                           " 16-bit and callbacks"
                                                           " are off");
                                wow32_setret(f, 0); return 1; }
                wu_puts(note, notecap, &k, " -> its own window procedure");
                wow32_setret(f, 0);
                wowuser_want_msg(f, cw,
                                 cw->hinst ? cw->hinst : g_wu_class[cw->cls].hinst,
                                 m, wp, lp, WOWCALL_RET_RESULT);
                return 1;
            }
            wow32_setret(f, (DWORD)wowuser_defproc(f, cw, m, wp, lp,
                                                   note, notecap));
            return 1;
        }
    }

    /* ── ★★ EndDialog(hDlg, nResult) ────────────────────────────────────────
       ⚠⚠ WHAT ENDS HERE IS THE WINDOW, AND POSSIBLY NOT THE LOOP. USER's
         `DialogBox` is 16-bit code (entry seg1:0x208e) and runs its own modal
         message loop; on real WOW this call tells the 32-bit side to end a real
         dialog and the loop notices. Our "dialog" is a plain window that USER
         built by calling CreateWindow through this host, so Win32's EndDialog
         has nothing to end -- it is called anyway, because if the window ever IS
         a real dialog that is the correct thing, and its failure is reported
         rather than hidden. The window is then hidden so the user is not left
         looking at a dead dialog.
       ⇒ IF A RUN SHOWS USER'S LOOP SPINNING AFTER THIS, that is the measurement
         that says how the loop learns it is over, and it will be in the log. */
    case WOWUSER_ENDDIALOG: {
        WORD hdlg = wow32_argw(f, ED_ARG_HDLG);
        WORD res  = wow32_argw(f, ED_ARG_RESULT);
        wowuser_win_t *w = wowuser_findwin(hdlg);
        int k = 0, ok = 0;
        wu_puts(note, notecap, &k, "EndDialog 0x");
        wu_puthex(note, notecap, &k, hdlg, 4);
        wu_puts(note, notecap, &k, " result 0x");
        wu_puthex(note, notecap, &k, res, 4);
        if (!w || !w->hwnd32) { wu_puts(note, notecap, &k, " -- no real window");
                                wow32_setret(f, 0); return 1; }
        ok = EndDialog(w->hwnd32, (INT_PTR)(short)res) ? 1 : 0;
        if (!ok) {
            ShowWindow(w->hwnd32, SW_HIDE);
            wu_puts(note, notecap, &k, " -- Win32 EndDialog refused it (this is a"
                                       " window, not a real dialog); HIDDEN"
                                       " instead. ★ If USER's own modal loop keeps"
                                       " spinning, THAT is the next thing to read");
        } else {
            wu_puts(note, notecap, &k, " -> ended");
        }
        wow32_setret(f, 1);
        return 1;
    }

    default:
        return 0;
    }
}

#endif /* WOWUSER_H */
