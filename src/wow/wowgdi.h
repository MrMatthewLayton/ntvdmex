#ifndef WOWGDI_H
#define WOWGDI_H
/*
 * wowgdi.h -- ★★ GDI.EXE's OWN ID SPACE.  GH #128, session 44.
 *
 * A SEVENTH id space, and the first one opened for its own sake rather than
 * because a guest stopped on it: `tools/ne/neneeds.py` says NOTEPAD.EXE reaches
 * three of GDI's thunks directly, and MS PAINT reaches **41**. So this file is
 * where the north star's other half begins, and the three below are its first
 * three lines rather than the whole of it.
 *
 * ── THE IDS, FROM THE FILE ──────────────────────────────────────────────────
 * GDI's exports are tail-jumps, like USER's, so the stub is one hop past the
 * entry point (`neneeds.py` follows it; see the note there):
 *      68 DELETEDC       -> stub seg1:0x032d  id 0x44   2 args  retstub 0x033a
 *      69 DELETEOBJECT   -> stub seg1:0x0347  id 0x45   2 args  retstub 0x0354
 *      80 GETDEVICECAPS  -> stub seg1:0x05d1  id 0x50   4 args  retstub 0x05de
 * The ids are the export ordinals again -- checked here, as it is checked per
 * module, and never assumed: krnl386's are nothing like its ordinals.
 *
 * ── ★★ WHY THERE IS AN OBJECT MAP AND NOT A CAST ───────────────────────────
 * A Win32 `HDC`, `HBRUSH`, `HBITMAP` or `HFONT` is 32 bits and a Win16 program
 * has 16 to hold it in. Every one of them also travels back through 16-bit code
 * -- GDI's own `CreateDC` and `GetStockObject` are `native16` wrappers -- so the
 * value has to survive a round trip and still name the right object. Same answer
 * as the windows, the menus and the cursor/icon tokens, and for the same reason:
 * a truncated pointer would name the wrong object and would not fail loudly.
 *
 * ── ★★ THE PRODUCERS ARRIVED IN SESSION 45, AND THE FIRST ONE WAS NOT HERE ──
 * Session 44 left this file saying the honest gap was that nothing yet PRODUCED
 * a DC or an object, so a handle arriving here would be one this host never
 * issued. That is now closed, and the way it closed is worth keeping: the first
 * device context this host ever issued came out of **USER's `GetDC`**, not out
 * of GDI at all (see wowuser.h). The plan on record had GDI's own `CreateDC`
 * first; a run of MS Paint said otherwise.
 * The producers here now are `CreateDC` (0x99), `CreateCompatibleDC` (0x34),
 * `CreateBitmap` (0x30), `CreateCompatibleBitmap` (0x33), `CreateSolidBrush`
 * (0x42) and `GetStockObject` (0x57) -- and three of those six are `native16`
 * wrappers whose ids NO amount of reading the export table could give. Each was
 * named from the call it actually made, and each is written up where it is
 * implemented.
 */

#define WOWGDI_DELETEDC       0x0044
#define WOWGDI_DELETEOBJECT   0x0045
#define WOWGDI_GETDEVICECAPS  0x0050

#define GDC_ARG_INDEX   0
#define GDC_ARG_HDC     2
#define DOBJ_ARG_HANDLE 0

/* ── ★★★ THE PRODUCERS -- WHAT MS PAINT ASKS FOR ONCE IT HAS A WINDOW. ───────
     With `GetDC` in place (it is USER's call, not GDI's -- see wowuser.h) Paint
     gets as far as needing a canvas, and says so in its own words when it does
     not get one: "Not enough memory to edit image."

   ★★ 0x57 IS `GetStockObject`, AND ONLY A RUN COULD HAVE SAID SO. Its export
     (GDI ordinal 87) is `native16` -- 16-bit code that reaches a WOW32 stub from
     inside its own body -- so `neneeds.py` cannot see the stub and correctly
     classifies the import as free. Session 44 wrote down that its id would have
     to come from a run; this is that run, 24 calls of it, and the file agrees
     afterwards: the stub at seg1:0x06a4 is `6a 02 / 68 00 00 / 68 57 00 / 9a`,
     the neighbouring stub at 0x06b1 is id 0x58 = GDI.88 GETSTRETCHBLTMODE, and
     the ids run with the ordinals either side of 87.

     The rest are ordinary exports, from `neneeds.py --stubs`:
       ord 45 SELECTOBJECT           id 0x2d   4 args  retstub 0x0a0b
       ord 51 CREATECOMPATIBLEBITMAP id 0x33   6 args  retstub 0x01db
       ord 52 CREATECOMPATIBLEDC     id 0x34   2 args  retstub 0x01e8
       ord 66 CREATESOLIDBRUSH       id 0x42   4 args  retstub 0x0306
     Blocks reversed as always, and each adds up to what its stub declares:
     (HDC)=2, (HDC,HGDIOBJ)=4, (COLORREF)=4, (HDC,int,int)=6. */
/* ── ★★★★ THE FIRST DRAWING CALLS. ──────────────────────────────────────────
     Named by the run in which WM_PAINT was relayed to a guest for the first
     time (session 45): MS Paint answered it with ten MoveTo/LineTo pairs and two
     PatBlts. All three are ordinary exports, and `neneeds.py --stubs` agrees
     with the return addresses the run printed:
       ord 19 LINETO   id 0x13   6 args  retstub 0x07dc
       ord 20 MOVETO   id 0x14   6 args  retstub 0x0810
       ord 29 PATBLT   id 0x1d  14 args  retstub 0x0885
     Reversed as always: (HDC,int,int) puts y at +0, x at +2 and the DC at +4;
     PatBlt's (HDC,int,int,int,int,DWORD) puts the 4-byte rop at +0 and the DC at
     +12, which is the 14 bytes its stub declares.
   ⚠ THE COORDINATES ARE SIGNED 16-BIT and must be sign-extended, not widened:
     a Win16 program draws at negative coordinates routinely (scrolled content),
     and 0xFFF0 read as 65520 would put the line off the far edge instead of 16
     pixels to the left.
   ★ MoveTo RETURNS THE PREVIOUS POSITION packed as y:x in a DWORD, which is why
     it is not simply a void call -- guests save and restore it. */
/* ── ★★★★ THE REST OF PAINT'S DRAWING SET. ──────────────────────────────────
     Named from the calls themselves, and every one of them confirms its own
     reading out of the values it carried:

       0x22 BITBLT      20 args  (0020 00cc | 0000 | 0000 | 2090 | 029d | 04d1 |
                                  0000 | 0000 | 20c0)
            ★ the rop is 0x00CC0020 = SRCCOPY, and the two DC fields are both
              tokens of ours -- which is what pins which end is source.
       0x1b RECTANGLE   10 args  (0204 | 0079 | 0000 | 0000 | 20c0)
            ★ = Rectangle(hdc, 0,0, 121, 516) -- exactly the toolbox's own size,
              so this is its border.
       0x04 SETROP2      4 args  (000d | 20c0)  -- 13 = R2_COPYPEN
       0x07 SETSTRETCHBLTMODE  4 (0003 | 20c0)  -- 3 = COLORONCOLOR
       0x01 SETBKCOLOR   6 args      0x09 SETTEXTCOLOR  6 args
       0x0b SETWINDOWORG 6 args      0x1e SAVEDC 2       0x27 RESTOREDC 4
     ⚠ 0x04 and 0x07 are internal stubs (their exports are native16), so only the
       run could name them -- and the two constants are what makes it a reading
       rather than a guess at the ordinal.
   ★ The raster ops, the ROP2 codes, the stretch modes and COLORREF all mean the
     same thing in both worlds, so those travel unchanged; only the handles and
     the signed 16-bit coordinates need work. */
#define WOWGDI_SETBKCOLOR       0x0001
#define WOWGDI_SETTEXTCOLOR     0x0009
#define COL_ARG_COLOR   0
#define COL_ARG_HDC     4

#define WOWGDI_SETROP2          0x0004
#define WOWGDI_SETSTRETCHMODE   0x0007
#define MODE_ARG_MODE   0
#define MODE_ARG_HDC    2

#define WOWGDI_SETWINDOWORG     0x000b
#define ORG_ARG_Y       0
#define ORG_ARG_X       2
#define ORG_ARG_HDC     4

#define WOWGDI_RECTANGLE        0x001b
#define RC_ARG_BOTTOM   0
#define RC_ARG_RIGHT    2
#define RC_ARG_TOP      4
#define RC_ARG_LEFT     6
#define RC_ARG_HDC      8

#define WOWGDI_SAVEDC           0x001e
#define SDC_ARG_HDC     0
#define WOWGDI_RESTOREDC        0x0027
#define RDC2_ARG_LEVEL  0
#define RDC2_ARG_HDC    2

/* ── ★★★★★ 0x23 StretchBlt -- THE TOOL ICONS THEMSELVES. ────────────────────
     GDI ordinal 35, a direct export, 24 argument bytes, and its own call names
     every field:

       (0020 00cc | 0117 | 003a | 0000 | 0000 | 20c8 |
                    0204 | 0079 | 0000 | 0000 | 20c0)

     = StretchBlt(dst, 0,0, 0x79 x 0x204, src, 0,0, 0x3a x 0x117, SRCCOPY)
     -- 58x279 stretched into 121x516. ★ 58x279 is exactly the pToolbox DIB this
     host loads, and 121x516 is exactly the toolbox window's size as measured
     against stock. Two numbers this session already knew independently, both
     turning up in one argument block. */
#define WOWGDI_STRETCHBLT       0x0023
#define SB_ARG_ROP      0        /* DWORD */
#define SB_ARG_SRCH     4
#define SB_ARG_SRCW     6
#define SB_ARG_SRCY     8
#define SB_ARG_SRCX    10
#define SB_ARG_SRCDC   12
#define SB_ARG_DSTH    14
#define SB_ARG_DSTW    16
#define SB_ARG_DSTY    18
#define SB_ARG_DSTX    20
#define SB_ARG_DSTDC   22

#define WOWGDI_BITBLT           0x0022
#define BB_ARG_ROP      0        /* DWORD */
#define BB_ARG_SRCY     4
#define BB_ARG_SRCX     6
#define BB_ARG_SRCDC    8
#define BB_ARG_HEIGHT  10
#define BB_ARG_WIDTH   12
#define BB_ARG_Y       14
#define BB_ARG_X       16
#define BB_ARG_DSTDC   18

#define WOWGDI_LINETO           0x0013
#define WOWGDI_MOVETO           0x0014
#define XY_ARG_Y        0
#define XY_ARG_X        2
#define XY_ARG_HDC      4

#define WOWGDI_PATBLT           0x001d
#define PB_ARG_ROP      0
#define PB_ARG_HEIGHT   4
#define PB_ARG_WIDTH    6
#define PB_ARG_Y        8
#define PB_ARG_X       10
#define PB_ARG_HDC     12

#define WOWGDI_SELECTOBJECT     0x002d
#define SEL_ARG_OBJ     0
#define SEL_ARG_HDC     2

/* ── ★★★ 0x30 CreateBitmap, named from the call it actually made ────────────
     GDI ordinal 48 `CREATEBITMAP` is `native16`, so -- as with `CreateDC` and
     `GetStockObject` -- only a run could give its internal id. It arrived as

       FUNC=0x00000030 args=0x0c retstub=0x01b4 (0000 0000 | 0001 | 0001 | 03ce | 0690)

     Twelve argument bytes is `(int, int, BYTE, BYTE, const void FAR*)`, which is
     CreateBitmap's list and nothing else's, and reversed it reads
     `CreateBitmap(0x0690, 0x03ce, 1, 1, NULL)` -- 1680 x 974, monochrome. ★ The
     numbers are the confirmation: Paint had just read `width`=0x0690 and
     `height`=0x03ce out of WIN.INI's [Paintbrush] section, four calls earlier in
     the same log. The ids run with the ordinals here (0x30 = 48) and the two
     direct exports either side agree -- 51 -> 0x33, 52 -> 0x34 -- so the
     numbering is continuous across native16 and wow32 entries alike.
   ⚠ Which is NOT a rule to lean on: `CreateDC` is ordinal 53 and id 0x99. Name
     each internal stub from its own call, never from arithmetic. */
#define WOWGDI_CREATEBITMAP     0x0030
#define CBM_ARG_BITS    0        /* const void FAR* -- NULL = uninitialised */
#define CBM_ARG_BPP     4
#define CBM_ARG_PLANES  6
#define CBM_ARG_HEIGHT  8
#define CBM_ARG_WIDTH  10

#define WOWGDI_CREATECOMPATBM   0x0033
#define CCB_ARG_HEIGHT  0
#define CCB_ARG_WIDTH   2
#define CCB_ARG_HDC     4

#define WOWGDI_CREATECOMPATDC   0x0034
#define CCD_ARG_HDC     0

#define WOWGDI_CREATESOLIDBRUSH 0x0042
#define CSB_ARG_COLOR   0

#define WOWGDI_GETSTOCKOBJECT   0x0057
#define GSO_ARG_INDEX   0

/* ── ★★★ 0x52 GetObject(hObject, nCount, lpObject) ──────────────────────────
     GDI ordinal 82, `native16`, so the id came from the run again:

       FUNC=0x00000052 args=0x08 retstub=0x062c (4ee4 09c7 | 0032 | 2060)

     Eight argument bytes reversed give lpObject at +0 (a far pointer into
     Paint's own data), nCount at +4 and the object at +6 -- and 0x2060 is one of
     OUR GDI tokens, which is the confirmation that the last field is the handle.

   ⚠⚠ EVERY ONE OF THESE STRUCTURES IS A DIFFERENT SIZE IN Win16, so this is a
     CONVERSION and not a copy -- the same trap OPENFILENAME was. Win16 keeps
     coordinates and dimensions in `int` (2 bytes) where Win32 uses `LONG`:
         BITMAP    14 bytes here, 24 on Win32
         LOGFONT   50            , 60
         LOGPEN    10            , 16
         LOGBRUSH   8            , 16
     ★ Paint's `nCount` of 0x32 = 50 is exactly Win16's LOGFONT (5 ints + 8 bytes
       + a 32-byte face name), which is what makes the reading self-checking
       rather than a size recalled from somewhere.
   ⚠ THE TYPE COMES FROM THE OBJECT, NOT FROM nCount. Dispatching on the byte
     count would be guessing at what the guest meant and would break the moment
     one asked for a partial structure -- which Win16 explicitly allows. The OS
     is asked what the handle actually is, the full Win16 form is built, and then
     `min(nCount, that)` bytes are handed over, which is what Windows does.
   ⚠ ONLY THE FONT CASE HAS BEEN SEEN IN A RUN. The other three are written from
     the same size arithmetic and are marked in the log as they go, so the first
     run that exercises one says so rather than passing silently. */
#define WOWGDI_GETOBJECT        0x0052
#define GOB_ARG_BUF     0
#define GOB_ARG_COUNT   4
#define GOB_ARG_HANDLE  6

#define WOW16_BITMAP_CB    14
#define WOW16_LOGFONT_CB   50
#define WOW16_LOGPEN_CB    10
#define WOW16_LOGBRUSH_CB   8

/* ── ★★★ 0x99 CreateDC, AND ITS ID IS NOT ITS ORDINAL ───────────────────────
     `CreateDC` is GDI ordinal 53, and its export is `native16` -- 16-bit code
     that reaches a WOW32 stub from inside its own body -- so `neneeds.py` cannot
     see the stub and the id had to come from a run. It did, and the call named
     itself completely:

       FUNC=0x00000099 stub=0x037f args=0x10 retstub=0x026a from=0x09df:0x13bb
         (0000 0000 | 0000 0000 | 0000 0000 | 0880 09c6)
         ★ arg[6] 0x09c6:0x0880 = "display"

     Sixteen argument bytes is four far pointers, which is exactly
     `CreateDC(lpszDriver, lpszDevice, lpszOutput, lpInitData)`, and reversed as
     always the driver -- pushed first -- is at +12. So this is
     `CreateDC("display", NULL, NULL, NULL)`: MS Paint asking for a screen DC to
     size its canvas against.
   ⚠ NOTE THE ID IS 0x99 AND THE ORDINAL IS 53. Wherever a wrapper reaches a stub
     from inside itself, the two numbering schemes part company, so an id here
     must never be inferred from an ordinal the way the direct exports' can be. */
#define WOWGDI_CREATEDC         0x0099
#define CDC_ARG_INITDATA 0
#define CDC_ARG_OUTPUT   4
#define CDC_ARG_DEVICE   8
#define CDC_ARG_DRIVER  12

/* GDI tokens sit below the menu tokens (0x4000) and above the window handles,
   so a stray handle of any kind is recognisable on sight in a log. */
#define WOWGDI_BASE      0x2000
#define WOWGDI_STEP      0x0008
#define WOWGDI_MAX       256

/* ── ★★ THREE KINDS, NOT TWO -- AND THE THIRD IS WHY THIS IS NOT A BOOLEAN. ──
     A handle's kind decides which call is allowed to dispose of it, and getting
     that wrong is not a loud failure, it is a leak or a corrupted DC cache:

       OBJ    a brush/pen/bitmap/font        -> DeleteObject
       DC     from CreateDC/CreateCompatibleDC -> DeleteDC
       WINDC  BORROWED from GetDC/GetWindowDC  -> ReleaseDC, and ONLY ReleaseDC

     The third kind arrived with `GetDC`, which is USER's call and not GDI's --
     so the first thing that ever hands this host a DC is in another id space
     entirely. `DeleteDC` on a borrowed DC is a documented bug (the DC belongs to
     the window's cache, not to the caller), and this map is the only place that
     can still tell the difference, so it records it rather than reconstructing
     it later from which call happens to arrive. */
#define WOWGDI_KIND_OBJ    0
#define WOWGDI_KIND_DC     1
#define WOWGDI_KIND_WINDC  2
/* A fourth: the system's own objects, which belong to nobody and must not be
   destroyed. Win32 tolerates DeleteObject on one silently; a guest doing it is
   still worth seeing, and it costs one value in this enum to be able to say so. */
#define WOWGDI_KIND_STOCK  3

typedef struct { WORD h; HGDIOBJ o; int kind; } wowgdi_obj_t;
static wowgdi_obj_t g_wg_obj[WOWGDI_MAX];
static int          g_wg_nobj = 0;

/* One token per object. `kind` is kept because DeleteDC, DeleteObject and
   ReleaseDC are three different calls with three different rules, and handing an
   object to the wrong one is a defect this map can catch instead of passing on
   to Win32. */
static WORD wowgdi_h16(HGDIOBJ o, int kind)
{
    int i;
    if (!o) return 0;
    for (i = 0; i < g_wg_nobj; ++i)
        if (g_wg_obj[i].o == o) return g_wg_obj[i].h;
    for (i = 0; i < g_wg_nobj; ++i)                    /* reuse a freed slot */
        if (!g_wg_obj[i].h && !g_wg_obj[i].o) break;
    if (i == g_wg_nobj) {
        if (g_wg_nobj >= WOWGDI_MAX) return 0;
        i = g_wg_nobj++;
    }
    g_wg_obj[i].o = o;
    g_wg_obj[i].kind = kind;
    g_wg_obj[i].h = (WORD)(WOWGDI_BASE + i * WOWGDI_STEP);
    return g_wg_obj[i].h;
}

static HGDIOBJ wowgdi_h32(WORD h, int *kind)
{
    int i;
    if (kind) *kind = -1;
    if (!h) return NULL;
    for (i = 0; i < g_wg_nobj; ++i)
        if (g_wg_obj[i].h == h) {
            if (kind) *kind = g_wg_obj[i].kind;
            return g_wg_obj[i].o;
        }
    return NULL;
}

/* Forget a token whose object has been destroyed. ⚠ The slot is cleared rather
   than compacted: a token is its INDEX, so compacting would silently rename
   every object above it.
 ⚠ A CLEARED SLOT IS REUSED, and that is a deliberate trade. Paint takes a DC and
   releases it on every single paint, so 256 slots without reuse are gone in
   seconds and the map would start refusing handles -- a certain failure traded
   for a possible one. The possible one is real and is named here rather than
   left to be discovered: a guest that keeps a token past its ReleaseDC will,
   after a reuse, name a DIFFERENT object instead of failing. That is exactly
   what Win32 does with its own handles, so a guest that does it is already
   broken on real Windows. */
static void wowgdi_forget(WORD h)
{
    int i;
    for (i = 0; i < g_wg_nobj; ++i)
        if (g_wg_obj[i].h == h) { g_wg_obj[i].o = NULL; g_wg_obj[i].h = 0; return; }
}

/*
 * ⚠ CALLED ONLY WHEN THE STUB IS GDI'S. The caller checks, as it does for every
 *   other table -- `0x45` is `SetWindowPos`-adjacent in USER's numbering and
 *   `DeleteObject` here.
 */
static int wowgdi_call(wow32_frame_t *f, char *note, int notecap)
{
    if (notecap) note[0] = 0;
    switch (f->id) {

    /* ── ★ 0x50 GetDeviceCaps(hDC, nIndex) ──────────────────────────────────
         Straight through to the OS on the same claim the WS_* bits and the SM_*
         indices travel on: Win32 inherited the DC capability indices from Win16
         unchanged. ⚠ As with GetSystemMetrics, a wrong answer here does not
         fail, it lays something out slightly wrong -- so the index AND the answer
         are logged on every call, and a guest whose arithmetic looks wrong can be
         checked against what it was actually told. */
    case WOWGDI_GETDEVICECAPS: {
        WORD hdc = wow32_argw(f, GDC_ARG_HDC);
        WORD idx = wow32_argw(f, GDC_ARG_INDEX);
        int  kind = -1;
        HGDIOBJ o = wowgdi_h32(hdc, &kind);
        int k = 0, v;
        int isdc = (kind == WOWGDI_KIND_DC || kind == WOWGDI_KIND_WINDC);
        wu_puts(note, notecap, &k, "GetDeviceCaps(0x");
        wu_puthex(note, notecap, &k, hdc, 4);
        wu_puts(note, notecap, &k, ", 0x");
        wu_puthex(note, notecap, &k, idx, 4);
        wu_puts(note, notecap, &k, ")");
        if (!o || !isdc) {
            wu_puts(note, notecap, &k, " -- ★ NOT ONE OF OUR GDI DC TOKENS (the"
                                       " calls that CREATE a DC are not serviced"
                                       " yet); answered 0");
            wow32_setret(f, 0);
            return 1;
        }
        v = GetDeviceCaps((HDC)o, (int)idx);
        /* ⚠ REFUTED, session 45 -- recorded so it is not re-tried. Win32
             answers NUMCOLORS with -1 for any device deeper than 8bpp, which
             reaches a Win16 program as 0xffff, and MS Paint asks for NUMCOLORS
             four times and never asks BITSPIXEL or PLANES -- so it looked
             certain that -1 was why every bitmap it makes is `planes=1 bpp=1`.
             Substituting 256 (the deepest a Win3.1 driver ever reported) changed
             NOTHING: the log shows `= 0x0100` and the very same 33 monochrome
             bitmaps. The 1bpp bitmaps are MASKS -- the Win16 idiom is a
             monochrome pattern plus SetTextColor/SetBkColor at blit time, and
             this run makes 34 of each -- so they are not evidence of a
             mis-detected display at all. The answer is left as the OS gives it. */
        /* ⚠ REFUTED, session 45, and recorded so it is not re-tried: forcing
             HORZRES/HORZSIZE to stock's 2.0 (from our 2.625) changed MS Paint's
             toolbox by NOTHING -- pbTool stayed 163x731 to the pixel. Paint
             reads all four of HORZRES/HORZSIZE/VERTRES/VERTSIZE, and the ratio
             matched the over-scale exactly, and it is still not the lever. */
        wu_puts(note, notecap, &k, " = 0x");
        wu_puthex(note, notecap, &k, (DWORD)v, 4);
        wow32_setret(f, (DWORD)(WORD)v);
        return 1;
    }

    /* ── ★★★★★ 0x22 BitBlt -- THE TOOL ICONS. ───────────────────────────────
       ⚠ BOTH DCs ARE TOKENS and both must resolve, which is the whole reason a
         blit could not work before the producers went in: the source is almost
         always a memory DC with the toolbox bitmap selected into it. */
    case WOWGDI_BITBLT: {
        WORD hdst = wow32_argw(f, BB_ARG_DSTDC);
        WORD hsrc = wow32_argw(f, BB_ARG_SRCDC);
        int  x  = (int)(short)wow32_argw(f, BB_ARG_X);
        int  y  = (int)(short)wow32_argw(f, BB_ARG_Y);
        int  cx = (int)(short)wow32_argw(f, BB_ARG_WIDTH);
        int  cy = (int)(short)wow32_argw(f, BB_ARG_HEIGHT);
        int  sx = (int)(short)wow32_argw(f, BB_ARG_SRCX);
        int  sy = (int)(short)wow32_argw(f, BB_ARG_SRCY);
        DWORD rop = wow32_argd(f, BB_ARG_ROP);
        int  dk = -1, sk = -1;
        HGDIOBJ d = wowgdi_h32(hdst, &dk);
        HGDIOBJ s = wowgdi_h32(hsrc, &sk);
        int  k = 0, ok;
        wu_puts(note, notecap, &k, "BitBlt dst 0x");
        wu_puthex(note, notecap, &k, hdst, 4);
        wu_puts(note, notecap, &k, " (");
        wu_puthex(note, notecap, &k, (DWORD)x, 4);
        wu_puts(note, notecap, &k, ",");
        wu_puthex(note, notecap, &k, (DWORD)y, 4);
        wu_puts(note, notecap, &k, ") ");
        wu_puthex(note, notecap, &k, (DWORD)cx, 4);
        wu_puts(note, notecap, &k, "x");
        wu_puthex(note, notecap, &k, (DWORD)cy, 4);
        wu_puts(note, notecap, &k, " <- src 0x");
        wu_puthex(note, notecap, &k, hsrc, 4);
        wu_puts(note, notecap, &k, " (");
        wu_puthex(note, notecap, &k, (DWORD)sx, 4);
        wu_puts(note, notecap, &k, ",");
        wu_puthex(note, notecap, &k, (DWORD)sy, 4);
        wu_puts(note, notecap, &k, ") rop=0x");
        wu_puthex(note, notecap, &k, rop, 8);
        if (!d || (dk != WOWGDI_KIND_DC && dk != WOWGDI_KIND_WINDC)) {
            wu_puts(note, notecap, &k, " -- ★ THE DESTINATION IS NOT ONE OF OUR"
                                       " DC TOKENS; answered 0");
            wow32_setret(f, 0);
            return 1;
        }
        /* ⚠ A NULL SOURCE IS LEGAL for the rops that do not read one (BLACKNESS,
             WHITENESS, PATCOPY...), so a zero handle is passed through as NULL
             rather than refused; a NON-zero handle that we cannot name is a
             different thing and is refused. */
        if (hsrc && (!s || (sk != WOWGDI_KIND_DC && sk != WOWGDI_KIND_WINDC))) {
            wu_puts(note, notecap, &k, " -- ★ THE SOURCE IS NOT ONE OF OUR DC"
                                       " TOKENS; answered 0");
            wow32_setret(f, 0);
            return 1;
        }
        ok = BitBlt((HDC)d, x, y, cx, cy, hsrc ? (HDC)s : NULL, sx, sy, rop)
             ? 1 : 0;
        wu_puts(note, notecap, &k, ok ? " -> blitted" : " -- ★ the OS refused it");
        wow32_setret(f, (DWORD)ok);
        return 1;
    }

    /* ── ★★★★★ 0x23 StretchBlt -- see the note above. ───────────────────────
       ⚠ THE STRETCH MODE IS THE DC'S, and Paint sets it (SetStretchBltMode,
         COLORONCOLOR) before getting here -- which is why that call had to go in
         with this one rather than after it. */
    case WOWGDI_STRETCHBLT: {
        WORD hdst = wow32_argw(f, SB_ARG_DSTDC);
        WORD hsrc = wow32_argw(f, SB_ARG_SRCDC);
        int  x  = (int)(short)wow32_argw(f, SB_ARG_DSTX);
        int  y  = (int)(short)wow32_argw(f, SB_ARG_DSTY);
        int  cx = (int)(short)wow32_argw(f, SB_ARG_DSTW);
        int  cy = (int)(short)wow32_argw(f, SB_ARG_DSTH);
        int  sx = (int)(short)wow32_argw(f, SB_ARG_SRCX);
        int  sy = (int)(short)wow32_argw(f, SB_ARG_SRCY);
        int  sw = (int)(short)wow32_argw(f, SB_ARG_SRCW);
        int  sh = (int)(short)wow32_argw(f, SB_ARG_SRCH);
        DWORD rop = wow32_argd(f, SB_ARG_ROP);
        int  dk = -1, sk = -1;
        HGDIOBJ d = wowgdi_h32(hdst, &dk);
        HGDIOBJ s = wowgdi_h32(hsrc, &sk);
        int  k = 0, ok;
        wu_puts(note, notecap, &k, "StretchBlt dst 0x");
        wu_puthex(note, notecap, &k, hdst, 4);
        wu_puts(note, notecap, &k, " (");
        wu_puthex(note, notecap, &k, (DWORD)x, 4);
        wu_puts(note, notecap, &k, ",");
        wu_puthex(note, notecap, &k, (DWORD)y, 4);
        wu_puts(note, notecap, &k, ") ");
        wu_puthex(note, notecap, &k, (DWORD)cx, 4);
        wu_puts(note, notecap, &k, "x");
        wu_puthex(note, notecap, &k, (DWORD)cy, 4);
        wu_puts(note, notecap, &k, " <- src 0x");
        wu_puthex(note, notecap, &k, hsrc, 4);
        wu_puts(note, notecap, &k, " ");
        wu_puthex(note, notecap, &k, (DWORD)sw, 4);
        wu_puts(note, notecap, &k, "x");
        wu_puthex(note, notecap, &k, (DWORD)sh, 4);
        wu_puts(note, notecap, &k, " rop=0x");
        wu_puthex(note, notecap, &k, rop, 8);
        if (!d || (dk != WOWGDI_KIND_DC && dk != WOWGDI_KIND_WINDC)) {
            wu_puts(note, notecap, &k, " -- ★ THE DESTINATION IS NOT ONE OF OUR"
                                       " DC TOKENS; answered 0");
            wow32_setret(f, 0);
            return 1;
        }
        if (hsrc && (!s || (sk != WOWGDI_KIND_DC && sk != WOWGDI_KIND_WINDC))) {
            wu_puts(note, notecap, &k, " -- ★ THE SOURCE IS NOT ONE OF OUR DC"
                                       " TOKENS; answered 0");
            wow32_setret(f, 0);
            return 1;
        }
        ok = StretchBlt((HDC)d, x, y, cx, cy, hsrc ? (HDC)s : NULL,
                        sx, sy, sw, sh, rop) ? 1 : 0;
        wu_puts(note, notecap, &k, ok ? " -> stretched"
                                      : " -- ★ the OS refused it");
        wow32_setret(f, (DWORD)ok);
        return 1;
    }

    /* ── ★★ 0x1b Rectangle(hDC, left, top, right, bottom) ───────────────────*/
    case WOWGDI_RECTANGLE: {
        WORD hdc = wow32_argw(f, RC_ARG_HDC);
        int  l = (int)(short)wow32_argw(f, RC_ARG_LEFT);
        int  t = (int)(short)wow32_argw(f, RC_ARG_TOP);
        int  r = (int)(short)wow32_argw(f, RC_ARG_RIGHT);
        int  b = (int)(short)wow32_argw(f, RC_ARG_BOTTOM);
        int  kind = -1;
        HGDIOBJ o = wowgdi_h32(hdc, &kind);
        int  k = 0;
        wu_puts(note, notecap, &k, "Rectangle(0x");
        wu_puthex(note, notecap, &k, hdc, 4);
        wu_puts(note, notecap, &k, ", ");
        wu_puthex(note, notecap, &k, (DWORD)l, 4);
        wu_puts(note, notecap, &k, ",");
        wu_puthex(note, notecap, &k, (DWORD)t, 4);
        wu_puts(note, notecap, &k, " ");
        wu_puthex(note, notecap, &k, (DWORD)r, 4);
        wu_puts(note, notecap, &k, ",");
        wu_puthex(note, notecap, &k, (DWORD)b, 4);
        wu_puts(note, notecap, &k, ")");
        if (!o || (kind != WOWGDI_KIND_DC && kind != WOWGDI_KIND_WINDC)) {
            wu_puts(note, notecap, &k, " -- ★ NOT ONE OF OUR DC TOKENS;"
                                       " answered 0");
            wow32_setret(f, 0);
            return 1;
        }
        wow32_setret(f, (DWORD)(Rectangle((HDC)o, l, t, r, b) ? 1 : 0));
        return 1;
    }

    /* ── ★ 0x01 SetBkColor / 0x09 SetTextColor -- a COLORREF is a COLORREF. ──
       ⚠ The PREVIOUS colour is the return value and it is a DWORD, so this is
         one of the calls where the 32-bit return really is 32 bits. */
    case WOWGDI_SETBKCOLOR:
    case WOWGDI_SETTEXTCOLOR: {
        int  istext = (f->id == WOWGDI_SETTEXTCOLOR);
        WORD hdc = wow32_argw(f, COL_ARG_HDC);
        DWORD col = wow32_argd(f, COL_ARG_COLOR);
        int  kind = -1;
        HGDIOBJ o = wowgdi_h32(hdc, &kind);
        int  k = 0;
        COLORREF prev;
        wu_puts(note, notecap, &k, istext ? "SetTextColor(0x" : "SetBkColor(0x");
        wu_puthex(note, notecap, &k, hdc, 4);
        wu_puts(note, notecap, &k, ", 0x");
        wu_puthex(note, notecap, &k, col, 8);
        wu_puts(note, notecap, &k, ")");
        if (!o || (kind != WOWGDI_KIND_DC && kind != WOWGDI_KIND_WINDC)) {
            wu_puts(note, notecap, &k, " -- ★ NOT ONE OF OUR DC TOKENS;"
                                       " answered 0");
            wow32_setret(f, 0);
            return 1;
        }
        prev = istext ? SetTextColor((HDC)o, (COLORREF)col)
                      : SetBkColor((HDC)o, (COLORREF)col);
        wow32_setret(f, (DWORD)prev);
        return 1;
    }

    /* ── ★ 0x04 SetROP2 / 0x07 SetStretchBltMode -- one int, same numbering. */
    case WOWGDI_SETROP2:
    case WOWGDI_SETSTRETCHMODE: {
        int  isrop = (f->id == WOWGDI_SETROP2);
        WORD hdc  = wow32_argw(f, MODE_ARG_HDC);
        int  mode = (int)(short)wow32_argw(f, MODE_ARG_MODE);
        int  kind = -1;
        HGDIOBJ o = wowgdi_h32(hdc, &kind);
        int  k = 0, prev;
        wu_puts(note, notecap, &k, isrop ? "SetROP2(0x" : "SetStretchBltMode(0x");
        wu_puthex(note, notecap, &k, hdc, 4);
        wu_puts(note, notecap, &k, ", ");
        wu_puthex(note, notecap, &k, (DWORD)mode, 4);
        wu_puts(note, notecap, &k, ")");
        if (!o || (kind != WOWGDI_KIND_DC && kind != WOWGDI_KIND_WINDC)) {
            wu_puts(note, notecap, &k, " -- ★ NOT ONE OF OUR DC TOKENS;"
                                       " answered 0");
            wow32_setret(f, 0);
            return 1;
        }
        prev = isrop ? SetROP2((HDC)o, mode) : SetStretchBltMode((HDC)o, mode);
        wow32_setret(f, (DWORD)(WORD)prev);
        return 1;
    }

    /* ── ★ 0x0b SetWindowOrg(hDC, x, y) ─────────────────────────────────────*/
    case WOWGDI_SETWINDOWORG: {
        WORD hdc = wow32_argw(f, ORG_ARG_HDC);
        int  x = (int)(short)wow32_argw(f, ORG_ARG_X);
        int  y = (int)(short)wow32_argw(f, ORG_ARG_Y);
        int  kind = -1;
        HGDIOBJ o = wowgdi_h32(hdc, &kind);
        int  k = 0;
        POINT prev;
        wu_puts(note, notecap, &k, "SetWindowOrg(0x");
        wu_puthex(note, notecap, &k, hdc, 4);
        wu_puts(note, notecap, &k, ", ");
        wu_puthex(note, notecap, &k, (DWORD)x, 4);
        wu_puts(note, notecap, &k, ",");
        wu_puthex(note, notecap, &k, (DWORD)y, 4);
        wu_puts(note, notecap, &k, ")");
        if (!o || (kind != WOWGDI_KIND_DC && kind != WOWGDI_KIND_WINDC)) {
            wu_puts(note, notecap, &k, " -- ★ NOT ONE OF OUR DC TOKENS;"
                                       " answered 0");
            wow32_setret(f, 0);
            return 1;
        }
        prev.x = prev.y = 0;
        SetWindowOrgEx((HDC)o, x, y, &prev);
        wow32_setret(f, ((DWORD)(WORD)(short)prev.y << 16)
                        | (DWORD)(WORD)(short)prev.x);
        return 1;
    }

    /* ── ★ 0x1e SaveDC / 0x27 RestoreDC -- the guest's own nesting level. ────
       ⚠ THE LEVEL IS THE GUEST'S NUMBER AND IT TRAVELS UNCHANGED. Win32 keeps
         its own stack per DC and uses the same convention (positive = absolute,
         negative = relative), and our DCs are the ones the guest saved on, so
         the level it hands back is meaningful without translation. */
    case WOWGDI_SAVEDC:
    case WOWGDI_RESTOREDC: {
        int  issave = (f->id == WOWGDI_SAVEDC);
        WORD hdc = issave ? wow32_argw(f, SDC_ARG_HDC)
                          : wow32_argw(f, RDC2_ARG_HDC);
        int  lvl = issave ? 0 : (int)(short)wow32_argw(f, RDC2_ARG_LEVEL);
        int  kind = -1;
        HGDIOBJ o = wowgdi_h32(hdc, &kind);
        int  k = 0, r;
        wu_puts(note, notecap, &k, issave ? "SaveDC(0x" : "RestoreDC(0x");
        wu_puthex(note, notecap, &k, hdc, 4);
        if (!issave) {
            wu_puts(note, notecap, &k, ", level ");
            wu_puthex(note, notecap, &k, (DWORD)lvl, 4);
        }
        wu_puts(note, notecap, &k, ")");
        if (!o || (kind != WOWGDI_KIND_DC && kind != WOWGDI_KIND_WINDC)) {
            wu_puts(note, notecap, &k, " -- ★ NOT ONE OF OUR DC TOKENS;"
                                       " answered 0");
            wow32_setret(f, 0);
            return 1;
        }
        r = issave ? SaveDC((HDC)o) : (RestoreDC((HDC)o, lvl) ? 1 : 0);
        wu_puts(note, notecap, &k, " -> ");
        wu_puthex(note, notecap, &k, (DWORD)r, 4);
        wow32_setret(f, (DWORD)(WORD)r);
        return 1;
    }

    /* ── ★★★★ 0x13 LineTo / 0x14 MoveTo -- see the note above. ──────────────*/
    case WOWGDI_LINETO:
    case WOWGDI_MOVETO: {
        int  isline = (f->id == WOWGDI_LINETO);
        WORD hdc = wow32_argw(f, XY_ARG_HDC);
        int  x = (int)(short)wow32_argw(f, XY_ARG_X);
        int  y = (int)(short)wow32_argw(f, XY_ARG_Y);
        int  kind = -1;
        HGDIOBJ o = wowgdi_h32(hdc, &kind);
        int  k = 0;
        POINT prev;
        wu_puts(note, notecap, &k, isline ? "LineTo(0x" : "MoveTo(0x");
        wu_puthex(note, notecap, &k, hdc, 4);
        wu_puts(note, notecap, &k, ", ");
        wu_puthex(note, notecap, &k, (DWORD)x, 4);
        wu_puts(note, notecap, &k, ",");
        wu_puthex(note, notecap, &k, (DWORD)y, 4);
        wu_puts(note, notecap, &k, ")");
        if (!o || (kind != WOWGDI_KIND_DC && kind != WOWGDI_KIND_WINDC)) {
            wu_puts(note, notecap, &k, " -- ★ NOT ONE OF OUR DC TOKENS;"
                                       " answered 0");
            wow32_setret(f, 0);
            return 1;
        }
        if (isline) {
            wow32_setret(f, (DWORD)(LineTo((HDC)o, x, y) ? 1 : 0));
            return 1;
        }
        prev.x = prev.y = 0;
        MoveToEx((HDC)o, x, y, &prev);
        /* ★ y in the HIGH word, x in the LOW -- Win16's MAKELONG order. */
        wow32_setret(f, ((DWORD)(WORD)(short)prev.y << 16)
                        | (DWORD)(WORD)(short)prev.x);
        return 1;
    }

    /* ── ★★★ 0x1d PatBlt(hDC, x, y, nWidth, nHeight, dwRop) ─────────────────
         The raster ops are the same numbers in both worlds (PATCOPY, PATINVERT,
         DSTINVERT, BLACKNESS, WHITENESS), so the rop travels unchanged. */
    case WOWGDI_PATBLT: {
        WORD  hdc = wow32_argw(f, PB_ARG_HDC);
        int   x = (int)(short)wow32_argw(f, PB_ARG_X);
        int   y = (int)(short)wow32_argw(f, PB_ARG_Y);
        int   cx = (int)(short)wow32_argw(f, PB_ARG_WIDTH);
        int   cy = (int)(short)wow32_argw(f, PB_ARG_HEIGHT);
        DWORD rop = wow32_argd(f, PB_ARG_ROP);
        int   kind = -1;
        HGDIOBJ o = wowgdi_h32(hdc, &kind);
        int   k = 0;
        wu_puts(note, notecap, &k, "PatBlt(0x");
        wu_puthex(note, notecap, &k, hdc, 4);
        wu_puts(note, notecap, &k, ", ");
        wu_puthex(note, notecap, &k, (DWORD)x, 4);
        wu_puts(note, notecap, &k, ",");
        wu_puthex(note, notecap, &k, (DWORD)y, 4);
        wu_puts(note, notecap, &k, " ");
        wu_puthex(note, notecap, &k, (DWORD)cx, 4);
        wu_puts(note, notecap, &k, "x");
        wu_puthex(note, notecap, &k, (DWORD)cy, 4);
        wu_puts(note, notecap, &k, " rop=0x");
        wu_puthex(note, notecap, &k, rop, 8);
        wu_puts(note, notecap, &k, ")");
        if (!o || (kind != WOWGDI_KIND_DC && kind != WOWGDI_KIND_WINDC)) {
            wu_puts(note, notecap, &k, " -- ★ NOT ONE OF OUR DC TOKENS;"
                                       " answered 0");
            wow32_setret(f, 0);
            return 1;
        }
        wow32_setret(f, (DWORD)(PatBlt((HDC)o, x, y, cx, cy, rop) ? 1 : 0));
        return 1;
    }

    /* ── ★★ 0x57 GetStockObject(nIndex) ─────────────────────────────────────
         Straight through, on the same claim the SM_* indices and the DC
         capability indices travel on: Win32 inherited the stock-object indices
         from Win16 unchanged (WHITE_BRUSH 0 .. SYSTEM_FIXED_FONT 16). Win16
         stopped at 16, so an index above that is a guest asking for something
         its own Windows never had, and it is refused rather than quietly given
         a Win32-only object.
       ⚠ THE TOKEN IS MINTED AS STOCK so that a later DeleteObject on it can say
         what it is instead of asking Win32 to destroy something the system owns.
       ★ The map de-duplicates by object, so the same stock object always comes
         back as the same token -- which matters, because guests compare them. */
    case WOWGDI_GETSTOCKOBJECT: {
        WORD idx = wow32_argw(f, GSO_ARG_INDEX);
        HGDIOBJ o;
        WORD tok;
        int k = 0;
        wu_puts(note, notecap, &k, "GetStockObject(0x");
        wu_puthex(note, notecap, &k, idx, 4);
        wu_puts(note, notecap, &k, ")");
        if (idx > 16) {
            wu_puts(note, notecap, &k, " -- ★ NOT A Win16 STOCK OBJECT (they stop"
                                       " at 16); answered 0");
            wow32_setret(f, 0);
            return 1;
        }
        o = GetStockObject((int)idx);
        /* ⚠ "NO SUCH OBJECT" AND "NO ROOM" ARE DIFFERENT FACTS, and the first
             cut of this printed the second for both -- so a run reported "THE
             GDI TOKEN MAP IS FULL" with nine of 256 slots used, because index 9
             is a hole in the stock-object numbering (it sits between NULL_PEN
             and OEM_FIXED_FONT) and the OS correctly returned NULL. A counter's
             message is a claim; this one was false. */
        if (!o) {
            wu_puts(note, notecap, &k, " -- ★ THE OS HAS NO SUCH STOCK OBJECT"
                                       " (index 9 is a hole in the numbering);"
                                       " answered 0");
            wow32_setret(f, 0);
            return 1;
        }
        tok = wowgdi_h16(o, WOWGDI_KIND_STOCK);
        if (!tok) {
            wu_puts(note, notecap, &k, " -- ★ THE GDI TOKEN MAP IS FULL;"
                                       " answered 0");
            wow32_setret(f, 0);
            return 1;
        }
        wu_puts(note, notecap, &k, " -> token 0x");
        wu_puthex(note, notecap, &k, tok, 4);
        wow32_setret(f, tok);
        return 1;
    }

    /* ── ★★★ 0x99 CreateDC(lpszDriver, lpszDevice, lpszOutput, lpInitData) ──
       ★ ONLY "DISPLAY" IS ANSWERED, and that is a decision rather than a
         shortcut. The screen is a device this host really does have, and Win32's
         own `CreateDCA("DISPLAY", NULL, NULL, NULL)` is the same request for the
         same thing. A printer driver name is not: it would name a Windows 3.1
         driver that does not exist on XP, and handing the string to Win32 would
         either fail obscurely or -- worse -- succeed against some unrelated
         printer the user did not ask to be drawn on. Refused, by name, in the
         log.
       ⚠ lpInitData IS IGNORED WHEN IT IS NULL, WHICH IS THE ONLY CASE SEEN. It
         carries a DEVMODE for printers; if one ever arrives here it will be
         reported rather than silently dropped, because a DEVMODE that is not
         honoured changes what gets drawn. */
    case WOWGDI_CREATEDC: {
        char drv[64], dev[64];
        HDC dc;
        WORD tok;
        int  k = 0, isdisp, i;
        DWORD init = wow32_argd(f, CDC_ARG_INITDATA);
        wow32_argstr(f, CDC_ARG_DRIVER, drv, sizeof drv);
        wow32_argstr(f, CDC_ARG_DEVICE, dev, sizeof dev);
        wu_puts(note, notecap, &k, "CreateDC driver=");
        wu_putq(note, notecap, &k, drv);
        if (dev[0]) { wu_puts(note, notecap, &k, " device="); wu_putq(note, notecap, &k, dev); }
        /* Case-insensitively "DISPLAY" -- the guest writes it lower case. */
        isdisp = 1;
        for (i = 0; i < 7; ++i) {
            char c = drv[i];
            if (c >= 'a' && c <= 'z') c = (char)(c - 32);
            if (c != "DISPLAY"[i]) { isdisp = 0; break; }
        }
        if (isdisp && drv[7]) isdisp = 0;
        if (!isdisp) {
            wu_puts(note, notecap, &k, " -- ★ NOT THE DISPLAY. That names a"
                                       " Windows 3.1 device driver which does not"
                                       " exist here, so it is refused rather than"
                                       " resolved against one of the host's own"
                                       " devices; answered 0");
            wow32_setret(f, 0);
            return 1;
        }
        if (init)
            wu_puts(note, notecap, &k, " -- ⚠ lpInitData IS NOT NULL and is being"
                                       " ignored (it carries a DEVMODE)");
        dc = CreateDCA("DISPLAY", NULL, NULL, NULL);
        tok = dc ? wowgdi_h16((HGDIOBJ)dc, WOWGDI_KIND_DC) : 0;
        if (!tok) {
            if (dc) DeleteDC(dc);
            wu_puts(note, notecap, &k, dc ? " -- ★ THE GDI TOKEN MAP IS FULL; the"
                                            " DC was deleted again and 0 answered"
                                          : " -- ★ THE OS REFUSED IT; answered 0");
            wow32_setret(f, 0);
            return 1;
        }
        wu_puts(note, notecap, &k, " -> screen DC token 0x");
        wu_puthex(note, notecap, &k, tok, 4);
        wow32_setret(f, tok);
        return 1;
    }

    /* ── ★★ 0x34 CreateCompatibleDC(hDC) ────────────────────────────────────
         The memory DC a paint program draws its canvas into. hDC == 0 is legal
         and means "compatible with the screen", so a null token is passed
         through as NULL rather than refused -- the same rule GetDC follows.
       ⚠ MINTED AS DC, not WINDC: this one is OWNED by the guest and goes back
         through DeleteDC. That distinction is the whole reason the map records a
         kind (see the header). */
    case WOWGDI_CREATECOMPATDC: {
        WORD src = wow32_argw(f, CCD_ARG_HDC);
        int  kind = -1;
        HGDIOBJ o = src ? wowgdi_h32(src, &kind) : NULL;
        HDC dc;
        WORD tok;
        int k = 0;
        wu_puts(note, notecap, &k, "CreateCompatibleDC(0x");
        wu_puthex(note, notecap, &k, src, 4);
        wu_puts(note, notecap, &k, ")");
        if (src && (!o || (kind != WOWGDI_KIND_DC && kind != WOWGDI_KIND_WINDC))) {
            wu_puts(note, notecap, &k, " -- ★ THAT IS NOT ONE OF OUR DC TOKENS;"
                                       " answered 0");
            wow32_setret(f, 0);
            return 1;
        }
        if (!src) wu_puts(note, notecap, &k, " (compatible with the SCREEN)");
        dc = CreateCompatibleDC(src ? (HDC)o : NULL);
        tok = dc ? wowgdi_h16((HGDIOBJ)dc, WOWGDI_KIND_DC) : 0;
        if (!tok) {
            if (dc) DeleteDC(dc);         /* never issue a DC we cannot name */
            wu_puts(note, notecap, &k, dc ? " -- ★ THE GDI TOKEN MAP IS FULL; the"
                                            " DC was deleted again and 0 answered"
                                          : " -- ★ THE OS REFUSED IT; answered 0");
            wow32_setret(f, 0);
            return 1;
        }
        wu_puts(note, notecap, &k, " -> DC token 0x");
        wu_puthex(note, notecap, &k, tok, 4);
        wow32_setret(f, tok);
        return 1;
    }

    /* ── ★★★ 0x30 CreateBitmap(nWidth, nHeight, cPlanes, cBitsPixel, lpvBits) ─
         Unlike CreateCompatibleBitmap this one names its own format, so it needs
         no DC at all.
       ⚠ lpvBits IS OPTIONAL AND IS BOUNDS-CHECKED WHEN PRESENT. Paint passes
         NULL (an uninitialised bitmap), but a non-null pointer means GDI will
         read `((width*planes*bpp + 15)/16)*2 * height` bytes out of a 16-bit
         segment -- so the arithmetic is done here, in 32 bits, and a bitmap
         whose bits would run past a 64K segment is refused rather than handed to
         GDI to read whatever follows.
       ⚠ THE DIMENSIONS ARE SIGNED 16-BIT, as in CreateCompatibleBitmap. */
    case WOWGDI_CREATEBITMAP: {
        int  w  = (int)(short)wow32_argw(f, CBM_ARG_WIDTH);
        int  h  = (int)(short)wow32_argw(f, CBM_ARG_HEIGHT);
        WORD pl = wow32_argw(f, CBM_ARG_PLANES);
        WORD bp = wow32_argw(f, CBM_ARG_BPP);
        volatile BYTE *bits = wow32_argptr(f, CBM_ARG_BITS);
        HBITMAP bm;
        WORD tok;
        int  k = 0;
        wu_puts(note, notecap, &k, "CreateBitmap ");
        wu_puthex(note, notecap, &k, (DWORD)w, 4);
        wu_puts(note, notecap, &k, "x");
        wu_puthex(note, notecap, &k, (DWORD)h, 4);
        wu_puts(note, notecap, &k, " planes=");
        wu_puthex(note, notecap, &k, pl, 2);
        wu_puts(note, notecap, &k, " bpp=");
        wu_puthex(note, notecap, &k, bp, 2);
        wu_puts(note, notecap, &k, bits ? " with bits" : " uninitialised");
        if (w <= 0 || h <= 0 || !pl || !bp) {
            wu_puts(note, notecap, &k, " -- ★ A DIMENSION OR FORMAT IS NOT"
                                       " POSITIVE; answered 0");
            wow32_setret(f, 0);
            return 1;
        }
        if (bits) {
            DWORD stride = ((((DWORD)w * pl * bp) + 15) / 16) * 2;
            if (stride * (DWORD)h > 0x10000ul) {
                wu_puts(note, notecap, &k, " -- ★ THE BITS WOULD RUN PAST A 64K"
                                           " SEGMENT; refused rather than letting"
                                           " GDI read past them; answered 0");
                wow32_setret(f, 0);
                return 1;
            }
        }
        bm = CreateBitmap(w, h, (UINT)pl, (UINT)bp,
                          bits ? (const void *)(const BYTE *)bits : NULL);
        tok = bm ? wowgdi_h16((HGDIOBJ)bm, WOWGDI_KIND_OBJ) : 0;
        if (!tok) {
            if (bm) DeleteObject((HGDIOBJ)bm);
            wu_puts(note, notecap, &k, bm ? " -- ★ THE GDI TOKEN MAP IS FULL; the"
                                            " bitmap was freed and 0 answered"
                                          : " -- ★ THE OS REFUSED IT; answered 0");
            wow32_setret(f, 0);
            return 1;
        }
        wu_puts(note, notecap, &k, " -> bitmap token 0x");
        wu_puthex(note, notecap, &k, tok, 4);
        wow32_setret(f, tok);
        return 1;
    }

    /* ── ★★★ 0x33 CreateCompatibleBitmap(hDC, nWidth, nHeight) ──────────────
         MS Paint's canvas itself, and the allocation whose failure it reports as
         "Not enough memory to edit image".
       ⚠ THE DIMENSIONS ARE SIGNED 16-BIT. A Win16 program passes ints, and a
         negative or zero one must not be widened into a huge Win32 request --
         that would either fail obscurely or succeed at a size nobody asked for.
       ⚠ AND IT IS COMPATIBLE WITH THE hDC IT IS GIVEN, which is why passing NULL
         here is NOT the harmless default it is for CreateCompatibleDC: a bitmap
         compatible with nothing is a 1x1 monochrome one, and a guest that drew
         into it would get a black-and-white canvas and no explanation. So a null
         or unknown DC is refused. */
    case WOWGDI_CREATECOMPATBM: {
        WORD hdc = wow32_argw(f, CCB_ARG_HDC);
        int  w   = (int)(short)wow32_argw(f, CCB_ARG_WIDTH);
        int  h   = (int)(short)wow32_argw(f, CCB_ARG_HEIGHT);
        int  kind = -1;
        HGDIOBJ o = wowgdi_h32(hdc, &kind);
        HBITMAP bm;
        WORD tok;
        int k = 0;
        wu_puts(note, notecap, &k, "CreateCompatibleBitmap(0x");
        wu_puthex(note, notecap, &k, hdc, 4);
        wu_puts(note, notecap, &k, ", ");
        wu_puthex(note, notecap, &k, (DWORD)w, 4);
        wu_puts(note, notecap, &k, "x");
        wu_puthex(note, notecap, &k, (DWORD)h, 4);
        wu_puts(note, notecap, &k, ")");
        if (!o || (kind != WOWGDI_KIND_DC && kind != WOWGDI_KIND_WINDC)) {
            wu_puts(note, notecap, &k, " -- ★ THAT IS NOT ONE OF OUR DC TOKENS;"
                                       " answered 0");
            wow32_setret(f, 0);
            return 1;
        }
        if (w <= 0 || h <= 0) {
            wu_puts(note, notecap, &k, " -- ★ A DIMENSION IS NOT POSITIVE;"
                                       " answered 0 rather than widening it into"
                                       " a size nobody asked for");
            wow32_setret(f, 0);
            return 1;
        }
        bm = CreateCompatibleBitmap((HDC)o, w, h);
        tok = bm ? wowgdi_h16((HGDIOBJ)bm, WOWGDI_KIND_OBJ) : 0;
        if (!tok) {
            if (bm) DeleteObject((HGDIOBJ)bm);
            wu_puts(note, notecap, &k, bm ? " -- ★ THE GDI TOKEN MAP IS FULL; the"
                                            " bitmap was freed and 0 answered"
                                          : " -- ★ THE OS REFUSED IT; answered 0");
            wow32_setret(f, 0);
            return 1;
        }
        wu_puts(note, notecap, &k, " -> bitmap token 0x");
        wu_puthex(note, notecap, &k, tok, 4);
        wow32_setret(f, tok);
        return 1;
    }

    /* ── ★ 0x42 CreateSolidBrush(crColor) ───────────────────────────────────
         A COLORREF is a DWORD and means the same thing in both worlds. */
    case WOWGDI_CREATESOLIDBRUSH: {
        DWORD col = wow32_argd(f, CSB_ARG_COLOR);
        HBRUSH br = CreateSolidBrush((COLORREF)col);
        WORD tok = br ? wowgdi_h16((HGDIOBJ)br, WOWGDI_KIND_OBJ) : 0;
        int k = 0;
        wu_puts(note, notecap, &k, "CreateSolidBrush(0x");
        wu_puthex(note, notecap, &k, col, 8);
        wu_puts(note, notecap, &k, ")");
        if (!tok) {
            if (br) DeleteObject((HGDIOBJ)br);
            wu_puts(note, notecap, &k, " -- ★ refused or the token map is full;"
                                       " answered 0");
            wow32_setret(f, 0);
            return 1;
        }
        wu_puts(note, notecap, &k, " -> brush token 0x");
        wu_puthex(note, notecap, &k, tok, 4);
        wow32_setret(f, tok);
        return 1;
    }

    /* ── ★★★ 0x2d SelectObject(hDC, hObject) ────────────────────────────────
         The call that puts the canvas bitmap into the memory DC, and the reason
         the two above are worth anything.
       ⚠ IT RETURNS THE OBJECT THAT WAS THERE BEFORE, and that has to come back
         as a token too -- guests keep it and select it back before deleting the
         DC, which is the documented way to avoid destroying a bitmap that is
         still selected. A truncated HGDIOBJ here would be handed straight back
         to us later and would name nothing.
       ★ The previous object is usually one we never issued (the 1x1 bitmap a new
         memory DC starts with), so it is minted on the spot as an ordinary
         object. That is correct: from the guest's side it is simply a handle to
         give back, and the map now knows it if it returns. */
    case WOWGDI_SELECTOBJECT: {
        WORD hdc = wow32_argw(f, SEL_ARG_HDC);
        WORD hob = wow32_argw(f, SEL_ARG_OBJ);
        int  dkind = -1, okind = -1;
        HGDIOBJ d = wowgdi_h32(hdc, &dkind);
        HGDIOBJ o = wowgdi_h32(hob, &okind);
        HGDIOBJ prev;
        WORD tok;
        int k = 0;
        wu_puts(note, notecap, &k, "SelectObject(dc 0x");
        wu_puthex(note, notecap, &k, hdc, 4);
        wu_puts(note, notecap, &k, ", obj 0x");
        wu_puthex(note, notecap, &k, hob, 4);
        wu_puts(note, notecap, &k, ")");
        if (!d || (dkind != WOWGDI_KIND_DC && dkind != WOWGDI_KIND_WINDC)) {
            wu_puts(note, notecap, &k, " -- ★ THE DC IS NOT ONE OF OUR TOKENS;"
                                       " answered 0");
            wow32_setret(f, 0);
            return 1;
        }
        if (!o) {
            wu_puts(note, notecap, &k, " -- ★ THE OBJECT IS NOT ONE OF OUR TOKENS;"
                                       " answered 0");
            wow32_setret(f, 0);
            return 1;
        }
        if (okind == WOWGDI_KIND_DC || okind == WOWGDI_KIND_WINDC) {
            wu_puts(note, notecap, &k, " -- ★ THAT IS A DC, NOT A DRAWING OBJECT;"
                                       " refused");
            wow32_setret(f, 0);
            return 1;
        }
        prev = SelectObject((HDC)d, o);
        if (!prev) {
            wu_puts(note, notecap, &k, " -- ★ THE OS REFUSED THE SELECTION;"
                                       " answered 0");
            wow32_setret(f, 0);
            return 1;
        }
        tok = wowgdi_h16(prev, WOWGDI_KIND_OBJ);
        wu_puts(note, notecap, &k, " -> previous 0x");
        wu_puthex(note, notecap, &k, tok, 4);
        if (!tok)
            wu_puts(note, notecap, &k, " -- ⚠ THE MAP IS FULL, so the guest cannot"
                                       " select it back");
        wow32_setret(f, tok);
        return 1;
    }

    /* ── ★★★ 0x52 GetObject -- see the long note above. ─────────────────────*/
    case WOWGDI_GETOBJECT: {
        WORD h    = wow32_argw(f, GOB_ARG_HANDLE);
        WORD want = wow32_argw(f, GOB_ARG_COUNT);
        volatile BYTE *dst = wow32_argptr(f, GOB_ARG_BUF);
        int kind = -1;
        HGDIOBJ o = wowgdi_h32(h, &kind);
        BYTE b[WOW16_LOGFONT_CB];
        DWORD type;
        int n = 0, k = 0, i;
        const char *what = "?";

        wu_puts(note, notecap, &k, "GetObject 0x");
        wu_puthex(note, notecap, &k, h, 4);
        wu_puts(note, notecap, &k, " cb=0x");
        wu_puthex(note, notecap, &k, want, 4);
        if (!o) {
            wu_puts(note, notecap, &k, " -- ★ NOT ONE OF OUR GDI TOKENS;"
                                       " answered 0");
            wow32_setret(f, 0);
            return 1;
        }
        for (i = 0; i < (int)sizeof b; ++i) b[i] = 0;
        type = GetObjectType(o);
        if (type == OBJ_BITMAP) {
            BITMAP bm;
            if (!GetObject(o, (int)sizeof bm, &bm)) type = 0;
            else {
                what = "BITMAP";
                n = WOW16_BITMAP_CB;
                wow32_pokew(b + 0,  (WORD)(short)bm.bmType);
                wow32_pokew(b + 2,  (WORD)(short)bm.bmWidth);
                wow32_pokew(b + 4,  (WORD)(short)bm.bmHeight);
                wow32_pokew(b + 6,  (WORD)(short)bm.bmWidthBytes);
                b[8]  = (BYTE)bm.bmPlanes;
                /* ⚠ REFUTED, session 45. We report bmBitsPixel as the OS
                     gives it -- 0x20 on this rig -- and 32bpp is a depth Win16
                     never had (it knew 1/4/8/16/24), and this is the ONLY
                     pixel-format number MS Paint can see. Reporting 24 instead
                     changed NOTHING: still 33 bitmaps at `planes=1 bpp=1`. So
                     Paint's monochrome off-screen bitmaps do not come from here
                     either. The answer is left as the OS gives it. */
                b[9]  = (BYTE)bm.bmBitsPixel;
                /* ★ THE FORMAT, NOT JUST THE TYPE. A guest can only learn a
                     pixel format from here (MS Paint never asks GetDeviceCaps
                     for BITSPIXEL or PLANES), so these two numbers are the only
                     thing that can tell it whether to make a colour or a
                     monochrome off-screen bitmap -- which is exactly the open
                     question about its colour palette. */
                {   wu_puts(note, notecap, &k, " ");
                    wu_puthex(note, notecap, &k, (DWORD)(WORD)bm.bmWidth, 4);
                    wu_puts(note, notecap, &k, "x");
                    wu_puthex(note, notecap, &k, (DWORD)(WORD)bm.bmHeight, 4);
                    wu_puts(note, notecap, &k, " planes=");
                    wu_puthex(note, notecap, &k, (DWORD)bm.bmPlanes, 2);
                    wu_puts(note, notecap, &k, " bpp=");
                    wu_puthex(note, notecap, &k, (DWORD)bm.bmBitsPixel, 2);
                }
                /* ⚠ bmBits STAYS NULL, and that is correct rather than lazy: it
                     is a 32-bit host pointer with no 16-bit address, and Windows
                     itself returns NULL here for a device-dependent bitmap. */
            }
        } else if (type == OBJ_FONT) {
            LOGFONTA lf;
            if (!GetObjectA(o, (int)sizeof lf, &lf)) type = 0;
            else {
                what = "LOGFONT";
                n = WOW16_LOGFONT_CB;
                wow32_pokew(b + 0,  (WORD)(short)lf.lfHeight);
                wow32_pokew(b + 2,  (WORD)(short)lf.lfWidth);
                wow32_pokew(b + 4,  (WORD)(short)lf.lfEscapement);
                wow32_pokew(b + 6,  (WORD)(short)lf.lfOrientation);
                wow32_pokew(b + 8,  (WORD)(short)lf.lfWeight);
                b[10] = lf.lfItalic;        b[11] = lf.lfUnderline;
                b[12] = lf.lfStrikeOut;     b[13] = lf.lfCharSet;
                b[14] = lf.lfOutPrecision;  b[15] = lf.lfClipPrecision;
                b[16] = lf.lfQuality;       b[17] = lf.lfPitchAndFamily;
                for (i = 0; i < 32 && lf.lfFaceName[i]; ++i)
                    b[18 + i] = (BYTE)lf.lfFaceName[i];
                /* ★ THE FIELDS, NOT JUST THE TYPE. MS Paint sizes its whole
                     toolbox from the system font's metrics, so lfHeight is
                     load-bearing geometry and a log that only says "LOGFONT"
                     cannot be compared against an oracle. */
                {   int j = 0;
                    for (j = 0; j < 32 && lf.lfFaceName[j]; ++j) { }
                    wu_puts(note, notecap, &k, " h=");
                    wu_puthex(note, notecap, &k, (DWORD)(WORD)(short)lf.lfHeight, 4);
                    wu_puts(note, notecap, &k, " w=");
                    wu_puthex(note, notecap, &k, (DWORD)(WORD)(short)lf.lfWidth, 4);
                    wu_puts(note, notecap, &k, " wt=");
                    wu_puthex(note, notecap, &k, (DWORD)(WORD)(short)lf.lfWeight, 4);
                    wu_puts(note, notecap, &k, " ");
                    wu_putq(note, notecap, &k, lf.lfFaceName);
                }
            }
        } else if (type == OBJ_PEN || type == OBJ_EXTPEN) {
            LOGPEN lp;
            if (!GetObject(o, (int)sizeof lp, &lp)) type = 0;
            else {
                what = "LOGPEN";
                n = WOW16_LOGPEN_CB;
                wow32_pokew(b + 0, (WORD)lp.lopnStyle);
                wow32_pokew(b + 2, (WORD)(short)lp.lopnWidth.x);
                wow32_pokew(b + 4, (WORD)(short)lp.lopnWidth.y);
                wow32_pokew(b + 6, (WORD)(lp.lopnColor & 0xFFFF));
                wow32_pokew(b + 8, (WORD)(lp.lopnColor >> 16));
            }
        } else if (type == OBJ_BRUSH) {
            LOGBRUSH lb;
            if (!GetObject(o, (int)sizeof lb, &lb)) type = 0;
            else {
                what = "LOGBRUSH";
                n = WOW16_LOGBRUSH_CB;
                wow32_pokew(b + 0, (WORD)lb.lbStyle);
                wow32_pokew(b + 2, (WORD)(lb.lbColor & 0xFFFF));
                wow32_pokew(b + 4, (WORD)(lb.lbColor >> 16));
                wow32_pokew(b + 6, (WORD)lb.lbHatch);
            }
        } else {
            type = 0;
        }
        if (!type || !n) {
            wu_puts(note, notecap, &k, " -- ★ THE OS WILL NOT DESCRIBE THAT OBJECT"
                                       " (not a bitmap, font, pen or brush);"
                                       " answered 0");
            wow32_setret(f, 0);
            return 1;
        }
        wu_puts(note, notecap, &k, " -> ");
        wu_puts(note, notecap, &k, what);
        wu_puts(note, notecap, &k, " (Win16 form is 0x");
        wu_puthex(note, notecap, &k, (DWORD)n, 2);
        wu_puts(note, notecap, &k, " bytes)");
        /* ★ A NULL buffer is a legal QUERY: Win16 answers the size needed. */
        if (!dst) {
            wu_puts(note, notecap, &k, " -- lpObject is NULL, so this is a size"
                                       " query");
            wow32_setret(f, (DWORD)n);
            return 1;
        }
        if ((int)want < n) {
            n = (int)want;                 /* a partial read is allowed */
            wu_puts(note, notecap, &k, " -- PARTIAL, the guest asked for less");
        }
        for (i = 0; i < n; ++i) dst[i] = b[i];
        wow32_setret(f, (DWORD)n);
        return 1;
    }

    /* ── ★ 0x44 DeleteDC / 0x45 DeleteObject ────────────────────────────────
       ⚠ THE TOKEN IS FORGOTTEN AS WELL AS THE OBJECT DELETED. A Win32 handle is
         reusable once freed, so a token left pointing at a dead HGDIOBJ would
         eventually name somebody else's object -- the same hazard DestroyWindow
         has, and the same answer.
       ⚠ AND THE TWO ARE NOT INTERCHANGEABLE: DeleteObject on a DC leaks it and
         DeleteDC on a brush fails, so the map records which it issued and this
         says so rather than passing the mistake to Win32. */
    case WOWGDI_DELETEDC:
    case WOWGDI_DELETEOBJECT: {
        int  wantdc = (f->id == WOWGDI_DELETEDC);
        WORD h = wow32_argw(f, DOBJ_ARG_HANDLE);
        int  kind = -1;
        HGDIOBJ o = wowgdi_h32(h, &kind);
        int k = 0, ok;
        int want = wantdc ? WOWGDI_KIND_DC : WOWGDI_KIND_OBJ;
        wu_puts(note, notecap, &k, wantdc ? "DeleteDC 0x" : "DeleteObject 0x");
        wu_puthex(note, notecap, &k, h, 4);
        if (!o) {
            wu_puts(note, notecap, &k, " -- ★ NOT ONE OF OUR GDI TOKENS;"
                                       " answered 0");
            wow32_setret(f, 0);
            return 1;
        }
        /* ⚠ A STOCK OBJECT IS NOT DESTROYED, AND THE CALL STILL SUCCEEDS. The
             system owns it; Windows makes DeleteObject on one a no-op that
             reports success, and answering anything else would make a guest
             think its cleanup had failed. The token is KEPT, because the object
             is still there and the guest may well ask for it again. */
        if (kind == WOWGDI_KIND_STOCK) {
            wu_puts(note, notecap, &k, " -- ★ that is a STOCK object; the system"
                                       " owns it, so nothing was destroyed and"
                                       " success answered (as Windows does)");
            wow32_setret(f, 1);
            return 1;
        }
        /* ⚠ A BORROWED DC IS THE INTERESTING REFUSAL. DeleteDC on a DC that came
             from GetDC does not fail visibly on Win32 -- it damages the window's
             DC cache and the symptom surfaces somewhere else entirely. Say which
             call it should have been. */
        if (kind != want) {
            wu_puts(note, notecap, &k,
                    kind == WOWGDI_KIND_WINDC
                        ? " -- ★ THAT DC WAS BORROWED FROM A WINDOW (GetDC);"
                          " it must go back through ReleaseDC, not this. Refused"
                    : kind == WOWGDI_KIND_DC
                        ? " -- ★ THAT IS A DC AND THIS IS DeleteObject; refused"
                        : " -- ★ THAT IS NOT A DC AND THIS IS DeleteDC; refused");
            wow32_setret(f, 0);
            return 1;
        }
        ok = wantdc ? (DeleteDC((HDC)o) ? 1 : 0) : (DeleteObject(o) ? 1 : 0);
        if (ok) wowgdi_forget(h);
        wu_puts(note, notecap, &k, ok ? " -> deleted, token released"
                                      : " -- ★ the OS refused the delete");
        wow32_setret(f, (DWORD)ok);
        return 1;
    }

    default:
        return 0;
    }
}

#endif /* WOWGDI_H */
