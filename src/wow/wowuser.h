#ifndef WOWUSER_H
#define WOWUSER_H
#include "wowconv.h"   /* the Win16/Win32 semantic deltas, pinned by tests/unit/wow_test.c */
/*
 * wowuser.h -- USER.EXE's half of the WOW32 interface. GH #128, session 38.
 *
 * ── WHY THIS IS A SEPARATE FILE AND NOT MORE CASES IN wow32.h ────────────────
 * THE ID SPACE IS PER MODULE. Every id in wow32.h is one krnl386 sends; USER
 * sends its own 457 with its own numbering, and the numbers COLLIDE. `0x39` is `GetProfileInt` in krnl386's table and
 * `RegisterClass` in USER's -- and for one run this host serviced the second with
 * the first and handed WOWEXEC the answer. Two id spaces in one switch is how that
 * happens; two files with two dispatchers, chosen by the stub's own segment, is how
 * it stops happening.
 *
 * The surface is enumerated in docs/research/wow-user-surface.md -- 441 ids, 262 of
 * them named by USER's own export table, regenerable with
 *     tools/ne/wowmap.py guest/ne/user.exe --md
 *
 * ── THE Win16 WNDCLASS (documented; checked against what WOWEXEC passes) ─────
 * 26 bytes. The block WOWEXEC hands RegisterClass reads back sensibly at every
 * offset -- hCursor is what LoadCursor returned, hbrBackground a stock object,
 * lpszMenuName NULL, lpszClassName a far pointer into its own DGROUP:
 *
 *   +0x00 WORD  style          +0x0c WORD hIcon
 *   +0x02 DWORD lpfnWndProc    +0x0e WORD hCursor
 *   +0x06 WORD  cbClsExtra     +0x10 WORD hbrBackground
 *   +0x08 WORD  cbWndExtra     +0x12 DWORD lpszMenuName
 *   +0x0a WORD  hInstance      +0x16 DWORD lpszClassName
 *
 * ⚠ The 0x82 seen at +0x16 is the CLASS NAME's OFFSET, not a style. An earlier
 *   note read it as "style 0x82, hInstance=ds"; the 26-byte layout is what
 *   settles it.
 */

#define WOWUSER_REGISTERCLASS   0x39
#define WOWUSER_CREATEWINDOW    0x29
/* ── ★★★★★ 0xEF -- THE DIALOG HELPER, AND NO EXPORT MAPS TO IT. ──────────────
     USER's wow32 id space is derived from its export table, and this id is in
     one of the GAPS: `CreateDialog`, `DialogBoxParam` and `CreateDialogParam`
     are implemented in USER's OWN 16-bit code, so `neneeds.py` classifies them
     `native16` and reports them as costing us nothing. THEY DO NOT. That
     16-bit code does the FindResource/LoadResource/LockResource itself and then
     calls OUT to the 32-bit side -- here -- to build the window and its
     controls, because under WOW the windows are the 32-bit side's.
   ⇒ `native16` IS NOT `free`, and this id is the proof: the tool said CALC had
     0 services to do, and CALC produced no window at all. (session 55) */
#define WOWUSER_CREATEDIALOG    0xEF
#define WOWUSER_NOTIFYWOW       0x217
/* ── ★★★ 0x16c LookupIconIdFromDirectoryEx(lpDir, fIcon, cx, cy, flags) = 12 ──
     (s89, #216) Observed arriving inside every guest LoadCursor/LoadIcon,
     between the GROUP resource being loaded and the entry it names being
     looked up (fIcon 0 from LoadCursor, 1 from LoadIcon; both cx = cy = 0,
     flags 0x40 = LR_DEFAULTSIZE). Stepped over, it answered 0, FindResource(hInst, 0,
     RT_CURSOR) failed and LoadCursor returned NULL for EVERY cursor a program
     ships -- Paintbrush's zoom rectangle among them. The group directory is the
     same 6-byte header + 14-byte entries in Win16 and Win32, so the bytes go to
     the OS's own LookupIconIdFromDirectoryEx as they are. Frame, reversed:
     +0 flags, +2 cy, +4 cx, +6 fIcon, +8 the directory, far. */
#define WOWUSER_LOOKUPICONID    0x16c
#define WOWUSER_LII_ARG_FLAGS           0
#define WOWUSER_LII_ARG_CY              2
#define WOWUSER_LII_ARG_CX              4
#define WOWUSER_LII_ARG_FICON           6
#define WOWUSER_LII_ARG_DIR             8
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
   unnamed ids, and they are named here by where they arrive from in SYSEDIT's
   message loop, matched against its NE import relocations. See wowmsg.h. */
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
#define WOWUSER_RCF_ARG_NAME             0

/* ── ★★★ 0x76 RegisterWindowMessage(lpString) ────────────────────────────────
     Another of the ids USER's export table cannot name, and NOTEPAD names it by
     what it passes: it arrives twice at start-up, with the strings
     "commdlg_FindReplace" and "commdlg_help" -- the two message names the common
     dialogs are documented to register. Answered 0, Notepad abandons its whole
     initialisation (observed): it shows its window and then throws it away, for
     want of a message id.
   ★ THE ANSWER IS THE OS's, for the same reason RegisterClipboardFormat's is: a
     registered window message is a name in a SYSTEM-WIDE atom table, and its
     whole purpose is that two programs which register the same string get the
     same number. Our own table would agree with nothing. */
#define WOWUSER_REGWINMSG        0x76
#define WOWUSER_RWM_ARG_NAME             0

/* SetWindowText(hWnd, lpString) -- 6 bytes, reversed: +0/+2 the string's far
   pointer, +4 the window. Without it a window's caption stays whatever
   CreateWindow was given, which for NOTEPAD is the empty string -- so its title
   bar and its taskbar button are blank and it looks like a window with no name
   rather than a program that has not been told to say one. */
#define WOWUSER_SETWINDOWTEXT    0x25
#define WOWUSER_SWT_ARG_TEXT             0
#define WOWUSER_SWT_ARG_HWND             4

/* ── ★★★ 0xad -- "BUILD ME A CURSOR OR AN ICON". ─────────────────────────────
     One of the 56 ids USER's export table cannot name, and the reason it matters
     is NOTEPAD: it calls `LoadCursor(NULL, 0x7f02)` at start-up, and if the
     answer is 0 it abandons its whole initialisation and WinMain returns without
     ever registering a class (observed). No cursor, no Notepad.

     USER's 16-bit LoadCursor/LoadIcon do the resource lookup themselves and then
     ask the 32-bit side, through this id, to build the object. For a NULL
     hInstance the block is (reversed, as logged):

         +18  0                 (the hInstance -- NULL)
         +16  lpName HIGH word  (0 => MAKEINTRESOURCE)
         +14  lpName LOW word   = the ORDINAL
         +12..+4  0
         +2   a version word
         +0   ★ KIND

     Notepad's call arrives as `(1 0 0 0 0 0 0 0x7f02 0 0)`. A call with kind 3
     and a real resource pointer -- a module's OWN icon -- was NOT answered here at
     first; the log names the kind so a run can say what it carries instead of
     this being widened by guesswork.

   ★★ WHY THE HOST DOES NOT HAVE TO KNOW WHETHER IT IS A CURSOR OR AN ICON.
     Win16's predefined cursor and icon ordinals share one numeric range, and both
     LoadCursor and LoadIcon reach this same id -- and the GUEST says which it is
     a moment later, when it puts the handle into
     `WNDCLASS.hCursor` or `WNDCLASS.hIcon`. So this returns a TOKEN that remembers
     the ordinal, and the real `LoadCursorA`/`LoadIconA` happens at the point of
     use, where the answer is not a guess. Same shape as every other handle here:
     synthetic to the guest, a real OS object behind it. */
#define WOWUSER_LOADSYSOBJ       0xad
#define WOWUSER_AD_ARG_KIND              0
#define WOWUSER_AD_ARG_NAMELO            14
#define WOWUSER_AD_ARG_NAMEHI            16
#define WOWUSER_AD_KIND_PREDEFINED       1
#define WOWUSER_AD_KIND_MODULERES        3
/* ── ★★ s89 (#216): WHAT `kind` ACTUALLY IS. It arrives as 1 from every
     LoadCursor -- NULL hInstance or a module's own -- and as 3 from every
     LoadIcon. So `kind` is CURSOR (1) or ICON (3), and whether the object is a
     PREDEFINED one or the MODULE's own is said by the hInstance at +18, which
     arrives as 0 exactly when no module resolved the request. The token keeps the two names above for what they have always meant
     (predefined / module), now derived from hInstance; the module arm also
     passes the RT_CURSOR bytes it has just locked:
       +2 GetExpWinVer   +4 hResData   +6 SizeofResource (DWORD)
       +10 LockResource (far)   +14/+16 lpName   +18 hInstance */
#define WOWUSER_AD_KIND_CURSOR           1
#define WOWUSER_AD_KIND_ICON             3
#define WOWUSER_AD_ARG_SIZE              6
#define WOWUSER_AD_ARG_BITS              10
#define WOWUSER_AD_ARG_HINST             18
/* ── ★ A FOURTH KIND: AN ICON THAT IS ALREADY A REAL OBJECT. (session 57) ────
     The two kinds above are both LAZY -- the token carries an ordinal or a name
     and the resolver goes and gets the image when somebody uses it, because
     until then we do not know whether the guest means a cursor or an icon. That
     cannot describe SHELL's ExtractIcon, whose whole job is to reach into
     ANOTHER FILE and come back with an image: there is no ordinal in this
     module's resources to remember, and no name either. So this kind carries the
     HICON itself, and the resolver hands it straight back. */
#define WOWUSER_AD_KIND_REALICON         4

/* ── ★★★★★ 0xaf -- "BUILD ME A BITMAP FROM THESE RESOURCE BYTES". ────────────
     MS Paint's second-to-last wall: with the registration, the DCs and the
     CREATESTRUCT in place it gets as far as building its toolbox, fails to load
     the bitmap for it, and says "Not enough memory to edit image."

     Another id USER's export table cannot name. It arrives with 14 argument
     bytes, laid out (reversed, as logged) as:

         +12  hInstance
         +8   lpszName, far  (SEG at +10, OFF at +8)
         +4   the resource's BYTES, far
         +0   its SIZE, a DWORD

   ★★★ AND IT IS `LoadBitmap`, PROVEN THREE WAYS RATHER THAN INFERRED:
     1. It arrives exactly when the guest calls `USER.175 LOADBITMAP` (an
        export that does not thunk by itself -- `native16`), whose documented
        parameters are (HINSTANCE, LPCSTR) -- the hInstance and name above.
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
#define WOWUSER_LBM_ARG_SIZE     0
#define WOWUSER_LBM_ARG_BITS     4
#define WOWUSER_LBM_ARG_NAME     8
#define WOWUSER_LBM_ARG_HINST   12

/* Values every part of USER below shares. */
#define WOWUSER_LOWER_TO_UPPER       32      /* 'a'..'z' minus this is 'A'..'Z'          */
#define WOWUSER_SHORT_NAME_SIZE      32      /* a resource or property name               */
#define WOWUSER_NAME_SIZE            64      /* a class name, a menu name, a window title */
#define WOWUSER_CLASS32_SIZE         96      /* the real Win32 class name behind a class  */
#define WOWUSER_FULL_CLASS_NAME_SIZE 160     /* WOWWIN_CLASS_PREFIX + a guest class name  */
#define WOWUSER_TASK_NONE16          0xFFFF  /* krnl386's current-task word: no task     */
#define WOWUSER_KRNL_CURRENT_TASK    0x228   /* that word's offset in krnl386's DGROUP    */
#define WOW_TDB_INSTANCE             0x1C    /* a task database's hInstance (InitTask writes it) */
#define WOW_INSTANCE_FROM_SELECTOR   0xFFFE  /* a task's SS with the low bit clear = its hInstance */
#define WOWUSER_MINUS_ONE16          0xFFFF  /* -1 as a Win16 WORD argument               */
#define WOWUSER_WNDPROC_ARGUMENTS    5       /* hwnd, msg, wParam, lParam high, low       */
#define WOWUSER_WNDPROC_ARG_LPARAM   3       /* lParam's high word: a blob's far pointer  */
/* The Win16 owner-draw structures WM_DRAWITEM / MEASUREITEM / DELETEITEM / COMPAREITEM carry
   (all WORD-sized handles and ids; itemState keeps only the first five ODS_ bits). */
#define WOWUSER_OWNERDRAW16_CTLTYPE        0
#define WOWUSER_OWNERDRAW16_CTLID          2
#define WOWUSER_DRAWITEM16_ITEMID          4
#define WOWUSER_DRAWITEM16_ITEMACTION      6
#define WOWUSER_DRAWITEM16_ITEMSTATE       8
#define WOWUSER_DRAWITEM16_HWNDITEM        10
#define WOWUSER_DRAWITEM16_HDC             12
#define WOWUSER_DRAWITEM16_RCITEM_LEFT     14
#define WOWUSER_DRAWITEM16_RCITEM_TOP      16
#define WOWUSER_DRAWITEM16_RCITEM_RIGHT    18
#define WOWUSER_DRAWITEM16_RCITEM_BOTTOM   20
#define WOWUSER_DRAWITEM16_ITEMDATA        22
#define WOWUSER_DRAWITEM16_SIZE            26
#define WOWUSER_ODS16_MASK                 0x1F
#define WOWUSER_MEASUREITEM16_ITEMID       4
#define WOWUSER_MEASUREITEM16_ITEMWIDTH    6
#define WOWUSER_MEASUREITEM16_ITEMHEIGHT   8
#define WOWUSER_MEASUREITEM16_ITEMDATA     10
#define WOWUSER_MEASUREITEM16_SIZE         14
#define WOWUSER_DELETEITEM16_ITEMID        4
#define WOWUSER_DELETEITEM16_HWNDITEM      6
#define WOWUSER_DELETEITEM16_ITEMDATA      8
#define WOWUSER_DELETEITEM16_SIZE          12
#define WOWUSER_COMPAREITEM16_HWNDITEM     4
#define WOWUSER_COMPAREITEM16_ITEMID1      6
#define WOWUSER_COMPAREITEM16_ITEMDATA1    8
#define WOWUSER_COMPAREITEM16_ITEMID2      12
#define WOWUSER_COMPAREITEM16_ITEMDATA2    14
#define WOWUSER_COMPAREITEM16_SIZE         18
#define WOWUSER_MAX_PARENT_DEPTH     8       /* how far an instance is looked for upward  */
#define WOWUSER_MAX_ALIASES          48      /* other programs' windows given a handle    */
#define WM_CTLCOLOR16                0x0019  /* Win16 only: Win32 split it per control    */

/* Tokens live well above the window handles (0x0100 + n*0x20) so a stray one is
   never mistaken for a window, and vice versa. */
#define WOWUSER_SYSRES_BASE      0x8000
#define WOWUSER_SYSRES_STEP      0x0008
#define WOWUSER_MAX_SYSRES       16

typedef struct _WOWUSER_SYSRES {
    WORD Handle16;                   /* 0 = free */
    WORD Ordinal;                    /* the ordinal the guest asked for, or 0 */
    WORD Kind;                       /* 1 = predefined system object, 3 = the
                                        MODULE's own resource */
    /* ★★ A RESOURCE CAN BE NAMED, AND MS PAINT'S ALL ARE. (session 47) Its icon
         group is "PBRUSH" and its seven cursors are "FLOOD", "CROSSH", "PICK"…,
         so a token that can only carry an ORDINAL cannot name any of them --
         which is why Paint had no icon at all and never changed its pointer.
         `Ordinal` and `Name` are alternatives: exactly one is set. */
    char Name[WOWUSER_SHORT_NAME_SIZE];   /* char, not CHAR: the spelling moves code (#333) */
    /* Set only for WOWUSER_AD_KIND_REALICON: the object itself, because there is nothing
       to look it up BY -- it came out of a file that is not this module. */
    HICON RealIcon;
    /* s89 (#216): the cursor, once built -- from the bytes USER handed 0xad, or
       on first use. SetCursor runs on every mouse move; building there each time
       would leak an object per move. */
    HCURSOR Cursor;
} WOWUSER_SYSRES, *PWOWUSER_SYSRES; typedef const WOWUSER_SYSRES *PCWOWUSER_SYSRES;


static WOWUSER_SYSRES g_WowUserSystemResources[WOWUSER_MAX_SYSRES];
static INT              g_WowUserSystemResourceCount = 0;

/* The slot behind a token, or NULL. */
static PWOWUSER_SYSRES WowUserSystemResourceSlot(WORD handle16)
{
    INT index;
    if (!handle16) return NULL;
    for (index = 0; index < g_WowUserSystemResourceCount; ++index)
        if (g_WowUserSystemResources[index].Handle16 == handle16) return &g_WowUserSystemResources[index];
    return NULL;
}

/* The ordinal behind a token, or 0 if this is not one of ours. */
static WORD WowUserSystemResourceOrdinal(WORD handle16)
{
    INT index;
    if (!handle16) return 0;
    for (index = 0; index < g_WowUserSystemResourceCount; ++index)
        if (g_WowUserSystemResources[index].Handle16 == handle16) return g_WowUserSystemResources[index].Ordinal;
    return 0;
}

/* Which kind of thing the token stands for -- 0 if it is not one of ours. */
static WORD WowUserSystemResourceKind(WORD handle16)
{
    INT index;
    if (!handle16) return 0;
    for (index = 0; index < g_WowUserSystemResourceCount; ++index)
        if (g_WowUserSystemResources[index].Handle16 == handle16) return g_WowUserSystemResources[index].Kind;
    return 0;
}

/* Case-insensitive compare, for the same reason `WowUserFindClass` is: a Win16
   resource name is stored upper-cased and asked for however the source wrote it
   ("PBRUSH2" stored, "PBrush2" asked -- session 45). */
static INT WowUserIsEqualNoCase(PCSTR first, PCSTR second)
{
    INT index;
    for (index = 0; ; ++index) {
        CHAR firstChar = first[index], secondChar = second[index];
        if (firstChar >= 'a' && firstChar <= 'z') firstChar = (CHAR)(firstChar - WOWUSER_LOWER_TO_UPPER);
        if (secondChar >= 'a' && secondChar <= 'z') secondChar = (CHAR)(secondChar - WOWUSER_LOWER_TO_UPPER);
        if (firstChar != secondChar) return 0;
        if (!firstChar) return 1;
    }
}

/* The resource NAME behind a token, or NULL if it was asked for by ordinal. */
static PCSTR WowUserSystemResourceName(WORD handle16)
{
    INT index;
    if (!handle16) return NULL;
    for (index = 0; index < g_WowUserSystemResourceCount; ++index)
        if (g_WowUserSystemResources[index].Handle16 == handle16)
            return g_WowUserSystemResources[index].Name[0] ? g_WowUserSystemResources[index].Name : NULL;
    return NULL;
}

/* ── ★★★ THE LAYOUT CLUSTER -- WHAT MAKES NOTEPAD USABLE. (session 44) ────────
     Every id here was named by the RUN and confirmed against USER's own export
     table (`docs/research/wow-user-surface.md`), not chosen from a list: the host
     log records each unimplemented USER call with the return address it came
     from, and `tools/ne/neimports.py` matches that against NOTEPAD.EXE's own
     NE import relocations. Four agreed both ways:

        0x38  12 args  MOVEWINDOW
        0x7d   8 args  INVALIDATERECT
        0xb3   2 args  GETSYSTEMMETRICS
        0x1f   2 args  ISICONIC

   ★ Notepad's resize arrives as InvalidateRect(hEdit, NULL, TRUE) followed by
     MoveWindow(hEdit, 8, 2, cx - 15, cy - 4, TRUE) -- 8 bytes and 12 bytes
     exactly, as each call carries, and the logged values put each field where
     the block below says. The block is REVERSED as always (the base is the LAST
     push).
   ⚠ `DefWindowProc` is NOT here and must not be added: `USER.107` never arrives
     as a BOP -- a whole run of Notepad produced none -- because USER handles it
     in its own 16-bit code. `DefFrameProc` (0x1bd) and `DefMDIChildProc` (0x1bf)
     DO thunk; nothing had called them when this was written. */
#define WOWUSER_MOVEWINDOW       0x0038
#define WOWUSER_MW_ARG_REPAINT   0
#define WOWUSER_MW_ARG_CY        2
#define WOWUSER_MW_ARG_CX        4
#define WOWUSER_MW_ARG_Y         6
#define WOWUSER_MW_ARG_X         8
#define WOWUSER_MW_ARG_HWND     10

#define WOWUSER_INVALIDATERECT   0x007d
#define WOWUSER_IR_ARG_ERASE     0
#define WOWUSER_IR_ARG_RECT      2
#define WOWUSER_IR_ARG_HWND      6

/* ── THE SMALL USER CALLS THE SHELF STILL ASKS FOR. (session 55) ─────────────
     Each is a Win32 function of the same name and meaning, so the body is a
     handle translation and a call. They are listed together because they were
     found together -- `neneeds.py --todo` over the whole shelf -- and because
     doing them one at a time is how a batch of one-liners turns into a session. */
#define WOWUSER_ISCHILD          0x0030   /* ord 48,   4 args  */
#define WOWUSER_ICH_ARG_HWND     0
#define WOWUSER_ICH_ARG_PARENT   2

#define WOWUSER_VALIDATERECT     0x007f   /* ord 127,  6 args  */
#define WOWUSER_VR_ARG_RECT      0
#define WOWUSER_VR_ARG_HWND      4

#define WOWUSER_INVALIDATERGN    0x007e   /* ord 126,  6 args  */
#define WOWUSER_IRG_ARG_ERASE    0
#define WOWUSER_IRG_ARG_RGN      2
#define WOWUSER_IRG_ARG_HWND     4

#define WOWUSER_GETCARETBLINK    0x00a9   /* ord 169,  0 args  */
#define WOWUSER_INSENDMESSAGE    0x00c0   /* ord 192,  0 args  */

#define WOWUSER_GETNEXTWINDOW    0x00e6   /* ord 230,  4 args  */
#define WOWUSER_GNW_ARG_FLAG     0
#define WOWUSER_GNW_ARG_HWND     2

#define WOWUSER_HILITEMENUITEM   0x00a2   /* ord 162,  8 args  */
#define WOWUSER_HMI_ARG_FLAGS    0
#define WOWUSER_HMI_ARG_ITEM     2
#define WOWUSER_HMI_ARG_MENU     4
#define WOWUSER_HMI_ARG_HWND     6

/* ── ★★★ 0x7a CallWindowProc -- AND IT IS ONE OF THE FOUR SINGLE CALLS THAT
     EACH BLOCK A GUEST. (CARDFILE) ─────────────────────────────────────────
     A subclassing program keeps the procedure it displaced and calls it for
     everything it does not handle. The displaced procedure here can be either
     kind, and that is the whole difficulty: a 16-bit one has to go through
     wowcall.h, and a window of one of OUR system classes has no 16-bit
     procedure at all and must go to the OS. */
#define WOWUSER_CALLWINDOWPROC   0x007a   /* ord 122, 14 args  */
#define WOWUSER_CWP_ARG_LPARAM   0
#define WOWUSER_CWP_ARG_WPARAM   4
#define WOWUSER_CWP_ARG_MSG      6
#define WOWUSER_CWP_ARG_HWND     8
#define WOWUSER_CWP_ARG_PROC     10

#define WOWUSER_GETSYSTEMMETRICS 0x00b3
#define WOWUSER_GSM_ARG_INDEX    0

#define WOWUSER_ISICONIC         0x001f
#define WOWUSER_II_ARG_HWND      0

/* ── ★★★ THE ENUMERATED BATCH -- FROM `tools/ne/neneeds.py`, NOT FROM A RUN ───
     Everything above was named by a guest stopping on it. These were named by
     reading NOTEPAD.EXE's import table -- so they are calls the program provably
     can make, including the ones that would have failed QUIETLY. See the tool for
     why an import is not automatically work.

     Every argument block below is the standard reversal (the base is the LAST
     word pushed), and every one is cross-checked against the argument-byte count
     the call carries:

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
     `neneeds.py` classifies `USER.1 MESSAGEBOX` as native16 -- the export does
     not thunk by itself -- yet a call to it still reaches us, as this id, from
     inside USER's 16-bit side: exactly the `LoadIcon`/`0xad` shape the tool's own
     header warns about. So this is the first thing the enumeration could not
     see, found the old way: a run stopped on it.
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
     what the call carries.
   ⚠ MODAL, on the exec thread, like ShellAbout and the file dialog. */
#define WOWUSER_MESSAGEBOX       0x0001
#define WOWUSER_MSGB_ARG_TYPE    0
#define WOWUSER_MSGB_ARG_CAPTION 2
#define WOWUSER_MSGB_ARG_TEXT    6
#define WOWUSER_MSGB_ARG_HWND   10

#define WOWUSER_GETFOCUS         0x0017
#define WOWUSER_ENABLEWINDOW     0x0022
#define WOWUSER_EW_ARG_ENABLE    0
#define WOWUSER_EW_ARG_HWND      2
#define WOWUSER_DESTROYWINDOW    0x0035
#define WOWUSER_DW_ARG_HWND      0
#define WOWUSER_SETACTIVEWINDOW  0x003b
#define WOWUSER_SAW_ARG_HWND     0
#define WOWUSER_MESSAGEBEEP      0x0068
#define WOWUSER_MB_ARG_TYPE      0
#define WOWUSER_OPENCLIPBOARD    0x0089
#define WOWUSER_OC_ARG_HWND      0
#define WOWUSER_CLOSECLIPBOARD   0x008a
#define WOWUSER_ENUMCLIPFMT      0x0090
#define WOWUSER_ECF_ARG_FORMAT   0

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
   ★ The ids, argument counts and return-stub offsets (the value each call
     carries at OFF_FROM), from `tools/ne/neneeds.py --stubs`; the run printed
     the same value to the digit where it reached one:
         66 GETDC        id 0x42   2 args  retstub 0x076a   (the run: 0x076a)
         67 GETWINDOWDC  id 0x43   2 args  retstub 0x090a
         68 RELEASEDC    id 0x44   4 args  retstub 0x0c5c
     2 = (HWND); 4 = (HWND, HDC). Both add up to what the calls carry.
   ⚠ 0x44 IS `DeleteDC` IN GDI'S NUMBERING AND `ReleaseDC` HERE -- the same
     collision this file exists to prevent, and a reason the dispatcher must
     never reach this switch with another module's stub segment. */
/* ── ★★★★★ 0x21 GetClientRect -- WHY MS PAINT LAID ITSELF OUT WRONG. ────────
     The single call behind "the way it paints is completely wrong". Measured
     against STOCK ntvdm running the SAME PBRUSH.EXE on the SAME box, via
     `rigshot tree`, with both frames at an identical 1252x688 client:

         child      stock (the oracle)     ours
         pbPaint    at(128,2) 1100x604     at(7,4)    1682x976
         pbTool     at(3,2)    121x516     at(4,2)     163x731
         pbSize     at(3,521)  121x164     at(4,736)   163x235
         pbColor    at(128,608)1099x66     at(172,860)1475x94

     Ours are laid out for a 1680x974 client -- which is exactly the `width` and
     `height` Paint reads from WIN.INI's [Paintbrush] section. It falls back to
     those because it asked how big it actually was and nobody answered: USER
     id 0x21, 6 argument bytes, `(lpRect far, hWnd=0x0160)` = GetClientRect on
     its own frame, stepped over. So the palette and the line-size box were laid
     out BELOW THE BOTTOM of the window and the toolbox was taller than its own
     parent -- which is why the only thing on screen was a column of stray lines.
   ★ Nothing was wrong with the drawing. The drawing was faithful; it was drawing
     the right picture at the wrong size, in a window the guest had been given no
     way to measure.
   ⚠ A Win16 RECT IS FOUR `int`s = 8 BYTES, against Win32's four LONGs = 16.

   ── AND THE THREE THAT CAME WITH IT, each named from its own call ───────────
     0x3e  8 args  (bRedraw=1, nPos=0, nBar=0, hWnd=0x180)      SetScrollPos
     0x40 10 args  (0, nMax=0x68e, nMin=0, nBar=0, hWnd=0x180)  SetScrollRange
     0x45  2 args  (hCursor)                                     SetCursor
     ★ `nMax = 0x68e` = 1678 is the canvas width, which is what identifies 0x40:
       a scroll RANGE over the image. These two are the missing scrollbars the
       oracle shows on the canvas and ours does not have. SB_HORZ/SB_VERT/SB_CTL
       are 0/1/2 in both worlds. */
#define WOWUSER_GETCLIENTRECT    0x0021
#define WOWUSER_GCR_ARG_RECT     0
#define WOWUSER_GCR_ARG_HWND     4

/* ── ★ SOLITAIRE AND MINESWEEPER -- ENUMERATED, NOT DISCOVERED. ──────────────
     Every id below came out of `tools/ne/neneeds.py --todo` run on the two
     binaries, so each is a call one of them really makes, and the argument-BYTE
     count the tool prints is what pins each offset table. The rule, and it is
     the one that has been got wrong most often here:

       THE ARGUMENT BLOCK IS REVERSED. Win16 is FAR PASCAL, so arguments are
       pushed LEFT TO RIGHT and the block's base is the LAST push -- offset 0 is
       the RIGHTMOST parameter. A far pointer is 4 bytes, an int/HWND/HDC is 2.

     Each table below is annotated with the prototype it was derived from and
     adds up to the byte count neneeds reported; if a call misbehaves, check the
     sum first -- an offset table that does not total the reported width is
     wrong by construction. */

/* UINT SetTimer(HWND, UINT nIDEvent, UINT wElapse, FARPROC lpTimerFunc) = 10 */
#define WOWUSER_SETTIMER         0x000a
#define WOWUSER_ST_ARG_PROC      0       /* far */
#define WOWUSER_ST_ARG_ELAPSE    4
#define WOWUSER_ST_ARG_ID        6
#define WOWUSER_ST_ARG_HWND      8

/* BOOL KillTimer(HWND, UINT nIDEvent) = 4
 ⚠ NOT in either program's TO-DO list -- KillTimer resolves to 16-bit code in
   USER, which then calls DOWN to this id. neneeds cannot see that call (it is
   not an import), which is exactly the `native16 does not mean free` trap in its
   own header. Arming a timer with no way to disarm it is a leak per game, so it
   is implemented alongside SetTimer rather than waiting for a run to show it. */
#define WOWUSER_KILLTIMER        0x000b
#define WOWUSER_KT_ARG_ID        0
#define WOWUSER_KT_ARG_HWND      2

/* DWORD GetCurrentTime(void) = 0 */
#define WOWUSER_GETCURRENTTIME   0x000f

/* HWND FindWindow(LPCSTR lpClassName, LPCSTR lpWindowName) = 8 */
#define WOWUSER_FINDWINDOW       0x0032
#define WOWUSER_FW_ARG_NAME      0       /* far */
#define WOWUSER_FW_ARG_CLASS     4       /* far */

/* int FrameRect(HDC, LPRECT, HBRUSH) = 8
 ⚠ `FRAMER_`, NOT `FR_`: FillRect (0x0051) already owns that prefix further down
   this file. The two prototypes happen to be identical, so the values coincide
   and this collision was harmless -- which is exactly why it survived. If either
   ever gained an argument, the later definition would silently win and one of
   them would read the other's layout. Caught by tests/unit/wow_test.c on its
   first run, not by anybody reading the build output. */
#define WOWUSER_FRAMERECT        0x0053
#define WOWUSER_FRAMER_ARG_BRUSH 0
#define WOWUSER_FRAMER_ARG_RECT  2       /* far */
#define WOWUSER_FRAMER_ARG_HDC   6

/* int DrawText(HDC, LPCSTR, int nCount, LPRECT, UINT uFormat) = 14 */
#define WOWUSER_DRAWTEXT         0x0055
#define WOWUSER_DT_ARG_FORMAT    0
#define WOWUSER_DT_ARG_RECT      2       /* far */
#define WOWUSER_DT_ARG_COUNT     6
#define WOWUSER_DT_ARG_STR       8       /* far */
#define WOWUSER_DT_ARG_HDC      12

/* void SetDlgItemText(HWND hDlg, int nIDDlgItem, LPCSTR) = 8 */
#define WOWUSER_SETDLGITEMTEXT   0x005c
#define WOWUSER_SDIT_ARG_TEXT    0       /* far */
#define WOWUSER_SDIT_ARG_ID      4
#define WOWUSER_SDIT_ARG_HDLG    6

/* UINT GetDlgItemInt(HWND, int nIDDlgItem, BOOL FAR *lpTranslated, BOOL) = 10 */
#define WOWUSER_GETDLGITEMINT    0x005f
#define WOWUSER_GDII_ARG_SIGNED  0
#define WOWUSER_GDII_ARG_XLATED  2       /* far */
#define WOWUSER_GDII_ARG_ID      6
#define WOWUSER_GDII_ARG_HDLG    8

/* void CheckRadioButton(HWND, int nIDFirst, int nIDLast, int nIDCheck) = 8 */
#define WOWUSER_CHECKRADIOBUTTON 0x0060
#define WOWUSER_CRB_ARG_CHECK    0
#define WOWUSER_CRB_ARG_LAST     2
#define WOWUSER_CRB_ARG_FIRST    4
#define WOWUSER_CRB_ARG_HDLG     6

/* void CheckDlgButton(HWND, int nIDButton, UINT uCheck) = 6 */
#define WOWUSER_CHECKDLGBUTTON   0x0061
#define WOWUSER_CDB_ARG_CHECK    0
#define WOWUSER_CDB_ARG_ID       2
#define WOWUSER_CDB_ARG_HDLG     4

/* UINT IsDlgButtonChecked(HWND, int nIDButton) = 4 */
#define WOWUSER_ISDLGBUTTONCHECKED 0x0062
#define WOWUSER_IDBC_ARG_ID      0
#define WOWUSER_IDBC_ARG_HDLG    2

/* void AdjustWindowRect(LPRECT, DWORD dwStyle, BOOL bMenu) = 10 */
#define WOWUSER_ADJUSTWINDOWRECT 0x0066
#define WOWUSER_AWR_ARG_MENU     0
#define WOWUSER_AWR_ARG_STYLE    2       /* DWORD */
#define WOWUSER_AWR_ARG_RECT     6       /* far */

/* ── ★★★ 0x96 -- USER's OWN DOWNCALL FOR LoadMenu. 16 arg bytes. ────────────
     NOT the exported LoadMenu, which is 16-bit code and never reaches us. This
     is what that code calls DOWN to once it has found and locked the resource,
     the same shape as 0xad for icons -- so it is invisible to neneeds.py, which
     only sees a program's imports. WINMINE's USER surface reads 41/41 COMPLETE
     and it still had no menu. **native16 does not mean free.**

     The block, as it arrives (eight words, reversed):

         offset 14  hInstance
         offset 10  lpMenuName (high word at 12)
         offset 6   the locked resource, far (segment at 8)
         offsets 4, 2, 0  -- not identified

     Minesweeper's live call carries hInstance 0x0b86 and lpMenuName 0x000001f4
     -- a MAKEINTRESOURCE whose high word is 0, so an ORDINAL -- and
     `neres.py list WINMINE.EXE` says `MENU 500`. 0x1f4 IS 500. The id was in the
     arguments the whole time.
   ⚠ Offsets 0/2/4 carry values this host does not need and are NOT
     named as anything: guessing at them would put three inventions in a header
     that is otherwise all measurement. They are covered so the block tiles. */
#define WOWUSER_LOADMENU         0x0096
#define WOWUSER_LOADMENU_ARG_LOCAL0  0
#define WOWUSER_LOADMENU_ARG_LOCAL2  2
#define WOWUSER_LOADMENU_ARG_LOCAL4  4
#define WOWUSER_LOADMENU_ARG_RES     6    /* far -- the locked resource */
#define WOWUSER_LOADMENU_ARG_NAME   10    /* MAKEINTRESOURCE, or a far string */
#define WOWUSER_LOADMENU_ARG_HINST  14

/* BOOL SetMenu(HWND, HMENU) = 4
 ⚠ NOT `SM_ARG_*`: SendMessage already owns that prefix further down this file
   and redefines it to 8. The LAST definition before the use wins, so a SetMenu
   written with `WOWUSER_SM_ARG_HWND` silently reads SendMessage's offset and the
   compiler says only "redefined". Prefixes here are a namespace, not a habit. */
#define WOWUSER_SETMENU          0x009e
#define WOWUSER_SETMENU_ARG_MENU 0
#define WOWUSER_SETMENU_ARG_HWND 2

/* HWND GetLastActivePopup(HWND hwndOwner) = 2 */
#define WOWUSER_GETLASTACTIVEPOPUP 0x011f
#define WOWUSER_GLAP_ARG_HWND    0

/* ── ★★★★★ 0x2f IsWindow / 0x31 IsWindowVisible -- WHY THE TOOLBOX NEVER MOVED.
     Two of the smallest calls in USER, and between them they were the whole of
     the second half of "the way it paints is completely wrong".

     After `GetClientRect` went in, Paint's WM_SIZE handler did exactly one thing:
     `MoveWindow(pbPaint, (3,2) 1251x687)` -- it gave the CANVAS the entire client
     and moved nothing else, while stock puts the canvas at (128,2) 1100x604 and
     leaves room around it. Paint had decided there was no toolbox and no palette
     to leave room for, and it decided that by ASKING:

         IsWindowVisible(0x01a0)   x2   -- pbTool,  the toolbox
         IsWindowVisible(0x01e0)        -- pbColor, the palette
         IsWindow(0x0180)          x2   -- pbPaint, the canvas

     all stepped over, all answered 0. ⇒ Paint believed its own toolbox and
     palette were hidden -- so it never resized them, and they kept the size they
     were CREATED at, which came from Paint's fallback window height of 974
     (`SM_CYFULLSCREEN - SM_CYMENU`, the default it passes to `GetProfileInt`
     because this rig's WIN.INI has no `[Paintbrush] height`). That is why they
     were ~1.35x too large and fell below the bottom of a 688-tall client.

   ★ Windows 3.1 Paintbrush can genuinely hide both (View > Tools and Linesize,
     View > Palette), so "is it visible" is a real question with a real answer,
     and answering 0 was not a harmless default -- it was a lie about the state
     of windows this host had itself created and shown.
   ⚠ TWO REFUTED HYPOTHESES ARE BURIED HERE, both plausible and both wrong:
     `GetDeviceCaps(HORZSIZE/VERTSIZE)` (forcing the ratio to stock's 2.0 changed
     the toolbox by nothing) and "the guest is never told its size" (WM_SIZE has
     been relayed since session 43, and the log shows it arriving with the
     correct 1252x688). The layout was not mis-computed; it was never
     re-computed. */
#define WOWUSER_ISWINDOW         0x002f
/* ── ★★★ THE SHELF, BATCH ONE: TASKMAN, CLOCK AND CALC. (session 53) ──────────
     `tools/ne/neneeds.py` says these three programs are 2, 5 and 4 services away
     from launching -- not "roughly", but by name, from their own import tables.
     Ten of them are here and ExtTextOut is in wowgdi.h.
   ⚠ EVERY ONE OF THESE IS THE REAL WIN32 CALL ON THE GUEST'S REAL WINDOW, for the
     same reason the menu and the caption are: these ARE Win32 windows, and an
     answer composed here would be a second opinion about state the OS already
     owns. Where that is NOT possible it is said so at the call, loudly, rather
     than answered plausibly -- see the clipboard pair. */

/* UINT ArrangeIconicWindows(HWND) -- TASKMAN's whole reason to exist. */
#define WOWUSER_ARRANGEICONICWINDOWS 0x00aa
#define WOWUSER_AIW_ARG_HWND     0

/* void SwitchToThisWindow(HWND, BOOL fAltTab) -- TASKMAN's "Switch To" button.
   ⚠ USER32 exports it undocumented and no import library declares it, so this is
     the documented pair that does the same job: restore it if minimised, then put
     it in front. A guest cannot tell the difference; a linker can. */
#define WOWUSER_SWITCHTOTHISWINDOW   0x00ac
#define WOWUSER_STW_ARG_ALTTAB   0
#define WOWUSER_STW_ARG_HWND     2

/* DWORD GetDialogBaseUnits(void) -- LOWORD x, HIWORD y. */
#define WOWUSER_GETDIALOGBASEUNITS   0x00f3

/* SHORT GetAsyncKeyState(int vk) -- Win16 and Win32 agree on the shape: bit 15
   is "down now", bit 0 "pressed since the last call". */
#define WOWUSER_GETASYNCKEYSTATE     0x00f9
#define WOWUSER_GAKS_ARG_VK      0

/* BOOL IsZoomed(HWND). Same family as IsIconic, and the same reason to ask the
   OS rather than to answer 0: the wrong answer is right most of the time. */
#define WOWUSER_ISZOOMED             0x0110
#define WOWUSER_IZ_ARG_HWND      0

/* BOOL AppendMenu(HMENU, UINT flags, UINT id, LPCSTR item) -- CLOCK builds its
   own menu at run time rather than from a resource. */
#define WOWUSER_APPENDMENU           0x019b
#define WOWUSER_AM_ARG_ITEM      0
#define WOWUSER_AM_ARG_ID        4
#define WOWUSER_AM_ARG_FLAGS     6
#define WOWUSER_AM_ARG_HMENU     8

/* BOOL IsDialogMessage(HWND hDlg, LPMSG lpMsg) -- CALC's message loop. */
#define WOWUSER_ISDIALOGMESSAGE      0x005a
#define WOWUSER_IDM_ARG_MSG      0
#define WOWUSER_IDM_ARG_HDLG     4

/* void MapDialogRect(HWND hDlg, LPRECT lprc) -- dialog units to client pixels. */
#define WOWUSER_MAPDIALOGRECT        0x0067
#define WOWUSER_MDR_ARG_RECT     0
#define WOWUSER_MDR_ARG_HDLG     4

/* ── THE CLIPBOARD PAIR -- AND THE BRIDGE THAT LETS BOTH TELL THE TRUTH. (#160) ──
     GetClipboardData must hand back a handle the GUEST can lock -- a 16-bit
     global handle in its own address space. The real Win32 handle is not that, so
     for s40-s81 the honest pair was "nothing available, nothing returned".
   ★ The bridge: for TEXT (CF_TEXT, CF_OEMTEXT) the host asks krnl386 itself for a
     global block (KERNEL.15 GlobalAlloc through wowcall), locks it (KERNEL.18),
     copies the host clipboard's text in and unlocks it (KERNEL.19) -- the chain in
     WOWCALL_ACT_CLIP*. The guest gets a handle its own KERNEL made.
   ⚠ ONE VOICE STILL: availability is answered from the host ONLY for the formats
     the bridge can deliver, and only when it can run (callbacks on, krnl386's
     segment known). Available-but-empty is a state no real clipboard is ever in.
   ⚠ The block is GMEM_DDESHARE and belongs to the asking task; it is freed when
     that task ends, so each paste costs one block for the task's lifetime. */
#define WOWUSER_GETCLIPBOARDDATA     0x008e
#define WOWUSER_ISCLIPBOARDFORMATAVAILABLE 0x00c1
#define WOWUSER_CB_ARG_FORMAT    0
/* HANDLE SetClipboardData(UINT fmt, HANDLE hMem) -- USER.141, between Empty (139),
   GetClipboardOwner (140) and Get (142). Pascal: hMem is the last argument, so it
   sits lowest. */
#define WOWUSER_SETCLIPBOARDDATA     0x008d
#define WOWUSER_SCD_ARG_HMEM     0
#define WOWUSER_SCD_ARG_FORMAT   2
#define CF_TEXT16        1
#define CF_OEMTEXT16     7
#define GMEM_MOVEABLE_DDESHARE16 0x2002
/* The host-side copy of the text in flight. One transfer at a time is all the
   exec thread can have: a chain runs to completion before the guest's next call. */
#define WOWUSER_CLIPBOARD_SIZE 65536
static CHAR g_WowUserClipboard[WOWUSER_CLIPBOARD_SIZE];
static INT  g_WowUserClipboardLength;
static WORD g_WowUserClipboardFormat;  /* SetClipboardData's format, for the put */

/* ── ★★★ THE SHELF, BATCH TWO: RECORDER, MPLAYER AND CHARMAP. (session 53) ────
     Priced by neneeds.py at 9, 11 and 13 services. Most are a Win32 call with a
     handle translated at each end, and they are written out plainly rather than
     wrapped in a macro: the ones that are NOT plain (a 256-byte key array, a
     RECT that is 8 bytes not 16, a hook chain we do not own) are the ones worth
     seeing, and a macro would hide them among the ones that are.
   ⚠ NONE of these composes an answer. Where the OS cannot be asked, the call
     says so at the site instead of returning something plausible. */
#define WOWUSER_ISWINDOWENABLED      0x0023
#define WOWUSER_GETWINDOWTEXTLENGTH  0x0026
#define WOWUSER_GETWINDOWTEXT        0x0024   /* USER.36, 8 args: see its case */
#define WOWUSER_GWT_ARG_MAX      0
#define WOWUSER_GWT_ARG_BUF      2
#define WOWUSER_GWT_ARG_HWND     6
#define WOWUSER_WINDOWFROMPOINT      0x001e
#define WOWUSER_FLASHWINDOW          0x0069
#define WOWUSER_GETCAPTURE           0x00ec
#define WOWUSER_GETKEYBOARDSTATE     0x00de
#define WOWUSER_SETKEYBOARDSTATE     0x00df
#define WOWUSER_VKKEYSCAN            0x0081
#define WOWUSER_MAPVIRTUALKEY        0x0083
#define WOWUSER_EMPTYCLIPBOARD       0x008b
#define WOWUSER_GETUPDATERECT        0x00be
#define WOWUSER_GETNEXTDLGTABITEM    0x00e4
#define WOWUSER_GETDLGCTRLID         0x0115
#define WOWUSER_DRAWFOCUSRECT        0x01d2
#define WOWUSER_DELETEMENU           0x019d
#define WOWUSER_GETWINDOWPLACEMENT   0x0172
#define WOWUSER_UNHOOKWINDOWSHOOK    0x00ea
/* s93: USER.121 SetWindowsHook arrives as an 8-byte block: lpfn (far) at +0,
   nFilterType at +4, and at +6 the module handle owning the hook procedure (as
   logged); the DX:AX answered is what SetWindowsHook returns ("the previous
   hook"). */
#define WOWUSER_SETWINDOWSHOOK       0x0079
#define WOWUSER_SWH_ARG_PROC   0
#define WOWUSER_SWH_ARG_ID     4
#define WOWUSER_SWH_ARG_HMOD   6
#define WOWUSER_DEFHOOKPROC          0x00eb
#define WOWUSER_SYSTEMPARAMETERSINFO 0x01e3

#define WOWUSER_W1_ARG_HWND      0  /* every one-word (HWND) call                    */
/* ⚠ s90: WAS y-at-+0 FROM s53 TO s90, WRITTEN FROM REASONING AND NEVER MEASURED.
     w_misc `wfp.visible` (a visible popup at (300,500), point (350,520)) read the
     window under stock and NOT under ours. The POINT lies in field order: x, y. */
#define WOWUSER_WFP_ARG_X        0  /* WindowFromPoint(POINT)                         */
#define WOWUSER_WFP_ARG_Y        2
/* ── s90 (#297): the USER singles. Frames reversed as always (+0 = last param). */
#define WOWUSER_GETCLIPBOARDFORMATNAME 0x0092 /* (fmt, buf, cch)        8 */
#define WOWUSER_GCFN_ARG_CCH     0
#define WOWUSER_GCFN_ARG_BUF     2
#define WOWUSER_GCFN_ARG_FMT     6
#define WOWUSER_DLGDIRSELECT     0x0063  /* (hDlg, lpString, nIDListBox)  8 */
#define WOWUSER_DDS_ARG_ID       0
#define WOWUSER_DDS_ARG_STR      2
#define WOWUSER_DDS_ARG_HDLG     6
#define WOWUSER_SETPARENT        0x00e9  /* (hwndChild, hwndNewParent)    4 */
#define WOWUSER_SPA_ARG_NEW      0
#define WOWUSER_SPA_ARG_CHILD    2
#define WOWUSER_GETCLASSINFO     0x0194  /* (hInst, lpszClass, lpWndClass) 10 */
#define WOWUSER_GCI_ARG_WC       0
#define WOWUSER_GCI_ARG_NAME     4
#define WOWUSER_GCI_ARG_HINST    8
#define WOWUSER_CHILDWINDOWFROMPOINT 0x00bf /* (hwnd, POINT) -- POINT as WFP_ 6 */
/* ⚠ MEASURED (w_misc, s90): x at +0, y at +2 -- the POINT lies in the frame in
     field order. The first cut copied WFP_'s y-first reading and stock disagreed
     on both asymmetric cases. */
#define WOWUSER_CWFP_ARG_X       0
#define WOWUSER_CWFP_ARG_Y       2
#define WOWUSER_CWFP_ARG_HWND    4
#define WOWUSER_CALLMSGFILTER    0x007b  /* (lpMsg, nCode)                 6 */
/* s90 (#296): EnumProps(hwnd, proc) = 6, reversed: +0 proc, +4 hwnd. */
#define WOWUSER_ENUMPROPS        0x001b
#define WOWUSER_EPR_ARG_PROC     0
#define WOWUSER_EPR_ARG_HWND     4
/* GetInternalIconHeader(lp, lp) -- undocumented; stock answers 0 (w_misc `giih`). */
#define WOWUSER_GETINTERNALICONHEADER 0x0174
#define WOWUSER_CMF16_ARG_CODE   0
#define WOWUSER_CMF16_ARG_MSG    2
#define WOWUSER_FW_ARG_INVERT    0
#define WOWUSER_FW_ARG_HWND      2
#define WOWUSER_KS_ARG_BUF       0  /* far pointer to 256 bytes                      */
#define WOWUSER_VKS_ARG_CHAR     0
#define WOWUSER_MVK_ARG_TYPE     0
#define WOWUSER_MVK_ARG_CODE     2
#define WOWUSER_GUR_ARG_ERASE    0
#define WOWUSER_GUR_ARG_RECT     2
#define WOWUSER_GUR_ARG_HWND     6
#define WOWUSER_GNDTI_ARG_PREV   0
#define WOWUSER_GNDTI_ARG_CTL    2
#define WOWUSER_GNDTI_ARG_HDLG   4
#define WOWUSER_DFR_ARG_RECT     0
#define WOWUSER_DFR_ARG_HDC      4
#define WOWUSER_DM_ARG_FLAGS     0
#define WOWUSER_DM_ARG_POS       2
#define WOWUSER_DM_ARG_HMENU     4
#define WOWUSER_GWP_ARG_PL       0
#define WOWUSER_GWP_ARG_HWND     4
#define WOWUSER_SPI_ARG_WINI     0
#define WOWUSER_SPI_ARG_PARAM    2
#define WOWUSER_SPI_ARG_UIPARAM  6
#define WOWUSER_SPI_ARG_ACTION   8

#define WOWUSER_DLGDIRLIST           0x0064
#define WOWUSER_DDL_ARG_FILETYPE 0
#define WOWUSER_DDL_ARG_IDSTATIC 2
#define WOWUSER_DDL_ARG_IDLIST   4
#define WOWUSER_DDL_ARG_SPEC     6
#define WOWUSER_DDL_ARG_HDLG    10

/* ⚠ ChangeMenu IS FIVE FUNCTIONS BEHIND ONE ORDINAL -- the Win16 legacy
     multiplexer that predates Append/Insert/Modify/Delete/Remove. The action is
     in the FLAGS, not in the name, so dispatching on them is the whole job; a
     version that only appended would silently do the wrong thing four times out
     of five. */
#define WOWUSER_CHANGEMENU           0x0099
#define WOWUSER_CM_ARG_CHANGE    0
#define WOWUSER_CM_ARG_IDNEW     2
#define WOWUSER_CM_ARG_ITEM      4
#define WOWUSER_CM_ARG_IDCHANGE  8
#define WOWUSER_CM_ARG_HMENU    10

#define WOWUSER_GRAYSTRING           0x00b9
#define WOWUSER_GS_ARG_HEIGHT    0
#define WOWUSER_GS_ARG_WIDTH     2
#define WOWUSER_GS_ARG_Y         4
#define WOWUSER_GS_ARG_X         6
#define WOWUSER_GS_ARG_COUNT     8
#define WOWUSER_GS_ARG_DATA     10
#define WOWUSER_GS_ARG_OUTFUNC  14
#define WOWUSER_GS_ARG_HBRUSH   18
#define WOWUSER_GS_ARG_HDC      20

#define WOWUSER_DEFDLGPROC           0x0134
#define WOWUSER_DDP_ARG_LPARAM   0
#define WOWUSER_DDP_ARG_WPARAM   4
#define WOWUSER_DDP_ARG_MSG      6
#define WOWUSER_DDP_ARG_HDLG     8

/* ── ★★★ THE SHELF, BATCH THREE. (session 53) ────────────────────────────────
     SOUNDREC is six services away and four of the remaining guests share most of
     them -- the menu family in particular is wanted by CARDFILE, PACKAGER and
     WINFILE as well, so these are chosen for OVERLAP rather than for one guest. */
#define WOWUSER_DRAWICON             0x0054
#define WOWUSER_DI2_ARG_HICON    0
#define WOWUSER_DI2_ARG_Y        2
#define WOWUSER_DI2_ARG_X        4
#define WOWUSER_DI2_ARG_HDC      6

#define WOWUSER_GETCLIPBOARDOWNER    0x008c
#define WOWUSER_GETDOUBLECLICKTIME   0x0015
#define WOWUSER_CREATEPOPUPMENU      0x019f
#define WOWUSER_DESTROYMENU          0x0098
#define WOWUSER_DESTROYICON          0x01c9
#define WOWUSER_GETMENUITEMCOUNT     0x0107
#define WOWUSER_GETTOPWINDOW         0x00e5

/* ModifyMenu and InsertMenu share a signature exactly, so they share a case;
   the id decides which of the two the OS is asked for. */
#define WOWUSER_MODIFYMENU           0x019e
#define WOWUSER_INSERTMENU           0x019a
#define WOWUSER_MI2_ARG_ITEM     0
#define WOWUSER_MI2_ARG_IDNEW    4
#define WOWUSER_MI2_ARG_FLAGS    6
#define WOWUSER_MI2_ARG_POS      8
#define WOWUSER_MI2_ARG_HMENU   10

#define WOWUSER_GETMENUITEMID        0x0108
#define WOWUSER_GMII_ARG_POS     0
#define WOWUSER_GMII_ARG_HMENU   2

#define WOWUSER_GETMENUSTATE         0x00fa
#define WOWUSER_GMS_ARG_FLAGS    0
#define WOWUSER_GMS_ARG_ID       2
#define WOWUSER_GMS_ARG_HMENU    4

#define WOWUSER_GETMENUSTRING        0x00a1
#define WOWUSER_GMSTR_ARG_FLAGS  0
#define WOWUSER_GMSTR_ARG_MAX    2
#define WOWUSER_GMSTR_ARG_BUF    4
#define WOWUSER_GMSTR_ARG_ID     8
#define WOWUSER_GMSTR_ARG_HMENU 10

#define WOWUSER_SCROLLWINDOW         0x003d
#define WOWUSER_SW2_ARG_CLIP     0
#define WOWUSER_SW2_ARG_RECT     4
#define WOWUSER_SW2_ARG_DY       8
#define WOWUSER_SW2_ARG_DX      10
#define WOWUSER_SW2_ARG_HWND    12

#define WOWUSER_GETSCROLLRANGE       0x0041
#define WOWUSER_GSR_ARG_MAX      0
#define WOWUSER_GSR_ARG_MIN      4
#define WOWUSER_GSR_ARG_BAR      8
#define WOWUSER_GSR_ARG_HWND    10

#define WOWUSER_SHOWSCROLLBAR        0x010b
/* #300: EnableScrollBar(hWnd, wSBflags, wArrows) -- 6 arg bytes, Pascal: wArrows@0,
   wSBflags@2, hWnd@4. Stepped over 8 times in s89's logs (USER reaches it from its
   own code). Same constants in Win16 and Win32 (SB_*, ESB_*). */
#define WOWUSER_ENABLESCROLLBAR      0x01e2
#define WOWUSER_SSB_ARG_SHOW     0
#define WOWUSER_SSB_ARG_BAR      2
#define WOWUSER_SSB_ARG_HWND     4

/* ── ★★ BATCH FOUR: what CARDFILE and WINFILE still want that is answerable. ──
   ⚠⚠ THE COMM FAMILY ANSWERS "NO SUCH PORT", AND THAT IS THE TRUTH, NOT A STUB.
     This host's BIOS equipment word claims ONE parallel port and NO serial ports
     -- deliberately, because writing COM1..COM4 into the BDA would be inventing
     hardware nothing answers for, and COMM.DRV's LibMain returns whatever is at
     that BDA slot (docs/STATE.md, session 36). So there is no port to open, and
     IE_BADID (-2) is exactly what Windows returns for a port id that does not
     exist. CARDFILE uses these to AUTODIAL; it gets a clean refusal and works
     without a modem, which is the same thing a real machine with no COM port
     would give it. */
/* Provided by the host (main.c) over vdd_comm -- DECLARATIONS ONLY, because this
   header must not see host internals. See the note beside them. */
INT  WowCommOpen(PCSTR dev);
INT  WowCommClose(INT id);
INT  WowCommRead(INT port, PBYTE buffer, INT count);
INT  WowCommWrite(INT port, PCBYTE buffer, INT count);
INT  WowCommInqueue(INT id);
VOID WowCommDtr(INT id, INT on);
VOID WowCommRts(INT id, INT on);

#define WOWUSER_OPENCOMM       0x00c8
/* Pascal order: base = the LAST argument pushed. OpenComm(dev, cbIn, cbOut). */
#define WOWUSER_OC_ARG_DEV    4
#define WOWUSER_CC_ARG_ID     0
#define WOWUSER_RC_ARG_CB     0
#define WOWUSER_RC_ARG_BUF    2
#define WOWUSER_RC_ARG_ID     6
#define WOWUSER_TC_ARG_CH     0
#define WOWUSER_TC_ARG_ID     2
#define WOWUSER_GCE_ARG_STAT  0
#define WOWUSER_GCE_ARG_ID    4
#define WOWUSER_ECF_ARG_FN    0
#define WOWUSER_ECF_ARG_ID    2
#define WOWUSER_CLOSECOMM      0x00cf
#define WOWUSER_TRANSMITCHAR   0x00ce
#define WOWUSER_SETCOMMSTATE   0x00c9
#define WOWUSER_GETCOMMSTATE   0x00ca
#define WOWUSER_GETCOMMERROR   0x00cb
#define WOWUSER_WRITECOMM      0x00cd
#define WOWUSER_FLUSHCOMM      0x00d7
/* The rest of the Win16 comm surface the shelf asks for -- TERMINAL.EXE wants
   all five. They join the honest refusal below rather than getting stubs.
   ⚠ SetCommEventMask IS THE ODD ONE: it returns a FAR POINTER to the event
     word, not a status, so IE_BADID would be a pointer to 0xFFFE. Its failure
     value is a null pointer, and it is answered separately for that reason. */
#define WOWUSER_READCOMM       0x00cc
#define WOWUSER_SETCOMMEVTMASK 0x00d0
#define WOWUSER_SETCOMMBREAK   0x00d2
#define WOWUSER_CLEARCOMMBREAK 0x00d3
#define WOWUSER_ESCAPECOMMFN   0x00d6

#define WOWUSER_SETWINDOWPLACEMENT 0x0173
#define WOWUSER_EXITWINDOWS        0x0007
#define WOWUSER_DEFFRAMEPROC       0x01bd
#define WOWUSER_DFP_ARG_LPARAM   0
#define WOWUSER_DFP_ARG_WPARAM   4
#define WOWUSER_DFP_ARG_MSG      6
#define WOWUSER_DFP_ARG_HCLIENT  8
#define WOWUSER_DFP_ARG_HWND    10
#define WOWUSER_DEFMDICHILDPROC    0x01bf

#define WOWUSER_TABBEDTEXTOUT      0x00c4
#define WOWUSER_TTO_ARG_TABORG   0
#define WOWUSER_TTO_ARG_TABPOS   2
#define WOWUSER_TTO_ARG_TABCNT   6
#define WOWUSER_TTO_ARG_COUNT    8
#define WOWUSER_TTO_ARG_STR     10
#define WOWUSER_TTO_ARG_Y       14
#define WOWUSER_TTO_ARG_X       16
#define WOWUSER_TTO_ARG_HDC     18

#define WOWUSER_ISWINDOWVISIBLE  0x0031
#define WOWUSER_IW_ARG_HWND      0

/* ── ★★★★ 0x12 SetCapture / 0x13 ReleaseCapture -- WHAT MAKES A DRAG A STROKE.
     From `neneeds.py`'s list for PBRUSH (ord 18 and 19, 2 and 0 argument bytes).
     A paint program takes the capture on button-down so that the rest of the
     stroke arrives even when the pointer leaves the canvas, and gives it back on
     button-up. Without it a stroke ends at the window edge -- or, worse, the
     button-up is delivered to whatever window the pointer happens to be over and
     the guest never learns the stroke finished, so it keeps drawing.
   ★ The real Win32 capture is the right mechanism, exactly as the real menu and
     the real caption are: these windows ARE Win32 windows.
   ⚠ SetCapture RETURNS THE PREVIOUS CAPTURE WINDOW, and it has to come back as a
     16-bit handle -- a guest that restores it would otherwise be handed a
     truncated HWND. A previous window that is not one of ours answers 0, which
     is what "nobody had it" looks like to the guest. */
#define WOWUSER_SETCAPTURE       0x0012
#define WOWUSER_RELEASECAPTURE   0x0013
#define WOWUSER_CAP_ARG_HWND     0

/* ── ★★★★ THE REST OF THE DRAWING PATH. ─────────────────────────────────────
     Named from the run in which the mouse first reached MS Paint. Paint answers
     a button-down on its canvas with `SetCapture`, `GetDC`, `CreateSolidBrush`,
     `GetClientRect` -- all of which worked -- and then a loop that was stepped
     over 21 times per stroke. Their ids track USER's ordinals, and the
     neighbours prove the mapping rather than assuming it: ord 30 -> 0x1e, ord 31
     -> 0x1f, and ord 33 GETCLIENTRECT -> 0x21, which is the id this file already
     implements.
       ord 16 CLIPCURSOR      id 0x10   4 args  (LPRECT)
       ord 28 CLIENTTOSCREEN  id 0x1c   6 args  (HWND, LPPOINT)
       ord 32 GETWINDOWRECT   id 0x20   6 args  (HWND, LPRECT)
       ord 60 GETACTIVEWINDOW id 0x3c   0 args  ()
     ⚠ A Win16 POINT is two `int`s = 4 bytes and a RECT is four = 8, against
       Win32's 8 and 16. Same conversion as everywhere else in this file.

   ⚠⚠ CLIPCURSOR IS DELIBERATELY NOT APPLIED, and this is a stated deviation
     rather than an oversight. Paint uses it to pen the pointer inside its canvas
     for the duration of a stroke, which is a nicety it does not need to draw
     correctly -- but `ClipCursor` is SYSTEM-WIDE, and this host is a VDM the
     harness kills with `taskkill` several times a session. A guest that is
     terminated mid-stroke while holding a clip would leave the user's real
     pointer confined to a rectangle on their own desktop. The call is accepted
     and logged; a later session that wants it can apply it and release it on
     WM_KILLFOCUS, capture loss and task exit. */
#define WOWUSER_CLIPCURSOR       0x0010
#define WOWUSER_CC_ARG_RECT      0
#define WOWUSER_CLIENTTOSCREEN   0x001c
#define WOWUSER_C2S_ARG_POINT    0
#define WOWUSER_C2S_ARG_HWND     4
#define WOWUSER_GETWINDOWRECT    0x0020
#define WOWUSER_GWR_ARG_RECT     0
#define WOWUSER_GWR_ARG_HWND     4
#define WOWUSER_GETACTIVEWINDOW  0x003c

/* ★ 0x51 FillRect(hDC, lprc, hbr) -- USER ordinal 81, 8 argument bytes
     (2 + 4 + 2), and `neimports.py` names the call site in PBRUSH.EXE outright.
     The run confirms the order: `(2020 | 6ed6 09c7 | 20c0)` is a stock-object
     BRUSH token at +0, a far RECT at +2 and one of our DC tokens at +6. */
/* ── ★★★★★ THE SECOND USER SWEEP (session 47). ──────────────────────────────
     With `neneeds.py` able to see through USER's validating export wrappers, the
     surface these two programs actually reach went from 52 to 92 calls, and
     everything below is on that list with its id and argument count. The
     constants are the ones the GUEST passes at run time, not out of a header:

       SetClassWord  index -12 = GCW_HCURSOR, and the value is
                     LoadCursor(0, 0x7f00) = IDC_ARROW. **This is how MS Paint
                     changes its pointer per tool** -- eighteen calls a run,
                     every one of them stepped over until now.
       GetWindowLong index -16 = GWL_STYLE, read and written back through
                     SetWindowLong.
       GetKeyState   Paint tests the HIGH BIT, i.e. "is the key down now", which
                     is Win32's convention unchanged.
   ⚠ Win16's negative indices are the same numbers as Win32's for the fields that
     exist in both, which is what the two observations above confirm --
     but only for those two. Anything else is refused by name rather than passed
     through on the strength of a pattern. */
#define WOWUSER_DEFWINDOWPROC    0x006b
#define WOWUSER_DWP_ARG_LPARAM   0       /* DWORD */
#define WOWUSER_DWP_ARG_WPARAM   4
#define WOWUSER_DWP_ARG_MSG      6
#define WOWUSER_DWP_ARG_HWND     8

#define WOWUSER_SETCLASSWORD     0x0082
#define WOWUSER_SCW_ARG_VALUE    0
#define WOWUSER_SCW_ARG_INDEX    2
#define WOWUSER_SCW_ARG_HWND     4
#define WOWUSER_GCW16_HCURSOR  (-12)

#define WOWUSER_GETWINDOWLONG    0x0087
#define WOWUSER_GWL_ARG_INDEX    0
#define WOWUSER_GWL_ARG_HWND     2
#define WOWUSER_SETWINDOWLONG    0x0088
#define WOWUSER_SWL_ARG_VALUE    0       /* DWORD */
#define WOWUSER_SWL_ARG_INDEX    4
#define WOWUSER_SWL_ARG_HWND     6
#define WOWUSER_GWL16_WNDPROC  (-4)
#define WOWUSER_GWL16_STYLE   (-16)
#define WOWUSER_GWL16_EXSTYLE (-20)

#define WOWUSER_GETKEYSTATE      0x006a
#define WOWUSER_GETSYSCOLOR      0x00b4
#define WOWUSER_GETMESSAGEPOS    0x0077
#define WOWUSER_GETMSGEXTRAINFO  0x0120
#define WOWUSER_GETDESKTOPWINDOW 0x011e
#define WOWUSER_BRINGWINDOWTOTOP 0x002d
#define WOWUSER_DRAWMENUBAR      0x00a0
#define WOWUSER_SHOWCURSOR       0x0047
#define WOWUSER_GETCURSORPOS     0x0011  /* 4 args, far LPPOINT */
#define WOWUSER_SETCURSORPOS     0x0046  /* 4 args (x, y)       */
#define WOWUSER_SCREENTOCLIENT   0x001d  /* 6 args              */
#define WOWUSER_STC_ARG_POINT    0
#define WOWUSER_STC_ARG_HWND     4

/* ⚠⚠ `INVR_`, NOT `IR_` -- AND THAT RENAME IS A BUG FIX, NOT TIDYING.
     InvalidateRect above already owns `WOWUSER_IR_ARG_RECT` and sets it to **2**. This
     block used to redefine it to **0**, and because the preprocessor takes the
     last definition before the use, BOTH handlers read offset 0 -- so every
     InvalidateRect in this host has been fetching its `lpRect` out of `bErase`,
     getting a junk far pointer, and falling into the NULL path that invalidates
     THE WHOLE CLIENT AREA. It never looked broken because over-invalidating
     still repaints correctly; it just repaints everything, every time.
   ★ The only evidence was a `warning: 'WOWUSER_IR_ARG_RECT' redefined` that had been in
     the build output all along. A prefix here is a namespace, and a collision in
     it is a silent wrong answer -- exactly the class this project treats as most
     expensive. (found while adding SetMenu, which collided the same way) */
#define WOWUSER_INVERTRECT       0x0052  /* 6 args (hDC, lpRect) */
#define WOWUSER_INVR_ARG_RECT    0
#define WOWUSER_INVR_ARG_HDC     4

#define WOWUSER_GLOBALADDATOM    0x010c  /* 4 args, far LPCSTR */
#define WOWUSER_GLOBALDELATOM    0x010d  /* 2 args             */

#define WOWUSER_SELECTPALETTE    0x011a  /* 6 args (hDC, hPal, bForce) */
#define WOWUSER_SPL_ARG_FORCE    0
#define WOWUSER_SPL_ARG_PAL      2
#define WOWUSER_SPL_ARG_HDC      4
#define WOWUSER_REALIZEPALETTE   0x011b  /* 2 args (hDC) */

/* The caret -- five calls that stand or fall together, which is why they are one
   group here. MS Paint's Text tool needs all of them. */
#define WOWUSER_CREATECARET      0x00a3  /* 8 args */
#define WOWUSER_CC_ARG_HEIGHT    0
#define WOWUSER_CC_ARG_WIDTH     2
#define WOWUSER_CC_ARG_BITMAP    4
#define WOWUSER_CC_ARG_HWND      6
#define WOWUSER_DESTROYCARET     0x00a4  /* 0 args */
#define WOWUSER_SETCARETPOS      0x00a5  /* 4 args */
#define WOWUSER_HIDECARET        0x00a6  /* 2 args */
#define WOWUSER_SHOWCARET        0x00a7  /* 2 args */

#define WOWUSER_SETWINDOWPOS     0x00e8  /* 14 args */
#define WOWUSER_SWP_ARG_FLAGS    0
#define WOWUSER_SWP_ARG_CY       2
#define WOWUSER_SWP_ARG_CX       4
#define WOWUSER_SWP_ARG_Y        6
#define WOWUSER_SWP_ARG_X        8
#define WOWUSER_SWP_ARG_AFTER   10
#define WOWUSER_SWP_ARG_HWND    12

#define WOWUSER_GETSCROLLPOS     0x003f  /* 4 args (hWnd, nBar) */
#define WOWUSER_GSP_ARG_BAR      0
#define WOWUSER_GSP_ARG_HWND     2

/* ── ★★★★★ THE OLE CLUSTER -- WHAT `File > Save As` DIES ON NOW. (session 49) ─
     With the LDT collision fixed, MS Paint's save runs, reads its whole canvas
     with `GetDIBits`, and then faults inside **OLESVR.DLL** -- because Paint
     registers itself as an OLE server and OLESVR notifies its clients that the
     document changed. The host's fault frame gives the reason: ES = 0x0000 {NO
     DESCRIPTOR}, BX = 0, i.e. a NULL far pointer dereferenced without a check.
     Three lines of log before it say where the null came from:

       FUNC=0x2e from=OLESVR (0x0200) -> UNIMPLEMENTED, answered 0
       FUNC=0x35 ... DestroyWindow 0x0200 -> destroyed
       FUNC=0x87 from=OLESVR (0,0) -> GetWindowLong(0x0000,0)
                                      ★ NOT ONE OF OUR WINDOWS; 0

     `USER.46 GetParent` was stepped over, so OLESVR asked window 0 for its
     window long, got 0, and dereferenced it. ⇒ **`GetParent` is the whole bug.**
   ★ AND THE ANSWER IS KNOWN TO BE RIGHT BEFORE THE RUN. Window `0x0200` is
     `CreateWindow("DocWndClass","Doc", style=0x40000000 = WS_CHILD)` and its
     argument block carries `hwndParent = 0x0140` -- and thirty lines earlier the
     log has `SetWindowLong(0x0140, 0000, 0x09b70000)`, which is OLESVR storing
     its server object on exactly that window. So `GetParent(0x200) -> 0x140`
     makes `GetWindowLong(0x140,0)` hand back the pointer it stored itself.
   ⚠ The rest of this cluster is everything else OLESVR reaches that is still
     stepped over, because a null from any of them lands the same way. */
#define WOWUSER_GETPARENT        0x002e  /* ord 46,  2 args  ★ THE FIX */
#define WOWUSER_GETWINDOW        0x0106  /* ord 262, 4 args            */
#define WOWUSER_GW_ARG_CMD       0
#define WOWUSER_GW_ARG_HWND      2
#define WOWUSER_GETCLASSNAME     0x003a  /* ord 58,  8 args            */
#define WOWUSER_GCN_ARG_MAX      0
#define WOWUSER_GCN_ARG_BUF      2       /* far */
#define WOWUSER_GCN_ARG_HWND     6
#define WOWUSER_GETWINDOWTASK    0x00e0  /* ord 224, 2 args            */

/* The window property list. ⚠ A property NAME is either a far string or an ATOM
   in the low word of a far pointer whose SELECTOR IS ZERO (MAKEINTATOM) -- so a
   lookup that only understands strings finds nothing and a store that only
   understands strings keeps nothing, which is the same null-pointer ending. Both
   forms are canonicalised to one key here, atoms as "#nnnn". */
#define WOWUSER_REMOVEPROP       0x0018  /* ord 24,  6 args */
#define WOWUSER_GETPROP          0x0019  /* ord 25,  6 args */
#define WOWUSER_SETPROP          0x001a  /* ord 26,  8 args */
#define WOWUSER_PROP_ARG_NAME_G  0       /* Get/Remove: far name @0, hWnd @4 */
#define WOWUSER_PROP_ARG_HWND_G  4
#define WOWUSER_PROP_ARG_DATA_S  0       /* Set: data @0, far name @2, hWnd @6 */
#define WOWUSER_PROP_ARG_NAME_S  2
#define WOWUSER_PROP_ARG_HWND_S  6

#define WOWUSER_GLOBALFINDATOM   0x010e  /* ord 270, 4 args */
#define WOWUSER_GLOBALATOMNAME   0x010f  /* ord 271, 8 args */
#define WOWUSER_GAN_ARG_SIZE     0
#define WOWUSER_GAN_ARG_BUF      2       /* far */
#define WOWUSER_GAN_ARG_ATOM     6

#define WOWUSER_MAX_PROP 64
typedef struct _WOWUSER_PROP { WORD Window; WORD Data; char Name[WOWUSER_SHORT_NAME_SIZE]; } WOWUSER_PROP;
static WOWUSER_PROP g_WowUserProps[WOWUSER_MAX_PROP];
static INT            g_WowUserPropCount = 0;

/* The Win16 task that is running right now. ⚠ NOT invented and not derived here:
   it is krnl386's own current-task word -- the DGROUP word at offset 0x228,
   observed at run time (session 38) to hold the running task's handle -- which
   the dispatcher already reads at every BOP for the log -- this just keeps the
   last value where `GetWindowTask` can see it. 0 until the first BOP. */
static WORD g_WowUserCurrentTask = 0;

/* ── s93: THE HOOK BRIDGE (see SetWindowsHook). One entry per Win16 hook; the
     Win32 hook's callback finds its entry by kind and calls the 16-bit procedure
     through the nested run, with the procedure's own DS (its module's DGROUP). */
static INT WowCall16SyncEx(DWORD proc, WORD ds, PCWORD args, INT n,
                              WORD hwnd, WORD msg, PWORD res,
                              PBYTE blob, INT blobLength, INT blobArgument,
                              const INT *fix, INT nfix);
#define WOWUSER_HOOKS 8
#define WOWUSER_HOOK_ARGUMENTS      4    /* nCode, wParam, lParam high, low            */
#define WOWUSER_HOOK_ARG_LPARAM     2    /* lParam's high word: the EVENTMSG's pointer */
#define WOWUSER_HOOK_RECORDED_ONLY  2    /* WowUserHookSet: a kind with no Win32 hook  */
/* Win16's EVENTMSG -- what a journal hook reads and writes. */
#define WOWUSER_EVENTMSG16_MESSAGE  0
#define WOWUSER_EVENTMSG16_PARAML   2
#define WOWUSER_EVENTMSG16_PARAMH   4
#define WOWUSER_EVENTMSG16_TIME     6
#define WOWUSER_EVENTMSG16_SIZE     10
typedef struct _WOWUSER_HOOK { SHORT Id; DWORD Procedure; WORD DataSelector; HHOOK Hook32; } WOWUSER_HOOK; static WOWUSER_HOOK g_WowUserHooks[WOWUSER_HOOKS];

static INT WowUserHookFind(SHORT hookId)
{
    INT index;
    for (index = 0; index < WOWUSER_HOOKS; ++index)
        if (g_WowUserHooks[index].Procedure && g_WowUserHooks[index].Id == hookId) return index;
    return -1;
}
/* code, wParam, lParam -> the 16-bit procedure; lParam may be a 10-byte EVENTMSG
   blob (Win16: message, paramL, paramH WORDs, then a DWORD time). */
static INT WowUserHookCall(INT slot, INT code, WORD wParam, DWORD lParam, PBYTE eventBlob, PWORD result)
{
    WORD arguments[WOWUSER_HOOK_ARGUMENTS];
    arguments[0] = (WORD)code; arguments[1] = wParam;
    arguments[2] = (WORD)(lParam >> WORD_SHIFT); arguments[3] = (WORD)lParam;
    return WowCall16SyncEx(g_WowUserHooks[slot].Procedure, g_WowUserHooks[slot].DataSelector, arguments, WOWUSER_HOOK_ARGUMENTS, 0, 0, result,
                              eventBlob, eventBlob ? WOWUSER_EVENTMSG16_SIZE : 0, eventBlob ? WOWUSER_HOOK_ARG_LPARAM : -1, NULL, 0);
}
static VOID WowUserEventTo16(const EVENTMSG *eventMessage, PBYTE blob)
{
    blob[WOWUSER_EVENTMSG16_MESSAGE] = (BYTE)eventMessage->message; blob[WOWUSER_EVENTMSG16_MESSAGE + 1] = (BYTE)(eventMessage->message >> BYTE_SHIFT);
    blob[WOWUSER_EVENTMSG16_PARAML] = (BYTE)eventMessage->paramL;  blob[WOWUSER_EVENTMSG16_PARAML + 1] = (BYTE)(eventMessage->paramL >> BYTE_SHIFT);
    blob[WOWUSER_EVENTMSG16_PARAMH] = (BYTE)eventMessage->paramH;  blob[WOWUSER_EVENTMSG16_PARAMH + 1] = (BYTE)(eventMessage->paramH >> BYTE_SHIFT);
    blob[WOWUSER_EVENTMSG16_TIME] = (BYTE)eventMessage->time; blob[WOWUSER_EVENTMSG16_TIME + 1] = (BYTE)(eventMessage->time >> BYTE_SHIFT);
    blob[WOWUSER_EVENTMSG16_TIME + 2] = (BYTE)(eventMessage->time >> WORD_SHIFT); blob[WOWUSER_EVENTMSG16_TIME + 3] = (BYTE)(eventMessage->time >> TOP_BYTE_SHIFT);
}
static LRESULT CALLBACK WowUserHookKeyboard(INT code, WPARAM wParam, LPARAM lParam)
{
    INT slot = WowUserHookFind(WH_KEYBOARD);
    WORD result = 0;
    if (code >= 0 && slot >= 0 && WowUserHookCall(slot, code, (WORD)wParam, (DWORD)lParam, NULL, &result) && result)
        return 1;                                      /* the program swallowed it */
    return CallNextHookEx(NULL, code, wParam, lParam);
}
/* ⚠ ONE AT A TIME, IN ORDER. The nested run that calls the program pumps Win32
     messages, and each input event retrieved there calls this hook again -- measured
     six deep on the rig, which reaches the nesting limit and drops events. So an
     event that arrives while one is being recorded is queued and handed over, in
     order, when the outer call returns. */
#define WOWUSER_JREC_Q 64
static BYTE g_WowUserJournalQueue[WOWUSER_JREC_Q][WOWUSER_EVENTMSG16_SIZE];
static INT  g_WowUserJournalQueueCount, g_WowUserIsJournalRecordBusy;
static LRESULT CALLBACK WowUserHookJournalRecord(INT code, WPARAM wParam, LPARAM lParam)
{
    INT slot = WowUserHookFind(WH_JOURNALRECORD);
    if (code == HC_ACTION && lParam && slot >= 0) {
        BYTE blob[WOWUSER_EVENTMSG16_SIZE];
        WORD result = 0;
        INT queued;
        WowUserEventTo16((const EVENTMSG *)lParam, blob);
        if (g_WowUserIsJournalRecordBusy) {
            if (g_WowUserJournalQueueCount < WOWUSER_JREC_Q) memcpy(g_WowUserJournalQueue[g_WowUserJournalQueueCount++], blob, WOWUSER_EVENTMSG16_SIZE);
            return CallNextHookEx(NULL, code, wParam, lParam);
        }
        g_WowUserIsJournalRecordBusy = 1;
        WowUserHookCall(slot, code, (WORD)wParam, 0, blob, &result);
        for (queued = 0; queued < g_WowUserJournalQueueCount; ++queued) {  /* those that came in meanwhile */
            BYTE queuedBlob[WOWUSER_EVENTMSG16_SIZE];
            memcpy(queuedBlob, g_WowUserJournalQueue[queued], WOWUSER_EVENTMSG16_SIZE);
            if ((slot = WowUserHookFind(WH_JOURNALRECORD)) < 0) break;  /* unhooked meanwhile */
            WowUserHookCall(slot, HC_ACTION, 0, 0, queuedBlob, &result);
        }
        g_WowUserJournalQueueCount = 0;
        g_WowUserIsJournalRecordBusy = 0;
    } else if (code >= 0 && slot >= 0 && !g_WowUserIsJournalRecordBusy) {
        WORD result = 0;
        WowUserHookCall(slot, code, (WORD)wParam, 0, NULL, &result);  /* HC_SYSMODALON/OFF */
    }
    return CallNextHookEx(NULL, code, wParam, lParam);
}
static LRESULT CALLBACK WowUserHookJournalPlayback(INT code, WPARAM wParam, LPARAM lParam)
{
    INT slot = WowUserHookFind(WH_JOURNALPLAYBACK);
    if (code >= 0 && slot >= 0) {
        BYTE blob[WOWUSER_EVENTMSG16_SIZE] = { 0 };
        WORD result = 0;
        if (code == HC_GETNEXT && lParam) {
            EVENTMSG *eventMessage = (EVENTMSG *)lParam;
            if (WowUserHookCall(slot, code, (WORD)wParam, 0, blob, &result)) {
                eventMessage->message = (UINT)(blob[WOWUSER_EVENTMSG16_MESSAGE] | (blob[WOWUSER_EVENTMSG16_MESSAGE + 1] << BYTE_SHIFT));
                eventMessage->paramL  = (UINT)(blob[WOWUSER_EVENTMSG16_PARAML] | (blob[WOWUSER_EVENTMSG16_PARAML + 1] << BYTE_SHIFT));
                eventMessage->paramH  = (UINT)(blob[WOWUSER_EVENTMSG16_PARAMH] | (blob[WOWUSER_EVENTMSG16_PARAMH + 1] << BYTE_SHIFT));
                eventMessage->time    = (DWORD)blob[WOWUSER_EVENTMSG16_TIME] | ((DWORD)blob[WOWUSER_EVENTMSG16_TIME + 1] << BYTE_SHIFT) | ((DWORD)blob[WOWUSER_EVENTMSG16_TIME + 2] << WORD_SHIFT)
                           | ((DWORD)blob[WOWUSER_EVENTMSG16_TIME + 3] << TOP_BYTE_SHIFT);
                eventMessage->hwnd    = NULL;
                return (LRESULT)result;                /* the delay before it */
            }
            return 0;
        }
        WowUserHookCall(slot, code, (WORD)wParam, 0, NULL, &result);
        return 0;
    }
    return CallNextHookEx(NULL, code, wParam, lParam);
}
static INT WowUserHookUnset(SHORT hookId, DWORD procedure);
/* 1 = installed, 2 = a kind not run here (recorded), 0 = refused. */
static INT WowUserHookSet(SHORT hookId, DWORD procedure, WORD dataSelector)
{
    INT index, slot = -1;
    HOOKPROC hookProcedure = NULL;
    INT kind32 = 0;
    for (index = 0; index < WOWUSER_HOOKS; ++index) if (!g_WowUserHooks[index].Procedure) { slot = index; break; }
    if (slot < 0 || !procedure) return 0;
    if (WowUserHookFind(hookId) >= 0) WowUserHookUnset(hookId, g_WowUserHooks[WowUserHookFind(hookId)].Procedure);
    switch (hookId) {
    case WH_JOURNALRECORD: hookProcedure = WowUserHookJournalRecord;  kind32 = WH_JOURNALRECORD;   break;
    case WH_JOURNALPLAYBACK: hookProcedure = WowUserHookJournalPlayback; kind32 = WH_JOURNALPLAYBACK; break;
    case WH_KEYBOARD: hookProcedure = WowUserHookKeyboard;   kind32 = WH_KEYBOARD;        break;
    default: break;
    }
    g_WowUserHooks[slot].Id = hookId; g_WowUserHooks[slot].Procedure = procedure; g_WowUserHooks[slot].DataSelector = dataSelector;
    g_WowUserHooks[slot].Hook32 = NULL;
    if (!hookProcedure) return WOWUSER_HOOK_RECORDED_ONLY;
    g_WowUserHooks[slot].Hook32 = SetWindowsHookExA(kind32, hookProcedure, GetModuleHandleA(NULL),
                                             kind32 == WH_KEYBOARD ? GetCurrentThreadId() : 0);
    if (!g_WowUserHooks[slot].Hook32) { g_WowUserHooks[slot].Procedure = 0; return 0; }
    return 1;
}
static INT WowUserHookUnset(SHORT hookId, DWORD procedure)
{
    INT index;
    for (index = 0; index < WOWUSER_HOOKS; ++index)
        if (g_WowUserHooks[index].Procedure && g_WowUserHooks[index].Id == hookId && (!procedure || g_WowUserHooks[index].Procedure == procedure)) {
            if (g_WowUserHooks[index].Hook32) UnhookWindowsHookEx(g_WowUserHooks[index].Hook32);
            g_WowUserHooks[index].Procedure = 0; g_WowUserHooks[index].Hook32 = NULL;
            return 1;
        }
    return 0;
}

/* ── s92 (#306): WHOSE FILE A RESOURCE IS IN. Every menu, icon, cursor and
     accelerator table used to be read from g_WowCommandProgram, the program on the
     command line -- right while there was one Win16 program, wrong for the next:
     WinHelp, started by Calc, came up with NO MENU because WINHELP.EXE's menu
     #0fa0 was looked for in CALC.EXE. The running task's module says which file:
     TDB+0x1E is hModule (TDB+0x1C, hInstance, is already read at (A)); the module
     database starts "NE", and its word at +0x0A points at the OFSTRUCT krnl386
     opened the file with, path at +8. Anything that does not check out falls back
     to the command-line program, which is what every single-task run had. */
#define WOWUSER_TDB_HMODULE        0x1E    /* TDB: the task's module             */
#define WOWUSER_NE_FILE_INFO       0x0A    /* module database: its OFSTRUCT      */
#define WOWUSER_NE_FILE_INFO_MIN   0x40    /* ...where a real one can lie        */
#define WOWUSER_NE_FILE_INFO_MAX   0x8000
#define WOWUSER_OFSTRUCT_PATH      8       /* OFSTRUCT.szPathName                */
#define WOWUSER_MIN_FULL_PATH      4       /* "C:\x"                             */
static DWORD DpmiSelectorBase(WORD selector);
static CHAR g_WowUserResourceProgram[MAX_PATH];
static PCSTR WowUserResourceProgram(VOID)
{
    WORD  task = g_WowUserCurrentTask, module, offset;
    DWORD taskBase, moduleBase;
    const volatile BYTE *taskBytes, *moduleBytes;
    INT   index;
    if (!task || task == WOWUSER_TASK_NONE16 || !(taskBase = DpmiSelectorBase(task))) return g_WowCommandProgram;
    taskBytes = (const volatile BYTE *)(ULONG_PTR)taskBase;
    module = (WORD)(taskBytes[WOWUSER_TDB_HMODULE] | (taskBytes[WOWUSER_TDB_HMODULE + 1] << BYTE_SHIFT));
    if (!module || !(moduleBase = DpmiSelectorBase(module))) return g_WowCommandProgram;
    moduleBytes = (const volatile BYTE *)(ULONG_PTR)moduleBase;
    if (moduleBytes[0] != 'N' || moduleBytes[1] != 'E') return g_WowCommandProgram;
    offset = (WORD)(moduleBytes[WOWUSER_NE_FILE_INFO] | (moduleBytes[WOWUSER_NE_FILE_INFO + 1] << BYTE_SHIFT));
    if (offset < WOWUSER_NE_FILE_INFO_MIN || offset > WOWUSER_NE_FILE_INFO_MAX) return g_WowCommandProgram;
    for (index = 0; index < (INT)sizeof g_WowUserResourceProgram - 1 && moduleBytes[offset + WOWUSER_OFSTRUCT_PATH + index]; ++index)
        g_WowUserResourceProgram[index] = (CHAR)moduleBytes[offset + WOWUSER_OFSTRUCT_PATH + index];
    g_WowUserResourceProgram[index] = 0;
    if (index < WOWUSER_MIN_FULL_PATH || g_WowUserResourceProgram[1] != ':'
        || GetFileAttributesA(g_WowUserResourceProgram) == INVALID_FILE_ATTRIBUTES)
        return g_WowCommandProgram;
    return g_WowUserResourceProgram;
}

#define WOWUSER_FILLRECT         0x0051
#define WOWUSER_FR_ARG_BRUSH     0
#define WOWUSER_FR_ARG_RECT      2
#define WOWUSER_FR_ARG_HDC       6

#define WOWUSER_SETSCROLLPOS     0x003e
#define WOWUSER_SSP_ARG_REDRAW   0
#define WOWUSER_SSP_ARG_POS      2
#define WOWUSER_SSP_ARG_BAR      4
#define WOWUSER_SSP_ARG_HWND     6

#define WOWUSER_SETSCROLLRANGE   0x0040
#define WOWUSER_SSR_ARG_REDRAW   0
#define WOWUSER_SSR_ARG_MAX      2
#define WOWUSER_SSR_ARG_MIN      4
#define WOWUSER_SSR_ARG_BAR      6
#define WOWUSER_SSR_ARG_HWND     8

#define WOWUSER_SETCURSOR        0x0045
#define WOWUSER_SC_ARG_HCURSOR   0

/* ── ★★★★★ 0x27 BeginPaint / 0x28 EndPaint -- WHERE A GUEST DRAWS. ──────────
     Both are internal stubs (their exports are native16), so the ids came from
     the run that first relayed WM_PAINT to a guest -- see wowwin.h. Each carries
     6 argument bytes and the same block: a far `lpPaint` at +0 and the hWnd at
     +4, and the hWnd that arrived was 0x0180, MS Paint's CANVAS window.

   ★★ THE PAINTSTRUCT LAYOUT -- the documented Win16 one: hdc at +0, fErase at
     +2, rcPaint at +4 as four `int`s. Checked on Paint: the HDC its GDI calls
     carry after BeginPaint is the word we wrote at +0.
       Win16's PAINTSTRUCT is 32 bytes (16 of them a reserved tail); Win32's is
       64 with LONGs, so this is a conversion like every other structure here.

   ⚠⚠ THE UPDATE REGION HAS ALREADY BEEN CONSUMED by the time the guest gets
     here -- WowWinProc had to validate it to stop Win32 re-synthesising
     WM_PAINT forever. So the rectangle comes out of the pending-paint record
     that kept it. A window with no record still gets a DC and its whole client
     rectangle, which is correct-but-wasteful rather than wrong. */
#define WOWUSER_BEGINPAINT       0x0027
#define WOWUSER_ENDPAINT         0x0028
#define WOWUSER_BP_ARG_PS        0
#define WOWUSER_BP_ARG_HWND      4
#define WOWUSER_PS16_HDC     0
#define WOWUSER_PS16_ERASE   2
#define WOWUSER_PS16_RECT    4
#define WOWUSER_PS16_SIZE     32

#define WOWUSER_GETDC            0x0042
#define WOWUSER_GETWINDOWDC      0x0043
#define WOWUSER_GDC_ARG_HWND16   0
#define WOWUSER_RELEASEDC        0x0044
#define WOWUSER_RDC_ARG_HDC      0
#define WOWUSER_RDC_ARG_HWND     2

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
#define WOWUSER_MI_ARG_FLAGS     0
#define WOWUSER_MI_ARG_ID        2
#define WOWUSER_MI_ARG_HMENU     4

#define WOWUSER_GETSYSTEMMENU    0x009c
#define WOWUSER_GSYM_ARG_REVERT  0
#define WOWUSER_GSYM_ARG_HWND    2
#define WOWUSER_GETMENU          0x009d
#define WOWUSER_GM2_ARG_HWND     0
#define WOWUSER_GETSUBMENU       0x009f
#define WOWUSER_GSM2_ARG_POS     0
#define WOWUSER_GSM2_ARG_HMENU   2

#define WOWUSER_ENDDIALOG        0x0058
#define WOWUSER_ED_ARG_RESULT    0
#define WOWUSER_ED_ARG_HDLG      2
#define WOWUSER_GETDLGITEM       0x005b
#define WOWUSER_GDI2_ARG_ID      0
#define WOWUSER_GDI2_ARG_HDLG    2
#define WOWUSER_GETDLGITEMTEXT   0x005d
#define WOWUSER_GDIT_ARG_MAX     0
#define WOWUSER_GDIT_ARG_BUF     2
#define WOWUSER_GDIT_ARG_ID      6
#define WOWUSER_GDIT_ARG_HDLG    8
#define WOWUSER_SETDLGITEMINT    0x005e
#define WOWUSER_SDII_ARG_SIGNED  0
#define WOWUSER_SDII_ARG_VALUE   2
#define WOWUSER_SDII_ARG_ID      4
#define WOWUSER_SDII_ARG_HDLG    6
#define WOWUSER_SENDDLGITEMMSG   0x0065
#define WOWUSER_SDIM_ARG_LPARAM  0
#define WOWUSER_SDIM_ARG_WPARAM  4
#define WOWUSER_SDIM_ARG_MSG     6
#define WOWUSER_SDIM_ARG_ID      8
#define WOWUSER_SDIM_ARG_HDLG   10

/* Menu tokens live between the window handles (0x0100 + n*0x20) and the
   cursor/icon tokens (0x8000 + n*8), so a stray one of any kind is recognisable
   on sight in a log rather than being mistaken for another kind of object. */
#define WOWUSER_MENU_BASE   0x4000
#define WOWUSER_MENU_STEP   0x0008
#define WOWUSER_MAX_MENU    64

typedef struct _WOWUSER_MENU { WORD Handle16; HMENU Menu; } WOWUSER_MENU;
static WOWUSER_MENU g_WowUserMenus[WOWUSER_MAX_MENU];
static INT            g_WowUserMenuCount = 0;

/* One token per HMENU: the OS hands back the same handle for the same menu, and
   a program that asks twice must get one answer, the way it does for a cursor. */
static WORD WowUserMenu16(HMENU menu)
{
    INT index;
    if (!menu) return 0;
    for (index = 0; index < g_WowUserMenuCount; ++index)
        if (g_WowUserMenus[index].Menu == menu) return g_WowUserMenus[index].Handle16;
    if (g_WowUserMenuCount >= WOWUSER_MAX_MENU) return 0;
    index = g_WowUserMenuCount++;
    g_WowUserMenus[index].Menu = menu;
    g_WowUserMenus[index].Handle16 = (WORD)(WOWUSER_MENU_BASE + index * WOWUSER_MENU_STEP);
    return g_WowUserMenus[index].Handle16;
}

static HMENU WowUserMenu32(WORD handle16)
{
    INT index;
    if (!handle16) return NULL;
    for (index = 0; index < g_WowUserMenuCount; ++index)
        if (g_WowUserMenus[index].Handle16 == handle16) return g_WowUserMenus[index].Menu;
    return NULL;
}

/* ⚠⚠ A Win16 RECT IS FOUR **WORDS**; A Win32 RECT IS FOUR **LONGS**. Eight bytes
     against sixteen, and nothing about a wrong reading looks wrong -- it just
     produces coordinates off by whatever the neighbouring field held. Anywhere a
     RECT crosses this boundary it is converted field by field, and this is the
     only place that says so. */
#define WOWUSER_RECT16_SIZE  8

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
/* The HICON behind an WOWUSER_AD_KIND_REALICON token, or NULL. Separate from the
   resolver below because it is a LOOKUP, not a resolution: nothing is loaded. */
static HICON WowUserSystemResourceRealIcon(WORD handle16)
{
    INT index;
    if (!handle16) return NULL;
    for (index = 0; index < g_WowUserSystemResourceCount; ++index)
        if (g_WowUserSystemResources[index].Handle16 == handle16 && g_WowUserSystemResources[index].Kind == WOWUSER_AD_KIND_REALICON)
            return g_WowUserSystemResources[index].RealIcon;
    return NULL;
}

/* Mint a token for an icon we already hold. ⚠ THE SAME HICON GETS THE SAME
   TOKEN: a guest that extracts the same icon twice and compares the handles must
   find them equal, which is the rule every other handle map here follows. */
static WORD WowUserSystemResourceMintIcon(HICON icon)
{
    INT index;
    if (!icon) return 0;
    for (index = 0; index < g_WowUserSystemResourceCount; ++index)
        if (g_WowUserSystemResources[index].Handle16 && g_WowUserSystemResources[index].Kind == WOWUSER_AD_KIND_REALICON
            && g_WowUserSystemResources[index].RealIcon == icon)
            return g_WowUserSystemResources[index].Handle16;
    for (index = 0; index < g_WowUserSystemResourceCount; ++index) if (!g_WowUserSystemResources[index].Handle16) break;
    if (index == g_WowUserSystemResourceCount) {
        if (g_WowUserSystemResourceCount >= WOWUSER_MAX_SYSRES) return 0;
        index = g_WowUserSystemResourceCount++;
    }
    g_WowUserSystemResources[index].Handle16    = (WORD)(WOWUSER_SYSRES_BASE + index * WOWUSER_SYSRES_STEP);
    g_WowUserSystemResources[index].Ordinal  = 0;
    g_WowUserSystemResources[index].Kind = WOWUSER_AD_KIND_REALICON;
    g_WowUserSystemResources[index].Name[0] = 0;
    g_WowUserSystemResources[index].RealIcon = icon;
    g_WowUserSystemResources[index].Cursor  = NULL;
    return g_WowUserSystemResources[index].Handle16;
}

static HICON WowUserSystemResourceIcon(WORD token, PINT picked, INT width, INT height)
{
    WORD ordinal  = WowUserSystemResourceOrdinal(token);
    WORD kind = WowUserSystemResourceKind(token);
    PCSTR name = WowUserSystemResourceName(token);
    if (picked) *picked = 0;
    /* ★ Already an object: hand it back. Nothing to load, nothing to guess. */
    if (kind == WOWUSER_AD_KIND_REALICON) return WowUserSystemResourceRealIcon(token);
    if (name)
        return WowResOpen(WowUserResourceProgram()) ? WowResIconNamed(name, picked, width, height)
                                           : NULL;
    if (!ordinal) return NULL;
    if (kind == WOWUSER_AD_KIND_MODULERES)
        return WowResOpen(WowUserResourceProgram()) ? WowResIcon(ordinal, picked, width, height) : NULL;
    /* ⚠ A PREDEFINED icon at an explicit size needs LoadImage, not LoadIcon --
         LoadIcon always gives SM_CXICON and the small one would be derived
         again, which is the defect this parameter exists to remove. */
    if (width || height)
        return (HICON)LoadImageA(NULL, MAKEINTRESOURCEA(ordinal), IMAGE_ICON,
                                 width, height, LR_SHARED | LR_DEFAULTCOLOR);
    return LoadIconA(NULL, MAKEINTRESOURCEA(ordinal));
}

/* ── ★ THE SAME TOKEN, RESOLVED AS A CURSOR. ────────────────────────────────
     The mirror of the function above, and it exists for the same reason: only
     the guest knows which of the two a token is, and it says so by which
     WNDCLASS field it drops it into. ⚠ `fell` reports "the OS did not know that
     predefined ordinal", which is a different failure from "this application has
     no such named cursor" -- the caller logs them differently because one is our
     assumption being wrong and the other is the guest's resource missing. */
/* s89 (#216): build a module's cursor from the RT_CURSOR bytes USER's
   LoadCursor has just locked and passed to 0xad (+10, size at +6). Better than
   the file: it is the resource of the module USER found, a DLL's included, and
   it is the exact entry LookupIconIdFromDirectoryEx picked. Win 3.x layout -- a
   4-byte hotspot, then the DIB -- which is what CreateIconFromResourceEx takes
   with fIcon FALSE at version 3.0. */
#define WOWUSER_CURSOR_HOTSPOT_SIZE    4
#define WOWUSER_BITMAPINFOHEADER_SIZE  40
#define WOWUSER_CURSOR_MAX_SIZE        0x10000
#define WOWUSER_ICON_RESOURCE_VERSION  0x00030000   /* CreateIconFromResourceEx: 3.x */
static VOID WowUserSystemResourcePrime(PWOWUSER_SYSRES resource, PCWOW32_FRAME frame,
                                 PSTR note, INT noteCapacity, PINT noteLength)
{
    volatile BYTE *bits = Wow32ArgPointer(frame, WOWUSER_AD_ARG_BITS);
    DWORD size = Wow32ArgDword(frame, WOWUSER_AD_ARG_SIZE);
    if (resource->Cursor) return;
    if (!bits || size < WOWUSER_CURSOR_HOTSPOT_SIZE + WOWUSER_BITMAPINFOHEADER_SIZE || size > WOWUSER_CURSOR_MAX_SIZE) {
        WowNotePut(note, noteCapacity, noteLength, " [no cursor bytes passed; built on first use]");
        return;
    }
    resource->Cursor = (HCURSOR)CreateIconFromResourceEx((PBYTE)bits, size, FALSE, WOWUSER_ICON_RESOURCE_VERSION,
                                               0, 0, LR_DEFAULTCOLOR);
    WowNotePut(note, noteCapacity, noteLength, resource->Cursor ? " [cursor BUILT from USER's bytes, cb=0x"
                                     : " [★ CreateIconFromResourceEx REFUSED cb=0x");
    WowNoteHex(note, noteCapacity, noteLength, size, WOW_HEX_WORD_DIGITS);
    WowNotePut(note, noteCapacity, noteLength, "]");
}

static HCURSOR WowUserSystemResourceCursor(WORD token, PINT fell)
{
    PWOWUSER_SYSRES resource = WowUserSystemResourceSlot(token);
    WORD ordinal  = WowUserSystemResourceOrdinal(token);
    PCSTR name = WowUserSystemResourceName(token);
    HCURSOR cursor;
    if (fell) *fell = 0;
    if (!resource) return NULL;
    if (resource->Cursor) return resource->Cursor;  /* built once (#216) */
    if (resource->Kind == WOWUSER_AD_KIND_REALICON) return (HCURSOR)resource->RealIcon;
    if (name) {
        cursor = WowResOpen(WowUserResourceProgram()) ? WowResCursorNamed(name) : NULL;
        resource->Cursor = cursor;
        return cursor;
    }
    if (!ordinal) return NULL;
    /* s89 (#216): a module's own cursor asked for BY ORDINAL is in its file,
       not in the system's set -- LoadCursorA(NULL, 2) would be a stranger's. */
    if (resource->Kind == WOWUSER_AD_KIND_MODULERES) {
        cursor = WowResOpen(WowUserResourceProgram()) ? WowResCursor(ordinal) : NULL;
        resource->Cursor = cursor;
        if (!cursor && fell) *fell = 1;
        return cursor;
    }
    cursor = LoadCursorA(NULL, MAKEINTRESOURCEA(ordinal));
    if (!cursor && fell) *fell = 1;
    return cursor;
}

/* ── ★ BOOL ScrollDC(HDC, int dx, int dy, LPRECT scroll, LPRECT clip,
                       HRGN update, LPRECT lprcUpdate) = 20 ────────────────────
     Win32 has the same call with the same seven arguments and the same meaning.
     What is NOT the same is the RECTANGLES: a Win16 RECT is 8 bytes and Win32's
     is 16 (see wowconv.h), so each one is read field by field and rebuilt, and
     the update rectangle is written back the same way. Passing a guest's RECT
     straight to Win32 would read this rectangle plus eight bytes of whatever
     follows it. */
#define WOWUSER_SCROLLDC         0x00dd
#define WOWUSER_SCRDC_ARG_LPRCUPDATE  0    /* far */
#define WOWUSER_SCRDC_ARG_HRGNUPDATE  4
#define WOWUSER_SCRDC_ARG_LPRCCLIP    6    /* far */
#define WOWUSER_SCRDC_ARG_LPRCSCROLL 10    /* far */
#define WOWUSER_SCRDC_ARG_DY         14
#define WOWUSER_SCRDC_ARG_DX         16
#define WOWUSER_SCRDC_ARG_HDC        18

/* ── ★ 0x1ce CalcChildScroll(HWND hwnd, WORD wScroll) = 4 ────────────────────
     An undocumented USER internal that PROGMAN calls: "recompute the scroll bars
     of this MDI client". ⚠ THE HONEST ANSWER HERE IS THAT THE OS ALREADY DID IT.
     Our MDICLIENT is the REAL Win32 system class (see WowUserEnsureSystemClasses),
     so its scroll bars are managed by Win32's own MDI client, not by anything
     this host keeps -- there is no state of ours to recalculate. That is a
     statement about our architecture, not a stub, and the log says it every
     time so a guest that visibly disagrees is a line to grep for. */
#define WOWUSER_CALCCHILDSCROLL  0x01ce
#define WOWUSER_CCS_ARG_SCROLL   0
#define WOWUSER_CCS_ARG_HWND     2

/* ── ★★ THE ENUMERATIONS. (session 57) One callback per item; the mechanism is
     in src/wow/wowenum.h and the argument blocks are reversed as always.
       BOOL EnumWindows(FARPROC lpEnumFunc, LPARAM lParam)              = 8
       BOOL EnumChildWindows(HWND hParent, FARPROC, LPARAM)             = 10
       BOOL EnumTaskWindows(HTASK hTask, FARPROC, LPARAM)               = 10
     ⚠ THE CALLBACK'S SIGNATURE IS (HWND, LPARAM) -- three words -- and its answer
       is a veto: 0 ends the enumeration and the function returns FALSE. */
#define WOWUSER_ENUMWINDOWS      0x0036
#define WOWUSER_EW_ARG_LPARAM    0       /* DWORD */
#define WOWUSER_EW_ARG_PROC      4       /* far   */
#define WOWUSER_ENUMCHILDWINDOWS 0x0037
#define WOWUSER_ECW_ARG_LPARAM   0
#define WOWUSER_ECW_ARG_PROC     4
#define WOWUSER_ECW_ARG_PARENT   8
#define WOWUSER_ENUMTASKWINDOWS  0x00e1
#define WOWUSER_ETW_ARG_LPARAM   0
#define WOWUSER_ETW_ARG_PROC     4
#define WOWUSER_ETW_ARG_TASK     8

/* ShowWindow(hWnd, nCmdShow) -- 4 bytes, reversed as always, and confirmed by the
   run: SYSEDIT's two calls carry `(0x0005 0x0160)` (the MDI client) and
   `(0x0001 0x0140)` (the frame). UpdateWindow(hWnd) -- 2 bytes. */
#define WOWUSER_SW_ARG_CMDSHOW  0
#define WOWUSER_SW_ARG_HWND     2
#define WOWUSER_UW_ARG_HWND     0

/* Get/SetWindowWord argument blocks, reversed as always (base = last push):
     GetWindowWord(hWnd, nIndex)               -> +0 nIndex, +2 hWnd
     SetWindowWord(hWnd, nIndex, wNewWord)     -> +0 value, +2 nIndex, +4 hWnd
   Confirmed against the run: SYSEDIT's `mpchild` WM_CREATE calls
   SetWindowWord(hwnd, 2, 0) and the frame carries `(0x0000 0x0002 <hwnd>)`. */
#define WOWUSER_GWW_ARG_INDEX   0
#define WOWUSER_GWW_ARG_HWND    2
#define WOWUSER_SWW_ARG_VALUE   0
#define WOWUSER_SWW_ARG_INDEX   2
#define WOWUSER_SWW_ARG_HWND    4

/* ── ★★★ SendMessage(hWnd, msg, wParam, lParam) -- 10 argument bytes ──────────
     `USER.111 SENDMESSAGE`, named from SYSEDIT's NE import relocations. The
     block is REVERSED from the parameter list as always (the base is the LAST
     push), and the run confirms every offset in one line: SYSEDIT's
     SendMessage(hwndMDIClient, WM_MDICREATE, 0, &stackStruct) arrives as
     `args=(0x2378 0x0a9f 0x0000 0x0220 0x0160)`. */
#define WOWUSER_SM_ARG_LPARAM   0
#define WOWUSER_SM_ARG_WPARAM   4
#define WOWUSER_SM_ARG_MSG      6
#define WOWUSER_SM_ARG_HWND     8

/* The messages this host names. `0x0220` is what SYSEDIT sends its MDI client,
   and its lParam is an MDICREATESTRUCT -- see below. */
#define WM_MDICREATE16  0x0220

/*
 * ── ★★★★ AN EDIT CONTROL'S TEXT IS A HANDLE IN THE APPLICATION'S OWN HEAP ────
 * `0x040C` and `0x040D` are `WM_USER + 12` and `WM_USER + 13`, and which is
 * which is settled by what SYSEDIT does with them when it loads a file
 * (observed call sequence):
 *
 *   OpenFile, _llseek to the end (★ the file's SIZE), _llseek back to the start
 *   SendMessage(hEdit, 0x40D, 0, 0)           ; ★ so 0x40D takes NO parameters...
 *   LocalReAlloc(<that answer>, size + 1, 0x42) ; ★ ...and RETURNS A LOCAL HANDLE
 *   LocalLock, _lread the file straight in, NUL-terminate it, LocalUnlock
 *   SendMessage(hEdit, 0x40C, hMem, 0)        ; ★ 0x40C TAKES the handle
 *
 * ⇒ `0x040D` is **EM_GETHANDLE** and `0x040C` is **EM_SETHANDLE** (as documented),
 *   and the run that stopped here was stopping on the FIRST of the pair.
 *
 * ★★ AND THE HANDLE MUST BE VALID IN THE APPLICATION'S OWN LOCAL HEAP. That is
 *   not an assumption about how Windows implements edit controls -- it is what
 *   this program demonstrably requires: it hands the answer straight to
 *   `LocalReAlloc` and `LocalLock`, which operate on the local heap of the
 *   CURRENT DS, and DS throughout is SYSEDIT's own DGROUP. A handle this host
 *   invented would be a number `LocalReAlloc` rejects, and rejecting it is
 *   exactly what produced *"Cannot open this file."*
 * ⇒ The host cannot make this handle. The GUEST'S KERNEL has to, and since
 *   session 40 the host can ask it: `KERNEL.5 LOCALALLOC` is, in krnl386.exe's
 *   NE entry table (documented format), `FIXED, segment 1, offset 0x3ddb`, and
 *   segment 1 is the code segment every WOW32 BOP arrives from -- so its runtime
 *   address is `<the BOP's CS>:0x3ddb`, with no resolution machinery at all.
 *   Called with the documented LocalAlloc(flags, cb) -- two words, far.
 */
#define EM_SETHANDLE16  0x040C
#define EM_GETHANDLE16  0x040D
#define WOWUSER_KRNL_LOCALALLOC_OFF 0x3ddb
/* ★ AND ITS NEIGHBOURS -- the names from krnl386.exe's non-resident name table,
     the offsets from its NE entry table (both documented NE structures):
        5 LOCALALLOC   0x3ddb      7 LOCALFREE    0x3df7
        6 LOCALREALLOC 0x3e1f      8 LOCALLOCK    0x3e0b
                                   9 LOCALUNLOCK  0x3e55
   Called with the documented signatures: `LocalLock(HLOCAL)` and
   `LocalUnlock(HLOCAL)` take one WORD, far;
   `HLOCAL LocalReAlloc(HLOCAL, WORD cbNew, WORD flags)` three words, far --
   the order SYSEDIT's own call passes them in (above). */
#define WOWUSER_KRNL_LOCALREALLOC_OFF 0x3e1f
#define WOWUSER_KRNL_LOCALLOCK_OFF   0x3e0b
#define WOWUSER_KRNL_LOCALUNLOCK_OFF 0x3e55
#define WOWUSER_LOCALALLOC_ARGUMENTS   2   /* flags, cb          */
#define WOWUSER_LOCALREALLOC_ARGUMENTS 3   /* hMem, cbNew, flags */
/* ★ #160: the GLOBAL trio, from the same entry table (all FIXED, segment 1) and
     the same non-resident names -- 15 GLOBALALLOC, 18 GLOBALLOCK, 19 GLOBALUNLOCK
     -- cross-checked by the three Local* offsets above coming out of the same parse.
     Documented frames: GlobalAlloc(WORD flags, DWORD cb); GlobalLock/GlobalUnlock
     take one WORD. GlobalLock answers a far pointer in DX:AX. */
#define WOWUSER_KRNL_GLOBALALLOC_OFF  0x3ac3
#define WOWUSER_KRNL_GLOBALLOCK_OFF   0x3b10
#define WOWUSER_KRNL_GLOBALUNLOCK_OFF 0x3b63
/* LMEM_MOVEABLE | LMEM_ZEROINIT -- the same flags SYSEDIT itself passes to
   LocalReAlloc, so the block it grows is the kind it expects to be growing. */
#define LMEM_MOVEABLE_ZEROINIT 0x0042
/* Small on purpose: the guest reallocs it to the file's size before using it, so
   anything bigger would be memory the application immediately replaces. */
#define WOWUSER_EDIT_INITIAL   0x20

/* ── ★★★ NOTIFYWOW: "HERE IS A 16-BIT RESOURCE I HAVE JUST LOADED." ───────────
     Named by USER's own export table (`wowmap.py`: id 0x217, 6 argument bytes,
     NOTIFYWOW). It arrives inside every guest `LoadAccelerators`, with kind 3
     and a far pointer to a block describing the RT_ACCELERATOR resource USER
     has just found, loaded and locked. Answered 0, LoadAccelerators returns
     NULL (observed); answered non-zero, it returns the resource's own handle.

   ★ SO THE RETURN IS NOT A HANDLE. The application receives krnl386's global
     handle for the resource (hResData below) whichever non-zero value this
     answers. All this answer decides is whether LoadAccelerators SUCCEEDS.
     Returning a fabricated handle here would be inventing a value nobody reads;
     the honest answer is "noted", which is what the function's name says.

   ── The 12-byte block, as logged ─────────────────────────────────────────────
       +0x00 WORD  hInstance    ( the caller's module )
       +0x02 WORD  hResData     ( the resource's handle )
       +0x04 DWORD lpResource   ( 16:16 -- the bytes themselves )
       +0x08 DWORD cbResource   ( the resource's size )

   ⚠⚠ AND lpResource IS STALE THE MOMENT WE RETURN. USER unlocks the resource
     as soon as this call returns, so a host that recorded that
     pointer for a later TranslateAccelerator would be keeping an address the
     guest has already released -- an instrument that lies later, which is this
     project's most expensive shape. It is LOGGED, not kept. When accelerators
     are actually implemented, the bytes must be COPIED here, while they are
     locked, or asked for again through FindResource/LoadResource. */
#define WOWNOTIFY_ACCEL         3
/* ── ⛔⛔⛔ wKind 4: USER'S START-UP CALL -- AND WHY NO WIN16 X BUTTON EVER WORKED. ──
     (s88, user: "some close buttons (X) don't work") XP's 16-bit DefWindowProc
     (USER.107) forwards a message to WOW32 0x6b only if the message is no
     greater than a WORD maximum AND its bit is set in a message bitmap USER
     keeps -- NOTHING IS FORWARDED UNLESS ITS BIT IS SET, and as shipped the
     maximum is 0 and the bitmap is all zero. USER hands WOW32 pointers to both
     at start-up through NotifyWow(4, &block), for the 32-bit side to fill. This
     host stepped that call over, so DefWindowProc reached us ZERO times in 16
     shelf apps, and every app that leaves WM_CLOSE to DefWindowProc (Clock among
     them) could not end.
   ★ The block, as it arrives: +0x0e/+0x10 far ptr to the WORD maximum,
     +0x12/+0x14 far ptr to the bitmap, +0x16 its byte count (0x65 -> messages
     0..0x327).
   ⚠ ONLY MESSAGES THE 0x6b HANDLER CAN TAKE RAW. It passes wParam/lParam straight
     to DefWindowProcA; a message carrying a 16:16 pointer (WM_SETTEXT) or a GDI
     token (WM_ERASEBKGND's HDC) would hand Windows a value it cannot use. Each
     message joins this list when its parameters are translated AND a run needs it. */
#define WOWNOTIFY_USERINIT      4
#define WOWNOTIFY_FINDCLASS     6       /* "where is the window of this class?" */
#define WOWUSER_NOTIFY_UI_MAX_OFF       0x0e
#define WOWUSER_NOTIFY_UI_BITS_OFF      0x12
#define WOWUSER_NOTIFY_UI_BITS_CB       0x16
static const WORD g_WowUserDefWindowProcForwarded[] = {
    WM_CLOSE,           /* WM_CLOSE: its default is DestroyWindow (see 0x6b) */
    WM_ERASEBKGND,      /* WM_ERASEBKGND: its DC token is translated (s89)   */
    WM_PAINT,           /* WM_PAINT: the default erases + validates (s89)    */
};
#define WOWUSER_NOTIFY_ARG_BLOCK        0
#define WOWUSER_NOTIFY_ARG_KIND         4
#define WOWUSER_NOTIFY_HINSTANCE        0x00
#define WOWUSER_NOTIFY_HRESDATA         0x02
#define WOWUSER_NOTIFY_LPRESOURCE       0x04
#define WOWUSER_NOTIFY_CBRESOURCE       0x08

/*
 * ── ★★★ CreateWindow's ARGUMENT BLOCK, CHECKED AGAINST WOWEXEC'S CALL ───────
 * The arg block grows the opposite way from the pushes, so "lpClassName is the
 * first parameter" says nothing about where it lands. The block base is the
 * LOWEST address, which holds the LAST word pushed -- so the parameter list is
 * reversed, and a DWORD's high word is at the LOWER offset because Pascal pushes
 * it first. `USER.41 CREATEWINDOW` arrives with exactly 30 argument bytes.
 *
 *   +26/+28 lpClassName (far)  |  +14 y
 *   +22/+24 lpWindowName (far) |  +12 nWidth
 *   +18/+20 dwStyle            |  +10 nHeight
 *   +16 x                      |  +8  hWndParent
 *                              |  +6  hMenu
 *                              |  +4  hInstance
 *                              |  +0/+2 lpParam
 *
 * ★ AND THE DATA WOWEXEC's CALL CARRIES CROSS-VALIDATES THE LAYOUT (as logged):
 *     +26/+28  the string "WOWExecClass" -- the class WOWEXEC has just
 *              registered (a second copy of the literal from the one in its
 *              WNDCLASS; both decode to the same name).
 *     +18/+20  0x02CF0000 = WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN. Read the other
 *              way round it would be 0x000002CF, which is not a window style.
 *     +10..+16 four copies of 0x8000 = CW_USEDEFAULT, exactly where x/y/w/h are.
 *     +4       the SAME word WOWEXEC put into WNDCLASS.hInstance.
 *   Four independent agreements. A wrong offset assignment produces none of them.
 */
#define WOWUSER_CW_ARG_LPPARAM      0
#define WOWUSER_CW_ARG_HINSTANCE    4
#define WOWUSER_CW_ARG_HMENU        6
#define WOWUSER_CW_ARG_HWNDPARENT   8
#define WOWUSER_CW_ARG_HEIGHT       10
#define WOWUSER_CW_ARG_WIDTH        12
#define WOWUSER_CW_ARG_Y            14
#define WOWUSER_CW_ARG_X            16
#define WOWUSER_CW_ARG_STYLE        18
#define WOWUSER_CW_ARG_WINDOWNAME   22
#define WOWUSER_CW_ARG_CLASSNAME    26

/* ── ★★ 0x1c4 CreateWindowEx -- AND ITS BLOCK IS CreateWindow's WITH ONE FIELD
     ON THE END. (session 55) WINFILE.EXE imports it and creates no window at
     all without it.
     CreateWindowEx pushes dwExStyle FIRST, and the first push sits at the
     HIGHEST offset -- so every one of the eleven offsets above is unchanged and
     the extra DWORD lands at +30. 4+4+4+4+2+2+2+2+2+2+2+4 = 34, which is what
     the stub declares. That is why this shares the CreateWindow body outright
     rather than getting a copy of it: two copies of a window builder is how two
     window builders come to disagree. */
#define WOWUSER_CREATEWINDOWEX  0x01c4   /* ord 452, 34 args */
#define WOWUSER_CWX_ARG_EXSTYLE     30

/* Win16's CW_USEDEFAULT, and what this host resolves it to.
   ★ SESSION 42: IT RESOLVES TO Win32's. This used to be a stated placeholder --
     "there is no desktop yet, so there is no honest answer" -- and the answer
     turned out not to be a better number but a better question: the window is a
     REAL Win32 window on the real desktop, so `CW_USEDEFAULT` is handed to the
     OS's own window manager, which is what it means. ⚠ The two constants are NOT
     the same value (`0x8000` here, `0x80000000` there); see WowWinCoordinate.
   The DESK_* numbers survive only as the fallback for a rectangle asked about
   before a window exists. */
/* ⚠ CW_USEDEFAULT16 lives in wowwin.h, beside the Win32 value it is NOT. */
#define WOWUSER_DESK_CX     640
#define WOWUSER_DESK_CY     480

/* ── TWO STYLE BITS, BELIEVED BECAUSE FOUR WINDOWS AGREE ──────────────────────
     WS_VISIBLE  `mpframe` (0x02cf0000) does NOT have it, and is the one window
                 SYSEDIT calls ShowWindow on (as logged). `MDICLIENT`
                 (0x42300000) and `EDIT` (0x513000c4) DO have it and are never
                 shown explicitly, yet both must be visible.
     WS_CHILD    `EDIT` and `MDICLIENT` have 0x40000000; `mpframe` does not.
   ★ The Win16 and Win32 WS_* bits are the SAME VALUES -- Win32 inherited them --
     which is why a style word can be handed straight to CreateWindowEx while a
     CW_USEDEFAULT cannot. */
#define WS_VISIBLE16        0x10000000u
#define WS_CHILD16          0x40000000u

/* Win16 WNDCLASS field offsets -- see the note above. */
#define WOWUSER_WNDCLASS16_STYLE          0x00
#define WOWUSER_WNDCLASS16_WNDPROC        0x02
#define WOWUSER_WNDCLASS16_CLSEXTRA       0x06
#define WOWUSER_WNDCLASS16_WNDEXTRA       0x08
#define WOWUSER_WNDCLASS16_HINSTANCE      0x0a
#define WOWUSER_WNDCLASS16_HICON          0x0c
#define WOWUSER_WNDCLASS16_HCURSOR        0x0e
#define WOWUSER_WNDCLASS16_HBRBACKGROUND  0x10
/* The highest COLOR_* index a Win16 class can name in hbrBackground. Win 3.1 had
   COLOR_BTNHIGHLIGHT = 20 as its last; anything above that is a real brush
   handle, not a system colour. `mingw` has no COLOR_ENDCOLORS, and hard-coding
   Win32's larger set would let a stray handle pass as a colour index. */
#define WOWUSER_COLOR_MAX       20
#define WOWUSER_WNDCLASS16_MENUNAME       0x12
#define WOWUSER_WNDCLASS16_CLASSNAME      0x16
#define WOWUSER_WNDCLASS16_SIZE           0x1a

/* The control id Win32 gives an MDI client's first child. Any value works as long
   as it is above the ids a program uses for its own controls; this is the one
   Win32's own MDI samples use and it is above SYSEDIT's 0x0cac. */
#define WOWUSER_MDI_FIRSTCHILD 0xFF00

#define WOWUSER_MAX_CLASS 32

typedef struct _WOWUSER_CLASS {
    char  Name[WOWUSER_NAME_SIZE];                   /* char, not CHAR: the spelling moves code (#333) */
    WORD  Atom;
    WORD  Style;
    DWORD WindowProcedure;           /* 16:16 far pointer into the guest */
    WORD  Instance;
    WORD  Icon16, Cursor16, Background16;
    WORD  ClassExtra, WindowExtra;
    INT   IsSystemClass;             /* 1 = the SYSTEM provides it, not a program */
    /* ★ The REAL Win32 class this one is made from. For a program's class that is
         a prefixed clone of its name registered against our own window procedure;
         for a system class it is the OS's own name, because MDICLIENT and EDIT
         already exist and reimplementing them would be the whole mistake again. */
    char  Class32[WOWUSER_CLASS32_SIZE];
    INT   IsRegistered32;            /* 1 = a real Win32 class is behind this */
    /* The predefined ordinals resolved out of hCursor/hIcon at registration --
       kept only so the log can say what the class actually got. */
    WORD  CursorOrdinal, IconOrdinal, IconKind;
    INT   IsCursorUnknown;           /* the OS did not know that cursor ordinal */
    INT   IconBits;                  /* colour depth of the icon actually built  */
    /* What the class said its menu was -- a name, or an ordinal. Recorded and
       logged; not yet turned into a real HMENU (that needs the guest's own MENU
       resource, which lives in its NE file). */
    char  MenuName[WOWUSER_NAME_SIZE];
    WORD  MenuOrdinal;
} WOWUSER_CLASS, *PWOWUSER_CLASS; typedef const WOWUSER_CLASS *PCWOWUSER_CLASS;

static WOWUSER_CLASS g_WowUserClasses[WOWUSER_MAX_CLASS];
static INT             g_WowUserClassCount = 0;

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
 *   to do -- `CreateWindow("MDICLIENT", ...)` -- and this host answered "no such
 *   class", because nothing had ever registered it. SYSEDIT's MDI client handle
 *   stayed zero for a NEW reason, one step further on.
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
/* ⚠ `EDIT` IS HERE BECAUSE THE GUEST NAMES IT, not because it is on a list of
     system classes. SYSEDIT's `mpchild` creates a window of class `"edit"` in
     its own WM_CREATE handler -- the string sits in SYSEDIT's DGROUP next to
     `"mdiclient"` (which the frame procedure creates, as a run has shown) and
     `"mpchild"` (the class named in the MDICREATESTRUCT below). */
/* ⚠ `LISTBOX` JOINED THE LIST IN SESSION 53 BECAUSE A RUN NAMED IT, which is
     the rule two paragraphs up and not an exception to it: RECORDER.EXE stopped
     dead with `CreateWindow: no such class "ListBox"` and produced no window at
     all. The remaining standard classes (BUTTON, STATIC, COMBOBOX, SCROLLBAR,
     the numbered dialog classes) are still absent for the same reason as before
     -- nothing has asked -- and each is one string when something does.
   ★ The "a class that exists and does nothing" objection does NOT apply to these:
     a system class here resolves to the OS's OWN Win32 class, so a window made
     from it is a real listbox with real behaviour, not a stub. */
/* ⚠ THE DIALOG CONTROL CLASSES ARE HERE BECAUSE A DLGITEMTEMPLATE NAMES THEM BY
     NUMBER, NOT BY STRING. A Win16 dialog item encodes its class as a single
     byte 0x80..0x85, and those six values ARE these classes -- so a dialog
     cannot be built at all until each one resolves to something CreateWindow
     will accept. They are the OS's own classes, used as-is, for exactly the
     reason MDICLIENT and EDIT already are: a BUTTON that we drew ourselves
     would be a reimplementation of a control this machine already has.
   ★ "#32770" is the standard DIALOG class and it is a real class on XP, so a
     dialog whose template names NO class gets the OS's dialog window rather
     than one of ours. (session 55) */
static PCSTR const g_WowUserSystemClassNames[] = { "MDICLIENT", "EDIT", "LISTBOX",
                                             "BUTTON", "STATIC", "SCROLLBAR",
                                             "COMBOBOX", "#32770",
                                             "~FOREIGN" };  /* s91: see WowUserAlias16 */
static INT               g_WowUserIsSystemClassesDone = 0;

static VOID WowUserEnsureSystemClasses(VOID)
{
    UINT index;
    INT charIndex;
    if (g_WowUserIsSystemClassesDone) return;
    g_WowUserIsSystemClassesDone = 1;
    for (index = 0; index < sizeof g_WowUserSystemClassNames / sizeof g_WowUserSystemClassNames[0]; ++index) {
        PWOWUSER_CLASS windowClass;
        if (g_WowUserClassCount >= WOWUSER_MAX_CLASS) return;
        windowClass = &g_WowUserClasses[g_WowUserClassCount++];
        for (charIndex = 0; charIndex < (INT)sizeof windowClass->Name - 1 && g_WowUserSystemClassNames[index][charIndex]; ++charIndex)
            windowClass->Name[charIndex] = g_WowUserSystemClassNames[index][charIndex];
        windowClass->Name[charIndex]   = 0;
        windowClass->Atom      = (WORD)(MAXINTATOM + g_WowUserClassCount);
        windowClass->IsSystemClass  = 1;
        /* ── ★★★★★ `#32770` IS THE ONE SYSTEM CLASS WE MUST *NOT* USE AS-IS.
             (session 57, and the run named it) The rule below is right for four
             of these five and wrong for the fifth, and the difference is exactly
             whether the OS's implementation can reach OUR code:
               MDICLIENT / EDIT / LISTBOX / COMBOBOX -- the OS implements the
                 whole control, and what it does with a click is DRAW and EDIT.
                 Its answers are the ones we want; nothing has to come back.
               #32770 -- the OS implements a DIALOG MANAGER, and what it does
                 with a click is call the window's DLGPROC. Ours is 16-BIT code,
                 and there is no way to put a 16-bit procedure in a Win32
                 window's DWLP_DLGPROC slot. So a dialog built on the real class
                 is INERT: `DefDlgProc` handles everything and `WowWinProc`
                 never runs, which means no WM_COMMAND is ever relayed, no
                 WM_PAINT reaches the guest, and the modal loop below waits
                 forever for a message the OS quietly consumed.
           ⇒ MEASURED, TASKMAN on the rig, session 57. The modal loop ran, the
             dialog and its eight controls were built, and a click on Cancel at
             its measured screen position produced:
                 WOWDLG/win32: msg=0x0201 hwnd=0x003600e6 -> win16 0x01c0
                 ...  Win16 queued 0x00000000
             -- the button pressed, redrew itself, and sent its WM_COMMAND to a
             dialog procedure that is not ours. Nothing arrived, and the dialog
             could not be dismissed.
           ⇒ So this one gets OUR procedure, like every other Win16 window: it is
             a window whose messages belong to the guest, not a control whose
             behaviour belongs to the OS.
           ⚠ THE ROAD NOT TAKEN, and why: a 32-bit DLGPROC could be installed on
             the real class with SetWindowLongPtr(DWLP_DLGPROC), which would keep
             the OS's dialog manager -- tab order, mnemonics, ESC=IDCANCEL, the
             default button. That is real behaviour we are giving up here and it
             is worth having. It is not taken NOW because installing a dialog
             procedure on a window that USER32 did not create as a dialog is
             undocumented, and this host does not guess about undocumented
             structure when a plain answer works. Written down as the upgrade.
           ⚠ THE BACKGROUND BRUSH IS REASONED, NOT MEASURED: COLOR_BTNFACE is
             what a dialog is grey with on XP, and the run that found this showed
             WALLPAPER through the client area because the real class erases with
             a brush it only uses for windows it created itself. Stock ntvdm is
             the oracle for the exact colour and has not been asked yet. */
        if (windowClass->Name[0] == '#') {
            windowClass->IsRegistered32 = WowWinRegister(windowClass->Name, windowClass->Class32, sizeof windowClass->Class32,
                                       LoadCursorA(NULL, IDC_ARROW), NULL, NULL,
                                       NULL, (HBRUSH)(COLOR_BTNFACE + 1));
            continue;
        }
        /* ★ A SYSTEM CLASS IS THE OS's OWN, AND WE USE IT AS-IS. `MDICLIENT` and
             `EDIT` are real Win32 classes on this machine; registering clones of
             them against our procedure would be reimplementing an edit control
             and an MDI client that already exist -- which is the same mistake as
             drawing our own window frames. No prefix, no registration. */
        for (charIndex = 0; charIndex < (INT)sizeof windowClass->Class32 - 1 && windowClass->Name[charIndex]; ++charIndex)
            windowClass->Class32[charIndex] = windowClass->Name[charIndex];
        windowClass->Class32[charIndex] = 0;
        windowClass->IsRegistered32 = 1;
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
/* ⚠ 32 WAS ENOUGH UNTIL DIALOGS. A dialog is not one window, it is one window
     PER CONTROL -- CALC's `SciCalc` template alone is a dialog plus 15 items,
     and it opens that on top of the windows the program already has. At 32 the
     table ran out mid-dialog, which does not fail loudly: the controls simply
     stop being created and the dialog comes up half-built. (session 55) */
#define WOWUSER_MAX_WIN   128
#define WOWUSER_MAX_EXTRA 16            /* words -- 32 bytes of cbWndExtra */
#define WOWUSER_HWND_BASE 0x0100        /* first synthetic handle */
#define WOWUSER_HWND_STEP 0x0020        /* spaced so a stray +n is not a hit    */

typedef struct _WOWUSER_WINDOW {
    WORD  Window16;                  /* 0 = free slot */
    WORD  Class;                     /* index into g_WowUserClasses */
    DWORD Style;
    DWORD WindowProcedure;           /* copied from the class AT CREATION -- Win16
                                        keeps it per window, so a later
                                        RegisterClass cannot retarget this one */
    /* ── ★★★ AND THE OTHER PROCEDURE A WINDOW CAN HAVE. (session 57) ──────────
         A dialog created from a template that names no class is a `#32770`
         window -- a SYSTEM class, so `WindowProcedure` above is 0 and always was, which
         is correct and is also why a dialog could not be told anything. The
         thing that drives it is the DLGPROC the guest passed to
         DialogBox/CreateDialog, which is not a window procedure and does not
         live in a class: it belongs to this one window, for its lifetime.
       ⚠ NOT A FALLBACK FOR WindowProcedure AT LARGE. Where a template DOES name the
         application's own class (CALC's `SciCalc`) the class procedure is the
         one Windows calls, and this stays 0 unless the guest supplied one. The
         order is settled in WowUserWindowProcedureOf() and nowhere else. */
    DWORD DialogProcedure;
    INT   PositionX, PositionY, Width, Height;
    WORD  Parent, Menu, Instance;
    char  Text[WOWUSER_NAME_SIZE];                   /* char, not CHAR: the spelling moves code (#333) */
    /* ── ★★★ THE WINDOW'S EXTRA BYTES -- cbWndExtra, AND THEY ARE LOAD-BEARING.
         Not storage for its own sake: SYSEDIT keeps its EDIT control's handle
         and its file state in them. `mpchild`'s WNDCLASS declares
         `cbWndExtra = 8` (as it arrives in RegisterClass, the same block that
         carries `"mpchild"` at +0x16), its WM_CREATE writes indices 0/2/4/6, and
         it reads them back to address the control.
         With no store behind them every read answered 0 and the run reached
         `SendMessage: no such window 0x0000 msg 0x040d` -- EM_SETHANDLE to a
         window handle the program had just been told to forget. */
    WORD  Extra[WOWUSER_MAX_EXTRA];
    /* An EDIT control's text: a Win16 LOCAL handle in the APPLICATION's own
       heap, allocated by the guest's own KERNEL. See EM_GETHANDLE16. */
    WORD  Memory16;
    INT   MenuItems;                 /* how many entries its class menu produced */
    /* ★★★★★ THE REAL WINDOW. Session 42: a Win16 window IS a Win32 window on the
         XP desktop, and this is it. The Win16 handle above stays synthetic and
         16-bit because that is what the guest can hold; this is what the OS
         holds, and the pair of them is the whole of the bridge. */
    HWND  Window32;
    /* ★ DESTROYED, BUT NOT YET TOLD. Set between DestroyWindow tearing the real
         window down and the guest's own procedure receiving WM_DESTROY -- the
         record has to outlive the window by exactly that long, because
         DispatchMessage finds the procedure THROUGH it. Cleared when that
         message is dispatched. See both arms. */
    BYTE  IsDying;
    /* s89 (#283/#282): a dialog's base units, from its TEMPLATE font -- what
       its controls were laid out with, and what MapDialogRect must use. 0 = not
       a dialog (the system's base units apply). */
    WORD  DialogBaseUnitX, DialogBaseUnitY;
    /* s89: the template named a font (DS_SETFONT). Stock gives such a dialog the 3-D
       look -- its static text defaults to the button face (Charmap) -- and a dialog
       without one the window colour (Calc's display). Measured on those two. */
    BYTE  IsDialog3D;
    /* #308 (s91): A SUBCLASSED SYSTEM CONTROL. `SubclassProcedure` is the guest's 16:16
       procedure installed by SetWindowLong(GWL_WNDPROC) on a real Win32 control
       (EDIT, LISTBOX...), kept apart from `WindowProcedure` on purpose: every check of
       `WindowProcedure` in this file means "a window whose CLASS is 16-bit", and this one is
       not. `OriginalProcedure32` is the control's own Win32 procedure, displaced by
       WowUserSubclassProcedure. 0/NULL = not subclassed. */
    DWORD   SubclassProcedure;
    WNDPROC OriginalProcedure32;
    /* s91: an ALIAS -- a Win16 handle for a window that is NOT the guest's (another
       program's top-level window), minted by WowUserAlias16 for GetWindow. No
       procedure, never destroyed by us, skipped by everything that means "ours". */
    BYTE    IsForeign;
    /* s92 (#306): the task that created it -- whose queue its posted messages are
       in. 0 = not known; such a window's messages go to whichever task asks. */
    WORD    Task;
} WOWUSER_WINDOW, *PWOWUSER_WINDOW; typedef const WOWUSER_WINDOW *PCWOWUSER_WINDOW;

static WOWUSER_WINDOW g_WowUserWindows[WOWUSER_MAX_WIN];
static INT           g_WowUserWindowCount = 0;
/* s92 (#306): the hTask an EnumTaskWindows walk is for (wowenum.h); 0 = any. */
static WORD          g_WowUserEnumTask = 0;

/* s93: is this one of our windows driven by a DIALOG procedure? (wowwin.h) */
static INT WowUserIsDialog16(WORD window16)
{
    INT index;
    for (index = 0; index < g_WowUserWindowCount; ++index)
        if (g_WowUserWindows[index].Window16 == window16) return g_WowUserWindows[index].DialogProcedure != 0;
    return 0;
}

/* s92 (#306): whose queue a window's messages are in -- wowmsg.h's g_WowMsgOwner. */
static WORD WowUserOwner16(WORD window16)
{
    INT index;
    for (index = 0; index < g_WowUserWindowCount; ++index)
        if (g_WowUserWindows[index].Window16 == window16) return g_WowUserWindows[index].Task;
    return 0;
}

/* ── krnl386's SEGMENT 1, AS A LIVE SELECTOR. ─────────────────────────────────
     Needed to call KERNEL exports (see EM_GETHANDLE16), and it costs nothing to
     know: the WOW32 common thunk lives in krnl386's segment 1, so the CS at
     every WOW32 BOP IS that selector. The host records it there rather than
     looking it up -- `wow_module_of_sel()` is a bind-stage table and cannot name
     a selector krnl386 allocated at run time, which is the same trap that made
     the id-space label print `?` about a segment the dispatcher had identified. */
static WORD g_WowUserKernelSegment = 0;

/* ══ #308 (s91): SUBCLASSING. ═══════════════════════════════════════════════════════
   ★ THE SHAPE OF THE ANSWER. XP's USER.EXE exports one 16-bit procedure per
     system control -- EDITWNDPROC (301), BUTTONWNDPROC (303), STATICWNDPROC (302),
     SBWNDPROC (304), LBOXCTLWNDPROC (307), the combo box's (344), MDICLIENTWNDPROC
     (444) -- at the segment-1 entry-table offsets in the table below. Calling one
     behaves as CallWindowProc on ITSELF: it reaches us as CallWindowProc with its
     own address as the procedure, and the 32-bit side knows from that to call the
     real control. So that address is the right answer to GetWindowLong(
     GWL_WNDPROC) for a system control: a subclass that chains --
     CallWindowProc(old, ...) or a direct far call to `old` -- comes back here.
     Each carries the marker bytes 'SCLS' and its class index at +0x34, which
     this host checks before trusting the offset. This host:
       GetWindowLong  -> that export's own 16:16 address (marker checked first)
       SetWindowLong  -> the real control is subclassed with WowUserSubclassProcedure, which
                         SENDS its input/focus messages to the guest's procedure
                         through the nested run (g_WowUserCall16, main.c)
       CallWindowProc(<one of these>) -> the control's own Win32 procedure.
   ⚠ WHAT THE GUEST'S PROCEDURE SEES: the messages whose parameters mean the same in
     Win16 and Win32 -- keys, characters, mouse, focus, WM_GETDLGCODE, WM_SETCURSOR,
     WM_NCHITTEST, WM_TIMER, WM_ENABLE, WM_CANCELMODE. Everything else (WM_PAINT,
     text and EM_/LB_ messages with pointers) goes straight to the control. Those are
     the ones subclassers filter -- an edit control that refuses letters, a list box
     that drags -- and the rest would need the full 32->16 message translation. */
#define WOWUSER_STUB_SIGNATURE     0x34    /* "SCLS" in USER's class-procedure stub */
#define WOWUSER_STUB_CLASS_INDEX   0x38    /* ...then the class's index             */
typedef struct _WOWUSER_SYSPROC { PCSTR ClassName; WORD Offset; BYTE Index; } WOWUSER_SYSPROC, *PWOWUSER_SYSPROC; typedef const WOWUSER_SYSPROC *PCWOWUSER_SYSPROC;
static const WOWUSER_SYSPROC g_WowUserSystemProcedures[] = {
    { "BUTTON",    0x43f4, 3 }, { "COMBOBOX",  0x4434, 4 }, { "EDIT",      0x4474, 5 },
    { "STATIC",    0x4534, 6 }, { "LISTBOX",   0x44b4, 7 }, { "SCROLLBAR", 0x44f4, 8 },
    { "MDICLIENT", 0x4574, 11 },
};
#define WOWUSER_SYSPROC_COUNT ((INT)(sizeof g_WowUserSystemProcedures / sizeof g_WowUserSystemProcedures[0]))
/* main.c: a 16-bit procedure, now, through the nested run (WowCall16Sync). */
static INT (*g_WowUserCall16)(DWORD procedure, WORD dataSelector, PCWORD arguments, INT argumentCount,
                              WORD window16, WORD message, PWORD result);
static HWND g_WowUserSubclassBypass;  /* CallWindowProc(thunk) in progress for this HWND */
static UINT g_WowUserSubclassSent, g_WowUserSubclassDirect, g_WowUserSubclassChained;  /* for the STAGE2 line */

/* A free window slot with its synthetic handle assigned, or NULL. Factored out
   of CreateWindow the moment a SECOND thing started making windows -- the MDI
   client's WM_MDICREATE -- because two copies of a handle formula is how two
   windows come to share a handle. */
static PWOWUSER_WINDOW WowUserNewWindow(VOID)
{
    PWOWUSER_WINDOW window = NULL;
    INT index;
    for (index = 0; index < g_WowUserWindowCount; ++index) if (!g_WowUserWindows[index].Window16) { window = &g_WowUserWindows[index]; break; }
    if (!window) {
        if (g_WowUserWindowCount >= WOWUSER_MAX_WIN) return NULL;
        window = &g_WowUserWindows[g_WowUserWindowCount++];
    }
    window->Window16 = (WORD)(WOWUSER_HWND_BASE + (window - g_WowUserWindows) * WOWUSER_HWND_STEP);
    /* ⚠ THIS SLOT MAY BE A REUSED ONE, AND EVERY CALLER SETS THE FIELDS IT
         KNOWS ABOUT -- so a field only ONE caller sets has to be cleared here or
         it is inherited from whatever window used to live in this slot. That is
         the `f.cbact` trap in main.c exactly, and its cost was a callback
         running another call's action. `dlgproc` is that field: only a dialog
         sets it, and a plain window landing on a dead dialog's slot would
         otherwise be driven by a procedure that belongs to a window that no
         longer exists. */
    window->DialogProcedure = 0;
    window->DialogBaseUnitX = window->DialogBaseUnitY = 0;  /* the same trap: only a dialog sets them */
    window->IsDialog3D = 0;
    window->SubclassProcedure = 0; window->OriginalProcedure32 = NULL;  /* #308: only SetWindowLong sets them */
    window->IsForeign = 0;             /* s91: only WowUserAlias16 sets it */
    window->Task = (g_WowUserCurrentTask == WOWUSER_TASK_NONE16) ? 0 : g_WowUserCurrentTask;
    g_WowMsgOwner = WowUserOwner16;
    return window;
}

/* ── ★★ WHICH PROCEDURE DRIVES THIS WINDOW, AND IN WHICH ORDER. (session 57) ──
     ONE place decides, because two places deciding is two answers. The class's
     procedure wins where there is one -- that is the window Windows created and
     the procedure it calls -- and the dialog procedure is what a `#32770` window
     has INSTEAD, never as well. 0 means nothing can be told about this window,
     which is a fact its callers must handle rather than paper over. */
static DWORD WowUserWindowProcedureOf(PCWOWUSER_WINDOW window)
{
    if (!window) return 0;
    /* ★ The rule itself is in wowconv.h and pinned by wow_test.c; this is the
         table lookup around it. */
    return (DWORD)WowConvWindowProcedure((UINT)window->WindowProcedure, (UINT)window->DialogProcedure);
}

/* ── The modal dialog loop lives in wowdlg.h, which is included AFTER this file
     because it reads the window table above. These three are what USER's own
     DialogBox and EndDialog arms call into it. */
static INT WowDlgPush(WORD window, DWORD returnLinear, DWORD dialogProcedure, DWORD windowProcedure,
                       WORD dataSelector, INT isShowDeferred, HWND owner32);
static INT WowDlgEnd(WORD window, WORD result);
static VOID WowDlgSetInit(DWORD initParameter, WORD firstFocus);
static INT WowDlgActive(VOID);

/* ── s89 (#270): THE DESKTOP HAS A HANDLE. GetDesktopWindow used to answer 0,
     on the grounds that GetDC(0) is the screen -- but a program that CENTRES a
     dialog asks GetWindowRect(GetDesktopWindow()), and IsWindow of it must be
     TRUE (the Win16 test `user.desktop.*`). One record outside the table: no
     loop over g_WowUserWindows sees it, so it is never destroyed, enumerated or given a
     message; every handler that takes an hWnd finds the real desktop behind it.
     wndproc/dlgproc 0: nothing of the guest's is ever called for it. Below the
     first synthetic window handle, on the same 0x20 spacing. */
#define WOWUSER_HWND_DESKTOP 0x00e0
static WOWUSER_WINDOW g_WowUserDesktop;

static PWOWUSER_WINDOW WowUserFindWindow(WORD window16)
{
    INT index;
    if (!window16) return NULL;
    if (window16 == WOWUSER_HWND_DESKTOP) {
        if (!g_WowUserDesktop.Window32) {
            RECT rect;
            g_WowUserDesktop.Window16   = WOWUSER_HWND_DESKTOP;
            g_WowUserDesktop.Window32 = GetDesktopWindow();
            g_WowUserDesktop.Style  = (DWORD)GetWindowLongA(g_WowUserDesktop.Window32, GWL_STYLE);
            if (GetWindowRect(g_WowUserDesktop.Window32, &rect)) {
                g_WowUserDesktop.PositionX = rect.left;  g_WowUserDesktop.Width = rect.right - rect.left;
                g_WowUserDesktop.PositionY = rect.top;   g_WowUserDesktop.Height = rect.bottom - rect.top;
            }
        }
        return &g_WowUserDesktop;
    }
    for (index = 0; index < g_WowUserWindowCount; ++index)
        if (g_WowUserWindows[index].Window16 == window16) return &g_WowUserWindows[index];
    return NULL;
}

/* The other direction: the OS hands our window procedure a real HWND and we have
   to say which Win16 window that is. Declared in wowwin.h, defined here because
   this is where the table lives. 0 means "not one of ours", which is not an error
   -- DefWindowProc gets it, as it should. */
static WORD WowWinHwnd16(HWND window)
{
    INT index;
    if (!window) return 0;
    for (index = 0; index < g_WowUserWindowCount; ++index)
        if (g_WowUserWindows[index].Window16 && g_WowUserWindows[index].Window32 == window) return g_WowUserWindows[index].Window16;
    return 0;
}

/* The real window behind a Win16 handle, or NULL. */
static HWND WowUserHwnd32(WORD window16)
{
    PCWOWUSER_WINDOW window = WowUserFindWindow(window16);
    return window ? window->Window32 : NULL;
}

/* ── #308: the USER thunk standing for this window's system class, or NULL. */
static PCWOWUSER_SYSPROC WowUserSystemProcedureOf(PCWOWUSER_WINDOW window)
{
    INT index, charIndex;
    if (!window || !g_WowUserClasses[window->Class].IsSystemClass) return NULL;
    for (index = 0; index < WOWUSER_SYSPROC_COUNT; ++index) {
        PCSTR className = g_WowUserClasses[window->Class].Name, systemName = g_WowUserSystemProcedures[index].ClassName;
        for (charIndex = 0; className[charIndex] && systemName[charIndex]; ++charIndex) {
            CHAR upper = className[charIndex];
            if (upper >= 'a' && upper <= 'z') upper = (CHAR)(upper - WOWUSER_LOWER_TO_UPPER);
            if (upper != systemName[charIndex]) break;
        }
        if (!className[charIndex] && !systemName[charIndex]) return &g_WowUserSystemProcedures[index];
    }
    return NULL;
}

/* ...and is this 16:16 procedure one of those thunks? Only in USER's own segment
   (the stub segment of the call asking), only at a known offset, and only if the
   bytes there still carry the 'SCLS' signature and that class's index -- a USER.EXE
   of another build has other offsets, and then nothing is recognised (the caller
   says so) rather than something guessed. */
static PCWOWUSER_SYSPROC WowUserSystemProcedureAt(PCWOW32_FRAME frame, DWORD procedure)
{
    INT index;
    if (!procedure || (WORD)(procedure >> WORD_SHIFT) != frame->StubSegment) return NULL;
    for (index = 0; index < WOWUSER_SYSPROC_COUNT; ++index)
        if ((WORD)procedure == g_WowUserSystemProcedures[index].Offset) {
            const volatile BYTE *stub = (const volatile BYTE *)(ULONG_PTR)Wow32Flat(frame, procedure);
            if (stub && stub[WOWUSER_STUB_SIGNATURE] == 'S' && stub[WOWUSER_STUB_SIGNATURE + 1] == 'C' && stub[WOWUSER_STUB_SIGNATURE + 2] == 'L' && stub[WOWUSER_STUB_SIGNATURE + 3] == 'S'
                  && stub[WOWUSER_STUB_CLASS_INDEX] == g_WowUserSystemProcedures[index].Index)
                return &g_WowUserSystemProcedures[index];
            return NULL;
        }
    return NULL;
}

/* The messages a subclass procedure is SENT (see the note by g_WowUserSystemProcedures). */
static INT WowUserSubclassRelays(UINT message)
{
    switch (message) {
    case WM_KEYDOWN: case WM_KEYUP: case WM_CHAR: case WM_DEADCHAR:
    case WM_SYSKEYDOWN: case WM_SYSKEYUP: case WM_SYSCHAR: case WM_SYSDEADCHAR:
    case WM_MOUSEMOVE: case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK:
    case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_RBUTTONDBLCLK:
    case WM_MBUTTONDOWN: case WM_MBUTTONUP: case WM_MBUTTONDBLCLK:
    case WM_SETFOCUS: case WM_KILLFOCUS: case WM_GETDLGCODE: case WM_SETCURSOR:
    case WM_NCHITTEST: case WM_TIMER: case WM_ENABLE: case WM_CANCELMODE:
        return 1;
    }
    return 0;
}

/* The DS a control's procedure runs with: its own instance, else its parent's. */
static WORD WowUserInstanceOf(PCWOWUSER_WINDOW window)
{
    INT depth = 0;
    while (window && depth++ < WOWUSER_MAX_PARENT_DEPTH) {
        if (window->Instance) return window->Instance;
        if (g_WowUserClasses[window->Class].Instance) return g_WowUserClasses[window->Class].Instance;
        window = window->Parent ? WowUserFindWindow(window->Parent) : NULL;
    }
    return 0;
}

/* The Win32 procedure of a subclassed system control. */
static LRESULT CALLBACK WowUserSubclassProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    WORD window16 = WowWinHwnd16(window);
    PWOWUSER_WINDOW record = window16 ? WowUserFindWindow(window16) : NULL;
    WNDPROC original = record ? record->OriginalProcedure32 : NULL;
    if (!original) return DefWindowProcA(window, message, wParam, lParam);  /* cannot happen: we set both */
    if (g_WowUserSubclassBypass != window && record->SubclassProcedure && g_WowUserCall16 && WowUserSubclassRelays(message)) {
        WORD arguments[WOWUSER_WNDPROC_ARGUMENTS], result = 0, wParam16 = (WORD)wParam, dataSelector = WowUserInstanceOf(record);
        DWORD lParam16 = (DWORD)lParam;
        if (message == WM_SETFOCUS || message == WM_KILLFOCUS || message == WM_SETCURSOR)
            wParam16 = WowWinHwnd16((HWND)wParam);  /* a real HWND -> the guest's, or 0 */
        else if (message == WM_GETDLGCODE || message == WM_TIMER)
            lParam16 = 0;                      /* Win32: an LPMSG / a TIMERPROC   */
        arguments[0] = window16; arguments[1] = (WORD)message; arguments[2] = wParam16;
        arguments[3] = (WORD)(lParam16 >> WORD_SHIFT); arguments[4] = (WORD)(lParam16 & WORD_MASK);
        if (dataSelector && g_WowUserCall16(record->SubclassProcedure, dataSelector, arguments, WOWUSER_WNDPROC_ARGUMENTS, window16, (WORD)message, &result)) {
            ++g_WowUserSubclassSent;
            return (message == WM_NCHITTEST) ? (LRESULT)(SHORT)result : (LRESULT)result;
        }
    }
    ++g_WowUserSubclassDirect;
    return CallWindowProcA(original, window, message, wParam, lParam);
}

/* Is this window's parent an MDI client? Decides which default procedure the OS
   should run for it -- see WowWinProc. */
static INT WowUserIsMdiChild(PCWOWUSER_WINDOW window)
{
    PCWOWUSER_WINDOW parent = window->Parent ? WowUserFindWindow(window->Parent) : NULL;
    return parent && g_WowUserClasses[parent->Class].IsSystemClass
             && g_WowUserClasses[parent->Class].Name[0] == 'M';  /* MDICLIENT */
}

/* The MDI client owned by this window, if it has one -- a frame window has to
   pass it to DefFrameProc, which is how Win32 makes an MDI frame behave. */
static HWND WowUserMdiClientOf(PCWOWUSER_WINDOW window)
{
    INT index;
    for (index = 0; index < g_WowUserWindowCount; ++index) {
        PCWOWUSER_WINDOW child = &g_WowUserWindows[index];
        if (child->Window16 && child->Parent == window->Window16 && child->Window32
            && g_WowUserClasses[child->Class].IsSystemClass && g_WowUserClasses[child->Class].Name[0] == 'M')
            return child->Window32;
    }
    return NULL;
}

/* Resolve a 16:16 far pointer that is NOT an argument -- one we found inside a
   structure the guest handed us. Same null-selector rule as Wow32ArgPointer: 0
   rather than the LDT base, so a missing check cannot scribble at the bottom of
   the address space. */
static volatile BYTE *WowUserFarPointer(PCWOW32_FRAME frame, DWORD farPointer)
{
    WORD selector = (WORD)(farPointer >> WORD_SHIFT);
    DWORD base;
    if (!selector || !frame->SelectorToLinear) return NULL;
    base = frame->SelectorToLinear(selector, frame->Context);
    if (!base) return NULL;
    return (volatile BYTE *)(ULONG_PTR)(base + (farPointer & WORD_MASK));
}

/* Read a NUL-terminated guest string through a 16:16 far pointer. */
static INT WowUserFarString(PCWOW32_FRAME frame, DWORD farPointer, PSTR out, INT capacity)
{
    WORD selector = (WORD)(farPointer >> WORD_SHIFT);
    DWORD base;
    const volatile BYTE *source;
    INT length = 0;
    if (capacity) out[0] = 0;
    if (!selector || !frame->SelectorToLinear) return 0;
    base = frame->SelectorToLinear(selector, frame->Context);
    if (!base) return 0;
    source = (const volatile BYTE *)(ULONG_PTR)(base + (farPointer & WORD_MASK));
    while (length < capacity - 1 && source[length]) { out[length] = (CHAR)source[length]; ++length; }
    out[length] = 0;
    return 1;
}

static WORD WowUserPeek(const volatile BYTE *bytes, INT offset)
{
    return (WORD)(bytes[offset] | (bytes[offset + 1] << BYTE_SHIFT));
}

/* ── A GUEST FAR POINTER AS SOMETHING THE HOST CAN READ. ──────────────────────
     WowUserFarString already did this for strings; a DLGTEMPLATE is a STRUCTURE,
     so the pointer itself is what is wanted. Same rules: a null selector or a
     selector the LDT cannot resolve yields NULL rather than a wild address. */
static const volatile BYTE *WowUserFarMemory(PCWOW32_FRAME frame, DWORD farPointer)
{
    WORD selector = (WORD)(farPointer >> WORD_SHIFT);
    DWORD base;
    if (!selector || !frame->SelectorToLinear) return NULL;
    base = frame->SelectorToLinear(selector, frame->Context);
    if (!base) return NULL;
    return (const volatile BYTE *)(ULONG_PTR)(base + (farPointer & WORD_MASK));
}

/* ── ★★★★★ THE Win16 DIALOG TEMPLATE, AND EVERY FIELD IS A DIFFERENT WIDTH
     FROM ITS Win32 DESCENDANT. (session 55) ────────────────────────────────────
     This is the 16-bit DLGTEMPLATE: the item count is a BYTE, every coordinate
     is a WORD, and the variable-length name fields come in three forms. Win32's
     structure has a WORD count and a different field ORDER, so a reader written
     from the modern layout produces a dialog with a plausible size and the
     wrong number of controls -- which looks like a drawing bug, not a parsing one.

         DWORD dtStyle;  BYTE dtItemCount;  WORD dtX, dtY, dtCX, dtCY;
         <menu>  <class>  <caption>
         if (dtStyle & DS_SETFONT):  WORD pointsize;  <typeface>
       then dtItemCount x:
         WORD x, y, cx, cy;  WORD id;  DWORD style;
         <class: ONE BYTE 0x80..0x85, or a string>  <text>  BYTE cbCreationData

   ★ THE READING WAS CONFIRMED BEFORE ANY OF THIS EXISTED, by decoding CALC.EXE's
     own resource offline (`tools/ne/neres.py dialog`) -- the same discipline the
     menu decoder was held to in session 43. A wrong offset does not spell
     'Calculator', 'SciCalc', and buttons reading Hex/Dec/Oct/Bin/Hyp/Inv. */
#define WOWDLG_SETFONT 0x40
#define WOWDLG_NAME_EMPTY          0x00    /* a name-or-ordinal field: nothing      */
#define WOWDLG_NAME_ORDINAL        0xFF    /* ...an ordinal follows                 */
#define WOWDLG_NAME_ORDINAL_SIZE   3
#define WOWDLG_CLASS_BUTTON        0x80    /* a control's predefined class byte     */
#define WOWDLG_CLASS_EDIT          0x81
#define WOWDLG_CLASS_STATIC        0x82
#define WOWDLG_CLASS_LISTBOX       0x83
#define WOWDLG_CLASS_SCROLLBAR     0x84
#define WOWDLG_CLASS_COMBOBOX      0x85
#define WOWDLG_POINTS_PER_INCH     72
#define WOWDLG_SAMPLE_LENGTH       52      /* "A..Za..z": the base-unit sample      */
/* DLGTEMPLATE (Win16): style, item count, x, y, cx, cy, then the menu name. */
#define WOWDLG_TEMPLATE_COUNT      4
#define WOWDLG_TEMPLATE_X          5
#define WOWDLG_TEMPLATE_Y          7
#define WOWDLG_TEMPLATE_WIDTH      9
#define WOWDLG_TEMPLATE_HEIGHT     11
#define WOWDLG_TEMPLATE_MENU       13
/* DLGITEMTEMPLATE (Win16): x, y, cx, cy, id, style, then the class. */
#define WOWDLG_ITEM_Y              2
#define WOWDLG_ITEM_WIDTH          4
#define WOWDLG_ITEM_HEIGHT         6
#define WOWDLG_ITEM_ID             8
#define WOWDLG_ITEM_STYLE          10
#define WOWDLG_ITEM_SIZE           14
#define WOWDLG_UNITS_PER_BASE_X    4       /* dialog units: x*baseX/4, y*baseY/8 */
#define WOWDLG_UNITS_PER_BASE_Y    8

static WORD WowDlgTemplateWord(const volatile BYTE *bytes, INT offset)
{
    return (WORD)(bytes[offset] | (bytes[offset + 1] << BYTE_SHIFT));
}
static DWORD WowDlgTemplateDword(const volatile BYTE *bytes, INT offset)
{
    return (DWORD)WowDlgTemplateWord(bytes, offset) | ((DWORD)WowDlgTemplateWord(bytes, offset + WOW_WORD_BYTES) << WORD_SHIFT);
}

/* A NUL-terminated string out of guest memory. Returns BYTES CONSUMED including
   the terminator, because every caller's next field depends on it. */
static INT WowDlgTemplateString(const volatile BYTE *bytes, INT offset, PSTR out, INT capacity)
{
    INT length = 0;
    while (bytes[offset + length]) { if (length < capacity - 1) out[length] = (CHAR)bytes[offset + length]; ++length; }
    if (capacity) out[length < capacity - 1 ? length : capacity - 1] = 0;
    return length + 1;
}

/* The menu/class field: 0x00 absent, 0xFF + WORD ordinal, else a string.
   ⚠ THE 0xFF FORM CARRIES A **WORD**, SO IT IS THREE BYTES. Reading it as a
     byte leaves one behind and every field after it is garbage -- and garbage
     here still parses, it just yields nonsense coordinates. */
static INT WowDlgTemplateNameOrdinal(const volatile BYTE *bytes, INT offset, PSTR out, INT capacity,
                          PWORD ordinal)
{
    *ordinal = 0;
    if (capacity) out[0] = 0;
    if (bytes[offset] == WOWDLG_NAME_EMPTY) return 1;
    if (bytes[offset] == WOWDLG_NAME_ORDINAL) { *ordinal = WowDlgTemplateWord(bytes, offset + 1); return WOWDLG_NAME_ORDINAL_SIZE; }
    return WowDlgTemplateString(bytes, offset, out, capacity);
}

/* The six predefined control classes, in the order their byte codes run. */
static PCSTR WowDlgClassName(BYTE classByte)
{
    switch (classByte) {
    case WOWDLG_CLASS_BUTTON: return "BUTTON";
    case WOWDLG_CLASS_EDIT: return "EDIT";
    case WOWDLG_CLASS_STATIC: return "STATIC";
    case WOWDLG_CLASS_LISTBOX: return "LISTBOX";
    case WOWDLG_CLASS_SCROLLBAR: return "SCROLLBAR";
    case WOWDLG_CLASS_COMBOBOX: return "COMBOBOX";
    }
    return NULL;
}

/* ── s89 (#283): A DIALOG IS LAID OUT IN ITS OWN FONT'S UNITS. ─────────────────
     Dialog units are quarters of the dialog font's average character width and
     eighths of its height. We used the SYSTEM font's (GetDialogBaseUnits), so
     every Win16 dialog came out ~13% too big against stock on the same desktop
     (Terminal's port dialog 218x152 vs 192x130, Calc 294 vs 275 tall, Charmap 702
     vs 611 wide) and its controls drew in the system font. Measured the way USER32
     measures it: the alphabet's average width (rounded), and tmHeight. BOLD: a
     3.x application's dialog font is bold under stock's WOW (its labels are, in
     the same screenshots). Fonts are kept for the run, one per face+size -- a
     shelf program opens a handful. */
#define WOWDLG_MAXFONT 8
typedef struct _WOWDLG_FONT { char FaceName[LF_FACESIZE]; INT PointSize; HFONT Font; INT BaseX, BaseY; } WOWDLG_FONT; static WOWDLG_FONT g_WowDlgFonts[WOWDLG_MAXFONT];
static INT g_WowDlgFontCount;
static HFONT WowDlgFont(PCSTR faceName, INT pointSize, PINT baseX, PINT baseY)
{
    INT index;
    HDC dc; HFONT font, previous; TEXTMETRICA textMetric; SIZE extent;
    for (index = 0; index < g_WowDlgFontCount; ++index)
        if (g_WowDlgFonts[index].PointSize == pointSize && !lstrcmpiA(g_WowDlgFonts[index].FaceName, faceName)) {
            *baseX = g_WowDlgFonts[index].BaseX; *baseY = g_WowDlgFonts[index].BaseY; return g_WowDlgFonts[index].Font;
        }
    if (g_WowDlgFontCount >= WOWDLG_MAXFONT || !faceName[0] || pointSize <= 0) return NULL;
    dc = GetDC(NULL);
    if (!dc) return NULL;
    font = CreateFontA(-MulDiv(pointSize, GetDeviceCaps(dc, LOGPIXELSY), WOWDLG_POINTS_PER_INCH), 0, 0, 0, FW_BOLD,
                    FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                    CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH | FF_DONTCARE, faceName);
    if (!font) { ReleaseDC(NULL, dc); return NULL; }
    previous = (HFONT)SelectObject(dc, font);
    if (!GetTextMetricsA(dc, &textMetric)
        || !GetTextExtentPoint32A(dc, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz", WOWDLG_SAMPLE_LENGTH, &extent)) {
        SelectObject(dc, previous); ReleaseDC(NULL, dc); DeleteObject(font); return NULL;
    }
    SelectObject(dc, previous); ReleaseDC(NULL, dc);
    index = g_WowDlgFontCount++;
    lstrcpynA(g_WowDlgFonts[index].FaceName, faceName, sizeof g_WowDlgFonts[index].FaceName);
    g_WowDlgFonts[index].PointSize = pointSize; g_WowDlgFonts[index].Font = font;
    g_WowDlgFonts[index].BaseX = (extent.cx / (WOWDLG_SAMPLE_LENGTH / 2) + 1) / 2;  /* USER32's rounding */
    g_WowDlgFonts[index].BaseY = textMetric.tmHeight;
    *baseX = g_WowDlgFonts[index].BaseX; *baseY = g_WowDlgFonts[index].BaseY;
    return font;
}

/* The class a name is registered under, or NULL. Win16 class names are
   case-insensitive, and a lookup that is not would silently register duplicates. */
static PWOWUSER_CLASS WowUserFindClass(PCSTR name)
{
    INT index, charIndex;
    for (index = 0; index < g_WowUserClassCount; ++index) {
        PCSTR className = g_WowUserClasses[index].Name, wanted = name;
        for (charIndex = 0; ; ++charIndex) {
            CHAR classChar = className[charIndex], wantedChar = wanted[charIndex];
            if (classChar >= 'a' && classChar <= 'z') classChar = (CHAR)(classChar - WOWUSER_LOWER_TO_UPPER);
            if (wantedChar >= 'a' && wantedChar <= 'z') wantedChar = (CHAR)(wantedChar - WOWUSER_LOWER_TO_UPPER);
            if (classChar != wantedChar) break;
            if (!classChar) return &g_WowUserClasses[index];
        }
    }
    return NULL;
}

/* ── s91 (TASKMAN): A WIN16 HANDLE FOR ANY WINDOW, AS WOW GIVES ONE. ─────────────────
     Real WOW hands a 16-bit program a handle for every window on the desktop, so a
     Win16 Task List lists the XP desktop's windows (stock, runs/stockshot/
     s91_taskman_stock.bmp: cmd.exe, Notepad, Program Manager). Ours issued handles
     only for the guest's own windows, so GetWindow(GW_HWNDFIRST) named nothing and
     TASKMAN's list was empty. An alias is a window record pointing at the real HWND
     with no procedure: what is ASKED of it (text, visibility, rectangle, owner) is
     answered from the real window by the same code as for ours. Recycled once the
     window is gone; at most 48 live. Only GetWindow mints them -- the message relay
     still maps a foreign HWND to 0, as before. */
static WORD WowUserAlias16(HWND window)
{
    WORD window16;
    PWOWUSER_CLASS windowClass;
    PWOWUSER_WINDOW record;
    INT index, aliasCount = 0;
    if (!window) return 0;
    window16 = WowWinHwnd16(window);
    if (window16) return window16;
    for (index = 0; index < g_WowUserWindowCount; ++index) {
        PWOWUSER_WINDOW alias = &g_WowUserWindows[index];
        if (!alias->Window16 || !alias->IsForeign) continue;
        if (!alias->Window32 || !IsWindow(alias->Window32)) { alias->Window16 = 0; alias->Window32 = NULL; alias->IsForeign = 0; }
        else ++aliasCount;
    }
    if (aliasCount >= WOWUSER_MAX_ALIASES) return 0;
    WowUserEnsureSystemClasses();
    windowClass = WowUserFindClass("~FOREIGN");
    if (!windowClass || !(record = WowUserNewWindow())) return 0;
    record->Class = (WORD)(windowClass - g_WowUserClasses);
    record->Style = (DWORD)GetWindowLongA(window, GWL_STYLE);
    record->WindowProcedure = 0; record->Parent = 0; record->Menu = 0; record->Instance = 0;
    record->Text[0] = 0; record->Memory16 = 0; record->MenuItems = 0; record->IsDying = 0;
    for (index = 0; index < WOWUSER_MAX_EXTRA; ++index) record->Extra[index] = 0;
    record->Window32 = window;
    record->IsForeign = 1;
    return record->Window16;
}

/* The class registered under an ATOM, or NULL. ⚠ Win16 lets lpClassName be an
   atom rather than a string -- a null selector with the atom in the offset -- and
   a host that only understood strings would fail every CreateWindow made from
   RegisterClass's own return value, which is the idiomatic way to write it. */
static PWOWUSER_CLASS WowUserFindClassByAtom(WORD atom)
{
    INT index;
    if (!atom) return NULL;
    for (index = 0; index < g_WowUserClassCount; ++index)
        if (g_WowUserClasses[index].Atom == atom) return &g_WowUserClasses[index];
    return NULL;
}

/* ── ★ ASK FOR THE WM_CREATE. One helper, because there are now TWO places that
     make a window with a 16-bit procedure behind it (CreateWindow, and the MDI
     client's WM_MDICREATE) and they must send the same message with the same
     entry conditions. See wowcall.h for what the fields mean and for why the
     return mode is KEEP: the caller has already written the handle it made, and
     the procedure's answer only gets a veto. */
/* ── ★★★ THE TIMER TABLE, AND WHY A TIMERPROC IS NOT A NESTED CALL. ──────────
     Solitaire's first run named this: it arms `SetTimer(hWnd, 0x029a, 250ms,
     lpTimerFunc)` with a REAL procedure at 0x0b9f:0x00ba, and a host that
     refuses the call gets **"Out of memory"** -- Win16 timers were a scarce
     system-wide resource, so failing to get one is genuinely how a program of
     this era reports it.
   ★ THE TRAP TO AVOID: calling that procedure from inside a Win32 timer
     callback, which would mean re-entering the guest from a place the host is
     not running it -- the nested run this project has not built. It is not
     needed, because WIN16 DOES NOT CALL A TIMERPROC FROM THE TIMER EITHER. It
     posts WM_TIMER with the procedure in lParam, and **DispatchMessage** calls
     it instead of the window procedure. DispatchMessage is already a place this
     host calls 16-bit code from, on the guest's own thread, with its own stack.
     So the faithful implementation and the safe one are the same implementation.
   ⇒ This table exists only so the WM_TIMER relay in wowwin.h can put the right
     procedure in lParam; the OS keeps the actual timing. */
#define WOWUSER_MAXTIMER 32
typedef struct _WOWUSER_TIMER { WORD Window; WORD Id; DWORD Procedure; INT IsUsed; } WOWUSER_TIMER;
static WOWUSER_TIMER g_WowUserTimers[WOWUSER_MAXTIMER];

static VOID WowUserTimerSet(WORD window16, WORD timerId, DWORD procedure)
{
    INT index, freeSlot = -1;
    for (index = 0; index < WOWUSER_MAXTIMER; ++index) {
        if (g_WowUserTimers[index].IsUsed && g_WowUserTimers[index].Window == window16
                               && g_WowUserTimers[index].Id == timerId) {
            g_WowUserTimers[index].Procedure = procedure;  /* re-arming replaces the proc */
            return;
        }
        if (!g_WowUserTimers[index].IsUsed && freeSlot < 0) freeSlot = index;
    }
    if (freeSlot < 0) return;               /* full: the timer still runs, but
                                               with no proc -- WM_TIMER reaches
                                               the window procedure instead */
    g_WowUserTimers[freeSlot].Window = window16;
    g_WowUserTimers[freeSlot].Id   = timerId;
    g_WowUserTimers[freeSlot].Procedure = procedure;
    g_WowUserTimers[freeSlot].IsUsed = 1;
}

static VOID WowUserTimerClear(WORD window16, WORD timerId)
{
    INT index;
    for (index = 0; index < WOWUSER_MAXTIMER; ++index)
        if (g_WowUserTimers[index].IsUsed && g_WowUserTimers[index].Window == window16
                               && g_WowUserTimers[index].Id == timerId)
            { g_WowUserTimers[index].IsUsed = 0; g_WowUserTimers[index].Procedure = 0; return; }
}

/* Declared in wowwin.h, which is included first and relays WM_TIMER. */
static DWORD WowUserTimerProcedure(WORD window16, WORD timerId)
{
    INT index;
    for (index = 0; index < WOWUSER_MAXTIMER; ++index)
        if (g_WowUserTimers[index].IsUsed && g_WowUserTimers[index].Window == window16
                               && g_WowUserTimers[index].Id == timerId)
            return g_WowUserTimers[index].Procedure;
    return 0;
}

/* Fill in a window-procedure call: five words, in DECLARED order. */
static VOID WowUserWantMessage(PWOW32_FRAME frame, PCWOWUSER_WINDOW window, WORD dataSelector,
                             WORD message, WORD wParam, DWORD lParam, INT returnMode)
{
    /* ★ THE WINDOW'S procedure, which for a `#32770` dialog is its DLGPROC --
         one rule, in WowUserWindowProcedureOf(), so that a message cannot reach a
         window through SendMessage and fail to reach it through DispatchMessage
         (or the modal loop) because three call sites each decided for
         themselves. */
    frame->CallbackProcedure   = WowUserWindowProcedureOf(window);
    frame->CallbackDataSelector     = dataSelector;
    frame->CallbackArguments[0] = window->Window16;
    frame->CallbackArguments[1] = message;
    frame->CallbackArguments[2] = wParam;
    frame->CallbackArguments[3] = (WORD)(lParam >> WORD_SHIFT);
    frame->CallbackArguments[4] = (WORD)(lParam & WORD_MASK);
    frame->CallbackArgumentCount   = WOWUSER_WNDPROC_ARGUMENTS;
    frame->CallbackReturnMode    = returnMode;
    frame->CallbackWindow   = window->Window16;
    frame->CallbackMessage    = message;
}

/*
 * ── ★★★★★ THE CREATESTRUCT, AND IT WAS READ OFF A RUN, NOT A HEADER ─────────
 * WM_CREATE's lParam is an LPCREATESTRUCT. This host passed 0 and said so, and
 * that stayed harmless until MS PAINT: its canvas procedure GP-faults on the
 * spot in WM_CREATE, reading through the null pointer at +0x0c (the host's
 * fault frame: ES:BX = lParam = 0) -- CREATESTRUCT.cx, i.e. how big it is.
 *
 * ── ★★★ THE LAYOUT IS THE CreateWindow ARGUMENT BLOCK, UNCHANGED ────────────
 * Which is why this needs no header and no guesswork. Paint's own call carried
 *     (0000 0000 | 09c6 | 0001 | 0160 | 0002 | 0002 | 0002 | 00ac | 0000 40b0
 *      | 0000 0000 | 08bd 09c7)
 * and the CW_ARG_* offsets this file already uses -- named from earlier runs and
 * cross-checked against the 30 bytes the stub declares -- read that as
 * lpParam@0, hInstance@4, hMenu@6, hwndParent@8, cy@10, cx@12, y@14, x@16,
 * style@18 (0x40b00000, exactly what the log printed), lpszName@22,
 * lpszClass@26 ("pbPaint"). Those are the documented CREATESTRUCT's members, in
 * order, at those offsets -- and PBRUSH faulting on cx at +0x0c agrees with it
 * independently. So the structure is the argument block COPIED, plus a
 * `dwExStyle` of 0 at +30 to make up the 34 bytes.
 * ⇒ Nothing here is taken on trust: two independent observations agree.
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
#define WOWUSER_CW_GEOMETRY        4     /* cy, cx, y, x                          */
#define WOWUSER_CW_ARGUMENT_BYTES  30    /* CreateWindow's arguments, as passed   */
#define WOWUSER_CW_BLOB_EXSTYLE    30    /* ...then dwExStyle                     */
#define WOWUSER_CW_BLOB_SIZE       34
static VOID WowUserWantCreate(PWOW32_FRAME frame, PCWOWUSER_CLASS windowClass,
                                PCWOWUSER_WINDOW window)
{
    PBYTE blob = frame->CallbackBlob;
    INT index;
    WORD geometry[WOWUSER_CW_GEOMETRY];
    static const INT geometryArguments[WOWUSER_CW_GEOMETRY] = { WOWUSER_CW_ARG_HEIGHT, WOWUSER_CW_ARG_WIDTH,
                                WOWUSER_CW_ARG_Y, WOWUSER_CW_ARG_X };
    if (!frame->IsCallbackAllowed || !window->WindowProcedure) return;

    /* The 30 argument bytes, verbatim -- they ARE the first eleven members. */
    for (index = 0; index < WOWUSER_CW_ARGUMENT_BYTES; ++index) blob[index] = frame->FrameBase[WOW32_OFF_ARGS + index];
    blob[WOWUSER_CW_BLOB_EXSTYLE] = blob[WOWUSER_CW_BLOB_EXSTYLE + 1] = blob[WOWUSER_CW_BLOB_EXSTYLE + 2] = blob[WOWUSER_CW_BLOB_EXSTYLE + 3] = 0;   /* dwExStyle, always 0 here */

    /* ⚠ CW_USEDEFAULT is 0x8000 in Win16 (it is 0x80000000 in Win32 -- the trap
         this host has already been caught by once). Where the guest defaulted a
         field, tell it what it actually got. */
    if (window->Window32) {
        RECT rect;
        if (GetWindowRect(window->Window32, &rect)) {
            geometry[0] = (WORD)(rect.bottom - rect.top);  /* cy, cx, y, x -- GEO order */
            geometry[1] = (WORD)(rect.right - rect.left);
            geometry[2] = (WORD)rect.top;
            geometry[3] = (WORD)rect.left;
            for (index = 0; index < WOWUSER_CW_GEOMETRY; ++index)
                if (Wow32ArgWord(frame, geometryArguments[index]) == CW_USEDEFAULT16) {
                    blob[geometryArguments[index]]     = (BYTE)(geometry[index] & BYTE_MASK);
                    blob[geometryArguments[index] + 1] = (BYTE)(geometry[index] >> BYTE_SHIFT);
                }
        }
    }
    frame->CallbackBlobLength = WOWUSER_CW_BLOB_SIZE;

    WowUserWantMessage(frame, window, window->Instance ? window->Instance : windowClass->Instance,
                     WM_CREATE16, 0, 0, WOWCALL_RET_KEEP);
    /* cbarg[3] is lParam's HIGH word (see WowUserWantMessage); WowCallEnter fills
       both halves once it knows where on the stack the structure landed. */
    frame->CallbackBlobArgument = WOWUSER_WNDPROC_ARG_LPARAM;
}

/*
 * ── ★★★ THE MDICREATESTRUCT, CHECKED AGAINST WHAT SYSEDIT SENDS ─────────────
 * SYSEDIT builds one on its stack and hands it to
 * `SendMessage(hwndMDIClient, WM_MDICREATE, 0, &it)` (`USER.111 SENDMESSAGE`).
 * The documented Win16 layout, each field confirmed by the values that arrive:
 *
 *   +0x00 szClass (far)    +0x0a x    +0x0c y    (CW_USEDEFAULT, 0x8000)
 *   +0x04 szTitle (far)    +0x0e cx   +0x10 cy
 *   +0x08 hOwner           +0x12 style DWORD
 *
 * ★ szClass decodes to `"mpchild"` -- the class SYSEDIT registered two calls
 *   earlier. A wrong offset for szClass does not decode to a class this program
 *   has registered.
 * ⚠ THE STRUCT ENDS AT +0x16. `+0x16` (where a `lParam` member would sit) is NOT
 *   set by this program -- the stack word there holds unrelated data -- and must
 *   not be read.
 */
#define WOWUSER_MCS_SZCLASS   0x00
#define WOWUSER_MCS_SZTITLE   0x04
#define WOWUSER_MCS_HOWNER    0x08
#define WOWUSER_MCS_X         0x0a
#define WOWUSER_MCS_Y         0x0c
#define WOWUSER_MCS_CX        0x0e
#define WOWUSER_MCS_CY        0x10
#define WOWUSER_MCS_STYLE     0x12

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
/* #160: the edit commands -- also shared numbers. */
#define WM_CUT16            0x0300
#define WM_COPY16           0x0301
#define WM_PASTE16          0x0302
#define WM_CLEAR16          0x0303
#define WM_UNDO16           0x0304

/* Resolve a 16:16 far pointer VALUE (as carried in an lParam) to a host address.
   ⚠ Null selector yields NULL rather than the LDT base, the same rule
     Wow32ArgPointer follows, so a forgotten check cannot scribble at the bottom of
     the address space. */
static volatile BYTE *WowUserLinear(PCWOW32_FRAME frame, DWORD farPointer)
{
    WORD  selector = (WORD)(farPointer >> WORD_SHIFT);
    DWORD base;
    if (!selector || !frame->SelectorToLinear) return NULL;
    base = frame->SelectorToLinear(selector, frame->Context);
    if (!base) return NULL;
    return (volatile BYTE *)(ULONG_PTR)(base + (farPointer & WORD_MASK));
}

/* Win16 numbers a control's messages from WM_USER, per class (see below). */
#define WOWUSER_CONTROL_MESSAGE_LAST16 0x0430
#define WOWUSER_CONTROL_CLASS_SIZE     16
#define WOWUSER_BM_BASE16              WM_USER     /* BM_GETCHECK                 */
#define WOWUSER_BM_LAST16              0x0404      /* BM_SETSTYLE                 */
#define WOWUSER_CB_BASE16              WM_USER     /* CB_GETEDITSEL               */
#define WOWUSER_CB_COUNT16             25
#define WOWUSER_LB_BASE16              0x0401      /* LB_ADDSTRING                */
#define WOWUSER_LB_COUNT16             0x23
#define WOWUSER_EM_BASE16              WM_USER     /* EM_GETSEL                   */
#define WOWUSER_EM_LAST16              0x041D
#define WOWUSER_EM_COUNT16             30
#define WOWUSER_EM_INDEX(message32)    ((message32) - EM_GETSEL)   /* n, from Win32's number */
#define WOWUSER_EM_SELECT_TO_END16     0x7FFF      /* EM_SETSEL's end: to the end */
#define WOWUSER_RECT16_WORDS           4
#define WOWUSER_LIST_TEXT_SIZE         256
#define WOWUSER_LIST_MAX_SELECTION     1024
#define WOWUSER_MAX_TAB_STOPS          256
#define WOWUSER_EDIT_TEXT_SIZE         4096
/* What a list message's lParam/wParam carry -- the kinds in the tables below. */
#define WOWUSER_LIST_VALUES            0
#define WOWUSER_LIST_IN_STRING         1
#define WOWUSER_LIST_OUT_BUFFER        2
#define WOWUSER_LIST_STRUCTURE         3
#define WOWUSER_LIST_INDEX             4

/* ── s89 (#162, Charmap's font list): LISTBOX AND COMBOBOX MESSAGES. ──────────────
     Win16 numbers a control's messages from WM_USER per class, so 0x040C is
     EM_SETHANDLE to an edit control and CB_FINDSTRING to a combo box; everything
     in this range used to be read as an edit message or answered 0, so every Win16
     list and combo box stayed EMPTY. Same lists, same order, different bases:
       CB_*: Win16 0x0400+n -> Win32 0x0140+n   (GETEDITSEL .. FINDSTRINGEXACT)
       LB_*: Win16 0x0401+n -> Win32 0x0180+n   (ADDSTRING ..)
     Strings are far pointers into the guest (copied in; GETTEXT copies back out,
     bounded by the item's own length); an index of -1 travels as 0xFFFF and is
     sign-extended; LB_ERR/CB_ERR come back as 0xFFFF in the low word. A control
     without HASSTRINGS keeps lParam as its item data. Messages with structures
     (GETITEMRECT, GETSELITEMS, GETDROPPEDCONTROLRECT, SETTABSTOPS) are not
     translated yet and still answer 0. */
static INT WowUserListMessage(PWOW32_FRAME frame, PWOWUSER_WINDOW window, WORD message, WORD wParam,
                           DWORD lParam, INT isComboBox, PLONG out, PSTR note, INT noteCapacity,
                           PINT noteLengthInOut)
{
    /* per n: 0 = values only, 1 = lParam IN string, 2 = lParam OUT buffer (text of
       item wParam), 3 = lParam is a structure (not translated), 4 = wParam index */
    static const BYTE comboBoxKinds[WOWUSER_CB_COUNT16] = { 0,0,0,1,4,1,0,0,2,4,1,0,1,1,4,0,4,4,3,4,4,0,0,0,1 };
    static const BYTE listBoxKinds[WOWUSER_LB_COUNT16] = {
        /* 01 ADD 02 INS 03 DEL 04 (SELITEMRANGEEX) 05 RESET 06 SETSEL 07 SETCURSEL
           08 GETSEL 09 GETCURSEL */
        1,1,4,3,0,0,4,4,0,
        /* 0A GETTEXT 0B GETTEXTLEN 0C GETCOUNT 0D SELECTSTRING 0E DIR 0F GETTOPINDEX */
        2,4,0,1,1,0,
        /* 10 FINDSTRING 11 GETSELCOUNT 12 GETSELITEMS 13 SETTABSTOPS 14 GETHORZEXT
           15 SETHORZEXT 16 SETCOLWIDTH 17 (ADDFILE) 18 SETTOPINDEX 19 GETITEMRECT
           1A GETITEMDATA 1B SETITEMDATA 1C SELITEMRANGE 1D-1E (ANCHOR)
           1F SETCARETINDEX 20 GETCARETINDEX 21 SETITEMHEIGHT 22 GETITEMHEIGHT
           23 FINDSTRINGEXACT  -- Win16 WM_USER+n is Win32 0x17F+n throughout */
        1,0,3,3,0,0,0,3,4,3,4,4,0,3,3,4,0,4,4,1 };
    INT messageIndex, kind, noteLength = *noteLengthInOut;
    UINT message32;
    WPARAM wParam32 = (WPARAM)wParam;
    LPARAM lParam32 = (LPARAM)lParam;
    CHAR buffer[WOWUSER_LIST_TEXT_SIZE];
    volatile BYTE *guestBuffer = NULL;
    LRESULT result;
    LONG style = GetWindowLongA(window->Window32, GWL_STYLE);
    INT hasStrings = isComboBox ? ((style & CBS_HASSTRINGS) || !(style & (CBS_OWNERDRAWFIXED | CBS_OWNERDRAWVARIABLE)))  /* CBS_HASSTRINGS / not owner-draw */
                    : ((style & LBS_HASSTRINGS) || !(style & (LBS_OWNERDRAWFIXED | LBS_OWNERDRAWVARIABLE)));  /* LBS_HASSTRINGS / not owner-draw */
    if (isComboBox) { messageIndex = message - WOWUSER_CB_BASE16; if (messageIndex < 0 || messageIndex >= WOWUSER_CB_COUNT16) return 0; kind = comboBoxKinds[messageIndex]; message32 = CB_GETEDITSEL + messageIndex; }
    else      { messageIndex = message - WOWUSER_LB_BASE16; if (messageIndex < 0 || messageIndex >= WOWUSER_LB_COUNT16) return 0; kind = listBoxKinds[messageIndex]; message32 = LB_ADDSTRING + messageIndex; }
    if (kind == WOWUSER_LIST_STRUCTURE) {
        /* ── #304 (M6, s90): THE STRUCTURE MESSAGES, translated. Win16's INT is a
             WORD and its RECT four shorts; Win32's are DWORDs and four LONGs, so
             every one of these is a copy WITH A WIDTH CHANGE, in or out. */
        volatile BYTE *guest = WowUserLinear(frame, lParam);
        LRESULT structureResult = 0;
        INT index, isDone = 1;
        switch (message32) {
        case LB_SELITEMRANGEEX: case LB_SETANCHORINDEX: case LB_GETANCHORINDEX:     /* SELITEMRANGEEX, SET/GETANCHORINDEX */
            structureResult = SendMessageA(window->Window32, message32, (WPARAM)(LONG)(SHORT)wParam, (LPARAM)lParam);
            break;
        case LB_ADDFILE: {                              /* LB_ADDFILE: a file name, in */
            CHAR fileName[MAX_PATH];
            for (index = 0; guest && index < (INT)sizeof fileName - 1 && guest[index]; ++index) fileName[index] = (CHAR)guest[index];
            fileName[index] = 0;
            structureResult = guest ? SendMessageA(window->Window32, message32, 0, (LPARAM)fileName) : -1;
            break; }
        case LB_GETSELITEMS: {                              /* LB_GETSELITEMS: INT16[] out */
            static INT selection[WOWUSER_LIST_MAX_SELECTION];
            INT wanted = (INT)(wParam > WOWUSER_LIST_MAX_SELECTION ? WOWUSER_LIST_MAX_SELECTION : wParam);
            structureResult = (guest && wanted) ? SendMessageA(window->Window32, message32, (WPARAM)wanted, (LPARAM)selection) : 0;
            for (index = 0; guest && index < (INT)structureResult && index < wanted; ++index) {
                guest[index * WOW_WORD_BYTES] = (BYTE)selection[index]; guest[index * WOW_WORD_BYTES + 1] = (BYTE)(selection[index] >> BYTE_SHIFT); }
            break; }
        case LB_SETTABSTOPS: {                              /* LB_SETTABSTOPS: INT16[] in */
            static INT tabStops[WOWUSER_MAX_TAB_STOPS];
            INT count = (INT)(wParam > WOWUSER_MAX_TAB_STOPS ? WOWUSER_MAX_TAB_STOPS : wParam);
            for (index = 0; guest && index < count; ++index) tabStops[index] = (INT)(SHORT)(guest[index * WOW_WORD_BYTES] | (guest[index * WOW_WORD_BYTES + 1] << BYTE_SHIFT));
            structureResult = SendMessageA(window->Window32, message32, (WPARAM)count, count ? (LPARAM)tabStops : 0);
            break; }
        case LB_GETITEMRECT:                             /* LB_GETITEMRECT: RECT16 out */
        case CB_GETDROPPEDCONTROLRECT: {                           /* CB_GETDROPPEDCONTROLRECT   */
            RECT rect;
            rect.left = rect.top = rect.right = rect.bottom = 0;
            structureResult = SendMessageA(window->Window32, message32, (WPARAM)(LONG)(SHORT)wParam, (LPARAM)&rect);
            if (guest) {
                LONG values[WOWUSER_RECT16_WORDS]; values[0] = rect.left; values[1] = rect.top; values[2] = rect.right; values[3] = rect.bottom;
                for (index = 0; index < WOWUSER_RECT16_WORDS; ++index) { guest[index * WOW_WORD_BYTES] = (BYTE)values[index]; guest[index * WOW_WORD_BYTES + 1] = (BYTE)(values[index] >> BYTE_SHIFT); }
            }
            break; }
        default: isDone = 0; break;
        }
        WowNotePut(note, noteCapacity, &noteLength, isComboBox ? "CB_" : "LB_");
        WowNotePut(note, noteCapacity, &noteLength, " n=0x"); WowNoteHex(note, noteCapacity, &noteLength, (DWORD)messageIndex, WOW_HEX_BYTE_DIGITS);
        if (!isDone) {
            WowNotePut(note, noteCapacity, &noteLength, " carries a structure -- not translated; 0");
            *noteLengthInOut = noteLength; *out = 0; return 1;
        }
        WowNotePut(note, noteCapacity, &noteLength, " (structure translated) -> 0x");
        WowNoteHex(note, noteCapacity, &noteLength, (DWORD)structureResult, WOW_HEX_DWORD_DIGITS);
        *noteLengthInOut = noteLength; *out = (LONG)structureResult; return 1;
    }
    if (kind == WOWUSER_LIST_INDEX || kind == WOWUSER_LIST_OUT_BUFFER || (kind == WOWUSER_LIST_IN_STRING && wParam == WOWUSER_MINUS_ONE16)) wParam32 = (WPARAM)(LONG)(SHORT)wParam;
    if (kind == WOWUSER_LIST_IN_STRING && hasStrings) {
        INT index;
        guestBuffer = WowUserLinear(frame, lParam);
        if (!guestBuffer) { *noteLengthInOut = noteLength; *out = isComboBox ? CB_ERR : LB_ERR; return 1; }
        for (index = 0; index < (INT)sizeof buffer - 1 && guestBuffer[index]; ++index) buffer[index] = (CHAR)guestBuffer[index];
        buffer[index] = 0;
        lParam32 = (LPARAM)buffer;
    }
    if (kind == WOWUSER_LIST_OUT_BUFFER && hasStrings) {
        LRESULT length = SendMessageA(window->Window32, isComboBox ? CB_GETLBTEXTLEN
                                                  : LB_GETTEXTLEN, wParam32, 0);
        INT index;
        guestBuffer = WowUserLinear(frame, lParam);
        if (!guestBuffer || length < 0 || length >= (LRESULT)sizeof buffer) { *noteLengthInOut = noteLength; *out = -1; return 1; }
        result = SendMessageA(window->Window32, message32, wParam32, (LPARAM)buffer);
        if (result >= 0) for (index = 0; index <= (INT)result && index < (INT)sizeof buffer; ++index) guestBuffer[index] = (BYTE)buffer[index];
    } else if (kind == WOWUSER_LIST_OUT_BUFFER) {
        /* ⚠ #304 (M7): GETTEXT on an owner-draw control WITHOUT strings returns the
             item's DATA, a DWORD, through lParam. This fell to the raw send below,
             which handed Win32 the guest's 16:16 pointer as a flat address -- and
             Win32 wrote four bytes there. The DWORD goes into the guest's buffer. */
        DWORD value = 0;
        guestBuffer = WowUserLinear(frame, lParam);
        result = SendMessageA(window->Window32, message32, wParam32, (LPARAM)&value);
        if (guestBuffer && result >= 0) { guestBuffer[0] = (BYTE)value; guestBuffer[1] = (BYTE)(value >> BYTE_SHIFT);
                            guestBuffer[2] = (BYTE)(value >> WORD_SHIFT); guestBuffer[3] = (BYTE)(value >> TOP_BYTE_SHIFT); }
    } else {
        result = SendMessageA(window->Window32, message32, wParam32, lParam32);
    }
    WowNotePut(note, noteCapacity, &noteLength, isComboBox ? "CB_ n=0x" : "LB_ n=0x");
    WowNoteHex(note, noteCapacity, &noteLength, (DWORD)messageIndex, WOW_HEX_BYTE_DIGITS);
    if (kind == WOWUSER_LIST_IN_STRING && hasStrings) { WowNotePut(note, noteCapacity, &noteLength, " \""); WowNotePut(note, noteCapacity, &noteLength, buffer);
                             WowNotePut(note, noteCapacity, &noteLength, "\""); }
    WowNotePut(note, noteCapacity, &noteLength, " -> the real control -> 0x");
    WowNoteHex(note, noteCapacity, &noteLength, (DWORD)result, WOW_HEX_DWORD_DIGITS);
    *noteLengthInOut = noteLength;
    *out = (LONG)result;
    return 1;
}

static INT WowUserDestroy(WORD window16, PSTR note, INT noteCapacity, PINT noteLengthInOut);
static LONG WowUserDefProc(PWOW32_FRAME frame, PWOWUSER_WINDOW window, WORD message,
                            WORD wParam, DWORD lParam, PSTR note, INT noteCapacity)
{
    INT noteLength = 0;
    /* s89: a list or combo box's own messages, before anything reads the range as
       an edit control's (0x040C is EM_SETHANDLE only to an EDIT). */
    if (message >= WM_USER && message <= WOWUSER_CONTROL_MESSAGE_LAST16 && window && window->Window32) {
        CHAR className[WOWUSER_CONTROL_CLASS_SIZE]; LONG listResult;
        if (GetClassNameA(window->Window32, className, sizeof className)) {
            INT isComboBox = !lstrcmpiA(className, "ComboBox"), isListBox = !lstrcmpiA(className, "ListBox");
            if ((isComboBox || isListBox)
                && WowUserListMessage(frame, window, message, wParam, lParam, isComboBox, &listResult, note, noteCapacity, &noteLength))
                return listResult;
            /* ── #301 (M2): A BUTTON's own messages. Win16 BM_GETCHECK..BM_SETSTYLE
                 are WM_USER+0..4; Win32 moved the same five, same order, to 0xF0.
                 All plain values (BM_SETSTYLE's lParam is the redraw flag in both),
                 so the number is the whole translation. Answered 0 before, so a
                 dialog that set its check boxes by message showed them all clear
                 and read them back as clear. Keyed on the class, like EM_ and
                 LB_/CB_: the same numbers mean other things to other controls. */
            if (!lstrcmpiA(className, "Button") && message <= WOWUSER_BM_LAST16) {
                LRESULT buttonResult = SendMessageA(window->Window32, (UINT)(BM_GETCHECK + (message - WOWUSER_BM_BASE16)),
                                          (WPARAM)wParam, (LPARAM)lParam);
                WowNotePut(note, noteCapacity, &noteLength, "BM_ n=0x");
                WowNoteHex(note, noteCapacity, &noteLength, (DWORD)(message - WOWUSER_BM_BASE16), WOW_HEX_BYTE_DIGITS);
                WowNotePut(note, noteCapacity, &noteLength, " -> the real BUTTON -> 0x");
                WowNoteHex(note, noteCapacity, &noteLength, (DWORD)buttonResult, WOW_HEX_DWORD_DIGITS);
                return (LONG)buttonResult;
            }
        }
    }
    switch (message) {

    /* ── ★★★★★ WM_MDICREATE: MAKE THE CHILD WINDOW ────────────────────────────
         The message SYSEDIT stops on. lParam is a far pointer to the
         MDICREATESTRUCT above; the answer is the child's handle, and 0 means
         "no child", which is what the program has been correctly reporting as
         *"Cannot open this file."* */
    case WM_MDICREATE16: {
        volatile BYTE *createStruct = WowUserFarPointer(frame, lParam);
        CHAR className[WOWUSER_NAME_SIZE], title[WOWUSER_NAME_SIZE];
        PWOWUSER_CLASS windowClass;
        PWOWUSER_WINDOW child;
        INT index;
        if (!createStruct) { WowNotePut(note, noteCapacity, &noteLength, "WM_MDICREATE: unreadable "
                                             "MDICREATESTRUCT"); return 0; }
        WowUserFarString(frame, (DWORD)WowUserPeek(createStruct, WOWUSER_MCS_SZCLASS)
                          | ((DWORD)WowUserPeek(createStruct, WOWUSER_MCS_SZCLASS + WOW_WORD_BYTES) << WORD_SHIFT),
                       className, sizeof className);
        WowUserFarString(frame, (DWORD)WowUserPeek(createStruct, WOWUSER_MCS_SZTITLE)
                          | ((DWORD)WowUserPeek(createStruct, WOWUSER_MCS_SZTITLE + WOW_WORD_BYTES) << WORD_SHIFT),
                       title, sizeof title);
        windowClass = className[0] ? WowUserFindClass(className) : NULL;
        if (!windowClass) {
            WowNotePut(note, noteCapacity, &noteLength, "WM_MDICREATE: no such class ");
            WowNoteQuoted(note, noteCapacity, &noteLength, className);
            return 0;
        }
        child = WowUserNewWindow();
        if (!child) { WowNotePut(note, noteCapacity, &noteLength, "WM_MDICREATE: no window slot");
                   return 0; }
        child->Class     = (WORD)(windowClass - g_WowUserClasses);
        child->WindowProcedure = windowClass->WindowProcedure;  /* per WINDOW, as at CreateWindow */
        child->Style   = (DWORD)WowUserPeek(createStruct, WOWUSER_MCS_STYLE)
                    | ((DWORD)WowUserPeek(createStruct, WOWUSER_MCS_STYLE + WOW_WORD_BYTES) << WORD_SHIFT);
        child->Parent  = window->Window16;    /* ★ the MDI CLIENT is the parent */
        child->Menu    = 0;
        child->Instance   = WowUserPeek(createStruct, WOWUSER_MCS_HOWNER);
        {   WORD positionX  = WowUserPeek(createStruct, WOWUSER_MCS_X),  positionY  = WowUserPeek(createStruct, WOWUSER_MCS_Y);
            WORD width = WowUserPeek(createStruct, WOWUSER_MCS_CX), height = WowUserPeek(createStruct, WOWUSER_MCS_CY);
            child->PositionX  = (positionX  == CW_USEDEFAULT16) ? 0 : (INT)(SHORT)positionX;
            child->PositionY  = (positionY  == CW_USEDEFAULT16) ? 0 : (INT)(SHORT)positionY;
            child->Width = (width == CW_USEDEFAULT16) ? WOWUSER_DESK_CX : (INT)(SHORT)width;
            child->Height = (height == CW_USEDEFAULT16) ? WOWUSER_DESK_CY : (INT)(SHORT)height;
        }
        for (index = 0; index < (INT)sizeof child->Text; ++index) child->Text[index] = title[index];
        /* ── ★★★★★ AND THE REAL MDI CLIENT MAKES THE REAL CHILD. (session 42) ──
             The first cut created it with a plain CreateWindowEx, and the run
             said why that is not the same thing: SYSEDIT passes **style 0**, and
             `WOWUSER_MCS_STYLE == 0` is not "no style", it is *"give me the MDI
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
           ⚠ Win32 sends its own WM_CREATE to `WowWinProc` from inside this
             SendMessage, before `ch->hwnd32` is set, so the child is momentarily
             unknown to `WowWinHwnd16`. That is correct and harmless: an unknown
             HWND falls through to DefWindowProc, and the message that matters to
             the guest is the WIN16 WM_CREATE requested below. */
        if (windowClass->IsRegistered32 && window->Window32) {
            MDICREATESTRUCTA mdiCreate;
            HWND previous32 = (HWND)(ULONG_PTR)SendMessageA(window->Window32, WM_MDIGETACTIVE, 0, 0);
            ZeroMemory(&mdiCreate, sizeof mdiCreate);
            mdiCreate.szClass = windowClass->Class32;
            mdiCreate.szTitle = child->Text;
            mdiCreate.hOwner  = GetModuleHandleA(NULL);
            mdiCreate.x       = WowWinCoordinate(WowUserPeek(createStruct, WOWUSER_MCS_X));
            mdiCreate.y       = WowWinCoordinate(WowUserPeek(createStruct, WOWUSER_MCS_Y));
            mdiCreate.cx      = WowWinCoordinate(WowUserPeek(createStruct, WOWUSER_MCS_CX));
            mdiCreate.cy      = WowWinCoordinate(WowUserPeek(createStruct, WOWUSER_MCS_CY));
            mdiCreate.style   = child->Style;
            child->Window32  = (HWND)(ULONG_PTR)SendMessageA(window->Window32, WM_MDICREATE16,
                                                        0, (LPARAM)&mdiCreate);
            if (child->Window32) {
                RECT rect;
                ++g_WowWinCreated;
                /* Take the rectangle back FROM the MDI client rather than keeping
                   the one we asked for: it chose, and every later answer this
                   host gives about this window has to agree with the screen. */
                if (GetWindowRect(child->Window32, &rect)) {
                    POINT point; point.x = rect.left; point.y = rect.top;
                    ScreenToClient(window->Window32, &point);
                    child->PositionX = point.x; child->PositionY = point.y;
                    child->Width = rect.right - rect.left; child->Height = rect.bottom - rect.top;
                }
                /* s91 (#305 M13): THE NEW CHILD IS THE ACTIVE ONE, as stock answers
                   (w_mdi: WM_MDIGETACTIVE right after two WM_MDICREATEs names the
                   second). The real client did not activate it here -- the frame
                   is hidden at this point in the probe and in most programs. */
                SendMessageA(window->Window32, WM_MDIACTIVATE, (WPARAM)child->Window32, 0);
                /* ── s93: AND THE CHILD IS TOLD. Win32's client activated it INSIDE the
                     WM_MDICREATE above, before ch->hwnd32 was known, so the relay had no
                     Win16 handle for it and the WM_MDIACTIVATE was dropped -- and the
                     SendMessage just above changes nothing (it is already active). Win16
                     tells the old child it lost and the new one it gained, after
                     WM_CREATE; SYSEDIT enables File > Save only for an active file, so
                     without this it was greyed for good. Posted, so WM_CREATE (armed
                     below) runs first. */
                {   WORD previous16 = previous32 ? WowWinHwnd16(previous32) : 0;
                    DWORD activateLParam = ((DWORD)previous16 << WORD_SHIFT) | child->Window16;
                    if (previous16) WowMsgPost(previous16, WM_MDIACTIVATE, 0, activateLParam, GetTickCount(), 0, 0);
                    WowMsgPost(child->Window16, WM_MDIACTIVATE, 1, activateLParam, GetTickCount(), 0, 0);
                }
            }
        }
        WowNotePut(note, noteCapacity, &noteLength, "WM_MDICREATE ");
        WowNoteQuoted(note, noteCapacity, &noteLength, windowClass->Name);
        WowNotePut(note, noteCapacity, &noteLength, " ");
        WowNoteQuoted(note, noteCapacity, &noteLength, child->Text);
        WowNotePut(note, noteCapacity, &noteLength, " in client 0x");
        WowNoteHex(note, noteCapacity, &noteLength, window->Window16, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, " -> hwnd=0x");
        WowNoteHex(note, noteCapacity, &noteLength, child->Window16, WOW_HEX_WORD_DIGITS);
        /* ★ SAY WHETHER THE REAL WINDOW EXISTS, for the same reason CreateWindow
             does: a Win16 handle and a window on the desktop are two different
             achievements and only one of them can be seen. */
        if (child->Window32) {
            WowNotePut(note, noteCapacity, &noteLength, " HWND=0x");
            WowNoteHex(note, noteCapacity, &noteLength, (DWORD)(ULONG_PTR)child->Window32, WOW_HEX_DWORD_DIGITS);
            WowNotePut(note, noteCapacity, &noteLength, " @");
            WowNoteHex(note, noteCapacity, &noteLength, (DWORD)(SHORT)child->PositionX, WOW_HEX_WORD_DIGITS);
            WowNotePut(note, noteCapacity, &noteLength, ",");
            WowNoteHex(note, noteCapacity, &noteLength, (DWORD)(SHORT)child->PositionY, WOW_HEX_WORD_DIGITS);
            WowNotePut(note, noteCapacity, &noteLength, " ");
            WowNoteHex(note, noteCapacity, &noteLength, (DWORD)(SHORT)child->Width, WOW_HEX_WORD_DIGITS);
            WowNotePut(note, noteCapacity, &noteLength, "x");
            WowNoteHex(note, noteCapacity, &noteLength, (DWORD)(SHORT)child->Height, WOW_HEX_WORD_DIGITS);
            WowNotePut(note, noteCapacity, &noteLength, " style=0x");
            WowNoteHex(note, noteCapacity, &noteLength, child->Style, WOW_HEX_DWORD_DIGITS);
        } else {
            WowNotePut(note, noteCapacity, &noteLength, " -- ★ NO REAL WINDOW (Win32 gle=0x");
            WowNoteHex(note, noteCapacity, &noteLength, GetLastError(), WOW_HEX_DWORD_DIGITS);
            WowNotePut(note, noteCapacity, &noteLength, ")");
        }
        WowUserWantCreate(frame, windowClass, child);
        return (LONG)child->Window16;
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
    /* ── #305 M13 (s91): THE REST OF THE MDI CLIENT'S MESSAGES. Same numbers in
         Win16 and Win32 (0x221-0x228); the child handles cross through the table.
         Two differ in their packing:
           WM_MDIGETACTIVE (0x229): Win16 answers DX:AX = (fMaximized, hwnd); Win32
             answers the hwnd and writes the flag through lParam.
           WM_MDISETMENU (0x230): Win16 lParam = MAKELONG(hmenuFrame, hmenuWindow)
             and wParam = fRefresh; Win32 wParam/lParam = the two menus. Our menus
             are real HMENUs on the real frame, so it is answered by refreshing
             (DrawMenuBar) and the frame's current menu, not by swapping.
         WM_MDIDESTROY goes through this host's own DestroyWindow path, so the
         child's record and its WM_DESTROY are handled as for any window. */
    case WM_MDIDESTROY: case WM_MDIACTIVATE: case WM_MDIRESTORE: case WM_MDINEXT: case WM_MDIMAXIMIZE:
    case WM_MDITILE: case WM_MDICASCADE: case WM_MDIICONARRANGE: case WM_MDIGETACTIVE: case WM_MDISETMENU: {
        LRESULT result = 0;
        HWND child32 = (message >= WM_MDIDESTROY && message <= WM_MDIMAXIMIZE) ? WowUserHwnd32(wParam) : NULL;
        if (!window || !window->Window32 || !g_WowUserClasses[window->Class].IsSystemClass) {
            WowNotePut(note, noteCapacity, &noteLength, "WM_MDI* to a window that is not an MDI client");
            return 0;
        }
        if (message == WM_MDIDESTROY) {
            WowNotePut(note, noteCapacity, &noteLength, "WM_MDIDESTROY -> ");
            WowUserDestroy(wParam, note, noteCapacity, &noteLength);
            return 0;
        }
        if (message >= WM_MDIDESTROY && message <= WM_MDIMAXIMIZE && !child32 && message != WM_MDINEXT) {
            WowNotePut(note, noteCapacity, &noteLength, "WM_MDI* names no window of ours; 0");
            return 0;
        }
        switch (message) {
        case WM_MDIGETACTIVE: {
            BOOL isMaximized = FALSE;
            HWND active = (HWND)SendMessageA(window->Window32, WM_MDIGETACTIVE, 0, (LPARAM)&isMaximized);
            WORD active16 = WowWinHwnd16(active);
            WowNotePut(note, noteCapacity, &noteLength, "WM_MDIGETACTIVE -> 0x");
            WowNoteHex(note, noteCapacity, &noteLength, active16, WOW_HEX_WORD_DIGITS);
            return (LONG)(((DWORD)(isMaximized ? 1 : 0) << WORD_SHIFT) | active16);
        }
        case WM_MDISETMENU: {
            HWND frameWindow = GetParent(window->Window32);
            if (frameWindow) DrawMenuBar(frameWindow);
            WowNotePut(note, noteCapacity, &noteLength, "WM_MDISETMENU -> refreshed the frame's menu");
            return 0;
        }
        case WM_MDINEXT:
            result = SendMessageA(window->Window32, message, (WPARAM)child32, (LPARAM)(lParam ? 1 : 0));
            break;
        case WM_MDITILE: case WM_MDICASCADE: case WM_MDIICONARRANGE:
            result = SendMessageA(window->Window32, message, (WPARAM)wParam, 0);
            break;
        default:
            result = SendMessageA(window->Window32, message, (WPARAM)child32, 0);
            break;
        }
        WowNotePut(note, noteCapacity, &noteLength, "WM_MDI 0x");
        WowNoteHex(note, noteCapacity, &noteLength, message, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, " -> the real MDI client -> 0x");
        WowNoteHex(note, noteCapacity, &noteLength, (DWORD)result, WOW_HEX_DWORD_DIGITS);
        return (LONG)result;
    }

    case EM_GETHANDLE16: {
        INT  textLength    = window->Window32 ? GetWindowTextLengthA(window->Window32) : 0;
        WORD needed = (WORD)(textLength + 1);
        if (needed < WOWUSER_EDIT_INITIAL) needed = WOWUSER_EDIT_INITIAL;
        if (!frame->IsCallbackAllowed || !window->Instance || !g_WowUserKernelSegment) {
            WowNotePut(note, noteCapacity, &noteLength, "EM_GETHANDLE: cannot reach the guest's heap"
                                       " (no callback, no instance, or krnl386's"
                                       " segment is not known yet), answered 0x");
            WowNoteHex(note, noteCapacity, &noteLength, window->Memory16, WOW_HEX_WORD_DIGITS);
            return (LONG)window->Memory16;
        }
        WowNotePut(note, noteCapacity, &noteLength, "EM_GETHANDLE: the real control holds 0x");
        WowNoteHex(note, noteCapacity, &noteLength, (DWORD)textLength, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, " char(s); ");
        if (window->Memory16) {
            WowNotePut(note, noteCapacity, &noteLength, "LocalReAlloc 0x");
            WowNoteHex(note, noteCapacity, &noteLength, window->Memory16, WOW_HEX_WORD_DIGITS);
            WowNotePut(note, noteCapacity, &noteLength, " to 0x");
            WowNoteHex(note, noteCapacity, &noteLength, needed, WOW_HEX_WORD_DIGITS);
            frame->CallbackProcedure   = ((DWORD)g_WowUserKernelSegment << WORD_SHIFT) | WOWUSER_KRNL_LOCALREALLOC_OFF;
            frame->CallbackArguments[0] = window->Memory16;
            frame->CallbackArguments[1] = needed;
            frame->CallbackArguments[2] = LMEM_MOVEABLE_ZEROINIT;
            frame->CallbackArgumentCount   = WOWUSER_LOCALREALLOC_ARGUMENTS;
        } else {
            WowNotePut(note, noteCapacity, &noteLength, "LocalAlloc 0x");
            WowNoteHex(note, noteCapacity, &noteLength, needed, WOW_HEX_WORD_DIGITS);
            frame->CallbackProcedure   = ((DWORD)g_WowUserKernelSegment << WORD_SHIFT) | WOWUSER_KRNL_LOCALALLOC_OFF;
            frame->CallbackArguments[0] = LMEM_MOVEABLE_ZEROINIT;
            frame->CallbackArguments[1] = needed;
            frame->CallbackArgumentCount   = WOWUSER_LOCALALLOC_ARGUMENTS;
        }
        WowNotePut(note, noteCapacity, &noteLength, " in DGROUP 0x");
        WowNoteHex(note, noteCapacity, &noteLength, window->Instance, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, ", then lock and fill it from the control");
        frame->CallbackDataSelector     = window->Instance;
        frame->CallbackReturnMode    = WOWCALL_RET_RESULTW;  /* a WORD handle in AX */
        frame->CallbackSink   = &window->Memory16;
        frame->CallbackAction    = WOWCALL_ACT_EDITLOCK;
        frame->CallbackActionArgument = window->Window16;
        frame->CallbackWindow   = window->Window16;
        frame->CallbackMessage    = message;
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
        window->Memory16 = wParam;
        WowNotePut(note, noteCapacity, &noteLength, "EM_SETHANDLE 0x");
        WowNoteHex(note, noteCapacity, &noteLength, wParam, WOW_HEX_WORD_DIGITS);
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
        if (frame->IsCallbackAllowed && window->Window32 && window->Instance && g_WowUserKernelSegment && wParam) {
            frame->CallbackProcedure   = ((DWORD)g_WowUserKernelSegment << WORD_SHIFT) | WOWUSER_KRNL_LOCALLOCK_OFF;
            frame->CallbackDataSelector     = window->Instance;
            frame->CallbackArguments[0] = wParam;
            frame->CallbackArgumentCount   = 1;
            frame->CallbackReturnMode    = WOWCALL_RET_KEEP;
            frame->CallbackSink   = NULL;
            frame->CallbackAction    = WOWCALL_ACT_EDITTEXT;
            frame->CallbackActionArgument = window->Window16;
            frame->CallbackWindow   = window->Window16;
            frame->CallbackMessage    = message;
            WowNotePut(note, noteCapacity, &noteLength, " -- asking the guest's KERNEL.8 LocalLock"
                                       " for its text");
        } else {
            WowNotePut(note, noteCapacity, &noteLength, " -- recorded, but nothing can read it"
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
         is written before WowCallEnter is reached, and the chain uses RET_KEEP
         so nothing overwrites it. The sink still fires, because a handle that
         moved must still be recorded.
       ⚠ A Win16 LOCAL handle is STABLE across LocalReAlloc (the memory moves,
         the handle does not), which is what makes this safe -- Notepad is still
         holding that handle and will write through it a moment from now.
       ⚠ Only when there is a block to refresh. Before EM_SETHANDLE there is
         nothing the guest owns and nothing to update. */
    case WM_GETTEXTLENGTH16: {
        INT textLength = window->Window32 ? GetWindowTextLengthA(window->Window32) : 0;
        WowNotePut(note, noteCapacity, &noteLength, "WM_GETTEXTLENGTH -> 0x");
        WowNoteHex(note, noteCapacity, &noteLength, (DWORD)textLength, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, window->Window32 ? " (the real control's)"
                                             : " -- no real control");
        if (textLength > 0 && window->Memory16 && window->Window32 && frame->IsCallbackAllowed && window->Instance
            && g_WowUserKernelSegment) {
            frame->CallbackProcedure   = ((DWORD)g_WowUserKernelSegment << WORD_SHIFT) | WOWUSER_KRNL_LOCALREALLOC_OFF;
            frame->CallbackDataSelector     = window->Instance;
            frame->CallbackArguments[0] = window->Memory16;
            frame->CallbackArguments[1] = (WORD)(textLength + 1);
            frame->CallbackArguments[2] = LMEM_MOVEABLE_ZEROINIT;
            frame->CallbackArgumentCount   = WOWUSER_LOCALREALLOC_ARGUMENTS;
            frame->CallbackReturnMode    = WOWCALL_RET_KEEP;  /* the LENGTH is the answer */
            frame->CallbackSink   = &window->Memory16;
            frame->CallbackAction    = WOWCALL_ACT_EDITLOCK;
            frame->CallbackActionArgument = window->Window16;
            frame->CallbackWindow   = window->Window16;
            frame->CallbackMessage    = message;
            WowNotePut(note, noteCapacity, &noteLength, "; refreshing the guest's block 0x");
            WowNoteHex(note, noteCapacity, &noteLength, window->Memory16, WOW_HEX_WORD_DIGITS);
            WowNotePut(note, noteCapacity, &noteLength, " from the control before it writes");
        }
        return (LONG)textLength;
    }

    case WM_GETTEXT16: {
        volatile BYTE *destination = WowUserLinear(frame, lParam);
        INT textLength = 0;
        WowNotePut(note, noteCapacity, &noteLength, "WM_GETTEXT max=0x");
        WowNoteHex(note, noteCapacity, &noteLength, wParam, WOW_HEX_WORD_DIGITS);
        if (!destination || !window->Window32 || !wParam) {
            WowNotePut(note, noteCapacity, &noteLength, " -- no buffer or no real control;"
                                       " answered 0");
            return 0;
        }
        textLength = GetWindowTextA(window->Window32, (LPSTR)destination, (INT)wParam);
        WowNotePut(note, noteCapacity, &noteLength, " -> 0x");
        WowNoteHex(note, noteCapacity, &noteLength, (DWORD)textLength, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, " char(s) into the guest's own buffer");
        return (LONG)textLength;
    }

    case WM_SETTEXT16: {
        volatile BYTE *source = WowUserLinear(frame, lParam);
        WowNotePut(note, noteCapacity, &noteLength, "WM_SETTEXT ");
        if (!source || !window->Window32) {
            WowNotePut(note, noteCapacity, &noteLength, "-- no string or no real control;"
                                       " answered 0");
            return 0;
        }
        WowNoteQuoted(note, noteCapacity, &noteLength, (PCSTR)source);
        SetWindowTextA(window->Window32, (LPCSTR)source);
        return 1;
    }

    /* ── #160: THE EDIT MENU. Cut, Copy, Paste, Delete and Undo are parameterless
         and Win16 and Win32 share their numbers, so the real control does the work
         -- including reaching the host clipboard, which is why Notepad's copy and
         paste need no bridge of their own. */
    case WM_CUT16: case WM_COPY16: case WM_PASTE16: case WM_CLEAR16: case WM_UNDO16: {
        LRESULT result = window->Window32 ? SendMessageA(window->Window32, message, 0, 0) : 0;
        WowNotePut(note, noteCapacity, &noteLength, "edit command msg 0x");
        WowNoteHex(note, noteCapacity, &noteLength, message, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, window->Window32 ? " -> the real control"
                                             : " -- no real control; answered 0");
        return (LONG)result;
    }

    default:
        /* ── #160: THE EM_ MESSAGES AN EDIT MENU ASKS. Win16 numbers them WM_USER+n
             (0x400+n), Win32 0xB0+n with the same n -- EM_SETHANDLE/GETHANDLE above
             are n=12/13 of the same list. Only for a real EDIT: 0x400+n is also
             listbox and combobox territory, where the same number means something else.
             Only the ones with no pointer in them; EM_SETSEL's arguments move from
             lParam's two halves (Win16) to wParam/lParam (Win32). */
        if (message >= WOWUSER_EM_BASE16 && message <= WOWUSER_EM_LAST16 && window->Window32) {
            static const BYTE editPlainOk[WOWUSER_EM_COUNT16] = {
                /* n: 0 GETSEL, 1 SETSEL, 8 GETMODIFY, 9 SETMODIFY, 10 GETLINECOUNT,
                      11 LINEINDEX, 17 LINELENGTH, 21 LIMITTEXT, 22 CANUNDO, 23 UNDO,
                      25 LINEFROMCHAR, 29 EMPTYUNDOBUFFER */
                1,1,0,0,0,0,0,0, 1,1,1,1,0,0,0,0, 0,1,0,0,0,1,1,1, 0,1,0,0,0,1 };
            /* #304 (M5, s90): the rest of the list, each by its own shape --
               2 GETRECT, 3 SETRECT, 4 SETRECTNP (RECT16), 5 SCROLL (values),
               6 LINESCROLL (Win16 packs vert/horz into lParam; Win32 splits them),
               18 REPLACESEL (string in), 20 GETLINE (buffer, first WORD = size),
               24 FMTLINES, 28 SETPASSWORDCHAR (values), 27 SETTABSTOPS (INT16[]).
               19 SETFONT is Windows 3.0's and "not used" by 3.1's EDIT; 26
               SETWORDBREAK needs a 16-bit callback inside Win32's EDIT -- both
               still answer 0, and say so. */
            static const BYTE editCarriesStructure[WOWUSER_EM_COUNT16] = {
                0,0,1,1,1,1,1,0, 0,0,0,0,0,0,0,0, 0,0,1,0,1,0,0,0, 1,0,0,1,1,0 };
            CHAR className[WOWUSER_CONTROL_CLASS_SIZE];
            INT  editIndex = message - WOWUSER_EM_BASE16;
            if (editCarriesStructure[editIndex] && GetClassNameA(window->Window32, className, sizeof className)
                && !lstrcmpiA(className, "Edit")) {
                volatile BYTE *guest = WowUserLinear(frame, lParam);
                LRESULT result = 0;
                INT index;
                UINT message32 = (UINT)(EM_GETSEL + editIndex);
                switch (editIndex) {
                case WOWUSER_EM_INDEX(EM_GETRECT): case WOWUSER_EM_INDEX(EM_SETRECT): case WOWUSER_EM_INDEX(EM_SETRECTNP): {                     /* RECT16 out / in    */
                    RECT rect;
                    rect.left = rect.top = rect.right = rect.bottom = 0;
                    if (editIndex != WOWUSER_EM_INDEX(EM_GETRECT) && guest) {
                        rect.left  = (SHORT)(guest[0] | (guest[1] << BYTE_SHIFT)); rect.top    = (SHORT)(guest[2] | (guest[3] << BYTE_SHIFT));
                        rect.right = (SHORT)(guest[4] | (guest[5] << BYTE_SHIFT)); rect.bottom = (SHORT)(guest[6] | (guest[7] << BYTE_SHIFT));
                    }
                    result = SendMessageA(window->Window32, message32, 0, (editIndex != WOWUSER_EM_INDEX(EM_GETRECT) && !guest) ? 0 : (LPARAM)&rect);
                    if (editIndex == WOWUSER_EM_INDEX(EM_GETRECT) && guest) {
                        LONG values[WOWUSER_RECT16_WORDS]; values[0] = rect.left; values[1] = rect.top; values[2] = rect.right; values[3] = rect.bottom;
                        for (index = 0; index < WOWUSER_RECT16_WORDS; ++index) { guest[index * WOW_WORD_BYTES] = (BYTE)values[index]; guest[index * WOW_WORD_BYTES + 1] = (BYTE)(values[index] >> BYTE_SHIFT); }
                    }
                    break; }
                case WOWUSER_EM_INDEX(EM_SCROLL): case WOWUSER_EM_INDEX(EM_FMTLINES): case WOWUSER_EM_INDEX(EM_SETPASSWORDCHAR):
                    result = SendMessageA(window->Window32, message32, (WPARAM)wParam, (LPARAM)lParam);
                    break;
                case WOWUSER_EM_INDEX(EM_LINESCROLL):                                        /* LINESCROLL */
                    result = SendMessageA(window->Window32, message32, (WPARAM)(LONG)(SHORT)(lParam >> WORD_SHIFT),
                                     (LPARAM)(LONG)(SHORT)(lParam & WORD_MASK));
                    break;
                case WOWUSER_EM_INDEX(EM_REPLACESEL): {                                      /* REPLACESEL */
                    static CHAR replaceText[WOWUSER_EDIT_TEXT_SIZE];
                    for (index = 0; guest && index < (INT)sizeof replaceText - 1 && guest[index]; ++index) replaceText[index] = (CHAR)guest[index];
                    replaceText[index] = 0;
                    result = SendMessageA(window->Window32, message32, (WPARAM)wParam, (LPARAM)replaceText);
                    break; }
                case WOWUSER_EM_INDEX(EM_GETLINE): {                                      /* GETLINE    */
                    static CHAR lineBuffer[WOWUSER_EDIT_TEXT_SIZE];
                    WORD capacity = guest ? (WORD)(guest[0] | (guest[1] << BYTE_SHIFT)) : 0;
                    if (capacity > sizeof lineBuffer - 1) capacity = sizeof lineBuffer - 1;
                    lineBuffer[0] = (CHAR)capacity; lineBuffer[1] = (CHAR)(capacity >> BYTE_SHIFT);
                    result = (guest && capacity) ? SendMessageA(window->Window32, message32, (WPARAM)(LONG)(SHORT)wParam,
                                                  (LPARAM)lineBuffer) : 0;
                    for (index = 0; guest && index < (INT)result && index < capacity; ++index) guest[index] = (BYTE)lineBuffer[index];
                    break; }
                case WOWUSER_EM_INDEX(EM_SETTABSTOPS): {                                      /* SETTABSTOPS */
                    static INT tabStops[WOWUSER_MAX_TAB_STOPS];
                    INT count = (INT)(wParam > WOWUSER_MAX_TAB_STOPS ? WOWUSER_MAX_TAB_STOPS : wParam);
                    for (index = 0; guest && index < count; ++index) tabStops[index] = (INT)(SHORT)(guest[index * WOW_WORD_BYTES] | (guest[index * WOW_WORD_BYTES + 1] << BYTE_SHIFT));
                    result = SendMessageA(window->Window32, message32, (WPARAM)count, count ? (LPARAM)tabStops : 0);
                    break; }
                }
                WowNotePut(note, noteCapacity, &noteLength, "EM_ n=0x");
                WowNoteHex(note, noteCapacity, &noteLength, (DWORD)editIndex, WOW_HEX_BYTE_DIGITS);
                WowNotePut(note, noteCapacity, &noteLength, " (translated) -> the real EDIT -> 0x");
                WowNoteHex(note, noteCapacity, &noteLength, (DWORD)result, WOW_HEX_DWORD_DIGITS);
                return (LONG)result;
            }
            if (editPlainOk[editIndex] && GetClassNameA(window->Window32, className, sizeof className)
                && !lstrcmpiA(className, "Edit")) {
                WPARAM wParam32 = wParam; LPARAM lParam32 = (LPARAM)lParam;
                LRESULT result;
                if (editIndex == WOWUSER_EM_INDEX(EM_SETSEL)) {                         /* EM_SETSEL */
                    WORD start = (WORD)(lParam & WORD_MASK), end = (WORD)(lParam >> WORD_SHIFT);
                    wParam32 = (start == WOWUSER_MINUS_ONE16) ? (WPARAM)-1 : start;
                    lParam32 = (end == WOWUSER_MINUS_ONE16 || end == WOWUSER_EM_SELECT_TO_END16) ? (LPARAM)-1 : end;
                } else if ((editIndex == WOWUSER_EM_INDEX(EM_LINEINDEX) || editIndex == WOWUSER_EM_INDEX(EM_LINELENGTH) || editIndex == WOWUSER_EM_INDEX(EM_LINEFROMCHAR)) && wParam == WOWUSER_MINUS_ONE16) {
                    wParam32 = (WPARAM)-1;                    /* "the current line" */
                }
                result = SendMessageA(window->Window32, (UINT)(EM_GETSEL + editIndex), wParam32, lParam32);
                WowNotePut(note, noteCapacity, &noteLength, "EM_ n=0x");
                WowNoteHex(note, noteCapacity, &noteLength, (DWORD)editIndex, WOW_HEX_BYTE_DIGITS);
                WowNotePut(note, noteCapacity, &noteLength, " -> the real EDIT -> 0x");
                WowNoteHex(note, noteCapacity, &noteLength, (DWORD)result, WOW_HEX_DWORD_DIGITS);
                return (LONG)result;
            }
        }
        /* #305 M8 (s91): WM_SETFONT / WM_GETFONT to a SYSTEM control. The font
           crosses as a GDI token: in, mapped to the real HFONT (0 = the system
           font, as both sides say); out, the token this host already gave the
           guest for that HFONT, or -- a font the guest never held (the dialog
           manager's) -- one minted as STOCK, so a DeleteObject on it cannot free
           the control's font from under it. Answered 0 before: a dialog that set
           its edit's font saw nothing, and WM_GETFONT always said "system". */
        if (window && window->Window32 && g_WowUserClasses[window->Class].IsSystemClass
            && (message == WM_SETFONT || message == WM_GETFONT)) {
            if (message == WM_SETFONT) {
                INT kind = -1;
                HGDIOBJ font = wParam ? WowGdiH32(wParam, &kind) : NULL;
                SendMessageA(window->Window32, WM_SETFONT, (WPARAM)font, (LPARAM)(lParam & WORD_MASK));
                WowNotePut(note, noteCapacity, &noteLength, "WM_SETFONT token 0x");
                WowNoteHex(note, noteCapacity, &noteLength, wParam, WOW_HEX_WORD_DIGITS);
                WowNotePut(note, noteCapacity, &noteLength, font || !wParam ? " -> the real control"
                                                         : " (NOT a token of ours: system font)");
                return 0;
            } else {
                HGDIOBJ font = (HGDIOBJ)SendMessageA(window->Window32, WM_GETFONT, 0, 0);
                WORD token16 = 0;
                INT index;
                for (index = 0; font && index < g_WowGdiObjectCount; ++index)
                    if (g_WowGdiObjects[index].Object == font && g_WowGdiObjects[index].Handle16
                        && (g_WowGdiObjects[index].Kind == WOWGDI_KIND_OBJ
                            || g_WowGdiObjects[index].Kind == WOWGDI_KIND_STOCK)) { token16 = g_WowGdiObjects[index].Handle16; break; }
                if (font && !token16) token16 = WowGdiH16(font, WOWGDI_KIND_STOCK);
                WowNotePut(note, noteCapacity, &noteLength, "WM_GETFONT -> token 0x");
                WowNoteHex(note, noteCapacity, &noteLength, token16, WOW_HEX_WORD_DIGITS);
                return (LONG)token16;
            }
        }
        /* #308 (s91): a SYSTEM control's input/focus messages -- the same scalar set
           a subclass is sent -- go to the real control, whose answer it is. A 16-bit
           program SendMessage-ing WM_CHAR to its EDIT typed nothing before (w_subcl:
           stock's text grows, ours did not). Not for a 16-bit class: there the real
           window's procedure is WowWinProc, which would post it straight back. */
        if (window && window->Window32 && g_WowUserClasses[window->Class].IsSystemClass && WowUserSubclassRelays(message)) {
            WPARAM wParam32 = wParam;
            LPARAM lParam32 = (LPARAM)lParam;
            LRESULT result;
            if (message == WM_SETFOCUS || message == WM_KILLFOCUS || message == WM_SETCURSOR)
                wParam32 = (WPARAM)WowUserHwnd32(wParam);
            else if (message == WM_GETDLGCODE || message == WM_TIMER)
                lParam32 = 0;
            result = SendMessageA(window->Window32, message, wParam32, lParam32);
            WowNotePut(note, noteCapacity, &noteLength, "msg 0x");
            WowNoteHex(note, noteCapacity, &noteLength, message, WOW_HEX_WORD_DIGITS);
            WowNotePut(note, noteCapacity, &noteLength, " -> the real control -> 0x");
            WowNoteHex(note, noteCapacity, &noteLength, (DWORD)result, WOW_HEX_DWORD_DIGITS);
            return (message == WM_NCHITTEST) ? (LONG)(SHORT)result : (LONG)result;
        }
        WowNotePut(note, noteCapacity, &noteLength, "default procedure: msg 0x");
        WowNoteHex(note, noteCapacity, &noteLength, message, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, " not implemented, answered 0");
        (VOID)wParam;
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
/* The guest's DestroyWindow: the real window goes, its child records are released,
   and the guest is TOLD (WM_DESTROY) -- lifted out of USER 0x35 in s82 so that
   DefWindowProc's WM_CLOSE (#162) destroys a window the same way. Appends to `note`
   at *k; returns 0 if there is no such window. */
/* s89 (#305 M10): send a message to a guest window NOW, through the nested run
   (main.c wires this to WowCall16Sync with the window's own procedure and
   instance, exactly as DispatchMessage would choose them). 0 = could not. */
static INT (*g_WowUserSend16)(WORD window16, WORD message, WORD wParam, DWORD lParam, PWORD result);
/* ...and with a structure as lParam, placed on the guest's stack; `fix` lists the
   far pointers inside it that point back into it (main.c: WowSend16Blob). */
static INT (*g_WowUserSend16Blob)(WORD window16, WORD message, WORD wParam, PBYTE blob, INT blobLength,
                                  const INT *fix, INT fixCount, PWORD result);
/* s89 (#302): the modeless dialog whose WM_INITDIALOG is running right now, and
   whether the program called ShowWindow on it meanwhile (CreateDialog, below). */
static WORD g_WowUserInitDialogWindow = 0;
static INT  g_WowUserIsInitDialogShown = 0;

static INT WowUserDestroy(WORD window16, PSTR note, INT noteCapacity, PINT noteLengthInOut)
{
        PWOWUSER_WINDOW window = WowUserFindWindow(window16);
        INT noteLength = *noteLengthInOut, index, childCount = 0, isSent = 0;
        HWND window32;
        WORD result16;
        WowNotePut(note, noteCapacity, &noteLength, "DestroyWindow 0x");
        WowNoteHex(note, noteCapacity, &noteLength, window16, WOW_HEX_WORD_DIGITS);
        if (!window) { WowNotePut(note, noteCapacity, &noteLength, " -- NO SUCH WINDOW"); *noteLengthInOut = noteLength; return 0; }
        /* ⚠ RE-ENTRY: a WM_DESTROY handler that destroys its own window again is
             legal and common. It is already going; say yes and do nothing. */
        if (window->IsDying) { WowNotePut(note, noteCapacity, &noteLength, " -- already being destroyed");
                        *noteLengthInOut = noteLength; return 1; }
        window32 = window->Window32;
        /* ── ★★★ #305 (M10): WM_DESTROY IS SENT, WHILE EVERYTHING STILL EXISTS. ──
             Measured on Charmap (s89, the inventory's profile-write check): it
             saves its font in its WM_DESTROY handler by asking its own font combo
             box -- CB_GETCURSEL, CB_GETLBTEXT -- and wrote an EMPTY name, because
             this function had already destroyed the real window and released
             every child record before the posted WM_DESTROY reached it ("no real
             window"). On real Windows DestroyWindow SENDS WM_DESTROY to the window
             and then to each child, all still alive, and only then takes them
             down. Any program that saves state on close works that way.
             The nested run makes that possible now; the old posted path below
             stays as the fallback for a context where a nested call cannot run. */
        window->IsDying = 1;
        if (g_WowUserSend16 && WowUserWindowProcedureOf(window)
            && g_WowUserSend16(window16, WM_DESTROY16, 0, 0, &result16)) {
            isSent = 1;
            for (index = 0; index < g_WowUserWindowCount; ++index) {
                PWOWUSER_WINDOW child = &g_WowUserWindows[index];
                if (child->Window16 && child != window && !child->IsDying && child->Window32 && window32
                    && IsChild(window32, child->Window32) && WowUserWindowProcedureOf(child)) {
                    child->IsDying = 1;
                    g_WowUserSend16(child->Window16, WM_DESTROY16, 0, 0, &result16);
                }
            }
            window32 = window->Window32;  /* the guest may have changed things meanwhile */
        }
        window->IsDying = 0;
        for (index = 0; index < g_WowUserWindowCount; ++index) {
            PWOWUSER_WINDOW child = &g_WowUserWindows[index];
            if (child->Window16 && child != window && child->Window32 && window32 && IsChild(window32, child->Window32)) {
                child->Window16 = 0; child->Window32 = NULL; child->IsDying = 0; ++childCount;
            }
        }
        /* ── ★★★★★ AND TELL THE GUEST, WHICH THIS NEVER DID. (session 56) ──
             REPORTED BY THE USER: Win16 tray icons "stacking up". ⚠ I called
             them live hosts rather than ghosts and the user refuted it in one
             line -- "they all disappear when the mouse hovers over them", which
             only a DEAD owner's icon does. Measured: 5 icons, 0 processes.
             The chain is: this missing message left the host ALIVE with nothing
             to do, the next launch's `taskkill /f` killed it without cleanup, and
             its icon became a ghost. Fixing this line means no host is left to
             kill. See the note in main.c's exec-loop tail.
             A Win16 application ends when its window does: WM_CLOSE ->
             DestroyWindow -> **WM_DESTROY** -> PostQuitMessage -> GetMessage
             returns 0 -> WinMain returns -> the task exits -> the VDM has
             nothing left to run. We relayed WM_CLOSE (wowwin.h) and implemented
             DestroyWindow, and then dropped the middle link: WM_DESTROY was
             delivered by nothing, anywhere. So the guest destroyed its window
             and went straight back to GetMessage, where it blocked FOREVER --
             measured, `WOWMSG: blocked 0x7f23 ms` and climbing, with the window
             already gone from the desktop.
           ⚠ ORDER: post BEFORE releasing the record, and KEEP the Win16 handle
             until the message is dispatched. DispatchMessage resolves the window
             procedure THROUGH this record (WowUserFindWindow), so clearing `hwnd`
             here -- which is what the note above rightly wants for a dead
             window -- would make the message we just posted undeliverable. The
             record is marked `dying` instead and released the moment its
             WM_DESTROY is dispatched.
           ⚠ ON REAL WINDOWS WM_DESTROY IS **SENT**, NOT POSTED. Same caveat, and
             for the same reason, as WM_SIZE and WM_SETFOCUS in wowwin.h: sending
             it means re-entering the guest from inside a service. Posted, the
             guest sees it at its next GetMessage, which for this message is
             precisely where its message loop already is. */
        if (isSent) {
            /* Told already, synchronously: the record goes with the window. */
            window->Window16 = 0; window->IsDying = 0;
            window->Window32 = NULL;
            if (g_WowMsgFocus == window16) g_WowMsgFocus = 0;
            if (window32) DestroyWindow(window32);
            WowNotePut(note, noteCapacity, &noteLength, " -> WM_DESTROY SENT (nested), then destroyed");
            if (childCount) { WowNotePut(note, noteCapacity, &noteLength, ", with 0x");
                        WowNoteHex(note, noteCapacity, &noteLength, (DWORD)childCount, WOW_HEX_BYTE_DIGITS);
                        WowNotePut(note, noteCapacity, &noteLength, " child record(s) released too"); }
            *noteLengthInOut = noteLength;
            return 1;
        }
        WowMsgPost(window16, WM_DESTROY16, 0, 0, GetTickCount(), 0, 0);
        window->IsDying  = 1;
        window->Window32 = NULL;       /* the real window is going NOW... */
        if (g_WowMsgFocus == window16) g_WowMsgFocus = 0;
        if (window32) DestroyWindow(window32);
        WowNotePut(note, noteCapacity, &noteLength, " -> destroyed, WM_DESTROY posted to the guest");
        if (childCount) { WowNotePut(note, noteCapacity, &noteLength, ", with 0x");
                    WowNoteHex(note, noteCapacity, &noteLength, (DWORD)childCount, WOW_HEX_BYTE_DIGITS);
                    WowNotePut(note, noteCapacity, &noteLength, " child record(s) released too"); }
        *noteLengthInOut = noteLength;
        return 1;
}

/* ── s88: the DIALOG MANAGER'S DEFAULT for one message, once the DLGPROC has
     answered FALSE (or there is none). See WOWUSER_DEFDLGPROC. The two arms are
     the ones this host always had:
   ★ #162: a dialog's WM_CLOSE is a Cancel. Win16's DefDlgProc posts
     WM_COMMAND(IDCANCEL, BN_CLICKED) to the dialog, and the program's own
     IDCANCEL handling decides what closing means (Charmap: end the program).
   ⚠ Everything else goes to DefWindowProc on the real window: calling the OS's
     DefDlgProc on a window WE created with CreateWindow is undefined (it reads
     the dialog class's extra bytes), so a guest dialog keeps the ordinary
     defaults and loses the dialog-specific keyboard ones (default button, ESC,
     tab order) until real dialog creation lands. */
typedef struct _WOWUSER_DLGDEF { WORD WParam; DWORD LParam; } WOWUSER_DLGDEF; static WOWUSER_DLGDEF g_WowUserDlgDefaults[WOWCALL_MAX_DEPTH];
/* ⚠ AND THE LAST WINDOW WHOSE RECORD DispatchMessage RELEASED. It frees the slot
     as it dispatches WM_DESTROY (see there), so when that WM_DESTROY reaches
     DefDlgProc the window can no longer be found -- and WM_DESTROY is where a
     dialog-as-main-window program (Charmap) calls PostQuitMessage. Measured s88:
     "DefDlgProc 0x0140 msg=0x0002 -- no real window", the DLGPROC never ran, and
     the program outlived its window. Its DLGPROC is kept here for that one call. */
typedef struct _WOWUSER_GONE { WORD Window; DWORD DialogProcedure; } WOWUSER_GONE; static WOWUSER_GONE g_WowUserGone;

/* s89 (#162): a Win16 message's wParam as the OS's default procedure needs it.
   WM_ERASEBKGND carries the guest's DC TOKEN; DefWindowProc fills the real DC
   behind it with the class brush. Everything else still goes raw (see the
   forward-table note: a message joins when its parameters are translated). */
static WPARAM WowUserWParam32(WORD message, WORD wParam16)
{
    if (message == WM_ERASEBKGND) {
        INT kind = -1;
        HGDIOBJ object = WowGdiH32(wParam16, &kind);
        return (object && (kind == WOWGDI_KIND_DC || kind == WOWGDI_KIND_WINDC))
               ? (WPARAM)object : 0;
    }
    return (WPARAM)wParam16;
}

/* s92 (#314): WM_CTLCOLOR's DEFAULT, AS A BRUSH TOKEN. The default procedures
   answered 0, which the host's own path (main.c WowControlColour) turns into the
   stock-measured 3.x default -- but a GUEST that sends WM_CTLCOLOR itself gets the
   0. Media Player's SScrollBar does: it selected "brush 0" and painted its trough
   with whatever the DC held (solid blue). Same rules as WowControlColour: edit and list
   box, and static/button outside a 3-D dialog, are the window colour; the rest is
   Win32's DefWindowProc (what stock's WOW forwards to). The text and background
   colours are set on the DC, as Windows' default does. 0 if the DC is not ours. */
static WORD WowUserCtlColorDefault(PWOWUSER_WINDOW window, HWND window32, WORD dc16, DWORD lParam16)
{
    INT  kind = -1;
    HDC  dc = (HDC)WowGdiH32(dc16, &kind);
    WORD controlType = HIWORD(lParam16);
    HBRUSH brush;
    if (!dc || (kind != WOWGDI_KIND_DC && kind != WOWGDI_KIND_WINDC) || controlType > CTLCOLOR_STATIC) return 0;
    if (controlType == CTLCOLOR_EDIT || controlType == CTLCOLOR_LISTBOX || ((controlType == CTLCOLOR_BTN || controlType == CTLCOLOR_STATIC) && !(window && window->IsDialog3D))) {
        SetTextColor(dc, GetSysColor(COLOR_WINDOWTEXT));
        SetBkColor(dc, GetSysColor(COLOR_WINDOW));
        brush = GetSysColorBrush(COLOR_WINDOW);
    } else {
        brush = (HBRUSH)DefWindowProcA(window32, WM_CTLCOLORMSGBOX + controlType, (WPARAM)dc,
                                    (LPARAM)WowUserHwnd32(LOWORD(lParam16)));
    }
    return brush ? WowGdiH16((HGDIOBJ)brush, WOWGDI_KIND_OBJ) : 0;
}

/* s89: DefWindowProc's / DefDlgProc's WM_PAINT. Win16's does BeginPaint/EndPaint,
   which erases what is still owed: the class brush for a window, the dialog colour
   for a dialog. Since the relay stopped erasing on the guest's behalf (Clock), a
   window that leaves WM_PAINT to the default -- Sound Recorder's dialog -- was
   never erased at all and showed black. Takes the paint record, so the area is
   not erased twice; children are clipped out as in BeginPaint. */
static VOID WowUserDefaultPaint(PWOWUSER_WINDOW window, INT isDialog)
{
    RECT rect; INT shouldErase = 0;
    HDC dc;
    if (!window || !window->Window32) return;
    if (!WowWinPaintTake(window->Window16, &rect, &shouldErase) || !shouldErase) return;
    dc = GetDCEx(window->Window32, NULL, DCX_CACHE | DCX_CLIPCHILDREN);
    if (!dc) return;
    if (isDialog) FillRect(dc, &rect, GetSysColorBrush(COLOR_BTNFACE));
    else {
        HRGN region = CreateRectRgnIndirect(&rect);
        if (region) { SelectClipRgn(dc, region); DeleteObject(region); }
        DefWindowProcA(window->Window32, WM_ERASEBKGND, (WPARAM)dc, 0);
    }
    ReleaseDC(window->Window32, dc);
}

/* ── s91: A Win16 DEFAULT PROCEDURE'S CALL INTO WIN32, WITH THE s91 MESSAGES TRANSLATED.
     The default-procedure forwards handed lParam to Win32 as it came. Harmless while
     nothing sent these messages; since #305 M9 sends WM_GETMINMAXINFO with a 16:16
     pointer, an MDI child passing it to DefMDIChildProc made USER32 write through
     0B87:15B4 as a flat address (w_mdi, the final s91 regression run). kind: 0
     DefWindowProc, 1 DefFrameProc, 2 DefMDIChildProc. */
static PCWOW32_FRAME g_WowUserCurrentFrame;  /* the frame being serviced (sel2lin) */
#define WOWUSER_DEF_WINDOW             0     /* WowUserDef32's kind: DefWindowProc   */
#define WOWUSER_DEF_FRAME              1     /* ...DefFrameProc                      */
#define WOWUSER_DEF_MDICHILD           2     /* ...DefMDIChildProc                   */
#define WOWUSER_MINMAXINFO16_POINTS    5
#define WOWUSER_POINT16_SIZE           4
#define WOWUSER_POINT16_Y              2
static LRESULT WowUserDef32(INT kind, HWND window, HWND client, WORD message, WORD wParam16, DWORD lParam16)
{
    WPARAM wParam = WowUserWParam32(message, wParam16);
    LPARAM lParam = (LPARAM)lParam16;
    MINMAXINFO minMaxInfo;
    volatile BYTE *minMaxInfo16 = NULL;
    LRESULT result;
    INT index;
    if (message == WM_GETMINMAXINFO) {                              /* WM_GETMINMAXINFO */
        minMaxInfo16 = g_WowUserCurrentFrame ? WowUserFarPointer(g_WowUserCurrentFrame, lParam16) : NULL;
        if (!minMaxInfo16) return 0;
        for (index = 0; index < WOWUSER_MINMAXINFO16_POINTS; ++index) {
            (&minMaxInfo.ptReserved)[index].x = (LONG)(SHORT)Wow32PeekWord(minMaxInfo16 + index * WOWUSER_POINT16_SIZE);
            (&minMaxInfo.ptReserved)[index].y = (LONG)(SHORT)Wow32PeekWord(minMaxInfo16 + index * WOWUSER_POINT16_SIZE + WOWUSER_POINT16_Y);
        }
        lParam = (LPARAM)&minMaxInfo;
    } else if (message == WM_ACTIVATE) {                       /* WM_ACTIVATE */
        wParam = (WPARAM)MAKELONG(wParam16, HIWORD(lParam16));
        lParam = (LPARAM)WowUserHwnd32(LOWORD(lParam16));
    } else if (message == WM_MENUSELECT) {                       /* WM_MENUSELECT */
        wParam = (WPARAM)MAKELONG(wParam16, LOWORD(lParam16));
        lParam = 0;
    }
    result = kind == WOWUSER_DEF_FRAME ? DefFrameProcA(window, client, message, wParam, lParam)
      : kind == WOWUSER_DEF_MDICHILD ? DefMDIChildProcA(window, message, wParam, lParam)
      :             DefWindowProcA(window, message, wParam, lParam);
    if (minMaxInfo16)
        for (index = 1; index < WOWUSER_MINMAXINFO16_POINTS; ++index) {
            Wow32PokeWord(minMaxInfo16 + index * WOWUSER_POINT16_SIZE,     (WORD)(SHORT)(&minMaxInfo.ptReserved)[index].x);
            Wow32PokeWord(minMaxInfo16 + index * WOWUSER_POINT16_SIZE + WOWUSER_POINT16_Y, (WORD)(SHORT)(&minMaxInfo.ptReserved)[index].y);
        }
    return result;
}

static LRESULT WowUserDlgDefault(PWOWUSER_WINDOW window, WORD dialog16, WORD message, WORD wParam16,
                                   DWORD lParam32, PSTR note, INT noteCapacity, PINT noteLengthInOut)
{
    INT noteLength = *noteLengthInOut;
    LRESULT result;
    if (!window || !window->Window32) { WowNotePut(note, noteCapacity, &noteLength, " -- no real window; 0");
                            *noteLengthInOut = noteLength; return 0; }
    if (message == WM_CLOSE) {
        HWND cancelButton = GetDlgItem(window->Window32, IDCANCEL);
        WORD cancelButton16 = cancelButton ? WowWinHwnd16(cancelButton) : 0;
        WowMsgPost(dialog16, WM_COMMAND, IDCANCEL,
                    (DWORD)cancelButton16 | ((DWORD)BN_CLICKED << WORD_SHIFT), GetTickCount(), 0, 0);
        WowNotePut(note, noteCapacity, &noteLength, " -> WM_CLOSE: WM_COMMAND IDCANCEL posted to the"
                                   " dialog, as Win16's DefDlgProc does");
        *noteLengthInOut = noteLength;
        return 0;
    }
    /* WM_CTLCOLOR: the default brush as a TOKEN -- never Win32's raw HBRUSH, which
       truncated to garbage in the 16-bit answer (Charmap: 0x0060). It was 0 until s92;
       see WowUserCtlColorDefault for why a guest's own sender needs a real one. */
    if (message == WM_CTLCOLOR16) {
        WORD brush16 = WowUserCtlColorDefault(window, window->Window32, wParam16, lParam32);
        WowNotePut(note, noteCapacity, &noteLength, " -> WM_CTLCOLOR: the default brush, token 0x");
        WowNoteHex(note, noteCapacity, &noteLength, brush16, WOW_HEX_WORD_DIGITS);
        *noteLengthInOut = noteLength; return brush16;
    }
    if (message == WM_PAINT) {
        WowUserDefaultPaint(window, 1);
        WowNotePut(note, noteCapacity, &noteLength, " -> WM_PAINT: erased what was owed, as DefDlgProc");
        *noteLengthInOut = noteLength;
        return 0;
    }
    /* WM_ERASEBKGND: Win16's DefDlgProc erases with the DIALOG colour (button
       face; WM_CTLCOLOR(CTLCOLOR_DLG)'s default), not the class brush -- with the
       class brush Sound Recorder came out black and Charmap white (s89). */
    if (message == WM_ERASEBKGND) {
        HDC eraseDc = (HDC)WowUserWParam32(message, wParam16);
        RECT rect;
        if (!eraseDc) { *noteLengthInOut = noteLength; return 0; }
        GetClientRect(window->Window32, &rect);
        FillRect(eraseDc, &rect, GetSysColorBrush(COLOR_BTNFACE));
        WowNotePut(note, noteCapacity, &noteLength, " -> WM_ERASEBKGND: the dialog colour, as DefDlgProc");
        *noteLengthInOut = noteLength;
        return 1;
    }
    result = WowUserDef32(0, window->Window32, NULL, message, wParam16, lParam32);
    WowNotePut(note, noteCapacity, &noteLength, " -> DefWindowProc (no dialog keyboard defaults) = 0x");
    WowNoteHex(note, noteCapacity, &noteLength, (DWORD)result, WOW_HEX_DWORD_DIGITS);
    *noteLengthInOut = noteLength;
    return result;
}


/* s92: FindWindow BY A WIN16 CLASS NAME. A guest's class is registered in Win32 under
   WOWWIN_CLASS_PREFIX + its name ("NTVDMEX16.MS_WINHELP"), so the bare name never
   matched a guest window -- FindWindow by class found only system classes (#32770 and
   friends, which keep their names). The prefixed name first, then the bare one. */
static HWND WowUserFindWindowByClass(PCSTR className, PCSTR windowName)
{
    HWND window = NULL;
    if (className && className[0]) {
        CHAR fullName[WOWUSER_FULL_CLASS_NAME_SIZE];
        INT  length = 0, index;
        for (index = 0; WOWWIN_CLASS_PREFIX[index] && length < (INT)sizeof fullName - 1; ++index) fullName[length++] = WOWWIN_CLASS_PREFIX[index];
        for (index = 0; className[index] && length < (INT)sizeof fullName - 1; ++index) fullName[length++] = className[index];
        fullName[length] = 0;
        window = FindWindowA(fullName, windowName);
    }
    return window ? window : FindWindowA(className, windowName);
}

/* ── The dispatcher's named values. ─────────────────────────────────────────── */
#define WOWUSER_TEXT_SIZE              128
/* USER's internal CreateDialog/DialogBox call: its argument block. */
#define WOWUSER_CD_ARG_MODAL           0     /* 0 = CreateDialog, 1 = DialogBox    */
#define WOWUSER_CD_ARG_LENGTH          2     /* the template's length              */
#define WOWUSER_CD_ARG_PARAM           6     /* DialogBoxParam's lParam            */
#define WOWUSER_CD_ARG_PROC            10
#define WOWUSER_CD_ARG_PARENT          14
#define WOWUSER_CD_ARG_TEMPLATE        16
#define WOWUSER_CD_ARG_INSTANCE        20
/* CREATESTRUCT (Win16), as WM_CREATE carries it -- see WowUserWantCreate. */
#define WOWUSER_CS16_INSTANCE          4
#define WOWUSER_CS16_MENU              6
#define WOWUSER_CS16_PARENT            8
#define WOWUSER_CS16_HEIGHT            10
#define WOWUSER_CS16_WIDTH             12
#define WOWUSER_CS16_Y                 14
#define WOWUSER_CS16_X                 16
#define WOWUSER_CS16_STYLE             18
#define WOWUSER_CS16_NAME              22    /* lpszName: a far pointer, fixed up  */
#define WOWUSER_CS16_CLASS             26    /* lpszClass: likewise                */
#define WOWUSER_CS16_SIZE              34
#define WOWUSER_CS16_FIXUPS            2
/* GetWindowWord/SetWindowWord's negative indexes (Win16). */
#define WOWUSER_GWW16_HINSTANCE        (-6)
#define WOWUSER_GWW16_HWNDPARENT       (-8)
#define WOWUSER_GWW16_ID               (-12)
/* An icon/cursor resource directory: idReserved, idType, idCount, then entries. */
#define WOWUSER_ICONDIR_COUNT          4
#define WOWUSER_ICONDIR_HEADER_SIZE    6
#define WOWUSER_ICONDIR_ENTRY_SIZE     14
#define WOWUSER_ICONDIR_MAX_ENTRIES    64
#define WOWUSER_NOTIFY_UI_BITS_MAX     0x100   /* the forward bitmap's largest size */
#define WOWUSER_NOTIFY_FOUND           0x00010000u   /* DX non-zero: found          */
#define WOWUSER_TDB_HINSTANCE          0x1C  /* TDB: the task's instance           */
#define WOWUSER_KEY_DOWN               0x8000  /* GetKeyState: the key is down     */
#define WOWUSER_COMMAND_FROM_ACCELERATOR 0x00010000u   /* WM_COMMAND: HIWORD 1     */
/* DIB headers, as a resource holds them. */
#define WOWUSER_BITMAPCOREHEADER_SIZE  12
#define WOWUSER_BCH_WIDTH              4
#define WOWUSER_BCH_HEIGHT             6
#define WOWUSER_BCH_BITCOUNT           10
#define WOWUSER_BIH_WIDTH              4
#define WOWUSER_BIH_HEIGHT             8
#define WOWUSER_BIH_BITCOUNT           14
#define WOWUSER_BIH_SIZEIMAGE          20
#define WOWUSER_BIH_CLRUSED            32
#define WOWUSER_RGBQUAD_SIZE           4
#define WOWUSER_RGBTRIPLE_SIZE         3
#define WOWUSER_MAX_PALETTE            256
#define WOWUSER_MAX_PALETTE_BITS       8
#define WOWUSER_BITMAP_MAX_SIZE        0x10000
#define WOWUSER_STRING_SIZE            256
#define WOWUSER_LONG_TEXT_SIZE         512
#define WOWUSER_MAX_TEXT_TABS          64    /* TabbedTextOut's tab stops           */
#define WOWUSER_KEY_STATE_SIZE         256   /* Get/SetKeyboardState's table        */
#define WOWUSER_WNDCLASS16_WORDS       13    /* GetClassInfo's WNDCLASS, in words   */
#define WOWUSER_GLOBALALLOC_ARGUMENTS  3     /* flags, cb high, cb low              */
#define WOWUSER_ATOM_NAME_LENGTH       5     /* "#xxxx": an integer atom's name     */
#define WOWUSER_HEX_LETTER_VALUE       10    /* 'a' in a hex digit                  */
#define WOWUSER_UWH_ARG_PROC           0     /* UnhookWindowsHook(nCode, lpfn)      */
#define WOWUSER_UWH_ARG_ID             4
#define WOWUSER_ESB_ARG_ARROWS         0     /* EnableScrollBar(hwnd, bar, arrows)  */
#define WOWUSER_ESB_ARG_BAR            2
#define WOWUSER_ESB_ARG_HWND           4
/* RECT16 and POINT16 fields, as offsets. */
#define WOWUSER_RECT16_TOP             2
#define WOWUSER_RECT16_RIGHT           4
#define WOWUSER_RECT16_BOTTOM          6
/* WINDOWPLACEMENT (Win16). */
#define WOWUSER_WP16_LENGTH            0
#define WOWUSER_WP16_FLAGS             2
#define WOWUSER_WP16_SHOWCMD           4
#define WOWUSER_WP16_MIN_X             6
#define WOWUSER_WP16_MIN_Y             8
#define WOWUSER_WP16_MAX_X             10
#define WOWUSER_WP16_MAX_Y             12
#define WOWUSER_WP16_NORMAL_LEFT       14
#define WOWUSER_WP16_NORMAL_TOP        16
#define WOWUSER_WP16_NORMAL_RIGHT      18
#define WOWUSER_WP16_NORMAL_BOTTOM     20
#define WOWUSER_WP16_SIZE              22
/* The comm services: what wowcomm_* and the Win16 answers carry. */
#define WOWUSER_COMM_BUFFER_SIZE       512
#define WOWUSER_COMM_ALREADY_OPEN      (-5)  /* WowCommOpen: the port is open      */
#define WOWUSER_COMM_FAILED            (-2)  /* the answer these arms give on error */
#define WOWUSER_COMSTAT16_INQUEUE      1     /* COMSTAT (Win16): cbInQue            */
#define WOWUSER_COMSTAT16_OUTQUEUE     3     /* ...cbOutQue                         */
#define WOWUSER_MINUS_ONE32            0xFFFFFFFF
#define WOWUSER_DWORD_BYTES            4
#define WOWUSER_CAPTION_SIZE           96
#define WOWUSER_MAX_WINDOW_WALK        4096  /* GetWindow: windows stepped past      */
#define WOWUSER_ATOM_TOP_DIGIT_SHIFT   12    /* an atom's first hex digit          */
#define WOWUSER_SCP_ARG_Y              0     /* SetCursorPos/SetCaretPos(x, y)       */
#define WOWUSER_SCP_ARG_X              2
/* USER ids krnl386 calls from its own code (see the arms). */
#define WOWUSER_SIGNALPROC             0x013a
#define WOWUSER_SIGNALPROC_ARG_CODE    6
#define WOWUSER_FINALUSERINIT          0x0190
#define WOWUSER_SYSERRORBOX            0x0140
#define WOWUSER_SEB_ARG_BUTTON3        0     /* SysErrorBox(text, caption, b1, b2, b3) */
#define WOWUSER_SEB_ARG_BUTTON2        2
#define WOWUSER_SEB_ARG_BUTTON1        4
#define WOWUSER_SEB_ARG_CAPTION        6
#define WOWUSER_SEB_ARG_TEXT           10
#define WOWUSER_SEB_BUTTONS            3
#define WOWUSER_SEB_BUTTON_MASK        0x7FFF  /* the button, without SEB_DEFBUTTON  */

static INT WowUserCall(PWOW32_FRAME frame, PSTR note, INT noteCapacity)
{
    g_WowUserCurrentFrame = frame;      /* s91: for WowUserDef32's 16:16 reads */
    if (noteCapacity) note[0] = 0;
    WowUserEnsureSystemClasses();
    switch (frame->Id) {

    /* ── ★★★ 0x39 RegisterClass(const WNDCLASS FAR*) ──────────────────────────
         Named by USER's own export table: ordinal 57 `REGISTERCLASS` arrives as
         id 0x39 with 4 argument bytes -- one far pointer. As documented, 0 is
         failure and any non-zero value is the class ATOM.
       ★ REGISTERING IS REAL WORK, NOT A STUB. The window procedure, the class
         styles and the extra-bytes counts are what CreateWindow and DefWindowProc
         will need, and they are only available here -- the guest hands them over
         once and then refers to the class by name. So keep them.
       ⚠ THE ATOM MUST BE STABLE AND NON-ZERO. Win16 atoms live at 0xC000 and up;
         a second RegisterClass of the same name returns the SAME atom rather than
         allocating another, because a program that re-registers (WOWEXEC does, once
         per relaunch) must not exhaust the table. */
    case WOWUSER_REGISTERCLASS: {
        volatile BYTE *wndClass = Wow32ArgPointer(frame, 0);
        CHAR className[WOWUSER_NAME_SIZE];
        PWOWUSER_CLASS windowClass;
        INT count;
        if (!wndClass) { Wow32SetReturn(frame, 0); return 1; }  /* an unreadable WNDCLASS fails */
        WowUserFarString(frame, (DWORD)WowUserPeek(wndClass, WOWUSER_WNDCLASS16_CLASSNAME)
                          | ((DWORD)WowUserPeek(wndClass, WOWUSER_WNDCLASS16_CLASSNAME + WOW_WORD_BYTES) << WORD_SHIFT),
                       className, sizeof className);
        if (!className[0]) { Wow32SetReturn(frame, 0); return 1; } /* no name, no class */
        windowClass = WowUserFindClass(className);
        /* ⚠ A SYSTEM CLASS MAY NOT BE OVERWRITTEN. Re-registering an app's own
             class is allowed above (WOWEXEC does it once per relaunch), but the
             same code path would let a program point MDICLIENT at its own
             procedure and quietly take the system's class away from every other
             window made from it. Real Windows fails the call; so do we. */
        if (windowClass && windowClass->IsSystemClass) {
            INT menuNoteLength = 0;
            WowNotePut(note, noteCapacity, &menuNoteLength, "RegisterClass REFUSED (system class) ");
            WowNoteQuoted(note, noteCapacity, &menuNoteLength, className);
            Wow32SetReturn(frame, 0);
            return 1;
        }
        if (!windowClass) {
            if (g_WowUserClassCount >= WOWUSER_MAX_CLASS) { Wow32SetReturn(frame, 0); return 1; }
            windowClass = &g_WowUserClasses[g_WowUserClassCount++];
            for (count = 0; count < (INT)sizeof windowClass->Name; ++count) windowClass->Name[count] = className[count];
            windowClass->Atom = (WORD)(MAXINTATOM + g_WowUserClassCount);
        }
        windowClass->Style    = WowUserPeek(wndClass, WOWUSER_WNDCLASS16_STYLE);
        windowClass->WindowProcedure  = (DWORD)WowUserPeek(wndClass, WOWUSER_WNDCLASS16_WNDPROC)
                    | ((DWORD)WowUserPeek(wndClass, WOWUSER_WNDCLASS16_WNDPROC + WOW_WORD_BYTES) << WORD_SHIFT);
        windowClass->ClassExtra = WowUserPeek(wndClass, WOWUSER_WNDCLASS16_CLSEXTRA);
        windowClass->WindowExtra = WowUserPeek(wndClass, WOWUSER_WNDCLASS16_WNDEXTRA);
        windowClass->Instance    = WowUserPeek(wndClass, WOWUSER_WNDCLASS16_HINSTANCE);
        windowClass->Icon16    = WowUserPeek(wndClass, WOWUSER_WNDCLASS16_HICON);
        windowClass->Cursor16  = WowUserPeek(wndClass, WOWUSER_WNDCLASS16_HCURSOR);
        windowClass->Background16  = WowUserPeek(wndClass, WOWUSER_WNDCLASS16_HBRBACKGROUND);
        /* ★ THE MENU THE CLASS NAMES. A Win16 program does not have to call
             LoadMenu -- it can put the resource's NAME in the WNDCLASS and let
             CreateWindow attach it, which is what Notepad appears to do (it calls
             LoadMenu never, and it certainly has a File menu). Read and LOGGED
             before it is used, because "the class named a menu and we ignored it"
             and "the class named no menu" are different facts and the window looks
             identical either way. */
        windowClass->MenuName[0] = 0;
        {   DWORD menuName = (DWORD)WowUserPeek(wndClass, WOWUSER_WNDCLASS16_MENUNAME)
                     | ((DWORD)WowUserPeek(wndClass, WOWUSER_WNDCLASS16_MENUNAME + WOW_WORD_BYTES) << WORD_SHIFT);
            windowClass->MenuOrdinal = 0;
            if ((WORD)(menuName >> WORD_SHIFT) == 0) windowClass->MenuOrdinal = (WORD)menuName;  /* MAKEINTRESOURCE */
            else WowUserFarString(frame, menuName, windowClass->MenuName, sizeof windowClass->MenuName);
        }
        /* ★★★★★ AND REGISTER A REAL Win32 CLASS BEHIND IT. (session 42) A Win16
             window is a real window on the real desktop, so its class has to be a
             real class -- and it is OUR window procedure that goes in it, because
             the guest's is 16-bit and can only be entered through wowcall.h. */
        {   /* ★ THE TOKEN BECOMES A REAL OBJECT HERE, because this is where the
                 guest says which field it belongs in. A value that is not one of
                 our tokens yields 0 and the OS default is used -- an app's OWN
                 icon is a module resource (kind 3) and is not built yet. */
            WORD cursorOrdinal = WowUserSystemResourceOrdinal(windowClass->Cursor16);
            WORD iconOrdinal = WowUserSystemResourceOrdinal(windowClass->Icon16);
            WORD iconKind = WowUserSystemResourceKind(windowClass->Icon16);
            INT  fell   = 0, iconBits = 0;
            HICON   icon = WowUserSystemResourceIcon(windowClass->Icon16, &iconBits, 0, 0);
            /* ★ AND AN EXPLICIT SMALL ONE -- see the note in wowres.h. Built
                 from the same group at 16x16 rather than left to be derived,
                 because the derived one measured monochrome against stock. */
            HICON   smallIcon  = WowUserSystemResourceIcon(windowClass->Icon16, NULL,
                                                GetSystemMetrics(SM_CXSMICON),
                                                GetSystemMetrics(SM_CYSMICON));
            HCURSOR cursor = WowUserSystemResourceCursor(windowClass->Cursor16, NULL);
            /* ── ★★★★ THE CLASS'S BACKGROUND BRUSH -- TWO FORMS, AND BOTH ARE
                 REAL. Win16's `hbrBackground` is EITHER a real HBRUSH the program
                 made, OR a COLOR_* system index BIASED BY ONE (so that 0 can mean
                 "no background"). The two are told apart the way Windows tells
                 them apart: a value that is not one of our GDI tokens and is
                 small enough to be a colour index is one.
               ⚠ 0 MEANS "NO BACKGROUND ERASE", and it must stay 0 rather than
                 become a default -- a program that says it paints its own
                 background is telling us not to paint over it.
               ★ Solitaire creates a green brush and names it here; it used to be
                 read and thrown away, so its table was erased WHITE. */
            HBRUSH  classBrush = NULL;
            switch (WowConvBackgroundBrushKind(windowClass->Background16)) {  /* ★ tested in wow_test.c */
            case WOWCONV_HBR_NONE:                        /* 0 = no erase; keep 0 */
                break;
            case WOWCONV_HBR_SYSCOLOR:
                classBrush = (HBRUSH)(ULONG_PTR)windowClass->Background16;  /* COLOR_* + 1 */
                break;
            default: {
                INT backgroundKind = -1;
                HGDIOBJ background = WowGdiH32(windowClass->Background16, &backgroundKind);
                if (background && backgroundKind == WOWGDI_KIND_OBJ) classBrush = (HBRUSH)background;
                break; }
            }
            if (!windowClass->IsRegistered32)
                windowClass->IsRegistered32 = WowWinRegister(windowClass->Name, windowClass->Class32, sizeof windowClass->Class32,
                                           cursor, icon, smallIcon, &fell, classBrush);
            windowClass->CursorOrdinal = cursorOrdinal; windowClass->IconOrdinal = iconOrdinal; windowClass->IsCursorUnknown = fell;
            windowClass->IconBits = iconBits; windowClass->IconKind = iconKind;
        }
        /* The note is the whole point of servicing this: it is the first time this
           project can say WHAT a Win16 program is trying to put on the screen. */
        {   INT noteLength = 0;
            WowNotePut(note, noteCapacity, &noteLength, "RegisterClass ");
            WowNoteQuoted(note, noteCapacity, &noteLength, className);
            WowNotePut(note, noteCapacity, &noteLength, windowClass->IsRegistered32 ? " -> Win32 class " : " -- ★ Win32 "
                                                  "RegisterClass FAILED for ");
            WowNotePut(note, noteCapacity, &noteLength, windowClass->Class32);
            if (windowClass->CursorOrdinal) { WowNotePut(note, noteCapacity, &noteLength, " cursor=0x");
                             WowNoteHex(note, noteCapacity, &noteLength, windowClass->CursorOrdinal, WOW_HEX_WORD_DIGITS); }
            else if (WowUserSystemResourceName(windowClass->Cursor16)) {
                WowNotePut(note, noteCapacity, &noteLength, " cursor=");
                WowNoteQuoted(note, noteCapacity, &noteLength, WowUserSystemResourceName(windowClass->Cursor16));
            }
            if (windowClass->MenuName[0]) { WowNotePut(note, noteCapacity, &noteLength, " MENU=");
                                  WowNoteQuoted(note, noteCapacity, &noteLength, windowClass->MenuName); }
            else if (windowClass->MenuOrdinal) { WowNotePut(note, noteCapacity, &noteLength, " MENU=#");
                                   WowNoteHex(note, noteCapacity, &noteLength, windowClass->MenuOrdinal, WOW_HEX_WORD_DIGITS); }
            else WowNotePut(note, noteCapacity, &noteLength, " (no menu named)");
            if (windowClass->IconOrdinal || WowUserSystemResourceName(windowClass->Icon16)) {
                if (windowClass->IconOrdinal) {
                    WowNotePut(note, noteCapacity, &noteLength, " icon=0x");
                    WowNoteHex(note, noteCapacity, &noteLength, windowClass->IconOrdinal, WOW_HEX_WORD_DIGITS);
                } else {
                    WowNotePut(note, noteCapacity, &noteLength, " icon=");
                    WowNoteQuoted(note, noteCapacity, &noteLength, WowUserSystemResourceName(windowClass->Icon16));
                }
                if (windowClass->IconKind == WOWUSER_AD_KIND_MODULERES) {
                    WowNotePut(note, noteCapacity, &noteLength, windowClass->IconBits ? " (the app's own, "
                                                            : " (the app's own -- "
                                                              "★ NOT BUILT");
                    if (windowClass->IconBits) { WowNoteHex(note, noteCapacity, &noteLength,
                                                (DWORD)windowClass->IconBits, WOW_HEX_BYTE_DIGITS);
                                      WowNotePut(note, noteCapacity, &noteLength, " bpp)"); }
                    else WowNotePut(note, noteCapacity, &noteLength, ")");
                }
            }
            /* s92 (#289): the brush as the guest named it -- 0, COLOR_*+1, or a token. */
            WowNotePut(note, noteCapacity, &noteLength, " hbr=0x");
            WowNoteHex(note, noteCapacity, &noteLength, windowClass->Background16, WOW_HEX_WORD_DIGITS);
            if (windowClass->IsCursorUnknown)
                WowNotePut(note, noteCapacity, &noteLength, " -- ★ NO CURSOR WAS BUILT (an ordinal"
                                           " the OS does not know, or a named"
                                           " resource not in this module); fell"
                                           " back to IDC_ARROW");
        }
        Wow32SetReturn(frame, windowClass->Atom);
        return 1;
    }

    /* ── ★★★ 0x29 CreateWindow(...) -- 30 argument bytes ──────────────────────
         `USER.41 CREATEWINDOW`, named by WOWEXEC's NE import relocations rather
         than inferred: it arrives as id 0x29 with `0x1e` argument bytes, and
         Win16's eleven parameters come to exactly 30. The block layout is
         derived and cross-validated in the comment on CW_ARG_* above.
       ★ AN UNREGISTERED CLASS MUST FAIL. Real Windows returns NULL, and a host
         that made a window for any name at all would hide a broken RegisterClass
         behind a working CreateWindow -- the "runs but lies" class.
       ⚠ As documented, 0 is the failure the guest expects and any non-zero value
         is taken as the window handle. */
    case WOWUSER_CREATEWINDOW:
    case WOWUSER_CREATEWINDOWEX: {
        DWORD classPointer = Wow32ArgDword(frame, WOWUSER_CW_ARG_CLASSNAME);
        /* The ONLY difference between the two calls -- see the note by the ids. */
        DWORD exStyle = (frame->Id == WOWUSER_CREATEWINDOWEX)
                      ? Wow32ArgDword(frame, WOWUSER_CWX_ARG_EXSTYLE) : 0;
        CHAR  className[WOWUSER_NAME_SIZE], windowName[WOWUSER_NAME_SIZE];
        PWOWUSER_CLASS windowClass;
        PWOWUSER_WINDOW window;
        INT index, noteLength = 0;

        className[0] = 0;
        if ((WORD)(classPointer >> WORD_SHIFT) == 0)             /* an ATOM, not a string */
            windowClass = WowUserFindClassByAtom((WORD)classPointer);
        else {
            WowUserFarString(frame, classPointer, className, sizeof className);
            windowClass = className[0] ? WowUserFindClass(className) : NULL;
        }
        /* ⚠ SAY WHAT WAS ASKED FOR. This read "CreateWindow: no such class" and
             nothing else, and the one run where it mattered -- SYSEDIT's frame
             procedure asking for MDICLIENT -- was therefore a line that named the
             class of failure and withheld the instance, which is the exact shape
             this project has been caught by before. An atom prints as an atom,
             because a lookup that failed on an atom did not have a name to fail on. */
        if (!windowClass) {
            Wow32SetReturn(frame, 0);
            WowNotePut(note, noteCapacity, &noteLength, "CreateWindow: no such class ");
            if (className[0]) WowNoteQuoted(note, noteCapacity, &noteLength, className);
            else { WowNotePut(note, noteCapacity, &noteLength, "atom 0x");
                   WowNoteHex(note, noteCapacity, &noteLength, classPointer & WORD_MASK, WOW_HEX_WORD_DIGITS); }
            return 1;
        }

        window = WowUserNewWindow();
        if (!window) { Wow32SetReturn(frame, 0); return 1; }
        window->Class     = (WORD)(windowClass - g_WowUserClasses);
        window->Style   = Wow32ArgDword(frame, WOWUSER_CW_ARG_STYLE);
        window->WindowProcedure = windowClass->WindowProcedure;
        window->Parent  = Wow32ArgWord(frame, WOWUSER_CW_ARG_HWNDPARENT);
        window->Menu    = Wow32ArgWord(frame, WOWUSER_CW_ARG_HMENU);
        window->Instance   = Wow32ArgWord(frame, WOWUSER_CW_ARG_HINSTANCE);
        {   WORD positionX  = Wow32ArgWord(frame, WOWUSER_CW_ARG_X),  positionY  = Wow32ArgWord(frame, WOWUSER_CW_ARG_Y);
            WORD width = Wow32ArgWord(frame, WOWUSER_CW_ARG_WIDTH), height = Wow32ArgWord(frame, WOWUSER_CW_ARG_HEIGHT);
            /* CW_USEDEFAULT is only meaningful before there is a rectangle; every
               later reader wants numbers, so resolve it once, here. */
            window->PositionX  = (positionX  == CW_USEDEFAULT16) ? 0 : (INT)(SHORT)positionX;
            window->PositionY  = (positionY  == CW_USEDEFAULT16) ? 0 : (INT)(SHORT)positionY;
            window->Width = (width == CW_USEDEFAULT16) ? WOWUSER_DESK_CX : (INT)(SHORT)width;
            window->Height = (height == CW_USEDEFAULT16) ? WOWUSER_DESK_CY : (INT)(SHORT)height;
        }
        window->Text[0] = 0;
        if (WowUserFarString(frame, Wow32ArgDword(frame, WOWUSER_CW_ARG_WINDOWNAME), windowName, sizeof windowName))
            for (index = 0; index < (INT)sizeof window->Text; ++index) window->Text[index] = windowName[index];

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
        {   PCWOWUSER_CLASS childClass = &g_WowUserClasses[window->Class];
            HWND parent32 = window->Parent ? WowUserHwnd32(window->Parent) : NULL;
            CLIENTCREATESTRUCT clientCreate;
            PVOID createParam = NULL;
            HMENU menu = NULL;
            if (childClass->IsSystemClass && childClass->Name[0] == 'M') {  /* MDICLIENT */
                clientCreate.hWindowMenu  = NULL;
                clientCreate.idFirstChild = WOWUSER_MDI_FIRSTCHILD;
                createParam = &clientCreate;
            }
            if ((window->Style & WS_CHILD16) && window->Menu)
                menu = (HMENU)(ULONG_PTR)window->Menu;
            /* s91: A TOP-LEVEL WINDOW'S OWN hMenu ARGUMENT -- a LoadMenu token -- and
                 it beats the class's menu, as in Windows. RECORDER passes LoadMenu(#2)
                 here and registers its class with no menu: its window came up with no
                 menu bar at all (stock: File/Macro/Options/Help). */
            else if (!(window->Style & WS_CHILD16) && window->Menu && WowUserMenu32(window->Menu))
                menu = WowUserMenu32(window->Menu);
            /* ── ★★★ THE CLASS'S OWN MENU, BUILT FROM THE GUEST'S RESOURCE.
                 A Win16 program does not have to call LoadMenu: it can name the
                 resource in its WNDCLASS and let CreateWindow attach it, which is
                 exactly what NOTEPAD does (`MENU=#0001`, and it calls LoadMenu
                 never). The bytes are in its own file, so the host reads them --
                 see wowres.h, whose decoding was confirmed against the data before
                 any of this existed.
               ⚠ A CHILD WINDOW HAS NO MENU: its hMenu slot is a control id, which
                 is why this is inside the else. */
            /* ⚠ AND THE MENU CAN BE NAMED RATHER THAN NUMBERED. Notepad's is
                 `#0001`, so the integer path alone was enough to give it a menu
                 bar and this gap went unnoticed for two sessions. MS PAINT
                 registers `pbParent` with `MENU="PBrush2"`, and its window came
                 up with no menu at all -- silently, because a class that names a
                 menu we cannot find is indistinguishable from a class with no
                 menu. Both forms end in the same builder; see wowres.h. */
            else if (!(window->Style & WS_CHILD16)
                     && (childClass->MenuOrdinal || childClass->MenuName[0])) {
                INT menuItems = 0;
                if (WowResOpen(WowUserResourceProgram()))
                    menu = childClass->MenuOrdinal ? WowResMenu(childClass->MenuOrdinal, &menuItems)
                                     : WowResMenuByName(childClass->MenuName, &menuItems);
                window->MenuItems = menuItems;
            }
            if (childClass->IsRegistered32) {
                window->Window32 = CreateWindowExA(exStyle, childClass->Class32, window->Text, window->Style,
                                            WowWinCoordinate(Wow32ArgWord(frame, WOWUSER_CW_ARG_X)),
                                            WowWinCoordinate(Wow32ArgWord(frame, WOWUSER_CW_ARG_Y)),
                                            WowWinCoordinate(Wow32ArgWord(frame, WOWUSER_CW_ARG_WIDTH)),
                                            WowWinCoordinate(Wow32ArgWord(frame, WOWUSER_CW_ARG_HEIGHT)),
                                            parent32, menu, GetModuleHandleA(NULL),
                                            createParam);
                if (window->Window32) { ++g_WowWinCreated;
                                 if (!g_WowWinThread) g_WowWinThread = GetCurrentThreadId(); }
            }
        }

        /* Name which of the two it was: they share a body, and a log that
           called both "CreateWindow" would hide an exstyle we never applied. */
        WowNotePut(note, noteCapacity, &noteLength, exStyle ? "CreateWindowEx "
                : (frame->Id == WOWUSER_CREATEWINDOWEX ? "CreateWindowEx(ex=0) "
                                                   : "CreateWindow "));
        if (exStyle) { WowNotePut(note, noteCapacity, &noteLength, "ex=0x");
                       WowNoteHex(note, noteCapacity, &noteLength, exStyle, WOW_HEX_DWORD_DIGITS);
                       WowNotePut(note, noteCapacity, &noteLength, " "); }
        WowNoteQuoted(note, noteCapacity, &noteLength, windowClass->Name);
        WowNotePut(note, noteCapacity, &noteLength, " ");
        WowNoteQuoted(note, noteCapacity, &noteLength, window->Text);
        WowNotePut(note, noteCapacity, &noteLength, " style=0x");
        WowNoteHex(note, noteCapacity, &noteLength, window->Style, WOW_HEX_DWORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, " -> hwnd=0x");
        WowNoteHex(note, noteCapacity, &noteLength, window->Window16, WOW_HEX_WORD_DIGITS);
        /* A window made from a system class has no 16-bit procedure behind it, so
           it gets no WM_CREATE. Say which kind of window this is on the line that
           creates it, or the ABSENCE of the callback below reads like a defect. */
        if (windowClass->IsSystemClass) WowNotePut(note, noteCapacity, &noteLength, " [system class: no wndproc]");
        /* ★ SAY WHETHER IT IS REALLY THERE. A Win16 handle that answers questions
             about itself and a WINDOW ON THE DESKTOP are different achievements,
             and only one of them is visible -- so the line has to distinguish
             them, or a failed CreateWindowEx reads as a success. */
        if (window->Window32) {
            WowNotePut(note, noteCapacity, &noteLength, " HWND=0x");
            WowNoteHex(note, noteCapacity, &noteLength, (DWORD)(ULONG_PTR)window->Window32, WOW_HEX_DWORD_DIGITS);
            if (window->MenuItems) { WowNotePut(note, noteCapacity, &noteLength, " MENU=");
                                WowNoteHex(note, noteCapacity, &noteLength, (DWORD)window->MenuItems, WOW_HEX_BYTE_DIGITS);
                                WowNotePut(note, noteCapacity, &noteLength, " items from the guest's"
                                                           " own resource"); }
            else if (g_WowUserClasses[window->Class].MenuOrdinal)
                WowNotePut(note, noteCapacity, &noteLength, " -- \u2605 ITS CLASS NAMED A MENU AND"
                                           " NONE WAS BUILT");
        } else {
            WowNotePut(note, noteCapacity, &noteLength, " -- ★ NO REAL WINDOW (Win32 gle=0x");
            WowNoteHex(note, noteCapacity, &noteLength, GetLastError(), WOW_HEX_DWORD_DIGITS);
            WowNotePut(note, noteCapacity, &noteLength, ")");
        }
        Wow32SetReturn(frame, window->Window16);

        /* ── ★★★★★ AND NOW SEND IT WM_CREATE. (GH #128, session 40) ───────────
             This is the whole point of the window, and until this session it was
             the one thing this host had never done in either direction. SYSEDIT's
             frame procedure creates its MDI client while handling WM_CREATE and
             keeps the handle; without it, it decides it has no usable window
             (observed). So a CreateWindow that does not send WM_CREATE is not a window that
             is missing a message -- it is a window the application will correctly
             refuse to use.
           ★ THE PROCEDURE IS THE WINDOW'S, NOT THE CLASS'S. Win16 copies
             lpfnWndProc into the window at creation, which is why w->wndproc
             exists; sending to the class would be wrong the moment anything
             subclasses.
           ★ AND DS IS THE WINDOW'S hInstance. sysedit.exe is MULTIPLEDATA, so its
             exported procedures take DS from their caller (the standard Win16
             exported-function convention) -- see the contract in wowcall.h. `hinst` is the same
             word the program put in WNDCLASS.hInstance and passed to
             CreateWindow, so it is the guest's own statement about its data
             segment rather than ours.
           ⚠ lParam SHOULD BE AN LPCREATESTRUCT and is 0 -- a gap this host names
             rather than fakes. See wowcall.h. */
        WowUserWantCreate(frame, windowClass, window);
        return 1;
    }

    /* ── ★★★★★ 0xEF CreateDialog -- BUILD THE DIALOG AND ALL OF ITS CONTROLS.
         (session 55) ────────────────────────────────────────────────────────────
         THE ARGUMENTS ARE MEASURED, not taken from a header, because there is no
         header: this is an internal entry point. One CALC.EXE run, 22 argument
         bytes, and the WOWBOP line above the call settles the important one --
         `ax=cx=dx=0x0b47`, the selector USER had just got back from
         LockResource, and 0x0b47 is exactly what sits at +18.

             +20  hInstance        0x0b6e
             +16  template FAR*    0x0b47:0000   (high word = selector, as
                                                  everywhere else in this file)
             +14  hWndParent       0x0000        (CALC's dialog IS its main window)
             +10  dialog procedure 0x00000000
             +6   init parameter   0x00000000
             +0..+5                UNEXPLAINED -- logged, not guessed. See below.

       ★ THE DIALOG PROCEDURE IS NULL AND THAT IS NOT A BUG. USER keeps it: the
         16-bit side runs the modal loop and calls the procedure itself, so what
         it wants from us is the WINDOW, not the dispatching. Inventing a
         procedure here would give the dialog two.

       ★ WHY THIS IS 15 WINDOWS AND NOT ONE. A dialog template is a window plus
         one child per item, and every one of those children has to be a real
         control -- which is why the failure looked the way it did. With 0xEF
         stepped over, CALC's log read `GetDlgItem` x15 and `ShowWindow 0x0000`
         x15: the program walking the 15 controls its own template declares and
         being handed window 0 for each. Fifteen is the item count in `SciCalc`.

       ⚠ DIALOG UNITS ARE NOT PIXELS, and the conversion is the OS's own:
         x*baseX/4 and y*baseY/8. Passing the template's numbers through as
         pixels yields a dialog about a third of the right size with its controls
         piled in the top-left -- which reads as a layout bug in the guest.
       ⚠ AND THE TEMPLATE'S RECTANGLE IS THE CLIENT AREA. AdjustWindowRect adds
         the caption and border, or the dialog comes up short by exactly the
         chrome and the bottom row of controls falls outside it. This project has
         already paid for that shape once, in the status-bar height (session 54). */
    case WOWUSER_CREATEDIALOG: {
        /* ★ ARG 0 IS THE MODAL FLAG -- 0 = CreateDialog, 1 = DialogBox. Pinned
             from USER.EXE's own two call sites; see the long note below. */
        WORD  isModal   = Wow32ArgWord(frame, WOWUSER_CD_ARG_MODAL);
        DWORD templatePointer     = Wow32ArgDword(frame, WOWUSER_CD_ARG_TEMPLATE);
        WORD  instance   = Wow32ArgWord(frame, WOWUSER_CD_ARG_INSTANCE);
        WORD  parent16  = Wow32ArgWord(frame, WOWUSER_CD_ARG_PARENT);
        DWORD dialogProcedure = Wow32ArgDword(frame, WOWUSER_CD_ARG_PROC);
        const volatile BYTE *templateBytes = WowUserFarMemory(frame, templatePointer);
        CHAR  className[WOWUSER_NAME_SIZE], caption[WOWUSER_NAME_SIZE], menuName[WOWUSER_NAME_SIZE];
        WORD  menuOrdinal = 0, classOrdinal = 0;
        DWORD style, controlStyle;
        INT   isShowDeferred = 0;
        INT   count, positionX, positionY, width, height, pointer, index, noteLength = 0, made = 0;
        DWORD baseUnits;
        INT   baseUnitX, baseUnitY, usesDefault = 0;
        CHAR  fontFace[WOWUSER_NAME_SIZE] = ""; INT fontPoints = 0; HFONT font = NULL;
        WORD  firstFocus = 0;          /* WM_INITDIALOG's wParam (#162) */
        PWOWUSER_CLASS windowClass;
        PWOWUSER_WINDOW window;
        HWND parent32;

        if (!templateBytes) {
            WowNotePut(note, noteCapacity, &noteLength, "CreateDialog: template far pointer 0x");
            WowNoteHex(note, noteCapacity, &noteLength, templatePointer, WOW_HEX_DWORD_DIGITS);
            WowNotePut(note, noteCapacity, &noteLength, " does not resolve");
            Wow32SetReturn(frame, 0);
            return 1;
        }

        style = WowDlgTemplateDword(templateBytes, 0);
        controlStyle = style;           /* what we actually CREATE it with */
        count = templateBytes[WOWDLG_TEMPLATE_COUNT];
        positionX  = (INT)(SHORT)WowDlgTemplateWord(templateBytes, WOWDLG_TEMPLATE_X);
        positionY  = (INT)(SHORT)WowDlgTemplateWord(templateBytes, WOWDLG_TEMPLATE_Y);
        width = (INT)(SHORT)WowDlgTemplateWord(templateBytes, WOWDLG_TEMPLATE_WIDTH);
        height = (INT)(SHORT)WowDlgTemplateWord(templateBytes, WOWDLG_TEMPLATE_HEIGHT);
        pointer  = WOWDLG_TEMPLATE_MENU;
        pointer += WowDlgTemplateNameOrdinal(templateBytes, pointer, menuName, sizeof menuName, &menuOrdinal);
        pointer += WowDlgTemplateNameOrdinal(templateBytes, pointer, className,    sizeof className,    &classOrdinal);
        pointer += WowDlgTemplateString(templateBytes, pointer, caption, sizeof caption);
        if (style & WOWDLG_SETFONT) {
            fontPoints = (INT)(SHORT)WowDlgTemplateWord(templateBytes, pointer);
            pointer += WOW_WORD_BYTES;                /* WORD point size */
            pointer += WowDlgTemplateString(templateBytes, pointer, fontFace, sizeof fontFace);
        }

        /* ── THE DIALOG'S OWN CLASS. A template may name one, and when it does
             it is the APPLICATION's -- CALC's is `SciCalc`, already registered,
             with its own window procedure, its own menu and its own icon. That
             is the class the window must be made from, or the program gets a
             dialog it cannot drive. A template that names no class gets the
             OS's standard dialog class. */
        windowClass = className[0] ? WowUserFindClass(className) : NULL;
        if (!windowClass) windowClass = WowUserFindClass("#32770");
        if (!windowClass) { Wow32SetReturn(frame, 0); return 1; }

        window = WowUserNewWindow();
        if (!window) {
            WowNotePut(note, noteCapacity, &noteLength, "CreateDialog: OUT OF WINDOW SLOTS");
            Wow32SetReturn(frame, 0);
            return 1;
        }
        window->Class     = (WORD)(windowClass - g_WowUserClasses);
        window->Style   = style;
        window->WindowProcedure = windowClass->WindowProcedure;
        window->Parent  = parent16;
        window->Instance   = instance;
        window->Menu    = 0;
        for (index = 0; index < (INT)sizeof window->Text; ++index)
            window->Text[index] = (index < (INT)sizeof caption) ? caption[index] : 0;

        baseUnits  = GetDialogBaseUnits();
        baseUnitX = (INT)LOWORD(baseUnits);
        baseUnitY = (INT)HIWORD(baseUnits);
        if (fontPoints > 0 && fontFace[0]) {
            INT fontX = 0, fontY = 0;
            font = WowDlgFont(fontFace, fontPoints, &fontX, &fontY);
            if (font && fontX > 0 && fontY > 0) { baseUnitX = fontX; baseUnitY = fontY; }
        }
        window->DialogBaseUnitX = (WORD)baseUnitX; window->DialogBaseUnitY = (WORD)baseUnitY;
        window->IsDialog3D  = (BYTE)((style & WOWDLG_SETFONT) ? 1 : 0);
        window->Width = MulDiv(width, baseUnitX, WOWDLG_UNITS_PER_BASE_X);
        window->Height = MulDiv(height, baseUnitY, WOWDLG_UNITS_PER_BASE_Y);
        /* ── ⚠ -32768 IN A TEMPLATE'S x IS "YOU PLACE IT", NOT A COORDINATE.
             (session 55) A DLGTEMPLATE says "put this where you like" with
             0x8000 in dtX -- the same CW_USEDEFAULT value CreateWindow uses,
             which is why WowWinCoordinate() already knows it. Scaling it as a
             number instead gives MulDiv(-32768, 8, 4) = -65536, and
             AdjustWindowRect then shifts it by the border to -65539.
           ★ MEASURED, and it is what SOUND RECORDER did: its DIALOG 1 is
             `180x80 at (-32768,0)` and its window came up at x=-65539 --
             a real, correct, fully-built window placed entirely off screen,
             which from the outside looks exactly like "no window". */
        if ((WORD)positionX == CW_USEDEFAULT16) {
            window->PositionX = window->PositionY = 0;  /* our own record; the OS places it */
            usesDefault = 1;
        } else {
            window->PositionX = MulDiv(positionX, baseUnitX, WOWDLG_UNITS_PER_BASE_X);
            window->PositionY = MulDiv(positionY, baseUnitY, WOWDLG_UNITS_PER_BASE_Y);
        }

        parent32 = parent16 ? WowUserHwnd32(parent16) : NULL;
        {   RECT rect;
            HMENU menu = NULL;
            INT menuItems = 0;
            /* The class may name a menu even though the template does not --
               CALC registers `SciCalc` with `MENU="SM"` -- so ask the class
               too, exactly as CreateWindow does. Named OR numbered; both forms
               have been a gap in this host before. */
            if (!(style & WS_CHILD16)) {
                if (menuName[0] || menuOrdinal) {
                    if (WowResOpen(WowUserResourceProgram()))
                        menu = menuOrdinal ? WowResMenu(menuOrdinal, &menuItems)
                                     : WowResMenuByName(menuName, &menuItems);
                } else if (windowClass->MenuOrdinal || windowClass->MenuName[0]) {
                    if (WowResOpen(WowUserResourceProgram()))
                        menu = windowClass->MenuOrdinal ? WowResMenu(windowClass->MenuOrdinal, &menuItems)
                                        : WowResMenuByName(windowClass->MenuName, &menuItems);
                }
            }
            window->MenuItems = menuItems;
            rect.left = window->PositionX; rect.top = window->PositionY;
            rect.right = window->PositionX + window->Width; rect.bottom = window->PositionY + window->Height;
            /* ── s93: A POPUP DIALOG'S POSITION IS RELATIVE TO ITS OWNER'S CLIENT
                 AREA unless the template says DS_ABSALIGN (01h) -- Windows' rule,
                 Win16's and Win32's alike. Taken as screen coordinates, Program
                 Manager's "New Program Object" opened at (32,4) where stock puts it
                 at (193,286), over Program Manager. (TERMINAL looked right only
                 because it re-centres itself in WM_INITDIALOG.) Kept on the screen
                 afterwards, as DialogBox does. */
            INT   isOwnerRelative = 0;      /* s93: placed relative to the owner */
            POINT ownerPoint = { 0, 0 };
            if (!(style & WS_CHILD16) && !(style & DS_ABSALIGN) && parent32 && !usesDefault) {
                POINT origin = { 0, 0 };
                RECT  workArea;
                isOwnerRelative = 1;
                ClientToScreen(parent32, &origin);
                OffsetRect(&rect, origin.x, origin.y);
                if (SystemParametersInfoA(SPI_GETWORKAREA, 0, &workArea, 0)) {
                    INT windowWidth = window->Width, windowHeight = window->Height;
                    if (rect.left + windowWidth > workArea.right)  OffsetRect(&rect, workArea.right - (rect.left + windowWidth), 0);
                    if (rect.top + windowHeight > workArea.bottom)  OffsetRect(&rect, 0, workArea.bottom - (rect.top + windowHeight));
                    if (rect.left < workArea.left) OffsetRect(&rect, workArea.left - rect.left, 0);
                    if (rect.top < workArea.top)   OffsetRect(&rect, 0, workArea.top - rect.top);
                }
                ownerPoint.x = rect.left; ownerPoint.y = rect.top;
            }
            AdjustWindowRect(&rect, style, menu != NULL);
            /* ...and the template's point is the WINDOW's corner, frame included:
               measured, stock puts Program Manager's dialogs exactly the border and
               caption further down-right than the client-rect placement did. */
            if (isOwnerRelative) OffsetRect(&rect, ownerPoint.x - rect.left, ownerPoint.y - rect.top);
            /* ── ★★★★★ A MODAL DIALOG IS CREATED HIDDEN AND SHOWN AFTERWARDS.
                 (session 57) Real USER creates the dialog window, sends
                 WM_INITDIALOG, and only THEN shows it -- and the order is not a
                 detail, it is what makes the dialog's own WM_INITDIALOG work.
                 A dialog procedure's first act is routinely to MOVE itself:
                 TASKMAN centres its Task List with
                     MoveWindow(hDlg, (cxScreen-w)/2, (cyScreen-h)/2, w, h, FALSE)
                 and that FALSE is `bRepaint`, which Win32 documents as "no
                 repainting of ANY kind occurs -- not the client area, not the
                 non-client area, and not the part of the parent uncovered by the
                 move". On a window that is not yet visible that costs nothing,
                 because showing it paints everything. On a window we have ALREADY
                 shown it is a disaster, and this is exactly what it looked like
                 on the rig: the dialog's old pixels LEFT BEHIND at the top-left
                 corner, and at its real position a window with the DESKTOP
                 WALLPAPER showing through its client area -- painted once at an
                 address it no longer occupied.
               ⚠ THE TEMPLATE'S OWN WS_VISIBLE IS WHAT WE ARE DEFERRING, not
                 overriding: `defer_show` remembers that the guest asked for a
                 visible window, and the modal loop shows it the moment
                 WM_INITDIALOG returns. If the loop cannot run at all (callbacks
                 off, stack full) the arm below shows it immediately instead --
                 an unpainted dialog is bad, an INVISIBLE one is worse.
               ⚠ MODELESS IS UNTOUCHED. CreateDialog hands the window back for
                 the caller to show and CALC's SciCalc dialog is measured working
                 that way; changing when a modeless dialog appears would be
                 changing something that is already right. */
            /* s91 (#283): EVERY modal dialog, not only a WS_VISIBLE one. DialogBox
                 displays the dialog after WM_INITDIALOG "regardless of whether the
                 template specifies WS_VISIBLE" (the 3.1 SDK's own words), and
                 TERMINAL's Default Serial Port template (DIALOG 89, style
                 00C800C0) does not set it -- so it was neither deferred nor hidden,
                 appeared at its template position, centred itself with a
                 non-repainting MoveWindow and left its image at the top-left. */
            if (isModal) {
                controlStyle    &= ~(DWORD)WS_VISIBLE;
                isShowDeferred = 1;
            }
            /* ⚠ s89 (#302): AND MODELESS NOW TOO -- the note above predates the
                 nested run. A modeless dialog was shown before its WM_INITDIALOG,
                 so everything the program did there painted at once: Charmap
                 selects a font in its owner-drawn font list FIRST and loads the
                 TrueType mark it draws beside the name AFTERWARDS, so the list was
                 drawn without it, and never again. Real USER (Wine's
                 DIALOG_CreateIndirect agrees) creates the window hidden, sends
                 WM_INITDIALOG, and only then shows it. See the arm at the end. */
            if (!isModal && g_WowUserSend16 && frame->IsCallbackAllowed)
                controlStyle &= ~(DWORD)WS_VISIBLE;
            if (windowClass->IsRegistered32) {
                window->Window32 = CreateWindowExA(0, windowClass->Class32, window->Text, controlStyle,
                                            usesDefault ? CW_USEDEFAULT32 : rect.left,
                                            usesDefault ? CW_USEDEFAULT32 : rect.top,
                                            rect.right - rect.left,
                                            rect.bottom - rect.top,
                                            parent32, menu,
                                            GetModuleHandleA(NULL), NULL);
                if (window->Window32) { ++g_WowWinCreated;
                                 if (!g_WowWinThread) g_WowWinThread = GetCurrentThreadId(); }
            }
        }

        /* ── AND NOW THE CONTROLS. Each item is a real child window of the
             dialog AND a Win16 window in our table, because the guest addresses
             them both ways: by real HWND when the OS delivers a message, and by
             16-bit handle the moment it calls GetDlgItem or SetDlgItemText.
           ⚠ THE CONTROL ID GOES IN hMenu, which is where Win32 keeps a child's
             id -- that is what makes GetDlgItem(hDlg, id) work at all, and it
             is the OS doing the lookup rather than us. */
        for (index = 0; index < count; ++index) {
            INT itemX, itemY, itemWidth, itemHeight;
            WORD itemId, itemTextOrdinal = 0;
            DWORD itemStyle;
            CHAR itemClassName[WOWUSER_NAME_SIZE], itemText[WOWUSER_NAME_SIZE];
            PCSTR prefix;
            PWOWUSER_CLASS itemClass;
            PWOWUSER_WINDOW control;

            itemX  = (INT)(SHORT)WowDlgTemplateWord(templateBytes, pointer);
            itemY  = (INT)(SHORT)WowDlgTemplateWord(templateBytes, pointer + WOWDLG_ITEM_Y);
            itemWidth = (INT)(SHORT)WowDlgTemplateWord(templateBytes, pointer + WOWDLG_ITEM_WIDTH);
            itemHeight = (INT)(SHORT)WowDlgTemplateWord(templateBytes, pointer + WOWDLG_ITEM_HEIGHT);
            itemId = WowDlgTemplateWord(templateBytes, pointer + WOWDLG_ITEM_ID);
            itemStyle = WowDlgTemplateDword(templateBytes, pointer + WOWDLG_ITEM_STYLE);
            pointer += WOWDLG_ITEM_SIZE;
            prefix = WowDlgClassName(templateBytes[pointer]);
            if (prefix) {
                INT itemCount;
                for (itemCount = 0; itemCount < (INT)sizeof itemClassName - 1 && prefix[itemCount]; ++itemCount) itemClassName[itemCount] = prefix[itemCount];
                itemClassName[itemCount] = 0;
                pointer += 1;
            } else {
                pointer += WowDlgTemplateString(templateBytes, pointer, itemClassName, sizeof itemClassName);
            }
            pointer += WowDlgTemplateNameOrdinal(templateBytes, pointer, itemText, sizeof itemText, &itemTextOrdinal);
            pointer += 1 + templateBytes[pointer];  /* BYTE cbCreationData, then the data */

            itemClass = WowUserFindClass(itemClassName);
            control = WowUserNewWindow();
            if (!control) {
                WowNotePut(note, noteCapacity, &noteLength, " -- ★ OUT OF WINDOW SLOTS AT ITEM ");
                WowNoteHex(note, noteCapacity, &noteLength, (DWORD)index, WOW_HEX_BYTE_DIGITS);
                break;
            }
            control->Class     = itemClass ? (WORD)(itemClass - g_WowUserClasses) : window->Class;
            control->Style   = itemStyle;
            control->WindowProcedure = itemClass ? itemClass->WindowProcedure : 0;
            control->Parent  = window->Window16;
            control->Menu    = itemId;
            control->Instance   = instance;
            control->PositionX  = MulDiv(itemX,  baseUnitX, WOWDLG_UNITS_PER_BASE_X);
            control->PositionY  = MulDiv(itemY,  baseUnitY, WOWDLG_UNITS_PER_BASE_Y);
            control->Width = MulDiv(itemWidth, baseUnitX, WOWDLG_UNITS_PER_BASE_X);
            control->Height = MulDiv(itemHeight, baseUnitY, WOWDLG_UNITS_PER_BASE_Y);
            {   INT itemCount;
                for (itemCount = 0; itemCount < (INT)sizeof control->Text; ++itemCount)
                    control->Text[itemCount] = (itemCount < (INT)sizeof itemText) ? itemText[itemCount] : 0;
            }
            if (itemClass && itemClass->IsRegistered32 && window->Window32) {
                /* s89: an owner-drawn list/combo box used to be made a plain one here,
                   because WM_DRAWITEM could not reach a 16-bit dialog and Charmap's
                   font list came up blank. It can now (#302, WowOwnerDraw in main.c),
                   so the control is created with the style the template asked for and
                   the program draws its own items -- Charmap's TrueType marks included. */
                DWORD controlStyle32 = itemStyle;
                control->Window32 = CreateWindowExA(0, itemClass->Class32, control->Text, controlStyle32,
                                             control->PositionX, control->PositionY, control->Width, control->Height,
                                             window->Window32,
                                             (HMENU)(ULONG_PTR)itemId,
                                             GetModuleHandleA(NULL), NULL);
                if (control->Window32) { ++made; ++g_WowWinCreated;
                    if (!firstFocus && (itemStyle & WS_TABSTOP)) firstFocus = control->Window16;  /* WS_TABSTOP */
                    /* #283: the template's font, as the dialog manager gives it */
                    /* ⚠ SYSTEM CONTROLS ONLY. An application class is 16-bit code:
                         the relay would hand it this raw Win32 HFONT as its 16-bit
                         font handle, and Sound Recorder's own controls then drew
                         no text at all (s89). */
                    if (font && itemClass->IsSystemClass)
                        SendMessageA(control->Window32, WM_SETFONT, (WPARAM)font, FALSE); }
            }
            /* ── ★★★ #302: AN APPLICATION-CLASS CONTROL IS TOLD IT WAS CREATED. ────
                 Measured on Sound Recorder (s89): its transport buttons are its own
                 class `SButton`, whose WM_CREATE loads the bitmap its text names
                 ("#Rewind" -> BITMAP REWIND) and keeps it in window word 6; its
                 paint reads word 6, finds 0, and falls back to drawing the text --
                 the "#Rewind" the user saw. No control built from a TEMPLATE ever
                 got WM_CREATE (CreateWindow's path sends it; this loop did not), so
                 every custom control in every dialog started uninitialised.
                 Sent now, through the nested run, as the dialog manager does: one
                 per control, in template order, before WM_INITDIALOG. The Win16
                 CREATESTRUCT (34 bytes, the CreateWindow argument block's order --
                 see WowUserWantCreate) is followed by the text and class strings
                 it points at. A -1 answer fails the control, as on Windows. */
            if (control->Window32 && control->WindowProcedure && itemClass && !itemClass->IsSystemClass && g_WowUserSend16Blob) {
                BYTE createStruct[WOWUSER_CS16_SIZE + WOWUSER_NAME_SIZE + WOWUSER_NAME_SIZE];
                static const INT createStructFixups[WOWUSER_CS16_FIXUPS] = { WOWUSER_CS16_NAME, WOWUSER_CS16_CLASS };  /* lpszName, lpszClass */
                INT itemIndex, titleLength = 0, createLength = 0;
                WORD result16 = 0;
                for (itemIndex = 0; itemIndex < (INT)sizeof createStruct; ++itemIndex) createStruct[itemIndex] = 0;
                createStruct[WOWUSER_CS16_INSTANCE]  = (BYTE)instance;  createStruct[WOWUSER_CS16_INSTANCE + 1]  = (BYTE)(instance >> BYTE_SHIFT);  /* hInstance  */
                createStruct[WOWUSER_CS16_MENU]  = (BYTE)itemId;    createStruct[WOWUSER_CS16_MENU + 1]  = (BYTE)(itemId >> BYTE_SHIFT);  /* hMenu = id */
                createStruct[WOWUSER_CS16_PARENT]  = (BYTE)window->Window16; createStruct[WOWUSER_CS16_PARENT + 1] = (BYTE)(window->Window16 >> BYTE_SHIFT);  /* hwndParent */
                createStruct[WOWUSER_CS16_HEIGHT] = (BYTE)control->Height; createStruct[WOWUSER_CS16_HEIGHT + 1] = (BYTE)(control->Height >> BYTE_SHIFT);
                createStruct[WOWUSER_CS16_WIDTH] = (BYTE)control->Width; createStruct[WOWUSER_CS16_WIDTH + 1] = (BYTE)(control->Width >> BYTE_SHIFT);
                createStruct[WOWUSER_CS16_Y] = (BYTE)control->PositionY;  createStruct[WOWUSER_CS16_Y + 1] = (BYTE)(control->PositionY >> BYTE_SHIFT);
                createStruct[WOWUSER_CS16_X] = (BYTE)control->PositionX;  createStruct[WOWUSER_CS16_X + 1] = (BYTE)(control->PositionX >> BYTE_SHIFT);
                createStruct[WOWUSER_CS16_STYLE] = (BYTE)itemStyle; createStruct[WOWUSER_CS16_STYLE + 1] = (BYTE)(itemStyle >> BYTE_SHIFT);
                createStruct[WOWUSER_CS16_STYLE + 2] = (BYTE)(itemStyle >> WORD_SHIFT); createStruct[WOWUSER_CS16_STYLE + 3] = (BYTE)(itemStyle >> TOP_BYTE_SHIFT);
                while (titleLength < WOWUSER_NAME_SIZE - 1 && control->Text[titleLength] && titleLength < (INT)sizeof control->Text) { createStruct[WOWUSER_CS16_SIZE + titleLength] = (BYTE)control->Text[titleLength]; ++titleLength; }
                while (createLength < WOWUSER_NAME_SIZE - 1 && itemClassName[createLength]) { createStruct[WOWUSER_CS16_SIZE + WOWUSER_NAME_SIZE + createLength] = (BYTE)itemClassName[createLength]; ++createLength; }
                createStruct[WOWUSER_CS16_NAME] = WOWUSER_CS16_SIZE;  createStruct[WOWUSER_CS16_NAME + 1] = 0;  /* -> fixed up to ss:sp+34 */
                createStruct[WOWUSER_CS16_CLASS] = WOWUSER_CS16_SIZE + WOWUSER_NAME_SIZE; createStruct[WOWUSER_CS16_CLASS + 1] = 0;
                if (g_WowUserSend16Blob(control->Window16, WM_CREATE16, 0, createStruct, (INT)sizeof createStruct,
                                 createStructFixups, WOWUSER_CS16_FIXUPS, &result16) && result16 == WOWUSER_MINUS_ONE16) {
                    DestroyWindow(control->Window32);
                    control->Window32 = NULL; control->Window16 = 0; --made;
                }
            }
        }

        /* ── ★★ AND SHOW IT, BECAUSE THE DIALOG MANAGER IS WHAT SHOWS A DIALOG.
             A template whose style omits WS_VISIBLE is not a hidden dialog: it
             is the ordinary case, and the manager shows it once the controls
             exist. We are the manager's 32-bit half, so it falls here.
           ★ MEASURED, and this is why it is not left to the guest: with the
             window created and every control built, CALC ran its ENTIRE
             WM_INITDIALOG -- GetWindowRect, the text metrics, the menu radio
             via CheckMenuItem, GetDlgItem+ShowWindow over all 15 scientific
             controls, CheckRadioButton, SetDlgItemText -- and then sat in its
             message loop. Nothing was stepped over afterwards, so nothing was
             waiting on us; there was simply no ShowWindow for the dialog
             ITSELF anywhere in the run, on either side.
           ★★★★★ SETTLED, session 56: this one thunk serves BOTH, and the
             argument at OFFSET 0 IS THE MODAL FLAG -- 0 for the modeless family
             (CreateDialog*) and 1 for the modal one (DialogBox*). Session 55
             recorded that field as "+0 still unexplained (0 in every run)"; it
             was 0 in every run because every guest measured then (CALC,
             SOUNDREC, TERMINAL) uses CreateDialog. USER.87 DIALOGBOX and
             USER.89 CREATEDIALOG BOTH return to their caller as soon as this
             thunk is answered (observed: TASKMAN, below) -- so the modal message
             loop is not in USER's 16-bit code. IT IS OURS.
           ⚠⚠ AND WE DO NOT RUN ONE, WHICH IS WHY A MODAL DIALOG ENDS ITS
             PROGRAM. DialogBox's whole contract is that it does not return
             until EndDialog; we create the dialog and return at once, so a
             caller whose WinMain is `DialogBox(...); return;` -- which is
             TASKMAN exactly -- exits immediately. Measured: TASKMAN builds the
             dialog and all 8 controls, then STAGE2: complete with no window.
             The window s55 saw was an ORPHAN kept alive by the host leak.
           ▶ FIVE GUESTS ON THE SHELF CALL DialogBox: NOTEPAD, PACKAGER,
             SYSEDIT, TASKMAN and WINMINE (3 calls). This is therefore the same
             mechanism as the SUB-DIALOG gap the user named in s53 --
             Minesweeper's Game > Preferences, Solitaire's Options and Deck.
             One feature unblocks all of it.
           ★★★★★ AND IT IS BUILT NOW -- session 57, in src/wow/wowdlg.h, exactly
             as the sentence below prescribed: park the guest inside the service,
             pump the dialog by calling its 16-bit dlgproc through the existing
             wowcall chain (the EDITLOCK -> EDITFILL -> LocalUnlock chain is the
             same shape), and complete the original DialogBox call with
             EndDialog's result. The warning that came with it -- "a half-built
             modal loop that never returns is worse than an honest immediate
             return, because it hangs the guest instead of ending it" -- is why
             that file has FOUR named exits and why two paths in the arm below
             still return immediately and say so. */
        if (window->Window32 && !(style & WS_VISIBLE) && (isModal || !g_WowUserSend16 || !frame->IsCallbackAllowed))
            ShowWindow(window->Window32, SW_SHOW);

        /* ★ SAY WHICH ONE THIS IS, EVERY TIME. Until the modal loop exists, a
             modal call is a program about to exit, and a log that called it
             "CreateDialog" would leave the reader hunting the wrong thing. */
        WowNotePut(note, noteCapacity, &noteLength, isModal ? "DialogBox (MODAL) " : "CreateDialog ");
        WowNoteQuoted(note, noteCapacity, &noteLength, caption);
        WowNotePut(note, noteCapacity, &noteLength, " class=");
        WowNoteQuoted(note, noteCapacity, &noteLength, className[0] ? className : "#32770");
        WowNotePut(note, noteCapacity, &noteLength, " style=0x");
        WowNoteHex(note, noteCapacity, &noteLength, style, WOW_HEX_DWORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, " items=");
        WowNoteHex(note, noteCapacity, &noteLength, (DWORD)count, WOW_HEX_BYTE_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, " built=");
        WowNoteHex(note, noteCapacity, &noteLength, (DWORD)made, WOW_HEX_BYTE_DIGITS);
        /* ── ★★★ THE DIALOG'S OWN PROCEDURE, KEPT ON THE WINDOW. ─────────────
             For a `#32770` dialog this is the ONLY procedure there is: the class
             is the system's, so `w->wndproc` is 0 and every message we could
             ever deliver had nowhere to go. Recorded for modeless dialogs too,
             because DispatchMessage needs the same answer for the same reason.
           ⚠ NOT USED IN PREFERENCE TO A CLASS PROCEDURE -- see
             WowUserWindowProcedureOf(). CALC's dialog is its own `SciCalc` class and
             passes dlgproc=0; SOUND RECORDER's passes 0x0a970028. Both were
             measured in session 56 and the pair is why the order matters. */
        window->DialogProcedure = dialogProcedure;

        if (isModal) {
            /* ── ★★★★★ AND HERE THE CALLER STOPS. (session 57) ────────────────
                 Everything above built the dialog; this is the half that makes
                 it a MODAL one. The DialogBox call is PARKED -- its return hole
                 is handed to the modal loop and nothing writes it until
                 EndDialog -- and the loop itself runs out of the BOP handler,
                 which is the only code in a position to enter 16-bit code. See
                 src/wow/wowdlg.h for the mechanism and for its four exits.
               ⚠ THE TWO FAILURE PATHS BELOW ARE SESSION 56'S BEHAVIOUR ON
                 PURPOSE, not an oversight: with callbacks off (wowcall.txt) or
                 the modal stack full, there is no way to run a loop, and an
                 immediate return ends the program legibly where a wait would
                 hang it. The line says which happened. */
            DWORD hole = (DWORD)(ULONG_PTR)(frame->FrameBase + WOW32_OFF_RET);
            WORD  dataSelector  = window->Instance ? window->Instance : windowClass->Instance;
            if (WowDlgActive())
                WowNotePut(note, noteCapacity, &noteLength, " [NESTED: a modal dialog is already"
                                           " running]");
            /* ⚠ AND IF THERE IS NO LOOP, UNDO THE DEFERRAL HERE. A dialog we
                 hid for an initialisation that is never going to happen is a
                 program with no window at all -- strictly worse than session
                 56's behaviour, which is what these two arms exist to preserve. */
            if (!frame->IsCallbackAllowed) {
                if (isShowDeferred && window->Window32) ShowWindow(window->Window32, SW_SHOW);
                WowNotePut(note, noteCapacity, &noteLength, " -- ★ MODAL, BUT CALLBACKS ARE NOT"
                                           " ARMED (wowcall.txt), so there can be"
                                           " no modal loop: we return immediately"
                                           " and a program whose WinMain ends at"
                                           " DialogBox EXITS.");
            } else if (!WowDlgPush(window->Window16, hole, dialogProcedure, window->WindowProcedure, dataSelector,
                                    isShowDeferred, parent32)) {
                if (isShowDeferred && window->Window32) ShowWindow(window->Window32, SW_SHOW);
                WowNotePut(note, noteCapacity, &noteLength, " -- ★ MODAL, BUT THE MODAL STACK IS"
                                           " FULL; returning immediately.");
            } else {
                frame->IsModalDialog = 1;
                /* s89: the frame's +6 DWORD is DialogBoxParam's lParam (it arrives
                   as 0 for plain DialogBox). */
                WowDlgSetInit(Wow32ArgDword(frame, WOWUSER_CD_ARG_PARAM), firstFocus);
                WowNotePut(note, noteCapacity, &noteLength, " -- ★ MODAL: the caller is PARKED here"
                                           " and does not resume until EndDialog."
                                           " Its return value is held open; the"
                                           " MODAL lines that follow are the loop");
            }
        }
        WowNotePut(note, noteCapacity, &noteLength, " -> hwnd=0x");
        WowNoteHex(note, noteCapacity, &noteLength, window->Window16, WOW_HEX_WORD_DIGITS);
        if (window->Window32) {
            WowNotePut(note, noteCapacity, &noteLength, " HWND=0x");
            WowNoteHex(note, noteCapacity, &noteLength, (DWORD)(ULONG_PTR)window->Window32, WOW_HEX_DWORD_DIGITS);
        } else {
            WowNotePut(note, noteCapacity, &noteLength, " -- ★ NO REAL WINDOW (gle=0x");
            WowNoteHex(note, noteCapacity, &noteLength, GetLastError(), WOW_HEX_DWORD_DIGITS);
            WowNotePut(note, noteCapacity, &noteLength, ")");
        }
        /* ── ★★ +2 IS THE TEMPLATE'S LENGTH IN BYTES, AND TWO RUNS PROVE IT.
             It was printed as "unexplained" for exactly one session, which is
             the right way round: CALC passed 0x0140 there and `neres.py list`
             reports its SC dialog as 320 bytes; SOUND RECORDER passed 0x0210
             and its DIALOG 1 is 528. Two different programs, two different
             numbers, both the resource's own size to the byte.
           ⚠ AND IT WAS NEARLY READ AS A WINDOW HANDLE, because CALC's 0x0140
             also happened to be the next handle our own allocator would issue.
             A number matching something is not a number meaning it.
           +0 IS THE MODAL FLAG (session 56, above) and +4 is still unaccounted
             for -- zero in every run so far, so there is nothing yet to explain.
             dlgproc is 0 for CALC and non-zero for SOUND RECORDER (0x0a970028),
             so USER does pass it sometimes.
           ⚠ AND THE SENTENCE THAT USED TO END THIS PARAGRAPH WAS WRONG: "we do
             not need it, because USER runs the dialog's own message loop and
             calls the procedure itself". USER does neither -- session 56 showed
             both exports return immediately -- and a `#32770` dialog
             has no other procedure, so dlgproc is the ONLY way to drive one. It
             is now kept on the window. */
        WowNotePut(note, noteCapacity, &noteLength, " [tmpl_len=0x");
        WowNoteHex(note, noteCapacity, &noteLength, Wow32ArgDword(frame, WOWUSER_CD_ARG_LENGTH), WOW_HEX_DWORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, " dlgproc=0x");
        WowNoteHex(note, noteCapacity, &noteLength, dialogProcedure, WOW_HEX_DWORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, " modal=0x");
        WowNoteHex(note, noteCapacity, &noteLength, isModal, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, "]");
        if (usesDefault) WowNotePut(note, noteCapacity, &noteLength, " [template said -32768: the OS"
                                               " placed it]");
        Wow32SetReturn(frame, window->Window16);
        /* ── ⛔ #162 (Charmap): A MODELESS DIALOG GETS WM_INITDIALOG TOO, before
             CreateDialog returns. Only the modal loop (wowdlg.h) ever sent it, so
             a program whose main window is a modeless dialog never initialised:
             Charmap enumerates its fonts, fills its font list and builds its
             character grid there -- empty list, no grid. Through the window's own
             procedure (for an app-class dialog that is the class procedure, whose
             DefDlgProc reaches the DLGPROC); KEEP, so the caller still gets the
             handle. wParam = the first WS_TABSTOP control, as USER passes it;
             lParam 0, as the modal loop sends it (CreateDialogParam's value is not
             pinned in this frame yet). */
        if (!isModal && frame->IsCallbackAllowed && window->Window32 && WowUserWindowProcedureOf(window)) {
            /* lParam: CreateDialogParam's value, the frame's +6 DWORD; it arrives as
               0 for plain CreateDialog.
               ⛔ Passing 0 to a CreateDialogParam caller HID Sound Recorder: it reads
               its show state from it and called ShowWindow(SW_HIDE). */
            WORD result16 = 0, savedWindow = g_WowUserInitDialogWindow;
            INT  savedShown = g_WowUserIsInitDialogShown, isSent = 0;
            /* ★ s89 (#302): SENT, before CreateDialog returns, with the window still
                 hidden -- then shown, as the dialog manager does. If the program
                 called ShowWindow on it during WM_INITDIALOG (Sound Recorder hides
                 itself that way), its choice stands. If the nested run cannot go,
                 the old deferred path below is used unchanged. */
            if (g_WowUserSend16) {
                g_WowUserInitDialogWindow = window->Window16; g_WowUserIsInitDialogShown = 0;
                isSent = g_WowUserSend16(window->Window16, WM_INITDIALOG, firstFocus,
                                   Wow32ArgDword(frame, WOWUSER_CD_ARG_PARAM), &result16);
                if (isSent && !g_WowUserIsInitDialogShown && window->Window32 && IsWindow(window->Window32))
                    ShowWindow(window->Window32, SW_SHOW);
                if (!isSent && window->Window32) ShowWindow(window->Window32, SW_SHOW);
                g_WowUserInitDialogWindow = savedWindow; g_WowUserIsInitDialogShown = savedShown;
            }
            if (isSent) {
                WowNotePut(note, noteCapacity, &noteLength, " + WM_INITDIALOG SENT (modeless), then shown");
            } else {
                WowUserWantMessage(frame, window, window->Instance ? window->Instance : g_WowUserClasses[window->Class].Instance,
                                 WM_INITDIALOG, firstFocus, Wow32ArgDword(frame, WOWUSER_CD_ARG_PARAM),
                                 WOWCALL_RET_KEEP);
                WowNotePut(note, noteCapacity, &noteLength, " + WM_INITDIALOG (modeless)");
            }
        } else if (!isModal && window->Window32 && !IsWindowVisible(window->Window32)
                   && g_WowUserSend16 && frame->IsCallbackAllowed) {
            ShowWindow(window->Window32, SW_SHOW);  /* no procedure: shown as before */
        }
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
             WowUserDefProc.

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
        WORD  window16 = Wow32ArgWord(frame, WOWUSER_SM_ARG_HWND);
        WORD  message16  = Wow32ArgWord(frame, WOWUSER_SM_ARG_MSG);
        WORD  wParam   = Wow32ArgWord(frame, WOWUSER_SM_ARG_WPARAM);
        DWORD lParam   = Wow32ArgDword(frame, WOWUSER_SM_ARG_LPARAM);
        PWOWUSER_WINDOW window;
        INT noteLength = 0;
        if (!frame->IsCallbackAllowed) return 0;
        window = WowUserFindWindow(window16);
        if (!window) {
            WowNotePut(note, noteCapacity, &noteLength, "SendMessage: no such window 0x");
            WowNoteHex(note, noteCapacity, &noteLength, window16, WOW_HEX_WORD_DIGITS);
            WowNotePut(note, noteCapacity, &noteLength, " msg 0x");
            WowNoteHex(note, noteCapacity, &noteLength, message16, WOW_HEX_WORD_DIGITS);
            Wow32SetReturn(frame, 0);
            return 1;
        }
        if (WowUserWindowProcedureOf(window)) {
            WowNotePut(note, noteCapacity, &noteLength, "SendMessage 0x");
            WowNoteHex(note, noteCapacity, &noteLength, window16, WOW_HEX_WORD_DIGITS);
            WowNotePut(note, noteCapacity, &noteLength, " msg 0x");
            WowNoteHex(note, noteCapacity, &noteLength, message16, WOW_HEX_WORD_DIGITS);
            WowNotePut(note, noteCapacity, &noteLength, window->WindowProcedure ? " -> its own window procedure"
                                                  : " -> its DIALOG procedure");
            /* Written so the hole is never uninitialised if the call is
               refused; wowcall.h overwrites it with the real answer. */
            Wow32SetReturn(frame, 0);
            WowUserWantMessage(frame, window, window->Instance ? window->Instance : g_WowUserClasses[window->Class].Instance,
                             message16, wParam, lParam, WOWCALL_RET_RESULT);
            return 1;
        }
        Wow32SetReturn(frame, (DWORD)WowUserDefProc(frame, window, message16, wParam, lParam, note, noteCapacity));
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
        INT isSet = (frame->Id == WOWUSER_SETWINDOWWORD);
        WORD window16 = Wow32ArgWord(frame, isSet ? WOWUSER_SWW_ARG_HWND  : WOWUSER_GWW_ARG_HWND);
        SHORT fieldIndex = (SHORT)Wow32ArgWord(frame, isSet ? WOWUSER_SWW_ARG_INDEX : WOWUSER_GWW_ARG_INDEX);
        WORD value  = isSet ? Wow32ArgWord(frame, WOWUSER_SWW_ARG_VALUE) : 0;
        PWOWUSER_WINDOW window = WowUserFindWindow(window16);
        INT noteLength = 0;
        WowNotePut(note, noteCapacity, &noteLength, isSet ? "SetWindowWord 0x" : "GetWindowWord 0x");
        WowNoteHex(note, noteCapacity, &noteLength, window16, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, " index ");
        if (fieldIndex < 0) { WowNotePut(note, noteCapacity, &noteLength, "-0x");
                       WowNoteHex(note, noteCapacity, &noteLength, (DWORD)(-fieldIndex), WOW_HEX_BYTE_DIGITS); }
        else         { WowNotePut(note, noteCapacity, &noteLength, "0x");
                       WowNoteHex(note, noteCapacity, &noteLength, (DWORD)fieldIndex, WOW_HEX_BYTE_DIGITS); }
        if (!window) {
            WowNotePut(note, noteCapacity, &noteLength, " -- NO SUCH WINDOW");
            Wow32SetReturn(frame, 0);
            return 1;
        }
        /* ── #302 (s89): THREE STANDARD FIELDS, from a source and from a guest.
             GWW_HINSTANCE -6, GWW_HWNDPARENT -8, GWW_ID -12 -- the values Win32
             kept as GWL_* (Wine include/winuser.h), and Sound Recorder's SButton
             asked -6 and passed the answer to LoadBitmap as its hInstance: with 0
             there, BITMAP REWIND was never found and the button drew "#Rewind".
             The answers are this window's own record: its instance (the class's
             if the window has none), its parent, its id (a child's hMenu). A set
             updates the record and returns the previous value. Other negative
             indexes are still an honest 0. */
        if (fieldIndex == WOWUSER_GWW16_HINSTANCE || fieldIndex == WOWUSER_GWW16_HWNDPARENT || fieldIndex == WOWUSER_GWW16_ID) {
            PWORD field = (fieldIndex == WOWUSER_GWW16_HINSTANCE) ? &window->Instance : (fieldIndex == WOWUSER_GWW16_HWNDPARENT) ? &window->Parent : &window->Menu;
            WORD current  = *field;
            if (fieldIndex == WOWUSER_GWW16_HINSTANCE && !current) current = g_WowUserClasses[window->Class].Instance;
            if (isSet) { *field = value; WowNotePut(note, noteCapacity, &noteLength, " (standard) <- 0x");
                       WowNoteHex(note, noteCapacity, &noteLength, value, WOW_HEX_WORD_DIGITS); }
            else     { WowNotePut(note, noteCapacity, &noteLength, fieldIndex == WOWUSER_GWW16_HINSTANCE ? " GWW_HINSTANCE -> 0x"
                                                 : fieldIndex == WOWUSER_GWW16_HWNDPARENT ? " GWW_HWNDPARENT -> 0x"
                                                             : " GWW_ID -> 0x");
                       WowNoteHex(note, noteCapacity, &noteLength, current, WOW_HEX_WORD_DIGITS); }
            Wow32SetReturn(frame, current);
            return 1;
        }
        if (fieldIndex < 0) {
            WowNotePut(note, noteCapacity, &noteLength, " -- a STANDARD field; this host does not"
                                       " answer those yet, returning 0");
            Wow32SetReturn(frame, 0);
            return 1;
        }
        {   PCWOWUSER_CLASS windowClass = &g_WowUserClasses[window->Class];
            if (fieldIndex + WOW_WORD_BYTES > (INT)windowClass->WindowExtra || fieldIndex + WOW_WORD_BYTES > (INT)sizeof window->Extra) {
                WowNotePut(note, noteCapacity, &noteLength, " -- OUT OF RANGE, class declared"
                                           " cbWndExtra=0x");
                WowNoteHex(note, noteCapacity, &noteLength, windowClass->WindowExtra, WOW_HEX_WORD_DIGITS);
                Wow32SetReturn(frame, 0);
                return 1;
            }
        }
        if (isSet) {
            WowNotePut(note, noteCapacity, &noteLength, " <- 0x");
            WowNoteHex(note, noteCapacity, &noteLength, value, WOW_HEX_WORD_DIGITS);
            /* Win16 returns the PREVIOUS value, so read before writing. */
            Wow32SetReturn(frame, window->Extra[fieldIndex / WOW_WORD_BYTES]);
            window->Extra[fieldIndex / WOW_WORD_BYTES] = value;
        } else {
            WowNotePut(note, noteCapacity, &noteLength, " -> 0x");
            WowNoteHex(note, noteCapacity, &noteLength, window->Extra[fieldIndex / WOW_WORD_BYTES], WOW_HEX_WORD_DIGITS);
            Wow32SetReturn(frame, window->Extra[fieldIndex / WOW_WORD_BYTES]);
        }
        return 1;
    }

    /* ── ★★★ 0x217 NotifyWow(lpBlock, wKind) -- see the note above. ───────────
         ⚠ ONLY THE KIND THAT HAS BEEN READ. `wKind == 3` is what LoadAccelerators
           passes, and that call site is the only one this run has ever taken.
           Answering every kind would be claiming to have understood a namespace
           of which exactly one member has been measured -- so anything else falls
           through to the honest "unimplemented", and the log says which kind. */
    /* ── ★★★ 0x16c LookupIconIdFromDirectoryEx -- see the note at the define. */
    case WOWUSER_LOOKUPICONID: {
        volatile BYTE *directoryBytes = Wow32ArgPointer(frame, WOWUSER_LII_ARG_DIR);
        WORD isIcon = Wow32ArgWord(frame, WOWUSER_LII_ARG_FICON);
        WORD width  = Wow32ArgWord(frame, WOWUSER_LII_ARG_CX), height = Wow32ArgWord(frame, WOWUSER_LII_ARG_CY);
        WORD flags  = Wow32ArgWord(frame, WOWUSER_LII_ARG_FLAGS);
        BYTE directory[WOWUSER_ICONDIR_HEADER_SIZE + WOWUSER_ICONDIR_MAX_ENTRIES * WOWUSER_ICONDIR_ENTRY_SIZE];
        WORD count;
        UINT entryCount, index2;
        INT iconId, noteLength = 0;
        WowNotePut(note, noteCapacity, &noteLength, isIcon ? "LookupIconIdFromDirectoryEx (icon"
                                       : "LookupIconIdFromDirectoryEx (cursor");
        WowNotePut(note, noteCapacity, &noteLength, ") cx=0x"); WowNoteHex(note, noteCapacity, &noteLength, width, WOW_HEX_BYTE_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, " cy=0x");  WowNoteHex(note, noteCapacity, &noteLength, height, WOW_HEX_BYTE_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, " flags=0x"); WowNoteHex(note, noteCapacity, &noteLength, flags, WOW_HEX_WORD_DIGITS);
        if (!directoryBytes) { WowNotePut(note, noteCapacity, &noteLength, " -- ★ NO DIRECTORY; answered 0");
                  Wow32SetReturn(frame, 0); return 1; }
        /* idReserved 0, idType 1 (icon) or 2 (cursor), idCount; then 14-byte
           entries ending in the WORD id. Copied out: the OS reads it as a whole. */
        count = (WORD)(directoryBytes[WOWUSER_ICONDIR_COUNT] | (directoryBytes[WOWUSER_ICONDIR_COUNT + 1] << BYTE_SHIFT));
        if (count == 0 || count > WOWUSER_ICONDIR_MAX_ENTRIES) {
            WowNotePut(note, noteCapacity, &noteLength, " -- ★ count=0x"); WowNoteHex(note, noteCapacity, &noteLength, count, WOW_HEX_WORD_DIGITS);
            WowNotePut(note, noteCapacity, &noteLength, " is not a directory; answered 0");
            Wow32SetReturn(frame, 0); return 1;
        }
        entryCount = (UINT)WOWUSER_ICONDIR_HEADER_SIZE + count * (UINT)WOWUSER_ICONDIR_ENTRY_SIZE;
        for (index2 = 0; index2 < entryCount; ++index2) directory[index2] = directoryBytes[index2];
        iconId = LookupIconIdFromDirectoryEx(directory, isIcon ? TRUE : FALSE, (SHORT)width, (SHORT)height, flags);
        WowNotePut(note, noteCapacity, &noteLength, " entries=0x"); WowNoteHex(note, noteCapacity, &noteLength, count, WOW_HEX_BYTE_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, " -> id 0x"); WowNoteHex(note, noteCapacity, &noteLength, (DWORD)iconId, WOW_HEX_WORD_DIGITS);
        Wow32SetReturn(frame, (DWORD)(WORD)iconId);
        return 1;
    }

    case WOWUSER_NOTIFYWOW: {
        volatile BYTE *block = Wow32ArgPointer(frame, WOWUSER_NOTIFY_ARG_BLOCK);
        WORD kind = Wow32ArgWord(frame, WOWUSER_NOTIFY_ARG_KIND);
        INT noteLength = 0;
        if (kind == WOWNOTIFY_USERINIT && block) {
            volatile BYTE *maximumBytes   = Wow32FarAt(frame, block, WOWUSER_NOTIFY_UI_MAX_OFF);
            volatile BYTE *bitsBytes = Wow32FarAt(frame, block, WOWUSER_NOTIFY_UI_BITS_OFF);
            WORD bitsSize = WowUserPeek(block, WOWUSER_NOTIFY_UI_BITS_CB), top = 0;
            UINT index;
            WowNotePut(note, noteCapacity, &noteLength, "NotifyWow(USER init) DefWindowProc forward table: ");
            if (!maximumBytes || !bitsBytes || !bitsSize || bitsSize > WOWUSER_NOTIFY_UI_BITS_MAX) {
                WowNotePut(note, noteCapacity, &noteLength, "★ UNREADABLE BLOCK -- nothing forwarded");
                Wow32SetReturn(frame, 0);
                return 1;
            }
            for (index = 0; index < bitsSize; ++index) bitsBytes[index] = 0;
            for (index = 0; index < sizeof g_WowUserDefWindowProcForwarded / sizeof g_WowUserDefWindowProcForwarded[0]; ++index) {
                WORD message16 = g_WowUserDefWindowProcForwarded[index];
                if ((UINT)(message16 >> BITMAP_BYTE_SHIFT) >= bitsSize) continue;
                bitsBytes[message16 >> BITMAP_BYTE_SHIFT] = (BYTE)(bitsBytes[message16 >> BITMAP_BYTE_SHIFT] | (1u << (message16 & BITMAP_BIT_MASK)));
                if (message16 > top) top = message16;
                WowNotePut(note, noteCapacity, &noteLength, "0x"); WowNoteHex(note, noteCapacity, &noteLength, message16, WOW_HEX_WORD_DIGITS);
                WowNotePut(note, noteCapacity, &noteLength, " ");
            }
            Wow32PokeWord(maximumBytes, top);
            WowNotePut(note, noteCapacity, &noteLength, "max=0x"); WowNoteHex(note, noteCapacity, &noteLength, top, WOW_HEX_WORD_DIGITS);
            WowNotePut(note, noteCapacity, &noteLength, " cb=0x"); WowNoteHex(note, noteCapacity, &noteLength, bitsSize, WOW_HEX_WORD_DIGITS);
            /* ⚠ 0, DELIBERATELY. USER keeps this answer, and a non-zero one switches
                 USER onto a different internal path at start-up (it modifies its
                 own code). Whether real WOW32 asks for that is not measured yet, and
                 the stepped-over call has always answered 0, so that path has never
                 run here. Only the table changes in this step. */
            Wow32SetReturn(frame, 0);
            return 1;
        }
        /* ── s92 (#306): kind 6 -- "where is the window of this CLASS?", arriving
             from a guest's WinHelp() with the class name "MS_WINHELP" (as logged).
             The answer is DX:AX: DX non-zero = found, and AX is the window the
             registered WM_WINHELP is then SENT to. Stepped over it answered 0, so
             Help said "Not enough memory available" with WinHelp's window open. */
        if (kind == WOWNOTIFY_FINDCLASS && block) {
            CHAR className[WOWUSER_NAME_SIZE];
            DWORD blockPointer = Wow32ArgDword(frame, WOWUSER_NOTIFY_ARG_BLOCK);
            HWND  window32 = NULL;
            WORD  window16Result = 0;
            if (WowUserFarString(frame, blockPointer, className, sizeof className)) window32 = WowUserFindWindowByClass(className, NULL);
            window16Result = window32 ? WowWinHwnd16(window32) : 0;
            WowNotePut(note, noteCapacity, &noteLength, "NotifyWow(6, find class \"");
            WowNotePut(note, noteCapacity, &noteLength, className);
            WowNotePut(note, noteCapacity, &noteLength, window16Result ? "\") -> 0x" : "\") -> not found");
            if (window16Result) WowNoteHex(note, noteCapacity, &noteLength, window16Result, WOW_HEX_WORD_DIGITS);
            Wow32SetReturn(frame, window16Result ? (WOWUSER_NOTIFY_FOUND | window16Result) : 0);
            return 1;
        }
        if (kind != WOWNOTIFY_ACCEL || !block) return 0;
        WowNotePut(note, noteCapacity, &noteLength, "NotifyWow(RT_ACCELERATOR) hInst=0x");
        WowNoteHex(note, noteCapacity, &noteLength, WowUserPeek(block, WOWUSER_NOTIFY_HINSTANCE), WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, " hRes=0x");
        WowNoteHex(note, noteCapacity, &noteLength, WowUserPeek(block, WOWUSER_NOTIFY_HRESDATA), WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, " at 0x");
        WowNoteHex(note, noteCapacity, &noteLength, WowUserPeek(block, WOWUSER_NOTIFY_LPRESOURCE + WOW_WORD_BYTES), WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, ":0x");
        WowNoteHex(note, noteCapacity, &noteLength, WowUserPeek(block, WOWUSER_NOTIFY_LPRESOURCE), WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, " cb=0x");
        WowNoteHex(note, noteCapacity, &noteLength, (DWORD)WowUserPeek(block, WOWUSER_NOTIFY_CBRESOURCE)
                  | ((DWORD)WowUserPeek(block, WOWUSER_NOTIFY_CBRESOURCE + WOW_WORD_BYTES) << WORD_SHIFT), WOW_HEX_DWORD_DIGITS);
        /* Non-zero is "noted"; the handle the application gets is the guest's
           own resource handle. 1 rather than a number that looks like a handle,
           so nothing downstream can mistake it for one. */
        Wow32SetReturn(frame, 1);
        return 1;
    }

    /* ── ★★★★★ 0x6c GetMessage / 0x6d PeekMessage -- THE LOOP TURNS ───────────
         The host's job here is exactly to FILL AN 18-BYTE STRUCTURE. It does not
         dispatch anything: `DispatchMessage` is a separate call -- see the note at
         the top of src/wow/wowmsg.h for the layout and the loop.

       ★ THE RETURN VALUES, AS DOCUMENTED AND AS SYSEDIT'S LOOP USES THEM:
         non-zero keeps the loop, and
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
        INT isPeek = (frame->Id == WOWUSER_PEEKMESSAGE);
        volatile BYTE *messageBytes = Wow32ArgPointer(frame, isPeek ? WOWMSG_PEEKMESSAGE_ARG_LPMSG : WOWMSG_GETMESSAGE_ARG_LPMSG);
        WORD filter16 = Wow32ArgWord(frame, isPeek ? WOWMSG_PEEKMESSAGE_ARG_HWND : WOWMSG_GETMESSAGE_ARG_HWND);
        WORD filterMin  = Wow32ArgWord(frame, isPeek ? WOWMSG_PEEKMESSAGE_ARG_MIN  : WOWMSG_GETMESSAGE_ARG_MIN);
        WORD filterMax  = Wow32ArgWord(frame, isPeek ? WOWMSG_PEEKMESSAGE_ARG_MAX  : WOWMSG_GETMESSAGE_ARG_MAX);
        WORD remove   = isPeek ? Wow32ArgWord(frame, WOWMSG_PEEKMESSAGE_ARG_REMOVE) : PM_REMOVE16;
        WOWMSG message;
        INT noteLength = 0;
        if (!messageBytes) {
            WowNotePut(note, noteCapacity, &noteLength, isPeek ? "PeekMessage" : "GetMessage");
            WowNotePut(note, noteCapacity, &noteLength, ": unreadable lpMsg -- answered 0");
            Wow32SetReturn(frame, 0);
            return 1;
        }
        {   /* #160: a menu held back for this application's inits is opened HERE,
                 once the take has passed its marker -- see WOWMSG_MENUREPLAY. */
            INT found;
            g_WowMsgTaker = (g_WowUserCurrentTask == WOWUSER_TASK_NONE16) ? 0 : g_WowUserCurrentTask;  /* s92 #306 */
            found = WowMsgTake(filter16, filterMin, filterMax, (remove & PM_REMOVE16) != 0, &message);
            if (g_WowMsgIsReplayDue) {
                WOWMSG replayed = g_WowMsgReplay;
                g_WowMsgIsReplayDue = 0;
                if (found && (remove & PM_REMOVE16) == 0) found = 0;  /* re-peek after */
                if (!found) {
                    WowWinMenuReplay(&replayed);
                    found = WowMsgTake(filter16, filterMin, filterMax, (remove & PM_REMOVE16) != 0, &message);
                    /* ⚠ An EMPTY queue after the menu closed must not read as the
                         expired wait that means WM_QUIT: hand back a WM_NULL, which
                         the loop dispatches to nothing and then asks again. */
                    if (!found && !isPeek) {
                        message.Window = replayed.Window; message.Message = 0; message.WParam = 0; message.LParam = 0;
                        message.Time = GetTickCount(); message.PointX = replayed.PointX; message.PointY = replayed.PointY;
                        found = 1;
                    }
                } else {
                    g_WowMsgReplay = replayed; g_WowMsgIsReplayDue = 1;  /* run it next call */
                }
                WowNotePut(note, noteCapacity, &noteLength, "[menu opened after its inits] ");
            }
            g_WowMsgTaker = 0;
            if (found) {
            WowMsgWrite(messageBytes, &message);
            WowNotePut(note, noteCapacity, &noteLength, isPeek ? "PeekMessage -> hwnd=0x"
                                            : "GetMessage -> hwnd=0x");
            WowNoteHex(note, noteCapacity, &noteLength, message.Window, WOW_HEX_WORD_DIGITS);
            WowNotePut(note, noteCapacity, &noteLength, " msg=0x");
            WowNoteHex(note, noteCapacity, &noteLength, message.Message, WOW_HEX_WORD_DIGITS);
            WowNotePut(note, noteCapacity, &noteLength, " wParam=0x");
            WowNoteHex(note, noteCapacity, &noteLength, message.WParam, WOW_HEX_WORD_DIGITS);
            WowNotePut(note, noteCapacity, &noteLength, " lParam=0x");
            WowNoteHex(note, noteCapacity, &noteLength, message.LParam, WOW_HEX_DWORD_DIGITS);
            WowNotePut(note, noteCapacity, &noteLength, (remove & PM_REMOVE16) ? " [removed]" : " [left]");
            WowNotePut(note, noteCapacity, &noteLength, ", ");
            WowNoteHex(note, noteCapacity, &noteLength, (DWORD)g_WowMsgCount, WOW_HEX_BYTE_DIGITS);
            WowNotePut(note, noteCapacity, &noteLength, " still queued");
            /* ⚠ WM_QUIT is delivered AND reported as the end. A loop that got a
                 non-zero for it would dispatch a message meant to stop it. */
            Wow32SetReturn(frame, (message.Message == WM_QUIT16 && !isPeek) ? 0 : 1);
            return 1;
            }
        }
        if (!isPeek && WowMsgQuitFor((g_WowUserCurrentTask == WOWUSER_TASK_NONE16) ? 0 : g_WowUserCurrentTask)) {
            WORD quitCode = WowMsgTakeQuit(
                WowMsgQuitFor((g_WowUserCurrentTask == WOWUSER_TASK_NONE16) ? 0 : g_WowUserCurrentTask));
            message.Window = 0; message.Message = WM_QUIT16; message.WParam = quitCode;
            message.LParam = 0; message.Time = 0; message.PointX = message.PointY = 0;
            WowMsgWrite(messageBytes, &message);
            WowNotePut(note, noteCapacity, &noteLength, "GetMessage -> WM_QUIT (PostQuitMessage 0x");
            WowNoteHex(note, noteCapacity, &noteLength, quitCode, WOW_HEX_WORD_DIGITS);
            WowNotePut(note, noteCapacity, &noteLength, ") -- the loop ends");
            Wow32SetReturn(frame, 0);
            return 1;
        }
        if (isPeek) {
            /* ⚠ "empty" AND "nothing matched" ARE DIFFERENT FACTS, and saying
                 the first for both is how a filtered-peek deadlock hid: the
                 depth is the number that names it. */
            WowNotePut(note, noteCapacity, &noteLength, g_WowMsgCount
                        ? "PeekMessage: nothing matched the filter -> 0; queued 0x"
                        : "PeekMessage: queue empty -> 0 (correct: peek does not"
                          " block); queued 0x");
            WowNoteHex(note, noteCapacity, &noteLength, (DWORD)g_WowMsgCount, WOW_HEX_BYTE_DIGITS);
            Wow32SetReturn(frame, 0);
            return 1;
        }
        /* Nothing, and the host has already waited. Say so in full: a reader who
           sees `GetMessage -> 0` without this sentence would read it as WM_QUIT
           having been posted, which is a different fact entirely. */
        WowNotePut(note, noteCapacity, &noteLength, "GetMessage: the queue is EMPTY and the host's"
                                   " input wait expired -- no message can arrive,"
                                   " so the application is told to quit. Posted 0x");
        WowNoteHex(note, noteCapacity, &noteLength, g_WowMsgPosted, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, " taken 0x");
        WowNoteHex(note, noteCapacity, &noteLength, g_WowMsgTaken, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, " this run");
        Wow32SetReturn(frame, 0);
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
        WORD  window16 = Wow32ArgWord(frame, WOWMSG_POSTMESSAGE_ARG_HWND);
        WORD  message16  = Wow32ArgWord(frame, WOWMSG_POSTMESSAGE_ARG_MSG);
        WORD  wParam   = Wow32ArgWord(frame, WOWMSG_POSTMESSAGE_ARG_WPARAM);
        DWORD lParam   = Wow32ArgDword(frame, WOWMSG_POSTMESSAGE_ARG_LPARAM);
        INT isOk, noteLength = 0;
        isOk = WowMsgPost(window16, message16, wParam, lParam, 0, 0, 0);
        WowNotePut(note, noteCapacity, &noteLength, "PostMessage 0x");
        WowNoteHex(note, noteCapacity, &noteLength, window16, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, " msg=0x");
        WowNoteHex(note, noteCapacity, &noteLength, message16, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, isOk ? " -> queued" : " -> ★ RING FULL, DROPPED");
        Wow32SetReturn(frame, (DWORD)isOk);
        return 1;
    }

    /* ── 0x06 PostQuitMessage(nExitCode) ───────────────────────────────────────
         Not a queue entry: Win16 sets a flag on the task and GetMessage
         manufactures WM_QUIT only once everything else has drained. Queueing it
         would let it overtake messages already posted. */
    case WOWUSER_POSTQUITMESSAGE: {
        INT noteLength = 0;
        WowMsgPostQuit((g_WowUserCurrentTask == WOWUSER_TASK_NONE16) ? 0 : g_WowUserCurrentTask,  /* s92 #306 */
                         Wow32ArgWord(frame, WOWMSG_POSTQUITMESSAGE_ARG_EXITCODE));
        WowNotePut(note, noteCapacity, &noteLength, "PostQuitMessage 0x");
        WowNoteHex(note, noteCapacity, &noteLength, g_WowMsgQuitCode, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, " -- the next drained queue ends the loop");
        Wow32SetReturn(frame, 0);
        return 1;
    }

    /* ── ★★★★★ 0x72 DispatchMessage(lpMsg) -- THE LAST LINK IN THE LOOP ───────
         The message the host queued, the application took out of GetMessage and
         is now handing back to be delivered. Everything a window procedure needs
         is in those 18 bytes, which is why this call takes nothing else -- and
         the delivery itself is `WowCallEnter`, built in session 40 and used here
         without a line of new machinery.
       ★ THE RETURN IS THE PROCEDURE'S, exactly as for SendMessage, so it goes
         back through WOWCALL_RET_RESULT rather than being invented.
       ⚠ hwnd 0 IS NOT AN ERROR -- it is a thread message, and USER's own
         DispatchMessage `jcxz`es one. Answering 0 is what that means. */
    case WOWUSER_DISPATCHMESSAGE: {
        volatile BYTE *messageBytes = Wow32ArgPointer(frame, WOWMSG_DISPATCHMESSAGE_ARG_LPMSG);
        WOWMSG message;
        PWOWUSER_WINDOW window;
        INT noteLength = 0;
        if (!messageBytes) {
            WowNotePut(note, noteCapacity, &noteLength, "DispatchMessage: unreadable lpMsg");
            Wow32SetReturn(frame, 0);
            return 1;
        }
        WowMsgRead(messageBytes, &message);
        WowNotePut(note, noteCapacity, &noteLength, "DispatchMessage hwnd=0x");
        WowNoteHex(note, noteCapacity, &noteLength, message.Window, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, " msg=0x");
        WowNoteHex(note, noteCapacity, &noteLength, message.Message, WOW_HEX_WORD_DIGITS);
        if (!message.Window && message.Message == WM_TIMER16 && message.LParam && frame->IsCallbackAllowed) {
            /* s93: a windowless timer's TIMERPROC, called (NULL, WM_TIMER, id, time);
               its DS comes from its MakeProcInstance thunk (AX), the task's own
               instance is passed for a procedure that reads DS instead. */
            WORD dataSelector = 0;
            DWORD taskBase = (g_WowUserCurrentTask && g_WowUserCurrentTask != WOWUSER_TASK_NONE16) ? DpmiSelectorBase(g_WowUserCurrentTask) : 0;
            if (taskBase) { const volatile BYTE *taskBytes = (const volatile BYTE *)(ULONG_PTR)taskBase;
                      dataSelector = (WORD)(taskBytes[WOWUSER_TDB_HINSTANCE] | (taskBytes[WOWUSER_TDB_HINSTANCE + 1] << BYTE_SHIFT)); }
            WowNotePut(note, noteCapacity, &noteLength, " -> a windowless timer's TIMERPROC 0x");
            WowNoteHex(note, noteCapacity, &noteLength, message.LParam, WOW_HEX_DWORD_DIGITS);
            Wow32SetReturn(frame, 0);
            frame->CallbackProcedure   = message.LParam;
            frame->CallbackDataSelector     = dataSelector;
            frame->CallbackArguments[0] = 0;
            frame->CallbackArguments[1] = message.Message;
            frame->CallbackArguments[2] = message.WParam;
            frame->CallbackArguments[3] = (WORD)(message.Time >> WORD_SHIFT);
            frame->CallbackArguments[4] = (WORD)(message.Time & WORD_MASK);
            frame->CallbackArgumentCount   = WOWUSER_WNDPROC_ARGUMENTS;
            frame->CallbackReturnMode    = WOWCALL_RET_RESULT;
            frame->CallbackWindow   = 0;
            frame->CallbackMessage    = message.Message;
            return 1;
        }
        if (!message.Window) {
            WowNotePut(note, noteCapacity, &noteLength, " -- a thread message, nowhere to dispatch");
            Wow32SetReturn(frame, 0);
            return 1;
        }
        window = WowUserFindWindow(message.Window);
        if (!window) {
            WowNotePut(note, noteCapacity, &noteLength, " -- NO SUCH WINDOW");
            Wow32SetReturn(frame, 0);
            return 1;
        }
        /* ── ★★★ A WM_TIMER CARRYING A TIMERPROC GOES TO THE PROC, NOT THE
             WINDOW. That is Win16's rule (and Win32 kept it), and it is the
             ONLY place a TIMERPROC is ever called from -- see the timer table
             above. The signature is the same five words as a window procedure,
             `(hwnd, WM_TIMER, idTimer, dwTime)`, so the same call shape carries
             it; only the target address differs.
           ⚠ dwTime IS THE SYSTEM TIME THE MESSAGE WAS POSTED, not now: it is
             carried in the queued message, and a timer proc that measures
             elapsed time with it would drift if this substituted the current
             tick at dispatch. */
        if (message.Message == WM_TIMER16 && message.LParam) {
            if (!frame->IsCallbackAllowed) {
                WowNotePut(note, noteCapacity, &noteLength, " -- a TIMERPROC, but callbacks are"
                                           " not armed");
                Wow32SetReturn(frame, 0);
                return 1;
            }
            WowNotePut(note, noteCapacity, &noteLength, " -> its TIMERPROC 0x");
            WowNoteHex(note, noteCapacity, &noteLength, message.LParam, WOW_HEX_DWORD_DIGITS);
            Wow32SetReturn(frame, 0);
            WowUserWantMessage(frame, window, window->Instance ? window->Instance : g_WowUserClasses[window->Class].Instance,
                             message.Message, message.WParam, message.Time, WOWCALL_RET_RESULT);
            frame->CallbackProcedure = message.LParam;   /* ...but to the PROC, not w->wndproc */
            return 1;
        }
        if (WowUserWindowProcedureOf(window)) {
            if (!frame->IsCallbackAllowed) {
                WowNotePut(note, noteCapacity, &noteLength, " -- its own window procedure, but"
                                           " callbacks are not armed");
                Wow32SetReturn(frame, 0);
                return 1;
            }
            /* ★ OR ITS DIALOG PROCEDURE, which is what a MODELESS `#32770`
                 dialog is driven by -- the guest's own message loop takes the
                 message out of GetMessage and hands it back here, and until
                 session 57 this arm dropped it because the system class has no
                 window procedure. Same rule as SendMessage and the modal loop;
                 see WowUserWindowProcedureOf(). */
            WowNotePut(note, noteCapacity, &noteLength, window->WindowProcedure ? " -> its own window procedure"
                                                  : " -> its DIALOG procedure");
            Wow32SetReturn(frame, 0);
            WowUserWantMessage(frame, window, window->Instance ? window->Instance : g_WowUserClasses[window->Class].Instance,
                             message.Message, message.WParam, message.LParam, WOWCALL_RET_RESULT);
            /* ★ AND NOW THE RECORD CAN GO. `want_msg` has already captured the
                 procedure and instance into the pending call, so the slot is no
                 longer needed to make it -- and holding a destroyed window's
                 handle any longer is exactly the dangling reference the
                 DestroyWindow note warns about. */
            if (message.Message == WM_DESTROY16 && window->IsDying) {
                g_WowUserGone.Window = window->Window16; g_WowUserGone.DialogProcedure = window->DialogProcedure;
                window->Window16 = 0; window->IsDying = 0;
                WowNotePut(note, noteCapacity, &noteLength, " (and its record is now released)");
            }
            return 1;
        }
        WowNotePut(note, noteCapacity, &noteLength, " -> ");
        Wow32SetReturn(frame, (DWORD)WowUserDefProc(frame, window, message.Message, message.WParam, message.LParam,
                                               note + noteLength, noteCapacity - noteLength));
        return 1;
    }

    case WOWUSER_SCROLLDC: {
        WORD dc16 = Wow32ArgWord(frame, WOWUSER_SCRDC_ARG_HDC);
        INT  deltaX  = (INT)(SHORT)Wow32ArgWord(frame, WOWUSER_SCRDC_ARG_DX);
        INT  deltaY  = (INT)(SHORT)Wow32ArgWord(frame, WOWUSER_SCRDC_ARG_DY);
        const volatile BYTE *scrollBytes = Wow32ArgPointer(frame, WOWUSER_SCRDC_ARG_LPRCSCROLL);
        const volatile BYTE *rectBytes = Wow32ArgPointer(frame, WOWUSER_SCRDC_ARG_LPRCCLIP);
        volatile BYTE *updateBytes = Wow32ArgPointer(frame, WOWUSER_SCRDC_ARG_LPRCUPDATE);
        WORD region16 = Wow32ArgWord(frame, WOWUSER_SCRDC_ARG_HRGNUPDATE);
        HDC  dc   = (HDC)WowGdiH32(dc16, NULL);
        HRGN region  = (HRGN)WowGdiH32(region16, NULL);
        RECT scroll, clip, update;
        INT  noteLength = 0, isOk;
        WowNotePut(note, noteCapacity, &noteLength, "ScrollDC(dc 0x");
        WowNoteHex(note, noteCapacity, &noteLength, dc16, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, ", d=");
        WowNoteHex(note, noteCapacity, &noteLength, (DWORD)deltaX, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, ",");
        WowNoteHex(note, noteCapacity, &noteLength, (DWORD)deltaY, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, ")");
        if (!dc) {
            WowNotePut(note, noteCapacity, &noteLength, " -- ★ NOT ONE OF OUR DCs; answered FALSE");
            Wow32SetReturn(frame, 0);
            return 1;
        }
        /* ⚠ EIGHT BYTES IN, SIXTEEN OUT. WowConvRect16Get is the same reader
             the rest of this host uses, and it sign-extends -- a scroll
             rectangle with a negative top is ordinary. A NULL rectangle means
             "the whole DC" in both worlds and is passed through as NULL. */
        if (scrollBytes) { scroll.left   = WowConvRect16Get((PCBYTE)scrollBytes, 0);
                  scroll.top    = WowConvRect16Get((PCBYTE)scrollBytes, 1);
                  scroll.right  = WowConvRect16Get((PCBYTE)scrollBytes, 2);
                  scroll.bottom = WowConvRect16Get((PCBYTE)scrollBytes, 3); }
        if (rectBytes) { clip.left   = WowConvRect16Get((PCBYTE)rectBytes, 0);
                  clip.top    = WowConvRect16Get((PCBYTE)rectBytes, 1);
                  clip.right  = WowConvRect16Get((PCBYTE)rectBytes, 2);
                  clip.bottom = WowConvRect16Get((PCBYTE)rectBytes, 3); }
        isOk = ScrollDC(dc, deltaX, deltaY, scrollBytes ? &scroll : NULL, rectBytes ? &clip : NULL,
                      region, updateBytes ? &update : NULL) ? 1 : 0;
        if (isOk && updateBytes) {
            WowConvRect16Put((PBYTE)updateBytes, 0, (INT)update.left);
            WowConvRect16Put((PBYTE)updateBytes, 1, (INT)update.top);
            WowConvRect16Put((PBYTE)updateBytes, 2, (INT)update.right);
            WowConvRect16Put((PBYTE)updateBytes, 3, (INT)update.bottom);
            WowNotePut(note, noteCapacity, &noteLength, " update=");
            WowNoteHex(note, noteCapacity, &noteLength, (DWORD)(update.right - update.left), WOW_HEX_WORD_DIGITS);
            WowNotePut(note, noteCapacity, &noteLength, "x");
            WowNoteHex(note, noteCapacity, &noteLength, (DWORD)(update.bottom - update.top), WOW_HEX_WORD_DIGITS);
        }
        WowNotePut(note, noteCapacity, &noteLength, isOk ? " -> TRUE" : " -> FALSE (the OS refused)");
        Wow32SetReturn(frame, (DWORD)isOk);
        return 1;
    }

    case WOWUSER_CALCCHILDSCROLL: {
        WORD window16 = Wow32ArgWord(frame, WOWUSER_CCS_ARG_HWND);
        WORD scrollKind = Wow32ArgWord(frame, WOWUSER_CCS_ARG_SCROLL);
        INT  noteLength = 0;
        WowNotePut(note, noteCapacity, &noteLength, "CalcChildScroll(0x");
        WowNoteHex(note, noteCapacity, &noteLength, window16, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, ", ");
        WowNoteHex(note, noteCapacity, &noteLength, scrollKind, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, ") -- ★ NOTHING TO RECALCULATE: our MDICLIENT"
                                   " is the OS's own system class, so its scroll"
                                   " bars are Win32's and are already current."
                                   " This host keeps no MDI scroll state of its"
                                   " own to bring into line.");
        Wow32SetReturn(frame, 0);
        return 1;
    }

    /* ── ★★ 0x36 / 0x37 / 0xe1 -- THE WINDOW ENUMERATIONS. ───────────────────
         PROGMAN, WRITE, PACKAGER and TASKMAN all walk windows this way. The
         service ARMS the walk and the BOP handler makes the calls -- see
         src/wow/wowenum.h for why it cannot be done from here.
       ★ THE ANSWER IS WRITTEN IN ADVANCE. TRUE means "the whole list was
         walked", which is what happens unless the callback stops it, and the
         chain revises the hole to FALSE if it does. A caller therefore always
         reads a defined value, including when the host refuses the walk.
       ⚠ WHAT THE GUEST CAN SEE IS THE GUEST'S OWN WINDOWS. A Win16 program on
         real Windows would also see other Win16 programs'; this host runs one
         Win16 task at a time, and a Win32 window has no 16-bit handle to report
         it by. Named on the line rather than left as a silent short list. */
    case WOWUSER_ENUMWINDOWS:
    case WOWUSER_ENUMCHILDWINDOWS:
    case WOWUSER_ENUMTASKWINDOWS: {
        INT  which = (frame->Id == WOWUSER_ENUMWINDOWS)      ? WOWENUM_WINDOWS
                   : (frame->Id == WOWUSER_ENUMCHILDWINDOWS) ? WOWENUM_CHILDREN
                                                         : WOWENUM_TASK;
        DWORD procedure = (which == WOWENUM_WINDOWS) ? Wow32ArgDword(frame, WOWUSER_EW_ARG_PROC)
                                                : Wow32ArgDword(frame, WOWUSER_ECW_ARG_PROC);
        DWORD lParam   = (which == WOWENUM_WINDOWS) ? Wow32ArgDword(frame, WOWUSER_EW_ARG_LPARAM)
                                                : Wow32ArgDword(frame, WOWUSER_ECW_ARG_LPARAM);
        WORD  parent16 = (which == WOWENUM_CHILDREN) ? Wow32ArgWord(frame, WOWUSER_ECW_ARG_PARENT)
                                                   : 0;
        DWORD hole = (DWORD)(ULONG_PTR)(frame->FrameBase + WOW32_OFF_RET);
        PWOWUSER_WINDOW parentWindow = parent16 ? WowUserFindWindow(parent16) : NULL;
        INT noteLength = 0;
        WowNotePut(note, noteCapacity, &noteLength,
                which == WOWENUM_WINDOWS  ? "EnumWindows" :
                which == WOWENUM_CHILDREN ? "EnumChildWindows" :
                                            "EnumTaskWindows");
        WowNotePut(note, noteCapacity, &noteLength, " proc=0x");
        WowNoteHex(note, noteCapacity, &noteLength, procedure, WOW_HEX_DWORD_DIGITS);
        if (which == WOWENUM_CHILDREN) {
            WowNotePut(note, noteCapacity, &noteLength, " parent=0x");
            WowNoteHex(note, noteCapacity, &noteLength, parent16, WOW_HEX_WORD_DIGITS);
        }
        Wow32SetReturn(frame, 1);                /* TRUE unless a callback stops it */
        if (!frame->IsCallbackAllowed) {
            WowNotePut(note, noteCapacity, &noteLength, " -- callbacks are not armed; answered TRUE"
                                       " with nothing enumerated");
            return 1;
        }
        if (which == WOWENUM_CHILDREN && !parentWindow) {
            WowNotePut(note, noteCapacity, &noteLength, " -- ★ NO SUCH PARENT; answered TRUE with"
                                       " nothing enumerated");
            return 1;
        }
        if (WowEnumBusy()) {
            /* See the nesting note in wowenum.h: one cursor, and a second walk
               would inherit the first one's position. */
            WowNotePut(note, noteCapacity, &noteLength, " -- ★ AN ENUMERATION IS ALREADY RUNNING;"
                                       " REFUSED (answered TRUE, nothing walked)"
                                       " rather than sharing one cursor between"
                                       " two walks");
            return 1;
        }
        if (!WowEnumBegin(which, procedure,
                           parentWindow && parentWindow->Instance ? parentWindow->Instance : g_WowUserClasses[0].Instance,
                           lParam, hole, parent16)) {
            WowNotePut(note, noteCapacity, &noteLength, " -- ★ the callback is not a usable far"
                                       " pointer; nothing walked");
            return 1;
        }
        frame->IsEnumerationRequested = 1;
        /* s92 (#306): EnumTaskWindows walks THE TASK IT IS GIVEN. WinHelp, running
             its WM_WINHELP handler inside Calc's SendMessage, enumerated "its own"
             windows, was handed Calc's/Notepad's main window and sent it a
             WM_COMMAND meant for itself -- Notepad said "You have not entered any
             text to be saved". */
        g_WowUserEnumTask = (which == WOWENUM_TASK) ? Wow32ArgWord(frame, WOWUSER_ETW_ARG_TASK) : 0;
        if (g_WowUserEnumTask) {
            WowNotePut(note, noteCapacity, &noteLength, " task=0x");
            WowNoteHex(note, noteCapacity, &noteLength, g_WowUserEnumTask, WOW_HEX_WORD_DIGITS);
        }
        WowNotePut(note, noteCapacity, &noteLength, " -- walking this task's own top-level windows"
                                   " (a Win32 window has no 16-bit handle to"
                                   " report it by)");
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
        volatile BYTE *messageBytes = Wow32ArgPointer(frame, WOWMSG_TRANSLATEACCELERATOR_ARG_LPMSG);
        WOWMSG message;
        INT noteLength = 0;
        INT count = 0;
        WowNotePut(note, noteCapacity, &noteLength, "TranslateMessage msg=0x");
        if (messageBytes) { WowMsgRead(messageBytes, &message); WowNoteHex(note, noteCapacity, &noteLength, message.Message, WOW_HEX_WORD_DIGITS); }
        else      WowNotePut(note, noteCapacity, &noteLength, "?");
        /* s93: the characters Win32 already made for this key are HELD (wowwin.h,
           WowWinHoldChar) and released here -- the OS's own translation, with the
           keyboard state it keeps, delivered only when the program asks. */
        if (messageBytes && message.Message == WM_KEYDOWN) count = WowWinReleaseChars(message.Window, message.LParam);
        WowNotePut(note, noteCapacity, &noteLength, count ? " -> 1: the OS's WM_CHAR for this key released"
                                         " into the queue"
                                       : " -> 0: no character for this key");
        Wow32SetReturn(frame, count ? 1 : 0);
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
        INT isMdi = (frame->Id == WOWUSER_TRANSLATEMDISYS);
        INT noteLength = 0;
        WowNotePut(note, noteCapacity, &noteLength, isMdi ? "TranslateMDISysAccel hwnd=0x"
                                       : "TranslateAccelerator hwnd=0x");
        WowNoteHex(note, noteCapacity, &noteLength,
                  Wow32ArgWord(frame, isMdi ? WOWMSG_TRANSLATEMDISYSACCEL_ARG_HWND : WOWMSG_TRANSLATEACCELERATOR_ARG_HWND), WOW_HEX_WORD_DIGITS);
        if (!isMdi) {
            WowNotePut(note, noteCapacity, &noteLength, " hAccel=0x");
            WowNoteHex(note, noteCapacity, &noteLength, Wow32ArgWord(frame, WOWMSG_TRANSLATEACCELERATOR_ARG_HACCEL), WOW_HEX_WORD_DIGITS);
        }
        /* ── ★★★ AND NOW THERE IS ONE. (session 51) ─────────────────────────
             The user's report was "clicking the smiley does not reset the game";
             the same reset is Game > New, whose accelerator is F2, and the F2
             keystroke was reaching the guest and dying HERE -- this returned 0
             and the message went on to the window procedure as an ordinary key,
             which Minesweeper does not handle. WINMINE's ACCELERATOR 501 is two
             entries: VK_F1 -> 591 and VK_F2 -> 510.
           ⚠ MDI SYS ACCELERATORS ARE STILL 0, deliberately -- those are the
             OS's own (Ctrl+F4 and friends) against an MDI client, and nothing
             measured needs them. */
        if (!isMdi) {
            static WOWRES_ACCEL accelerators[WOWRES_MAX_ACCEL];
            static INT acceleratorCount = -1;  /* -1 = not looked for yet */
            static WORD acceleratorResource = 0;
            volatile BYTE *messageBytes = Wow32ArgPointer(frame, WOWMSG_TRANSLATEACCELERATOR_ARG_LPMSG);
            WOWMSG message;
            if (acceleratorCount < 0) {
                acceleratorCount = WowResOpen(WowUserResourceProgram())
                     ? WowResAccelFirst(accelerators, WOWRES_MAX_ACCEL, &acceleratorResource) : 0;
            }
            if (acceleratorCount > 0 && messageBytes) {
                /* ── #215: TWO KINDS OF ENTRY, MATCHED AGAINST TWO KINDS OF MESSAGE. ──
                     A VIRTKEY entry names a virtual key and is matched on WM_KEYDOWN /
                     WM_SYSKEYDOWN with its Shift/Ctrl/Alt bits. An entry WITHOUT the
                     VIRTKEY bit is an ASCII accelerator -- `"^C"` in an .RC file -- and
                     names a CHARACTER: it matches WM_CHAR (WM_SYSCHAR with Alt) whose
                     wParam is that code, the Shift/Ctrl already folded into it.
                     Only the first kind was handled, so Calc's Ctrl+C (0x03 -> 300) and
                     Ctrl+V (0x16 -> 301), and Paint's and Write's cut/copy/paste/undo,
                     all ASCII, never matched. The WM_CHAR is here to be matched because
                     the OS translated the key on our side and it was relayed verbatim.
                   ★ WM_COMMAND from an accelerator carries notify code 1 in lParam's
                     high word (0 is a menu); Win16 apps may tell the two apart. */
                WowMsgRead(messageBytes, &message);
                if (message.Message == WM_KEYDOWN16 || message.Message == WM_SYSKEYDOWN /* WM_SYSKEYDOWN */
                    || message.Message == WM_CHAR /* WM_CHAR */ || message.Message == WM_SYSCHAR /* WM_SYSCHAR */) {
                    INT isShift = (GetKeyState(VK_SHIFT)   & WOWUSER_KEY_DOWN) != 0;
                    INT isControl  = (GetKeyState(VK_CONTROL) & WOWUSER_KEY_DOWN) != 0;
                    INT isAlt   = (GetKeyState(VK_MENU)    & WOWUSER_KEY_DOWN) != 0;
                    INT isChar = (message.Message == WM_CHAR || message.Message == WM_SYSCHAR);
                    INT index;
                    if (message.Message == WM_SYSCHAR) isAlt = 1;
                    for (index = 0; index < acceleratorCount; ++index) {
                        INT isVirtualKey = (accelerators[index].Flags & WOWRES_ACCEL_VIRTKEY) != 0;
                        if (isVirtualKey == isChar) continue;   /* wrong kind of message */
                        if (accelerators[index].Key != message.WParam) continue;
                        if (isVirtualKey) {
                            if (!!(accelerators[index].Flags & WOWRES_ACCEL_SHIFT)   != isShift) continue;
                            if (!!(accelerators[index].Flags & WOWRES_ACCEL_CONTROL) != isControl)  continue;
                        }
                        if (!!(accelerators[index].Flags & WOWRES_ACCEL_ALT)     != isAlt)   continue;
                        /* ★ A MATCH IS A WM_COMMAND, and the caller's `or ax,ax /
                             jne` must see non-zero so it does NOT also translate
                             and dispatch the keystroke. */
                        WowMsgPost(Wow32ArgWord(frame, WOWMSG_TRANSLATEACCELERATOR_ARG_HWND), WM_COMMAND16,
                                    accelerators[index].Id, WOWUSER_COMMAND_FROM_ACCELERATOR, GetTickCount(), 0, 0);
                        WowNotePut(note, noteCapacity, &noteLength, " -> ACCELERATOR #");
                        WowNoteHex(note, noteCapacity, &noteLength, acceleratorResource, WOW_HEX_WORD_DIGITS);
                        WowNotePut(note, noteCapacity, &noteLength, isVirtualKey ? " matched vk 0x" : " matched char 0x");
                        WowNoteHex(note, noteCapacity, &noteLength, message.WParam, WOW_HEX_WORD_DIGITS);
                        WowNotePut(note, noteCapacity, &noteLength, " -> WM_COMMAND 0x");
                        WowNoteHex(note, noteCapacity, &noteLength, accelerators[index].Id, WOW_HEX_WORD_DIGITS);
                        Wow32SetReturn(frame, 1);
                        return 1;
                    }
                }
            }
            if (acceleratorCount <= 0)
                WowNotePut(note, noteCapacity, &noteLength, " -> 0 (this module has no ACCELERATOR"
                                           " resource)");
            else
                WowNotePut(note, noteCapacity, &noteLength, " -> 0 (no entry matched)");
            Wow32SetReturn(frame, 0);
            return 1;
        }
        WowNotePut(note, noteCapacity, &noteLength, " -> 0 (MDI system accelerators are the OS's,"
                                   " and nothing measured needs them)");
        Wow32SetReturn(frame, 0);
        return 1;
    }

    /* ── ★★★ 0x2a ShowWindow(hWnd, nCmdShow) / 0x7c UpdateWindow(hWnd) ────────
         Both go straight to the OS, because the window is the OS's. That is the
         whole difference between this and the version of this host that drew its
         own frames: there is nothing here to decide.
       ★ THE ARGUMENTS ARE CONFIRMED BY THE RUN: `(0x0005 0x0160)` -- the MDI
         client, from inside the frame's WM_CREATE -- and `(0x0001 0x0140)`, the
         frame window, from WinMain. So `+0` is nCmdShow and `+2` is hWnd.
       ★ AND THE `1` IS OUR OWN VALUE COMING BACK: WinMain's `nCmdShow` is what
         this host put in the WOW command structure (`WOWCMD_NCMDSHOW`), handed to
         the application at launch and handed straight back here.
       ⚠ nCmdShow IS PASSED THROUGH UNTRANSLATED, and that is a claim worth
         making explicitly: the SW_* values are the same in Win16 and Win32, as
         the WS_* bits are. If a run ever shows a window doing the wrong thing,
         this is the line to doubt. */
    case WOWUSER_SHOWWINDOW: {
        WORD window16 = Wow32ArgWord(frame, WOWUSER_SW_ARG_HWND);
        WORD command  = Wow32ArgWord(frame, WOWUSER_SW_ARG_CMDSHOW);
        PWOWUSER_WINDOW window = WowUserFindWindow(window16);
        INT noteLength = 0;
        if (window16 && window16 == g_WowUserInitDialogWindow) g_WowUserIsInitDialogShown = 1;  /* see CreateDialog */
        WowNotePut(note, noteCapacity, &noteLength, "ShowWindow 0x");
        WowNoteHex(note, noteCapacity, &noteLength, window16, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, " nCmdShow=0x");
        WowNoteHex(note, noteCapacity, &noteLength, command, WOW_HEX_WORD_DIGITS);
        if (!window) {
            WowNotePut(note, noteCapacity, &noteLength, " -- NO SUCH WINDOW");
            Wow32SetReturn(frame, 0);
            return 1;
        }
        if (!window->Window32) {
            WowNotePut(note, noteCapacity, &noteLength, " -- no real window behind it");
            Wow32SetReturn(frame, 0);
            return 1;
        }
        Wow32SetReturn(frame, ShowWindow(window->Window32, (INT)(SHORT)command) ? 1 : 0);
        WowNotePut(note, noteCapacity, &noteLength, " -> the OS's ShowWindow on HWND=0x");
        WowNoteHex(note, noteCapacity, &noteLength, (DWORD)(ULONG_PTR)window->Window32, WOW_HEX_DWORD_DIGITS);
        return 1;
    }

    case WOWUSER_UPDATEWINDOW: {
        WORD window16 = Wow32ArgWord(frame, WOWUSER_UW_ARG_HWND);
        PWOWUSER_WINDOW window = WowUserFindWindow(window16);
        INT noteLength = 0;
        WowNotePut(note, noteCapacity, &noteLength, "UpdateWindow 0x");
        WowNoteHex(note, noteCapacity, &noteLength, window16, WOW_HEX_WORD_DIGITS);
        if (window && window->Window32) { UpdateWindow(window->Window32);
                              WowNotePut(note, noteCapacity, &noteLength, " -> the OS's"); }
        else                  WowNotePut(note, noteCapacity, &noteLength, " -- no real window");
        Wow32SetReturn(frame, 0);
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
        INT isClipboard = (frame->Id == WOWUSER_REGCLIPFORMAT);
        CHAR name[WOWUSER_TEXT_SIZE];
        UINT format = 0;
        INT noteLength = 0;
        WowNotePut(note, noteCapacity, &noteLength, isClipboard ? "RegisterClipboardFormat "
                                          : "RegisterWindowMessage ");
        if (!Wow32ArgString(frame, WOWUSER_RCF_ARG_NAME, name, sizeof name) || !name[0]) {
            WowNotePut(note, noteCapacity, &noteLength, "-- no name, answered 0");
            Wow32SetReturn(frame, 0);
            return 1;
        }
        WowNoteQuoted(note, noteCapacity, &noteLength, name);
        format = isClipboard ? RegisterClipboardFormatA(name) : RegisterWindowMessageA(name);
        WowNotePut(note, noteCapacity, &noteLength, " -> 0x");
        WowNoteHex(note, noteCapacity, &noteLength, format, WOW_HEX_WORD_DIGITS);
        if (format > WORD_MASK) {
            WowNotePut(note, noteCapacity, &noteLength, " -- ★ THE OS RETURNED AN ID THAT DOES NOT"
                                       " FIT IN A WORD; answered 0 rather than a"
                                       " truncation");
            Wow32SetReturn(frame, 0);
            return 1;
        }
        Wow32SetReturn(frame, format);
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
        static BYTE header[WOWUSER_BITMAPINFOHEADER_SIZE + WOWUSER_MAX_PALETTE * WOWUSER_RGBQUAD_SIZE];     /* header + palette, our own copy */
        DWORD size = Wow32ArgDword(frame, WOWUSER_LBM_ARG_SIZE);
        volatile BYTE *bitmapBits = Wow32ArgPointer(frame, WOWUSER_LBM_ARG_BITS);
        CHAR name[WOWUSER_NAME_SIZE];
        DWORD headerSize, paletteSize, bitsOffset, index, width = 0, height = 0;
        INT   bitCount, noteLength = 0, isCore = 0;
        HDC   dc;
        HBITMAP bitmap;
        WORD  bitmap16;

        Wow32ArgString(frame, WOWUSER_LBM_ARG_NAME, name, sizeof name);
        WowNotePut(note, noteCapacity, &noteLength, "LoadBitmap ");
        WowNoteQuoted(note, noteCapacity, &noteLength, name);
        WowNotePut(note, noteCapacity, &noteLength, " size=0x");
        WowNoteHex(note, noteCapacity, &noteLength, size, WOW_HEX_WORD_DIGITS);

        /* ── s92 (#314): LoadBitmap(NULL, OBM_*) -- A PREDEFINED BITMAP. No resource
             bytes, and the name is an ORDINAL (selector 0). Media Player's SScrollBar
             loads its arrows this way in WM_CREATE; answered 0, it sized itself from an
             uninitialised BITMAP and came out 9250 pixels tall. The OBM_* ids are the
             same numbers in Win32, which keeps them for exactly this; the OS's own. */
        {   DWORD nameArgument = Wow32ArgDword(frame, WOWUSER_LBM_ARG_NAME);
            if (!size && !(nameArgument >> WORD_SHIFT) && (nameArgument & WORD_MASK)) {
                HBITMAP oldBitmap = LoadBitmapA(NULL, MAKEINTRESOURCEA(nameArgument & WORD_MASK));
                bitmap16 = oldBitmap ? WowGdiH16((HGDIOBJ)oldBitmap, WOWGDI_KIND_OBJ) : 0;
                WowNotePut(note, noteCapacity, &noteLength, " predefined #");
                WowNoteHex(note, noteCapacity, &noteLength, nameArgument & WORD_MASK, WOW_HEX_WORD_DIGITS);
                if (!bitmap16 && oldBitmap) DeleteObject((HGDIOBJ)oldBitmap);
                WowNotePut(note, noteCapacity, &noteLength, bitmap16 ? " -> the OS's bitmap, token 0x"
                                               : " -- ★ the OS has no such bitmap; 0");
                if (bitmap16) WowNoteHex(note, noteCapacity, &noteLength, bitmap16, WOW_HEX_WORD_DIGITS);
                Wow32SetReturn(frame, bitmap16);
                return 1;
            }
        }

        if (!bitmapBits || size < WOWUSER_BITMAPCOREHEADER_SIZE || size > WOWUSER_BITMAP_MAX_SIZE) {
            WowNotePut(note, noteCapacity, &noteLength, " -- ★ NO BYTES, or a length that cannot be"
                                       " a packed DIB; answered 0");
            Wow32SetReturn(frame, 0);
            return 1;
        }
        headerSize = (DWORD)Wow32PeekWord(bitmapBits) | ((DWORD)Wow32PeekWord(bitmapBits + WOW_WORD_BYTES) << WORD_SHIFT);

        /* ── ★★★ TWO DIB HEADERS EXIST, AND WINDOWS 3.x RESOURCES USE THE OLD
             ONE. This used to accept only `biSize == 40` (BITMAPINFOHEADER) and
             refuse everything else -- deliberately, because guessing at a format
             is worse than declining it. Solitaire named the missing one on its
             first run: every card face in SOL.EXE is a **BITMAPCOREHEADER**
             DIB, `biSize == 12`, the Windows 3.0 / OS-2 form. It got 0 back for
             all of them and put up **"Out of memory"**.
           ★ THE TWO DIFFER IN MORE THAN LENGTH, which is why this converts
             rather than casts:
               core: bcWidth/bcHeight are **unsigned 16-bit**, and the colour
                     table is **RGBTRIPLE** -- 3 bytes per entry.
               info: biWidth/biHeight are 32-bit, table is RGBQUAD -- 4 bytes.
             A cast would read the width as a 32-bit value spanning bcWidth and
             bcHeight, and walk the palette at the wrong stride. So the core form
             is unpacked field by field into a real BITMAPINFOHEADER below, and
             the palette is widened a triple at a time.
           ⚠ There is no `biClrUsed` in the core header: the table is always the
             full 2^bcBitCount entries for <= 8bpp, and absent above it. */
        if (headerSize == WOWUSER_BITMAPINFOHEADER_SIZE) {
            bitCount = (INT)Wow32PeekWord(bitmapBits + WOWUSER_BIH_BITCOUNT);    /* biBitCount     */
            paletteSize  = (DWORD)Wow32PeekWord(bitmapBits + WOWUSER_BIH_CLRUSED)  /* biClrUsed      */
                 | ((DWORD)Wow32PeekWord(bitmapBits + WOWUSER_BIH_CLRUSED + WOW_WORD_BYTES) << WORD_SHIFT);
            if (!paletteSize && bitCount <= WOWUSER_MAX_PALETTE_BITS) paletteSize = 1ul << bitCount;
            width = (DWORD)Wow32PeekWord(bitmapBits + WOWUSER_BIH_WIDTH);
            height = (DWORD)Wow32PeekWord(bitmapBits + WOWUSER_BIH_HEIGHT);
            bitsOffset = WOWUSER_BITMAPINFOHEADER_SIZE + paletteSize * WOWUSER_RGBQUAD_SIZE;
        } else if (headerSize == WOWUSER_BITMAPCOREHEADER_SIZE) {
            isCore = 1;
            width  = (DWORD)Wow32PeekWord(bitmapBits + WOWUSER_BCH_WIDTH);     /* bcWidth        */
            height  = (DWORD)Wow32PeekWord(bitmapBits + WOWUSER_BCH_HEIGHT);    /* bcHeight       */
            bitCount = (INT)Wow32PeekWord(bitmapBits + WOWUSER_BCH_BITCOUNT);    /* bcBitCount     */
            paletteSize  = (bitCount <= WOWUSER_MAX_PALETTE_BITS) ? (1ul << bitCount) : 0;
            bitsOffset  = WOWUSER_BITMAPCOREHEADER_SIZE + paletteSize * WOWUSER_RGBTRIPLE_SIZE;
        } else {
            WowNotePut(note, noteCapacity, &noteLength, " -- ★ biSize IS 0x");
            WowNoteHex(note, noteCapacity, &noteLength, headerSize, WOW_HEX_WORD_DIGITS);
            WowNotePut(note, noteCapacity, &noteLength, ", neither 40 (BITMAPINFOHEADER) nor 12"
                                       " (BITMAPCOREHEADER). Refused rather than"
                                       " guessed at; answered 0");
            Wow32SetReturn(frame, 0);
            return 1;
        }

        WowNotePut(note, noteCapacity, &noteLength, isCore ? " CORE " : " ");
        WowNoteHex(note, noteCapacity, &noteLength, width, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, "x");
        WowNoteHex(note, noteCapacity, &noteLength, height, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, " ");
        WowNoteHex(note, noteCapacity, &noteLength, (DWORD)bitCount, WOW_HEX_BYTE_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, "bpp pal=0x");
        WowNoteHex(note, noteCapacity, &noteLength, paletteSize, WOW_HEX_WORD_DIGITS);

        if (paletteSize > WOWUSER_MAX_PALETTE || bitsOffset >= size) {
            WowNotePut(note, noteCapacity, &noteLength, " -- ★ THE HEADER DOES NOT ADD UP (pixels"
                                       " would start at 0x");
            WowNoteHex(note, noteCapacity, &noteLength, bitsOffset, WOW_HEX_WORD_DIGITS);
            WowNotePut(note, noteCapacity, &noteLength, " in 0x");
            WowNoteHex(note, noteCapacity, &noteLength, size, WOW_HEX_WORD_DIGITS);
            WowNotePut(note, noteCapacity, &noteLength, " bytes); answered 0");
            Wow32SetReturn(frame, 0);
            return 1;
        }

        if (!isCore) {
            for (index = 0; index < bitsOffset; ++index) header[index] = bitmapBits[index];
            header[WOWUSER_BIH_SIZEIMAGE] = header[WOWUSER_BIH_SIZEIMAGE + 1] = header[WOWUSER_BIH_SIZEIMAGE + 2] = header[WOWUSER_BIH_SIZEIMAGE + 3] = 0;  /* biSizeImage    */
        } else {
            /* ★ THE CONVERSION ITSELF LIVES IN wowconv.h AND IS TESTED THERE.
                 It is a pure function of bytes, so it is pinned off-VM by
                 tests/unit/wow_test.c -- the stride change (RGBTRIPLE ->
                 RGBQUAD) and the two UNSIGNED 16-bit dimensions are exactly the
                 details that a cast gets wrong and a screenshot cannot show.
               ⚠ THE GUEST'S BYTES ARE COPIED FIRST. wowconv takes plain memory
                 on purpose: the moment it touched a `volatile` guest pointer it
                 would stop being testable without a rig, which is the whole
                 point of the file. */
            static BYTE sourceBuffer[WOWUSER_BITMAPCOREHEADER_SIZE + WOWUSER_MAX_PALETTE * WOWUSER_RGBTRIPLE_SIZE];
            DWORD needed = WOWUSER_BITMAPCOREHEADER_SIZE + paletteSize * WOWUSER_RGBTRIPLE_SIZE, index2;
            if (needed > sizeof sourceBuffer) needed = sizeof sourceBuffer;
            for (index2 = 0; index2 < needed; ++index2) sourceBuffer[index2] = bitmapBits[index2];
            if (!WowConvDibCoreToInfo(sourceBuffer, needed + 1, header, sizeof header, NULL)) {
                WowNotePut(note, noteCapacity, &noteLength, " -- ★ THE CORE HEADER DID NOT CONVERT;"
                                           " answered 0");
                Wow32SetReturn(frame, 0);
                return 1;
            }
        }

        /* ⚠ A SCREEN DC, TAKEN AND GIVEN BACK HERE. CreateDIBitmap needs a DC to
             be compatible with, and this one is the host's own business -- it is
             never shown to the guest, so it takes no token. */
        dc = GetDC(NULL);
        if (!dc) {
            WowNotePut(note, noteCapacity, &noteLength, " -- ★ NO SCREEN DC; answered 0");
            Wow32SetReturn(frame, 0);
            return 1;
        }
        bitmap = CreateDIBitmap(dc, (const BITMAPINFOHEADER *)header, CBM_INIT,
                            (const VOID *)(PCBYTE)(bitmapBits + bitsOffset),
                            (const BITMAPINFO *)header, DIB_RGB_COLORS);
        ReleaseDC(NULL, dc);
        bitmap16 = bitmap ? WowGdiH16((HGDIOBJ)bitmap, WOWGDI_KIND_OBJ) : 0;
        if (!bitmap16) {
            if (bitmap) DeleteObject((HGDIOBJ)bitmap);
            WowNotePut(note, noteCapacity, &noteLength, bitmap ? " -- ★ THE GDI TOKEN MAP IS FULL; the"
                                            " bitmap was freed and 0 answered"
                                          : " -- ★ GDI REFUSED THE DIB;"
                                            " answered 0");
            Wow32SetReturn(frame, 0);
            return 1;
        }
        WowNotePut(note, noteCapacity, &noteLength, " -> bitmap token 0x");
        WowNoteHex(note, noteCapacity, &noteLength, bitmap16, WOW_HEX_WORD_DIGITS);
        Wow32SetReturn(frame, bitmap16);
        return 1;
    }

    /* ── ★★★★ 0xad LoadSystemObject -- see the long note above. ───────────────
         ⚠ ONLY THE KIND THAT HAS BEEN READ. `1` is what a NULL-instance
           LoadCursor/LoadIcon passes and it is the only call site any run has
           taken; kind 3 carries a module's own resource and needs its own site
           read rather than this one widened. Anything else falls through to the
           honest 0 and the log says which kind asked. */
    case WOWUSER_LOADSYSOBJ: {
        WORD kind = Wow32ArgWord(frame, WOWUSER_AD_ARG_KIND);
        WORD nameLow   = Wow32ArgWord(frame, WOWUSER_AD_ARG_NAMELO);
        WORD nameHigh   = Wow32ArgWord(frame, WOWUSER_AD_ARG_NAMEHI);
        WORD instance = Wow32ArgWord(frame, WOWUSER_AD_ARG_HINST);
        INT  isCursor = (kind == WOWUSER_AD_KIND_CURSOR);
        INT noteLength = 0, index;
        WowNotePut(note, noteCapacity, &noteLength, "LoadSystemObject kind=0x");
        WowNoteHex(note, noteCapacity, &noteLength, kind, WOW_HEX_WORD_DIGITS);
        /* ★ KIND 3 IS THE MODULE'S OWN RESOURCE, and it arrives with the same
             name fields at the same offsets -- the two call sites differ only in
             what they put in the middle. So an ordinal is enough to find the
             resource in the application's own file (see wowres.h), and the token
             carries the kind so RegisterClass knows which way to resolve it. */
        if (kind != WOWUSER_AD_KIND_CURSOR && kind != WOWUSER_AD_KIND_ICON) {
            WowNotePut(note, noteCapacity, &noteLength, " -- a kind no call site has been read for;"
                                       " answered 0");
            Wow32SetReturn(frame, 0);
            return 1;
        }
        /* s89 (#216): cursor-or-icon came in `kind`; predefined-or-module is the
           hInstance's to say (see WOWUSER_AD_KIND_CURSOR). From here `kind` is the
           token's meaning, as every resolver reads it. */
        WowNotePut(note, noteCapacity, &noteLength, isCursor ? " (cursor" : " (icon");
        if (instance) { WowNotePut(note, noteCapacity, &noteLength, " of module 0x");
                     WowNoteHex(note, noteCapacity, &noteLength, instance, WOW_HEX_WORD_DIGITS); }
        else         WowNotePut(note, noteCapacity, &noteLength, ", predefined");
        WowNotePut(note, noteCapacity, &noteLength, ")");
        kind = instance ? WOWUSER_AD_KIND_MODULERES : WOWUSER_AD_KIND_PREDEFINED;
        /* ── ★★★★★ A NAMED RESOURCE IS NOT AN EXOTIC CASE. (session 47) ──────
             This used to answer 0 here and say so, on the grounds that no run
             had shown one. One had -- MS Paint, every time, in silence: its icon
             group is `"PBRUSH"` and its seven cursors are `"FLOOD"`, `"CROSSH"`,
             `"PICK"`, `"TEXT"`, `"SIDEAROW"`, `"DUMMY"`, `"XDUMMY"`, all read
             out of its own resource table. **The window came up with the generic
             application icon and the pointer never changed shape**, which is
             exactly what "answered 0" looks like from the desktop and does not
             look like an error anywhere.
           ⚠ THIS IS THE THIRD TIME THE SAME GAP HAS BEEN FOUND. Session 45 hit
             it on MENUS (`MENU="PBrush2"`) and fixed `WowResFind` for menus
             only; the icon and cursor paths kept the integer-only lookup. **A
             fix that is not carried to every lookup of the same kind is half a
             fix**, so both are named here and both resolve through the same
             `WowResFindNamed`. */
        if (nameHigh) {
            CHAR name[WOWUSER_SHORT_NAME_SIZE];
            Wow32ArgString(frame, WOWUSER_AD_ARG_NAMELO, name, sizeof name);
            WowNotePut(note, noteCapacity, &noteLength, " name=");
            WowNoteQuoted(note, noteCapacity, &noteLength, name);
            if (!name[0]) {
                WowNotePut(note, noteCapacity, &noteLength, " -- ★ an unreadable name pointer;"
                                           " answered 0");
                Wow32SetReturn(frame, 0);
                return 1;
            }
            for (index = 0; index < g_WowUserSystemResourceCount; ++index)
                if (g_WowUserSystemResources[index].Kind == kind
                    && WowUserIsEqualNoCase(g_WowUserSystemResources[index].Name, name)) {
                    WowNotePut(note, noteCapacity, &noteLength, " -> 0x");
                    WowNoteHex(note, noteCapacity, &noteLength, g_WowUserSystemResources[index].Handle16, WOW_HEX_WORD_DIGITS);
                    WowNotePut(note, noteCapacity, &noteLength, " (already issued)");
                    if (isCursor && instance) WowUserSystemResourcePrime(&g_WowUserSystemResources[index], frame, note, noteCapacity, &noteLength);
                    Wow32SetReturn(frame, g_WowUserSystemResources[index].Handle16);
                    return 1;
                }
            if (g_WowUserSystemResourceCount >= WOWUSER_MAX_SYSRES) {
                WowNotePut(note, noteCapacity, &noteLength, " -- ★ NO TOKEN LEFT, answered 0");
                Wow32SetReturn(frame, 0);
                return 1;
            }
            index = g_WowUserSystemResourceCount++;
            g_WowUserSystemResources[index].Ordinal  = 0;
            g_WowUserSystemResources[index].Kind = kind;
            {   INT index2 = 0;
                while (name[index2] && index2 < (INT)sizeof g_WowUserSystemResources[index].Name - 1) {
                    g_WowUserSystemResources[index].Name[index2] = name[index2]; ++index2;
                }
                g_WowUserSystemResources[index].Name[index2] = 0;
            }
            g_WowUserSystemResources[index].Handle16 = (WORD)(WOWUSER_SYSRES_BASE + index * WOWUSER_SYSRES_STEP);
            g_WowUserSystemResources[index].Cursor = NULL;
            if (isCursor && instance) WowUserSystemResourcePrime(&g_WowUserSystemResources[index], frame, note, noteCapacity, &noteLength);
            WowNotePut(note, noteCapacity, &noteLength, " -> token 0x");
            WowNoteHex(note, noteCapacity, &noteLength, g_WowUserSystemResources[index].Handle16, WOW_HEX_WORD_DIGITS);
            WowNotePut(note, noteCapacity, &noteLength, "; the OS object is fetched when the guest"
                                       " says whether it is a cursor or an icon");
            Wow32SetReturn(frame, g_WowUserSystemResources[index].Handle16);
            return 1;
        }
        WowNotePut(note, noteCapacity, &noteLength, " ordinal=0x");
        WowNoteHex(note, noteCapacity, &noteLength, nameLow, WOW_HEX_WORD_DIGITS);
        /* One token per ordinal: the guest asks for IDC_ARROW in twenty classes
           and should get one answer, the way the OS gives one HCURSOR. */
        for (index = 0; index < g_WowUserSystemResourceCount; ++index)
            if (g_WowUserSystemResources[index].Ordinal == nameLow && g_WowUserSystemResources[index].Kind == kind
                && !g_WowUserSystemResources[index].Name[0]) {
                WowNotePut(note, noteCapacity, &noteLength, " -> 0x");
                WowNoteHex(note, noteCapacity, &noteLength, g_WowUserSystemResources[index].Handle16, WOW_HEX_WORD_DIGITS);
                WowNotePut(note, noteCapacity, &noteLength, " (already issued)");
                if (isCursor && instance) WowUserSystemResourcePrime(&g_WowUserSystemResources[index], frame, note, noteCapacity, &noteLength);
                Wow32SetReturn(frame, g_WowUserSystemResources[index].Handle16);
                return 1;
            }
        if (g_WowUserSystemResourceCount >= WOWUSER_MAX_SYSRES) {
            WowNotePut(note, noteCapacity, &noteLength, " -- ★ NO TOKEN LEFT, answered 0");
            Wow32SetReturn(frame, 0);
            return 1;
        }
        index = g_WowUserSystemResourceCount++;
        g_WowUserSystemResources[index].Ordinal  = nameLow;
        g_WowUserSystemResources[index].Kind = kind;
        g_WowUserSystemResources[index].Name[0] = 0;
        g_WowUserSystemResources[index].Handle16   = (WORD)(WOWUSER_SYSRES_BASE + index * WOWUSER_SYSRES_STEP);
        g_WowUserSystemResources[index].Cursor = NULL;
        if (isCursor && instance) WowUserSystemResourcePrime(&g_WowUserSystemResources[index], frame, note, noteCapacity, &noteLength);
        WowNotePut(note, noteCapacity, &noteLength, " -> token 0x");
        WowNoteHex(note, noteCapacity, &noteLength, g_WowUserSystemResources[index].Handle16, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, "; the OS object is fetched when the guest says"
                                   " whether it is a cursor or an icon");
        Wow32SetReturn(frame, g_WowUserSystemResources[index].Handle16);
        return 1;
    }

    /* ── ★ 0x25 SetWindowText(hWnd, lpString) -- straight to the OS. ───────────
         The caption belongs to the real window, so there is nothing here to keep;
         the copy in WOWUSER_WINDOW is updated only so the host's own log keeps
         saying which window is which. */
    case WOWUSER_SETWINDOWTEXT: {
        WORD window16 = Wow32ArgWord(frame, WOWUSER_SWT_ARG_HWND);
        PWOWUSER_WINDOW window = WowUserFindWindow(window16);
        CHAR text[WOWUSER_TEXT_SIZE];
        INT noteLength = 0, index;
        Wow32ArgString(frame, WOWUSER_SWT_ARG_TEXT, text, sizeof text);
        WowNotePut(note, noteCapacity, &noteLength, "SetWindowText 0x");
        WowNoteHex(note, noteCapacity, &noteLength, window16, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, " ");
        WowNoteQuoted(note, noteCapacity, &noteLength, text);
        if (!window) { WowNotePut(note, noteCapacity, &noteLength, " -- NO SUCH WINDOW");
                  Wow32SetReturn(frame, 0); return 1; }
        for (index = 0; index < (INT)sizeof window->Text - 1 && text[index]; ++index) window->Text[index] = text[index];
        window->Text[index] = 0;
        /* ── ★★ #302: IN Win16, SetWindowText IS SendMessage(WM_SETTEXT). A window
             with its own procedure sees its new text first. Measured on Sound
             Recorder (s89): its SButton class turns "#Rewind" into BITMAP REWIND
             in its WM_SETTEXT handler, and this call went straight to the OS, so
             the buttons drew their raw text. The program's own string pointer is
             passed -- it is guest memory already. Then the real window's text is
             set as before (USER's 16-bit DefWindowProc does not forward
             WM_SETTEXT here). One level deep: a handler that sets text again is
             not sent to a second time. */
        /* s90: the REAL text first. Win16's DefWindowProc stores the text DURING
           WM_SETTEXT, so a control that repaints from GetWindowText inside its handler
           sees the new text; set after the send, it painted the PREVIOUS one --
           Sound Recorder's status read "Stopped" while playing and "Playing" after. */
        if (window->Window32) { SetWindowTextA(window->Window32, text);
                         WowNotePut(note, noteCapacity, &noteLength, " -> the OS's"); }
        else             WowNotePut(note, noteCapacity, &noteLength, " -- no real window");
        {   static INT isSent = 0;
            WORD result16;
            DWORD textPointer16 = (DWORD)Wow32ArgWord(frame, WOWUSER_SWT_ARG_TEXT)
                       | ((DWORD)Wow32ArgWord(frame, WOWUSER_SWT_ARG_TEXT + WOW_WORD_BYTES) << WORD_SHIFT);
            if (!isSent && g_WowUserSend16 && window->WindowProcedure && textPointer16) {
                ++isSent;
                if (g_WowUserSend16(window16, WM_SETTEXT16, 0, textPointer16, &result16))
                    WowNotePut(note, noteCapacity, &noteLength, " -> WM_SETTEXT SENT to its procedure");
                --isSent;
            }
        }
        Wow32SetReturn(frame, 0);
        return 1;
    }

    /* ── 0x16 SetFocus(hWnd) ───────────────────────────────────────────────────
         Implemented because a keystroke has to be ADDRESSED, and Win16 addresses
         it to the focus window. SYSEDIT calls this once per MDI child it builds
         (as logged), so the target of a key is the guest's own
         decision rather than a choice this host makes.
       ⚠ An unknown handle is refused rather than recorded: focus on a window we
         never made would send every later key into nothing, silently. */
    case WOWUSER_SETFOCUS: {
        WORD window16 = Wow32ArgWord(frame, WOWMSG_SETFOCUS_ARG_HWND);
        WORD previousFocus = g_WowMsgFocus;
        PWOWUSER_WINDOW window = WowUserFindWindow(window16);
        INT noteLength = 0;
        WowNotePut(note, noteCapacity, &noteLength, "SetFocus 0x");
        WowNoteHex(note, noteCapacity, &noteLength, window16, WOW_HEX_WORD_DIGITS);
        if (window16 && !window) {
            WowNotePut(note, noteCapacity, &noteLength, " -- NO SUCH WINDOW, focus unchanged");
            Wow32SetReturn(frame, previousFocus);
            return 1;
        }
        g_WowMsgFocus = window16;
        /* ★ AND THE OS's FOCUS TOO. The Win16 handle decides where this host
             posts a keystroke, but the CARET belongs to the real control and only
             the real SetFocus creates one -- a window the OS has not focused is a
             window with no cursor blinking in it, however right our own table is. */
        if (window && window->Window32) SetFocus(window->Window32);
        ++g_WowWinSetFocusCount;                     /* s93: see WM_ACTIVATE in wowwin.h */
        g_WowWinSetFocusWindow = window ? window->Window32 : NULL;
        WowNotePut(note, noteCapacity, &noteLength, " (was 0x");
        WowNoteHex(note, noteCapacity, &noteLength, previousFocus, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, ") -- keyboard messages now go here");
        Wow32SetReturn(frame, previousFocus);
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
        WORD window16 = Wow32ArgWord(frame, WOWUSER_MW_ARG_HWND);
        INT  positionX  = (INT)(SHORT)Wow32ArgWord(frame, WOWUSER_MW_ARG_X);
        INT  positionY  = (INT)(SHORT)Wow32ArgWord(frame, WOWUSER_MW_ARG_Y);
        INT  width = (INT)(SHORT)Wow32ArgWord(frame, WOWUSER_MW_ARG_CX);
        INT  height = (INT)(SHORT)Wow32ArgWord(frame, WOWUSER_MW_ARG_CY);
        WORD repaint = Wow32ArgWord(frame, WOWUSER_MW_ARG_REPAINT);
        PWOWUSER_WINDOW window = WowUserFindWindow(window16);
        INT noteLength = 0;
        WowNotePut(note, noteCapacity, &noteLength, "MoveWindow 0x");
        WowNoteHex(note, noteCapacity, &noteLength, window16, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, " to ("); WowNoteHex(note, noteCapacity, &noteLength, (DWORD)positionX, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, ",");     WowNoteHex(note, noteCapacity, &noteLength, (DWORD)positionY, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, ") ");    WowNoteHex(note, noteCapacity, &noteLength, (DWORD)width, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, "x");     WowNoteHex(note, noteCapacity, &noteLength, (DWORD)height, WOW_HEX_WORD_DIGITS);
        if (!window) { WowNotePut(note, noteCapacity, &noteLength, " -- NO SUCH WINDOW");
                  Wow32SetReturn(frame, 0); return 1; }
        window->PositionX = positionX; window->PositionY = positionY; window->Width = width; window->Height = height;
        if (window->Window32) {
            MoveWindow(window->Window32, positionX, positionY, width, height, repaint ? TRUE : FALSE);
            WowNotePut(note, noteCapacity, &noteLength, " -> the OS's");
        } else {
            WowNotePut(note, noteCapacity, &noteLength, " -- no real window; recorded only");
        }
        Wow32SetReturn(frame, 1);
        return 1;
    }

    /* ── ★ 0x7d InvalidateRect(hWnd, lpRect, bErase) ─────────────────────────
         Notepad calls it on its EDIT control just before moving it. The rect is
         a FAR POINTER and NULL means "the whole client area" -- a real
         distinction, so a null pointer is passed through as NULL rather than
         turned into an empty rectangle, which would invalidate nothing.
       ⚠ THE Win16 RECT IS 8 BYTES, FOUR WORDS -- see WOWUSER_RECT16_SIZE. Handing
         the guest's 8 bytes to Win32 as a RECT would read four LONGs, i.e. this
         rectangle and eight bytes of whatever follows it. */
    case WOWUSER_INVALIDATERECT: {
        WORD window16 = Wow32ArgWord(frame, WOWUSER_IR_ARG_HWND);
        WORD erase   = Wow32ArgWord(frame, WOWUSER_IR_ARG_ERASE);
        volatile BYTE *rectBytes = Wow32ArgPointer(frame, WOWUSER_IR_ARG_RECT);
        PWOWUSER_WINDOW window = WowUserFindWindow(window16);
        RECT rect; INT noteLength = 0, hasRect = 0;
        WowNotePut(note, noteCapacity, &noteLength, "InvalidateRect 0x");
        WowNoteHex(note, noteCapacity, &noteLength, window16, WOW_HEX_WORD_DIGITS);
        if (rectBytes) {
            rect.left   = (LONG)(SHORT)Wow32PeekWord(rectBytes + 0);
            rect.top    = (LONG)(SHORT)Wow32PeekWord(rectBytes + WOWUSER_RECT16_TOP);
            rect.right  = (LONG)(SHORT)Wow32PeekWord(rectBytes + WOWUSER_RECT16_RIGHT);
            rect.bottom = (LONG)(SHORT)Wow32PeekWord(rectBytes + WOWUSER_RECT16_BOTTOM);
            hasRect = 1;
            WowNotePut(note, noteCapacity, &noteLength, " rect(");
            WowNoteHex(note, noteCapacity, &noteLength, (DWORD)rect.left, WOW_HEX_WORD_DIGITS);  WowNotePut(note, noteCapacity, &noteLength, ",");
            WowNoteHex(note, noteCapacity, &noteLength, (DWORD)rect.top, WOW_HEX_WORD_DIGITS);   WowNotePut(note, noteCapacity, &noteLength, ",");
            WowNoteHex(note, noteCapacity, &noteLength, (DWORD)rect.right, WOW_HEX_WORD_DIGITS); WowNotePut(note, noteCapacity, &noteLength, ",");
            WowNoteHex(note, noteCapacity, &noteLength, (DWORD)rect.bottom, WOW_HEX_WORD_DIGITS);
            WowNotePut(note, noteCapacity, &noteLength, ")");
        } else {
            WowNotePut(note, noteCapacity, &noteLength, " whole client area (lpRect NULL)");
        }
        WowNotePut(note, noteCapacity, &noteLength, erase ? " erase" : " no erase");
        if (!window || !window->Window32) { WowNotePut(note, noteCapacity, &noteLength, " -- no real window");
                                Wow32SetReturn(frame, 0); return 1; }
        InvalidateRect(window->Window32, hasRect ? &rect : NULL, erase ? TRUE : FALSE);
        Wow32SetReturn(frame, 1);
        return 1;
    }

    /* ── 0x7f ValidateRect(hWnd, lpRect) -- the other half of InvalidateRect:
         take an area OUT of the update region. A NULL lpRect means the whole
         client area, exactly as it does above. */
    case WOWUSER_VALIDATERECT: {
        WORD window16 = Wow32ArgWord(frame, WOWUSER_VR_ARG_HWND);
        volatile BYTE *rectBytes = Wow32ArgPointer(frame, WOWUSER_VR_ARG_RECT);
        PWOWUSER_WINDOW window = WowUserFindWindow(window16);
        RECT rect; INT noteLength = 0, hasRect = 0;
        WowNotePut(note, noteCapacity, &noteLength, "ValidateRect 0x");
        WowNoteHex(note, noteCapacity, &noteLength, window16, WOW_HEX_WORD_DIGITS);
        if (rectBytes) {
            rect.left   = (LONG)(SHORT)Wow32PeekWord(rectBytes + 0);
            rect.top    = (LONG)(SHORT)Wow32PeekWord(rectBytes + WOWUSER_RECT16_TOP);
            rect.right  = (LONG)(SHORT)Wow32PeekWord(rectBytes + WOWUSER_RECT16_RIGHT);
            rect.bottom = (LONG)(SHORT)Wow32PeekWord(rectBytes + WOWUSER_RECT16_BOTTOM);
            hasRect = 1;
        } else {
            WowNotePut(note, noteCapacity, &noteLength, " whole client area (lpRect NULL)");
        }
        if (!window || !window->Window32) {
            WowNotePut(note, noteCapacity, &noteLength, " -- no real window");
            Wow32SetReturn(frame, 0);
            return 1;
        }
        ValidateRect(window->Window32, hasRect ? &rect : NULL);
        Wow32SetReturn(frame, 1);
        return 1;
    }

    /* ── 0x7e InvalidateRgn(hWnd, hRgn, bErase). ─────────────────────────────
       ⚠ A NULL hRgn IS NOT AN ERROR: it means the whole client area, the same
         way a NULL lpRect does for InvalidateRect. Refusing it would leave a
         guest that asked for a full repaint with none. */
    case WOWUSER_INVALIDATERGN: {
        WORD window16 = Wow32ArgWord(frame, WOWUSER_IRG_ARG_HWND);
        WORD region16 = Wow32ArgWord(frame, WOWUSER_IRG_ARG_RGN);
        WORD erase   = Wow32ArgWord(frame, WOWUSER_IRG_ARG_ERASE);
        PWOWUSER_WINDOW window = WowUserFindWindow(window16);
        INT regionKind = -1, noteLength = 0;
        HGDIOBJ region = region16 ? WowGdiH32(region16, &regionKind) : NULL;
        WowNotePut(note, noteCapacity, &noteLength, "InvalidateRgn 0x");
        WowNoteHex(note, noteCapacity, &noteLength, window16, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, " rgn 0x");
        WowNoteHex(note, noteCapacity, &noteLength, region16, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, erase ? " erase" : " no erase");
        if (!window || !window->Window32) {
            WowNotePut(note, noteCapacity, &noteLength, " -- no real window");
            Wow32SetReturn(frame, 0);
            return 1;
        }
        if (region16 && !region) {
            WowNotePut(note, noteCapacity, &noteLength, " -- ★ NOT ONE OF OUR REGION TOKENS;"
                                       " answered 0 rather than invalidating"
                                       " everything, which is what NULL means");
            Wow32SetReturn(frame, 0);
            return 1;
        }
        InvalidateRgn(window->Window32, (HRGN)region, erase ? TRUE : FALSE);
        Wow32SetReturn(frame, 1);
        return 1;
    }

    /* ── 0x30 IsChild(hWndParent, hWnd) -- is one window a descendant of the
         other. Answered from the REAL windows, so it agrees with what the OS
         thinks rather than with our own parent field, which a reparent would
         leave stale. */
    case WOWUSER_ISCHILD: {
        WORD parent16 = Wow32ArgWord(frame, WOWUSER_ICH_ARG_PARENT), child16 = Wow32ArgWord(frame, WOWUSER_ICH_ARG_HWND);
        PWOWUSER_WINDOW parent = WowUserFindWindow(parent16), child = WowUserFindWindow(child16);
        INT noteLength = 0, result = 0;
        if (parent && child && parent->Window32 && child->Window32)
            result = IsChild(parent->Window32, child->Window32) ? 1 : 0;
        WowNotePut(note, noteCapacity, &noteLength, "IsChild(0x");
        WowNoteHex(note, noteCapacity, &noteLength, parent16, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, ", 0x");
        WowNoteHex(note, noteCapacity, &noteLength, child16, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, result ? ") -> TRUE" : ") -> FALSE");
        if (!parent || !child) WowNotePut(note, noteCapacity, &noteLength, " (one of them is not one of ours)");
        Wow32SetReturn(frame, (DWORD)result);
        return 1;
    }

    /* ── 0xe6 GetNextWindow(hWnd, wFlag) -- walk the sibling chain. Win16's
         wFlag is GW_HWNDNEXT(2)/GW_HWNDPREV(3), the same values Win32 uses, so
         it passes straight through to GetWindow.
       ⚠ THE ANSWER MUST BE TRANSLATED BACK. GetWindow hands us a real HWND and
         the guest can only hold a 16-bit one; a window that is not ours has no
         16-bit handle and the honest answer is 0, not the raw pointer. */
    case WOWUSER_GETNEXTWINDOW: {
        WORD window16 = Wow32ArgWord(frame, WOWUSER_GNW_ARG_HWND), flags = Wow32ArgWord(frame, WOWUSER_GNW_ARG_FLAG);
        PWOWUSER_WINDOW window = WowUserFindWindow(window16);
        INT noteLength = 0;
        WORD result16 = 0;
        if (window && window->Window32) {
            HWND next32 = GetWindow(window->Window32, (UINT)flags);
            if (next32) result16 = WowWinHwnd16(next32);
        }
        WowNotePut(note, noteCapacity, &noteLength, "GetNextWindow(0x");
        WowNoteHex(note, noteCapacity, &noteLength, window16, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, ", flag ");
        WowNoteHex(note, noteCapacity, &noteLength, flags, WOW_HEX_BYTE_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, ") -> 0x");
        WowNoteHex(note, noteCapacity, &noteLength, result16, WOW_HEX_WORD_DIGITS);
        if (!result16) WowNotePut(note, noteCapacity, &noteLength, " (end of the chain, or a window"
                                             " that is not one of ours)");
        Wow32SetReturn(frame, result16);
        return 1;
    }

    /* ── 0xa2 HiliteMenuItem(hWnd, hMenu, idItem, uHilite). */
    case WOWUSER_HILITEMENUITEM: {
        WORD window16 = Wow32ArgWord(frame, WOWUSER_HMI_ARG_HWND), menu16 = Wow32ArgWord(frame, WOWUSER_HMI_ARG_MENU);
        WORD item = Wow32ArgWord(frame, WOWUSER_HMI_ARG_ITEM), flags = Wow32ArgWord(frame, WOWUSER_HMI_ARG_FLAGS);
        PWOWUSER_WINDOW window = WowUserFindWindow(window16);
        INT noteLength = 0, result = 0;
        WowNotePut(note, noteCapacity, &noteLength, "HiliteMenuItem(0x");
        WowNoteHex(note, noteCapacity, &noteLength, window16, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, ", item ");
        WowNoteHex(note, noteCapacity, &noteLength, item, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, ")");
        if (window && window->Window32) {
            HMENU menu = menu16 ? (HMENU)(ULONG_PTR)menu16 : GetMenu(window->Window32);
            if (menu) result = HiliteMenuItem(window->Window32, menu, (UINT)item, (UINT)flags) ? 1 : 0;
        }
        Wow32SetReturn(frame, (DWORD)result);
        return 1;
    }

    /* ── 0xa9 GetCaretBlinkTime / 0xc0 InSendMessage -- no arguments, and both
         are the OS's own answer rather than ours.
       ★ InSendMessage is asked by a window procedure that wants to know whether
         it is running inside a SEND (where the sender is blocked) or a POST. We
         relay the host thread's real state, which is the truth for the thread
         the guest's procedure is actually running on. */
    case WOWUSER_GETCARETBLINK: {
        INT noteLength = 0;
        UINT blinkTime = GetCaretBlinkTime();
        WowNotePut(note, noteCapacity, &noteLength, "GetCaretBlinkTime -> ");
        WowNoteHex(note, noteCapacity, &noteLength, (DWORD)blinkTime, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, " ms");
        Wow32SetReturn(frame, (DWORD)(WORD)blinkTime);
        return 1;
    }

    case WOWUSER_INSENDMESSAGE: {
        INT noteLength = 0;
        INT result = InSendMessage() ? 1 : 0;
        WowNotePut(note, noteCapacity, &noteLength, result ? "InSendMessage -> TRUE"
                                     : "InSendMessage -> FALSE");
        Wow32SetReturn(frame, (DWORD)result);
        return 1;
    }

    /* ── ★★★ 0x7a CallWindowProc(lpPrevWndFunc, hWnd, Msg, wParam, lParam) ───
         What a subclassing program calls to reach the procedure it displaced,
         and CARDFILE does not get off the ground without it.
       ★ THE PROCEDURE IS THE ARGUMENT, NOT THE WINDOW'S. That is the whole
         point of the call: after SetWindowLong(GWL_WNDPROC) the window's own
         procedure is the NEW one, and passing this to the window would call the
         subclass again -- an immediate infinite recursion rather than a wrong
         answer. So cbproc is set from the argument.
       ⚠ DS IS STILL THE WINDOW'S hInstance, because that is the entry contract
         in wowcall.h and the displaced procedure belongs to the same module.
       ⚠ GATED ON cbok like every other route into 16-bit code: with callbacks
         off this falls through to the honest "unimplemented" rather than
         silently returning 0, which a subclass would take for a real answer. */
    case WOWUSER_CALLWINDOWPROC: {
        DWORD procedure = Wow32ArgDword(frame, WOWUSER_CWP_ARG_PROC);
        WORD  window16 = Wow32ArgWord(frame, WOWUSER_CWP_ARG_HWND);
        WORD  message16  = Wow32ArgWord(frame, WOWUSER_CWP_ARG_MSG);
        WORD  wParam   = Wow32ArgWord(frame, WOWUSER_CWP_ARG_WPARAM);
        DWORD lParam   = Wow32ArgDword(frame, WOWUSER_CWP_ARG_LPARAM);
        PWOWUSER_WINDOW window;
        INT noteLength = 0;
        if (!frame->IsCallbackAllowed) return 0;
        window = WowUserFindWindow(window16);
        WowNotePut(note, noteCapacity, &noteLength, "CallWindowProc(proc 0x");
        WowNoteHex(note, noteCapacity, &noteLength, procedure, WOW_HEX_DWORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, ", hwnd 0x");
        WowNoteHex(note, noteCapacity, &noteLength, window16, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, ", msg 0x");
        WowNoteHex(note, noteCapacity, &noteLength, message16, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, ")");
        if (!procedure) {
            /* No procedure to call. DefWindowProc is the right default only
               when we know the window; otherwise say so rather than invent. */
            if (window) {
                WowNotePut(note, noteCapacity, &noteLength, " -- NULL proc; DefWindowProc");
                Wow32SetReturn(frame, (DWORD)WowUserDefProc(frame, window, message16, wParam, lParam,
                                                       note, noteCapacity));
            } else {
                WowNotePut(note, noteCapacity, &noteLength, " -- ★ NULL proc and no such window;"
                                           " answered 0");
                Wow32SetReturn(frame, 0);
            }
            return 1;
        }
        if (!window) {
            WowNotePut(note, noteCapacity, &noteLength, " -- ★ no such window; answered 0");
            Wow32SetReturn(frame, 0);
            return 1;
        }
        /* #308 (s91): ONE OF USER'S SYSTEM-CONTROL THUNKS -- a subclass chaining to
           the control it displaced (or calling the thunk directly, which lands here
           through USER's own code). The control's Win32 procedure answers: scalar
           messages straight to it, the rest through the same 16->32 translation
           SendMessage uses (WowUserDefProc) with WowUserSubclassProcedure stepped around, so
           the subclass is not called again for its own chain. */
        {   PCWOWUSER_SYSPROC systemProcedure = WowUserSystemProcedureAt(frame, procedure);
            if (systemProcedure) {
                HWND window32 = window->Window32;
                WNDPROC target = window->OriginalProcedure32;
                LRESULT result = 0;
                HWND savedBypass = g_WowUserSubclassBypass;
                ++g_WowUserSubclassChained;
                WowNotePut(note, noteCapacity, &noteLength, " -> USER's ");
                WowNotePut(note, noteCapacity, &noteLength, systemProcedure->ClassName);
                WowNotePut(note, noteCapacity, &noteLength, " thunk: the control's own procedure");
                if (!window32) { WowNotePut(note, noteCapacity, &noteLength, " -- no real control; 0");
                          Wow32SetReturn(frame, 0); return 1; }
                if (!target) target = (WNDPROC)GetWindowLongPtrA(window32, GWLP_WNDPROC);
                if (WowUserSubclassRelays(message16)) {
                    WPARAM wParam32 = wParam;
                    LPARAM lParam32 = (LPARAM)lParam;
                    if (message16 == WM_SETFOCUS || message16 == WM_KILLFOCUS || message16 == WM_SETCURSOR)
                        wParam32 = (WPARAM)WowUserHwnd32(wParam);
                    else if (message16 == WM_GETDLGCODE || message16 == WM_TIMER)
                        lParam32 = 0;
                    g_WowUserSubclassBypass = window32;
                    result = CallWindowProcA(target, window32, message16, wParam32, lParam32);
                    g_WowUserSubclassBypass = savedBypass;
                    Wow32SetReturn(frame, (message16 == WM_NCHITTEST) ? (DWORD)(WORD)(SHORT)result
                                                          : (DWORD)result);
                } else {
                    g_WowUserSubclassBypass = window32;
                    Wow32SetReturn(frame, (DWORD)WowUserDefProc(frame, window, message16, wParam, lParam,
                                                           note, noteCapacity));
                    g_WowUserSubclassBypass = savedBypass;
                }
                return 1;
            }
            if ((WORD)(procedure >> WORD_SHIFT) == frame->StubSegment)
                WowNotePut(note, noteCapacity, &noteLength, " -- ⚠ a USER address that is not a known"
                                           " control thunk; called as 16-bit code");
        }
        WowNotePut(note, noteCapacity, &noteLength, " -> the displaced 16-bit procedure");
        Wow32SetReturn(frame, 0);          /* overwritten by wowcall.h */
        WowUserWantMessage(frame, window, window->Instance ? window->Instance : g_WowUserClasses[window->Class].Instance,
                         message16, wParam, lParam, WOWCALL_RET_RESULT);
        frame->CallbackProcedure = procedure;       /* ★ the argument, not the window's */
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
        WORD metric = Wow32ArgWord(frame, WOWUSER_GSM_ARG_INDEX);
        INT  value   = GetSystemMetrics((INT)metric);
        INT  noteLength = 0;
        WowNotePut(note, noteCapacity, &noteLength, "GetSystemMetrics(0x");
        WowNoteHex(note, noteCapacity, &noteLength, metric, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, ") = 0x");
        WowNoteHex(note, noteCapacity, &noteLength, (DWORD)value, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, " (the OS's own, SM_* assumed common to Win16/32)");
        Wow32SetReturn(frame, (DWORD)(WORD)value);
        return 1;
    }

    /* ── ★ 0x1f IsIconic(hWnd) -- ask the real window. ───────────────────────
         Notepad asks before laying anything out, because a minimised window has
         no useful client area. Answering 0 unconditionally (what an unimplemented
         call did) is the "runs but lies" shape: it is the right answer most of the
         time, which is exactly why the wrong one would never be noticed. */
    case WOWUSER_ISICONIC: {
        WORD window16 = Wow32ArgWord(frame, WOWUSER_II_ARG_HWND);
        PWOWUSER_WINDOW window = WowUserFindWindow(window16);
        INT noteLength = 0, isIconic = 0;
        WowNotePut(note, noteCapacity, &noteLength, "IsIconic 0x");
        WowNoteHex(note, noteCapacity, &noteLength, window16, WOW_HEX_WORD_DIGITS);
        if (!window || !window->Window32) {
            WowNotePut(note, noteCapacity, &noteLength, " -- no real window; answered 0");
            Wow32SetReturn(frame, 0);
            return 1;
        }
        isIconic = IsIconic(window->Window32) ? 1 : 0;
        WowNotePut(note, noteCapacity, &noteLength, isIconic ? " -> MINIMISED" : " -> not minimised");
        Wow32SetReturn(frame, (DWORD)isIconic);
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
        WORD window16 = Wow32ArgWord(frame, WOWUSER_MSGB_ARG_HWND);
        WORD type = Wow32ArgWord(frame, WOWUSER_MSGB_ARG_TYPE);
        PWOWUSER_WINDOW window = window16 ? WowUserFindWindow(window16) : NULL;
        CHAR text[WOWUSER_LONG_TEXT_SIZE], caption[WOWUSER_TEXT_SIZE];
        INT noteLength = 0, result;
        Wow32ArgString(frame, WOWUSER_MSGB_ARG_TEXT,    text, sizeof text);
        Wow32ArgString(frame, WOWUSER_MSGB_ARG_CAPTION, caption,  sizeof caption);
        WowNotePut(note, noteCapacity, &noteLength, "★ MessageBox ");
        WowNoteQuoted(note, noteCapacity, &noteLength, caption);
        WowNotePut(note, noteCapacity, &noteLength, ": ");
        WowNoteQuoted(note, noteCapacity, &noteLength, text);
        WowNotePut(note, noteCapacity, &noteLength, " type=0x");
        WowNoteHex(note, noteCapacity, &noteLength, type, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, " -- ★ MODAL: the VDM stops until it is"
                                   " dismissed");
        result = MessageBoxA(window ? window->Window32 : NULL, text, caption, (UINT)type);
        WowNotePut(note, noteCapacity, &noteLength, "; answered 0x");
        WowNoteHex(note, noteCapacity, &noteLength, (DWORD)result, WOW_HEX_WORD_DIGITS);
        Wow32SetReturn(frame, (DWORD)(WORD)result);
        return 1;
    }

    /* ── ★ 0x51 FillRect(hDC, lprc, hbr) -- see the note above. ─────────────*/
    case WOWUSER_FILLRECT: {
        WORD dc16 = Wow32ArgWord(frame, WOWUSER_FR_ARG_HDC);
        WORD brush16 = Wow32ArgWord(frame, WOWUSER_FR_ARG_BRUSH);
        volatile BYTE *rectBytes = Wow32ArgPointer(frame, WOWUSER_FR_ARG_RECT);
        INT  dcKind = -1, brushKind = -1;
        HGDIOBJ dc = WowGdiH32(dc16, &dcKind);
        HGDIOBJ brush = WowGdiH32(brush16, &brushKind);
        RECT rect;
        INT  noteLength = 0, isOk;
        WowNotePut(note, noteCapacity, &noteLength, "FillRect(dc 0x");
        WowNoteHex(note, noteCapacity, &noteLength, dc16, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, ", brush 0x");
        WowNoteHex(note, noteCapacity, &noteLength, brush16, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, ")");
        if (!rectBytes || !dc || (dcKind != WOWGDI_KIND_DC && dcKind != WOWGDI_KIND_WINDC)) {
            WowNotePut(note, noteCapacity, &noteLength, " -- ★ NO RECT, or not one of our DC"
                                       " tokens; answered 0");
            Wow32SetReturn(frame, 0);
            return 1;
        }
        /* ⚠ A brush we cannot name is refused rather than substituted: filling
             with the wrong colour is worse than not filling, because it looks
             like it worked. */
        if (!brush || brushKind == WOWGDI_KIND_DC || brushKind == WOWGDI_KIND_WINDC) {
            WowNotePut(note, noteCapacity, &noteLength, " -- ★ NOT ONE OF OUR BRUSH TOKENS;"
                                       " answered 0");
            Wow32SetReturn(frame, 0);
            return 1;
        }
        rect.left   = (INT)(SHORT)Wow32PeekWord(rectBytes);
        rect.top    = (INT)(SHORT)Wow32PeekWord(rectBytes + WOWUSER_RECT16_TOP);
        rect.right  = (INT)(SHORT)Wow32PeekWord(rectBytes + WOWUSER_RECT16_RIGHT);
        rect.bottom = (INT)(SHORT)Wow32PeekWord(rectBytes + WOWUSER_RECT16_BOTTOM);
        WowNotePut(note, noteCapacity, &noteLength, " ");
        WowNoteHex(note, noteCapacity, &noteLength, (DWORD)rect.left, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, ",");
        WowNoteHex(note, noteCapacity, &noteLength, (DWORD)rect.top, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, " ");
        WowNoteHex(note, noteCapacity, &noteLength, (DWORD)(rect.right - rect.left), WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, "x");
        WowNoteHex(note, noteCapacity, &noteLength, (DWORD)(rect.bottom - rect.top), WOW_HEX_WORD_DIGITS);
        isOk = FillRect((HDC)dc, &rect, (HBRUSH)brush) ? 1 : 0;
        WowNotePut(note, noteCapacity, &noteLength, isOk ? " -> filled" : " -- ★ the OS refused it");
        Wow32SetReturn(frame, (DWORD)isOk);
        return 1;
    }

    /* ── ★★★★ THE DRAWING PATH: 0x1c, 0x20, 0x3c, 0x10 -- see the note above. */
    case WOWUSER_CLIENTTOSCREEN: {
        WORD window16 = Wow32ArgWord(frame, WOWUSER_C2S_ARG_HWND);
        volatile BYTE *pointBytes = Wow32ArgPointer(frame, WOWUSER_C2S_ARG_POINT);
        PWOWUSER_WINDOW window = WowUserFindWindow(window16);
        POINT point;
        INT noteLength = 0;
        WowNotePut(note, noteCapacity, &noteLength, "ClientToScreen 0x");
        WowNoteHex(note, noteCapacity, &noteLength, window16, WOW_HEX_WORD_DIGITS);
        if (!pointBytes || !window || !window->Window32) {
            WowNotePut(note, noteCapacity, &noteLength, " -- ★ NO WINDOW OR NO POINT; unchanged");
            Wow32SetReturn(frame, 0);
            return 1;
        }
        point.x = (INT)(SHORT)Wow32PeekWord(pointBytes);
        point.y = (INT)(SHORT)Wow32PeekWord(pointBytes + WOWUSER_POINT16_Y);
        ClientToScreen(window->Window32, &point);
        Wow32PokeWord(pointBytes,     (WORD)(SHORT)point.x);
        Wow32PokeWord(pointBytes + WOWUSER_POINT16_Y, (WORD)(SHORT)point.y);
        WowNotePut(note, noteCapacity, &noteLength, " -> ");
        WowNoteHex(note, noteCapacity, &noteLength, (DWORD)point.x, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, ",");
        WowNoteHex(note, noteCapacity, &noteLength, (DWORD)point.y, WOW_HEX_WORD_DIGITS);
        Wow32SetReturn(frame, 0);
        return 1;
    }

    case WOWUSER_GETWINDOWRECT: {
        WORD window16 = Wow32ArgWord(frame, WOWUSER_GWR_ARG_HWND);
        volatile BYTE *rectBytes = Wow32ArgPointer(frame, WOWUSER_GWR_ARG_RECT);
        PWOWUSER_WINDOW window = WowUserFindWindow(window16);
        RECT rect;
        INT noteLength = 0, index;
        WowNotePut(note, noteCapacity, &noteLength, "GetWindowRect 0x");
        WowNoteHex(note, noteCapacity, &noteLength, window16, WOW_HEX_WORD_DIGITS);
        if (!rectBytes) { WowNotePut(note, noteCapacity, &noteLength, " -- ★ NULL lpRect");
                  Wow32SetReturn(frame, 0); return 1; }
        if (!window || !window->Window32 || !GetWindowRect(window->Window32, &rect)) {
            for (index = 0; index < WOWUSER_RECT16_SIZE; ++index) rectBytes[index] = 0;
            WowNotePut(note, noteCapacity, &noteLength, " -- ★ NO SUCH WINDOW; zeroed");
            Wow32SetReturn(frame, 0);
            return 1;
        }
        Wow32PokeWord(rectBytes + 0, (WORD)(SHORT)rect.left);
        Wow32PokeWord(rectBytes + WOWUSER_RECT16_TOP, (WORD)(SHORT)rect.top);
        Wow32PokeWord(rectBytes + WOWUSER_RECT16_RIGHT, (WORD)(SHORT)rect.right);
        Wow32PokeWord(rectBytes + WOWUSER_RECT16_BOTTOM, (WORD)(SHORT)rect.bottom);
        WowNotePut(note, noteCapacity, &noteLength, " -> ");
        WowNoteHex(note, noteCapacity, &noteLength, (DWORD)rect.left, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, ",");
        WowNoteHex(note, noteCapacity, &noteLength, (DWORD)rect.top, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, " ");
        WowNoteHex(note, noteCapacity, &noteLength, (DWORD)(rect.right - rect.left), WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, "x");
        WowNoteHex(note, noteCapacity, &noteLength, (DWORD)(rect.bottom - rect.top), WOW_HEX_WORD_DIGITS);
        Wow32SetReturn(frame, 0);
        return 1;
    }

    case WOWUSER_GETACTIVEWINDOW: {
        HWND active32 = GetActiveWindow();
        WORD window16Result = active32 ? WowWinHwnd16(active32) : 0;
        INT  noteLength = 0;
        WowNotePut(note, noteCapacity, &noteLength, "GetActiveWindow -> 0x");
        WowNoteHex(note, noteCapacity, &noteLength, window16Result, WOW_HEX_WORD_DIGITS);
        if (active32 && !window16Result)
            WowNotePut(note, noteCapacity, &noteLength, " (active window is not the guest's)");
        Wow32SetReturn(frame, (DWORD)window16Result);
        return 1;
    }

    /* ⚠ ACCEPTED AND NOT APPLIED -- see the note above for why a system-wide
         cursor clip must not outlive a VDM the harness kills at will. */
    case WOWUSER_CLIPCURSOR: {
        volatile BYTE *rectBytes = Wow32ArgPointer(frame, WOWUSER_CC_ARG_RECT);
        INT noteLength = 0;
        WowNotePut(note, noteCapacity, &noteLength, rectBytes ? "ClipCursor(rect)" : "ClipCursor(NULL)");
        WowNotePut(note, noteCapacity, &noteLength, " -- ★ ACCEPTED BUT NOT APPLIED ON PURPOSE:"
                                   " the clip is system-wide and this VDM is"
                                   " killed at will, which would leave the user's"
                                   " pointer penned on their own desktop");
        Wow32SetReturn(frame, 0);
        return 1;
    }

    /* ── ★★★★ 0x12 SetCapture / 0x13 ReleaseCapture -- see the note above. ──*/
    case WOWUSER_SETCAPTURE: {
        WORD window16 = Wow32ArgWord(frame, WOWUSER_CAP_ARG_HWND);
        PWOWUSER_WINDOW window = WowUserFindWindow(window16);
        HWND previousCapture32;
        INT  noteLength = 0;
        WowNotePut(note, noteCapacity, &noteLength, "SetCapture 0x");
        WowNoteHex(note, noteCapacity, &noteLength, window16, WOW_HEX_WORD_DIGITS);
        if (!window || !window->Window32) {
            WowNotePut(note, noteCapacity, &noteLength, " -- ★ NO SUCH WINDOW; answered 0");
            Wow32SetReturn(frame, 0);
            return 1;
        }
        previousCapture32 = SetCapture(window->Window32);
        WowNotePut(note, noteCapacity, &noteLength, " -> the OS's; previous 0x");
        WowNoteHex(note, noteCapacity, &noteLength, previousCapture32 ? WowWinHwnd16(previousCapture32) : 0, WOW_HEX_WORD_DIGITS);
        Wow32SetReturn(frame, (DWORD)(previousCapture32 ? WowWinHwnd16(previousCapture32) : 0));
        return 1;
    }

    case WOWUSER_RELEASECAPTURE: {
        INT noteLength = 0;
        INT isOk = ReleaseCapture() ? 1 : 0;
        WowNotePut(note, noteCapacity, &noteLength, isOk ? "ReleaseCapture -> released"
                                      : "ReleaseCapture -- ★ nobody held it");
        Wow32SetReturn(frame, (DWORD)isOk);
        return 1;
    }

    /* ── ★★★★★ 0x2f IsWindow / 0x31 IsWindowVisible -- see the note above. ──
       ⚠ THE OS IS ASKED, NOT OUR OWN TABLE. Our record says what we intended;
         the real window says what is true, and a guest that has hidden something
         through DefWindowProc or had it hidden by its parent must get the truth.
         A window we have no record of is genuinely not a window of the guest's,
         so that answers FALSE for both -- which is also the right answer for a
         handle it has already destroyed. */
    /* ⛔ #161: `case WOWUSER_ISWINDOW:` USED TO SIT HERE, and s53 inserted the
         ArrangeIconicWindows case between it and its body -- so every IsWindow was
         answered by ArrangeIconicWindows (0 for a non-minimised window). Paint's
         WM_SIZE handling re-lays out only when IsWindow(canvas) is TRUE,
         so its canvas kept the whole client, on top of the toolbox: clicks on the
         tools drew on the canvas and resizing moved nothing. The label now sits on
         the IsWindowVisible case below, which was always written for both ids. */
    /* ── ★★ TASKMAN: ARRANGE THE ICONS, AND SWITCH TO A TASK. ─────────────── */
    case WOWUSER_ARRANGEICONICWINDOWS: {
        WORD window16 = Wow32ArgWord(frame, WOWUSER_AIW_ARG_HWND);
        PWOWUSER_WINDOW window = WowUserFindWindow(window16);
        INT noteLength = 0; UINT result;
        WowNotePut(note, noteCapacity, &noteLength, "ArrangeIconicWindows 0x");
        WowNoteHex(note, noteCapacity, &noteLength, window16, WOW_HEX_WORD_DIGITS);
        if (!window || !window->Window32) {
            WowNotePut(note, noteCapacity, &noteLength, " -- no real window; answered 0");
            Wow32SetReturn(frame, 0); return 1;
        }
        result = ArrangeIconicWindows(window->Window32);
        WowNotePut(note, noteCapacity, &noteLength, " -> row height "); WowNoteHex(note, noteCapacity, &noteLength, result, WOW_HEX_WORD_DIGITS);
        Wow32SetReturn(frame, (DWORD)result);
        return 1;
    }

    case WOWUSER_SWITCHTOTHISWINDOW: {
        WORD window16 = Wow32ArgWord(frame, WOWUSER_STW_ARG_HWND);
        WORD isAltTab  = Wow32ArgWord(frame, WOWUSER_STW_ARG_ALTTAB);
        PWOWUSER_WINDOW window = WowUserFindWindow(window16);
        INT noteLength = 0;
        WowNotePut(note, noteCapacity, &noteLength, "SwitchToThisWindow 0x");
        WowNoteHex(note, noteCapacity, &noteLength, window16, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, isAltTab ? " (alt-tab style)" : "");
        if (!window || !window->Window32) {
            WowNotePut(note, noteCapacity, &noteLength, " -- no real window; nothing to switch to");
            Wow32SetReturn(frame, 0); return 1;
        }
        /* ⚠ Restore BEFORE raising: a minimised window is still WS_VISIBLE, and
             SetForegroundWindow on one leaves an icon in front. Same trap
             rigshot hit (SW_RESTORE, not SW_SHOW). */
        if (IsIconic(window->Window32)) ShowWindow(window->Window32, SW_RESTORE);
        SetForegroundWindow(window->Window32);
        WowNotePut(note, noteCapacity, &noteLength, " -> restored + foregrounded");
        Wow32SetReturn(frame, 0);
        return 1;
    }

    /* ── ★★ CLOCK: BASE UNITS, ASYNC KEYS, IsZoomed, AND A MENU IT BUILDS ITSELF. */
    case WOWUSER_GETDIALOGBASEUNITS: {
        DWORD baseUnits = (DWORD)GetDialogBaseUnits();
        INT noteLength = 0;
        WowNotePut(note, noteCapacity, &noteLength, "GetDialogBaseUnits -> x=");
        WowNoteHex(note, noteCapacity, &noteLength, baseUnits & WORD_MASK, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, " y=");
        WowNoteHex(note, noteCapacity, &noteLength, (baseUnits >> WORD_SHIFT) & WORD_MASK, WOW_HEX_WORD_DIGITS);
        Wow32SetReturn(frame, baseUnits);
        return 1;
    }

    case WOWUSER_GETASYNCKEYSTATE: {
        WORD virtualKey = Wow32ArgWord(frame, WOWUSER_GAKS_ARG_VK);
        SHORT state = GetAsyncKeyState((INT)virtualKey);
        INT noteLength = 0;
        WowNotePut(note, noteCapacity, &noteLength, "GetAsyncKeyState vk=0x");
        WowNoteHex(note, noteCapacity, &noteLength, virtualKey, WOW_HEX_BYTE_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, (state & WOWUSER_KEY_DOWN) ? " -> DOWN" : " -> up");
        Wow32SetReturn(frame, (DWORD)(WORD)state);
        return 1;
    }

    case WOWUSER_ISZOOMED: {
        WORD window16 = Wow32ArgWord(frame, WOWUSER_IZ_ARG_HWND);
        PWOWUSER_WINDOW window = WowUserFindWindow(window16);
        INT noteLength = 0, isZoomed;
        WowNotePut(note, noteCapacity, &noteLength, "IsZoomed 0x");
        WowNoteHex(note, noteCapacity, &noteLength, window16, WOW_HEX_WORD_DIGITS);
        if (!window || !window->Window32) {
            WowNotePut(note, noteCapacity, &noteLength, " -- no real window; answered 0");
            Wow32SetReturn(frame, 0); return 1;
        }
        isZoomed = IsZoomed(window->Window32) ? 1 : 0;
        WowNotePut(note, noteCapacity, &noteLength, isZoomed ? " -> MAXIMISED" : " -> not maximised");
        Wow32SetReturn(frame, (DWORD)isZoomed);
        return 1;
    }

    case WOWUSER_APPENDMENU: {
        WORD menu16    = Wow32ArgWord(frame, WOWUSER_AM_ARG_HMENU);
        WORD flags = Wow32ArgWord(frame, WOWUSER_AM_ARG_FLAGS);
        WORD itemId    = Wow32ArgWord(frame, WOWUSER_AM_ARG_ID);
        HMENU menu    = WowUserMenu32(menu16);
        CHAR  text[WOWUSER_TEXT_SIZE];
        INT   noteLength = 0, isOk;
        WowNotePut(note, noteCapacity, &noteLength, "AppendMenu 0x");
        WowNoteHex(note, noteCapacity, &noteLength, menu16, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, " flags=0x"); WowNoteHex(note, noteCapacity, &noteLength, flags, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, " id=0x");    WowNoteHex(note, noteCapacity, &noteLength, itemId, WOW_HEX_WORD_DIGITS);
        if (!menu) {
            WowNotePut(note, noteCapacity, &noteLength, " -- ★ NOT ONE OF OUR MENUS; FALSE");
            Wow32SetReturn(frame, 0); return 1;
        }
        /* MF_SEPARATOR 0x800: the item pointer is meaningless and must not be
           read. MF_BITMAP/MF_OWNERDRAW would carry a handle rather than text and
           are refused rather than guessed at -- neither is in these guests. */
        if (flags & MF_SEPARATOR) {
            isOk = AppendMenuA(menu, MF_SEPARATOR, 0, NULL) ? 1 : 0;
            WowNotePut(note, noteCapacity, &noteLength, " [separator]");
        } else if (flags & (MF_BITMAP | MF_OWNERDRAW)) {   /* MF_BITMAP | MF_OWNERDRAW */
            WowNotePut(note, noteCapacity, &noteLength, " -- ★ BITMAP/OWNERDRAW item NOT SUPPORTED; FALSE");
            Wow32SetReturn(frame, 0); return 1;
        } else {
            text[0] = 0;
            Wow32ArgString(frame, WOWUSER_AM_ARG_ITEM, text, (INT)sizeof text);
            WowNotePut(note, noteCapacity, &noteLength, " \"");
            WowNotePut(note, noteCapacity, &noteLength, text);
            WowNotePut(note, noteCapacity, &noteLength, "\"");
            isOk = AppendMenuA(menu, (UINT)(flags & ~(UINT)MF_SEPARATOR), (UINT_PTR)itemId, text) ? 1 : 0;
        }
        WowNotePut(note, noteCapacity, &noteLength, isOk ? " -> appended" : " -> REFUSED by the OS");
        Wow32SetReturn(frame, (DWORD)isOk);
        return 1;
    }

    /* ── ★★ CALC: THE DIALOG HELPERS. ────────────────────────────────────────
         ⚠ IsDialogMessage DISPATCHES what it handles, into our own window procedure,
           which relays to the guest queue. For a message the dialog manager GENERATES
           that is right; for the one the guest just handed us it was a loop (#162) --
           see g_WowWinInDialogMessage in wowwin.h. */
    case WOWUSER_ISDIALOGMESSAGE: {
        WORD dialog16 = Wow32ArgWord(frame, WOWUSER_IDM_ARG_HDLG);
        volatile BYTE *messageBytes = Wow32ArgPointer(frame, WOWUSER_IDM_ARG_MSG);
        PWOWUSER_WINDOW window = WowUserFindWindow(dialog16);
        PWOWUSER_WINDOW messageWindow;
        MSG message32;
        INT noteLength = 0, result;
        WowNotePut(note, noteCapacity, &noteLength, "IsDialogMessage 0x");
        WowNoteHex(note, noteCapacity, &noteLength, dialog16, WOW_HEX_WORD_DIGITS);
        if (!window || !window->Window32 || !messageBytes) {
            WowNotePut(note, noteCapacity, &noteLength, " -- no window or no MSG; FALSE");
            Wow32SetReturn(frame, 0); return 1;
        }
        messageWindow = WowUserFindWindow(Wow32PeekWord(messageBytes + WOWMSG_FIELD_HWND));
        message32.hwnd    = messageWindow ? messageWindow->Window32 : window->Window32;
        message32.message = Wow32PeekWord(messageBytes + WOWMSG_FIELD_MESSAGE);
        message32.wParam  = Wow32PeekWord(messageBytes + WOWMSG_FIELD_WPARAM);
        message32.lParam  = (LPARAM)((DWORD)Wow32PeekWord(messageBytes + WOWMSG_FIELD_LPARAM)
                             | ((DWORD)Wow32PeekWord(messageBytes + WOWMSG_FIELD_LPARAM + WOW_WORD_BYTES) << WORD_SHIFT));
        message32.time    = GetTickCount();
        message32.pt.x = 0; message32.pt.y = 0;
        WowNotePut(note, noteCapacity, &noteLength, " msg=0x"); WowNoteHex(note, noteCapacity, &noteLength, message32.message, WOW_HEX_WORD_DIGITS);
        /* #162: see g_WowWinInDialogMessage in wowwin.h -- a bounce is answered FALSE. */
        g_WowWinInDialogMessage = 1; g_WowWinIsDialogBounced = 0;
        g_WowWinDialogWindow = message32.hwnd; g_WowWinDialogMessage = message32.message;
        result = IsDialogMessageA(window->Window32, &message32) ? 1 : 0;
        g_WowWinInDialogMessage = 0;
        if (g_WowWinIsDialogBounced) {
            result = 0;
            WowNotePut(note, noteCapacity, &noteLength, " -> FALSE (would have come straight back to the"
                                       " guest's queue; its own loop dispatches it)");
        } else
        WowNotePut(note, noteCapacity, &noteLength, result ? " -> TRUE (the dialog took it)" : " -> FALSE");
        Wow32SetReturn(frame, (DWORD)result);
        return 1;
    }

    case WOWUSER_MAPDIALOGRECT: {
        WORD dialog16 = Wow32ArgWord(frame, WOWUSER_MDR_ARG_HDLG);
        volatile BYTE *rectBytes = Wow32ArgPointer(frame, WOWUSER_MDR_ARG_RECT);
        PWOWUSER_WINDOW window = WowUserFindWindow(dialog16);
        BYTE rect16[WOWUSER_RECT16_SIZE];
        RECT rect;
        INT noteLength = 0, index;
        WowNotePut(note, noteCapacity, &noteLength, "MapDialogRect 0x");
        WowNoteHex(note, noteCapacity, &noteLength, dialog16, WOW_HEX_WORD_DIGITS);
        if (!window || !window->Window32 || !rectBytes) {
            WowNotePut(note, noteCapacity, &noteLength, " -- no window or no RECT; left alone");
            Wow32SetReturn(frame, 0); return 1;
        }
        for (index = 0; index < WOWUSER_RECT16_SIZE; ++index) rect16[index] = (BYTE)rectBytes[index];
        rect.left   = WowConvRect16Get(rect16, 0);
        rect.top    = WowConvRect16Get(rect16, 1);
        rect.right  = WowConvRect16Get(rect16, 2);
        rect.bottom = WowConvRect16Get(rect16, 3);
        /* ── #282: NOT WIN32's MapDialogRect. That only works on a window the OS
             built as a dialog, and none of ours is one -- every Win16 dialog here
             is CreateWindowEx'd (CALC's is its own `SciCalc` class) -- so it
             FAILED and left the rectangle in dialog units. Calc then drew its
             display's border from unconverted numbers: too small and in the wrong
             place. Convert with the base units the dialog builder laid the
             controls out with, so the guest's own drawing lands on them. */
        {   LONG baseUnits = GetDialogBaseUnits();
            INT  baseUnitX = window->DialogBaseUnitX ? (INT)window->DialogBaseUnitX : (INT)LOWORD(baseUnits);
            INT  baseUnitY = window->DialogBaseUnitY ? (INT)window->DialogBaseUnitY : (INT)HIWORD(baseUnits);
            rect.left   = MulDiv(rect.left, baseUnitX, WOWDLG_UNITS_PER_BASE_X);
            rect.right  = MulDiv(rect.right, baseUnitX, WOWDLG_UNITS_PER_BASE_X);
            rect.top    = MulDiv(rect.top, baseUnitY, WOWDLG_UNITS_PER_BASE_Y);
            rect.bottom = MulDiv(rect.bottom, baseUnitY, WOWDLG_UNITS_PER_BASE_Y); }
        WowConvRect16Put(rect16, 0, (INT)rect.left);
        WowConvRect16Put(rect16, 1, (INT)rect.top);
        WowConvRect16Put(rect16, 2, (INT)rect.right);
        WowConvRect16Put(rect16, 3, (INT)rect.bottom);
        for (index = 0; index < WOWUSER_RECT16_SIZE; ++index) rectBytes[index] = rect16[index];
        WowNotePut(note, noteCapacity, &noteLength, " -> ");
        WowNoteHex(note, noteCapacity, &noteLength, (DWORD)(rect.right - rect.left), WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, "x");
        WowNoteHex(note, noteCapacity, &noteLength, (DWORD)(rect.bottom - rect.top), WOW_HEX_WORD_DIGITS);
        Wow32SetReturn(frame, 0);
        return 1;
    }

    /* ── THE CLIPBOARD, WITH ONE VOICE. See the note by the ids (#160). ────── */
    case WOWUSER_ISCLIPBOARDFORMATAVAILABLE: {
        WORD format = Wow32ArgWord(frame, WOWUSER_CB_ARG_FORMAT);
        INT  noteLength = 0, canRead = (format == CF_TEXT16 || format == CF_OEMTEXT16)
                          && frame->IsCallbackAllowed && g_WowUserKernelSegment;
        INT  result = canRead && IsClipboardFormatAvailable(format) ? 1 : 0;
        WowNotePut(note, noteCapacity, &noteLength, "IsClipboardFormatAvailable fmt=0x");
        WowNoteHex(note, noteCapacity, &noteLength, format, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, result ? " -> 1 (the host's clipboard has it)"
                                  : canRead ? " -> 0 (not on the host's clipboard)"
                                        : " -> 0 (the bridge cannot deliver this"
                                          " format, so it is not offered)");
        Wow32SetReturn(frame, (DWORD)result);
        return 1;
    }
    case WOWUSER_GETCLIPBOARDDATA: {
        WORD format = Wow32ArgWord(frame, WOWUSER_CB_ARG_FORMAT);
        INT  noteLength = 0;
        HANDLE clipData;
        WowNotePut(note, noteCapacity, &noteLength, "GetClipboardData fmt=0x");
        WowNoteHex(note, noteCapacity, &noteLength, format, WOW_HEX_WORD_DIGITS);
        Wow32SetReturn(frame, 0);
        if (!(format == CF_TEXT16 || format == CF_OEMTEXT16) || !frame->IsCallbackAllowed || !g_WowUserKernelSegment) {
            WowNotePut(note, noteCapacity, &noteLength, " -> 0 (not a format the bridge carries)");
            return 1;
        }
        clipData = GetClipboardData(format);
        {   PCSTR text = clipData ? (PCSTR)GlobalLock(clipData) : NULL;
            INT count = 0;
            if (text) {
                while (count < (INT)sizeof g_WowUserClipboard - 1 && text[count]) { g_WowUserClipboard[count] = text[count]; ++count; }
                GlobalUnlock(clipData);
            }
            g_WowUserClipboard[count] = 0;
            g_WowUserClipboardLength = count;
            if (!text) {
                WowNotePut(note, noteCapacity, &noteLength, " -> 0 (the host's clipboard has no such"
                                           " data, or is not open)");
                return 1;
            }
        }
        frame->CallbackProcedure   = ((DWORD)g_WowUserKernelSegment << WORD_SHIFT) | WOWUSER_KRNL_GLOBALALLOC_OFF;
        frame->CallbackDataSelector     = frame->GuestDataSelector;
        frame->CallbackArguments[0] = GMEM_MOVEABLE_DDESHARE16;
        frame->CallbackArguments[1] = 0;                              /* dwBytes, high word */
        frame->CallbackArguments[2] = (WORD)(g_WowUserClipboardLength + 1);  /* ...and low        */
        frame->CallbackArgumentCount   = WOWUSER_GLOBALALLOC_ARGUMENTS;
        frame->CallbackReturnMode    = WOWCALL_RET_RESULTW;  /* the guest gets GlobalAlloc's handle */
        frame->CallbackSink   = NULL;
        frame->CallbackAction    = WOWCALL_ACT_CLIPLOCK;
        frame->CallbackActionArgument = format;
        WowNotePut(note, noteCapacity, &noteLength, " -- 0x");
        WowNoteHex(note, noteCapacity, &noteLength, (DWORD)g_WowUserClipboardLength, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, " byte(s) of host text; asking KERNEL.15 GlobalAlloc"
                                   " for the guest's block");
        return 1;
    }
    /* ★ The other direction. Text only, same reason; anything else is refused with 0,
         which a Win16 program reads as "the clipboard did not take it". A NULL hMem is
         delayed rendering, which would need WM_RENDERFORMAT sent back into the guest --
         not built, and refused rather than half-promised. */
    case WOWUSER_SETCLIPBOARDDATA: {
        WORD memory16 = Wow32ArgWord(frame, WOWUSER_SCD_ARG_HMEM);
        WORD format  = Wow32ArgWord(frame, WOWUSER_SCD_ARG_FORMAT);
        INT  noteLength = 0;
        WowNotePut(note, noteCapacity, &noteLength, "SetClipboardData fmt=0x");
        WowNoteHex(note, noteCapacity, &noteLength, format, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, " hMem=0x");
        WowNoteHex(note, noteCapacity, &noteLength, memory16, WOW_HEX_WORD_DIGITS);
        if ((format == CF_TEXT16 || format == CF_OEMTEXT16) && memory16 && frame->IsCallbackAllowed && g_WowUserKernelSegment) {
            frame->CallbackProcedure   = ((DWORD)g_WowUserKernelSegment << WORD_SHIFT) | WOWUSER_KRNL_GLOBALLOCK_OFF;
            frame->CallbackDataSelector     = frame->GuestDataSelector;
            frame->CallbackArguments[0] = memory16;
            frame->CallbackArgumentCount   = 1;
            frame->CallbackReturnMode    = WOWCALL_RET_KEEP;   /* the answer is hMem, set here */
            frame->CallbackSink   = NULL;
            frame->CallbackAction    = WOWCALL_ACT_CLIPPUT;
            frame->CallbackActionArgument = memory16;
            g_WowUserClipboardFormat = format;
            Wow32SetReturn(frame, memory16);
            WowNotePut(note, noteCapacity, &noteLength, " -- locking it (KERNEL.18) to copy the text"
                                       " to the host's clipboard");
        } else {
            Wow32SetReturn(frame, 0);
            WowNotePut(note, noteCapacity, &noteLength, memory16 ? " -> 0 (not a format the bridge carries)"
                                            : " -> 0 (delayed rendering is not supported)");
        }
        return 1;
    }

    /* ── BATCH TWO: the plain ones. Handle in, OS asked, handle out. ────────── */
    case WOWUSER_ISWINDOWENABLED:
    case WOWUSER_GETWINDOWTEXTLENGTH: {
        WORD window16 = Wow32ArgWord(frame, WOWUSER_W1_ARG_HWND);
        PWOWUSER_WINDOW window = WowUserFindWindow(window16);
        INT noteLength = 0; DWORD result;
        WowNotePut(note, noteCapacity, &noteLength, (frame->Id == WOWUSER_ISWINDOWENABLED)
                ? "IsWindowEnabled 0x" : "GetWindowTextLength 0x");
        WowNoteHex(note, noteCapacity, &noteLength, window16, WOW_HEX_WORD_DIGITS);
        if (!window || !window->Window32) { WowNotePut(note, noteCapacity, &noteLength, " -- no real window; 0");
                                Wow32SetReturn(frame, 0); return 1; }
        result = (frame->Id == WOWUSER_ISWINDOWENABLED)
            ? (DWORD)(IsWindowEnabled(window->Window32) ? 1 : 0)
            : (DWORD)GetWindowTextLengthA(window->Window32);
        WowNotePut(note, noteCapacity, &noteLength, " -> "); WowNoteHex(note, noteCapacity, &noteLength, result, WOW_HEX_WORD_DIGITS);
        Wow32SetReturn(frame, result);
        return 1;
    }

    /* ── ⛔ 0x24 GetWindowText(hWnd, lpString, nMaxCount) -- SOUND RECORDER'S BLANK
         LABELS. (user, s88: "Sound Recorder is half working: UI shows, but no text")
         USER.36 thunks to us (8 arg bytes) and was never implemented: SOUNDREC
         calls it for each of its controls and draws what it gets back -- nothing,
         ten times a run. Arguments as logged (reversed as always): +0 nMaxCount
         (0x80), +2/+4 lpString,
         +6 hWnd. The copy is bounded by the guest's own nMaxCount, NUL included. */
    case WOWUSER_GETWINDOWTEXT: {
        WORD window16 = Wow32ArgWord(frame, WOWUSER_GWT_ARG_HWND);
        WORD maximum  = Wow32ArgWord(frame, WOWUSER_GWT_ARG_MAX);
        volatile BYTE *destination = Wow32ArgPointer(frame, WOWUSER_GWT_ARG_BUF);
        PWOWUSER_WINDOW window = WowUserFindWindow(window16);
        CHAR buffer[WOWUSER_LONG_TEXT_SIZE];
        INT noteLength = 0, length = 0, index;
        WowNotePut(note, noteCapacity, &noteLength, "GetWindowText 0x");
        WowNoteHex(note, noteCapacity, &noteLength, window16, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, " max=0x");
        WowNoteHex(note, noteCapacity, &noteLength, maximum, WOW_HEX_WORD_DIGITS);
        if (!destination || !maximum) { WowNotePut(note, noteCapacity, &noteLength, " -- no buffer; 0");
                            Wow32SetReturn(frame, 0); return 1; }
        destination[0] = 0;
        if (!window || !window->Window32) { WowNotePut(note, noteCapacity, &noteLength, " -- no real window; 0");
                                Wow32SetReturn(frame, 0); return 1; }
        length = GetWindowTextA(window->Window32, buffer, (INT)sizeof buffer);
        if (length < 0) length = 0;
        if (length > (INT)maximum - 1) length = (INT)maximum - 1;
        for (index = 0; index < length; ++index) destination[index] = (BYTE)buffer[index];
        destination[length] = 0;
        buffer[length] = 0;
        WowNotePut(note, noteCapacity, &noteLength, " -> ");
        WowNoteQuoted(note, noteCapacity, &noteLength, buffer);
        Wow32SetReturn(frame, (DWORD)length);
        return 1;
    }

    case WOWUSER_GETCAPTURE: {
        HWND capture32 = GetCapture();
        WORD window16Result = capture32 ? WowWinHwnd16(capture32) : 0;
        INT noteLength = 0;
        WowNotePut(note, noteCapacity, &noteLength, "GetCapture -> 0x");
        WowNoteHex(note, noteCapacity, &noteLength, window16Result, WOW_HEX_WORD_DIGITS);
        /* ⚠ A capture held by a window that is not one of ours answers 0, which
             is what "nobody has it" looks like from inside the VDM -- the guest
             cannot be handed a handle from another address space. */
        if (capture32 && !window16Result) WowNotePut(note, noteCapacity, &noteLength, " (held OUTSIDE this VDM; 0)");
        Wow32SetReturn(frame, (DWORD)window16Result);
        return 1;
    }

    case WOWUSER_WINDOWFROMPOINT: {
        POINT point;
        HWND  window32;
        WORD  window16Result;
        INT   noteLength = 0;
        point.x = (INT)(SHORT)Wow32ArgWord(frame, WOWUSER_WFP_ARG_X);
        point.y = (INT)(SHORT)Wow32ArgWord(frame, WOWUSER_WFP_ARG_Y);
        window32   = WindowFromPoint(point);
        window16Result  = window32 ? WowWinHwnd16(window32) : 0;
        WowNotePut(note, noteCapacity, &noteLength, "WindowFromPoint ");
        WowNoteHex(note, noteCapacity, &noteLength, (DWORD)point.x, WOW_HEX_WORD_DIGITS); WowNotePut(note, noteCapacity, &noteLength, ",");
        WowNoteHex(note, noteCapacity, &noteLength, (DWORD)point.y, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, " -> 0x"); WowNoteHex(note, noteCapacity, &noteLength, window16Result, WOW_HEX_WORD_DIGITS);
        Wow32SetReturn(frame, (DWORD)window16Result);
        return 1;
    }

    /* ── s90 (#297) GetClipboardFormatName(fmt, buf, cch). Format numbers cross
         unchanged (RegisterClipboardFormat hands back the OS's own), so this is the
         OS's answer; a predefined format has no name and answers 0 in both. */
    case WOWUSER_GETCLIPBOARDFORMATNAME: {
        WORD format = Wow32ArgWord(frame, WOWUSER_GCFN_ARG_FMT);
        INT  capacity = (INT)(SHORT)Wow32ArgWord(frame, WOWUSER_GCFN_ARG_CCH);
        volatile BYTE *destination = Wow32ArgPointer(frame, WOWUSER_GCFN_ARG_BUF);
        CHAR name[WOWUSER_STRING_SIZE];
        INT  noteLength = 0, length = 0, index;
        WowNotePut(note, noteCapacity, &noteLength, "GetClipboardFormatName(0x");
        WowNoteHex(note, noteCapacity, &noteLength, format, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, ")");
        if (destination && capacity > 0) {
            length = GetClipboardFormatNameA(format, name, capacity < (INT)sizeof name ? capacity : (INT)sizeof name);
            if (length < 0) length = 0;
            for (index = 0; index < length; ++index) destination[index] = (BYTE)name[index];
            destination[length] = 0;
            if (length) { WowNotePut(note, noteCapacity, &noteLength, " -> \""); WowNotePut(note, noteCapacity, &noteLength, name);
                     WowNotePut(note, noteCapacity, &noteLength, "\""); }
        }
        if (!length) WowNotePut(note, noteCapacity, &noteLength, " -> 0 (no name)");
        Wow32SetReturn(frame, (DWORD)length);
        return 1;
    }

    /* ── s90 (#297) DlgDirSelect(hDlg, lpString, nIDListBox). The list box is a
         REAL one that DlgDirList filled, so the selection is the OS's to read --
         DlgDirSelectExA, which strips the brackets and appends `\` or `:` exactly
         as Win16's does. ⚠ Win16 passes no buffer size: its contract is a buffer
         big enough for a path, so at most 128 bytes are written (what Win16's own
         USER copies), never more. */
    case WOWUSER_DLGDIRSELECT: {
        WORD dialog16 = Wow32ArgWord(frame, WOWUSER_DDS_ARG_HDLG);
        WORD listId  = Wow32ArgWord(frame, WOWUSER_DDS_ARG_ID);
        volatile BYTE *destination = Wow32ArgPointer(frame, WOWUSER_DDS_ARG_STR);
        PWOWUSER_WINDOW window = WowUserFindWindow(dialog16);
        CHAR selection[WOWUSER_TEXT_SIZE];
        INT  noteLength = 0, result, index;
        WowNotePut(note, noteCapacity, &noteLength, "DlgDirSelect(0x");
        WowNoteHex(note, noteCapacity, &noteLength, dialog16, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, ", id=0x"); WowNoteHex(note, noteCapacity, &noteLength, listId, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, ")");
        if (!window || !window->Window32 || !destination) {
            WowNotePut(note, noteCapacity, &noteLength, " -- no real window or no buffer; 0");
            Wow32SetReturn(frame, 0); return 1;
        }
        selection[0] = 0;
        result = DlgDirSelectExA(window->Window32, selection, (INT)sizeof selection, listId) ? 1 : 0;
        for (index = 0; index < (INT)sizeof selection - 1 && selection[index]; ++index) destination[index] = (BYTE)selection[index];
        destination[index] = 0;
        WowNotePut(note, noteCapacity, &noteLength, " -> \""); WowNotePut(note, noteCapacity, &noteLength, selection);
        WowNotePut(note, noteCapacity, &noteLength, result ? "\" (directory or drive)" : "\" (file)");
        Wow32SetReturn(frame, (DWORD)result);
        return 1;
    }

    /* ── s90 (#297) SetParent(hwndChild, hwndNewParent) -> the previous parent.
         The real windows are re-parented by the OS, and the record's own `parent`
         follows, because GetParent answers from the record. */
    case WOWUSER_SETPARENT: {
        WORD child16 = Wow32ArgWord(frame, WOWUSER_SPA_ARG_CHILD);
        WORD newParent16 = Wow32ArgWord(frame, WOWUSER_SPA_ARG_NEW);
        PWOWUSER_WINDOW child = WowUserFindWindow(child16);
        PWOWUSER_WINDOW newParent = newParent16 ? WowUserFindWindow(newParent16) : NULL;
        WORD previousParent;
        INT  noteLength = 0;
        WowNotePut(note, noteCapacity, &noteLength, "SetParent(0x");
        WowNoteHex(note, noteCapacity, &noteLength, child16, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, ", 0x"); WowNoteHex(note, noteCapacity, &noteLength, newParent16, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, ")");
        if (!child || (newParent16 && !newParent)) {
            WowNotePut(note, noteCapacity, &noteLength, " -- ★ NOT ONE OF OUR WINDOWS; 0");
            Wow32SetReturn(frame, 0); return 1;
        }
        previousParent = child->Parent;
        if (child->Window32) SetParent(child->Window32, newParent ? newParent->Window32 : NULL);
        child->Parent = newParent16;
        WowNotePut(note, noteCapacity, &noteLength, " -> was 0x"); WowNoteHex(note, noteCapacity, &noteLength, previousParent, WOW_HEX_WORD_DIGITS);
        Wow32SetReturn(frame, previousParent);
        return 1;
    }

    /* ── s90 (#297) ChildWindowFromPoint(hwnd, POINT) -- client coordinates of
         hwnd; the parent itself when no child is there, 0 when outside it. The
         OS answers on the real windows (it does not skip hidden or disabled ones,
         and nor did Win16's), and the answer is translated back. */
    case WOWUSER_CHILDWINDOWFROMPOINT: {
        WORD  parent16 = Wow32ArgWord(frame, WOWUSER_CWFP_ARG_HWND);
        POINT point;
        PWOWUSER_WINDOW window = WowUserFindWindow(parent16);
        HWND  result;
        WORD  window16Result = 0;
        INT   noteLength = 0;
        point.x = (INT)(SHORT)Wow32ArgWord(frame, WOWUSER_CWFP_ARG_X);
        point.y = (INT)(SHORT)Wow32ArgWord(frame, WOWUSER_CWFP_ARG_Y);
        WowNotePut(note, noteCapacity, &noteLength, "ChildWindowFromPoint(0x");
        WowNoteHex(note, noteCapacity, &noteLength, parent16, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, ", "); WowNoteHex(note, noteCapacity, &noteLength, (DWORD)point.x, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, ","); WowNoteHex(note, noteCapacity, &noteLength, (DWORD)point.y, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, ")");
        if (window && window->Window32) {
            result = ChildWindowFromPoint(window->Window32, point);
            window16Result = !result ? 0 : (result == window->Window32 ? parent16 : WowWinHwnd16(result));
        }
        WowNotePut(note, noteCapacity, &noteLength, " -> 0x"); WowNoteHex(note, noteCapacity, &noteLength, window16Result, WOW_HEX_WORD_DIGITS);
        Wow32SetReturn(frame, window16Result);
        return 1;
    }

    /* ── s90 (#297) CallMsgFilter(lpMsg, nCode) -> TRUE only if a WH_MSGFILTER /
         WH_SYSMSGFILTER hook processed the message. This host installs no Win16
         hooks yet (SetWindowsHook, #298), so there is no filter to call and FALSE
         is the true answer -- the same one Windows gives with no hook installed.
         ⚠ When #298 lands, this must call the guest's chain. */
    case WOWUSER_CALLMSGFILTER: {
        INT noteLength = 0;
        WowNotePut(note, noteCapacity, &noteLength, "CallMsgFilter(code=0x");
        WowNoteHex(note, noteCapacity, &noteLength, Wow32ArgWord(frame, WOWUSER_CMF16_ARG_CODE), WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, ") -> FALSE (no message-filter hook installed)");
        Wow32SetReturn(frame, 0);
        return 1;
    }

    /* ── s90 (#296) EnumProps(hwnd, proc) -- proc(hwnd, lpszName, hData) once per
         property. MEASURED against stock (w_props), and three guesses were wrong:
         the order is OLDEST FIRST; a property set by ATOM still arrives as a STRING
         (the atom's name -- "NtvdmexGamma" -- never a null selector); and a window
         with no properties answers 0, not the documented -1. */
    case WOWUSER_ENUMPROPS: {
        WORD  window16 = Wow32ArgWord(frame, WOWUSER_EPR_ARG_HWND);
        DWORD procedure = Wow32ArgDword(frame, WOWUSER_EPR_ARG_PROC);
        INT   noteLength = 0, index, index2;
        g_WowEnumFontCount = 0;
        for (index = 0; index < g_WowUserPropCount && g_WowEnumFontCount < WOWENUM_MAXFONT; ++index) {
            const WOWUSER_PROP *prop = &g_WowUserProps[index];
            PBYTE nameBytes;
            if (prop->Window != window16 || !prop->Name[0]) continue;
            nameBytes = g_WowEnumFonts[g_WowEnumFontCount].Blob;
            if (prop->Name[0] == '#' && prop->Name[WOWUSER_ATOM_NAME_LENGTH] == 0) {  /* "#xxxx" = an atom */
                WORD atom = 0;
                for (index2 = 1; index2 < WOWUSER_ATOM_NAME_LENGTH; ++index2) {
                    CHAR nameChar = prop->Name[index2];
                    atom = (WORD)((atom << WOW_HEX_DIGIT_BITS) | (nameChar >= 'a' ? nameChar - 'a' + WOWUSER_HEX_LETTER_VALUE : nameChar - '0'));
                }
                CHAR atomName[WOWUSER_SHORT_NAME_SIZE];
                INT  length = (INT)GlobalGetAtomNameA((ATOM)atom, atomName, (INT)sizeof atomName);
                if (length <= 0) { for (index2 = 0; index2 < WOWUSER_ATOM_NAME_LENGTH; ++index2) atomName[index2] = prop->Name[index2]; length = WOWUSER_ATOM_NAME_LENGTH; }
                for (index2 = 0; index2 < length && index2 < WOWUSER_SHORT_NAME_SIZE - 1; ++index2) nameBytes[index2] = (BYTE)atomName[index2];
                nameBytes[index2] = 0;
            } else {
                for (index2 = 0; index2 < WOWUSER_SHORT_NAME_SIZE - 1 && prop->Name[index2]; ++index2) nameBytes[index2] = (BYTE)prop->Name[index2];
                nameBytes[index2] = 0;
            }
            g_WowEnumFonts[g_WowEnumFontCount].FontType = prop->Data;
            ++g_WowEnumFontCount;
        }
        WowNotePut(note, noteCapacity, &noteLength, "EnumProps(0x");
        WowNoteHex(note, noteCapacity, &noteLength, window16, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, ") -> 0x");
        WowNoteHex(note, noteCapacity, &noteLength, (DWORD)g_WowEnumFontCount, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, " propert(ies)");
        if (!g_WowEnumFontCount) { Wow32SetReturn(frame, 0); return 1; }
        Wow32SetReturn(frame, 1);             /* the walk revises it to 0 on a stop */
        if (!frame->IsCallbackAllowed) {
            WowNotePut(note, noteCapacity, &noteLength, " -- callbacks are not armed");
            return 1;
        }
        if (WowEnumBusy()) {
            WowNotePut(note, noteCapacity, &noteLength, " -- ★ AN ENUMERATION IS ALREADY RUNNING; REFUSED");
            Wow32SetReturn(frame, 0);
            return 1;
        }
        if (!WowEnumBegin(WOWENUM_PROPS, procedure, frame->GuestDataSelector, 0,
                           (DWORD)(ULONG_PTR)(frame->FrameBase + WOW32_OFF_RET), window16)) {
            WowNotePut(note, noteCapacity, &noteLength, " -- ★ the callback is not a usable far pointer");
            Wow32SetReturn(frame, 0);
            return 1;
        }
        frame->IsEnumerationRequested = 1;
        return 1;
    }

    case WOWUSER_GETINTERNALICONHEADER: {
        INT noteLength = 0;
        WowNotePut(note, noteCapacity, &noteLength, "GetInternalIconHeader -- 0, as stock (w_misc)");
        Wow32SetReturn(frame, 0);
        return 1;
    }

    /* ── s90 (#297) GetClassInfo(hInst, lpszClass, lpWndClass) -> BOOL, filling
         WNDCLASS16 (26 bytes): style, lpfnWndProc (16:16), cbClsExtra, cbWndExtra,
         hInstance, hIcon, hCursor, hbrBackground, lpszMenuName, lpszClassName.
         A program's class answers what it registered; the name pointers are the
         caller's own lpszClass for the name, and NULL for the menu (the string
         the program registered with is not kept at a guest address).
         ⚠ A SYSTEM CLASS IS REFUSED (FALSE): its procedure is the OS's, and there
           is no 16-bit address to hand a program that wants to superclass it --
           answering TRUE with a NULL procedure would crash the first
           CallWindowProc instead of failing here where the caller checks. */
    case WOWUSER_GETCLASSINFO: {
        DWORD namePointer = Wow32ArgDword(frame, WOWUSER_GCI_ARG_NAME);
        volatile BYTE *wndClass = Wow32ArgPointer(frame, WOWUSER_GCI_ARG_WC);
        PWOWUSER_CLASS windowClass = NULL;
        CHAR name[WOWUSER_NAME_SIZE];
        INT  noteLength = 0, index;
        name[0] = 0;
        WowUserEnsureSystemClasses();
        if (!(namePointer >> WORD_SHIFT)) windowClass = WowUserFindClassByAtom((WORD)namePointer);
        else if (Wow32ArgString(frame, WOWUSER_GCI_ARG_NAME, name, (INT)sizeof name)) windowClass = WowUserFindClass(name);
        WowNotePut(note, noteCapacity, &noteLength, "GetClassInfo(\"");
        WowNotePut(note, noteCapacity, &noteLength, windowClass ? windowClass->Name : name);
        WowNotePut(note, noteCapacity, &noteLength, "\")");
        if (!windowClass || !wndClass || windowClass->IsSystemClass) {
            WowNotePut(note, noteCapacity, &noteLength, !windowClass ? " -- no such class; FALSE"
                                   : !wndClass ? " -- no buffer; FALSE"
                                   : " -- a SYSTEM class: no 16-bit procedure to give; FALSE");
            Wow32SetReturn(frame, 0); return 1;
        }
        {
            WORD fields[WOWUSER_WNDCLASS16_WORDS];
            fields[0] = windowClass->Style;
            fields[1] = (WORD)(windowClass->WindowProcedure & WORD_MASK); fields[2] = (WORD)(windowClass->WindowProcedure >> WORD_SHIFT);
            fields[3] = windowClass->ClassExtra; fields[4] = windowClass->WindowExtra; fields[5] = windowClass->Instance;
            fields[6] = windowClass->Icon16; fields[7] = windowClass->Cursor16; fields[8] = windowClass->Background16;
            fields[9] = 0; fields[10] = 0;             /* lpszMenuName */
            fields[11] = (WORD)(namePointer & WORD_MASK); fields[12] = (WORD)(namePointer >> WORD_SHIFT);
            for (index = 0; index < WOWUSER_WNDCLASS16_WORDS; ++index) { wndClass[index * WOW_WORD_BYTES] = (BYTE)fields[index]; wndClass[index * WOW_WORD_BYTES + 1] = (BYTE)(fields[index] >> BYTE_SHIFT); }
        }
        WowNotePut(note, noteCapacity, &noteLength, " -> TRUE proc=");
        WowNoteHex(note, noteCapacity, &noteLength, windowClass->WindowProcedure, WOW_HEX_DWORD_DIGITS);
        Wow32SetReturn(frame, 1);
        return 1;
    }

    case WOWUSER_FLASHWINDOW: {
        WORD window16 = Wow32ArgWord(frame, WOWUSER_FW_ARG_HWND);
        WORD invert  = Wow32ArgWord(frame, WOWUSER_FW_ARG_INVERT);
        PWOWUSER_WINDOW window = WowUserFindWindow(window16);
        INT noteLength = 0, result;
        WowNotePut(note, noteCapacity, &noteLength, "FlashWindow 0x");
        WowNoteHex(note, noteCapacity, &noteLength, window16, WOW_HEX_WORD_DIGITS);
        if (!window || !window->Window32) { WowNotePut(note, noteCapacity, &noteLength, " -- no real window; 0");
                                Wow32SetReturn(frame, 0); return 1; }
        result = FlashWindow(window->Window32, invert ? TRUE : FALSE) ? 1 : 0;
        WowNotePut(note, noteCapacity, &noteLength, result ? " -> was active" : " -> was inactive");
        Wow32SetReturn(frame, (DWORD)result);
        return 1;
    }

    /* ── ⚠ THE KEY-STATE ARRAY IS 256 BYTES AND THE GUEST OWNS THE BUFFER. ─────
         Win16 and Win32 agree on the shape exactly: one byte per virtual key,
         high bit down, low bit toggled. So it is a copy, not a conversion -- but
         it is a copy of 256 bytes into guest memory, so the pointer is checked. */
    case WOWUSER_GETKEYBOARDSTATE:
    case WOWUSER_SETKEYBOARDSTATE: {
        volatile BYTE *bytes16 = Wow32ArgPointer(frame, WOWUSER_KS_ARG_BUF);
        BYTE keyState[WOWUSER_KEY_STATE_SIZE];
        INT  noteLength = 0, index, isGet = (frame->Id == WOWUSER_GETKEYBOARDSTATE);
        WowNotePut(note, noteCapacity, &noteLength, isGet ? "GetKeyboardState" : "SetKeyboardState");
        if (!bytes16) { WowNotePut(note, noteCapacity, &noteLength, " -- ★ NO BUFFER; nothing done");
                    Wow32SetReturn(frame, 0); return 1; }
        if (isGet) {
            if (!GetKeyboardState(keyState)) {
                WowNotePut(note, noteCapacity, &noteLength, " -- the OS refused; nothing written");
                Wow32SetReturn(frame, 0); return 1;
            }
            for (index = 0; index < WOWUSER_KEY_STATE_SIZE; ++index) bytes16[index] = keyState[index];
            WowNotePut(note, noteCapacity, &noteLength, " -> 256 bytes written");
        } else {
            for (index = 0; index < WOWUSER_KEY_STATE_SIZE; ++index) keyState[index] = (BYTE)bytes16[index];
            SetKeyboardState(keyState);
            WowNotePut(note, noteCapacity, &noteLength, " -> 256 bytes taken");
        }
        Wow32SetReturn(frame, 1);
        return 1;
    }

    case WOWUSER_VKKEYSCAN: {
        WORD character = Wow32ArgWord(frame, WOWUSER_VKS_ARG_CHAR);
        SHORT result = VkKeyScanA((CHAR)(character & BYTE_MASK));
        INT noteLength = 0;
        WowNotePut(note, noteCapacity, &noteLength, "VkKeyScan '");
        { CHAR pair[2]; pair[0] = (CHAR)(character & BYTE_MASK); pair[1] = 0;
          WowNotePut(note, noteCapacity, &noteLength, pair); }
        WowNotePut(note, noteCapacity, &noteLength, "' -> 0x"); WowNoteHex(note, noteCapacity, &noteLength, (WORD)result, WOW_HEX_WORD_DIGITS);
        Wow32SetReturn(frame, (DWORD)(WORD)result);
        return 1;
    }

    case WOWUSER_MAPVIRTUALKEY: {
        WORD code = Wow32ArgWord(frame, WOWUSER_MVK_ARG_CODE);
        WORD type = Wow32ArgWord(frame, WOWUSER_MVK_ARG_TYPE);
        UINT result = MapVirtualKeyA(code, type);
        INT noteLength = 0;
        WowNotePut(note, noteCapacity, &noteLength, "MapVirtualKey code=0x");
        WowNoteHex(note, noteCapacity, &noteLength, code, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, " type="); WowNoteHex(note, noteCapacity, &noteLength, type, WOW_HEX_BYTE_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, " -> 0x"); WowNoteHex(note, noteCapacity, &noteLength, result, WOW_HEX_WORD_DIGITS);
        Wow32SetReturn(frame, (DWORD)result);
        return 1;
    }

    case WOWUSER_EMPTYCLIPBOARD: {
        INT noteLength = 0, result = EmptyClipboard() ? 1 : 0;
        WowNotePut(note, noteCapacity, &noteLength, result ? "EmptyClipboard -> emptied"
                                     : "EmptyClipboard -- ★ REFUSED (not open?)");
        Wow32SetReturn(frame, (DWORD)result);
        return 1;
    }

    case WOWUSER_GETUPDATERECT: {
        WORD window16 = Wow32ArgWord(frame, WOWUSER_GUR_ARG_HWND);
        WORD erase   = Wow32ArgWord(frame, WOWUSER_GUR_ARG_ERASE);
        volatile BYTE *rectBytes = Wow32ArgPointer(frame, WOWUSER_GUR_ARG_RECT);
        PWOWUSER_WINDOW window = WowUserFindWindow(window16);
        BYTE rect16[WOWUSER_RECT16_SIZE];
        RECT rect;
        INT noteLength = 0, hasAny, index;
        WowNotePut(note, noteCapacity, &noteLength, "GetUpdateRect 0x");
        WowNoteHex(note, noteCapacity, &noteLength, window16, WOW_HEX_WORD_DIGITS);
        if (!window || !window->Window32) { WowNotePut(note, noteCapacity, &noteLength, " -- no real window; FALSE");
                                Wow32SetReturn(frame, 0); return 1; }
        hasAny = GetUpdateRect(window->Window32, &rect, erase ? TRUE : FALSE) ? 1 : 0;
        if (rectBytes) {
            WowConvRect16Put(rect16, 0, (INT)rect.left);   WowConvRect16Put(rect16, 1, (INT)rect.top);
            WowConvRect16Put(rect16, 2, (INT)rect.right);  WowConvRect16Put(rect16, 3, (INT)rect.bottom);
            for (index = 0; index < WOWUSER_RECT16_SIZE; ++index) rectBytes[index] = rect16[index];
        }
        WowNotePut(note, noteCapacity, &noteLength, hasAny ? " -> dirty" : " -> clean");
        Wow32SetReturn(frame, (DWORD)hasAny);
        return 1;
    }

    case WOWUSER_GETNEXTDLGTABITEM: {
        WORD dialog16 = Wow32ArgWord(frame, WOWUSER_GNDTI_ARG_HDLG);
        WORD control16  = Wow32ArgWord(frame, WOWUSER_GNDTI_ARG_CTL);
        WORD previousControl = Wow32ArgWord(frame, WOWUSER_GNDTI_ARG_PREV);
        PWOWUSER_WINDOW window = WowUserFindWindow(dialog16);
        PWOWUSER_WINDOW child = WowUserFindWindow(control16);
        HWND next32; WORD window16Result;
        INT noteLength = 0;
        WowNotePut(note, noteCapacity, &noteLength, "GetNextDlgTabItem dlg 0x");
        WowNoteHex(note, noteCapacity, &noteLength, dialog16, WOW_HEX_WORD_DIGITS);
        if (!window || !window->Window32) { WowNotePut(note, noteCapacity, &noteLength, " -- no real window; 0");
                                Wow32SetReturn(frame, 0); return 1; }
        next32 = GetNextDlgTabItem(window->Window32, child ? child->Window32 : NULL, previousControl ? TRUE : FALSE);
        window16Result = next32 ? WowWinHwnd16(next32) : 0;
        WowNotePut(note, noteCapacity, &noteLength, " -> 0x"); WowNoteHex(note, noteCapacity, &noteLength, window16Result, WOW_HEX_WORD_DIGITS);
        Wow32SetReturn(frame, (DWORD)window16Result);
        return 1;
    }

    case WOWUSER_GETDLGCTRLID: {
        WORD window16 = Wow32ArgWord(frame, WOWUSER_W1_ARG_HWND);
        PWOWUSER_WINDOW window = WowUserFindWindow(window16);
        INT noteLength = 0, controlId;
        WowNotePut(note, noteCapacity, &noteLength, "GetDlgCtrlID 0x");
        WowNoteHex(note, noteCapacity, &noteLength, window16, WOW_HEX_WORD_DIGITS);
        if (!window || !window->Window32) { WowNotePut(note, noteCapacity, &noteLength, " -- no real window; 0");
                                Wow32SetReturn(frame, 0); return 1; }
        controlId = GetDlgCtrlID(window->Window32);
        WowNotePut(note, noteCapacity, &noteLength, " -> "); WowNoteHex(note, noteCapacity, &noteLength, (DWORD)controlId, WOW_HEX_WORD_DIGITS);
        Wow32SetReturn(frame, (DWORD)(WORD)controlId);
        return 1;
    }

    case WOWUSER_DRAWFOCUSRECT: {
        WORD dc16 = Wow32ArgWord(frame, WOWUSER_DFR_ARG_HDC);
        volatile BYTE *rectBytes = Wow32ArgPointer(frame, WOWUSER_DFR_ARG_RECT);
        INT dcKind = -1;
        HGDIOBJ dc = WowGdiH32(dc16, &dcKind);
        BYTE rect16[WOWUSER_RECT16_SIZE];
        RECT rect;
        INT noteLength = 0, index;
        WowNotePut(note, noteCapacity, &noteLength, "DrawFocusRect dc=0x");
        WowNoteHex(note, noteCapacity, &noteLength, dc16, WOW_HEX_WORD_DIGITS);
        if (!dc || (dcKind != WOWGDI_KIND_DC && dcKind != WOWGDI_KIND_WINDC) || !rectBytes) {
            WowNotePut(note, noteCapacity, &noteLength, " -- ★ NOT ONE OF OUR DC TOKENS, or no rect");
            Wow32SetReturn(frame, 0); return 1;
        }
        for (index = 0; index < WOWUSER_RECT16_SIZE; ++index) rect16[index] = (BYTE)rectBytes[index];
        rect.left   = WowConvRect16Get(rect16, 0); rect.top    = WowConvRect16Get(rect16, 1);
        rect.right  = WowConvRect16Get(rect16, 2); rect.bottom = WowConvRect16Get(rect16, 3);
        DrawFocusRect((HDC)dc, &rect);
        WowNotePut(note, noteCapacity, &noteLength, " -> drawn");
        Wow32SetReturn(frame, 1);
        return 1;
    }

    case WOWUSER_DELETEMENU: {
        WORD menu16    = Wow32ArgWord(frame, WOWUSER_DM_ARG_HMENU);
        WORD position   = Wow32ArgWord(frame, WOWUSER_DM_ARG_POS);
        WORD flags = Wow32ArgWord(frame, WOWUSER_DM_ARG_FLAGS);
        HMENU menu = WowUserMenu32(menu16);
        INT noteLength = 0, result;
        WowNotePut(note, noteCapacity, &noteLength, "DeleteMenu 0x");
        WowNoteHex(note, noteCapacity, &noteLength, menu16, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, " pos="); WowNoteHex(note, noteCapacity, &noteLength, position, WOW_HEX_WORD_DIGITS);
        if (!menu) { WowNotePut(note, noteCapacity, &noteLength, " -- ★ NOT ONE OF OUR MENUS; FALSE");
                  Wow32SetReturn(frame, 0); return 1; }
        result = DeleteMenu(menu, position, flags) ? 1 : 0;
        WowNotePut(note, noteCapacity, &noteLength, result ? " -> deleted" : " -> REFUSED");
        Wow32SetReturn(frame, (DWORD)result);
        return 1;
    }

    /* ── ⚠ WINDOWPLACEMENT IS A DIFFERENT STRUCTURE IN 16 BITS. Win16's is 22
         bytes of WORDs (length, flags, showCmd, ptMin, ptMax, rcNormal); Win32's
         is 44 with LONGs. Built field by field rather than copied. */
    case WOWUSER_GETWINDOWPLACEMENT: {
        WORD window16 = Wow32ArgWord(frame, WOWUSER_GWP_ARG_HWND);
        volatile BYTE *bytes16 = Wow32ArgPointer(frame, WOWUSER_GWP_ARG_PL);
        PWOWUSER_WINDOW window = WowUserFindWindow(window16);
        WINDOWPLACEMENT placement;
        INT noteLength = 0;
        WowNotePut(note, noteCapacity, &noteLength, "GetWindowPlacement 0x");
        WowNoteHex(note, noteCapacity, &noteLength, window16, WOW_HEX_WORD_DIGITS);
        if (!window || !window->Window32 || !bytes16) {
            WowNotePut(note, noteCapacity, &noteLength, " -- no real window or no struct; FALSE");
            Wow32SetReturn(frame, 0); return 1;
        }
        placement.length = sizeof placement;
        if (!GetWindowPlacement(window->Window32, &placement)) {
            WowNotePut(note, noteCapacity, &noteLength, " -- the OS refused; nothing written");
            Wow32SetReturn(frame, 0); return 1;
        }
        Wow32PokeWord(bytes16 + WOWUSER_WP16_LENGTH, WOWUSER_WP16_SIZE);
        Wow32PokeWord(bytes16 + WOWUSER_WP16_FLAGS, (WORD)placement.flags);
        Wow32PokeWord(bytes16 + WOWUSER_WP16_SHOWCMD, (WORD)placement.showCmd);
        Wow32PokeWord(bytes16 + WOWUSER_WP16_MIN_X, (WORD)(SHORT)placement.ptMinPosition.x);
        Wow32PokeWord(bytes16 + WOWUSER_WP16_MIN_Y, (WORD)(SHORT)placement.ptMinPosition.y);
        Wow32PokeWord(bytes16 + WOWUSER_WP16_MAX_X, (WORD)(SHORT)placement.ptMaxPosition.x);
        Wow32PokeWord(bytes16 + WOWUSER_WP16_MAX_Y, (WORD)(SHORT)placement.ptMaxPosition.y);
        Wow32PokeWord(bytes16 + WOWUSER_WP16_NORMAL_LEFT, (WORD)(SHORT)placement.rcNormalPosition.left);
        Wow32PokeWord(bytes16 + WOWUSER_WP16_NORMAL_TOP, (WORD)(SHORT)placement.rcNormalPosition.top);
        Wow32PokeWord(bytes16 + WOWUSER_WP16_NORMAL_RIGHT, (WORD)(SHORT)placement.rcNormalPosition.right);
        Wow32PokeWord(bytes16 + WOWUSER_WP16_NORMAL_BOTTOM, (WORD)(SHORT)placement.rcNormalPosition.bottom);
        WowNotePut(note, noteCapacity, &noteLength, " -> showCmd ");
        WowNoteHex(note, noteCapacity, &noteLength, (DWORD)placement.showCmd, WOW_HEX_BYTE_DIGITS);
        Wow32SetReturn(frame, 1);
        return 1;
    }

    /* ── s93: SetWindowsHook -- keyboard and journal hooks, on the OS's own. ──
         RECORDER installs WH_KEYBOARD (2) at start-up for its shortcut keys and
         Ctrl+Break, and WH_JOURNALRECORD (0) when a recording starts; playing a
         macro back is WH_JOURNALPLAYBACK (1). Each becomes the Win32 hook of the
         same kind, whose callback calls the 16-bit procedure (wowuser_hook_*).
         Other kinds are recorded and answered 0, as before, and say so. */
    case WOWUSER_SETWINDOWSHOOK: {
        DWORD procedure = Wow32ArgDword(frame, WOWUSER_SWH_ARG_PROC);
        SHORT hookId   = (SHORT)Wow32ArgWord(frame, WOWUSER_SWH_ARG_ID);
        WORD  module16 = Wow32ArgWord(frame, WOWUSER_SWH_ARG_HMOD);
        INT   noteLength = 0, isOk = WowUserHookSet(hookId, procedure, module16);
        WowNotePut(note, noteCapacity, &noteLength, "SetWindowsHook id=");
        WowNoteHex(note, noteCapacity, &noteLength, (DWORD)(WORD)hookId, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, " proc=0x"); WowNoteHex(note, noteCapacity, &noteLength, procedure, WOW_HEX_DWORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, isOk == 1 ? " -> the OS's hook of the same kind installed"
                                 : isOk == WOWUSER_HOOK_RECORDED_ONLY ? " -> ★ a kind this host does not run; recorded only"
                                           : " -> ★ the OS refused the hook");
        Wow32SetReturn(frame, 0);             /* no previous hook in the chain */
        return 1;
    }

    /* ── ⚠⚠ HOOKS: WE INSTALL NONE, AND SAYING SO IS THE HONEST ANSWER. ───────
         SetWindowsHook is not serviced, so no guest hook is ever in a chain
         here. UnhookWindowsHook therefore has nothing to remove (FALSE is what
         Windows returns for a hook it does not hold) and DefHookProc has no NEXT
         hook to call, which is exactly the case its own contract covers: with a
         null next-hook it returns 0. Both are TRUE statements about this VDM
         rather than stubs, and if hooks are ever implemented these are where the
         chain gets walked. */
    case WOWUSER_UNHOOKWINDOWSHOOK: {
        /* UnhookWindowsHook(int nCode, FARPROC lpfn): lpfn at 0, nCode at 4. */
        DWORD procedure = Wow32ArgDword(frame, WOWUSER_UWH_ARG_PROC);
        SHORT hookId   = (SHORT)Wow32ArgWord(frame, WOWUSER_UWH_ARG_ID);
        INT noteLength = 0, result = WowUserHookUnset(hookId, procedure);
        WowNotePut(note, noteCapacity, &noteLength, "UnhookWindowsHook id=");
        WowNoteHex(note, noteCapacity, &noteLength, (DWORD)(WORD)hookId, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, result ? " -> removed" : " -> not one we hold; FALSE");
        Wow32SetReturn(frame, (DWORD)result);
        return 1;
    }
    case WOWUSER_DEFHOOKPROC: {
        INT noteLength = 0;
        WowNotePut(note, noteCapacity, &noteLength, "DefHookProc -- no NEXT hook to pass to; 0, "
                                   "which is what a null chain returns");
        Wow32SetReturn(frame, 0);
        return 1;
    }

    /* ── ⚠⚠ SystemParametersInfo IS SYSTEM-WIDE, so a SET is REFUSED. ─────────
         A VDM that can be killed at any moment must not leave the user's
         desktop reconfigured -- same rule as ClipCursor above. Queries are
         answered from the real OS; anything that WRITES is declined and said so. */
    case WOWUSER_SYSTEMPARAMETERSINFO: {
        WORD action = Wow32ArgWord(frame, WOWUSER_SPI_ARG_ACTION);
        WORD uiParam     = Wow32ArgWord(frame, WOWUSER_SPI_ARG_UIPARAM);
        volatile BYTE *parameterBytes = Wow32ArgPointer(frame, WOWUSER_SPI_ARG_PARAM);
        WORD winIni   = Wow32ArgWord(frame, WOWUSER_SPI_ARG_WINI);
        INT  noteLength = 0;
        WowNotePut(note, noteCapacity, &noteLength, "SystemParametersInfo action=0x");
        WowNoteHex(note, noteCapacity, &noteLength, action, WOW_HEX_WORD_DIGITS);
        (VOID)uiParam; (VOID)parameterBytes;
        if (winIni) {
            WowNotePut(note, noteCapacity, &noteLength, " -- ★ WRITES REFUSED: this is a SYSTEM-WIDE"
                                       " setting and the VDM can be killed at will");
            Wow32SetReturn(frame, 0);
            return 1;
        }
        /* ── ⚠⚠ SPI_GETICONTITLELOGFONT (0x1F): ANSWERING "FALSE" IS NOT ENOUGH.
             MPLAYER asks for it and then calls CreateFontIndirect ON THE BUFFER
             REGARDLESS -- measured: with FALSE returned and the buffer untouched,
             the next log line is CreateFontIndirect with a face name of stack
             litter. The return value was not the answer it acted on; the BUFFER
             was. So fill it, or the guest builds a font out of rubbish.
           ⚠ AND A WIN16 LOGFONT IS 50 BYTES, NOT 60. Every metric is a WORD here
             and a LONG in Win32, and the 32-byte face name starts at 18 rather
             than 28 -- copying the Win32 structure across would put the typeface
             where the guest reads lfWeight. */
        if (action == SPI_GETICONTITLELOGFONT && parameterBytes) {
            LOGFONTA logFont;
            if (SystemParametersInfoA(SPI_GETICONTITLELOGFONT, 0, &logFont, 0)) {
                INT index2;
                Wow32PokeWord(parameterBytes +  0, (WORD)(SHORT)logFont.lfHeight);
                Wow32PokeWord(parameterBytes + WOWGDI_LF16_WIDTH, (WORD)(SHORT)logFont.lfWidth);
                Wow32PokeWord(parameterBytes + WOWGDI_LF16_ESCAPEMENT, (WORD)(SHORT)logFont.lfEscapement);
                Wow32PokeWord(parameterBytes + WOWGDI_LF16_ORIENTATION, (WORD)(SHORT)logFont.lfOrientation);
                Wow32PokeWord(parameterBytes + WOWGDI_LF16_WEIGHT, (WORD)(SHORT)logFont.lfWeight);
                parameterBytes[WOWGDI_LF16_ITALIC] = logFont.lfItalic;        parameterBytes[WOWGDI_LF16_UNDERLINE] = logFont.lfUnderline;
                parameterBytes[WOWGDI_LF16_STRIKEOUT] = logFont.lfStrikeOut;     parameterBytes[WOWGDI_LF16_CHARSET] = logFont.lfCharSet;
                parameterBytes[WOWGDI_LF16_OUTPRECISION] = logFont.lfOutPrecision;  parameterBytes[WOWGDI_LF16_CLIPPRECISION] = logFont.lfClipPrecision;
                parameterBytes[WOWGDI_LF16_QUALITY] = logFont.lfQuality;       parameterBytes[WOWGDI_LF16_PITCHANDFAMILY] = logFont.lfPitchAndFamily;
                for (index2 = 0; index2 < WOWGDI_LF16_FACESIZE; ++index2)
                    parameterBytes[WOWGDI_LF16_FACENAME + index2] = (BYTE)((index2 < LF_FACESIZE) ? logFont.lfFaceName[index2] : 0);
                WowNotePut(note, noteCapacity, &noteLength, " -> icon-title LOGFONT written (50 bytes) \"");
                { CHAR faceName[LF_FACESIZE + 1]; INT faceIndex;
                  for (faceIndex = 0; faceIndex < LF_FACESIZE && logFont.lfFaceName[faceIndex]; ++faceIndex) faceName[faceIndex] = logFont.lfFaceName[faceIndex];
                  faceName[faceIndex] = 0; WowNotePut(note, noteCapacity, &noteLength, faceName); }
                WowNotePut(note, noteCapacity, &noteLength, "\"");
                Wow32SetReturn(frame, 1);
                return 1;
            }
        }
        /* SPI_GETWORKAREA (0x30) is the one MPLAYER wants, and its RECT is the
           only structure involved -- answered in 16-bit RECT form. */
        if (action == SPI_GETWORKAREA && parameterBytes) {
            RECT workArea; BYTE rect16[WOWUSER_RECT16_SIZE]; INT index;
            if (SystemParametersInfoA(SPI_GETWORKAREA, 0, &workArea, 0)) {
                WowConvRect16Put(rect16, 0, (INT)workArea.left);  WowConvRect16Put(rect16, 1, (INT)workArea.top);
                WowConvRect16Put(rect16, 2, (INT)workArea.right); WowConvRect16Put(rect16, 3, (INT)workArea.bottom);
                for (index = 0; index < WOWUSER_RECT16_SIZE; ++index) parameterBytes[index] = rect16[index];
                WowNotePut(note, noteCapacity, &noteLength, " -> work area written");
                Wow32SetReturn(frame, 1);
                return 1;
            }
        }
        WowNotePut(note, noteCapacity, &noteLength, " -- ★ QUERY NOT IMPLEMENTED; answered FALSE "
                                   "rather than leaving the guest's buffer as litter");
        Wow32SetReturn(frame, 0);
        return 1;
    }

    case WOWUSER_DLGDIRLIST: {
        WORD dialog16 = Wow32ArgWord(frame, WOWUSER_DDL_ARG_HDLG);
        WORD listId  = Wow32ArgWord(frame, WOWUSER_DDL_ARG_IDLIST);
        WORD staticId  = Wow32ArgWord(frame, WOWUSER_DDL_ARG_IDSTATIC);
        WORD fileType   = Wow32ArgWord(frame, WOWUSER_DDL_ARG_FILETYPE);
        PWOWUSER_WINDOW window = WowUserFindWindow(dialog16);
        CHAR fileSpec[MAX_PATH];
        INT  noteLength = 0, result;
        fileSpec[0] = 0;
        Wow32ArgString(frame, WOWUSER_DDL_ARG_SPEC, fileSpec, (INT)sizeof fileSpec);
        WowNotePut(note, noteCapacity, &noteLength, "DlgDirList 0x");
        WowNoteHex(note, noteCapacity, &noteLength, dialog16, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, " \""); WowNotePut(note, noteCapacity, &noteLength, fileSpec);
        WowNotePut(note, noteCapacity, &noteLength, "\"");
        if (!window || !window->Window32) { WowNotePut(note, noteCapacity, &noteLength, " -- no real window; 0");
                                Wow32SetReturn(frame, 0); return 1; }
        result = DlgDirListA(window->Window32, fileSpec, listId, staticId, fileType) ? 1 : 0;
        WowNotePut(note, noteCapacity, &noteLength, result ? " -> filled" : " -> REFUSED (bad spec or no listbox)");
        Wow32SetReturn(frame, (DWORD)result);
        return 1;
    }

    case WOWUSER_CHANGEMENU: {
        WORD menu16   = Wow32ArgWord(frame, WOWUSER_CM_ARG_HMENU);
        WORD changeId  = Wow32ArgWord(frame, WOWUSER_CM_ARG_IDCHANGE);
        WORD newId  = Wow32ArgWord(frame, WOWUSER_CM_ARG_IDNEW);
        WORD change  = Wow32ArgWord(frame, WOWUSER_CM_ARG_CHANGE);
        HMENU menu   = WowUserMenu32(menu16);
        CHAR  text[WOWUSER_TEXT_SIZE];
        INT   noteLength = 0, result = 0;
        text[0] = 0;
        Wow32ArgString(frame, WOWUSER_CM_ARG_ITEM, text, (INT)sizeof text);
        WowNotePut(note, noteCapacity, &noteLength, "ChangeMenu 0x");
        WowNoteHex(note, noteCapacity, &noteLength, menu16, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, " change=0x"); WowNoteHex(note, noteCapacity, &noteLength, change, WOW_HEX_WORD_DIGITS);
        if (!menu) { WowNotePut(note, noteCapacity, &noteLength, " -- ★ NOT ONE OF OUR MENUS; FALSE");
                  Wow32SetReturn(frame, 0); return 1; }
        /* MF_APPEND 0x0100, MF_DELETE 0x0200, MF_CHANGE 0x0080, MF_REMOVE 0x1000;
           anything else is an INSERT, which is what a zero `change` means. */
        if (change & MF_DELETE) {
            result = DeleteMenu(menu, changeId, (UINT)(change & MF_BYPOSITION ? MF_BYPOSITION : MF_BYCOMMAND)) ? 1 : 0;
            WowNotePut(note, noteCapacity, &noteLength, " [delete]");
        } else if (change & MF_REMOVE) {
            result = RemoveMenu(menu, changeId, (UINT)(change & MF_BYPOSITION ? MF_BYPOSITION : MF_BYCOMMAND)) ? 1 : 0;
            WowNotePut(note, noteCapacity, &noteLength, " [remove]");
        } else if (change & MF_CHANGE) {
            result = ModifyMenuA(menu, changeId, (UINT)(change & ~(UINT)MF_CHANGE), (UINT_PTR)newId,
                            text[0] ? text : NULL) ? 1 : 0;
            WowNotePut(note, noteCapacity, &noteLength, " [change]");
        } else if (change & MF_APPEND) {
            result = AppendMenuA(menu, (UINT)(change & ~(UINT)MF_APPEND), (UINT_PTR)newId,
                            text[0] ? text : NULL) ? 1 : 0;
            WowNotePut(note, noteCapacity, &noteLength, " [append]");
        } else {
            result = InsertMenuA(menu, changeId, (UINT)change, (UINT_PTR)newId,
                            text[0] ? text : NULL) ? 1 : 0;
            WowNotePut(note, noteCapacity, &noteLength, " [insert]");
        }
        WowNotePut(note, noteCapacity, &noteLength, result ? " -> ok" : " -> REFUSED by the OS");
        Wow32SetReturn(frame, (DWORD)result);
        return 1;
    }

    /* ── ⚠ GrayString's OUTPUT FUNCTION IS A 16-BIT CALLBACK WE DO NOT RUN HERE.
         Passing NULL is not a shortcut: NULL is a DOCUMENTED value meaning "use
         TextOut", which is what every caller that supplies no proc gets anyway.
         A guest that DID supply one is told so in the log rather than having its
         proc silently ignored -- the string still draws, greyed, in the right
         place, which is the visible contract. */
    case WOWUSER_GRAYSTRING: {
        WORD dc16 = Wow32ArgWord(frame, WOWUSER_GS_ARG_HDC);
        WORD brush16 = Wow32ArgWord(frame, WOWUSER_GS_ARG_HBRUSH);
        DWORD outputFunction = Wow32ArgDword(frame, WOWUSER_GS_ARG_OUTFUNC);
        volatile BYTE *data = Wow32ArgPointer(frame, WOWUSER_GS_ARG_DATA);
        INT  count  = (INT)(SHORT)Wow32ArgWord(frame, WOWUSER_GS_ARG_COUNT);
        INT  positionX  = (INT)(SHORT)Wow32ArgWord(frame, WOWUSER_GS_ARG_X);
        INT  positionY  = (INT)(SHORT)Wow32ArgWord(frame, WOWUSER_GS_ARG_Y);
        INT  width = (INT)(SHORT)Wow32ArgWord(frame, WOWUSER_GS_ARG_WIDTH);
        INT  height = (INT)(SHORT)Wow32ArgWord(frame, WOWUSER_GS_ARG_HEIGHT);
        INT  dcKind = -1, brushKind = -1;
        HGDIOBJ dc = WowGdiH32(dc16, &dcKind);
        HGDIOBJ brush = brush16 ? WowGdiH32(brush16, &brushKind) : NULL;
        CHAR buffer[WOWUSER_LONG_TEXT_SIZE];
        INT  noteLength = 0, index, result;
        WowNotePut(note, noteCapacity, &noteLength, "GrayString(0x");
        WowNoteHex(note, noteCapacity, &noteLength, dc16, WOW_HEX_WORD_DIGITS);
        if (!dc || (dcKind != WOWGDI_KIND_DC && dcKind != WOWGDI_KIND_WINDC)) {
            WowNotePut(note, noteCapacity, &noteLength, ") -- ★ NOT ONE OF OUR DC TOKENS; FALSE");
            Wow32SetReturn(frame, 0); return 1;
        }
        if (count < 0) count = 0;
        if (count > (INT)sizeof buffer - 1) count = (INT)sizeof buffer - 1;
        for (index = 0; index < count; ++index) buffer[index] = data ? (CHAR)data[index] : ' ';
        buffer[count] = 0;
        if (outputFunction) WowNotePut(note, noteCapacity, &noteLength, ") ★ the guest supplied an OUTPUT PROC "
                                              "and we do not call it; drawn with TextOut");
        result = GrayStringA((HDC)dc, (HBRUSH)brush, NULL, (LPARAM)(LONG_PTR)buffer, count,
                        positionX, positionY, width, height) ? 1 : 0;
        WowNotePut(note, noteCapacity, &noteLength, result ? " -> greyed" : " -> REFUSED");
        Wow32SetReturn(frame, (DWORD)result);
        return 1;
    }

    /* ── ⚠⚠ DefDlgProc GOES TO DefWindowProc, AND THAT IS A REAL DIFFERENCE. ──
         The real DefDlgProc runs the dialog manager's default behaviour on a
         window created by the dialog manager -- it reads DWL_MSGRESULT and the
         dialog class's extra bytes, which a window WE created with CreateWindow
         does not have. Calling the OS's DefDlgProc on one is undefined, so this
         forwards to DefWindowProc, which handles everything except the
         dialog-specific parts (default button, ESC to cancel, tab order).
       ⇒ WHAT THIS COSTS, SAID OUT LOUD: a guest dialog gets the right answers to
         ordinary messages and loses keyboard defaults. That is a degradation, not
         a lie, and it disappears when real dialog creation lands (USER thunk
         0xEF). */
    /* ── ⛔⛔ s88: DefDlgProc CALLS THE DIALOG'S OWN PROCEDURE FIRST. (user: "some
         close buttons (X) don't work") A Win16 program that uses a dialog as its
         main window (Charmap) registers a class whose window procedure IS
         DefDlgProc and passes its real DLGPROC to CreateDialog -- so DefDlgProc is
         the ONLY route to the program's code. The real one calls the DLGPROC and
         applies the default only if it answers FALSE. Ours went straight to the
         default: Charmap's X posted IDCANCEL, IDCANCEL came back to DefDlgProc,
         and the code that ends Charmap on IDCANCEL never saw either.
       ► So with a DLGPROC: call it (the host's ordinary 16-bit callback) and arm
         WOWCALL_ACT_DLGDEFAULT, which applies WowUserDlgDefault() when it
         returns 0. Without one, or when the DLGPROC is already handling this very
         message for this window (a procedure that calls DefDlgProc on itself),
         the default runs here. */
    case WOWUSER_DEFDLGPROC: {
        WORD dialog16 = Wow32ArgWord(frame, WOWUSER_DDP_ARG_HDLG);
        WORD message16  = Wow32ArgWord(frame, WOWUSER_DDP_ARG_MSG);
        WORD wParam16 = Wow32ArgWord(frame, WOWUSER_DDP_ARG_WPARAM);
        DWORD lParam32 = Wow32ArgDword(frame, WOWUSER_DDP_ARG_LPARAM);
        PWOWUSER_WINDOW window = WowUserFindWindow(dialog16);
        DWORD dialogProcedure = window ? window->DialogProcedure
                        : (dialog16 && dialog16 == g_WowUserGone.Window ? g_WowUserGone.DialogProcedure : 0);
        INT noteLength = 0, isSelf = 0;
        WowNotePut(note, noteCapacity, &noteLength, "DefDlgProc 0x");
        WowNoteHex(note, noteCapacity, &noteLength, dialog16, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, " msg=0x"); WowNoteHex(note, noteCapacity, &noteLength, message16, WOW_HEX_WORD_DIGITS);
        if (!window && !dialogProcedure) { WowNotePut(note, noteCapacity, &noteLength, " -- no such window; 0");
                            Wow32SetReturn(frame, 0); return 1; }
        if (!window) WowNotePut(note, noteCapacity, &noteLength, " [its record is released; its DLGPROC kept]");
        if (g_WowCallDepth > 0) {
            const WOWCALL_FRAME *callFrame = &g_WowCallFrames[g_WowCallDepth - 1];
            isSelf = (callFrame->Procedure == dialogProcedure && callFrame->Window == dialog16 && callFrame->Message == message16);
        }
        if (dialogProcedure && !isSelf && g_WowCallDepth < WOWCALL_MAX_DEPTH) {
            g_WowUserDlgDefaults[g_WowCallDepth].WParam = wParam16;
            g_WowUserDlgDefaults[g_WowCallDepth].LParam = lParam32;
            frame->CallbackProcedure   = dialogProcedure;
            frame->CallbackDataSelector     = frame->GuestDataSelector;
            frame->CallbackArguments[0] = dialog16;
            frame->CallbackArguments[1] = message16;
            frame->CallbackArguments[2] = wParam16;
            frame->CallbackArguments[3] = (WORD)(lParam32 >> WORD_SHIFT);
            frame->CallbackArguments[4] = (WORD)(lParam32 & WORD_MASK);
            frame->CallbackArgumentCount   = WOWUSER_WNDPROC_ARGUMENTS;
            frame->CallbackReturnMode    = WOWCALL_RET_RESULT;
            frame->CallbackWindow   = dialog16;
            frame->CallbackMessage    = message16;
            frame->CallbackAction    = WOWCALL_ACT_DLGDEFAULT;
            frame->CallbackActionArgument = dialog16;
            WowNotePut(note, noteCapacity, &noteLength, " -> its DLGPROC 0x");
            WowNoteHex(note, noteCapacity, &noteLength, dialogProcedure, WOW_HEX_DWORD_DIGITS);
            WowNotePut(note, noteCapacity, &noteLength, " first; the default only if it answers FALSE");
            return 1;
        }
        Wow32SetReturn(frame, (DWORD)WowUserDlgDefault(window, dialog16, message16, wParam16, lParam32,
                                                   note, noteCapacity, &noteLength));
        return 1;
    }

    case WOWUSER_GETCLIPBOARDOWNER: {
        HWND owner32 = GetClipboardOwner();
        WORD window16Result = owner32 ? WowWinHwnd16(owner32) : 0;
        INT noteLength = 0;
        WowNotePut(note, noteCapacity, &noteLength, "GetClipboardOwner -> 0x");
        WowNoteHex(note, noteCapacity, &noteLength, window16Result, WOW_HEX_WORD_DIGITS);
        if (owner32 && !window16Result) WowNotePut(note, noteCapacity, &noteLength, " (owned OUTSIDE this VDM; 0)");
        Wow32SetReturn(frame, (DWORD)window16Result);
        return 1;
    }

    case WOWUSER_GETDOUBLECLICKTIME: {
        UINT doubleClickTime = GetDoubleClickTime();
        INT noteLength = 0;
        WowNotePut(note, noteCapacity, &noteLength, "GetDoubleClickTime -> ");
        WowNoteHex(note, noteCapacity, &noteLength, doubleClickTime, WOW_HEX_WORD_DIGITS);
        Wow32SetReturn(frame, (DWORD)doubleClickTime);
        return 1;
    }

    case WOWUSER_GETTOPWINDOW: {
        WORD window16 = Wow32ArgWord(frame, WOWUSER_W1_ARG_HWND);
        PWOWUSER_WINDOW window = window16 ? WowUserFindWindow(window16) : NULL;
        HWND top32 = GetTopWindow(window ? window->Window32 : NULL);
        WORD window16Result = top32 ? WowWinHwnd16(top32) : 0;
        INT noteLength = 0;
        WowNotePut(note, noteCapacity, &noteLength, "GetTopWindow 0x");
        WowNoteHex(note, noteCapacity, &noteLength, window16, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, " -> 0x"); WowNoteHex(note, noteCapacity, &noteLength, window16Result, WOW_HEX_WORD_DIGITS);
        Wow32SetReturn(frame, (DWORD)window16Result);
        return 1;
    }

    case WOWUSER_DRAWICON: {
        WORD dc16  = Wow32ArgWord(frame, WOWUSER_DI2_ARG_HDC);
        INT  positionX    = (INT)(SHORT)Wow32ArgWord(frame, WOWUSER_DI2_ARG_X);
        INT  positionY    = (INT)(SHORT)Wow32ArgWord(frame, WOWUSER_DI2_ARG_Y);
        WORD icon16  = Wow32ArgWord(frame, WOWUSER_DI2_ARG_HICON);
        INT  dcKind = -1;
        HGDIOBJ dc = WowGdiH32(dc16, &dcKind);
        HICON   icon = WowUserSystemResourceIcon(icon16, NULL, 0, 0);
        INT  noteLength = 0, result;
        WowNotePut(note, noteCapacity, &noteLength, "DrawIcon(dc=0x");
        WowNoteHex(note, noteCapacity, &noteLength, dc16, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, ", icon=0x"); WowNoteHex(note, noteCapacity, &noteLength, icon16, WOW_HEX_WORD_DIGITS);
        if (!dc || (dcKind != WOWGDI_KIND_DC && dcKind != WOWGDI_KIND_WINDC)) {
            WowNotePut(note, noteCapacity, &noteLength, ") -- ★ NOT ONE OF OUR DC TOKENS; FALSE");
            Wow32SetReturn(frame, 0); return 1;
        }
        if (!icon) {
            WowNotePut(note, noteCapacity, &noteLength, ") -- ★ NOT ONE OF OUR ICONS; FALSE");
            Wow32SetReturn(frame, 0); return 1;
        }
        result = DrawIcon((HDC)dc, positionX, positionY, icon) ? 1 : 0;
        WowNotePut(note, noteCapacity, &noteLength, result ? ") -> drawn" : ") -> REFUSED");
        Wow32SetReturn(frame, (DWORD)result);
        return 1;
    }

    case WOWUSER_CREATEPOPUPMENU: {
        HMENU menu = CreatePopupMenu();
        WORD window16Result = menu ? WowUserMenu16(menu) : 0;
        INT noteLength = 0;
        WowNotePut(note, noteCapacity, &noteLength, "CreatePopupMenu -> 0x");
        WowNoteHex(note, noteCapacity, &noteLength, window16Result, WOW_HEX_WORD_DIGITS);
        if (menu && !window16Result) { DestroyMenu(menu);
            WowNotePut(note, noteCapacity, &noteLength, " -- ★ TOKEN MAP FULL; menu destroyed"); }
        Wow32SetReturn(frame, (DWORD)window16Result);
        return 1;
    }

    case WOWUSER_DESTROYMENU: {
        WORD menu16 = Wow32ArgWord(frame, WOWUSER_W1_ARG_HWND);
        HMENU menu = WowUserMenu32(menu16);
        INT noteLength = 0, result;
        WowNotePut(note, noteCapacity, &noteLength, "DestroyMenu 0x");
        WowNoteHex(note, noteCapacity, &noteLength, menu16, WOW_HEX_WORD_DIGITS);
        if (!menu) { WowNotePut(note, noteCapacity, &noteLength, " -- ★ NOT ONE OF OUR MENUS; FALSE");
                  Wow32SetReturn(frame, 0); return 1; }
        result = DestroyMenu(menu) ? 1 : 0;
        WowNotePut(note, noteCapacity, &noteLength, result ? " -> destroyed" : " -> REFUSED");
        Wow32SetReturn(frame, (DWORD)result);
        return 1;
    }

    case WOWUSER_DESTROYICON: {
        WORD icon16 = Wow32ArgWord(frame, WOWUSER_W1_ARG_HWND);
        HICON icon = WowUserSystemResourceIcon(icon16, NULL, 0, 0);
        INT noteLength = 0, result = 0;
        WowNotePut(note, noteCapacity, &noteLength, "DestroyIcon 0x");
        WowNoteHex(note, noteCapacity, &noteLength, icon16, WOW_HEX_WORD_DIGITS);
        if (icon) result = DestroyIcon(icon) ? 1 : 0;
        WowNotePut(note, noteCapacity, &noteLength, result ? " -> destroyed"
                                     : " -- ★ NOT ONE OF OUR ICONS; FALSE");
        Wow32SetReturn(frame, (DWORD)result);
        return 1;
    }

    case WOWUSER_GETMENUITEMCOUNT: {
        WORD menu16 = Wow32ArgWord(frame, WOWUSER_W1_ARG_HWND);
        HMENU menu = WowUserMenu32(menu16);
        INT noteLength = 0, count;
        WowNotePut(note, noteCapacity, &noteLength, "GetMenuItemCount 0x");
        WowNoteHex(note, noteCapacity, &noteLength, menu16, WOW_HEX_WORD_DIGITS);
        if (!menu) { WowNotePut(note, noteCapacity, &noteLength, " -- ★ NOT ONE OF OUR MENUS; -1");
                  Wow32SetReturn(frame, WOWUSER_MINUS_ONE16); return 1; }
        count = GetMenuItemCount(menu);
        WowNotePut(note, noteCapacity, &noteLength, " -> "); WowNoteHex(note, noteCapacity, &noteLength, (DWORD)count, WOW_HEX_WORD_DIGITS);
        Wow32SetReturn(frame, (DWORD)(WORD)count);
        return 1;
    }

    case WOWUSER_GETMENUITEMID: {
        WORD menu16  = Wow32ArgWord(frame, WOWUSER_GMII_ARG_HMENU);
        WORD position = Wow32ArgWord(frame, WOWUSER_GMII_ARG_POS);
        HMENU menu = WowUserMenu32(menu16);
        INT noteLength = 0; UINT itemId;
        WowNotePut(note, noteCapacity, &noteLength, "GetMenuItemID 0x");
        WowNoteHex(note, noteCapacity, &noteLength, menu16, WOW_HEX_WORD_DIGITS);
        if (!menu) { WowNotePut(note, noteCapacity, &noteLength, " -- ★ NOT ONE OF OUR MENUS; -1");
                  Wow32SetReturn(frame, WOWUSER_MINUS_ONE16); return 1; }
        itemId = GetMenuItemID(menu, (INT)(SHORT)position);
        WowNotePut(note, noteCapacity, &noteLength, " pos="); WowNoteHex(note, noteCapacity, &noteLength, position, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, " -> "); WowNoteHex(note, noteCapacity, &noteLength, itemId, WOW_HEX_WORD_DIGITS);
        Wow32SetReturn(frame, (DWORD)(WORD)itemId);
        return 1;
    }

    case WOWUSER_GETMENUSTATE: {
        WORD menu16 = Wow32ArgWord(frame, WOWUSER_GMS_ARG_HMENU);
        WORD itemId = Wow32ArgWord(frame, WOWUSER_GMS_ARG_ID);
        WORD flags = Wow32ArgWord(frame, WOWUSER_GMS_ARG_FLAGS);
        HMENU menu = WowUserMenu32(menu16);
        INT noteLength = 0; UINT state;
        WowNotePut(note, noteCapacity, &noteLength, "GetMenuState 0x");
        WowNoteHex(note, noteCapacity, &noteLength, menu16, WOW_HEX_WORD_DIGITS);
        if (!menu) { WowNotePut(note, noteCapacity, &noteLength, " -- ★ NOT ONE OF OUR MENUS; -1");
                  Wow32SetReturn(frame, WOWUSER_MINUS_ONE16); return 1; }
        state = GetMenuState(menu, itemId, flags);
        WowNotePut(note, noteCapacity, &noteLength, " -> 0x"); WowNoteHex(note, noteCapacity, &noteLength, state, WOW_HEX_WORD_DIGITS);
        Wow32SetReturn(frame, (DWORD)(WORD)state);
        return 1;
    }

    case WOWUSER_GETMENUSTRING: {
        WORD menu16  = Wow32ArgWord(frame, WOWUSER_GMSTR_ARG_HMENU);
        WORD itemId  = Wow32ArgWord(frame, WOWUSER_GMSTR_ARG_ID);
        WORD maximum = Wow32ArgWord(frame, WOWUSER_GMSTR_ARG_MAX);
        WORD flags  = Wow32ArgWord(frame, WOWUSER_GMSTR_ARG_FLAGS);
        volatile BYTE *bufferBytes = Wow32ArgPointer(frame, WOWUSER_GMSTR_ARG_BUF);
        HMENU menu = WowUserMenu32(menu16);
        CHAR buffer[WOWUSER_STRING_SIZE];
        INT noteLength = 0, length, index;
        WowNotePut(note, noteCapacity, &noteLength, "GetMenuString 0x");
        WowNoteHex(note, noteCapacity, &noteLength, menu16, WOW_HEX_WORD_DIGITS);
        if (!menu || !bufferBytes || !maximum) {
            WowNotePut(note, noteCapacity, &noteLength, " -- ★ NOT ONE OF OUR MENUS, or no buffer; 0");
            Wow32SetReturn(frame, 0); return 1;
        }
        if (maximum > sizeof buffer) maximum = (WORD)sizeof buffer;
        length = GetMenuStringA(menu, itemId, buffer, (INT)maximum, flags);
        for (index = 0; index < length && index < (INT)maximum - 1; ++index) bufferBytes[index] = (BYTE)buffer[index];
        bufferBytes[(index < (INT)maximum) ? index : (INT)maximum - 1] = 0;
        WowNotePut(note, noteCapacity, &noteLength, " -> \""); WowNotePut(note, noteCapacity, &noteLength, buffer);
        WowNotePut(note, noteCapacity, &noteLength, "\"");
        Wow32SetReturn(frame, (DWORD)(WORD)length);
        return 1;
    }

    case WOWUSER_MODIFYMENU:
    case WOWUSER_INSERTMENU: {
        INT  isInsert  = (frame->Id == WOWUSER_INSERTMENU);
        WORD menu16   = Wow32ArgWord(frame, WOWUSER_MI2_ARG_HMENU);
        WORD position  = Wow32ArgWord(frame, WOWUSER_MI2_ARG_POS);
        WORD flags   = Wow32ArgWord(frame, WOWUSER_MI2_ARG_FLAGS);
        WORD newId  = Wow32ArgWord(frame, WOWUSER_MI2_ARG_IDNEW);
        HMENU menu   = WowUserMenu32(menu16);
        CHAR text[WOWUSER_TEXT_SIZE];
        INT  noteLength = 0, result;
        text[0] = 0;
        Wow32ArgString(frame, WOWUSER_MI2_ARG_ITEM, text, (INT)sizeof text);
        WowNotePut(note, noteCapacity, &noteLength, isInsert ? "InsertMenu 0x" : "ModifyMenu 0x");
        WowNoteHex(note, noteCapacity, &noteLength, menu16, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, " flags=0x"); WowNoteHex(note, noteCapacity, &noteLength, flags, WOW_HEX_WORD_DIGITS);
        if (!menu) { WowNotePut(note, noteCapacity, &noteLength, " -- ★ NOT ONE OF OUR MENUS; FALSE");
                  Wow32SetReturn(frame, 0); return 1; }
        /* ⚠ A SEPARATOR HAS NO TEXT, and reading the pointer for one would read
             whatever the guest happened to leave there. Same rule as AppendMenu. */
        if (flags & MF_SEPARATOR) {
            result = isInsert ? (InsertMenuA(menu, position, (UINT)flags, (UINT_PTR)newId, NULL) ? 1 : 0)
                    : (ModifyMenuA(menu, position, (UINT)flags, (UINT_PTR)newId, NULL) ? 1 : 0);
            WowNotePut(note, noteCapacity, &noteLength, " [separator]");
        } else if (flags & (MF_BITMAP | MF_OWNERDRAW)) {
            WowNotePut(note, noteCapacity, &noteLength, " -- ★ BITMAP/OWNERDRAW NOT SUPPORTED; FALSE");
            Wow32SetReturn(frame, 0); return 1;
        } else {
            result = isInsert ? (InsertMenuA(menu, position, (UINT)flags, (UINT_PTR)newId, text) ? 1 : 0)
                    : (ModifyMenuA(menu, position, (UINT)flags, (UINT_PTR)newId, text) ? 1 : 0);
            WowNotePut(note, noteCapacity, &noteLength, " \""); WowNotePut(note, noteCapacity, &noteLength, text);
            WowNotePut(note, noteCapacity, &noteLength, "\"");
        }
        WowNotePut(note, noteCapacity, &noteLength, result ? " -> ok" : " -> REFUSED by the OS");
        Wow32SetReturn(frame, (DWORD)result);
        return 1;
    }

    case WOWUSER_SCROLLWINDOW: {
        WORD window16 = Wow32ArgWord(frame, WOWUSER_SW2_ARG_HWND);
        INT  deltaX = (INT)(SHORT)Wow32ArgWord(frame, WOWUSER_SW2_ARG_DX);
        INT  deltaY = (INT)(SHORT)Wow32ArgWord(frame, WOWUSER_SW2_ARG_DY);
        volatile BYTE *rectBytes = Wow32ArgPointer(frame, WOWUSER_SW2_ARG_RECT);
        volatile BYTE *clipBytes = Wow32ArgPointer(frame, WOWUSER_SW2_ARG_CLIP);
        PWOWUSER_WINDOW window = WowUserFindWindow(window16);
        RECT rect, clientRect, *scrollRect = NULL, *clipRect = NULL;
        BYTE rect16[WOWUSER_RECT16_SIZE];
        INT noteLength = 0, index;
        WowNotePut(note, noteCapacity, &noteLength, "ScrollWindow 0x");
        WowNoteHex(note, noteCapacity, &noteLength, window16, WOW_HEX_WORD_DIGITS);
        if (!window || !window->Window32) { WowNotePut(note, noteCapacity, &noteLength, " -- no real window");
                                Wow32SetReturn(frame, 0); return 1; }
        /* ⚠ BOTH RECTS ARE OPTIONAL AND NULL MEANS SOMETHING: a null scroll rect
             scrolls the whole client area, and a null clip rect clips to none.
             Substituting an empty RECT would scroll nothing, silently. */
        if (rectBytes) { for (index = 0; index < WOWUSER_RECT16_SIZE; ++index) rect16[index] = (BYTE)rectBytes[index];
                  rect.left = WowConvRect16Get(rect16,0); rect.top = WowConvRect16Get(rect16,1);
                  rect.right = WowConvRect16Get(rect16,2); rect.bottom = WowConvRect16Get(rect16,3);
                  scrollRect = &rect; }
        if (clipBytes) { for (index = 0; index < WOWUSER_RECT16_SIZE; ++index) rect16[index] = (BYTE)clipBytes[index];
                  clientRect.left = WowConvRect16Get(rect16,0); clientRect.top = WowConvRect16Get(rect16,1);
                  clientRect.right = WowConvRect16Get(rect16,2); clientRect.bottom = WowConvRect16Get(rect16,3);
                  clipRect = &clientRect; }
        ScrollWindow(window->Window32, deltaX, deltaY, scrollRect, clipRect);
        WowNotePut(note, noteCapacity, &noteLength, " -> scrolled ");
        WowNoteHex(note, noteCapacity, &noteLength, (DWORD)deltaX, WOW_HEX_WORD_DIGITS); WowNotePut(note, noteCapacity, &noteLength, ",");
        WowNoteHex(note, noteCapacity, &noteLength, (DWORD)deltaY, WOW_HEX_WORD_DIGITS);
        Wow32SetReturn(frame, 1);
        return 1;
    }

    case WOWUSER_GETSCROLLRANGE: {
        WORD window16 = Wow32ArgWord(frame, WOWUSER_GSR_ARG_HWND);
        WORD bar  = Wow32ArgWord(frame, WOWUSER_GSR_ARG_BAR);
        volatile BYTE *minimumBytes = Wow32ArgPointer(frame, WOWUSER_GSR_ARG_MIN);
        volatile BYTE *maximumBytes = Wow32ArgPointer(frame, WOWUSER_GSR_ARG_MAX);
        PWOWUSER_WINDOW window = WowUserFindWindow(window16);
        INT noteLength = 0, minimum = 0, maximum = 0;
        WowNotePut(note, noteCapacity, &noteLength, "GetScrollRange 0x");
        WowNoteHex(note, noteCapacity, &noteLength, window16, WOW_HEX_WORD_DIGITS);
        if (!window || !window->Window32) { WowNotePut(note, noteCapacity, &noteLength, " -- no real window");
                                Wow32SetReturn(frame, 0); return 1; }
        GetScrollRange(window->Window32, (INT)(SHORT)bar, &minimum, &maximum);
        if (minimumBytes) Wow32PokeWord(minimumBytes, (WORD)(SHORT)minimum);
        if (maximumBytes) Wow32PokeWord(maximumBytes, (WORD)(SHORT)maximum);
        WowNotePut(note, noteCapacity, &noteLength, " -> "); WowNoteHex(note, noteCapacity, &noteLength, (DWORD)minimum, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, ".."); WowNoteHex(note, noteCapacity, &noteLength, (DWORD)maximum, WOW_HEX_WORD_DIGITS);
        Wow32SetReturn(frame, 1);
        return 1;
    }

    case WOWUSER_ENABLESCROLLBAR: {
        WORD arrows = Wow32ArgWord(frame, WOWUSER_ESB_ARG_ARROWS);
        WORD bar    = Wow32ArgWord(frame, WOWUSER_ESB_ARG_BAR);
        WORD window16   = Wow32ArgWord(frame, WOWUSER_ESB_ARG_HWND);
        PWOWUSER_WINDOW window = WowUserFindWindow(window16);
        INT noteLength = 0; BOOL result;
        WowNotePut(note, noteCapacity, &noteLength, "EnableScrollBar 0x");
        WowNoteHex(note, noteCapacity, &noteLength, window16, WOW_HEX_WORD_DIGITS);
        if (!window || !window->Window32) { WowNotePut(note, noteCapacity, &noteLength, " -- no real window");
                                Wow32SetReturn(frame, 0); return 1; }
        result = EnableScrollBar(window->Window32, (UINT)bar, (UINT)arrows);
        WowNotePut(note, noteCapacity, &noteLength, " bar=0x"); WowNoteHex(note, noteCapacity, &noteLength, bar, WOW_HEX_BYTE_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, " arrows=0x"); WowNoteHex(note, noteCapacity, &noteLength, arrows, WOW_HEX_BYTE_DIGITS);
        Wow32SetReturn(frame, result ? 1 : 0);
        return 1;
    }

    case WOWUSER_SHOWSCROLLBAR: {
        WORD window16 = Wow32ArgWord(frame, WOWUSER_SSB_ARG_HWND);
        WORD bar  = Wow32ArgWord(frame, WOWUSER_SSB_ARG_BAR);
        WORD show = Wow32ArgWord(frame, WOWUSER_SSB_ARG_SHOW);
        PWOWUSER_WINDOW window = WowUserFindWindow(window16);
        INT noteLength = 0;
        WowNotePut(note, noteCapacity, &noteLength, "ShowScrollBar 0x");
        WowNoteHex(note, noteCapacity, &noteLength, window16, WOW_HEX_WORD_DIGITS);
        if (!window || !window->Window32) { WowNotePut(note, noteCapacity, &noteLength, " -- no real window");
                                Wow32SetReturn(frame, 0); return 1; }
        ShowScrollBar(window->Window32, (INT)(SHORT)bar, show ? TRUE : FALSE);
        WowNotePut(note, noteCapacity, &noteLength, show ? " -> shown" : " -> hidden");
        Wow32SetReturn(frame, 1);
        return 1;
    }

    /* ── ★★★★ THE COMM FAMILY, ON THE REAL UART NOW. (session 56) ────────────
         This whole block used to answer IE_BADID, and its note gave a good
         reason: "the equipment word claims none, on purpose". THAT REASON WAS
         TRUE AND I MADE IT FALSE EARLIER THIS SESSION -- the equipment word now
         reports SER=2, the BDA carries 0x03F8/0x02F8, and there is a real 8250
         behind them (GH #9). A host that advertises two serial ports and then
         refuses to open either is the same two-layers-disagreeing fault the
         equipment word itself was fixed for, pointing the other way. So these
         now go to the SAME device the ports and INT 14h use.
       ⚠ THE REFUSAL WAS NOT WRONG WHEN IT WAS WRITTEN, and that is worth
         keeping in view: it was the honest answer to the machine as it then was.
         What went stale was the machine, not the reasoning. */
    case WOWUSER_OPENCOMM: {
        CHAR device[WOWUSER_SHORT_NAME_SIZE];
        INT noteLength = 0, portId;
        if (!Wow32ArgString(frame, WOWUSER_OC_ARG_DEV, device, sizeof device)) device[0] = 0;
        portId = WowCommOpen(device[0] ? device : NULL);
        WowNotePut(note, noteCapacity, &noteLength, "OpenComm \"");
        WowNotePut(note, noteCapacity, &noteLength, device);
        WowNotePut(note, noteCapacity, &noteLength, "\" -> ");
        if (portId < 0) {
            WowNotePut(note, noteCapacity, &noteLength, portId == WOWUSER_COMM_ALREADY_OPEN ? "IE_OPEN (already open)"
                                                : "IE_BADID (no such port here)");
        } else {
            WowNotePut(note, noteCapacity, &noteLength, "comm id 0x");
            WowNoteHex(note, noteCapacity, &noteLength, (DWORD)portId, WOW_HEX_BYTE_DIGITS);
        }
        Wow32SetReturn(frame, (DWORD)(WORD)(SHORT)portId);
        return 1;
    }
    case WOWUSER_CLOSECOMM: {
        INT noteLength = 0, portId = (SHORT)Wow32ArgWord(frame, WOWUSER_CC_ARG_ID);
        INT result = WowCommClose(portId);
        WowNotePut(note, noteCapacity, &noteLength, "CloseComm -> ");
        WowNotePut(note, noteCapacity, &noteLength, result == 0 ? "closed" : "IE_BADID");
        Wow32SetReturn(frame, (DWORD)(WORD)(SHORT)result);
        return 1;
    }
    case WOWUSER_READCOMM: {
        INT noteLength = 0, portId = (SHORT)Wow32ArgWord(frame, WOWUSER_RC_ARG_ID);
        INT size = (SHORT)Wow32ArgWord(frame, WOWUSER_RC_ARG_CB), got;
        volatile BYTE *destination = Wow32ArgPointer(frame, WOWUSER_RC_ARG_BUF);
        BYTE buffer[WOWUSER_COMM_BUFFER_SIZE];
        INT index;
        if (!destination || size <= 0) { Wow32SetReturn(frame, 0); return 1; }
        if (size > (INT)sizeof buffer) size = (INT)sizeof buffer;
        got = WowCommRead(portId, buffer, size);
        if (got > 0) for (index = 0; index < got; ++index) destination[index] = buffer[index];
        WowNotePut(note, noteCapacity, &noteLength, "ReadComm -> 0x");
        WowNoteHex(note, noteCapacity, &noteLength, (DWORD)(got < 0 ? 0 : got), WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, " byte(s)");
        Wow32SetReturn(frame, (DWORD)(WORD)(SHORT)(got < 0 ? 0 : got));
        return 1;
    }
    case WOWUSER_WRITECOMM: {
        INT noteLength = 0, portId = (SHORT)Wow32ArgWord(frame, WOWUSER_RC_ARG_ID);
        INT size = (SHORT)Wow32ArgWord(frame, WOWUSER_RC_ARG_CB), written;
        volatile BYTE *source = Wow32ArgPointer(frame, WOWUSER_RC_ARG_BUF);
        BYTE buffer[WOWUSER_COMM_BUFFER_SIZE];
        INT index;
        if (!source || size <= 0) { Wow32SetReturn(frame, 0); return 1; }
        if (size > (INT)sizeof buffer) size = (INT)sizeof buffer;
        for (index = 0; index < size; ++index) buffer[index] = source[index];
        written = WowCommWrite(portId, buffer, size);
        WowNotePut(note, noteCapacity, &noteLength, "WriteComm -> 0x");
        WowNoteHex(note, noteCapacity, &noteLength, (DWORD)(written < 0 ? 0 : written), WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, " byte(s) out of the port");
        Wow32SetReturn(frame, (DWORD)(WORD)(SHORT)(written < 0 ? 0 : written));
        return 1;
    }
    case WOWUSER_TRANSMITCHAR: {
        INT noteLength = 0, portId = (SHORT)Wow32ArgWord(frame, WOWUSER_TC_ARG_ID);
        BYTE character = (BYTE)Wow32ArgWord(frame, WOWUSER_TC_ARG_CH);
        INT result = WowCommWrite(portId, &character, 1);
        WowNotePut(note, noteCapacity, &noteLength, "TransmitCommChar -> ");
        WowNotePut(note, noteCapacity, &noteLength, result == 1 ? "sent" : "IE_BADID");
        Wow32SetReturn(frame, (DWORD)(WORD)(SHORT)(result == 1 ? 0 : WOWUSER_COMM_FAILED));
        return 1;
    }
    case WOWUSER_GETCOMMERROR: {
        /* COMSTAT is { BYTE status; UINT cbInQue; UINT cbOutQue; } and the
           RETURN is the error mask -- 0 meaning no error. A guest polls this to
           decide whether there is anything to read, so cbInQue must be the real
           queue depth and not a placeholder. cbOutQue is 0 because our
           transmitter never holds a byte (see the vdd_comm header). */
        INT noteLength = 0, portId = (SHORT)Wow32ArgWord(frame, WOWUSER_GCE_ARG_ID);
        volatile BYTE *statBytes = Wow32ArgPointer(frame, WOWUSER_GCE_ARG_STAT);
        INT inQueue = WowCommInqueue(portId);
        if (statBytes) { statBytes[0] = 0;
                  statBytes[WOWUSER_COMSTAT16_INQUEUE] = (BYTE)(inQueue & BYTE_MASK); statBytes[WOWUSER_COMSTAT16_INQUEUE + 1] = (BYTE)((inQueue >> BYTE_SHIFT) & BYTE_MASK);
                  statBytes[WOWUSER_COMSTAT16_OUTQUEUE] = 0; statBytes[WOWUSER_COMSTAT16_OUTQUEUE + 1] = 0; }
        WowNotePut(note, noteCapacity, &noteLength, "GetCommError -> 0 (no error), cbInQue=0x");
        WowNoteHex(note, noteCapacity, &noteLength, (DWORD)inQueue, WOW_HEX_WORD_DIGITS);
        Wow32SetReturn(frame, 0);
        return 1;
    }
    case WOWUSER_SETCOMMBREAK:
    case WOWUSER_CLEARCOMMBREAK: {
        INT noteLength = 0, portId = (SHORT)Wow32ArgWord(frame, WOWUSER_CC_ARG_ID);
        INT isSet = (frame->Id == WOWUSER_SETCOMMBREAK);
        /* LCR bit 6 is the break-control bit on an 8250. We do not model the
           line itself -- there is no wire -- so this is recorded and answered
           rather than pretended: the guest gets the success a real driver
           expects, and the log says the break went nowhere. */
        WowNotePut(note, noteCapacity, &noteLength, isSet ? "SetCommBreak" : "ClearCommBreak");
        WowNotePut(note, noteCapacity, &noteLength, " -- accepted; there is no wire to break, so"
                                   " the state is recorded and not transmitted");
        Wow32SetReturn(frame, (DWORD)(WORD)(SHORT)(portId >= 0 ? 0 : WOWUSER_COMM_FAILED));
        return 1;
    }
    case WOWUSER_ESCAPECOMMFN: {
        INT noteLength = 0, portId = (SHORT)Wow32ArgWord(frame, WOWUSER_ECF_ARG_ID);
        WORD function = Wow32ArgWord(frame, WOWUSER_ECF_ARG_FN);
        /* SETDTR 5, CLRDTR 6, SETRTS 3, CLRRTS 4 -- straight onto MCR, which is
           where they go on real hardware, so a guest that asserts DTR and reads
           MSR back in loopback sees DSR exactly as the port test does. */
        switch (function) {
        case SETDTR: WowCommDtr(portId, 1); break;  /* SETDTR */
        case CLRDTR: WowCommDtr(portId, 0); break;  /* CLRDTR */
        case SETRTS: WowCommRts(portId, 1); break;  /* SETRTS */
        case CLRRTS: WowCommRts(portId, 0); break;  /* CLRRTS */
        default: break;
        }
        WowNotePut(note, noteCapacity, &noteLength, "EscapeCommFunction fn=0x");
        WowNoteHex(note, noteCapacity, &noteLength, function, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, " -> MCR");
        Wow32SetReturn(frame, 0);
        return 1;
    }
    case WOWUSER_SETCOMMSTATE:
    case WOWUSER_GETCOMMSTATE:
    case WOWUSER_FLUSHCOMM: {
        /* ⚠ ANSWERED SUCCESS, AND THE DCB IS NOT MODELLED -- said here rather
             than implied. Baud, parity and stop bits pace nothing in this host
             (vdd_comm.h explains why: there is no wire whose timing must be
             met), so accepting a DCB and reporting success is the truthful
             answer about what will happen to the guest's bytes. Refusing would
             be false in the other direction now that the port exists. */
        INT noteLength = 0;
        WowNotePut(note, noteCapacity, &noteLength, "COMM id=0x");
        WowNoteHex(note, noteCapacity, &noteLength, frame->Id, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, " -- accepted (the DCB is stored by the port,"
                                   " and baud paces nothing here by design)");
        Wow32SetReturn(frame, 0);
        return 1;
    }

    /* ── 0xd0 SetCommEventMask(idComDev, fuEvtMask) -- returns a FAR POINTER to
         the port's event word, which the caller then polls directly.
       ⚠ ITS FAILURE VALUE IS A NULL POINTER, NOT IE_BADID. Grouping it with the
         calls above would hand back 0x0000FFFE, and a guest that dereferenced
         that would read offset 0xFFFE of a null selector rather than seeing an
         error -- the sentinel-that-means-yes shape this project keeps paying
         for. There is no port, so there is no event word, so the answer is 0. */
    case WOWUSER_SETCOMMEVTMASK: {
        /* Returns a FAR POINTER to the port's event word, which the caller polls
           directly -- so its failure value is a NULL POINTER, not IE_BADID.
           Grouping it with the calls above would hand back 0x0000FFFE, and a
           guest that dereferenced that would read offset 0xFFFE of a null
           selector rather than see an error: the sentinel-that-means-yes shape
           this project keeps paying for.
         ⚠ WE STILL RETURN NULL, AND THAT IS STILL THE TRUE ANSWER. The port is
           real now, but nothing in this host RAISES a comm event -- there is no
           peer to signal one. A pointer to a word that never changes would be
           worse than no pointer: a guest that waits on it waits forever, where a
           null makes it fall back to polling GetCommError, which does work. */
        INT noteLength = 0;
        WowNotePut(note, noteCapacity, &noteLength, "SetCommEventMask -- the port is real but no"
                                   " comm EVENT is ever raised here, so a NULL"
                                   " far pointer (poll GetCommError instead);"
                                   " NOT IE_BADID, which at this site is an"
                                   " address");
        Wow32SetReturn(frame, 0);
        return 1;
    }

    case WOWUSER_SETWINDOWPLACEMENT: {
        WORD window16 = Wow32ArgWord(frame, WOWUSER_GWP_ARG_HWND);
        volatile BYTE *bytes16 = Wow32ArgPointer(frame, WOWUSER_GWP_ARG_PL);
        PWOWUSER_WINDOW window = WowUserFindWindow(window16);
        WINDOWPLACEMENT placement;
        INT noteLength = 0, result;
        WowNotePut(note, noteCapacity, &noteLength, "SetWindowPlacement 0x");
        WowNoteHex(note, noteCapacity, &noteLength, window16, WOW_HEX_WORD_DIGITS);
        if (!window || !window->Window32 || !bytes16) {
            WowNotePut(note, noteCapacity, &noteLength, " -- no real window or no struct; FALSE");
            Wow32SetReturn(frame, 0); return 1;
        }
        /* ⚠ 22 bytes of WORDs in, 44 bytes of LONGs out. Same conversion as
             GetWindowPlacement, in the other direction. */
        placement.length           = sizeof placement;
        placement.flags            = Wow32PeekWord(bytes16 + WOWUSER_WP16_FLAGS);
        placement.showCmd          = Wow32PeekWord(bytes16 + WOWUSER_WP16_SHOWCMD);
        placement.ptMinPosition.x  = (SHORT)Wow32PeekWord(bytes16 + WOWUSER_WP16_MIN_X);
        placement.ptMinPosition.y  = (SHORT)Wow32PeekWord(bytes16 + WOWUSER_WP16_MIN_Y);
        placement.ptMaxPosition.x  = (SHORT)Wow32PeekWord(bytes16 + WOWUSER_WP16_MAX_X);
        placement.ptMaxPosition.y  = (SHORT)Wow32PeekWord(bytes16 + WOWUSER_WP16_MAX_Y);
        placement.rcNormalPosition.left   = (SHORT)Wow32PeekWord(bytes16 + WOWUSER_WP16_NORMAL_LEFT);
        placement.rcNormalPosition.top    = (SHORT)Wow32PeekWord(bytes16 + WOWUSER_WP16_NORMAL_TOP);
        placement.rcNormalPosition.right  = (SHORT)Wow32PeekWord(bytes16 + WOWUSER_WP16_NORMAL_RIGHT);
        placement.rcNormalPosition.bottom = (SHORT)Wow32PeekWord(bytes16 + WOWUSER_WP16_NORMAL_BOTTOM);
        result = SetWindowPlacement(window->Window32, &placement) ? 1 : 0;
        WowNotePut(note, noteCapacity, &noteLength, result ? " -> placed" : " -> REFUSED");
        Wow32SetReturn(frame, (DWORD)result);
        return 1;
    }

    /* ── ⚠⚠⚠ ExitWindows IS REFUSED, ALWAYS. A Win16 guest asking to end the
         Windows session must NOT be able to log the user out or restart the real
         machine -- that is the whole desktop, not this VDM, and the guest cannot
         tell the difference between "refused" and "the user said no", which is a
         documented outcome of this call. Returning FALSE is a legal answer that
         every caller already handles. */
    case WOWUSER_EXITWINDOWS: {
        INT noteLength = 0;
        WowNotePut(note, noteCapacity, &noteLength, "ExitWindows -- ★ REFUSED. This would end the "
                                   "REAL user's session, not the VDM. FALSE is a "
                                   "documented outcome (the user declined)");
        Wow32SetReturn(frame, 0);
        return 1;
    }

    case WOWUSER_DEFFRAMEPROC:
    case WOWUSER_DEFMDICHILDPROC: {
        INT  isFrameProc = (frame->Id == WOWUSER_DEFFRAMEPROC);
        WORD window16  = Wow32ArgWord(frame, isFrameProc ? WOWUSER_DFP_ARG_HWND : WOWUSER_DDP_ARG_HDLG);
        WORD client16  = isFrameProc ? Wow32ArgWord(frame, WOWUSER_DFP_ARG_HCLIENT) : 0;
        WORD message16   = Wow32ArgWord(frame, isFrameProc ? WOWUSER_DFP_ARG_MSG    : WOWUSER_DDP_ARG_MSG);
        WORD wParam16  = Wow32ArgWord(frame, isFrameProc ? WOWUSER_DFP_ARG_WPARAM : WOWUSER_DDP_ARG_WPARAM);
        DWORD lParam32 = Wow32ArgDword(frame, isFrameProc ? WOWUSER_DFP_ARG_LPARAM : WOWUSER_DDP_ARG_LPARAM);
        PWOWUSER_WINDOW window = WowUserFindWindow(window16);
        PWOWUSER_WINDOW child = client16 ? WowUserFindWindow(client16) : NULL;
        LRESULT result;
        INT noteLength = 0;
        WowNotePut(note, noteCapacity, &noteLength, isFrameProc ? "DefFrameProc 0x" : "DefMDIChildProc 0x");
        WowNoteHex(note, noteCapacity, &noteLength, window16, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, " msg=0x"); WowNoteHex(note, noteCapacity, &noteLength, message16, WOW_HEX_WORD_DIGITS);
        if (!window || !window->Window32) { WowNotePut(note, noteCapacity, &noteLength, " -- no real window; 0");
                                Wow32SetReturn(frame, 0); return 1; }
        /* s92: the defaults DefWindowProc already takes over (see 0x6b), which both MDI
           defaults fall back to in Win16. WM_CLOSE on a FRAME is DestroyWindow -- OURS:
           Program Manager's X reached Win32's DefFrameProc, which destroyed the real
           window only, and the task sat in GetMessage with no window, host and all
           (runs/s92/inst1_now.log). WM_PAINT erases what is owed; WM_CTLCOLOR is 0. */
        if (message16 == WM_CLOSE && isFrameProc) {
            WowNotePut(note, noteCapacity, &noteLength, " -> WM_CLOSE: ");
            WowUserDestroy(window16, note, noteCapacity, &noteLength);
            Wow32SetReturn(frame, 0);
            return 1;
        }
        if (message16 == WM_PAINT) {
            WowUserDefaultPaint(window, 0);
            WowNotePut(note, noteCapacity, &noteLength, " -> WM_PAINT: erased what was owed (class brush)");
            Wow32SetReturn(frame, 0);
            return 1;
        }
        if (message16 == WM_CTLCOLOR16) {
            WORD brush16 = WowUserCtlColorDefault(window, window->Window32, wParam16, lParam32);
            WowNotePut(note, noteCapacity, &noteLength, " -> WM_CTLCOLOR: the default brush, token 0x");
            WowNoteHex(note, noteCapacity, &noteLength, brush16, WOW_HEX_WORD_DIGITS);
            Wow32SetReturn(frame, brush16);
            return 1;
        }
        /* The OS's own MDI defaults, on the real windows -- the same argument as
           using the real MDICLIENT rather than drawing one. */
        result = WowUserDef32(isFrameProc ? WOWUSER_DEF_FRAME : WOWUSER_DEF_MDICHILD, window->Window32, child ? child->Window32 : NULL, message16, wParam16, lParam32);
        WowNotePut(note, noteCapacity, &noteLength, " -> 0x"); WowNoteHex(note, noteCapacity, &noteLength, (DWORD)result, WOW_HEX_DWORD_DIGITS);
        Wow32SetReturn(frame, (DWORD)result);
        return 1;
    }

    case WOWUSER_TABBEDTEXTOUT: {
        WORD dc16 = Wow32ArgWord(frame, WOWUSER_TTO_ARG_HDC);
        INT  positionX   = (INT)(SHORT)Wow32ArgWord(frame, WOWUSER_TTO_ARG_X);
        INT  positionY   = (INT)(SHORT)Wow32ArgWord(frame, WOWUSER_TTO_ARG_Y);
        INT  count   = (INT)(SHORT)Wow32ArgWord(frame, WOWUSER_TTO_ARG_COUNT);
        INT  tabCount= (INT)(SHORT)Wow32ArgWord(frame, WOWUSER_TTO_ARG_TABCNT);
        INT  tabOrigin= (INT)(SHORT)Wow32ArgWord(frame, WOWUSER_TTO_ARG_TABORG);
        volatile BYTE *textBytes = Wow32ArgPointer(frame, WOWUSER_TTO_ARG_STR);
        volatile BYTE *tabBytes = Wow32ArgPointer(frame, WOWUSER_TTO_ARG_TABPOS);
        INT  dcKind = -1;
        HGDIOBJ dc = WowGdiH32(dc16, &dcKind);
        CHAR buffer[WOWUSER_LONG_TEXT_SIZE];
        INT  tabStops[WOWUSER_MAX_TEXT_TABS];
        LONG result;
        INT  noteLength = 0, index;
        WowNotePut(note, noteCapacity, &noteLength, "TabbedTextOut(0x");
        WowNoteHex(note, noteCapacity, &noteLength, dc16, WOW_HEX_WORD_DIGITS);
        if (!dc || (dcKind != WOWGDI_KIND_DC && dcKind != WOWGDI_KIND_WINDC)) {
            WowNotePut(note, noteCapacity, &noteLength, ") -- ★ NOT ONE OF OUR DC TOKENS; 0");
            Wow32SetReturn(frame, 0); return 1;
        }
        if (count < 0) count = 0;
        if (count > (INT)sizeof buffer) count = (INT)sizeof buffer;
        for (index = 0; index < count; ++index) buffer[index] = textBytes ? (CHAR)textBytes[index] : ' ';
        if (tabCount < 0) tabCount = 0;
        if (tabCount > WOWUSER_MAX_TEXT_TABS) tabCount = WOWUSER_MAX_TEXT_TABS;
        for (index = 0; index < tabCount; ++index)
            tabStops[index] = tabBytes ? (INT)(SHORT)Wow32PeekWord(tabBytes + index * WOW_WORD_BYTES) : 0;
        result = TabbedTextOutA((HDC)dc, positionX, positionY, buffer, count, tabCount, (tabCount && tabBytes) ? tabStops : NULL, tabOrigin);
        WowNotePut(note, noteCapacity, &noteLength, ") -> extent 0x"); WowNoteHex(note, noteCapacity, &noteLength, (DWORD)result, WOW_HEX_DWORD_DIGITS);
        Wow32SetReturn(frame, (DWORD)result);
        return 1;
    }

    case WOWUSER_ISWINDOW:
    case WOWUSER_ISWINDOWVISIBLE: {
        INT  wantVisible = (frame->Id == WOWUSER_ISWINDOWVISIBLE);
        WORD window16 = Wow32ArgWord(frame, WOWUSER_IW_ARG_HWND);
        PWOWUSER_WINDOW window = WowUserFindWindow(window16);
        INT  noteLength = 0, result;
        WowNotePut(note, noteCapacity, &noteLength, wantVisible ? "IsWindowVisible 0x" : "IsWindow 0x");
        WowNoteHex(note, noteCapacity, &noteLength, window16, WOW_HEX_WORD_DIGITS);
        if (!window || !window->Window32) {
            WowNotePut(note, noteCapacity, &noteLength, " -- ★ NOT A WINDOW OF OURS; FALSE");
            Wow32SetReturn(frame, 0);
            return 1;
        }
        result = wantVisible ? (IsWindowVisible(window->Window32) ? 1 : 0)
                    : (IsWindow(window->Window32) ? 1 : 0);
        WowNotePut(note, noteCapacity, &noteLength, result ? " -> TRUE" : " -> FALSE");
        Wow32SetReturn(frame, (DWORD)result);
        return 1;
    }

    /* ── ★★★★★ 0x21 GetClientRect(hWnd, lpRect) -- see the long note above. ─*/
    case WOWUSER_GETCLIENTRECT: {
        WORD window16 = Wow32ArgWord(frame, WOWUSER_GCR_ARG_HWND);
        volatile BYTE *rectBytes = Wow32ArgPointer(frame, WOWUSER_GCR_ARG_RECT);
        PWOWUSER_WINDOW window = WowUserFindWindow(window16);
        RECT clientRect;
        INT noteLength = 0;
        WowNotePut(note, noteCapacity, &noteLength, "GetClientRect 0x");
        WowNoteHex(note, noteCapacity, &noteLength, window16, WOW_HEX_WORD_DIGITS);
        if (!rectBytes) {
            WowNotePut(note, noteCapacity, &noteLength, " -- ★ NULL lpRect; nothing written");
            Wow32SetReturn(frame, 0);
            return 1;
        }
        if (!window || !window->Window32 || !GetClientRect(window->Window32, &clientRect)) {
            /* ⚠ ZERO IT RATHER THAN LEAVE IT. An unwritten RECT is the caller's
                 stack litter, and a guest laying out from litter is worse than
                 one laying out from an empty rectangle -- the second is at least
                 visibly wrong. */
            INT index;
            for (index = 0; index < WOWUSER_RECT16_SIZE; ++index) rectBytes[index] = 0;
            WowNotePut(note, noteCapacity, &noteLength, " -- ★ NO SUCH WINDOW; zeroed");
            Wow32SetReturn(frame, 0);
            return 1;
        }
        Wow32PokeWord(rectBytes + 0, (WORD)(SHORT)clientRect.left);
        Wow32PokeWord(rectBytes + WOWUSER_RECT16_TOP, (WORD)(SHORT)clientRect.top);
        Wow32PokeWord(rectBytes + WOWUSER_RECT16_RIGHT, (WORD)(SHORT)clientRect.right);
        Wow32PokeWord(rectBytes + WOWUSER_RECT16_BOTTOM, (WORD)(SHORT)clientRect.bottom);
        WowNotePut(note, noteCapacity, &noteLength, " -> ");
        WowNoteHex(note, noteCapacity, &noteLength, (DWORD)clientRect.right, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, "x");
        WowNoteHex(note, noteCapacity, &noteLength, (DWORD)clientRect.bottom, WOW_HEX_WORD_DIGITS);
        Wow32SetReturn(frame, 0);
        return 1;
    }

    /* ── ★★ 0x40 SetScrollRange / 0x3e SetScrollPos -- the canvas scrollbars. */
    case WOWUSER_SETSCROLLRANGE: {
        WORD window16 = Wow32ArgWord(frame, WOWUSER_SSR_ARG_HWND);
        WORD bar  = Wow32ArgWord(frame, WOWUSER_SSR_ARG_BAR);
        INT  minimum   = (INT)(SHORT)Wow32ArgWord(frame, WOWUSER_SSR_ARG_MIN);
        INT  maximum   = (INT)(SHORT)Wow32ArgWord(frame, WOWUSER_SSR_ARG_MAX);
        WORD redraw  = Wow32ArgWord(frame, WOWUSER_SSR_ARG_REDRAW);
        PWOWUSER_WINDOW window = WowUserFindWindow(window16);
        INT noteLength = 0;
        WowNotePut(note, noteCapacity, &noteLength, "SetScrollRange 0x");
        WowNoteHex(note, noteCapacity, &noteLength, window16, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, bar == 0 ? " SB_HORZ " : bar == 1 ? " SB_VERT "
                                                                     : " SB_CTL ");
        WowNoteHex(note, noteCapacity, &noteLength, (DWORD)minimum, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, "..");
        WowNoteHex(note, noteCapacity, &noteLength, (DWORD)maximum, WOW_HEX_WORD_DIGITS);
        if (!window || !window->Window32) {
            WowNotePut(note, noteCapacity, &noteLength, " -- ★ NO SUCH WINDOW");
            Wow32SetReturn(frame, 0);
            return 1;
        }
        SetScrollRange(window->Window32, (INT)bar, minimum, maximum, redraw ? TRUE : FALSE);
        WowNotePut(note, noteCapacity, &noteLength, " -> the OS's");
        Wow32SetReturn(frame, 0);
        return 1;
    }

    case WOWUSER_SETSCROLLPOS: {
        WORD window16 = Wow32ArgWord(frame, WOWUSER_SSP_ARG_HWND);
        WORD bar  = Wow32ArgWord(frame, WOWUSER_SSP_ARG_BAR);
        INT  position  = (INT)(SHORT)Wow32ArgWord(frame, WOWUSER_SSP_ARG_POS);
        WORD redraw  = Wow32ArgWord(frame, WOWUSER_SSP_ARG_REDRAW);
        PWOWUSER_WINDOW window = WowUserFindWindow(window16);
        INT noteLength = 0, previousPosition = 0;
        WowNotePut(note, noteCapacity, &noteLength, "SetScrollPos 0x");
        WowNoteHex(note, noteCapacity, &noteLength, window16, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, bar == 0 ? " SB_HORZ " : bar == 1 ? " SB_VERT "
                                                                     : " SB_CTL ");
        WowNoteHex(note, noteCapacity, &noteLength, (DWORD)position, WOW_HEX_WORD_DIGITS);
        if (!window || !window->Window32) {
            WowNotePut(note, noteCapacity, &noteLength, " -- ★ NO SUCH WINDOW");
            Wow32SetReturn(frame, 0);
            return 1;
        }
        previousPosition = SetScrollPos(window->Window32, (INT)bar, position, redraw ? TRUE : FALSE);
        WowNotePut(note, noteCapacity, &noteLength, " -> previous ");
        WowNoteHex(note, noteCapacity, &noteLength, (DWORD)previousPosition, WOW_HEX_WORD_DIGITS);
        Wow32SetReturn(frame, (DWORD)(WORD)(SHORT)previousPosition);
        return 1;
    }

    /* ── ★ 0x45 SetCursor(hCursor) ──────────────────────────────────────────
       ⚠ NULL IS A REAL ARGUMENT and means "no cursor", which is what Paint
         passes while it is busy. It is passed through rather than treated as a
         missing token. */
    case WOWUSER_SETCURSOR: {
        WORD cursor16 = Wow32ArgWord(frame, WOWUSER_SC_ARG_HCURSOR);
        INT  noteLength = 0;
        HCURSOR cursor = NULL;
        WowNotePut(note, noteCapacity, &noteLength, "SetCursor 0x");
        WowNoteHex(note, noteCapacity, &noteLength, cursor16, WOW_HEX_WORD_DIGITS);
        if (cursor16) {
            /* s89 (#216): the same resolver RegisterClass uses -- predefined,
               the module's own by name or ordinal, or built from USER's bytes. */
            cursor = WowUserSystemResourceCursor(cursor16, NULL);
            if (!cursor)
                WowNotePut(note, noteCapacity, &noteLength, " -- ★ NOT A CURSOR TOKEN WE CAN BUILD;"
                                           " the cursor is left alone");
        } else {
            WowNotePut(note, noteCapacity, &noteLength, " (NULL -- hide)");
        }
        if (cursor || !cursor16) SetCursor(cursor);
        Wow32SetReturn(frame, 0);
        return 1;
    }

    /* ── ★★★★★ 0x27 BeginPaint / 0x28 EndPaint -- see the long note above. ──*/
    case WOWUSER_BEGINPAINT: {
        /* ── ★ PAINT LATENCY: how long the guest left the WM_PAINT sitting. ──
             Reported here because this is the moment the guest finally answers
             it. Large numbers mean the redraw is LATE, not slow; small ones mean
             any remaining sluggishness is in the drawing, which is measured
             separately by WOWPERF. */
        DWORD paintLatency = g_WowWinPaintMs ? (GetTickCount() - g_WowWinPaintMs) : 0;
        WORD window16 = Wow32ArgWord(frame, WOWUSER_BP_ARG_HWND);
        volatile BYTE *paintBytes = Wow32ArgPointer(frame, WOWUSER_BP_ARG_PS);
        PWOWUSER_WINDOW window = WowUserFindWindow(window16);
        RECT rect;
        HDC  dc;
        WORD dc16;
        INT  noteLength = 0, shouldErase = 1, have, index;
        WowNotePut(note, noteCapacity, &noteLength, "BeginPaint 0x");
        WowNotePut(note, noteCapacity, &noteLength, " [waited 0x");
        WowNoteHex(note, noteCapacity, &noteLength, paintLatency, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, " ms]");
        WowNoteHex(note, noteCapacity, &noteLength, window16, WOW_HEX_WORD_DIGITS);
        if (!window || !window->Window32 || !paintBytes) {
            WowNotePut(note, noteCapacity, &noteLength, !paintBytes ? " -- ★ NO PAINTSTRUCT; answered 0"
                                           : " -- ★ NO SUCH WINDOW; answered 0");
            Wow32SetReturn(frame, 0);
            return 1;
        }
        have = WowWinPaintTake(window16, &rect, &shouldErase);
        /* ── ⛔⛔ BeginPaint VALIDATES, OR A GUEST THAT INVALIDATES IN ITS OWN WM_PAINT
             NEVER STOPS PAINTING. (user, s88: "Clock flickers") Clock's WM_PAINT opens
             with `InvalidateRect(hwnd, NULL, TRUE)` and only then calls BeginPaint.
             On Windows that is harmless -- BeginPaint validates the whole update
             region, the fresh one included. Ours took only the paint record, so the
             REAL window stayed dirty: the OS synthesised another WM_PAINT, wowwin's
             relay erased the background and posted it, and Clock invalidated again --
             37,702 paints in one short run, each one an erase the user saw.
           ► So fold whatever the real window still has pending into the rectangle
             we report, and validate it, exactly as the real BeginPaint does. The
             pending part was not erased by anyone (ValidateRect does not erase), so
             it is reported with fErase set and the guest erases it, as it would.
           ⛔ AND THAT ALONE MEASURED NO CHANGE (36,609 paints): the exec loop pumps
             the real windows BETWEEN guest calls (WowWinPump), so the real WM_PAINT
             is usually handled before the guest's BeginPaint arrives -- the relay
             validated the region itself and QUEUED a WM_PAINT16 behind the one being
             answered. The real BeginPaint consumes that: the window's paint is THIS
             paint. So any WM_PAINT16 already queued for this window goes too (its
             rectangle is in the paint record, which WowWinPaintTake just took). */
        {   RECT updateRect; WOWMSG pending; INT dropped = 0;
            if (GetUpdateRect(window->Window32, &updateRect, FALSE)) {
                if (have) UnionRect(&rect, &rect, &updateRect); else rect = updateRect;
                have = 1; shouldErase = 1;
            }
            ValidateRect(window->Window32, NULL);
            while (WowMsgTake(window16, WM_PAINT16, WM_PAINT16, 1, &pending)) ++dropped;
            if (dropped) { WowNotePut(note, noteCapacity, &noteLength, " [queued paints absorbed 0x");
                           WowNoteHex(note, noteCapacity, &noteLength, (DWORD)dropped, WOW_HEX_BYTE_DIGITS);
                           WowNotePut(note, noteCapacity, &noteLength, "]"); }
        }
        if (!have) {
            GetClientRect(window->Window32, &rect);
            shouldErase = 1;
        }
        /* ── s89 (#162, Charmap): CHILDREN CLIPPED OUT. The real controls inside
             this window are OS windows that painted themselves already; Win16's
             order (parent erases and paints, then the children) does not hold here,
             so an unclipped DC let the guest's erase -- now sent from this
             BeginPaint -- wipe Charmap's font list, labels and buttons after they
             had drawn. ReleaseDC (EndPaint) takes a GetDCEx DC the same way. */
        /* Siblings only as the window's own style says (Win16's rule): Sound
           Recorder's text controls sit INSIDE sibling frame controls, and an
           unconditional DCX_CLIPSIBLINGS clipped every letter away. */
        dc = GetDCEx(window->Window32, NULL, DCX_CACHE | DCX_CLIPCHILDREN
                     | ((GetWindowLongA(window->Window32, GWL_STYLE) & WS_CLIPSIBLINGS)
                        ? DCX_CLIPSIBLINGS : 0));
        /* ── #287: CLIPPED TO THE PAINT, AS WIN16's BeginPaint DC IS. Unclipped, the
             erase this BeginPaint now sends (DefWindowProc, Calc's WHITE class brush)
             covered the whole client while Calc repaints its grey over rcPaint only
             -- so an uncovered corner turned the whole calculator white. What this
             reports as rcPaint is exactly what the guest may draw on. */
        if (dc) IntersectClipRect(dc, rect.left, rect.top, rect.right, rect.bottom);
        dc16 = dc ? WowGdiH16((HGDIOBJ)dc, WOWGDI_KIND_WINDC) : 0;
        if (!dc16) {
            if (dc) ReleaseDC(window->Window32, dc);
            WowNotePut(note, noteCapacity, &noteLength, " -- ★ NO DC (or the token map is full);"
                                       " answered 0");
            Wow32SetReturn(frame, 0);
            return 1;
        }
        /* ★ The whole 32 bytes are cleared first: fRestore, fIncUpdate and the
             16-byte reserved tail are all part of what the guest declared, and
             leaving them as stack litter is how a guest ends up branching on
             something nobody wrote. */
        for (index = 0; index < WOWUSER_PS16_SIZE; ++index) paintBytes[index] = 0;
        Wow32PokeWord(paintBytes + WOWUSER_PS16_HDC,   dc16);
        Wow32PokeWord(paintBytes + WOWUSER_PS16_ERASE, (WORD)(shouldErase ? 1 : 0));
        Wow32PokeWord(paintBytes + WOWUSER_PS16_RECT + 0, (WORD)(SHORT)rect.left);
        Wow32PokeWord(paintBytes + WOWUSER_PS16_RECT + WOWUSER_RECT16_TOP, (WORD)(SHORT)rect.top);
        Wow32PokeWord(paintBytes + WOWUSER_PS16_RECT + WOWUSER_RECT16_RIGHT, (WORD)(SHORT)rect.right);
        Wow32PokeWord(paintBytes + WOWUSER_PS16_RECT + WOWUSER_RECT16_BOTTOM, (WORD)(SHORT)rect.bottom);
        WowNotePut(note, noteCapacity, &noteLength, have ? " rect(" : " whole client rect(");
        WowNoteHex(note, noteCapacity, &noteLength, (DWORD)rect.left, WOW_HEX_WORD_DIGITS);   WowNotePut(note, noteCapacity, &noteLength, ",");
        WowNoteHex(note, noteCapacity, &noteLength, (DWORD)rect.top, WOW_HEX_WORD_DIGITS);    WowNotePut(note, noteCapacity, &noteLength, ",");
        WowNoteHex(note, noteCapacity, &noteLength, (DWORD)rect.right, WOW_HEX_WORD_DIGITS);  WowNotePut(note, noteCapacity, &noteLength, ",");
        WowNoteHex(note, noteCapacity, &noteLength, (DWORD)rect.bottom, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, ") -> DC token 0x");
        WowNoteHex(note, noteCapacity, &noteLength, dc16, WOW_HEX_WORD_DIGITS);
        Wow32SetReturn(frame, dc16);
        /* ── #282/#162: AND BeginPaint SENDS WM_ERASEBKGND, as Win16's does, before
             it returns -- with the paint's own DC. Clock paints its face colour in
             that handler and nowhere else (its class has no brush; the brush it
             uses is created after RegisterClass), so ours stayed WHITE where stock
             is the button face. The relay already erased with the class brush when
             there is one; a procedure that hands this to DefWindowProc gets 0 back
             (not in USER's forward table), i.e. nothing further -- same pixels.
             KEEP: BeginPaint's caller still gets the DC token. fErase is left as
             reported; a guest that also erases in WM_PAINT paints the same colour. */
        if (shouldErase && frame->IsCallbackAllowed && WowUserWindowProcedureOf(window)) {
            WowUserWantMessage(frame, window, window->Instance ? window->Instance : g_WowUserClasses[window->Class].Instance,
                             WM_ERASEBKGND, dc16, 0, WOWCALL_RET_KEEP);
            WowNotePut(note, noteCapacity, &noteLength, " + WM_ERASEBKGND to the window procedure");
        }
        return 1;
    }

    case WOWUSER_ENDPAINT: {
        WORD window16 = Wow32ArgWord(frame, WOWUSER_BP_ARG_HWND);
        volatile BYTE *paintBytes = Wow32ArgPointer(frame, WOWUSER_BP_ARG_PS);
        PWOWUSER_WINDOW window = WowUserFindWindow(window16);
        WORD dc16 = paintBytes ? Wow32PeekWord(paintBytes + WOWUSER_PS16_HDC) : 0;
        INT  dcKind = -1;
        HGDIOBJ dc = WowGdiH32(dc16, &dcKind);
        INT  noteLength = 0;
        WowNotePut(note, noteCapacity, &noteLength, "EndPaint 0x");
        WowNoteHex(note, noteCapacity, &noteLength, window16, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, " dc=0x");
        WowNoteHex(note, noteCapacity, &noteLength, dc16, WOW_HEX_WORD_DIGITS);
        /* ⚠ THE DC MUST GO BACK EVEN IF THE WINDOW HAS GONE. A guest that
             destroys a window inside its own WM_PAINT is rare but legal, and a
             leaked cache DC would eventually stop the OS handing out any. */
        if (dc && dcKind == WOWGDI_KIND_WINDC) {
            ReleaseDC(window ? window->Window32 : NULL, (HDC)dc);
            WowGdiForget(dc16);
            WowNotePut(note, noteCapacity, &noteLength, " -> released");
        } else {
            WowNotePut(note, noteCapacity, &noteLength, " -- ★ THAT IS NOT A DC THIS BeginPaint"
                                       " issued; nothing released");
        }
        Wow32SetReturn(frame, 1);
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
        INT  wantWindow = (frame->Id == WOWUSER_GETWINDOWDC);
        WORD window16 = Wow32ArgWord(frame, WOWUSER_GDC_ARG_HWND16);
        PWOWUSER_WINDOW window = window16 ? WowUserFindWindow(window16) : NULL;
        HDC  dc;
        WORD dc16;
        INT  noteLength = 0;
        WowNotePut(note, noteCapacity, &noteLength, wantWindow ? "GetWindowDC 0x" : "GetDC 0x");
        WowNoteHex(note, noteCapacity, &noteLength, window16, WOW_HEX_WORD_DIGITS);
        if (window16 && (!window || !window->Window32)) {
            WowNotePut(note, noteCapacity, &noteLength, " -- ★ NO SUCH WINDOW; answered 0 (a guest"
                                       " reads a null DC as out of memory)");
            Wow32SetReturn(frame, 0);
            return 1;
        }
        if (!window16) WowNotePut(note, noteCapacity, &noteLength, " (the SCREEN)");
        dc = wantWindow ? GetWindowDC(window ? window->Window32 : NULL)
                     : GetDC(window ? window->Window32 : NULL);
        if (!dc) {
            WowNotePut(note, noteCapacity, &noteLength, " -- ★ THE OS REFUSED THE DC; answered 0");
            Wow32SetReturn(frame, 0);
            return 1;
        }
        dc16 = WowGdiH16((HGDIOBJ)dc, WOWGDI_KIND_WINDC);
        if (!dc16) {
            /* ⚠ Do not hand back a DC we cannot name later: it could never be
                 released, which is the leak this map exists to prevent.
                 GetWindowDC's result goes back through ReleaseDC too. */
            ReleaseDC(window ? window->Window32 : NULL, dc);
            WowNotePut(note, noteCapacity, &noteLength, " -- ★ THE GDI TOKEN MAP IS FULL; the DC was"
                                       " given straight back and 0 answered");
            Wow32SetReturn(frame, 0);
            return 1;
        }
        WowNotePut(note, noteCapacity, &noteLength, " -> DC token 0x");
        WowNoteHex(note, noteCapacity, &noteLength, dc16, WOW_HEX_WORD_DIGITS);
        Wow32SetReturn(frame, dc16);
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
        WORD dc16  = Wow32ArgWord(frame, WOWUSER_RDC_ARG_HDC);
        WORD window16 = Wow32ArgWord(frame, WOWUSER_RDC_ARG_HWND);
        PWOWUSER_WINDOW window = window16 ? WowUserFindWindow(window16) : NULL;
        INT dcKind = -1;
        HGDIOBJ dc = WowGdiH32(dc16, &dcKind);
        INT noteLength = 0, isOk;
        WowNotePut(note, noteCapacity, &noteLength, "ReleaseDC dc=0x");
        WowNoteHex(note, noteCapacity, &noteLength, dc16, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, " hwnd=0x");
        WowNoteHex(note, noteCapacity, &noteLength, window16, WOW_HEX_WORD_DIGITS);
        if (!dc) {
            WowNotePut(note, noteCapacity, &noteLength, " -- ★ NOT ONE OF OUR GDI TOKENS;"
                                       " answered 0");
            Wow32SetReturn(frame, 0);
            return 1;
        }
        if (dcKind != WOWGDI_KIND_WINDC) {
            WowNotePut(note, noteCapacity, &noteLength, " -- ★ THAT DC WAS NOT BORROWED FROM A"
                                       " WINDOW; it must go through DeleteDC."
                                       " Refused");
            Wow32SetReturn(frame, 0);
            return 1;
        }
        isOk = ReleaseDC(window ? window->Window32 : NULL, (HDC)dc) ? 1 : 0;
        if (isOk) WowGdiForget(dc16);
        WowNotePut(note, noteCapacity, &noteLength, isOk ? " -> released, token freed"
                                      : " -- ★ the OS refused the release");
        Wow32SetReturn(frame, (DWORD)isOk);
        return 1;
    }

    /* ── ★ 0x17 GetFocus() -- ask the OS, not our own bookkeeping. ───────────
         g_WowMsgFocus is where this host POSTS a keystroke; the OS's focus is where
         one actually goes, and a caret only blinks in the second. They agree
         because SetFocus sets both, and if they ever disagree that is a defect
         worth seeing rather than papering over -- so the OS answers, and a
         mismatch is printed instead of being silently preferred either way. */
    case WOWUSER_GETFOCUS: {
        WORD focus16 = WowWinHwnd16(GetFocus());
        INT noteLength = 0;
        WowNotePut(note, noteCapacity, &noteLength, "GetFocus -> 0x");
        WowNoteHex(note, noteCapacity, &noteLength, focus16, WOW_HEX_WORD_DIGITS);
        if (focus16 != g_WowMsgFocus) {
            WowNotePut(note, noteCapacity, &noteLength, " -- ⚠ the OS says this and our queue says 0x");
            WowNoteHex(note, noteCapacity, &noteLength, g_WowMsgFocus, WOW_HEX_WORD_DIGITS);
        }
        Wow32SetReturn(frame, focus16);
        return 1;
    }

    /* ── ★ 0x22 EnableWindow(hWnd, bEnable) ─────────────────────────────────
         Notepad disables its window while a modal thing is up. Straight through:
         a disabled real window stops taking real input, which is the whole
         behaviour being asked for and not something this host could imitate. */
    case WOWUSER_ENABLEWINDOW: {
        WORD window16 = Wow32ArgWord(frame, WOWUSER_EW_ARG_HWND);
        WORD enable   = Wow32ArgWord(frame, WOWUSER_EW_ARG_ENABLE);
        PWOWUSER_WINDOW window = WowUserFindWindow(window16);
        INT noteLength = 0;
        WowNotePut(note, noteCapacity, &noteLength, enable ? "EnableWindow ENABLE 0x" : "EnableWindow DISABLE 0x");
        WowNoteHex(note, noteCapacity, &noteLength, window16, WOW_HEX_WORD_DIGITS);
        if (!window || !window->Window32) { WowNotePut(note, noteCapacity, &noteLength, " -- no real window");
                                Wow32SetReturn(frame, 0); return 1; }
        Wow32SetReturn(frame, (DWORD)(EnableWindow(window->Window32, enable ? TRUE : FALSE) ? 1 : 0));
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
        INT noteLength = 0;
        Wow32SetReturn(frame, (DWORD)WowUserDestroy(Wow32ArgWord(frame, WOWUSER_DW_ARG_HWND), note, noteCapacity, &noteLength));
        return 1;
    }

    /* ── ★ 0x3b SetActiveWindow(hWnd) -- returns the PREVIOUS active window. ── */
    case WOWUSER_SETACTIVEWINDOW: {
        WORD window16 = Wow32ArgWord(frame, WOWUSER_SAW_ARG_HWND);
        PWOWUSER_WINDOW window = WowUserFindWindow(window16);
        INT noteLength = 0;
        WORD previousActive = 0;
        WowNotePut(note, noteCapacity, &noteLength, "SetActiveWindow 0x");
        WowNoteHex(note, noteCapacity, &noteLength, window16, WOW_HEX_WORD_DIGITS);
        if (!window || !window->Window32) { WowNotePut(note, noteCapacity, &noteLength, " -- no real window");
                               Wow32SetReturn(frame, 0); return 1; }
        previousActive = WowWinHwnd16(SetActiveWindow(window->Window32));
        WowNotePut(note, noteCapacity, &noteLength, " (was 0x");
        WowNoteHex(note, noteCapacity, &noteLength, previousActive, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, ")");
        Wow32SetReturn(frame, previousActive);
        return 1;
    }

    /* ── ★ 0x68 MessageBeep(uType) ──────────────────────────────────────────
       ⚠ Win16's only documented argument is 0 and Win32's MB_OK is also 0, so
         the pass-through is exact for the one value a Win16 program can pass.
         Notepad beeps at a failed search. */
    case WOWUSER_MESSAGEBEEP: {
        WORD beepType = Wow32ArgWord(frame, WOWUSER_MB_ARG_TYPE);
        INT noteLength = 0;
        WowNotePut(note, noteCapacity, &noteLength, "MessageBeep(0x");
        WowNoteHex(note, noteCapacity, &noteLength, beepType, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, ")");
        MessageBeep((UINT)beepType);
        Wow32SetReturn(frame, 0);
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
        WORD window16 = Wow32ArgWord(frame, WOWUSER_OC_ARG_HWND);
        PWOWUSER_WINDOW window = window16 ? WowUserFindWindow(window16) : NULL;
        INT noteLength = 0, isOk;
        isOk = OpenClipboard(window ? window->Window32 : NULL) ? 1 : 0;
        WowNotePut(note, noteCapacity, &noteLength, "OpenClipboard owner=0x");
        WowNoteHex(note, noteCapacity, &noteLength, window16, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, isOk ? " -> opened (the OS's own)"
                                      : " -> REFUSED (someone else has it open)");
        Wow32SetReturn(frame, (DWORD)isOk);
        return 1;
    }

    case WOWUSER_CLOSECLIPBOARD: {
        INT noteLength = 0;
        INT isOk = CloseClipboard() ? 1 : 0;
        WowNotePut(note, noteCapacity, &noteLength, isOk ? "CloseClipboard -> closed"
                                      : "CloseClipboard -> it was not open");
        Wow32SetReturn(frame, (DWORD)isOk);
        return 1;
    }

    /* Enumerate from `wFormat`, 0 to start. Returns 0 at the end, which is the
       loop's termination condition, so a wrong answer here spins a guest. */
    case WOWUSER_ENUMCLIPFMT: {
        WORD format = Wow32ArgWord(frame, WOWUSER_ECF_ARG_FORMAT);
        UINT next = EnumClipboardFormats((UINT)format);
        INT noteLength = 0;
        WowNotePut(note, noteCapacity, &noteLength, "EnumClipboardFormats(0x");
        WowNoteHex(note, noteCapacity, &noteLength, format, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, ") -> 0x");
        WowNoteHex(note, noteCapacity, &noteLength, (DWORD)next, WOW_HEX_WORD_DIGITS);
        if (!next) WowNotePut(note, noteCapacity, &noteLength, " (end)");
        Wow32SetReturn(frame, (DWORD)(WORD)next);
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
        INT   isCheck  = (frame->Id == WOWUSER_CHECKMENUITEM);
        WORD  menu16   = Wow32ArgWord(frame, WOWUSER_MI_ARG_HMENU);
        WORD  itemId   = Wow32ArgWord(frame, WOWUSER_MI_ARG_ID);
        WORD  flags   = Wow32ArgWord(frame, WOWUSER_MI_ARG_FLAGS);
        HMENU menu    = WowUserMenu32(menu16);
        INT noteLength = 0;
        DWORD previousState;
        WowNotePut(note, noteCapacity, &noteLength, isCheck ? "CheckMenuItem 0x" : "EnableMenuItem 0x");
        WowNoteHex(note, noteCapacity, &noteLength, menu16, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, (flags & MF_BYPOSITION) ? " byPOSITION " : " byCOMMAND ");
        WowNoteHex(note, noteCapacity, &noteLength, itemId, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, " flags=0x");
        WowNoteHex(note, noteCapacity, &noteLength, flags, WOW_HEX_WORD_DIGITS);
        if (!menu) {
            WowNotePut(note, noteCapacity, &noteLength, " -- ★ NOT ONE OF OUR MENU TOKENS;"
                                       " answered -1");
            Wow32SetReturn(frame, WOWUSER_MINUS_ONE32);  /* Win16's "no such item" */
            return 1;
        }
        previousState = isCheck ? (DWORD)CheckMenuItem(menu, (UINT)itemId, (UINT)flags)
                   : (DWORD)EnableMenuItem(menu, (UINT)itemId, (UINT)flags);
        WowNotePut(note, noteCapacity, &noteLength, " -> previous state 0x");
        WowNoteHex(note, noteCapacity, &noteLength, previousState, WOW_HEX_WORD_DIGITS);
        Wow32SetReturn(frame, previousState);
        return 1;
    }

    /* ── ★★ THE MENU TRIO. Real menus, named by tokens. ─────────────────────
         Notepad greys and checks its own menu items (Edit > Undo, Word Wrap),
         and to do that it first has to GET the menu. It reads its own window's
         menu bar, walks into a popup, and works on that -- so all three are the
         same operation with the real HMENU behind a token. */
    case WOWUSER_GETMENU: {
        WORD window16 = Wow32ArgWord(frame, WOWUSER_GM2_ARG_HWND);
        PWOWUSER_WINDOW window = WowUserFindWindow(window16);
        WORD menu16 = 0;
        INT noteLength = 0;
        WowNotePut(note, noteCapacity, &noteLength, "GetMenu 0x");
        WowNoteHex(note, noteCapacity, &noteLength, window16, WOW_HEX_WORD_DIGITS);
        if (!window || !window->Window32) { WowNotePut(note, noteCapacity, &noteLength, " -- no real window");
                                Wow32SetReturn(frame, 0); return 1; }
        menu16 = WowUserMenu16(GetMenu(window->Window32));
        WowNotePut(note, noteCapacity, &noteLength, menu16 ? " -> token 0x" : " -- NO MENU (or no token"
                                                        " left) 0x");
        WowNoteHex(note, noteCapacity, &noteLength, menu16, WOW_HEX_WORD_DIGITS);
        Wow32SetReturn(frame, menu16);
        return 1;
    }

    case WOWUSER_GETSUBMENU: {
        WORD menu16  = Wow32ArgWord(frame, WOWUSER_GSM2_ARG_HMENU);
        WORD position = Wow32ArgWord(frame, WOWUSER_GSM2_ARG_POS);
        HMENU menu  = WowUserMenu32(menu16);
        WORD subMenu16 = 0;
        INT noteLength = 0;
        WowNotePut(note, noteCapacity, &noteLength, "GetSubMenu 0x");
        WowNoteHex(note, noteCapacity, &noteLength, menu16, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, " pos 0x");
        WowNoteHex(note, noteCapacity, &noteLength, position, WOW_HEX_WORD_DIGITS);
        if (!menu) { WowNotePut(note, noteCapacity, &noteLength, " -- ★ NOT ONE OF OUR MENU TOKENS");
                  Wow32SetReturn(frame, 0); return 1; }
        subMenu16 = WowUserMenu16(GetSubMenu(menu, (INT)(SHORT)position));
        WowNotePut(note, noteCapacity, &noteLength, " -> token 0x");
        WowNoteHex(note, noteCapacity, &noteLength, subMenu16, WOW_HEX_WORD_DIGITS);
        Wow32SetReturn(frame, subMenu16);
        return 1;
    }

    /* ⚠ bRevert TRUE DESTROYS the application's copy and rebuilds the default,
         which is a real side effect and not a query -- passed through as given,
         and logged, because a guest that passes TRUE by accident would otherwise
         lose its own system-menu customisations invisibly. */
    case WOWUSER_GETSYSTEMMENU: {
        WORD window16 = Wow32ArgWord(frame, WOWUSER_GSYM_ARG_HWND);
        WORD revert  = Wow32ArgWord(frame, WOWUSER_GSYM_ARG_REVERT);
        PWOWUSER_WINDOW window = WowUserFindWindow(window16);
        WORD menu16 = 0;
        INT noteLength = 0;
        WowNotePut(note, noteCapacity, &noteLength, "GetSystemMenu 0x");
        WowNoteHex(note, noteCapacity, &noteLength, window16, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, revert ? " REVERT (rebuilds the default menu)"
                                       : " (query)");
        if (!window || !window->Window32) { WowNotePut(note, noteCapacity, &noteLength, " -- no real window");
                                Wow32SetReturn(frame, 0); return 1; }
        menu16 = WowUserMenu16(GetSystemMenu(window->Window32, revert ? TRUE : FALSE));
        WowNotePut(note, noteCapacity, &noteLength, " -> token 0x");
        WowNoteHex(note, noteCapacity, &noteLength, menu16, WOW_HEX_WORD_DIGITS);
        Wow32SetReturn(frame, menu16);
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
        WORD dialog16 = Wow32ArgWord(frame, WOWUSER_GDI2_ARG_HDLG);
        WORD itemId   = Wow32ArgWord(frame, WOWUSER_GDI2_ARG_ID);
        PWOWUSER_WINDOW window = WowUserFindWindow(dialog16);
        WORD window16Result = 0;
        INT noteLength = 0;
        WowNotePut(note, noteCapacity, &noteLength, "GetDlgItem dlg 0x");
        WowNoteHex(note, noteCapacity, &noteLength, dialog16, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, " id 0x");
        WowNoteHex(note, noteCapacity, &noteLength, itemId, WOW_HEX_WORD_DIGITS);
        if (!window || !window->Window32) { WowNotePut(note, noteCapacity, &noteLength, " -- no real window");
                                Wow32SetReturn(frame, 0); return 1; }
        window16Result = WowWinHwnd16(GetDlgItem(window->Window32, (INT)(SHORT)itemId));
        WowNotePut(note, noteCapacity, &noteLength, window16Result ? " -> 0x" : " -- NOT FOUND (or not a window"
                                                    " this host made) 0x");
        WowNoteHex(note, noteCapacity, &noteLength, window16Result, WOW_HEX_WORD_DIGITS);
        Wow32SetReturn(frame, window16Result);
        return 1;
    }

    case WOWUSER_GETDLGITEMTEXT: {
        WORD dialog16 = Wow32ArgWord(frame, WOWUSER_GDIT_ARG_HDLG);
        WORD itemId   = Wow32ArgWord(frame, WOWUSER_GDIT_ARG_ID);
        WORD capacity  = Wow32ArgWord(frame, WOWUSER_GDIT_ARG_MAX);
        volatile BYTE *destination = Wow32ArgPointer(frame, WOWUSER_GDIT_ARG_BUF);
        PWOWUSER_WINDOW window = WowUserFindWindow(dialog16);
        UINT length = 0;
        INT noteLength = 0;
        WowNotePut(note, noteCapacity, &noteLength, "GetDlgItemText dlg 0x");
        WowNoteHex(note, noteCapacity, &noteLength, dialog16, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, " id 0x");
        WowNoteHex(note, noteCapacity, &noteLength, itemId, WOW_HEX_WORD_DIGITS);
        if (!window || !window->Window32 || !destination || !capacity) {
            WowNotePut(note, noteCapacity, &noteLength, " -- no window or no buffer; answered 0");
            Wow32SetReturn(frame, 0); return 1;
        }
        /* ⚠ `cap` is the caller's claim about its own buffer and the only bound
             there is -- handed to the OS, which respects it. */
        length = GetDlgItemTextA(window->Window32, (INT)(SHORT)itemId, (LPSTR)destination, (INT)capacity);
        WowNotePut(note, noteCapacity, &noteLength, " -> ");
        WowNoteQuoted(note, noteCapacity, &noteLength, (PCSTR)destination);
        Wow32SetReturn(frame, (DWORD)(WORD)length);
        return 1;
    }

    case WOWUSER_SETDLGITEMINT: {
        WORD dialog16 = Wow32ArgWord(frame, WOWUSER_SDII_ARG_HDLG);
        WORD itemId   = Wow32ArgWord(frame, WOWUSER_SDII_ARG_ID);
        WORD value  = Wow32ArgWord(frame, WOWUSER_SDII_ARG_VALUE);
        WORD isSigned  = Wow32ArgWord(frame, WOWUSER_SDII_ARG_SIGNED);
        PWOWUSER_WINDOW window = WowUserFindWindow(dialog16);
        INT noteLength = 0;
        WowNotePut(note, noteCapacity, &noteLength, "SetDlgItemInt dlg 0x");
        WowNoteHex(note, noteCapacity, &noteLength, dialog16, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, " id 0x");
        WowNoteHex(note, noteCapacity, &noteLength, itemId, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, " = 0x");
        WowNoteHex(note, noteCapacity, &noteLength, value, WOW_HEX_WORD_DIGITS);
        if (!window || !window->Window32) { WowNotePut(note, noteCapacity, &noteLength, " -- no real window");
                                Wow32SetReturn(frame, 0); return 1; }
        /* ⚠ SIGNEDNESS IS THE CALLER'S, and it changes the text: -1 or 65535.
             The Win16 value is a WORD, so it is widened the way the caller says
             rather than the way C would. */
        SetDlgItemInt(window->Window32, (INT)(SHORT)itemId,
                      isSigned ? (UINT)(INT)(SHORT)value : (UINT)value,
                      isSigned ? TRUE : FALSE);
        Wow32SetReturn(frame, 0);
        return 1;
    }

    case WOWUSER_SENDDLGITEMMSG: {
        WORD dialog16 = Wow32ArgWord(frame, WOWUSER_SDIM_ARG_HDLG);
        WORD itemId   = Wow32ArgWord(frame, WOWUSER_SDIM_ARG_ID);
        WORD message16    = Wow32ArgWord(frame, WOWUSER_SDIM_ARG_MSG);
        WORD wParam   = Wow32ArgWord(frame, WOWUSER_SDIM_ARG_WPARAM);
        DWORD lParam  = Wow32ArgDword(frame, WOWUSER_SDIM_ARG_LPARAM);
        PWOWUSER_WINDOW window = WowUserFindWindow(dialog16);
        WORD control16 = 0;
        INT noteLength = 0;
        WowNotePut(note, noteCapacity, &noteLength, "SendDlgItemMessage dlg 0x");
        WowNoteHex(note, noteCapacity, &noteLength, dialog16, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, " id 0x");
        WowNoteHex(note, noteCapacity, &noteLength, itemId, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, " msg 0x");
        WowNoteHex(note, noteCapacity, &noteLength, message16, WOW_HEX_WORD_DIGITS);
        if (!window || !window->Window32) { WowNotePut(note, noteCapacity, &noteLength, " -- no real window");
                                Wow32SetReturn(frame, 0); return 1; }
        /* ★ ROUTED THROUGH THIS HOST'S OWN SendMessage PATH, not straight to
             Win32: the control may be one whose window procedure is the GUEST'S,
             and it may carry a 16:16 pointer that Win32 must never see. Turning
             it into (hwnd16, msg, wParam, lParam) and reusing the machinery that
             already decides between those two worlds is the only answer that is
             right in both. */
        control16 = WowWinHwnd16(GetDlgItem(window->Window32, (INT)(SHORT)itemId));
        if (!control16) {
            WowNotePut(note, noteCapacity, &noteLength, " -- NO SUCH ITEM; answered 0");
            Wow32SetReturn(frame, 0);
            return 1;
        }
        WowNotePut(note, noteCapacity, &noteLength, " -> control 0x");
        WowNoteHex(note, noteCapacity, &noteLength, control16, WOW_HEX_WORD_DIGITS);
        {   PWOWUSER_WINDOW control = WowUserFindWindow(control16);
            if (!control) { WowNotePut(note, noteCapacity, &noteLength, " -- not in our table");
                       Wow32SetReturn(frame, 0); return 1; }
            if (control->WindowProcedure) {
                if (!frame->IsCallbackAllowed) { WowNotePut(note, noteCapacity, &noteLength, " -- its procedure is"
                                                           " 16-bit and callbacks"
                                                           " are off");
                                Wow32SetReturn(frame, 0); return 1; }
                WowNotePut(note, noteCapacity, &noteLength, " -> its own window procedure");
                Wow32SetReturn(frame, 0);
                WowUserWantMessage(frame, control,
                                 control->Instance ? control->Instance : g_WowUserClasses[control->Class].Instance,
                                 message16, wParam, lParam, WOWCALL_RET_RESULT);
                return 1;
            }
            Wow32SetReturn(frame, (DWORD)WowUserDefProc(frame, control, message16, wParam, lParam,
                                                   note, noteCapacity));
            return 1;
        }
    }

    /* ── ★★ EndDialog(hDlg, nResult) ────────────────────────────────────────
       ⚠⚠ WHAT ENDS HERE IS THE WINDOW, AND POSSIBLY NOT THE LOOP. (An early
         note, superseded by wowdlg.h: it assumed USER's 16-bit `DialogBox` ran its
         own modal message loop.) On real WOW this call tells the 32-bit side to end a real
         dialog and the loop notices. Our "dialog" is a plain window that USER
         built by calling CreateWindow through this host, so Win32's EndDialog
         has nothing to end -- it is called anyway, because if the window ever IS
         a real dialog that is the correct thing, and its failure is reported
         rather than hidden. The window is then hidden so the user is not left
         looking at a dead dialog.
       ⇒ IF A RUN SHOWS USER'S LOOP SPINNING AFTER THIS, that is the measurement
         that says how the loop learns it is over, and it will be in the log. */
    case WOWUSER_ENDDIALOG: {
        WORD dialog16 = Wow32ArgWord(frame, WOWUSER_ED_ARG_HDLG);
        WORD dialogResult  = Wow32ArgWord(frame, WOWUSER_ED_ARG_RESULT);
        PWOWUSER_WINDOW window = WowUserFindWindow(dialog16);
        INT noteLength = 0, isOk = 0;
        WowNotePut(note, noteCapacity, &noteLength, "EndDialog 0x");
        WowNoteHex(note, noteCapacity, &noteLength, dialog16, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, " result 0x");
        WowNoteHex(note, noteCapacity, &noteLength, dialogResult, WOW_HEX_WORD_DIGITS);
        /* ── ★★★★★ AND THIS IS WHAT THE LOOP HAS BEEN WAITING FOR. (session 57)
             ⚠ IT DOES NOT UNWIND ANYTHING HERE, AND IT MUST NOT: we are several
               frames down inside the guest's own dialog procedure, which has to
               return through our stub before its context can be put back.
               Recording the answer is the whole job; the pump reads it on the
               way out. Tearing the frame down from here would resume a caller
               while its callee was still running.
             ⚠ THE RESULT IS RECORDED EVEN IF Win32's EndDialog below refuses --
               that call is about the real window, this is about the parked
               DialogBox, and they are two different questions. Answering only
               the first is how the loop would miss its own exit. */
        if (WowDlgEnd(dialog16, dialogResult))
            WowNotePut(note, noteCapacity, &noteLength, " -- ★ THIS ENDS A MODAL LOOP: DialogBox"
                                       " returns it as soon as this procedure"
                                       " does");
        if (!window || !window->Window32) { WowNotePut(note, noteCapacity, &noteLength, " -- no real window");
                                Wow32SetReturn(frame, 0); return 1; }
        isOk = EndDialog(window->Window32, (INT_PTR)(SHORT)dialogResult) ? 1 : 0;
        if (!isOk) {
            ShowWindow(window->Window32, SW_HIDE);
            WowNotePut(note, noteCapacity, &noteLength, " -- Win32 EndDialog refused it (this is a"
                                       " window, not a real dialog); HIDDEN"
                                       " instead. ★ If USER's own modal loop keeps"
                                       " spinning, THAT is the next thing to read");
        } else {
            WowNotePut(note, noteCapacity, &noteLength, " -> ended");
        }
        Wow32SetReturn(frame, 1);
        return 1;
    }

    /* ── ★★★★★ 0x6b DefWindowProc -- AND USER.107 IS *NOT* PURELY 16-BIT. ────
       ⚠⚠ **A NOTE EARLIER IN THIS FILE SAID THE OPPOSITE AND IT IS CORRECTED
         HERE.** It saw that USER.107 does not thunk directly and concluded "USER
         implements it ITSELF". It does so only in part: USER answers what it can
         in 16-bit code and **forwards the rest to us** as id 0x6b, 10 argument
         bytes (which messages, see WOWNOTIFY_USERINIT), and everything it
         forwarded had been getting the harness sentinel.
       ⇒ The evidence that supported the old note -- "a whole run of Notepad never
         produced one as a BOP" -- was true and did not mean what it was taken to
         mean: it showed that USER's own half had handled everything Notepad
         passed on, not that the 32-bit half did not exist.
       ★ THE RIGHT ANSWER IS THE OS's. Our windows are real `HWND`s, so a message
         the guest declines belongs to `DefWindowProcA` on the real window --
         which is where non-client painting, sizing, activation and the system
         menu all come from. A window whose defaults are answered `0` is a window
         that looks right until someone uses it. */
    /* ── s92 (#298): USER ids krnl386 calls from its own code (USER ids are USER's
         ordinals). Named and answered on purpose; the call sites are in the #298 thread.
       0x13a SignalProc(hTask/hModule, code, uExitFn, hInstance, hQueue) -- 0x40 a DLL
         loaded, 0x80 one unloaded, 0x20 a task exit, 0x666 a task's #GP. Every krnl386
         caller ignores the result. ⚠ USER's per-task cleanup on 0x20/0x666 (its windows,
         hooks, timers) is NOT done here yet; the task's real windows go with the process.
       0x190 FinalUserInit -- once, at the first task; the result is discarded. */
    case WOWUSER_SIGNALPROC:
    case WOWUSER_FINALUSERINIT: {
        INT noteLength = 0;
        WowNotePut(note, noteCapacity, &noteLength, frame->Id == WOWUSER_SIGNALPROC ? "SignalProc code 0x" : "FinalUserInit");
        if (frame->Id == WOWUSER_SIGNALPROC) WowNoteHex(note, noteCapacity, &noteLength, Wow32ArgWord(frame, WOWUSER_SIGNALPROC_ARG_CODE), WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, " -> 0 (acknowledged)");
        Wow32SetReturn(frame, 0);
        return 1;
    }
    /* ── 0x140 SysErrorBox(lpszText, lpszCaption, btn1, btn2, btn3) -- USER.320. When a
         task GP-faults, krnl386 puts up "Application Error" with btn2 = SEB_CLOSE |
         SEB_DEFBUTTON (as logged); an answer of 1 resumes the task and anything else
         ends it. The answer is the 1-based index of the button pressed. Stepped over it
         answered 0: the task ended with no box at all. Win32 has no SysErrorBox, so the
         OS's MessageBox stands in: one button -> OK, two -> OK/Cancel, three ->
         Abort/Retry/Ignore, each mapped back to the index of the button it replaces. */
    case WOWUSER_SYSERRORBOX: {
        CHAR text[WOWUSER_STRING_SIZE], caption[WOWUSER_CAPTION_SIZE];
        WORD bounds[WOWUSER_SEB_BUTTONS];
        INT  indexes[WOWUSER_SEB_BUTTONS], count = 0, index, noteLength = 0, result;
        UINT type;
        bounds[0] = Wow32ArgWord(frame, WOWUSER_SEB_ARG_BUTTON1); bounds[1] = Wow32ArgWord(frame, WOWUSER_SEB_ARG_BUTTON2); bounds[2] = Wow32ArgWord(frame, WOWUSER_SEB_ARG_BUTTON3);
        if (!Wow32ArgString(frame, WOWUSER_SEB_ARG_TEXT, text, sizeof text)) text[0] = 0;
        if (!Wow32ArgString(frame, WOWUSER_SEB_ARG_CAPTION, caption, sizeof caption)) lstrcpynA(caption, "Error", sizeof caption);
        for (index = 0; index < WOWUSER_SEB_BUTTONS; ++index) if (bounds[index] & WOWUSER_SEB_BUTTON_MASK) indexes[count++] = index + 1;
        type = count >= WOWUSER_SEB_BUTTONS ? MB_ABORTRETRYIGNORE : count == 2 ? MB_OKCANCEL : MB_OK;
        WowNotePut(note, noteCapacity, &noteLength, "SysErrorBox ");
        WowNoteQuoted(note, noteCapacity, &noteLength, caption);
        WowNotePut(note, noteCapacity, &noteLength, ": ");
        WowNoteQuoted(note, noteCapacity, &noteLength, text);
        result = MessageBoxA(NULL, text, caption, type | MB_ICONSTOP | MB_SETFOREGROUND | MB_TASKMODAL);
        result = (result == IDCANCEL || result == IDRETRY) ? 1 : (result == IDIGNORE) ? 2 : 0;
        result = count ? indexes[result < count ? result : count - 1] : 0;
        WowNotePut(note, noteCapacity, &noteLength, " -> button 0x");
        WowNoteHex(note, noteCapacity, &noteLength, (DWORD)result, WOW_HEX_BYTE_DIGITS);
        Wow32SetReturn(frame, (DWORD)result);
        return 1;
    }

    case WOWUSER_DEFWINDOWPROC: {
        WORD  window16 = Wow32ArgWord(frame, WOWUSER_DWP_ARG_HWND);
        WORD  message16  = Wow32ArgWord(frame, WOWUSER_DWP_ARG_MSG);
        WORD  wParam   = Wow32ArgWord(frame, WOWUSER_DWP_ARG_WPARAM);
        DWORD lParam   = Wow32ArgDword(frame, WOWUSER_DWP_ARG_LPARAM);
        HWND  window32    = WowUserHwnd32(window16);
        INT   noteLength = 0;
        LRESULT result;
        WowNotePut(note, noteCapacity, &noteLength, "DefWindowProc(0x");
        WowNoteHex(note, noteCapacity, &noteLength, window16, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, " msg=0x");
        WowNoteHex(note, noteCapacity, &noteLength, message16, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, " wParam=0x");
        WowNoteHex(note, noteCapacity, &noteLength, wParam, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, " lParam=0x");
        WowNoteHex(note, noteCapacity, &noteLength, lParam, WOW_HEX_DWORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, ")");
        if (!window32) {
            WowNotePut(note, noteCapacity, &noteLength, " -- ★ NOT ONE OF OUR WINDOWS; answered 0");
            Wow32SetReturn(frame, 0);
            return 1;
        }
        /* #162: WM_CLOSE's default is DestroyWindow -- OURS, which tells the guest
           (WM_DESTROY -> PostQuitMessage -> the task ends). The real DefWindowProc
           destroyed only the real window: WinMine vanished from the screen and its
           task waited in GetMessage forever, host and all. */
        if (message16 == WM_CLOSE) {
            WowNotePut(note, noteCapacity, &noteLength, " -> WM_CLOSE: ");
            WowUserDestroy(window16, note, noteCapacity, &noteLength);
            Wow32SetReturn(frame, 0);
            return 1;
        }
        if (message16 == WM_CTLCOLOR16) {               /* see WowUserCtlColorDefault */
            WORD brush16 = WowUserCtlColorDefault(WowUserFindWindow(window16), window32, wParam, lParam);
            WowNotePut(note, noteCapacity, &noteLength, " -> WM_CTLCOLOR: the default brush, token 0x");
            WowNoteHex(note, noteCapacity, &noteLength, brush16, WOW_HEX_WORD_DIGITS);
            Wow32SetReturn(frame, brush16);
            return 1;
        }
        if (message16 == WM_PAINT) {
            WowUserDefaultPaint(WowUserFindWindow(window16), 0);
            WowNotePut(note, noteCapacity, &noteLength, " -> WM_PAINT: erased what was owed (class brush)");
            Wow32SetReturn(frame, 0);
            return 1;
        }
        if (message16 == WM_ERASEBKGND && !WowUserWParam32(message16, wParam)) {
            WowNotePut(note, noteCapacity, &noteLength, " -- ★ WM_ERASEBKGND with no DC we issued; 0");
            Wow32SetReturn(frame, 0);
            return 1;
        }
        if (message16 == WM_ERASEBKGND) {                   /* s92 #289: what the erase hits */
            HDC eraseDc = (HDC)WowUserWParam32(message16, wParam);
            RECT clipBox; HWND dcWindow = WindowFromDC(eraseDc);
            INT regionType = GetClipBox(eraseDc, &clipBox);
            WowNotePut(note, noteCapacity, &noteLength, dcWindow == window32 ? " [dc=this window" : " [★ dc=ANOTHER window");
            WowNotePut(note, noteCapacity, &noteLength, " clip="); WowNoteHex(note, noteCapacity, &noteLength, (DWORD)regionType, 1);
            WowNotePut(note, noteCapacity, &noteLength, ":");      WowNoteHex(note, noteCapacity, &noteLength, (DWORD)clipBox.left, WOW_HEX_WORD_DIGITS);
            WowNotePut(note, noteCapacity, &noteLength, ",");      WowNoteHex(note, noteCapacity, &noteLength, (DWORD)clipBox.top, WOW_HEX_WORD_DIGITS);
            WowNotePut(note, noteCapacity, &noteLength, ",");      WowNoteHex(note, noteCapacity, &noteLength, (DWORD)clipBox.right, WOW_HEX_WORD_DIGITS);
            WowNotePut(note, noteCapacity, &noteLength, ",");      WowNoteHex(note, noteCapacity, &noteLength, (DWORD)clipBox.bottom, WOW_HEX_WORD_DIGITS);
            WowNotePut(note, noteCapacity, &noteLength, " brush="); WowNoteHex(note, noteCapacity, &noteLength,
                        (DWORD)GetClassLongPtrA(window32, GCLP_HBRBACKGROUND), WOW_HEX_DWORD_DIGITS);
            WowNotePut(note, noteCapacity, &noteLength, "]");
        }
        result = DefWindowProcA(window32, message16, WowUserWParam32(message16, wParam), (LPARAM)lParam);
        WowNotePut(note, noteCapacity, &noteLength, " -> 0x");
        WowNoteHex(note, noteCapacity, &noteLength, (DWORD)result, WOW_HEX_DWORD_DIGITS);
        Wow32SetReturn(frame, (DWORD)result);
        return 1;
    }

    /* ── ★★★ 0x82 SetClassWord -- HOW MS PAINT CHANGES ITS POINTER. ──────────
         Its own call site pins both the index and the intent (see the note by
         the defines): `push -0x0c` is GCW_HCURSOR, and the value is a cursor
         token minted by 0xad. Eighteen of these a run, and with all of them
         stepped over a paint program showed an arrow over every tool.
       ⚠ ONLY GCW_HCURSOR. The other class words -- the background brush, the icon,
         the class style, the extra-byte counts -- change things this host either
         mirrors elsewhere or would have to re-register a Win32 class to honour,
         and a silent partial answer is how a guest comes to believe it changed
         something it did not. Anything else is logged by index and refused. */
    case WOWUSER_SETCLASSWORD: {
        WORD window16 = Wow32ArgWord(frame, WOWUSER_SCW_ARG_HWND);
        INT  fieldIndex  = (INT)(SHORT)Wow32ArgWord(frame, WOWUSER_SCW_ARG_INDEX);
        WORD value  = Wow32ArgWord(frame, WOWUSER_SCW_ARG_VALUE);
        HWND window32    = WowUserHwnd32(window16);
        INT  noteLength = 0, fell = 0;
        HCURSOR cursor;
        WowNotePut(note, noteCapacity, &noteLength, "SetClassWord(0x");
        WowNoteHex(note, noteCapacity, &noteLength, window16, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, ", ");
        WowNoteHex(note, noteCapacity, &noteLength, (DWORD)fieldIndex, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, ", 0x");
        WowNoteHex(note, noteCapacity, &noteLength, value, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, ")");
        if (!window32) {
            WowNotePut(note, noteCapacity, &noteLength, " -- ★ NOT ONE OF OUR WINDOWS; answered 0");
            Wow32SetReturn(frame, 0);
            return 1;
        }
        if (fieldIndex != WOWUSER_GCW16_HCURSOR) {
            WowNotePut(note, noteCapacity, &noteLength, " -- ★ ONLY GCW_HCURSOR (-12) is answered;"
                                       " this index is logged and refused rather"
                                       " than half-applied");
            Wow32SetReturn(frame, 0);
            return 1;
        }
        cursor = WowUserSystemResourceCursor(value, &fell);
        if (!cursor) {
            WowNotePut(note, noteCapacity, &noteLength, " -- ★ that is not a cursor this host"
                                       " built (an unknown token, or a named"
                                       " resource not in this module); refused");
            Wow32SetReturn(frame, 0);
            return 1;
        }
        {   PCSTR name = WowUserSystemResourceName(value);
            if (name) { WowNotePut(note, noteCapacity, &noteLength, " cursor="); WowNoteQuoted(note, noteCapacity, &noteLength, name); }
        }
        SetClassLongA(window32, GCL_HCURSOR, (LONG)(LONG_PTR)cursor);
        /* ⚠ The class cursor only takes effect on the next WM_SETCURSOR, and a
             guest that changed it mid-stroke expects it NOW -- which is what the
             OS does for its own programs because the mouse is inside the window. */
        SetCursor(cursor);
        WowNotePut(note, noteCapacity, &noteLength, " -> applied to the class and to the pointer now");
        Wow32SetReturn(frame, value);
        return 1;
    }

    /* ── ★ 0x87 GetWindowLong / 0x88 SetWindowLong ──────────────────────────
         MS Paint reads and writes GWL_STYLE (-16); the positive indices are the
         window's own extra bytes, which this host already keeps (as WORDs, per
         the class's `cbWndExtra`) and which a LONG spans two of.
       ★ GWL_WNDPROC IS ANSWERABLE AND IS ANSWERED: the guest's own 16:16 window
         procedure is recorded at creation, so this hands back the value the guest
         itself supplied rather than a host address it could not call.
       ⚠ SETTING GWL_WNDPROC IS REFUSED. Subclassing would have to re-point a
         procedure this host calls through `wowcall`, and answering "done" without
         doing it is the failure mode this project treats as the most expensive. */
    case WOWUSER_GETWINDOWLONG:
    case WOWUSER_SETWINDOWLONG: {
        INT   isSet = (frame->Id == WOWUSER_SETWINDOWLONG);
        WORD  window16  = Wow32ArgWord(frame, isSet ? WOWUSER_SWL_ARG_HWND  : WOWUSER_GWL_ARG_HWND);
        INT   fieldIndex   = (INT)(SHORT)Wow32ArgWord(frame, isSet ? WOWUSER_SWL_ARG_INDEX : WOWUSER_GWL_ARG_INDEX);
        DWORD value   = isSet ? Wow32ArgDword(frame, WOWUSER_SWL_ARG_VALUE) : 0;
        PWOWUSER_WINDOW window = WowUserFindWindow(window16);
        HWND  window32 = window ? window->Window32 : NULL;
        INT   noteLength = 0;
        DWORD previousValue = 0;
        WowNotePut(note, noteCapacity, &noteLength, isSet ? "SetWindowLong(0x" : "GetWindowLong(0x");
        WowNoteHex(note, noteCapacity, &noteLength, window16, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, ", ");
        WowNoteHex(note, noteCapacity, &noteLength, (DWORD)fieldIndex, WOW_HEX_WORD_DIGITS);
        if (isSet) { WowNotePut(note, noteCapacity, &noteLength, ", 0x");
                     WowNoteHex(note, noteCapacity, &noteLength, value, WOW_HEX_DWORD_DIGITS); }
        WowNotePut(note, noteCapacity, &noteLength, ")");
        if (!window) {
            WowNotePut(note, noteCapacity, &noteLength, " -- ★ NOT ONE OF OUR WINDOWS; answered 0");
            Wow32SetReturn(frame, 0);
            return 1;
        }
        if (fieldIndex == WOWUSER_GWL16_STYLE || fieldIndex == WOWUSER_GWL16_EXSTYLE) {
            INT index32 = (fieldIndex == WOWUSER_GWL16_STYLE) ? GWL_STYLE : GWL_EXSTYLE;
            previousValue = window32 ? (DWORD)GetWindowLongA(window32, index32) : window->Style;
            if (isSet) {
                if (window32) SetWindowLongA(window32, index32, (LONG)value);
                if (fieldIndex == WOWUSER_GWL16_STYLE) window->Style = value;
            }
        } else if (fieldIndex == WOWUSER_GWL16_WNDPROC) {
            /* #308 (s91): SUBCLASSING. A 16-bit class's window: the record's own
               procedure is re-pointed -- DispatchMessage and every send read it per
               message (WowUserWindowProcedureOf). A system control: see g_WowUserSystemProcedures. */
            PCWOWUSER_SYSPROC systemProcedure = WowUserSystemProcedureOf(window);
            if (systemProcedure) {
                DWORD thunk = ((DWORD)frame->StubSegment << WORD_SHIFT) | systemProcedure->Offset;
                previousValue = window->SubclassProcedure ? window->SubclassProcedure : thunk;
                if (isSet) {
                    PCWOWUSER_SYSPROC backProcedure = WowUserSystemProcedureAt(frame, value);
                    if (!window32) {
                        WowNotePut(note, noteCapacity, &noteLength, " -- no real control; refused");
                        Wow32SetReturn(frame, 0);
                        return 1;
                    }
                    if (backProcedure == systemProcedure || !value) {  /* putting the original back */
                        if (window->OriginalProcedure32) SetWindowLongPtrA(window32, GWLP_WNDPROC, (LONG_PTR)window->OriginalProcedure32);
                        window->SubclassProcedure = 0; window->OriginalProcedure32 = NULL;
                        WowNotePut(note, noteCapacity, &noteLength, " -- subclass REMOVED, the control's"
                                                   " own procedure restored");
                    } else if (!g_WowUserCall16) {
                        WowNotePut(note, noteCapacity, &noteLength, " -- ★ no nested run to SEND the"
                                                   " subclass its messages; refused");
                        Wow32SetReturn(frame, 0);
                        return 1;
                    } else {
                        if (!window->OriginalProcedure32) {
                            window->OriginalProcedure32 = (WNDPROC)GetWindowLongPtrA(window32, GWLP_WNDPROC);
                            SetWindowLongPtrA(window32, GWLP_WNDPROC, (LONG_PTR)WowUserSubclassProcedure);
                        }
                        window->SubclassProcedure = value;
                        WowNotePut(note, noteCapacity, &noteLength, " -- ★ SUBCLASSED the system ");
                        WowNotePut(note, noteCapacity, &noteLength, systemProcedure->ClassName);
                        WowNotePut(note, noteCapacity, &noteLength, "; input/focus messages go to the"
                                                   " 16-bit procedure");
                    }
                }
            } else {
                previousValue = window->WindowProcedure;
                if (isSet) {
                    if (!window->WindowProcedure || !value) {
                        WowNotePut(note, noteCapacity, &noteLength, " -- ★ no 16-bit procedure to"
                                                   " replace (a dialog's, or NULL);"
                                                   " refused");
                        Wow32SetReturn(frame, 0);
                        return 1;
                    }
                    window->WindowProcedure = value;
                    WowNotePut(note, noteCapacity, &noteLength, " -- ★ SUBCLASSED: the window's 16-bit"
                                               " procedure re-pointed");
                }
            }
        } else if (fieldIndex >= 0 && fieldIndex + WOWUSER_DWORD_BYTES - 1 < (INT)(WOWUSER_MAX_EXTRA * WOW_WORD_BYTES)) {
            previousValue = (DWORD)window->Extra[fieldIndex / WOW_WORD_BYTES] | ((DWORD)window->Extra[fieldIndex / WOW_WORD_BYTES + 1] << WORD_SHIFT);
            if (isSet) {
                window->Extra[fieldIndex / WOW_WORD_BYTES]     = (WORD)(value & WORD_MASK);
                window->Extra[fieldIndex / WOW_WORD_BYTES + 1] = (WORD)(value >> WORD_SHIFT);
            }
        } else {
            WowNotePut(note, noteCapacity, &noteLength, " -- ★ an index this host does not keep;"
                                       " answered 0");
            Wow32SetReturn(frame, 0);
            return 1;
        }
        WowNotePut(note, noteCapacity, &noteLength, " -> 0x");
        WowNoteHex(note, noteCapacity, &noteLength, previousValue, WOW_HEX_DWORD_DIGITS);
        Wow32SetReturn(frame, previousValue);
        return 1;
    }

    /* ── ★ The answers that are the OS's own, with nothing to translate. ──────
         Each of these is one call with one number in it, and grouping them is
         what keeps the id-to-call mapping readable rather than spread over two
         hundred lines of identical shape.
       ⚠ `GetKeyState` returns the state at the last message retrieved, NOT the
         live keyboard -- and that is the right one: MS Paint tests the high bit
         to decide whether SHIFT constrains the
         shape it is drawing, and it must be the SHIFT that was down when the
         mouse message was posted, not whenever the guest got round to asking. */
    case WOWUSER_GETKEYSTATE:
    case WOWUSER_GETSYSCOLOR:
    case WOWUSER_SHOWCURSOR:
    case WOWUSER_GETMESSAGEPOS:
    case WOWUSER_GETMSGEXTRAINFO:
    case WOWUSER_GETDESKTOPWINDOW: {
        WORD unused = Wow32ArgWord(frame, 0);
        INT  noteLength = 0;
        DWORD result = 0;
        switch (frame->Id) {
        case WOWUSER_GETKEYSTATE:
            WowNotePut(note, noteCapacity, &noteLength, "GetKeyState(0x");
            WowNoteHex(note, noteCapacity, &noteLength, unused, WOW_HEX_WORD_DIGITS);
            WowNotePut(note, noteCapacity, &noteLength, ")");
            result = (DWORD)(WORD)GetKeyState((INT)(SHORT)unused);
            break;
        case WOWUSER_GETSYSCOLOR:
            WowNotePut(note, noteCapacity, &noteLength, "GetSysColor(0x");
            WowNoteHex(note, noteCapacity, &noteLength, unused, WOW_HEX_WORD_DIGITS);
            WowNotePut(note, noteCapacity, &noteLength, ")");
            result = (DWORD)GetSysColor((INT)(SHORT)unused);
            break;
        case WOWUSER_SHOWCURSOR:
            WowNotePut(note, noteCapacity, &noteLength, "ShowCursor(");
            WowNoteHex(note, noteCapacity, &noteLength, unused, WOW_HEX_WORD_DIGITS);
            WowNotePut(note, noteCapacity, &noteLength, ")");
            result = (DWORD)(WORD)(SHORT)ShowCursor(unused ? TRUE : FALSE);
            break;
        case WOWUSER_GETMESSAGEPOS:
            WowNotePut(note, noteCapacity, &noteLength, "GetMessagePos()");
            result = (DWORD)GetMessagePos();
            break;
        case WOWUSER_GETMSGEXTRAINFO:
            WowNotePut(note, noteCapacity, &noteLength, "GetMessageExtraInfo()");
            result = 0;
            break;
        default:
            /* ⚠ ANSWERED 0, AND THAT IS THE USEFUL ANSWER RATHER THAN A REFUSAL.
                 This host mints a Win16 handle only for a window it created, and
                 the desktop is not one; but a NULL hWnd is what both Win16 and
                 Win32 accept to mean "the screen" in `GetDC`, which is what a
                 guest asks the desktop window for. So 0 travels correctly. */
            WowNotePut(note, noteCapacity, &noteLength, "GetDesktopWindow() -- the desktop's"
                                       " own handle (#270)");
            result = WOWUSER_HWND_DESKTOP;
            break;
        }
        WowNotePut(note, noteCapacity, &noteLength, " = 0x");
        WowNoteHex(note, noteCapacity, &noteLength, result, WOW_HEX_DWORD_DIGITS);
        Wow32SetReturn(frame, result);
        return 1;
    }

    /* ── ★ Two window verbs and the caret, all straight through. ──────────────
       ⚠ The caret is per THREAD, and every Win16 window here belongs to the exec
         thread, so the OS's own caret is the guest's caret with nothing to map. */
    case WOWUSER_BRINGWINDOWTOTOP:
    case WOWUSER_DRAWMENUBAR:
    case WOWUSER_HIDECARET:
    case WOWUSER_SHOWCARET: {
        WORD window16 = Wow32ArgWord(frame, 0);
        HWND window32 = WowUserHwnd32(window16);
        INT  noteLength = 0, result = 0;
        WowNotePut(note, noteCapacity, &noteLength,
                frame->Id == WOWUSER_BRINGWINDOWTOTOP ? "BringWindowToTop(0x" :
                frame->Id == WOWUSER_DRAWMENUBAR      ? "DrawMenuBar(0x" :
                frame->Id == WOWUSER_HIDECARET        ? "HideCaret(0x" : "ShowCaret(0x");
        WowNoteHex(note, noteCapacity, &noteLength, window16, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, ")");
        /* A null hWnd is legal for the caret calls -- it means "the window that
           owns the caret" -- and is not for the other two. */
        if (!window32 && !(window16 == 0 && (frame->Id == WOWUSER_HIDECARET
                                  || frame->Id == WOWUSER_SHOWCARET))) {
            WowNotePut(note, noteCapacity, &noteLength, " -- ★ NOT ONE OF OUR WINDOWS; answered 0");
            Wow32SetReturn(frame, 0);
            return 1;
        }
        switch (frame->Id) {
        case WOWUSER_BRINGWINDOWTOTOP: result = BringWindowToTop(window32) ? 1 : 0; break;
        case WOWUSER_DRAWMENUBAR:      DrawMenuBar(window32); result = 1;           break;
        case WOWUSER_HIDECARET:        result = HideCaret(window32) ? 1 : 0;        break;
        default:                       result = ShowCaret(window32) ? 1 : 0;        break;
        }
        Wow32SetReturn(frame, (DWORD)result);
        return 1;
    }

    case WOWUSER_CREATECARET: {
        WORD window16 = Wow32ArgWord(frame, WOWUSER_CC_ARG_HWND);
        WORD bitmap16  = Wow32ArgWord(frame, WOWUSER_CC_ARG_BITMAP);
        INT  caretWidth   = (INT)(SHORT)Wow32ArgWord(frame, WOWUSER_CC_ARG_WIDTH);
        INT  caretHeight   = (INT)(SHORT)Wow32ArgWord(frame, WOWUSER_CC_ARG_HEIGHT);
        HWND window32    = WowUserHwnd32(window16);
        INT  brushKind = -1;
        HGDIOBJ brush = bitmap16 ? WowGdiH32(bitmap16, &brushKind) : NULL;
        INT  noteLength = 0;
        WowNotePut(note, noteCapacity, &noteLength, "CreateCaret(0x");
        WowNoteHex(note, noteCapacity, &noteLength, window16, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, ", ");
        WowNoteHex(note, noteCapacity, &noteLength, (DWORD)caretWidth, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, "x");
        WowNoteHex(note, noteCapacity, &noteLength, (DWORD)caretHeight, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, ")");
        if (!window32) {
            WowNotePut(note, noteCapacity, &noteLength, " -- ★ NOT ONE OF OUR WINDOWS; answered 0");
            Wow32SetReturn(frame, 0);
            return 1;
        }
        /* ⚠ hBitmap 0 = a solid caret and 1 = a grey one; only a real bitmap is
             passed through, and a token we cannot name becomes a solid caret
             rather than a wrong pattern. */
        Wow32SetReturn(frame, (DWORD)(CreateCaret(window32, (HBITMAP)(brushKind == WOWGDI_KIND_OBJ ? brush : NULL),
                                            caretWidth, caretHeight) ? 1 : 0));
        return 1;
    }

    case WOWUSER_DESTROYCARET: {
        INT noteLength = 0;
        WowNotePut(note, noteCapacity, &noteLength, "DestroyCaret()");
        Wow32SetReturn(frame, (DWORD)(DestroyCaret() ? 1 : 0));
        return 1;
    }

    case WOWUSER_SETCARETPOS:
    case WOWUSER_SETCURSORPOS: {
        INT isCaret = (frame->Id == WOWUSER_SETCARETPOS);
        INT positionX = (INT)(SHORT)Wow32ArgWord(frame, WOWUSER_SCP_ARG_X);
        INT positionY = (INT)(SHORT)Wow32ArgWord(frame, WOWUSER_SCP_ARG_Y);
        INT noteLength = 0, result;
        WowNotePut(note, noteCapacity, &noteLength, isCaret ? "SetCaretPos(" : "SetCursorPos(");
        WowNoteHex(note, noteCapacity, &noteLength, (DWORD)positionX, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, ",");
        WowNoteHex(note, noteCapacity, &noteLength, (DWORD)positionY, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, ")");
        result = isCaret ? (SetCaretPos(positionX, positionY) ? 1 : 0) : (SetCursorPos(positionX, positionY) ? 1 : 0);
        Wow32SetReturn(frame, (DWORD)result);
        return 1;
    }

    /* ── ★ 0x11 GetCursorPos / 0x1d ScreenToClient -- a Win16 POINT is 4 bytes. */
    case WOWUSER_GETCURSORPOS:
    case WOWUSER_SCREENTOCLIENT: {
        INT  isScreenToClient = (frame->Id == WOWUSER_SCREENTOCLIENT);
        volatile BYTE *pointBytes = Wow32ArgPointer(frame, isScreenToClient ? WOWUSER_STC_ARG_POINT : 0);
        WORD window16 = isScreenToClient ? Wow32ArgWord(frame, WOWUSER_STC_ARG_HWND) : 0;
        HWND window32    = isScreenToClient ? WowUserHwnd32(window16) : NULL;
        POINT point;
        INT  noteLength = 0;
        WowNotePut(note, noteCapacity, &noteLength, isScreenToClient ? "ScreenToClient(0x" : "GetCursorPos(0x");
        WowNoteHex(note, noteCapacity, &noteLength, window16, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, ")");
        if (!pointBytes || (isScreenToClient && !window32)) {
            WowNotePut(note, noteCapacity, &noteLength, " -- ★ no POINT, or not one of our windows;"
                                       " answered 0");
            Wow32SetReturn(frame, 0);
            return 1;
        }
        if (isScreenToClient) {
            point.x = (INT)(SHORT)Wow32PeekWord(pointBytes);
            point.y = (INT)(SHORT)Wow32PeekWord(pointBytes + WOWUSER_POINT16_Y);
            ScreenToClient(window32, &point);
        } else {
            point.x = point.y = 0;
            GetCursorPos(&point);
        }
        Wow32PokeWord(pointBytes,     (WORD)(SHORT)point.x);
        Wow32PokeWord(pointBytes + WOWUSER_POINT16_Y, (WORD)(SHORT)point.y);
        WowNotePut(note, noteCapacity, &noteLength, " -> ");
        WowNoteHex(note, noteCapacity, &noteLength, (DWORD)(WORD)(SHORT)point.x, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, ",");
        WowNoteHex(note, noteCapacity, &noteLength, (DWORD)(WORD)(SHORT)point.y, WOW_HEX_WORD_DIGITS);
        Wow32SetReturn(frame, 1);
        return 1;
    }

    /* ── ★ 0x52 InvertRect -- USER's call, GDI's DC. ─────────────────────────*/
    case WOWUSER_INVERTRECT: {
        WORD dc16 = Wow32ArgWord(frame, WOWUSER_INVR_ARG_HDC);
        volatile BYTE *rectBytes = Wow32ArgPointer(frame, WOWUSER_INVR_ARG_RECT);
        INT  dcKind = -1;
        HGDIOBJ dc = WowGdiH32(dc16, &dcKind);
        RECT rect;
        INT  noteLength = 0;
        WowNotePut(note, noteCapacity, &noteLength, "InvertRect(dc 0x");
        WowNoteHex(note, noteCapacity, &noteLength, dc16, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, ")");
        if (!rectBytes || !dc || (dcKind != WOWGDI_KIND_DC && dcKind != WOWGDI_KIND_WINDC)) {
            WowNotePut(note, noteCapacity, &noteLength, " -- ★ NO RECT, or not one of our DC"
                                       " tokens; answered 0");
            Wow32SetReturn(frame, 0);
            return 1;
        }
        rect.left   = (INT)(SHORT)Wow32PeekWord(rectBytes);
        rect.top    = (INT)(SHORT)Wow32PeekWord(rectBytes + WOWUSER_RECT16_TOP);
        rect.right  = (INT)(SHORT)Wow32PeekWord(rectBytes + WOWUSER_RECT16_RIGHT);
        rect.bottom = (INT)(SHORT)Wow32PeekWord(rectBytes + WOWUSER_RECT16_BOTTOM);
        Wow32SetReturn(frame, (DWORD)(InvertRect((HDC)dc, &rect) ? 1 : 0));
        return 1;
    }

    /* ── ★ 0x10c GlobalAddAtom / 0x10d GlobalDeleteAtom -- 25 calls a run. ────
         The system atom table is the OS's and an ATOM is a WORD in both worlds,
         so this is the rare pair with nothing between the guest and Windows.
       ⚠ These are what OLE and DDE names go through, which is why MS Paint --
         which registers itself as an OLE server -- makes so many of them. */
    case WOWUSER_GLOBALADDATOM:
    case WOWUSER_GLOBALDELATOM: {
        INT  isAdd = (frame->Id == WOWUSER_GLOBALADDATOM);
        INT  noteLength = 0;
        DWORD result;
        if (isAdd) {
            CHAR text[WOWUSER_STRING_SIZE];
            if (!Wow32ArgString(frame, 0, text, sizeof text)) {
                WowNotePut(note, noteCapacity, &noteLength, "GlobalAddAtom(NULL) -- answered 0");
                Wow32SetReturn(frame, 0);
                return 1;
            }
            WowNotePut(note, noteCapacity, &noteLength, "GlobalAddAtom(");
            WowNoteQuoted(note, noteCapacity, &noteLength, text);
            WowNotePut(note, noteCapacity, &noteLength, ")");
            result = (DWORD)GlobalAddAtomA(text);
        } else {
            WORD atom = Wow32ArgWord(frame, 0);
            WowNotePut(note, noteCapacity, &noteLength, "GlobalDeleteAtom(0x");
            WowNoteHex(note, noteCapacity, &noteLength, atom, WOW_HEX_WORD_DIGITS);
            WowNotePut(note, noteCapacity, &noteLength, ")");
            result = (DWORD)GlobalDeleteAtom(atom);
        }
        WowNotePut(note, noteCapacity, &noteLength, " = 0x");
        WowNoteHex(note, noteCapacity, &noteLength, result, WOW_HEX_WORD_DIGITS);
        Wow32SetReturn(frame, result);
        return 1;
    }

    /* ── ★ 0x11a SelectPalette / 0x11b RealizePalette -- USER's, not GDI's. ───
         Win16 puts both in USER.EXE (ordinals 282 and 283), which is why they are
         here rather than next to the other palette calls.
       ⚠ On this rig's 32bpp display a realized palette changes nothing, and that
         is exactly why they are worth answering rather than leaving to the
         sentinel: MS Paint calls them before nearly every drawing operation and a
         guest whose SelectPalette "fails" may take a different path. */
    case WOWUSER_SELECTPALETTE:
    case WOWUSER_REALIZEPALETTE: {
        INT  isSelect = (frame->Id == WOWUSER_SELECTPALETTE);
        WORD dc16 = Wow32ArgWord(frame, isSelect ? WOWUSER_SPL_ARG_HDC : 0);
        WORD palette16  = isSelect ? Wow32ArgWord(frame, WOWUSER_SPL_ARG_PAL) : 0;
        INT  dcKind = -1, paletteKind = -1;
        HGDIOBJ dc = WowGdiH32(dc16, &dcKind);
        HGDIOBJ palette = palette16 ? WowGdiH32(palette16, &paletteKind) : NULL;
        INT  noteLength = 0;
        WowNotePut(note, noteCapacity, &noteLength, isSelect ? "SelectPalette(0x" : "RealizePalette(0x");
        WowNoteHex(note, noteCapacity, &noteLength, dc16, WOW_HEX_WORD_DIGITS);
        if (isSelect) { WowNotePut(note, noteCapacity, &noteLength, ", pal 0x");
                     WowNoteHex(note, noteCapacity, &noteLength, palette16, WOW_HEX_WORD_DIGITS); }
        WowNotePut(note, noteCapacity, &noteLength, ")");
        if (!dc || (dcKind != WOWGDI_KIND_DC && dcKind != WOWGDI_KIND_WINDC)) {
            WowNotePut(note, noteCapacity, &noteLength, " -- ★ NOT ONE OF OUR DC TOKENS; answered 0");
            Wow32SetReturn(frame, 0);
            return 1;
        }
        if (!isSelect) {
            Wow32SetReturn(frame, (DWORD)RealizePalette((HDC)dc));
            return 1;
        }
        if (!palette || (paletteKind != WOWGDI_KIND_OBJ && paletteKind != WOWGDI_KIND_STOCK)) {
            WowNotePut(note, noteCapacity, &noteLength, " -- ★ NOT ONE OF OUR PALETTE TOKENS;"
                                       " answered 0");
            Wow32SetReturn(frame, 0);
            return 1;
        }
        {   HPALETTE previousPalette = SelectPalette((HDC)dc, (HPALETTE)palette,
                                          Wow32ArgWord(frame, WOWUSER_SPL_ARG_FORCE) ? TRUE : FALSE);
            WORD previous16 = previousPalette ? WowGdiH16((HGDIOBJ)previousPalette, WOWGDI_KIND_OBJ) : 0;
            WowNotePut(note, noteCapacity, &noteLength, " -> previous 0x");
            WowNoteHex(note, noteCapacity, &noteLength, previous16, WOW_HEX_WORD_DIGITS);
            Wow32SetReturn(frame, previous16);
        }
        return 1;
    }

    /* ── ★ 0xe8 SetWindowPos / 0x3d-adjacent 0x3f GetScrollPos ───────────────
       ⚠ `hWndInsertAfter` is one of the FOUR SPECIAL VALUES (HWND_TOP = 0,
         HWND_BOTTOM = 1, HWND_TOPMOST = -1, HWND_NOTOPMOST = -2) far more often
         than it is a window, and those are the same numbers in both worlds -- so
         a value that is not one of our tokens is passed through as itself rather
         than refused, and the log says which reading was taken. */
    case WOWUSER_SETWINDOWPOS: {
        WORD window16  = Wow32ArgWord(frame, WOWUSER_SWP_ARG_HWND);
        WORD insertAfter = Wow32ArgWord(frame, WOWUSER_SWP_ARG_AFTER);
        INT  positionX  = (INT)(SHORT)Wow32ArgWord(frame, WOWUSER_SWP_ARG_X);
        INT  positionY  = (INT)(SHORT)Wow32ArgWord(frame, WOWUSER_SWP_ARG_Y);
        INT  width = (INT)(SHORT)Wow32ArgWord(frame, WOWUSER_SWP_ARG_CX);
        INT  height = (INT)(SHORT)Wow32ArgWord(frame, WOWUSER_SWP_ARG_CY);
        WORD flags = Wow32ArgWord(frame, WOWUSER_SWP_ARG_FLAGS);
        HWND window32  = WowUserHwnd32(window16);
        HWND insertAfter32 = WowUserHwnd32(insertAfter);
        INT  noteLength = 0;
        WowNotePut(note, noteCapacity, &noteLength, "SetWindowPos(0x");
        WowNoteHex(note, noteCapacity, &noteLength, window16, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, ", after 0x");
        WowNoteHex(note, noteCapacity, &noteLength, insertAfter, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, ", ");
        WowNoteHex(note, noteCapacity, &noteLength, (DWORD)positionX, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, ",");
        WowNoteHex(note, noteCapacity, &noteLength, (DWORD)positionY, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, " ");
        WowNoteHex(note, noteCapacity, &noteLength, (DWORD)width, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, "x");
        WowNoteHex(note, noteCapacity, &noteLength, (DWORD)height, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, " flags=0x");
        WowNoteHex(note, noteCapacity, &noteLength, flags, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, ")");
        if (!window32) {
            WowNotePut(note, noteCapacity, &noteLength, " -- ★ NOT ONE OF OUR WINDOWS; answered 0");
            Wow32SetReturn(frame, 0);
            return 1;
        }
        if (!insertAfter32) {
            insertAfter32 = (HWND)(LONG_PTR)(SHORT)insertAfter;  /* HWND_TOP / BOTTOM / … */
            WowNotePut(note, noteCapacity, &noteLength, " [insert-after read as a CONSTANT]");
        }
        Wow32SetReturn(frame, (DWORD)(SetWindowPos(window32, insertAfter32, positionX, positionY, width, height, flags) ? 1 : 0));
        return 1;
    }

    /* ── ★★★★★ 0x2e GetParent -- see the long note by the defines. ───────────
       ⚠ Win16's GetParent returns the parent of a child window AND THE OWNER of
         an owned popup; both arrive in `hwndParent` at CreateWindow and this host
         records exactly that field, so one answer serves both without the host
         having to decide which kind of relationship it was. */
    case WOWUSER_GETPARENT: {
        WORD window16 = Wow32ArgWord(frame, 0);
        PCWOWUSER_WINDOW window = WowUserFindWindow(window16);
        INT  noteLength = 0;
        WowNotePut(note, noteCapacity, &noteLength, "GetParent(0x");
        WowNoteHex(note, noteCapacity, &noteLength, window16, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, ")");
        if (!window) {
            WowNotePut(note, noteCapacity, &noteLength, " -- ★ NOT ONE OF OUR WINDOWS; answered 0");
            Wow32SetReturn(frame, 0);
            return 1;
        }
        WowNotePut(note, noteCapacity, &noteLength, " -> 0x");
        WowNoteHex(note, noteCapacity, &noteLength, window->Parent, WOW_HEX_WORD_DIGITS);
        if (!window->Parent)
            WowNotePut(note, noteCapacity, &noteLength, " (a top-level window with no owner --"
                                       " which is a real answer, not a failure)");
        Wow32SetReturn(frame, window->Parent);
        return 1;
    }

    /* ── ★ 0x106 GetWindow(hWnd, uCmd) ──────────────────────────────────────
         The OS knows the real relationships, so the walk is done on the real
         windows and the answer translated back. ⚠ A real window that is not one
         of ours -- the desktop, or a control the OS owns -- has no Win16 handle
         to give, and 0 is the honest answer rather than a synthetic one. */
    case WOWUSER_GETWINDOW: {
        WORD window16 = Wow32ArgWord(frame, WOWUSER_GW_ARG_HWND);
        WORD command  = Wow32ArgWord(frame, WOWUSER_GW_ARG_CMD);
        HWND window32    = WowUserHwnd32(window16);
        INT  noteLength = 0;
        WORD result16 = 0;
        WowNotePut(note, noteCapacity, &noteLength, "GetWindow(0x");
        WowNoteHex(note, noteCapacity, &noteLength, window16, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, ", cmd ");
        WowNoteHex(note, noteCapacity, &noteLength, command, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, ")");
        if (!window32) {
            WowNotePut(note, noteCapacity, &noteLength, " -- ★ NOT ONE OF OUR WINDOWS; answered 0");
            Wow32SetReturn(frame, 0);
            return 1;
        }
        /* s91 (TASKMAN): THE WALK SKIPS WINDOWS THAT ARE NOT THE GUEST'S. The real
             desktop's first top-level window is never one of ours, so GW_HWNDFIRST
             answered 0 and a Win16 program walking the window list (TASKMAN's Task
             List, an MDI Window menu) saw an empty one. FIRST/LAST/NEXT/PREV and
             CHILD now step on, in the same direction, past windows that have no
             Win16 handle -- the list a Win16 program can name. */
        {   HWND next32 = GetWindow(window32, command);
            UINT step = (command == GW_HWNDFIRST || command == GW_HWNDNEXT || command == GW_CHILD)
                        ? GW_HWNDNEXT
                        : (command == GW_HWNDLAST || command == GW_HWNDPREV) ? GW_HWNDPREV : 0;
            INT guard = 0;
            /* ...and a foreign TOP-LEVEL window gets an alias (WowUserAlias16), the
               way stock answers; a foreign CHILD is stepped past as before. */
            INT isTopLevel = (command != GW_CHILD && command != GW_OWNER
                            && GetAncestor(window32, GA_PARENT) == GetDesktopWindow());
            if (isTopLevel && next32) result16 = WowUserAlias16(next32);
            else {
                while (next32 && step && !WowWinHwnd16(next32) && guard++ < WOWUSER_MAX_WINDOW_WALK)
                    next32 = GetWindow(next32, step);
                result16 = WowWinHwnd16(next32);
            }
        }
        WowNotePut(note, noteCapacity, &noteLength, " -> 0x");
        WowNoteHex(note, noteCapacity, &noteLength, result16, WOW_HEX_WORD_DIGITS);
        Wow32SetReturn(frame, result16);
        return 1;
    }

    /* ── ★ 0x3a GetClassName -- the Win16 name, not the mangled Win32 one. ───
       ⚠⚠ THIS MUST NOT RETURN THE REAL CLASS NAME. Every Win32 class this host
         registers is prefixed (`NTVDMEX16.pbParent`), and a guest comparing what
         it gets against the name it registered would never match -- OLE looks its
         own server window up by class name. The Win16 name is the one in our own
         class record, and that is what goes back. */
    case WOWUSER_GETCLASSNAME: {
        WORD window16 = Wow32ArgWord(frame, WOWUSER_GCN_ARG_HWND);
        WORD capacity  = Wow32ArgWord(frame, WOWUSER_GCN_ARG_MAX);
        volatile BYTE *destination = Wow32ArgPointer(frame, WOWUSER_GCN_ARG_BUF);
        PCWOWUSER_WINDOW window = WowUserFindWindow(window16);
        INT  noteLength = 0, length = 0;
        WowNotePut(note, noteCapacity, &noteLength, "GetClassName(0x");
        WowNoteHex(note, noteCapacity, &noteLength, window16, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, ")");
        if (!window || !destination || !capacity) {
            WowNotePut(note, noteCapacity, &noteLength, " -- ★ NOT ONE OF OUR WINDOWS, or no"
                                       " buffer; answered 0");
            Wow32SetReturn(frame, 0);
            return 1;
        }
        {   PCSTR text = g_WowUserClasses[window->Class].Name;
            while (text[length] && length < (INT)capacity - 1) { destination[length] = (BYTE)text[length]; ++length; }
            destination[length] = 0;
            WowNotePut(note, noteCapacity, &noteLength, " -> ");
            WowNoteQuoted(note, noteCapacity, &noteLength, text);
        }
        Wow32SetReturn(frame, (DWORD)length);
        return 1;
    }

    /* ── ★ 0xe0 GetWindowTask -- which Win16 task owns this window. ──────────
         Every window this host makes is created by the guest running on the exec
         thread, so the owner is the current task and the frame carries it.
       ⚠ A window that is not ours gets 0 rather than the current task: "I do not
         know" and "it belongs to you" are different answers and OLE branches on
         the difference. */
    case WOWUSER_GETWINDOWTASK: {
        WORD window16 = Wow32ArgWord(frame, 0);
        PCWOWUSER_WINDOW window = WowUserFindWindow(window16);
        INT  noteLength = 0;
        WowNotePut(note, noteCapacity, &noteLength, "GetWindowTask(0x");
        WowNoteHex(note, noteCapacity, &noteLength, window16, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, ")");
        if (!window) {
            WowNotePut(note, noteCapacity, &noteLength, " -- ★ NOT ONE OF OUR WINDOWS; answered 0");
            Wow32SetReturn(frame, 0);
            return 1;
        }
        /* s92 (#306): the task that CREATED it, now that there is more than one. */
        WowNotePut(note, noteCapacity, &noteLength, window->Task ? " -> its creator, task 0x"
                                           : " -> the current task 0x");
        WowNoteHex(note, noteCapacity, &noteLength, window->Task ? window->Task : g_WowUserCurrentTask, WOW_HEX_WORD_DIGITS);
        Wow32SetReturn(frame, window->Task ? window->Task : g_WowUserCurrentTask);
        return 1;
    }

    /* ── ★★ 0x18/0x19/0x1a RemoveProp / GetProp / SetProp ───────────────────
         A small global table rather than a list per window: the whole point is
         that a guest stores a handful of pointers and reads them back, and one
         table keeps the (hwnd, name) key in one place.
       ⚠ THE NAME MAY BE AN ATOM. `MAKEINTATOM` is a far pointer with a NULL
         selector and the atom in the offset, which `Wow32ArgString` correctly
         refuses to read -- so an implementation that only handled strings would
         store nothing and find nothing, and hand back the same null OLESVR just
         died on. Atoms are keyed as "#nnnn". */
    case WOWUSER_REMOVEPROP:
    case WOWUSER_GETPROP:
    case WOWUSER_SETPROP: {
        INT  isSet = (frame->Id == WOWUSER_SETPROP);
        WORD window16  = Wow32ArgWord(frame, isSet ? WOWUSER_PROP_ARG_HWND_S : WOWUSER_PROP_ARG_HWND_G);
        INT  nameOffset  = isSet ? WOWUSER_PROP_ARG_NAME_S : WOWUSER_PROP_ARG_NAME_G;
        DWORD namePointer   = Wow32ArgDword(frame, nameOffset);
        CHAR key[WOWUSER_SHORT_NAME_SIZE];
        INT  noteLength = 0, index, slot = -1;
        if (!Wow32ArgString(frame, nameOffset, key, sizeof key)) {
            /* a null selector: the offset IS the atom */
            WORD atom = (WORD)(namePointer & WORD_MASK);
            INT  index2 = 0, shift;
            key[index2++] = '#';
            for (shift = WOWUSER_ATOM_TOP_DIGIT_SHIFT; shift >= 0; shift -= WOW_HEX_DIGIT_BITS) {
                INT nibble = (atom >> shift) & WOW_HEX_DIGIT_MASK;
                key[index2++] = (CHAR)(nibble < WOWUSER_HEX_LETTER_VALUE ? '0' + nibble : 'a' + nibble - WOWUSER_HEX_LETTER_VALUE);
            }
            key[index2] = 0;
        }
        WowNotePut(note, noteCapacity, &noteLength,
                isSet ? "SetProp(0x" : frame->Id == WOWUSER_GETPROP ? "GetProp(0x"
                                                                : "RemoveProp(0x");
        WowNoteHex(note, noteCapacity, &noteLength, window16, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, ", ");
        WowNoteQuoted(note, noteCapacity, &noteLength, key);
        WowNotePut(note, noteCapacity, &noteLength, ")");
        for (index = 0; index < g_WowUserPropCount; ++index)
            if (g_WowUserProps[index].Window == window16
                && WowUserIsEqualNoCase(g_WowUserProps[index].Name, key)) { slot = index; break; }
        if (isSet) {
            WORD data = Wow32ArgWord(frame, WOWUSER_PROP_ARG_DATA_S);
            if (slot < 0) {
                if (g_WowUserPropCount >= WOWUSER_MAX_PROP) {
                    WowNotePut(note, noteCapacity, &noteLength, " -- ★ THE PROPERTY TABLE IS FULL;"
                                               " answered 0");
                    Wow32SetReturn(frame, 0);
                    return 1;
                }
                slot = g_WowUserPropCount++;
                g_WowUserProps[slot].Window = window16;
                { INT index2 = 0; while (key[index2] && index2 < (INT)sizeof g_WowUserProps[slot].Name - 1)
                    { g_WowUserProps[slot].Name[index2] = key[index2]; ++index2; }
                  g_WowUserProps[slot].Name[index2] = 0; }
            }
            g_WowUserProps[slot].Data = data;
            WowNotePut(note, noteCapacity, &noteLength, " = 0x");
            WowNoteHex(note, noteCapacity, &noteLength, data, WOW_HEX_WORD_DIGITS);
            Wow32SetReturn(frame, 1);
            return 1;
        }
        if (slot < 0) {
            WowNotePut(note, noteCapacity, &noteLength, " -> 0 (no such property)");
            Wow32SetReturn(frame, 0);
            return 1;
        }
        WowNotePut(note, noteCapacity, &noteLength, " -> 0x");
        WowNoteHex(note, noteCapacity, &noteLength, g_WowUserProps[slot].Data, WOW_HEX_WORD_DIGITS);
        Wow32SetReturn(frame, g_WowUserProps[slot].Data);
        if (frame->Id == WOWUSER_REMOVEPROP) {
            g_WowUserProps[slot] = g_WowUserProps[g_WowUserPropCount - 1];
            --g_WowUserPropCount;
            WowNotePut(note, noteCapacity, &noteLength, " (removed)");
        }
        return 1;
    }

    /* ── ★ 0x10e GlobalFindAtom / 0x10f GlobalGetAtomName -- the OS's table. */
    case WOWUSER_GLOBALFINDATOM: {
        CHAR text[WOWUSER_STRING_SIZE];
        INT  noteLength = 0;
        DWORD result;
        if (!Wow32ArgString(frame, 0, text, sizeof text)) {
            WowNotePut(note, noteCapacity, &noteLength, "GlobalFindAtom(NULL) -- answered 0");
            Wow32SetReturn(frame, 0);
            return 1;
        }
        WowNotePut(note, noteCapacity, &noteLength, "GlobalFindAtom(");
        WowNoteQuoted(note, noteCapacity, &noteLength, text);
        WowNotePut(note, noteCapacity, &noteLength, ") = 0x");
        result = (DWORD)GlobalFindAtomA(text);
        WowNoteHex(note, noteCapacity, &noteLength, result, WOW_HEX_WORD_DIGITS);
        Wow32SetReturn(frame, result);
        return 1;
    }

    case WOWUSER_GLOBALATOMNAME: {
        WORD atom   = Wow32ArgWord(frame, WOWUSER_GAN_ARG_ATOM);
        WORD capacity = Wow32ArgWord(frame, WOWUSER_GAN_ARG_SIZE);
        volatile BYTE *destination = Wow32ArgPointer(frame, WOWUSER_GAN_ARG_BUF);
        CHAR text[WOWUSER_STRING_SIZE];
        UINT length;
        INT  noteLength = 0, index;
        WowNotePut(note, noteCapacity, &noteLength, "GlobalGetAtomName(0x");
        WowNoteHex(note, noteCapacity, &noteLength, atom, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, ")");
        if (!destination || !capacity) {
            WowNotePut(note, noteCapacity, &noteLength, " -- ★ no buffer; answered 0");
            Wow32SetReturn(frame, 0);
            return 1;
        }
        length = GlobalGetAtomNameA(atom, text, sizeof text);
        if (!length) {
            WowNotePut(note, noteCapacity, &noteLength, " -- ★ no such atom; answered 0");
            Wow32SetReturn(frame, 0);
            return 1;
        }
        if (length > (UINT)capacity - 1) length = (UINT)capacity - 1;
        for (index = 0; index < (INT)length; ++index) destination[index] = (BYTE)text[index];
        destination[length] = 0;
        WowNotePut(note, noteCapacity, &noteLength, " -> ");
        WowNoteQuoted(note, noteCapacity, &noteLength, text);
        Wow32SetReturn(frame, length);
        return 1;
    }

    case WOWUSER_GETSCROLLPOS: {
        WORD window16 = Wow32ArgWord(frame, WOWUSER_GSP_ARG_HWND);
        WORD bar  = Wow32ArgWord(frame, WOWUSER_GSP_ARG_BAR);
        HWND window32    = WowUserHwnd32(window16);
        INT  noteLength = 0;
        WowNotePut(note, noteCapacity, &noteLength, "GetScrollPos(0x");
        WowNoteHex(note, noteCapacity, &noteLength, window16, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, ", bar ");
        WowNoteHex(note, noteCapacity, &noteLength, bar, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, ")");
        if (!window32) {
            WowNotePut(note, noteCapacity, &noteLength, " -- ★ NOT ONE OF OUR WINDOWS; answered 0");
            Wow32SetReturn(frame, 0);
            return 1;
        }
        Wow32SetReturn(frame, (DWORD)(WORD)(SHORT)GetScrollPos(window32, (INT)(SHORT)bar));
        return 1;
    }

    /* ══ THE THIRD AND FOURTH GUESTS: SOLITAIRE AND MINESWEEPER ═══════════════
       Enumerated from the two binaries with `tools/ne/neneeds.py --todo`, not
       found one-per-run by watching where they stop. Every one below is a call
       one of them really makes.
     ⚠ THE DIALOG CALLS ARE PASS-THROUGHS ON PURPOSE. `DialogBox` is 16-bit code
       inside USER (neneeds classifies it native16 and it is right), so by the
       time any of these runs, USER's own code has already been round this host's
       `CreateWindow` for the dialog and for each of its controls -- which means
       the controls ARE real Win32 windows under real ids, and the OS's own
       CheckDlgButton/GetDlgItemInt do exactly the right thing. There is nothing
       here for this host to reimplement, only handles to translate. */

    /* ── ★★ SetTimer -- THE OS KEEPS TIME, DispatchMessage CALLS THE PROC. ───
         The engine is the OS's own timer on the real HWND (see wowwin.h's
         WM_TIMER relay); this only records the 16-bit TIMERPROC, if there is
         one, so the relay can put it in the message's lParam where Win16 puts
         it. Nothing here calls into the guest -- see the timer table above for
         why that is faithful rather than a shortcut. */
    case WOWUSER_SETTIMER: {
        WORD  window16  = Wow32ArgWord(frame, WOWUSER_ST_ARG_HWND);
        WORD  timerId    = Wow32ArgWord(frame, WOWUSER_ST_ARG_ID);
        WORD  elapse    = Wow32ArgWord(frame, WOWUSER_ST_ARG_ELAPSE);
        DWORD procedure  = Wow32ArgDword(frame, WOWUSER_ST_ARG_PROC);
        HWND  window32     = WowUserHwnd32(window16);
        INT   noteLength = 0;
        UINT_PTR result;
        WowNotePut(note, noteCapacity, &noteLength, "SetTimer(0x");
        WowNoteHex(note, noteCapacity, &noteLength, window16, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, ", id ");
        WowNoteHex(note, noteCapacity, &noteLength, timerId, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, ", ");
        WowNoteHex(note, noteCapacity, &noteLength, elapse, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, "ms");
        if (procedure) {
            WowNotePut(note, noteCapacity, &noteLength, ", proc 0x");
            WowNoteHex(note, noteCapacity, &noteLength, procedure, WOW_HEX_DWORD_DIGITS);
        }
        WowNotePut(note, noteCapacity, &noteLength, ")");
        if (!window16 && procedure) {
            /* s93: a windowless timer -- see WowWinThreadTimerFire. Win16 picks the id. */
            result = SetTimer(NULL, 0, (UINT)elapse, NULL);
            if (result && !WowWinThreadTimerAdd(result, procedure)) { KillTimer(NULL, result); result = 0; }
            WowNotePut(note, noteCapacity, &noteLength, result ? " -> a THREAD timer, id 0x" : " -> ★ OS REFUSED");
            if (result) WowNoteHex(note, noteCapacity, &noteLength, (DWORD)result, WOW_HEX_WORD_DIGITS);
            Wow32SetReturn(frame, result ? (DWORD)(WORD)result : 0);
            return 1;
        }
        if (!window16 || !window32) {
            /* ⚠ A NULL hWnd TIMER HAS NOWHERE TO BE DELIVERED HERE. Win16 sends
                 those to the task's queue, which only a TIMERPROC or a message
                 loop that tolerates hwnd 0 can collect; ours keys on a window. */
            WowNotePut(note, noteCapacity, &noteLength, " -- ★ REFUSED: no window to deliver"
                                       " WM_TIMER to; answered 0");
            Wow32SetReturn(frame, 0);
            return 1;
        }
        result = SetTimer(window32, (UINT_PTR)timerId, (UINT)elapse, NULL);
        if (result) WowUserTimerSet(window16, timerId, procedure);
        WowNotePut(note, noteCapacity, &noteLength, result ? (procedure ? " -> armed, proc via DispatchMessage"
                                             : " -> armed")
                                     : " -> ★ OS REFUSED");
        /* Win16 returns the id it armed, 0 on failure. */
        Wow32SetReturn(frame, result ? (DWORD)timerId : 0);
        return 1;
    }

    case WOWUSER_KILLTIMER: {
        WORD window16 = Wow32ArgWord(frame, WOWUSER_KT_ARG_HWND);
        WORD timerId   = Wow32ArgWord(frame, WOWUSER_KT_ARG_ID);
        HWND window32    = WowUserHwnd32(window16);
        INT  noteLength = 0, result;
        WowNotePut(note, noteCapacity, &noteLength, "KillTimer(0x");
        WowNoteHex(note, noteCapacity, &noteLength, window16, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, ", id ");
        WowNoteHex(note, noteCapacity, &noteLength, timerId, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, ")");
        if (!window16 && WowWinThreadTimerKill((UINT_PTR)timerId)) {  /* s93: a thread timer */
            result = KillTimer(NULL, (UINT_PTR)timerId) ? 1 : 0;
            WowNotePut(note, noteCapacity, &noteLength, " -- a thread timer, killed");
            Wow32SetReturn(frame, (DWORD)result);
            return 1;
        }
        if (!window32) { WowNotePut(note, noteCapacity, &noteLength, " -- ★ NOT ONE OF OUR WINDOWS;"
                                             " answered 0");
                  Wow32SetReturn(frame, 0); return 1; }
        result = KillTimer(window32, (UINT_PTR)timerId) ? 1 : 0;
        WowUserTimerClear(window16, timerId);
        WowNotePut(note, noteCapacity, &noteLength, result ? " -> killed" : " -> not armed");
        Wow32SetReturn(frame, (DWORD)result);
        return 1;
    }

    /* GetCurrentTime is Win16's name for GetTickCount -- same milliseconds since
       boot, same DWORD. Minesweeper times its game with it. */
    case WOWUSER_GETCURRENTTIME: {
        DWORD tickCount = GetTickCount();
        INT   noteLength = 0;
        WowNotePut(note, noteCapacity, &noteLength, "GetCurrentTime -> 0x");
        WowNoteHex(note, noteCapacity, &noteLength, tickCount, WOW_HEX_DWORD_DIGITS);
        Wow32SetReturn(frame, tickCount);
        return 1;
    }

    /* ── AdjustWindowRect: pure arithmetic on a rectangle, no handles at all.
         ⚠ A Win16 RECT IS FOUR `int`s = 8 BYTES against Win32's four LONGs = 16,
           so it is unpacked and repacked rather than cast. Solitaire sizes its
           card table with this: it computes the client area it wants and asks
           what frame that needs. Getting it wrong gives a window whose felt is
           the wrong size by exactly the border. */
    case WOWUSER_ADJUSTWINDOWRECT: {
        volatile BYTE *rectBytes = Wow32ArgPointer(frame, WOWUSER_AWR_ARG_RECT);
        DWORD style = Wow32ArgDword(frame, WOWUSER_AWR_ARG_STYLE);
        WORD  hasMenu  = Wow32ArgWord(frame, WOWUSER_AWR_ARG_MENU);
        RECT  rect;
        INT   noteLength = 0;
        WowNotePut(note, noteCapacity, &noteLength, "AdjustWindowRect(style 0x");
        WowNoteHex(note, noteCapacity, &noteLength, style, WOW_HEX_DWORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, hasMenu ? ", with menu)" : ", no menu)");
        if (!rectBytes) {
            WowNotePut(note, noteCapacity, &noteLength, " -- ★ NULL lpRect; nothing written");
            Wow32SetReturn(frame, 0);
            return 1;
        }
        rect.left   = (SHORT)Wow32PeekWord(rectBytes + 0);
        rect.top    = (SHORT)Wow32PeekWord(rectBytes + WOWUSER_RECT16_TOP);
        rect.right  = (SHORT)Wow32PeekWord(rectBytes + WOWUSER_RECT16_RIGHT);
        rect.bottom = (SHORT)Wow32PeekWord(rectBytes + WOWUSER_RECT16_BOTTOM);
        AdjustWindowRect(&rect, style, hasMenu ? TRUE : FALSE);
        Wow32PokeWord(rectBytes + 0, (WORD)(SHORT)rect.left);
        Wow32PokeWord(rectBytes + WOWUSER_RECT16_TOP, (WORD)(SHORT)rect.top);
        Wow32PokeWord(rectBytes + WOWUSER_RECT16_RIGHT, (WORD)(SHORT)rect.right);
        Wow32PokeWord(rectBytes + WOWUSER_RECT16_BOTTOM, (WORD)(SHORT)rect.bottom);
        WowNotePut(note, noteCapacity, &noteLength, " -> ");
        WowNoteHex(note, noteCapacity, &noteLength, (DWORD)(rect.right - rect.left), WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, "x");
        WowNoteHex(note, noteCapacity, &noteLength, (DWORD)(rect.bottom - rect.top), WOW_HEX_WORD_DIGITS);
        Wow32SetReturn(frame, 0);
        return 1;
    }

    case WOWUSER_GETLASTACTIVEPOPUP: {
        WORD window16 = Wow32ArgWord(frame, WOWUSER_GLAP_ARG_HWND);
        HWND window32    = WowUserHwnd32(window16);
        INT  noteLength = 0;
        WORD result16;
        WowNotePut(note, noteCapacity, &noteLength, "GetLastActivePopup(0x");
        WowNoteHex(note, noteCapacity, &noteLength, window16, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, ")");
        if (!window32) {
            /* ⚠ ANSWER THE OWNER, NOT 0. The documented return for a window with
                 no popup is the window ITSELF, and every caller uses the result
                 as a window to activate -- so 0 here is the sentinel-means-yes
                 shape that has cost this project four sessions. */
            WowNotePut(note, noteCapacity, &noteLength, " -- not one of ours; echoed the owner back");
            Wow32SetReturn(frame, (DWORD)window16);
            return 1;
        }
        result16 = WowWinHwnd16(GetLastActivePopup(window32));
        if (!result16) result16 = window16;
        WowNotePut(note, noteCapacity, &noteLength, " -> 0x");
        WowNoteHex(note, noteCapacity, &noteLength, result16, WOW_HEX_WORD_DIGITS);
        Wow32SetReturn(frame, (DWORD)result16);
        return 1;
    }

    case WOWUSER_FINDWINDOW: {
        CHAR className[WOWUSER_TEXT_SIZE], windowName[WOWUSER_TEXT_SIZE];
        DWORD classPointer = Wow32ArgDword(frame, WOWUSER_FW_ARG_CLASS);
        DWORD namePointer = Wow32ArgDword(frame, WOWUSER_FW_ARG_NAME);
        INT   hasClass = classPointer && WowUserFarString(frame, classPointer, className, sizeof className);
        INT   hasName = namePointer && WowUserFarString(frame, namePointer, windowName, sizeof windowName);
        HWND  window32;
        WORD  result16;
        INT   noteLength = 0;
        WowNotePut(note, noteCapacity, &noteLength, "FindWindow(");
        WowNotePut(note, noteCapacity, &noteLength, hasClass ? className : "(null)");
        WowNotePut(note, noteCapacity, &noteLength, ", ");
        WowNotePut(note, noteCapacity, &noteLength, hasName ? windowName : "(null)");
        WowNotePut(note, noteCapacity, &noteLength, ")");
        /* ★ SEARCHED OVER THE WHOLE DESKTOP, WHICH IS THE HONEST ANSWER HERE:
             our guest windows ARE real top-level windows on it, so the OS's own
             search sees exactly what a Win16 FindWindow would have seen, plus
             the host's other windows. Minesweeper uses this to find a previous
             instance of itself; a stale 32-bit window of ours cannot match its
             class name, so the extra scope costs nothing measurable. */
        window32 = WowUserFindWindowByClass(hasClass ? className : NULL, hasName ? windowName : NULL);
        result16 = window32 ? WowWinHwnd16(window32) : 0;
        if (window32 && !result16) {
            /* Found something that is not a guest window: the guest cannot be
               handed a handle it has no token for, and saying so beats inventing
               one. */
            WowNotePut(note, noteCapacity, &noteLength, " -- found a NON-GUEST window; answered 0");
            Wow32SetReturn(frame, 0);
            return 1;
        }
        WowNotePut(note, noteCapacity, &noteLength, result16 ? " -> 0x" : " -> not found");
        if (result16) WowNoteHex(note, noteCapacity, &noteLength, result16, WOW_HEX_WORD_DIGITS);
        Wow32SetReturn(frame, (DWORD)result16);
        return 1;
    }

    /* ★ THE MENU IS BUILT FROM THE PROGRAM'S OWN RESOURCE, by the same builder
         the class-menu path already uses -- so Minesweeper's Game/Help bar comes
         out of WINMINE.EXE exactly the way Solitaire's comes out of SOL.EXE.
       ⚠ ONLY THE LAUNCHED PROGRAM'S RESOURCES ARE SEARCHED. A LoadMenu against a
         DLL's hInstance would need that module's file, which this host does not
         track per-instance yet; it is refused loudly rather than answered with
         the program's menu, because the wrong menu is worse than none. */
    case WOWUSER_LOADMENU: {
        DWORD name  = Wow32ArgDword(frame, WOWUSER_LOADMENU_ARG_NAME);
        WORD  instance = Wow32ArgWord(frame, WOWUSER_LOADMENU_ARG_HINST);
        CHAR  nameBuffer[WOWUSER_NAME_SIZE];
        INT   menuItems = 0, noteLength = 0;
        HMENU menu = NULL;
        WORD  menu16;
        WowNotePut(note, noteCapacity, &noteLength, "LoadMenu(hInst 0x");
        WowNoteHex(note, noteCapacity, &noteLength, instance, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, ", ");
        if (!WowResOpen(WowUserResourceProgram())) {
            WowNotePut(note, noteCapacity, &noteLength, "?) -- ★ CANNOT OPEN THE PROGRAM'S OWN"
                                       " FILE; answered 0");
            Wow32SetReturn(frame, 0);
            return 1;
        }
        if ((name >> WORD_SHIFT) == 0) {
            /* MAKEINTRESOURCE: the high word is 0, so the low word is an ordinal. */
            WowNotePut(note, noteCapacity, &noteLength, "#");
            WowNoteHex(note, noteCapacity, &noteLength, name & WORD_MASK, WOW_HEX_WORD_DIGITS);
            menu = WowResMenu((WORD)(name & WORD_MASK), &menuItems);
        } else if (WowUserFarString(frame, name, nameBuffer, sizeof nameBuffer)) {
            WowNoteQuoted(note, noteCapacity, &noteLength, nameBuffer);
            menu = WowResMenuByName(nameBuffer, &menuItems);
        } else {
            WowNotePut(note, noteCapacity, &noteLength, "?");
        }
        WowNotePut(note, noteCapacity, &noteLength, ")");
        if (!menu) {
            WowNotePut(note, noteCapacity, &noteLength, " -- ★ NO SUCH MENU RESOURCE; answered 0");
            Wow32SetReturn(frame, 0);
            return 1;
        }
        menu16 = WowUserMenu16(menu);
        if (!menu16) {
            DestroyMenu(menu);        /* no token = the guest never learns of it */
            WowNotePut(note, noteCapacity, &noteLength, " -- ★ MENU TOKEN TABLE FULL; destroyed"
                                       " and answered 0");
            Wow32SetReturn(frame, 0);
            return 1;
        }
        WowNotePut(note, noteCapacity, &noteLength, " -> menu 0x");
        WowNoteHex(note, noteCapacity, &noteLength, menu16, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, ", ");
        WowNoteHex(note, noteCapacity, &noteLength, (DWORD)menuItems, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, " items");
        Wow32SetReturn(frame, (DWORD)menu16);
        return 1;
    }

    case WOWUSER_SETMENU: {
        WORD window16 = Wow32ArgWord(frame, WOWUSER_SETMENU_ARG_HWND);
        WORD menu16   = Wow32ArgWord(frame, WOWUSER_SETMENU_ARG_MENU);
        HWND window32    = WowUserHwnd32(window16);
        HMENU menu   = menu16 ? WowUserMenu32(menu16) : NULL;
        INT   noteLength = 0, result;
        WowNotePut(note, noteCapacity, &noteLength, "SetMenu(0x");
        WowNoteHex(note, noteCapacity, &noteLength, window16, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, ", menu 0x");
        WowNoteHex(note, noteCapacity, &noteLength, menu16, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, ")");
        if (!window32) { WowNotePut(note, noteCapacity, &noteLength, " -- ★ NOT ONE OF OUR WINDOWS;"
                                             " answered 0");
                  Wow32SetReturn(frame, 0); return 1; }
        if (menu16 && !menu) { WowNotePut(note, noteCapacity, &noteLength, " -- ★ NOT ONE OF OUR MENU"
                                                   " TOKENS; answered 0");
                        Wow32SetReturn(frame, 0); return 1; }
        result = SetMenu(window32, menu) ? 1 : 0;
        if (result) DrawMenuBar(window32);  /* the bar's height changed; Win16 redraws */
        WowNotePut(note, noteCapacity, &noteLength, result ? " -> set" : " -> ★ OS REFUSED");
        Wow32SetReturn(frame, (DWORD)result);
        return 1;
    }

    case WOWUSER_SETDLGITEMTEXT: {
        WORD  dialog16 = Wow32ArgWord(frame, WOWUSER_SDIT_ARG_HDLG);
        WORD  itemId   = Wow32ArgWord(frame, WOWUSER_SDIT_ARG_ID);
        DWORD textPointer   = Wow32ArgDword(frame, WOWUSER_SDIT_ARG_TEXT);
        HWND  window32    = WowUserHwnd32(dialog16);
        CHAR  buffer[WOWUSER_STRING_SIZE];
        INT   noteLength = 0;
        WowNotePut(note, noteCapacity, &noteLength, "SetDlgItemText dlg 0x");
        WowNoteHex(note, noteCapacity, &noteLength, dialog16, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, " id 0x");
        WowNoteHex(note, noteCapacity, &noteLength, itemId, WOW_HEX_WORD_DIGITS);
        if (!window32) { WowNotePut(note, noteCapacity, &noteLength, " -- no real window");
                  Wow32SetReturn(frame, 0); return 1; }
        if (!textPointer || !WowUserFarString(frame, textPointer, buffer, sizeof buffer)) buffer[0] = 0;
        WowNotePut(note, noteCapacity, &noteLength, " = \"");
        WowNotePut(note, noteCapacity, &noteLength, buffer);
        WowNotePut(note, noteCapacity, &noteLength, "\"");
        /* s90 (#278): Win16's SetDlgItemText IS SetWindowText(GetDlgItem(...)), so an
           item that is one of OUR windows gets what SetWindowText gives it (s89): its
           record's text, then WM_SETTEXT sent to its own 16-bit procedure with the
           program's string, then the real window. Going straight to the OS reached
           only the relay -- Sound Recorder's "noflickertext" Position readout was set
           to "1.98 sec." 37 times during playback and kept showing "0.00 sec." */
        {   HWND control32 = GetDlgItem(window32, (INT)(SHORT)itemId);
            WORD control16 = control32 ? WowWinHwnd16(control32) : 0;
            PWOWUSER_WINDOW control = control16 ? WowUserFindWindow(control16) : NULL;
            if (control && control->WindowProcedure) {
                static INT isSent = 0;
                WORD result16;
                INT index;
                for (index = 0; index < (INT)sizeof control->Text - 1 && buffer[index]; ++index) control->Text[index] = buffer[index];
                control->Text[index] = 0;
                SetWindowTextA(control32, buffer);  /* first: see SetWindowText */
                if (!isSent && g_WowUserSend16 && textPointer) {
                    ++isSent;
                    if (g_WowUserSend16(control16, WM_SETTEXT16, 0, textPointer, &result16))
                        WowNotePut(note, noteCapacity, &noteLength, " -> WM_SETTEXT SENT to the item's procedure");
                    --isSent;
                }
                Wow32SetReturn(frame, 0);
                return 1;
            }
        }
        SetDlgItemTextA(window32, (INT)(SHORT)itemId, buffer);
        Wow32SetReturn(frame, 0);
        return 1;
    }

    case WOWUSER_GETDLGITEMINT: {
        WORD  dialog16 = Wow32ArgWord(frame, WOWUSER_GDII_ARG_HDLG);
        WORD  itemId   = Wow32ArgWord(frame, WOWUSER_GDII_ARG_ID);
        WORD  isSigned  = Wow32ArgWord(frame, WOWUSER_GDII_ARG_SIGNED);
        volatile BYTE *translatedBytes = Wow32ArgPointer(frame, WOWUSER_GDII_ARG_XLATED);
        HWND  window32 = WowUserHwnd32(dialog16);
        BOOL  isOk = FALSE;
        UINT  value;
        INT   noteLength = 0;
        WowNotePut(note, noteCapacity, &noteLength, "GetDlgItemInt dlg 0x");
        WowNoteHex(note, noteCapacity, &noteLength, dialog16, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, " id 0x");
        WowNoteHex(note, noteCapacity, &noteLength, itemId, WOW_HEX_WORD_DIGITS);
        if (!window32) {
            /* ⚠ lpTranslated MUST BE WRITTEN FALSE, not left alone. It is the
                 caller's only way to tell "the box said 0" from "the box was not
                 a number", and leaving it as stack litter makes a failure read
                 as a valid 0. */
            if (translatedBytes) Wow32PokeWord(translatedBytes, 0);
            WowNotePut(note, noteCapacity, &noteLength, " -- no real window; 0, not translated");
            Wow32SetReturn(frame, 0);
            return 1;
        }
        value = GetDlgItemInt(window32, (INT)(SHORT)itemId, &isOk, isSigned ? TRUE : FALSE);
        if (translatedBytes) Wow32PokeWord(translatedBytes, (WORD)(isOk ? 1 : 0));
        WowNotePut(note, noteCapacity, &noteLength, isOk ? " -> 0x" : " -> NOT A NUMBER, 0x");
        WowNoteHex(note, noteCapacity, &noteLength, (DWORD)value, WOW_HEX_WORD_DIGITS);
        Wow32SetReturn(frame, (DWORD)(WORD)value);
        return 1;
    }

    case WOWUSER_CHECKDLGBUTTON: {
        WORD dialog16 = Wow32ArgWord(frame, WOWUSER_CDB_ARG_HDLG);
        WORD itemId   = Wow32ArgWord(frame, WOWUSER_CDB_ARG_ID);
        WORD check  = Wow32ArgWord(frame, WOWUSER_CDB_ARG_CHECK);
        HWND window32    = WowUserHwnd32(dialog16);
        INT  noteLength = 0;
        WowNotePut(note, noteCapacity, &noteLength, "CheckDlgButton dlg 0x");
        WowNoteHex(note, noteCapacity, &noteLength, dialog16, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, " id 0x");
        WowNoteHex(note, noteCapacity, &noteLength, itemId, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, check ? " = CHECKED" : " = clear");
        if (!window32) { WowNotePut(note, noteCapacity, &noteLength, " -- no real window");
                  Wow32SetReturn(frame, 0); return 1; }
        CheckDlgButton(window32, (INT)(SHORT)itemId, (UINT)check);
        Wow32SetReturn(frame, 0);
        return 1;
    }

    case WOWUSER_CHECKRADIOBUTTON: {
        WORD dialog16  = Wow32ArgWord(frame, WOWUSER_CRB_ARG_HDLG);
        WORD first = Wow32ArgWord(frame, WOWUSER_CRB_ARG_FIRST);
        WORD last  = Wow32ArgWord(frame, WOWUSER_CRB_ARG_LAST);
        WORD check   = Wow32ArgWord(frame, WOWUSER_CRB_ARG_CHECK);
        HWND window32     = WowUserHwnd32(dialog16);
        INT  noteLength = 0;
        WowNotePut(note, noteCapacity, &noteLength, "CheckRadioButton dlg 0x");
        WowNoteHex(note, noteCapacity, &noteLength, dialog16, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, " ids ");
        WowNoteHex(note, noteCapacity, &noteLength, first, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, "..");
        WowNoteHex(note, noteCapacity, &noteLength, last, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, " check ");
        WowNoteHex(note, noteCapacity, &noteLength, check, WOW_HEX_WORD_DIGITS);
        if (!window32) { WowNotePut(note, noteCapacity, &noteLength, " -- no real window");
                  Wow32SetReturn(frame, 0); return 1; }
        CheckRadioButton(window32, (INT)(SHORT)first, (INT)(SHORT)last,
                         (INT)(SHORT)check);
        Wow32SetReturn(frame, 0);
        return 1;
    }

    case WOWUSER_ISDLGBUTTONCHECKED: {
        WORD dialog16 = Wow32ArgWord(frame, WOWUSER_IDBC_ARG_HDLG);
        WORD itemId   = Wow32ArgWord(frame, WOWUSER_IDBC_ARG_ID);
        HWND window32    = WowUserHwnd32(dialog16);
        INT  noteLength = 0;
        UINT result;
        WowNotePut(note, noteCapacity, &noteLength, "IsDlgButtonChecked dlg 0x");
        WowNoteHex(note, noteCapacity, &noteLength, dialog16, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, " id 0x");
        WowNoteHex(note, noteCapacity, &noteLength, itemId, WOW_HEX_WORD_DIGITS);
        if (!window32) { WowNotePut(note, noteCapacity, &noteLength, " -- no real window; answered 0");
                  Wow32SetReturn(frame, 0); return 1; }
        result = IsDlgButtonChecked(window32, (INT)(SHORT)itemId);
        WowNotePut(note, noteCapacity, &noteLength, result ? " -> CHECKED" : " -> clear");
        Wow32SetReturn(frame, (DWORD)(WORD)result);
        return 1;
    }

    /* ── DrawText and FrameRect: USER calls that take a GDI DC. ───────────────
         ⚠ THE DC TOKEN IS GDI'S ID SPACE, NOT USER'S, and this is the seam where
           that matters: the handle arrives in a USER call and only WowGdiH32
           can resolve it. Solitaire draws its status line with DrawText and its
           drag outline with FrameRect. */
    case WOWUSER_DRAWTEXT: {
        WORD  dc16   = Wow32ArgWord(frame, WOWUSER_DT_ARG_HDC);
        DWORD textPointer    = Wow32ArgDword(frame, WOWUSER_DT_ARG_STR);
        WORD  count   = Wow32ArgWord(frame, WOWUSER_DT_ARG_COUNT);
        volatile BYTE *rectBytes = Wow32ArgPointer(frame, WOWUSER_DT_ARG_RECT);
        WORD  format   = Wow32ArgWord(frame, WOWUSER_DT_ARG_FORMAT);
        INT   dcKind = -1;
        HGDIOBJ dc = WowGdiH32(dc16, &dcKind);
        CHAR  buffer[WOWUSER_LONG_TEXT_SIZE];
        RECT  rect;
        INT   noteLength = 0, textLength, result;
        WowNotePut(note, noteCapacity, &noteLength, "DrawText(dc 0x");
        WowNoteHex(note, noteCapacity, &noteLength, dc16, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, ", fmt 0x");
        WowNoteHex(note, noteCapacity, &noteLength, format, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, ")");
        if (!dc || (dcKind != WOWGDI_KIND_DC && dcKind != WOWGDI_KIND_WINDC) || !rectBytes) {
            WowNotePut(note, noteCapacity, &noteLength, " -- ★ NOT ONE OF OUR DC TOKENS, or no"
                                       " lpRect; answered 0");
            Wow32SetReturn(frame, 0);
            return 1;
        }
        if (!textPointer || !WowUserFarString(frame, textPointer, buffer, sizeof buffer)) buffer[0] = 0;
        /* ⚠ nCount = -1 MEANS "NUL-TERMINATED" and is the usual call. Anything
             else is a byte count, and it is clamped to what was actually
             copied -- handing Win32 a longer count than the buffer holds reads
             off the end of OUR memory, not the guest's. */
        textLength = (INT)(SHORT)count;
        if (textLength >= 0) { INT have = 0; while (have < (INT)sizeof buffer && buffer[have]) ++have;
                      if (textLength > have) textLength = have; }
        else textLength = -1;
        rect.left   = (SHORT)Wow32PeekWord(rectBytes + 0);
        rect.top    = (SHORT)Wow32PeekWord(rectBytes + WOWUSER_RECT16_TOP);
        rect.right  = (SHORT)Wow32PeekWord(rectBytes + WOWUSER_RECT16_RIGHT);
        rect.bottom = (SHORT)Wow32PeekWord(rectBytes + WOWUSER_RECT16_BOTTOM);
        result = DrawTextA((HDC)dc, buffer, textLength, &rect, (UINT)format);
        /* DT_CALCRECT asks for the rectangle BACK, so it is always written out:
           for every other format the values are unchanged and writing them is a
           no-op. */
        Wow32PokeWord(rectBytes + 0, (WORD)(SHORT)rect.left);
        Wow32PokeWord(rectBytes + WOWUSER_RECT16_TOP, (WORD)(SHORT)rect.top);
        Wow32PokeWord(rectBytes + WOWUSER_RECT16_RIGHT, (WORD)(SHORT)rect.right);
        Wow32PokeWord(rectBytes + WOWUSER_RECT16_BOTTOM, (WORD)(SHORT)rect.bottom);
        WowNotePut(note, noteCapacity, &noteLength, " \"");
        WowNotePut(note, noteCapacity, &noteLength, buffer);
        WowNotePut(note, noteCapacity, &noteLength, "\" -> h=");
        WowNoteHex(note, noteCapacity, &noteLength, (DWORD)result, WOW_HEX_WORD_DIGITS);
        Wow32SetReturn(frame, (DWORD)(WORD)result);
        return 1;
    }

    case WOWUSER_FRAMERECT: {
        WORD  dc16 = Wow32ArgWord(frame, WOWUSER_FRAMER_ARG_HDC);
        volatile BYTE *rectBytes = Wow32ArgPointer(frame, WOWUSER_FRAMER_ARG_RECT);
        WORD  brush16 = Wow32ArgWord(frame, WOWUSER_FRAMER_ARG_BRUSH);
        INT   dcKind = -1, brushKind = -1;
        HGDIOBJ dc = WowGdiH32(dc16, &dcKind);
        HGDIOBJ brush = WowGdiH32(brush16, &brushKind);
        RECT  rect;
        INT   noteLength = 0, result;
        WowNotePut(note, noteCapacity, &noteLength, "FrameRect(dc 0x");
        WowNoteHex(note, noteCapacity, &noteLength, dc16, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, ", brush 0x");
        WowNoteHex(note, noteCapacity, &noteLength, brush16, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, ")");
        if (!dc || (dcKind != WOWGDI_KIND_DC && dcKind != WOWGDI_KIND_WINDC) || !rectBytes) {
            WowNotePut(note, noteCapacity, &noteLength, " -- ★ NOT ONE OF OUR DC TOKENS, or no"
                                       " lpRect; answered 0");
            Wow32SetReturn(frame, 0);
            return 1;
        }
        if (!brush) {
            WowNotePut(note, noteCapacity, &noteLength, " -- ★ NOT ONE OF OUR BRUSH TOKENS;"
                                       " answered 0");
            Wow32SetReturn(frame, 0);
            return 1;
        }
        rect.left   = (SHORT)Wow32PeekWord(rectBytes + 0);
        rect.top    = (SHORT)Wow32PeekWord(rectBytes + WOWUSER_RECT16_TOP);
        rect.right  = (SHORT)Wow32PeekWord(rectBytes + WOWUSER_RECT16_RIGHT);
        rect.bottom = (SHORT)Wow32PeekWord(rectBytes + WOWUSER_RECT16_BOTTOM);
        result = FrameRect((HDC)dc, &rect, (HBRUSH)brush);
        WowNotePut(note, noteCapacity, &noteLength, " -> ");
        WowNoteHex(note, noteCapacity, &noteLength, (DWORD)result, WOW_HEX_WORD_DIGITS);
        Wow32SetReturn(frame, (DWORD)(WORD)result);
        return 1;
    }

    default:
        return 0;
    }
}

#endif /* WOWUSER_H */
