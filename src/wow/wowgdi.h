#ifndef WOWGDI_H
#define WOWGDI_H
#include "wowconv.h"   /* the Win16/Win32 semantic deltas, pinned by tools/dostest/wow_test.c */
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
/* The capability index Win32 inherited from Win16 unchanged -- "how many entries
   in this device's colour table". It is the one index whose Win32 answer a Win16
   caller cannot read; see the handler. */
#define WOWGDI_CAP_NUMCOLORS  24
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

/* ── ★★★★ THE STROKE LOOP. ──────────────────────────────────────────────────
     Named from the run in which the mouse first reached MS Paint: a single drag
     across the canvas stepped over these 21 times each.
       0x63 LPtoDP                 8 args  (HDC, LPPOINT, int)  -- internal stub,
            named by its own call: `(0001 | 6df6 09c7 | 20c0)` is count 1, a far
            LPPOINT, and one of OUR DC tokens, which is what pins the order.
       0x9c CreateDiscardableBitmap ord 156, 6 args (HDC, int, int)
       0x94 SetBrushOrg            ord 148, 6 args (HDC, int, int)
   ⚠ A Win16 POINT is two `int`s = 4 bytes against Win32's 8, so LPtoDP is a
     conversion in BOTH directions -- it transforms in place, so every point has
     to be read out, converted, and written back narrowed. */
/* ★ 0x67 PtVisible(hDC, x, y) -- ord 103, 6 args, and the brush loop asks it
     eight times per stroke: "is this point inside the clip region?". Answered 0
     it means "no", so a guest politely declines to draw. */
/* ★ Three one-argument calls the brush loop makes, all direct exports:
     ord  79 GETDCORG        id 0x4f  (HDC)  -> the DC's origin, packed y:x
     ord 149 GETBRUSHORG     id 0x95  (HDC)  -> the brush origin, packed y:x
     ord 150 UNREALIZEOBJECT id 0x96  (HGDIOBJ) -> reset a brush's origin
   ⚠ UnrealizeObject takes an OBJECT, not a DC -- it is how a guest tells GDI to
     re-align a pattern brush before the next fill, which is exactly what a paint
     program does between strokes. */
#define WOWGDI_GETDCORG         0x004f
#define WOWGDI_GETBRUSHORG      0x0095
#define WOWGDI_UNREALIZEOBJ     0x0096
#define ONE_ARG_HANDLE  0

#define WOWGDI_PTVISIBLE        0x0067
#define PV_ARG_Y        0
#define PV_ARG_X        2
#define PV_ARG_HDC      4

/* ★ 0x24 Polygon(hDC, lpPoints, nCount) -- GDI ordinal 36, native16, so the id
     came from the run: `(0006 | 38fa 09c6 | 20d8)` is six points, a far LPPOINT
     and one of our DC tokens -- the same block shape as LPtoDP, which is what
     8 argument bytes buys you. ord 35 STRETCHBLT -> 0x23 either side confirms
     the numbering. ⚠ Polyline is ordinal 37 and would be 0x25 by the same
     reading; no run has produced one, so it is not written on that basis. */
#define WOWGDI_POLYGON          0x0024

#define WOWGDI_LPTODP           0x0063
#define LDP_ARG_COUNT   0
#define LDP_ARG_POINTS  2
#define LDP_ARG_HDC     6

#define WOWGDI_CREATEDISCARDBM  0x009c
#define WOWGDI_SETBRUSHORG      0x0094

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

/* ── ★★★ 0x99 -- AND IT IS `CreateIC`, NOT `CreateDC`. ──────────────────────
   ⚠⚠⚠ **THIS BLOCK'S ORIGINAL CONCLUSION WAS WRONG AND IS CORRECTED IN PLACE**
     (session 47). The call below is real and every argument reading of it holds;
     what was wrong was the NAME. `0x99` is `CreateIC`, **GDI ordinal 153**, and
     153 IS 0x99 -- the id tracked the ordinal all along. `CreateDC` is ordinal 53
     and its id is `0x35`, which is now answered next to it.
     The mistake survived because it is invisible: an information context and a
     device context answer every query identically, so servicing an IC as a DC
     works perfectly and only a name in a log was wrong. It was found by teaching
     `neneeds.py` to see through GDI's export wrappers, which resolves BOTH
     ordinals from the binary and puts 53 -> 0x35 and 153 -> 0x99 side by side.
   ⇒ **A wrong name is not harmless: the paragraph below drew a general rule
     ("the id is not the ordinal") from a case where it was not true.**

     The original reading, which stands except for the name:

       FUNC=0x00000099 stub=0x037f args=0x10 retstub=0x026a from=0x09df:0x13bb
         (0000 0000 | 0000 0000 | 0000 0000 | 0880 09c6)
         ★ arg[6] 0x09c6:0x0880 = "display"

     Sixteen argument bytes is four far pointers, which is exactly
     `CreateDC(lpszDriver, lpszDevice, lpszOutput, lpInitData)`, and reversed as
     always the driver -- pushed first -- is at +12. So this is
     `CreateDC("display", NULL, NULL, NULL)`: MS Paint asking for a screen DC to
     size its canvas against.
   ★ AND THE ARGUMENT BLOCK IS SHARED, which is why one case answers both: an IC
     and a DC take the same four far pointers in the same order. */
#define WOWGDI_CREATEDC         0x0099   /* ← ord 153 CreateIC (see above)        */
#define CDC_ARG_INITDATA 0
#define CDC_ARG_OUTPUT   4
#define CDC_ARG_DEVICE   8
#define CDC_ARG_DRIVER  12

/* ── ★★★★★ THE TOOLS THAT DID NOT WORK -- READ OUT OF GDI.EXE, NOT GUESSED. ──
     "Some drawing functions work, others (like fill) do not" is not a mystery
     once you enumerate what PBRUSH.EXE imports and resolve each ordinal through
     GDI.EXE's own entry table to the WOW32 stub it lands on. The pattern for a
     tail-jumped export is fixed --

       ELLIPSE (ord 24) = seg1:0x1b15   push 0x1b20 / pop dx / pop bp / jmp 0x040a
       0x040a                           push 0xa / push 0 / push 0x18   <- THE ID

     -- so the id and the argument byte count come out of the binary together,
     and `neneeds.py`'s independent `retstub 0x0417` for the same ordinal lands
     on the instruction after that stub's own `lcall`, i.e. on the SAME stub by
     a different route. Every id below was read that way.

   ★★★ THE FILL IS `ExtFloodFill`, GDI ordinal 372, and its export is NOT a bare
     tail-jump: `seg1:0x1b77` validates `[bp+6] <= 1` (the fill TYPE) and only
     then jumps to `0x048c`, which pushes 12 bytes and **id 0x174**. That extra
     hop is why a scan for tail-jumps calls it `native16` and reports it "free":
     it is not free, it is one instruction further away. Paint's fill tool is
     `seg4:0x1560..0x16e0` and it calls ExtFloodFill TWICE -- once for a solid
     colour and once after `CreatePatternBrush` -- which is why the pattern
     brush is in this batch and not a later one.

   ⚠ A Win16 fill type is Win32's fill type (0 = FLOODFILLBORDER, 1 = SURFACE),
     the same claim the ROPs and COLORREFs already travel on. */
#define WOWGDI_ELLIPSE          0x0018   /* ord 24, 10 args -- RC_ARG_* layout */
#define WOWGDI_EXCLUDECLIPRECT  0x0015   /* ord 21, 10 args -- RC_ARG_* layout */

#define WOWGDI_ROUNDRECT        0x001c   /* ord 28, 14 args                    */
#define RR_ARG_EH       0
#define RR_ARG_EW       2
#define RR_ARG_BOTTOM   4
#define RR_ARG_RIGHT    6
#define RR_ARG_TOP      8
#define RR_ARG_LEFT    10
#define RR_ARG_HDC     12

#define WOWGDI_EXTFLOODFILL     0x0174   /* ord 372, 12 args -- ★ THE FILL     */
#define FF_ARG_TYPE     0
#define FF_ARG_COLOR    2                /* DWORD                              */
#define FF_ARG_Y        6
#define FF_ARG_X        8
#define FF_ARG_HDC     10

#define WOWGDI_CREATEPATTERNBRUSH 0x003c /* ord 60,  2 args (HBITMAP)          */
#define WOWGDI_GETPIXEL         0x0053   /* ord 83,  6 args -- XY_ARG_* layout */
#define WOWGDI_GETBKCOLOR       0x004b   /* ord 75,  2 args -- PBRUSH.DLL's    */
#define WOWGDI_GETROP2          0x0055   /* ord 85,  2 args                    */
#define WOWGDI_UPDATECOLORS     0x016e   /* ord 366, 2 args                    */

#define WOWGDI_CREATERECTRGN    0x0040   /* ord 64,  8 args                    */
#define RGN_ARG_BOTTOM  0
#define RGN_ARG_RIGHT   2
#define RGN_ARG_TOP     4
#define RGN_ARG_LEFT    6

#define WOWGDI_SELECTCLIPRGN    0x002c   /* ord 44,  4 args                    */
#define SCR_ARG_RGN     0
#define SCR_ARG_HDC     2

/* The three mapping-mode setters. All 6 args, all the same (hDC, x, y) block as
   SetWindowOrg, and all returning the PREVIOUS pair packed y:x in a DWORD. */
#define WOWGDI_SETWINDOWEXT     0x000c   /* ord 12 */
#define WOWGDI_SETVIEWPORTORG   0x000d   /* ord 13 */
#define WOWGDI_SETVIEWPORTEXT   0x000e   /* ord 14 */
#define WOWGDI_SETBITMAPDIM     0x00a3   /* ord 163 -- same block, but a BITMAP */

#define WOWGDI_GETNEARESTCOLOR  0x009a   /* ord 154, 6 args -- COL_ARG_* layout */
#define WOWGDI_GETNEARESTPALIDX 0x0172   /* ord 370, 6 args -- HPALETTE + COLORREF */

/* ── ★★★★★ AND THIS IS WHY THE BOX AND THE ELLIPSE DREW NOTHING. ────────────
     The tools were selected, the rubber band tracked the drag in `R2_XORPEN`
     and the guest then set `R2_COPYPEN` to commit -- and the commit is six
     calls, of which the run showed TWO stepped over:

       SetBkMode(0x20c0, 0002)          id 0x02, 4 args   -- UNIMPLEMENTED
       SetROP2(0x20c0, 000d)            id 0x04           -- serviced
       SelectObject(0x20c0, 0x2028)     id 0x2d           -- serviced (the brush)
       CreatePen(006, 0002, 0x000000ff) id 0x3d, 8 args   -- UNIMPLEMENTED
       SelectObject(0x20c0, 0x2000)     ...               -- and it gave up

     ⇒ Paint asked for a 2-pixel `PS_INSIDEFRAME` pen in the colour it had been
     given, got 0, and correctly declined to draw with a pen that does not
     exist. Nothing was wrong with Ellipse or Rectangle -- 48 Ellipse calls in
     that same drag returned 1. **A tool that cannot make a pen has nothing to
     draw with.**
   ★ TWO INDEPENDENT READINGS AGREE ON 0x3d. The run logged
     `FUNC=0x3d ... (0000ff00 00000000 00000002 00000006)` -- a green pen for the
     ellipse and a red one for the box -- and GDI.EXE's own ordinal 61 wrapper at
     `seg1:0x177f` tail-jumps to `0x0284`, which pushes 8 argument bytes and the
     id `0x3d`. ⚠ Here the id happens to EQUAL the ordinal; elsewhere it does not
     (CreateDC is ordinal 53 and id 0x99), so each of these was resolved through
     the entry table rather than assumed.
   ⚠ `neneeds.py` calls all of these "free (16-bit)", because GDI's export is a
     validating wrapper rather than a bare tail-jump and the scan cannot see one
     instruction further. That is the `native16` trap again: **the run finds
     them, the static list does not.** */
#define WOWGDI_CREATEPEN        0x003d   /* ord 61,  8 args (style, width, colour) */
#define CP_ARG_COLOR    0                /* DWORD                                  */
#define CP_ARG_WIDTH    4
#define CP_ARG_STYLE    6

#define WOWGDI_CREATEHATCHBRUSH 0x003a   /* ord 58,  6 args (index, colour)         */
#define CH_ARG_COLOR    0                /* DWORD                                  */
#define CH_ARG_INDEX    4

#define WOWGDI_SETBKMODE        0x0002   /* ord 2,   4 args -- MODE_ARG_* layout    */
#define WOWGDI_SETMAPMODE       0x0003   /* ord 3,   4 args                         */
#define WOWGDI_SETPOLYFILLMODE  0x0006   /* ord 6,   4 args                         */

#define WOWGDI_SETPIXEL         0x001f   /* ord 31, 10 args                         */
#define SP_ARG_COLOR    0                /* DWORD                                  */
#define SP_ARG_Y        4
#define SP_ARG_X        6
#define SP_ARG_HDC      8

#define WOWGDI_POLYLINE         0x0025   /* ord 37,  8 args -- LDP_ARG_* layout     */

/* ── ★★★★★ THE SECOND SWEEP: EVERYTHING ELSE THESE TWO PROGRAMS IMPORT. ─────
     Session 47 taught `tools/ne/neneeds.py` to see through GDI's validating
     export wrappers (it now bounds the scan by the export's OWN `retf`, which
     the wrapper pushes at +3), and the list of what MS Paint and Notepad reach
     went from "41 need us" to **76**. Everything below is on that list, and every
     id and argument count is read out of `gdi.exe`'s entry table -- no id here
     was inferred from an ordinal, and several of them differ from it.
   ★★ THE CORRECTION IT FORCED: **`0x99` IS `CreateIC` (ordinal 153), NOT
     `CreateDC`.** `CreateDC` is ordinal 53 and its id is `0x35`. Session 45 named
     `0x99` from a run, and 153 = 0x99 -- the id tracked the ordinal after all,
     just not the ordinal we thought. It went unnoticed because an information
     context and a device context answer every query identically, so servicing an
     IC as a DC works and only the NAME was wrong. ⇒ both are answered here, and
     the log says which one the guest asked for. */
#define WOWGDI_CREATEDC2        0x0035   /* ord 53,  16 args -- the REAL CreateDC  */

#define WOWGDI_TEXTOUT          0x0021   /* ord 33,  12 args */
#define TO_ARG_COUNT    0
#define TO_ARG_STR      2                /* far */
#define TO_ARG_Y        6
#define TO_ARG_X        8
#define TO_ARG_HDC     10

#define WOWGDI_GETTEXTEXTENT    0x005b   /* ord 91,   8 args */
#define TE_ARG_COUNT    0
#define TE_ARG_STR      2                /* far */
#define TE_ARG_HDC      6

/* ── ★ 0x5d GetTextMetrics, and the Win16 TEXTMETRIC READ OFF NOTEPAD. ───────
     Its fields are `short` where Win32's are `LONG`, in the same order, and the
     order is not taken from a header -- `NOTEPAD.EXE seg1:0x1192` calls it with
     the structure at `ss:[bp-0xb4]` and then does, in the next nine
     instructions:
       mov ax,[bp-0xac] / add ax,[bp-0xb4]   -> tm+8 + tm+0   = the LINE HEIGHT
       mov ax,[bp-0xaa] / shl ax,3           -> tm+10 x 8     = the TAB STOP
     i.e. tm+0 is tmHeight, tm+8 is tmExternalLeading and tm+10 is
     tmAveCharWidth, which pins the first six fields and therefore the whole
     `short` prefix. Two independent uses, one layout. */
#define WOWGDI_GETTEXTMETRICS   0x005d   /* ord 93,   6 args */
#define TM_ARG_BUF      0                /* far */
#define TM_ARG_HDC      4
#define WOW16_TEXTMETRIC_CB  31

#define WOWGDI_SETTEXTALIGN     0x015a   /* ord 346,  4 args -- MODE_ARG_* layout */
#define WOWGDI_CREATEFONTIND    0x0039   /* ord 57,   4 args (far LOGFONT)        */
#define WOWGDI_CREATEPALETTE    0x0168   /* ord 360,  4 args (far LOGPALETTE)     */

#define WOWGDI_DPTOLP           0x0043   /* ord 67,   8 args -- LDP_ARG_* layout  */

/* ── ★ 0x4a GetBitmapBits / 0x6a SetBitmapBits -- PBRUSH.DLL's OWN PAIR. ─────
     (hBitmap, dwCount, lpBits): 2 + 4 + 4 = 10 bytes. The "virtual bitmap
     manager" DLL that owns MS Paint's off-screen image is built on these two,
     which is why they are the only thing it still needed. */
#define WOWGDI_GETBITMAPBITS    0x004a
#define WOWGDI_SETBITMAPBITS    0x006a
#define BB2_ARG_BITS    0                /* far */
#define BB2_ARG_COUNT   4                /* DWORD */
#define BB2_ARG_HBM     8

/* ── ★★★★★ THE DIB TRIO -- WHAT `File > Save As` DIES ON. ───────────────────
     A `BITMAPINFOHEADER` is 40 bytes and byte-identical in both worlds, and the
     bits and the header both live in guest memory this host can address
     directly, so these are the rare calls that need NO conversion at all -- only
     the handles and the signed 16-bit coordinates.
   ⚠ THE ARGUMENT BLOCK IS THE ONLY PLACE TO GET WRONG, and the counts pin it:
     SetDIBits/GetDIBits declare 18 bytes = 2+2+2+2+4+4+2 and StretchDIBits 32. */
#define WOWGDI_SETDIBITS        0x01b8   /* ord 440, 18 args */
#define WOWGDI_GETDIBITS        0x01b9   /* ord 441, 18 args */
#define DIB_ARG_USAGE   0
#define DIB_ARG_BMI     2                /* far */
#define DIB_ARG_BITS    6                /* far */
#define DIB_ARG_LINES  10
#define DIB_ARG_START  12
#define DIB_ARG_HBM    14
#define DIB_ARG_HDC    16

#define WOWGDI_STRETCHDIBITS    0x01b7   /* ord 439, 32 args */
#define SDI_ARG_ROP     0                /* DWORD */
#define SDI_ARG_USAGE   4
#define SDI_ARG_BMI     6                /* far */
#define SDI_ARG_BITS   10                /* far */
#define SDI_ARG_SRCH   14
#define SDI_ARG_SRCW   16
#define SDI_ARG_SRCY   18
#define SDI_ARG_SRCX   20
#define SDI_ARG_DSTH   22
#define SDI_ARG_DSTW   24
#define SDI_ARG_DSTY   26
#define SDI_ARG_DSTX   28
#define SDI_ARG_HDC    30

/* ── ★ MINESWEEPER'S TWO. It keeps its digits, mines and smiley faces as DIBs in
     its own resources and puts them on screen with these; nothing else it draws
     needs GDI at all.
   HBITMAP CreateDIBitmap(HDC, LPBITMAPINFOHEADER, DWORD dwInit, LPSTR lpbInit,
                          LPBITMAPINFO, UINT wUsage)                     = 20 */
#define WOWGDI_CREATEDIBITMAP   0x01ba   /* ord 442, 20 args */
#define CDIB_ARG_USAGE   0
#define CDIB_ARG_BMI     2               /* far */
#define CDIB_ARG_BITS    6               /* far */
#define CDIB_ARG_INIT   10               /* DWORD */
#define CDIB_ARG_BMIH   14               /* far */
#define CDIB_ARG_HDC    18

/* int SetDIBitsToDevice(HDC, int xDest, int yDest, WORD wWidth, WORD wHeight,
                         int XSrc, int YSrc, UINT nStartScan, UINT nNumScans,
                         LPSTR lpBits, LPBITMAPINFO, UINT wUsage)        = 28 */
#define WOWGDI_SETDIBITSTODEV   0x01bb   /* ord 443, 28 args */
#define SDD_ARG_USAGE    0
#define SDD_ARG_BMI      2               /* far */
#define SDD_ARG_BITS     6               /* far */
#define SDD_ARG_NSCANS  10
#define SDD_ARG_START   12
#define SDD_ARG_SRCY    14
#define SDD_ARG_SRCX    16
#define SDD_ARG_H       18
#define SDD_ARG_W       20
#define SDD_ARG_DSTY    22
#define SDD_ARG_DSTX    24
#define SDD_ARG_HDC     26

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
        if (g_wg_obj[i].o == o) {
            if (g_wg_obj[i].kind == kind) return g_wg_obj[i].h;
            /* ── ★★★★★ SAME ADDRESS, DIFFERENT KIND: THE ENTRY IS STALE, AND
                 THIS WAS SOLITAIRE'S BLACK CARDS. Win32 RECYCLES HGDIOBJ VALUES.
                 A memory DC is deleted, a bitmap is created, and the OS hands
                 back the SAME pointer -- so this loop found the dead DC's entry
                 and gave the new BITMAP the DC's token. The log shows one token
                 living both lives within a few calls:

                     SetTextColor(0x20e0, ...)            ; used as a DC
                     GetTextExtent(0x20e0, ...)           ; used as a DC
                     PatBlt(0x20e0, ...)                  ; used as a DC
                     CreateCompatibleDC -> 0x20e8
                     SelectObject(dc 0x20e8, obj 0x20e0)  ; now used as a BITMAP

                 Selecting it put no real bitmap in the memory DC, so the DC kept
                 its default 1x1 monochrome one and every 71x96 card blitted out
                 of it came through BLACK. The blit SUCCEEDED and said so -- there
                 was nothing in the log that looked like a failure.
               ⚠ The kind is not decoration: it is the only thing that can tell a
                 recycled handle from the object that used to live there. Retire
                 the dead entry and mint a fresh token below. */
            g_wg_obj[i].o = NULL;
            g_wg_obj[i].h = 0;
        }
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

        /* ── ★★★★★ NUMCOLORS: -1 IS A Win32 SENTINEL AND NO Win16 PROGRAM HAS
             EVER SEEN ONE. (session 51 -- MINESWEEPER RENDERED IN BLACK AND
             WHITE, and stock ntvdm runs the same binary in colour.)
             Win32 answers NUMCOLORS with -1 for any device deeper than 8bpp.
             That reaches the guest as 0xffff, and WINMINE.EXE's own code -- read
             out of the binary at seg1:0x1820, not guessed -- does this with it:

                 1820  push  0x18          ; NUMCOLORS
                 1822  lcall 0, 0xffff     ; GetDeviceCaps
                 1827  cmp   ax, 2
                 182a  jle   0x1831        ; ★ SIGNED
                 182c  mov   ax, 1         ;   colour
                 1831  sub   ax, ax        ;   MONOCHROME
                 1833  mov   [0x380], ax   ;   the global colour flag

             `jle` is the SIGNED branch, so -1 <= 2 is TRUE and Minesweeper sets
             its monochrome flag -- after which every DIB it builds and hands to
             SetDIBitsToDevice really is 1bpp, and this host draws it faithfully.
             Nothing downstream was wrong. The wrong answer was here.

           ★ SO IT IS ANSWERED IN Win16's OWN TERMS: the size of a colour table,
             which is what the index MEANS. <= 8bpp gives the exact count; deeper
             than that has no table at all, and 256 is both the largest a Windows
             3.1 driver ever reported and the largest a program of this era was
             built to read. Every Win16 caller tests it for ">2" or "==2".

           ⚠⚠ AND THE SESSION-45 REFUTATION STILL STANDS -- it was about a
             DIFFERENT GUEST and it is not contradicted. MS Paint asks NUMCOLORS
             four times and never asks BITSPIXEL or PLANES, so -1 looked certain
             to be why every bitmap it makes is `planes=1 bpp=1`; substituting
             256 changed NOTHING, because Paint's 1bpp bitmaps are MASKS (the
             Win16 idiom is a monochrome pattern plus SetTextColor/SetBkColor at
             blit time, and that run made 34 of each). Both are true: 256 is not
             the lever for Paint's masks, AND -1 is the wrong answer to give a
             16-bit caller. What was refuted was a hypothesis about Paint, not
             the value. ⇒ Re-run Paint after touching this. */
        if (idx == WOWGDI_CAP_NUMCOLORS && v < 0) {
            int bpp = GetDeviceCaps((HDC)o, BITSPIXEL)
                    * GetDeviceCaps((HDC)o, PLANES);
            v = wowconv_numcolors(bpp);       /* ★ tested in wow_test.c part 3 */
            wu_puts(note, notecap, &k, " [NUMCOLORS -1 -> ");
            wu_puthex(note, notecap, &k, (DWORD)v, 4);
            wu_puts(note, notecap, &k, "; a Win16 caller reads -1 as MONOCHROME]");
        }
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

    /* ── ★ 0x4f GetDCOrg / 0x95 GetBrushOrg / 0x96 UnrealizeObject ──────────*/
    case WOWGDI_GETDCORG:
    case WOWGDI_GETBRUSHORG:
    case WOWGDI_UNREALIZEOBJ: {
        WORD h = wow32_argw(f, ONE_ARG_HANDLE);
        int  kind = -1;
        HGDIOBJ o = wowgdi_h32(h, &kind);
        int  isdc = (f->id != WOWGDI_UNREALIZEOBJ);
        int  k = 0;
        POINT pt;
        wu_puts(note, notecap, &k,
                f->id == WOWGDI_GETDCORG    ? "GetDCOrg(0x" :
                f->id == WOWGDI_GETBRUSHORG ? "GetBrushOrg(0x"
                                            : "UnrealizeObject(0x");
        wu_puthex(note, notecap, &k, h, 4);
        wu_puts(note, notecap, &k, ")");
        if (!o) {
            wu_puts(note, notecap, &k, " -- ★ NOT ONE OF OUR GDI TOKENS;"
                                       " answered 0");
            wow32_setret(f, 0);
            return 1;
        }
        if (isdc && kind != WOWGDI_KIND_DC && kind != WOWGDI_KIND_WINDC) {
            wu_puts(note, notecap, &k, " -- ★ THAT IS NOT A DC; answered 0");
            wow32_setret(f, 0);
            return 1;
        }
        if (f->id == WOWGDI_UNREALIZEOBJ) {
            int r = UnrealizeObject(o) ? 1 : 0;
            wu_puts(note, notecap, &k, r ? " -> unrealized" : " -- ★ refused");
            wow32_setret(f, (DWORD)r);
            return 1;
        }
        pt.x = pt.y = 0;
        if (f->id == WOWGDI_GETBRUSHORG) GetBrushOrgEx((HDC)o, &pt);
        else                             GetDCOrgEx((HDC)o, &pt);
        wu_puts(note, notecap, &k, " -> ");
        wu_puthex(note, notecap, &k, (DWORD)pt.x, 4);
        wu_puts(note, notecap, &k, ",");
        wu_puthex(note, notecap, &k, (DWORD)pt.y, 4);
        wow32_setret(f, ((DWORD)(WORD)(short)pt.y << 16)
                        | (DWORD)(WORD)(short)pt.x);
        return 1;
    }

    /* ── ★ 0x67 PtVisible(hDC, x, y) ────────────────────────────────────────*/
    case WOWGDI_PTVISIBLE: {
        WORD hdc = wow32_argw(f, PV_ARG_HDC);
        int  x = (int)(short)wow32_argw(f, PV_ARG_X);
        int  y = (int)(short)wow32_argw(f, PV_ARG_Y);
        int  kind = -1;
        HGDIOBJ o = wowgdi_h32(hdc, &kind);
        int  k = 0, r;
        wu_puts(note, notecap, &k, "PtVisible(0x");
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
        r = PtVisible((HDC)o, x, y) ? 1 : 0;
        wu_puts(note, notecap, &k, r ? " -> visible" : " -> NOT visible");
        wow32_setret(f, (DWORD)r);
        return 1;
    }

    /* ── ★★★★ 0x63 LPtoDP(hDC, lpPoints, nCount) -- see the note above. ─────*/
    case WOWGDI_POLYGON:
    case WOWGDI_POLYLINE:
    case WOWGDI_DPTOLP:
    case WOWGDI_LPTODP: {
        WORD hdc = wow32_argw(f, LDP_ARG_HDC);
        WORD n   = wow32_argw(f, LDP_ARG_COUNT);
        volatile BYTE *p = wow32_argptr(f, LDP_ARG_POINTS);
        int  kind = -1;
        HGDIOBJ o = wowgdi_h32(hdc, &kind);
        POINT pt[64];
        int  k = 0, i, cnt = (int)n;
        int isline = (f->id == WOWGDI_POLYLINE);
        int ispoly = (f->id == WOWGDI_POLYGON) || isline;
        wu_puts(note, notecap, &k,
                isline ? "Polyline(0x" : ispoly ? "Polygon(0x"
                : f->id == WOWGDI_DPTOLP ? "DPtoLP(0x" : "LPtoDP(0x");
        wu_puthex(note, notecap, &k, hdc, 4);
        wu_puts(note, notecap, &k, ", ");
        wu_puthex(note, notecap, &k, (DWORD)n, 4);
        wu_puts(note, notecap, &k, " points)");
        if (!o || (kind != WOWGDI_KIND_DC && kind != WOWGDI_KIND_WINDC) || !p) {
            wu_puts(note, notecap, &k, " -- ★ NOT ONE OF OUR DC TOKENS, or no"
                                       " points; answered 0");
            wow32_setret(f, 0);
            return 1;
        }
        /* ⚠ BOUNDED. The count comes from the guest and the scratch array does
             not grow; a longer run is refused rather than overrunning. */
        if (cnt <= 0 || cnt > (int)(sizeof pt / sizeof pt[0])) {
            wu_puts(note, notecap, &k, " -- ★ COUNT OUT OF RANGE; answered 0");
            wow32_setret(f, 0);
            return 1;
        }
        for (i = 0; i < cnt; ++i) {
            pt[i].x = (int)(short)wow32_peekw(p + i * 4);
            pt[i].y = (int)(short)wow32_peekw(p + i * 4 + 2);
        }
        /* ★ Polygon DRAWS and writes nothing back; LPtoDP TRANSFORMS IN PLACE.
             Same block, opposite data flow -- so they share the read and part
             company here. */
        if (ispoly) {
            int r = (isline ? Polyline((HDC)o, pt, cnt)
                            : Polygon((HDC)o, pt, cnt)) ? 1 : 0;
            wu_puts(note, notecap, &k, r ? " -> drawn" : " -- ★ the OS refused it");
            wow32_setret(f, (DWORD)r);
            return 1;
        }
        /* ★ DPtoLP is LPtoDP's inverse and nothing else differs -- same block,
             same in-place write-back, opposite transform. */
        if (!(f->id == WOWGDI_DPTOLP ? DPtoLP((HDC)o, pt, cnt)
                                     : LPtoDP((HDC)o, pt, cnt))) {
            wu_puts(note, notecap, &k, " -- ★ the OS refused it; answered 0");
            wow32_setret(f, 0);
            return 1;
        }
        for (i = 0; i < cnt; ++i) {
            wow32_pokew(p + i * 4,     (WORD)(short)pt[i].x);
            wow32_pokew(p + i * 4 + 2, (WORD)(short)pt[i].y);
        }
        wu_puts(note, notecap, &k, " -> ");
        wu_puthex(note, notecap, &k, (DWORD)pt[0].x, 4);
        wu_puts(note, notecap, &k, ",");
        wu_puthex(note, notecap, &k, (DWORD)pt[0].y, 4);
        wow32_setret(f, 1);
        return 1;
    }

    /* ── ★★ 0x9c CreateDiscardableBitmap / 0x94 SetBrushOrg ─────────────────
       ★ "Discardable" was a Win16 memory-pressure hint; Win32 keeps the call and
         ignores the hint, which is the right answer -- the guest gets a real
         bitmap and the hint had no observable semantics to preserve. */
    case WOWGDI_CREATEDISCARDBM:
    case WOWGDI_SETBRUSHORG: {
        int  isbm = (f->id == WOWGDI_CREATEDISCARDBM);
        WORD hdc = wow32_argw(f, CCB_ARG_HDC);      /* same block shape as   */
        int  a = (int)(short)wow32_argw(f, CCB_ARG_WIDTH);   /* CreateCompat- */
        int  b = (int)(short)wow32_argw(f, CCB_ARG_HEIGHT);  /* ibleBitmap    */
        int  kind = -1;
        HGDIOBJ o = wowgdi_h32(hdc, &kind);
        int  k = 0;
        wu_puts(note, notecap, &k, isbm ? "CreateDiscardableBitmap(0x"
                                        : "SetBrushOrg(0x");
        wu_puthex(note, notecap, &k, hdc, 4);
        wu_puts(note, notecap, &k, ", ");
        wu_puthex(note, notecap, &k, (DWORD)a, 4);
        wu_puts(note, notecap, &k, ",");
        wu_puthex(note, notecap, &k, (DWORD)b, 4);
        wu_puts(note, notecap, &k, ")");
        if (!o || (kind != WOWGDI_KIND_DC && kind != WOWGDI_KIND_WINDC)) {
            wu_puts(note, notecap, &k, " -- ★ NOT ONE OF OUR DC TOKENS;"
                                       " answered 0");
            wow32_setret(f, 0);
            return 1;
        }
        if (!isbm) {
            POINT prev;
            prev.x = prev.y = 0;
            SetBrushOrgEx((HDC)o, a, b, &prev);
            wow32_setret(f, ((DWORD)(WORD)(short)prev.y << 16)
                            | (DWORD)(WORD)(short)prev.x);
            return 1;
        }
        {   HBITMAP bm;
            WORD tok;
            if (a <= 0 || b <= 0) {
                wu_puts(note, notecap, &k, " -- ★ A DIMENSION IS NOT POSITIVE;"
                                           " answered 0");
                wow32_setret(f, 0);
                return 1;
            }
            bm = CreateDiscardableBitmap((HDC)o, a, b);
            tok = bm ? wowgdi_h16((HGDIOBJ)bm, WOWGDI_KIND_OBJ) : 0;
            if (!tok) {
                if (bm) DeleteObject((HGDIOBJ)bm);
                wu_puts(note, notecap, &k, " -- ★ refused, or the token map is"
                                           " full; answered 0");
                wow32_setret(f, 0);
                return 1;
            }
            wu_puts(note, notecap, &k, " -> bitmap token 0x");
            wu_puthex(note, notecap, &k, tok, 4);
            wow32_setret(f, tok);
            return 1;
        }
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

    /* ── ★ The (hDC, int mode) family -- one int, same numbering both sides. ──
         0x04 SetROP2, 0x07 SetStretchBltMode, 0x02 SetBkMode, 0x03 SetMapMode,
         0x06 SetPolyFillMode. All 4 argument bytes, all returning the previous
         mode. ⚠ The last three arrived with MS Paint's shape tools: SetBkMode
         is the first call of the commit sequence and SetMapMode goes with the
         SetWindowExt/SetViewportExt pair, which is MM_ANISOTROPIC (8) being set
         up. */
    case WOWGDI_SETROP2:
    case WOWGDI_SETSTRETCHMODE:
    case WOWGDI_SETBKMODE:
    case WOWGDI_SETMAPMODE:
    case WOWGDI_SETTEXTALIGN:
    case WOWGDI_SETPOLYFILLMODE: {
        WORD hdc  = wow32_argw(f, MODE_ARG_HDC);
        int  mode = (int)(short)wow32_argw(f, MODE_ARG_MODE);
        int  kind = -1;
        HGDIOBJ o = wowgdi_h32(hdc, &kind);
        int  k = 0, prev;
        wu_puts(note, notecap, &k,
                f->id == WOWGDI_SETROP2         ? "SetROP2(0x" :
                f->id == WOWGDI_SETSTRETCHMODE  ? "SetStretchBltMode(0x" :
                f->id == WOWGDI_SETBKMODE       ? "SetBkMode(0x" :
                f->id == WOWGDI_SETMAPMODE      ? "SetMapMode(0x" :
                f->id == WOWGDI_SETTEXTALIGN    ? "SetTextAlign(0x"
                                                : "SetPolyFillMode(0x");
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
        prev = f->id == WOWGDI_SETROP2        ? SetROP2((HDC)o, mode)
             : f->id == WOWGDI_SETSTRETCHMODE ? SetStretchBltMode((HDC)o, mode)
             : f->id == WOWGDI_SETBKMODE      ? SetBkMode((HDC)o, mode)
             : f->id == WOWGDI_SETMAPMODE     ? SetMapMode((HDC)o, mode)
             : f->id == WOWGDI_SETTEXTALIGN   ? (int)SetTextAlign((HDC)o, (UINT)mode)
                                              : SetPolyFillMode((HDC)o, mode);
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
    case WOWGDI_CREATEDC:
    case WOWGDI_CREATEDC2: {
        char drv[64], dev[64];
        HDC dc;
        WORD tok;
        int  k = 0, isdisp, i;
        int  isic = (f->id == WOWGDI_CREATEDC);
        DWORD init = wow32_argd(f, CDC_ARG_INITDATA);
        wow32_argstr(f, CDC_ARG_DRIVER, drv, sizeof drv);
        wow32_argstr(f, CDC_ARG_DEVICE, dev, sizeof dev);
        wu_puts(note, notecap, &k, isic ? "CreateIC driver=" : "CreateDC driver=");
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
        /* ★ An INFORMATION context is asked for by name and answered with one:
             it is cheaper than a DC and a guest that draws on one is doing
             something Windows would refuse too, so the distinction is kept
             rather than flattened into CreateDC. */
        dc = isic ? CreateICA("DISPLAY", NULL, NULL, NULL)
                  : CreateDCA("DISPLAY", NULL, NULL, NULL);
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

    /* ── ★★ 0x18 Ellipse / 0x15 ExcludeClipRect -- Rectangle's block, twice. ──
         Both are (hDC, left, top, right, bottom) in 10 bytes, which is what lets
         them share RC_ARG_*. ⚠ They are NOT the same kind of call and the note
         says which is which: Ellipse draws, ExcludeClipRect changes what any
         later call is allowed to touch, and its answer is a region COMPLEXITY
         code (NULLREGION/SIMPLEREGION/COMPLEXREGION), not a boolean. */
    case WOWGDI_ELLIPSE:
    case WOWGDI_EXCLUDECLIPRECT: {
        int  isell = (f->id == WOWGDI_ELLIPSE);
        WORD hdc = wow32_argw(f, RC_ARG_HDC);
        int  l = (int)(short)wow32_argw(f, RC_ARG_LEFT);
        int  t = (int)(short)wow32_argw(f, RC_ARG_TOP);
        int  r = (int)(short)wow32_argw(f, RC_ARG_RIGHT);
        int  b = (int)(short)wow32_argw(f, RC_ARG_BOTTOM);
        int  kind = -1;
        HGDIOBJ o = wowgdi_h32(hdc, &kind);
        int  k = 0;
        wu_puts(note, notecap, &k, isell ? "Ellipse(0x" : "ExcludeClipRect(0x");
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
        if (isell) {
            wow32_setret(f, (DWORD)(Ellipse((HDC)o, l, t, r, b) ? 1 : 0));
        } else {
            int rc = ExcludeClipRect((HDC)o, l, t, r, b);
            wu_puts(note, notecap, &k, " -> region complexity ");
            wu_puthex(note, notecap, &k, (DWORD)rc, 4);
            wow32_setret(f, (DWORD)(WORD)rc);
        }
        return 1;
    }

    /* ── ★ 0x1c RoundRect(hDC, l, t, r, b, ellipseW, ellipseH) -- 14 bytes. ──*/
    case WOWGDI_ROUNDRECT: {
        WORD hdc = wow32_argw(f, RR_ARG_HDC);
        int  l  = (int)(short)wow32_argw(f, RR_ARG_LEFT);
        int  t  = (int)(short)wow32_argw(f, RR_ARG_TOP);
        int  r  = (int)(short)wow32_argw(f, RR_ARG_RIGHT);
        int  b  = (int)(short)wow32_argw(f, RR_ARG_BOTTOM);
        int  ew = (int)(short)wow32_argw(f, RR_ARG_EW);
        int  eh = (int)(short)wow32_argw(f, RR_ARG_EH);
        int  kind = -1;
        HGDIOBJ o = wowgdi_h32(hdc, &kind);
        int  k = 0;
        wu_puts(note, notecap, &k, "RoundRect(0x");
        wu_puthex(note, notecap, &k, hdc, 4);
        wu_puts(note, notecap, &k, ", ");
        wu_puthex(note, notecap, &k, (DWORD)l, 4);
        wu_puts(note, notecap, &k, ",");
        wu_puthex(note, notecap, &k, (DWORD)t, 4);
        wu_puts(note, notecap, &k, " ");
        wu_puthex(note, notecap, &k, (DWORD)r, 4);
        wu_puts(note, notecap, &k, ",");
        wu_puthex(note, notecap, &k, (DWORD)b, 4);
        wu_puts(note, notecap, &k, " corner ");
        wu_puthex(note, notecap, &k, (DWORD)ew, 4);
        wu_puts(note, notecap, &k, "x");
        wu_puthex(note, notecap, &k, (DWORD)eh, 4);
        wu_puts(note, notecap, &k, ")");
        if (!o || (kind != WOWGDI_KIND_DC && kind != WOWGDI_KIND_WINDC)) {
            wu_puts(note, notecap, &k, " -- ★ NOT ONE OF OUR DC TOKENS;"
                                       " answered 0");
            wow32_setret(f, 0);
            return 1;
        }
        wow32_setret(f, (DWORD)(RoundRect((HDC)o, l, t, r, b, ew, eh) ? 1 : 0));
        return 1;
    }

    /* ── ★★★★★ 0x174 ExtFloodFill -- THE FILL TOOL. ─────────────────────────
       ⚠ THE FAILURE MODE THIS REPLACES IS SILENCE. Unimplemented, the call was
         stepped over and answered with the harness sentinel 0 -- which is
         ExtFloodFill's own "I filled nothing", so Paint had no way to tell a
         host that cannot fill from a fill that had nothing to do. The tool
         appeared to be selected, appeared to take the click, and did nothing.
       ★ Paint calls it twice per fill: once with a solid brush selected and once
         with a pattern brush from `CreatePatternBrush` (seg4:0x164f), which is
         how a Win16 program fills with one of the palette's patterns. */
    case WOWGDI_EXTFLOODFILL: {
        WORD  hdc  = wow32_argw(f, FF_ARG_HDC);
        int   x    = (int)(short)wow32_argw(f, FF_ARG_X);
        int   y    = (int)(short)wow32_argw(f, FF_ARG_Y);
        DWORD col  = wow32_argd(f, FF_ARG_COLOR);
        WORD  type = wow32_argw(f, FF_ARG_TYPE);
        int   kind = -1;
        HGDIOBJ o = wowgdi_h32(hdc, &kind);
        int   k = 0, ok;
        wu_puts(note, notecap, &k, "ExtFloodFill(0x");
        wu_puthex(note, notecap, &k, hdc, 4);
        wu_puts(note, notecap, &k, ", ");
        wu_puthex(note, notecap, &k, (DWORD)x, 4);
        wu_puts(note, notecap, &k, ",");
        wu_puthex(note, notecap, &k, (DWORD)y, 4);
        wu_puts(note, notecap, &k, " colour 0x");
        wu_puthex(note, notecap, &k, col, 8);
        wu_puts(note, notecap, &k, type ? " SURFACE)" : " BORDER)");
        if (!o || (kind != WOWGDI_KIND_DC && kind != WOWGDI_KIND_WINDC)) {
            wu_puts(note, notecap, &k, " -- ★ NOT ONE OF OUR DC TOKENS;"
                                       " answered 0");
            wow32_setret(f, 0);
            return 1;
        }
        ok = ExtFloodFill((HDC)o, x, y, (COLORREF)col, (UINT)type) ? 1 : 0;
        wu_puts(note, notecap, &k, ok ? " -> filled" : " -> the OS filled NOTHING");
        wow32_setret(f, (DWORD)ok);
        return 1;
    }

    /* ── ★★★★★ 0x3d CreatePen -- THE CALL EVERY SHAPE TOOL WAITED ON. ───────
         (nPenStyle, nWidth, crColor), 8 bytes, and the two the run made say so:
         `(000000ff 0002 0006)` for the red box and `(0000ff00 0002 0006)` for
         the green ellipse -- style 6 is `PS_INSIDEFRAME`, which is exactly what
         a paint program wants so a thick outline stays inside the rectangle the
         user dragged. ⚠ Both the style numbering and the COLORREF are unchanged
         between Win16 and Win32, so nothing here is a translation.
       ★ 0x3a CreateHatchBrush(nIndex, crColor) is its sibling in every respect
         and is what the filled-shape and pattern tools ask for next; same
         indices (HS_HORIZONTAL..HS_DIAGCROSS), same COLORREF. */
    case WOWGDI_CREATEPEN:
    case WOWGDI_CREATEHATCHBRUSH: {
        int   ispen = (f->id == WOWGDI_CREATEPEN);
        DWORD col   = wow32_argd(f, ispen ? CP_ARG_COLOR : CH_ARG_COLOR);
        int   a     = (int)(short)wow32_argw(f, ispen ? CP_ARG_WIDTH : CH_ARG_INDEX);
        int   style = ispen ? (int)(short)wow32_argw(f, CP_ARG_STYLE) : 0;
        int   k = 0;
        HGDIOBJ obj;
        WORD tok;
        wu_puts(note, notecap, &k, ispen ? "CreatePen(style " : "CreateHatchBrush(index ");
        wu_puthex(note, notecap, &k, (DWORD)(ispen ? style : a), 4);
        if (ispen) {
            wu_puts(note, notecap, &k, ", width ");
            wu_puthex(note, notecap, &k, (DWORD)a, 4);
        }
        wu_puts(note, notecap, &k, ", 0x");
        wu_puthex(note, notecap, &k, col, 8);
        wu_puts(note, notecap, &k, ")");
        obj = ispen ? (HGDIOBJ)CreatePen(style, a, (COLORREF)col)
                    : (HGDIOBJ)CreateHatchBrush(a, (COLORREF)col);
        tok = obj ? wowgdi_h16(obj, WOWGDI_KIND_OBJ) : 0;
        if (!tok) {
            if (obj) DeleteObject(obj);
            wu_puts(note, notecap, &k, " -- ★ the OS refused it (or the token map"
                                       " is full); answered 0");
            wow32_setret(f, 0);
            return 1;
        }
        wu_puts(note, notecap, &k, ispen ? " -> pen token 0x" : " -> brush token 0x");
        wu_puthex(note, notecap, &k, tok, 4);
        wow32_setret(f, tok);
        return 1;
    }

    /* ── ★ 0x1f SetPixel(hDC, x, y, crColor) -- GetPixel's twin, 10 bytes. ───
       ⚠ The answer is the colour ACTUALLY set, which on a device that cannot
         represent the request is not the colour asked for -- so it is returned
         as the OS gives it rather than echoed. */
    case WOWGDI_SETPIXEL: {
        WORD  hdc = wow32_argw(f, SP_ARG_HDC);
        int   x = (int)(short)wow32_argw(f, SP_ARG_X);
        int   y = (int)(short)wow32_argw(f, SP_ARG_Y);
        DWORD col = wow32_argd(f, SP_ARG_COLOR);
        int   kind = -1;
        HGDIOBJ o = wowgdi_h32(hdc, &kind);
        int   k = 0;
        COLORREF got;
        wu_puts(note, notecap, &k, "SetPixel(0x");
        wu_puthex(note, notecap, &k, hdc, 4);
        wu_puts(note, notecap, &k, ", ");
        wu_puthex(note, notecap, &k, (DWORD)x, 4);
        wu_puts(note, notecap, &k, ",");
        wu_puthex(note, notecap, &k, (DWORD)y, 4);
        wu_puts(note, notecap, &k, " 0x");
        wu_puthex(note, notecap, &k, col, 8);
        wu_puts(note, notecap, &k, ")");
        if (!o || (kind != WOWGDI_KIND_DC && kind != WOWGDI_KIND_WINDC)) {
            wu_puts(note, notecap, &k, " -- ★ NOT ONE OF OUR DC TOKENS;"
                                       " answered CLR_INVALID");
            wow32_setret(f, 0xFFFFFFFFu);
            return 1;
        }
        got = SetPixel((HDC)o, x, y, (COLORREF)col);
        wow32_setret(f, (DWORD)got);
        return 1;
    }

    /* ── ★ 0x3c CreatePatternBrush(hBitmap) -- one of OUR bitmap tokens. ─────*/
    case WOWGDI_CREATEPATTERNBRUSH: {
        WORD hbm = wow32_argw(f, ONE_ARG_HANDLE);
        int  kind = -1;
        HGDIOBJ o = wowgdi_h32(hbm, &kind);
        int  k = 0;
        HBRUSH br;
        WORD tok;
        wu_puts(note, notecap, &k, "CreatePatternBrush(0x");
        wu_puthex(note, notecap, &k, hbm, 4);
        wu_puts(note, notecap, &k, ")");
        if (!o || (kind != WOWGDI_KIND_OBJ && kind != WOWGDI_KIND_STOCK)) {
            wu_puts(note, notecap, &k, " -- ★ NOT ONE OF OUR BITMAP TOKENS;"
                                       " answered 0");
            wow32_setret(f, 0);
            return 1;
        }
        br = CreatePatternBrush((HBITMAP)o);
        tok = br ? wowgdi_h16((HGDIOBJ)br, WOWGDI_KIND_OBJ) : 0;
        if (!tok) {
            if (br) DeleteObject((HGDIOBJ)br);
            wu_puts(note, notecap, &k, " -- ★ no brush (or the token map is"
                                       " full); answered 0");
            wow32_setret(f, 0);
            return 1;
        }
        wu_puts(note, notecap, &k, " -> brush token 0x");
        wu_puthex(note, notecap, &k, tok, 4);
        wow32_setret(f, tok);
        return 1;
    }

    /* ── ★ 0x53 GetPixel(hDC, x, y) ─────────────────────────────────────────
       ⚠ CLR_INVALID IS 0xFFFFFFFF AND IT IS NOT AN ERROR CODE THE GUEST CAN
         MISREAD AS A COLOUR -- Win16 uses the same value for the same reason, so
         a point outside the clip region travels through unchanged. */
    case WOWGDI_GETPIXEL: {
        WORD hdc = wow32_argw(f, XY_ARG_HDC);
        int  x = (int)(short)wow32_argw(f, XY_ARG_X);
        int  y = (int)(short)wow32_argw(f, XY_ARG_Y);
        int  kind = -1;
        HGDIOBJ o = wowgdi_h32(hdc, &kind);
        int  k = 0;
        COLORREF c;
        wu_puts(note, notecap, &k, "GetPixel(0x");
        wu_puthex(note, notecap, &k, hdc, 4);
        wu_puts(note, notecap, &k, ", ");
        wu_puthex(note, notecap, &k, (DWORD)x, 4);
        wu_puts(note, notecap, &k, ",");
        wu_puthex(note, notecap, &k, (DWORD)y, 4);
        wu_puts(note, notecap, &k, ")");
        if (!o || (kind != WOWGDI_KIND_DC && kind != WOWGDI_KIND_WINDC)) {
            wu_puts(note, notecap, &k, " -- ★ NOT ONE OF OUR DC TOKENS;"
                                       " answered CLR_INVALID");
            wow32_setret(f, 0xFFFFFFFFu);
            return 1;
        }
        c = GetPixel((HDC)o, x, y);
        wu_puts(note, notecap, &k, " = 0x");
        wu_puthex(note, notecap, &k, (DWORD)c, 8);
        wow32_setret(f, (DWORD)c);
        return 1;
    }

    /* ── ★ Three one-DC questions: 0x4b GetBkColor, 0x55 GetROP2,
         0x16e UpdateColors. ⚠ GetBkColor's answer is a DWORD COLORREF and the
         other two are ints, which is the only difference between them here. */
    case WOWGDI_GETBKCOLOR:
    case WOWGDI_GETROP2:
    case WOWGDI_UPDATECOLORS: {
        WORD hdc = wow32_argw(f, ONE_ARG_HANDLE);
        int  kind = -1;
        HGDIOBJ o = wowgdi_h32(hdc, &kind);
        int  k = 0;
        wu_puts(note, notecap, &k,
                f->id == WOWGDI_GETBKCOLOR ? "GetBkColor(0x" :
                f->id == WOWGDI_GETROP2    ? "GetROP2(0x"
                                           : "UpdateColors(0x");
        wu_puthex(note, notecap, &k, hdc, 4);
        wu_puts(note, notecap, &k, ")");
        if (!o || (kind != WOWGDI_KIND_DC && kind != WOWGDI_KIND_WINDC)) {
            wu_puts(note, notecap, &k, " -- ★ NOT ONE OF OUR DC TOKENS;"
                                       " answered 0");
            wow32_setret(f, 0);
            return 1;
        }
        if (f->id == WOWGDI_GETBKCOLOR) {
            COLORREF c = GetBkColor((HDC)o);
            wu_puts(note, notecap, &k, " = 0x");
            wu_puthex(note, notecap, &k, (DWORD)c, 8);
            wow32_setret(f, (DWORD)c);
        } else if (f->id == WOWGDI_GETROP2) {
            int v = GetROP2((HDC)o);
            wu_puts(note, notecap, &k, " = ");
            wu_puthex(note, notecap, &k, (DWORD)v, 4);
            wow32_setret(f, (DWORD)(WORD)v);
        } else {
            wow32_setret(f, (DWORD)(UpdateColors((HDC)o) ? 1 : 0));
        }
        return 1;
    }

    /* ── ★ 0x40 CreateRectRgn / 0x2c SelectClipRgn -- clipping, as a pair. ───
       ⚠ A REGION IS AN ORDINARY GDI OBJECT and goes in the same token map with
         KIND_OBJ, so the guest's own `DeleteObject` disposes of it with no new
         case. ⚠ `SelectClipRgn(hDC, NULL)` is the documented way to REMOVE the
         clip region, so a zero handle is not an error here -- it is the call
         doing its other job, and passing our "not one of our tokens" refusal for
         it would leave a guest permanently clipped. */
    case WOWGDI_CREATERECTRGN: {
        int  l = (int)(short)wow32_argw(f, RGN_ARG_LEFT);
        int  t = (int)(short)wow32_argw(f, RGN_ARG_TOP);
        int  r = (int)(short)wow32_argw(f, RGN_ARG_RIGHT);
        int  b = (int)(short)wow32_argw(f, RGN_ARG_BOTTOM);
        int  k = 0;
        HRGN rgn = CreateRectRgn(l, t, r, b);
        WORD tok = rgn ? wowgdi_h16((HGDIOBJ)rgn, WOWGDI_KIND_OBJ) : 0;
        wu_puts(note, notecap, &k, "CreateRectRgn(");
        wu_puthex(note, notecap, &k, (DWORD)l, 4);
        wu_puts(note, notecap, &k, ",");
        wu_puthex(note, notecap, &k, (DWORD)t, 4);
        wu_puts(note, notecap, &k, " ");
        wu_puthex(note, notecap, &k, (DWORD)r, 4);
        wu_puts(note, notecap, &k, ",");
        wu_puthex(note, notecap, &k, (DWORD)b, 4);
        wu_puts(note, notecap, &k, ")");
        if (!tok) {
            if (rgn) DeleteObject((HGDIOBJ)rgn);
            wu_puts(note, notecap, &k, " -- ★ no region (or the token map is"
                                       " full); answered 0");
            wow32_setret(f, 0);
            return 1;
        }
        wu_puts(note, notecap, &k, " -> region token 0x");
        wu_puthex(note, notecap, &k, tok, 4);
        wow32_setret(f, tok);
        return 1;
    }

    case WOWGDI_SELECTCLIPRGN: {
        WORD hdc  = wow32_argw(f, SCR_ARG_HDC);
        WORD hrgn = wow32_argw(f, SCR_ARG_RGN);
        int  dkind = -1, rkind = -1;
        HGDIOBJ d = wowgdi_h32(hdc, &dkind);
        HGDIOBJ r = wowgdi_h32(hrgn, &rkind);
        int  k = 0, rc;
        wu_puts(note, notecap, &k, "SelectClipRgn(0x");
        wu_puthex(note, notecap, &k, hdc, 4);
        wu_puts(note, notecap, &k, ", 0x");
        wu_puthex(note, notecap, &k, hrgn, 4);
        wu_puts(note, notecap, &k, ")");
        if (!d || (dkind != WOWGDI_KIND_DC && dkind != WOWGDI_KIND_WINDC)) {
            wu_puts(note, notecap, &k, " -- ★ NOT ONE OF OUR DC TOKENS;"
                                       " answered 0");
            wow32_setret(f, 0);
            return 1;
        }
        if (hrgn && !r) {
            wu_puts(note, notecap, &k, " -- ★ NOT ONE OF OUR REGION TOKENS;"
                                       " answered 0 rather than clearing the"
                                       " clip region, which is what NULL means");
            wow32_setret(f, 0);
            return 1;
        }
        if (!hrgn) wu_puts(note, notecap, &k, " -- NULL = remove the clip region");
        rc = SelectClipRgn((HDC)d, (HRGN)r);
        wu_puts(note, notecap, &k, " -> region complexity ");
        wu_puthex(note, notecap, &k, (DWORD)rc, 4);
        wow32_setret(f, (DWORD)(WORD)rc);
        return 1;
    }

    /* ── ★ 0x0c SetWindowExt / 0x0d SetViewportOrg / 0x0e SetViewportExt /
         0xa3 SetBitmapDimension -- one (handle, x, y) block, four calls. ──────
       ⚠ THREE TAKE A DC AND THE FOURTH TAKES A BITMAP, which is exactly the kind
         of difference that a shared case hides. It is checked per-id here rather
         than assumed, because handing a bitmap to SetViewportOrgEx would fail
         quietly and leave a guest drawing at the wrong origin.
       ★ All four answer with the PREVIOUS pair packed y:x in a DWORD, which is
         the same convention SetWindowOrg already uses next door. */
    case WOWGDI_SETWINDOWEXT:
    case WOWGDI_SETVIEWPORTORG:
    case WOWGDI_SETVIEWPORTEXT:
    case WOWGDI_SETBITMAPDIM: {
        WORD h = wow32_argw(f, ORG_ARG_HDC);
        int  x = (int)(short)wow32_argw(f, ORG_ARG_X);
        int  y = (int)(short)wow32_argw(f, ORG_ARG_Y);
        int  isbm = (f->id == WOWGDI_SETBITMAPDIM);
        int  kind = -1;
        HGDIOBJ o = wowgdi_h32(h, &kind);
        int  k = 0;
        SIZE sz; POINT pt;
        wu_puts(note, notecap, &k,
                f->id == WOWGDI_SETWINDOWEXT   ? "SetWindowExt(0x" :
                f->id == WOWGDI_SETVIEWPORTORG ? "SetViewportOrg(0x" :
                f->id == WOWGDI_SETVIEWPORTEXT ? "SetViewportExt(0x"
                                               : "SetBitmapDimension(0x");
        wu_puthex(note, notecap, &k, h, 4);
        wu_puts(note, notecap, &k, ", ");
        wu_puthex(note, notecap, &k, (DWORD)x, 4);
        wu_puts(note, notecap, &k, ",");
        wu_puthex(note, notecap, &k, (DWORD)y, 4);
        wu_puts(note, notecap, &k, ")");
        if (!o || (isbm ? (kind != WOWGDI_KIND_OBJ && kind != WOWGDI_KIND_STOCK)
                        : (kind != WOWGDI_KIND_DC && kind != WOWGDI_KIND_WINDC))) {
            wu_puts(note, notecap, &k, isbm
                        ? " -- ★ NOT ONE OF OUR BITMAP TOKENS; answered 0"
                        : " -- ★ NOT ONE OF OUR DC TOKENS; answered 0");
            wow32_setret(f, 0);
            return 1;
        }
        sz.cx = sz.cy = 0; pt.x = pt.y = 0;
        if (f->id == WOWGDI_SETWINDOWEXT)        SetWindowExtEx((HDC)o, x, y, &sz);
        else if (f->id == WOWGDI_SETVIEWPORTEXT) SetViewportExtEx((HDC)o, x, y, &sz);
        else if (f->id == WOWGDI_SETVIEWPORTORG) {
            SetViewportOrgEx((HDC)o, x, y, &pt);
            sz.cx = pt.x; sz.cy = pt.y;
        } else {
            SetBitmapDimensionEx((HBITMAP)o, x, y, &sz);
        }
        wow32_setret(f, ((DWORD)(WORD)(short)sz.cy << 16)
                        | (DWORD)(WORD)(short)sz.cx);
        return 1;
    }

    /* ── ★ 0x9a GetNearestColor(hDC, crColor) / 0x172 GetNearestPaletteIndex ─
       ⚠ THE FIRST ARGUMENT IS A DC IN ONE AND A PALETTE IN THE OTHER, and this
         host has never made a palette -- Paint's `CreatePalette` reaches a stub
         no run has yet named. So the palette arm answers 0 and SAYS SO, rather
         than pretending an index; a wrong index is a wrong colour with no way to
         tell afterwards that it was invented. */
    case WOWGDI_GETNEARESTCOLOR:
    case WOWGDI_GETNEARESTPALIDX: {
        int   isidx = (f->id == WOWGDI_GETNEARESTPALIDX);
        WORD  h   = wow32_argw(f, COL_ARG_HDC);
        DWORD col = wow32_argd(f, COL_ARG_COLOR);
        int   kind = -1;
        HGDIOBJ o = wowgdi_h32(h, &kind);
        int   k = 0;
        wu_puts(note, notecap, &k, isidx ? "GetNearestPaletteIndex(0x"
                                         : "GetNearestColor(0x");
        wu_puthex(note, notecap, &k, h, 4);
        wu_puts(note, notecap, &k, ", 0x");
        wu_puthex(note, notecap, &k, col, 8);
        wu_puts(note, notecap, &k, ")");
        /* ⚠ Session 46 answered this 0 because nothing could make a palette.
             `CreatePalette` (0x168) is serviced now, so it is a real lookup --
             and a handle that is still not ours is still refused rather than
             answered with an invented index. */
        if (isidx) {
            if (!o || (kind != WOWGDI_KIND_OBJ && kind != WOWGDI_KIND_STOCK)) {
                wu_puts(note, notecap, &k, " -- ★ NOT ONE OF OUR PALETTE TOKENS;"
                                           " answered 0 rather than inventing an"
                                           " index");
                wow32_setret(f, 0);
                return 1;
            }
            wow32_setret(f, (DWORD)GetNearestPaletteIndex((HPALETTE)o, (COLORREF)col));
            return 1;
        }
        if (!o || (kind != WOWGDI_KIND_DC && kind != WOWGDI_KIND_WINDC)) {
            wu_puts(note, notecap, &k, " -- ★ NOT ONE OF OUR DC TOKENS;"
                                       " answered the colour unchanged");
            wow32_setret(f, col);
            return 1;
        }
        col = (DWORD)GetNearestColor((HDC)o, (COLORREF)col);
        wu_puts(note, notecap, &k, " = 0x");
        wu_puthex(note, notecap, &k, col, 8);
        wow32_setret(f, col);
        return 1;
    }

    /* ── ★★ 0x21 TextOut / 0x5b GetTextExtent -- the same (string, count). ────
       ⚠ THE STRING IS COUNTED, NOT TERMINATED. Both take an explicit length and
         a Win16 program is entitled to hand over a fragment of a larger buffer,
         so this must NOT stop at a NUL the way `wow32_argstr` does -- it copies
         exactly the count it was given, bounded by the scratch buffer. */
    case WOWGDI_TEXTOUT:
    case WOWGDI_GETTEXTEXTENT: {
        int  isout = (f->id == WOWGDI_TEXTOUT);
        WORD hdc = wow32_argw(f, isout ? TO_ARG_HDC   : TE_ARG_HDC);
        WORD n   = wow32_argw(f, isout ? TO_ARG_COUNT : TE_ARG_COUNT);
        volatile BYTE *s = wow32_argptr(f, isout ? TO_ARG_STR : TE_ARG_STR);
        int  x = isout ? (int)(short)wow32_argw(f, TO_ARG_X) : 0;
        int  y = isout ? (int)(short)wow32_argw(f, TO_ARG_Y) : 0;
        int  kind = -1;
        HGDIOBJ o = wowgdi_h32(hdc, &kind);
        char buf[512];
        int  k = 0, i, cnt = (int)n;
        SIZE sz;
        wu_puts(note, notecap, &k, isout ? "TextOut(0x" : "GetTextExtent(0x");
        wu_puthex(note, notecap, &k, hdc, 4);
        if (isout) {
            wu_puts(note, notecap, &k, ", ");
            wu_puthex(note, notecap, &k, (DWORD)x, 4);
            wu_puts(note, notecap, &k, ",");
            wu_puthex(note, notecap, &k, (DWORD)y, 4);
        }
        wu_puts(note, notecap, &k, ", ");
        wu_puthex(note, notecap, &k, (DWORD)n, 4);
        wu_puts(note, notecap, &k, " chars)");
        if (!o || (kind != WOWGDI_KIND_DC && kind != WOWGDI_KIND_WINDC) || !s) {
            wu_puts(note, notecap, &k, " -- ★ NOT ONE OF OUR DC TOKENS, or no"
                                       " string; answered 0");
            wow32_setret(f, 0);
            return 1;
        }
        if (cnt < 0) cnt = 0;
        if (cnt > (int)sizeof buf) cnt = (int)sizeof buf;
        for (i = 0; i < cnt; ++i) buf[i] = (char)s[i];
        if (isout) {
            int r = TextOutA((HDC)o, x, y, buf, cnt) ? 1 : 0;
            wu_puts(note, notecap, &k, r ? " -> drawn" : " -- ★ the OS refused it");
            wow32_setret(f, (DWORD)r);
            return 1;
        }
        sz.cx = sz.cy = 0;
        GetTextExtentPoint32A((HDC)o, buf, cnt, &sz);
        wu_puts(note, notecap, &k, " = ");
        wu_puthex(note, notecap, &k, (DWORD)sz.cx, 4);
        wu_puts(note, notecap, &k, "x");
        wu_puthex(note, notecap, &k, (DWORD)sz.cy, 4);
        wow32_setret(f, ((DWORD)(WORD)sz.cy << 16) | (DWORD)(WORD)sz.cx);
        return 1;
    }

    /* ── ★★ 0x5d GetTextMetrics -- 31 bytes of `short`, layout read off
         NOTEPAD.EXE itself (see the note by the defines). ─────────────────────
       ⚠ THE WHOLE STRUCTURE IS WRITTEN, INCLUDING THE FIELDS NOTEPAD DOES NOT
         READ. A guest that finds a stale byte at tmPitchAndFamily picks a font
         for reasons nothing in the log explains. */
    case WOWGDI_GETTEXTMETRICS: {
        WORD hdc = wow32_argw(f, TM_ARG_HDC);
        volatile BYTE *dst = wow32_argptr(f, TM_ARG_BUF);
        int  kind = -1;
        HGDIOBJ o = wowgdi_h32(hdc, &kind);
        TEXTMETRICA tm;
        int k = 0, i;
        BYTE b[WOW16_TEXTMETRIC_CB];
        wu_puts(note, notecap, &k, "GetTextMetrics(0x");
        wu_puthex(note, notecap, &k, hdc, 4);
        wu_puts(note, notecap, &k, ")");
        if (!o || (kind != WOWGDI_KIND_DC && kind != WOWGDI_KIND_WINDC) || !dst) {
            wu_puts(note, notecap, &k, " -- ★ NOT ONE OF OUR DC TOKENS, or no"
                                       " buffer; answered 0");
            wow32_setret(f, 0);
            return 1;
        }
        if (!GetTextMetricsA((HDC)o, &tm)) {
            wu_puts(note, notecap, &k, " -- ★ the OS refused it; answered 0");
            wow32_setret(f, 0);
            return 1;
        }
        for (i = 0; i < (int)sizeof b; ++i) b[i] = 0;
        wow32_pokew(b +  0, (WORD)(short)tm.tmHeight);
        wow32_pokew(b +  2, (WORD)(short)tm.tmAscent);
        wow32_pokew(b +  4, (WORD)(short)tm.tmDescent);
        wow32_pokew(b +  6, (WORD)(short)tm.tmInternalLeading);
        wow32_pokew(b +  8, (WORD)(short)tm.tmExternalLeading);
        wow32_pokew(b + 10, (WORD)(short)tm.tmAveCharWidth);
        wow32_pokew(b + 12, (WORD)(short)tm.tmMaxCharWidth);
        wow32_pokew(b + 14, (WORD)(short)tm.tmWeight);
        wow32_pokew(b + 16, (WORD)(short)tm.tmOverhang);
        wow32_pokew(b + 18, (WORD)(short)tm.tmDigitizedAspectX);
        wow32_pokew(b + 20, (WORD)(short)tm.tmDigitizedAspectY);
        b[22] = tm.tmFirstChar;       b[23] = tm.tmLastChar;
        b[24] = tm.tmDefaultChar;     b[25] = tm.tmBreakChar;
        b[26] = tm.tmItalic;          b[27] = tm.tmUnderlined;
        b[28] = tm.tmStruckOut;       b[29] = tm.tmPitchAndFamily;
        b[30] = tm.tmCharSet;
        for (i = 0; i < (int)sizeof b; ++i) dst[i] = b[i];
        wu_puts(note, notecap, &k, " h=");
        wu_puthex(note, notecap, &k, (DWORD)(WORD)(short)tm.tmHeight, 4);
        wu_puts(note, notecap, &k, " extlead=");
        wu_puthex(note, notecap, &k, (DWORD)(WORD)(short)tm.tmExternalLeading, 4);
        wu_puts(note, notecap, &k, " avew=");
        wu_puthex(note, notecap, &k, (DWORD)(WORD)(short)tm.tmAveCharWidth, 4);
        wow32_setret(f, 1);
        return 1;
    }

    /* ── ★ 0x39 CreateFontIndirect -- the Win16 LOGFONT, read the other way. ──
         The 50-byte layout is the one `GetObject` already writes: 18 bytes of
         header and a 32-byte face name. This is the same table used backwards,
         which is why there is no second reading of it to get wrong. */
    case WOWGDI_CREATEFONTIND: {
        volatile BYTE *p = wow32_argptr(f, 0);
        LOGFONTA lf;
        int k = 0, i;
        HFONT fn;
        WORD tok;
        wu_puts(note, notecap, &k, "CreateFontIndirect(");
        if (!p) {
            wu_puts(note, notecap, &k, "NULL) -- ★ no LOGFONT; answered 0");
            wow32_setret(f, 0);
            return 1;
        }
        for (i = 0; i < (int)sizeof lf; ++i) ((BYTE *)&lf)[i] = 0;
        lf.lfHeight     = (LONG)(short)wow32_peekw(p + 0);
        lf.lfWidth      = (LONG)(short)wow32_peekw(p + 2);
        lf.lfEscapement = (LONG)(short)wow32_peekw(p + 4);
        lf.lfOrientation= (LONG)(short)wow32_peekw(p + 6);
        lf.lfWeight     = (LONG)(short)wow32_peekw(p + 8);
        lf.lfItalic     = p[10]; lf.lfUnderline     = p[11];
        lf.lfStrikeOut  = p[12]; lf.lfCharSet       = p[13];
        lf.lfOutPrecision = p[14]; lf.lfClipPrecision = p[15];
        lf.lfQuality      = p[16]; lf.lfPitchAndFamily = p[17];
        for (i = 0; i < 31; ++i) {
            BYTE c = p[18 + i];
            lf.lfFaceName[i] = (CHAR)c;
            if (!c) break;
        }
        lf.lfFaceName[31] = 0;
        wu_putq(note, notecap, &k, lf.lfFaceName);
        wu_puts(note, notecap, &k, " h=");
        wu_puthex(note, notecap, &k, (DWORD)(WORD)(short)lf.lfHeight, 4);
        wu_puts(note, notecap, &k, ")");
        fn = CreateFontIndirectA(&lf);
        tok = fn ? wowgdi_h16((HGDIOBJ)fn, WOWGDI_KIND_OBJ) : 0;
        if (!tok) {
            if (fn) DeleteObject((HGDIOBJ)fn);
            wu_puts(note, notecap, &k, " -- ★ no font (or the token map is full);"
                                       " answered 0");
            wow32_setret(f, 0);
            return 1;
        }
        wu_puts(note, notecap, &k, " -> font token 0x");
        wu_puthex(note, notecap, &k, tok, 4);
        wow32_setret(f, tok);
        return 1;
    }

    /* ── ★ 0x4a GetBitmapBits / 0x6a SetBitmapBits -- PBRUSH.DLL's own pair. ─
       ⚠ THE COUNT IS A DWORD AND IT IS THE GUEST'S. It is passed through
         unchanged and Windows bounds it against the bitmap; clamping it here
         would silently truncate an image the guest believes it wrote. What IS
         checked is that the far pointer resolves, because a bad selector is our
         problem rather than the guest's. */
    case WOWGDI_GETBITMAPBITS:
    case WOWGDI_SETBITMAPBITS: {
        int   isget = (f->id == WOWGDI_GETBITMAPBITS);
        WORD  hbm   = wow32_argw(f, BB2_ARG_HBM);
        DWORD cnt   = wow32_argd(f, BB2_ARG_COUNT);
        volatile BYTE *bits = wow32_argptr(f, BB2_ARG_BITS);
        int   kind = -1;
        HGDIOBJ o = wowgdi_h32(hbm, &kind);
        int   k = 0;
        LONG  got;
        wu_puts(note, notecap, &k, isget ? "GetBitmapBits(0x" : "SetBitmapBits(0x");
        wu_puthex(note, notecap, &k, hbm, 4);
        wu_puts(note, notecap, &k, ", ");
        wu_puthex(note, notecap, &k, cnt, 8);
        wu_puts(note, notecap, &k, " bytes)");
        if (!o || (kind != WOWGDI_KIND_OBJ && kind != WOWGDI_KIND_STOCK) || !bits) {
            wu_puts(note, notecap, &k, " -- ★ NOT ONE OF OUR BITMAP TOKENS, or no"
                                       " buffer; answered 0");
            wow32_setret(f, 0);
            return 1;
        }
        got = isget ? GetBitmapBits((HBITMAP)o, (LONG)cnt, (LPVOID)(ULONG_PTR)bits)
                    : SetBitmapBits((HBITMAP)o, (DWORD)cnt, (const void *)(ULONG_PTR)bits);
        wu_puts(note, notecap, &k, " -> ");
        wu_puthex(note, notecap, &k, (DWORD)got, 8);
        wow32_setret(f, (DWORD)got);
        return 1;
    }

    /* ── ★★★★★ 0x1b8 SetDIBits / 0x1b9 GetDIBits / 0x1b7 StretchDIBits ───────
         **This is what `File > Save As` needs.** MS Paint shows the common
         dialog, gets a filename back, and then has to turn its off-screen
         bitmap into the rows of a .BMP -- which is `GetDIBits`, and it was
         answered with the harness sentinel.
       ★ NO CONVERSION IS NEEDED AND THAT IS A READING, NOT AN ASSUMPTION. A
         `BITMAPINFOHEADER` is 40 bytes with the same fields in the same order in
         both worlds (Win16 wrote the format Win32 inherited), the colour table
         that follows it is RGBQUADs either way, and the bits are bytes. Both
         pointers are into the VDM's own memory, which this host can address
         directly, so they go to Win32 as they are.
       ⚠ `lpvBits == NULL` IS A QUERY, not an error: GetDIBits with a null bits
         pointer fills in the header and returns the scan-line count, and a guest
         uses that to size its buffer. Refusing it would break the call BEFORE
         the one that matters. */
    case WOWGDI_SETDIBITS:
    case WOWGDI_GETDIBITS: {
        int   isget = (f->id == WOWGDI_GETDIBITS);
        WORD  hdc   = wow32_argw(f, DIB_ARG_HDC);
        WORD  hbm   = wow32_argw(f, DIB_ARG_HBM);
        WORD  start = wow32_argw(f, DIB_ARG_START);
        WORD  lines = wow32_argw(f, DIB_ARG_LINES);
        WORD  usage = wow32_argw(f, DIB_ARG_USAGE);
        volatile BYTE *bits = wow32_argptr(f, DIB_ARG_BITS);
        volatile BYTE *bmi  = wow32_argptr(f, DIB_ARG_BMI);
        int   dk = -1, bk = -1;
        HGDIOBJ d = wowgdi_h32(hdc, &dk);
        HGDIOBJ b = wowgdi_h32(hbm, &bk);
        int   k = 0, r;
        wu_puts(note, notecap, &k, isget ? "GetDIBits(dc 0x" : "SetDIBits(dc 0x");
        wu_puthex(note, notecap, &k, hdc, 4);
        wu_puts(note, notecap, &k, ", bm 0x");
        wu_puthex(note, notecap, &k, hbm, 4);
        wu_puts(note, notecap, &k, ", scan ");
        wu_puthex(note, notecap, &k, (DWORD)start, 4);
        wu_puts(note, notecap, &k, "+");
        wu_puthex(note, notecap, &k, (DWORD)lines, 4);
        wu_puts(note, notecap, &k, bits ? ")" : ", bits=NULL = a size QUERY)");
        if (!d || (dk != WOWGDI_KIND_DC && dk != WOWGDI_KIND_WINDC)
               || !b || (bk != WOWGDI_KIND_OBJ && bk != WOWGDI_KIND_STOCK) || !bmi) {
            wu_puts(note, notecap, &k, " -- ★ NOT OUR DC/BITMAP TOKENS, or no"
                                       " BITMAPINFO; answered 0");
            wow32_setret(f, 0);
            return 1;
        }
        r = isget
            ? GetDIBits((HDC)d, (HBITMAP)b, start, lines,
                        (LPVOID)(ULONG_PTR)bits, (BITMAPINFO *)(ULONG_PTR)bmi, usage)
            : SetDIBits((HDC)d, (HBITMAP)b, start, lines,
                        (const void *)(ULONG_PTR)bits, (const BITMAPINFO *)(ULONG_PTR)bmi,
                        usage);
        wu_puts(note, notecap, &k, " -> ");
        wu_puthex(note, notecap, &k, (DWORD)r, 4);
        wu_puts(note, notecap, &k, " scan lines");
        wow32_setret(f, (DWORD)(WORD)r);
        return 1;
    }

    /* ── ★ 0x168 CreatePalette -- and it is what makes SelectPalette real. ────
         A `LOGPALETTE` is `{WORD palVersion; WORD palNumEntries; PALETTEENTRY
         palPalEntry[];}` with a 4-byte entry, and every field is the same width
         in both worlds, so the guest's own structure goes to Win32 unconverted.
       ⚠ Session 46 answered `GetNearestPaletteIndex` with 0 "because this host
         has no palette objects". It has now, so that refusal is a real lookup
         again -- which is the point of implementing the producer before the
         consumers. */
    case WOWGDI_CREATEPALETTE: {
        volatile BYTE *p = wow32_argptr(f, 0);
        int k = 0;
        HPALETTE pal;
        WORD tok;
        wu_puts(note, notecap, &k, "CreatePalette(");
        if (!p) {
            wu_puts(note, notecap, &k, "NULL) -- ★ no LOGPALETTE; answered 0");
            wow32_setret(f, 0);
            return 1;
        }
        wu_puthex(note, notecap, &k, (DWORD)wow32_peekw(p + 2), 4);
        wu_puts(note, notecap, &k, " entries)");
        pal = CreatePalette((const LOGPALETTE *)(ULONG_PTR)p);
        tok = pal ? wowgdi_h16((HGDIOBJ)pal, WOWGDI_KIND_OBJ) : 0;
        if (!tok) {
            if (pal) DeleteObject((HGDIOBJ)pal);
            wu_puts(note, notecap, &k, " -- ★ the OS refused it (or the token map"
                                       " is full); answered 0");
            wow32_setret(f, 0);
            return 1;
        }
        wu_puts(note, notecap, &k, " -> palette token 0x");
        wu_puthex(note, notecap, &k, tok, 4);
        wow32_setret(f, tok);
        return 1;
    }

    case WOWGDI_STRETCHDIBITS: {
        WORD  hdc = wow32_argw(f, SDI_ARG_HDC);
        int   dx = (int)(short)wow32_argw(f, SDI_ARG_DSTX);
        int   dy = (int)(short)wow32_argw(f, SDI_ARG_DSTY);
        int   dw = (int)(short)wow32_argw(f, SDI_ARG_DSTW);
        int   dh = (int)(short)wow32_argw(f, SDI_ARG_DSTH);
        int   sx = (int)(short)wow32_argw(f, SDI_ARG_SRCX);
        int   sy = (int)(short)wow32_argw(f, SDI_ARG_SRCY);
        int   sw = (int)(short)wow32_argw(f, SDI_ARG_SRCW);
        int   sh = (int)(short)wow32_argw(f, SDI_ARG_SRCH);
        WORD  usage = wow32_argw(f, SDI_ARG_USAGE);
        DWORD rop = wow32_argd(f, SDI_ARG_ROP);
        volatile BYTE *bits = wow32_argptr(f, SDI_ARG_BITS);
        volatile BYTE *bmi  = wow32_argptr(f, SDI_ARG_BMI);
        int   kind = -1;
        HGDIOBJ o = wowgdi_h32(hdc, &kind);
        int   k = 0, r;
        wu_puts(note, notecap, &k, "StretchDIBits(0x");
        wu_puthex(note, notecap, &k, hdc, 4);
        wu_puts(note, notecap, &k, " dst(");
        wu_puthex(note, notecap, &k, (DWORD)dx, 4);
        wu_puts(note, notecap, &k, ",");
        wu_puthex(note, notecap, &k, (DWORD)dy, 4);
        wu_puts(note, notecap, &k, ") ");
        wu_puthex(note, notecap, &k, (DWORD)dw, 4);
        wu_puts(note, notecap, &k, "x");
        wu_puthex(note, notecap, &k, (DWORD)dh, 4);
        wu_puts(note, notecap, &k, " <- src ");
        wu_puthex(note, notecap, &k, (DWORD)sw, 4);
        wu_puts(note, notecap, &k, "x");
        wu_puthex(note, notecap, &k, (DWORD)sh, 4);
        wu_puts(note, notecap, &k, " rop=0x");
        wu_puthex(note, notecap, &k, rop, 8);
        wu_puts(note, notecap, &k, ")");
        if (!o || (kind != WOWGDI_KIND_DC && kind != WOWGDI_KIND_WINDC)
               || !bmi || !bits) {
            wu_puts(note, notecap, &k, " -- ★ NOT ONE OF OUR DC TOKENS, or no"
                                       " bits/BITMAPINFO; answered 0");
            wow32_setret(f, 0);
            return 1;
        }
        r = StretchDIBits((HDC)o, dx, dy, dw, dh, sx, sy, sw, sh,
                          (const void *)(ULONG_PTR)bits,
                          (const BITMAPINFO *)(ULONG_PTR)bmi, usage, rop);
        wu_puts(note, notecap, &k, " -> ");
        wu_puthex(note, notecap, &k, (DWORD)r, 4);
        wow32_setret(f, (DWORD)(WORD)r);
        return 1;
    }

    /* ── CreateDIBitmap: a DIB in the guest's memory becomes a real DDB. ──────
         Like SetDIBits/GetDIBits above, the BITMAPINFOHEADER is 40 bytes and
         byte-identical in both worlds and both the header and the bits live in
         guest memory this host can address, so nothing is converted -- only the
         DC token resolved and the new bitmap tokenised on the way out.
       ⚠ `dwInit` DECIDES WHETHER lpbInit IS READ AT ALL (CBM_INIT = 4). With it
         clear, Win32 must be handed NULLs: passing a pointer alongside a zero
         flag is how a caller ends up with an uninitialised bitmap that looks
         initialised. */
    case WOWGDI_CREATEDIBITMAP: {
        WORD  hdc   = wow32_argw(f, CDIB_ARG_HDC);
        volatile BYTE *bmih = wow32_argptr(f, CDIB_ARG_BMIH);
        DWORD init  = wow32_argd(f, CDIB_ARG_INIT);
        volatile BYTE *bits = wow32_argptr(f, CDIB_ARG_BITS);
        volatile BYTE *bmi  = wow32_argptr(f, CDIB_ARG_BMI);
        WORD  usage = wow32_argw(f, CDIB_ARG_USAGE);
        int   kind = -1;
        HGDIOBJ o = wowgdi_h32(hdc, &kind);
        HBITMAP bm;
        WORD  tok;
        int   k = 0;
        wu_puts(note, notecap, &k, "CreateDIBitmap(dc 0x");
        wu_puthex(note, notecap, &k, hdc, 4);
        wu_puts(note, notecap, &k, ", init 0x");
        wu_puthex(note, notecap, &k, init, 8);
        wu_puts(note, notecap, &k, ")");
        if (!o || (kind != WOWGDI_KIND_DC && kind != WOWGDI_KIND_WINDC) || !bmih) {
            wu_puts(note, notecap, &k, " -- ★ NOT ONE OF OUR DC TOKENS, or no"
                                       " BITMAPINFOHEADER; answered 0");
            wow32_setret(f, 0);
            return 1;
        }
        bm = CreateDIBitmap((HDC)o,
                            (const BITMAPINFOHEADER *)(ULONG_PTR)bmih, init,
                            (init && bits) ? (const void *)(ULONG_PTR)bits : NULL,
                            (init && bmi)  ? (const BITMAPINFO *)(ULONG_PTR)bmi : NULL,
                            usage);
        if (!bm) {
            wu_puts(note, notecap, &k, " -- ★ GDI REFUSED; answered 0");
            wow32_setret(f, 0);
            return 1;
        }
        tok = wowgdi_h16((HGDIOBJ)bm, WOWGDI_KIND_OBJ);
        if (!tok) {
            /* ⚠ NO TOKEN LEFT MEANS THE BITMAP LEAKS IF WE JUST RETURN 0 -- the
                 guest never learns of it, so nobody will ever DeleteObject it.
                 Destroy it here and fail honestly. */
            DeleteObject(bm);
            wu_puts(note, notecap, &k, " -- ★ HANDLE MAP FULL; destroyed and"
                                       " answered 0");
            wow32_setret(f, 0);
            return 1;
        }
        wu_puts(note, notecap, &k, " -> 0x");
        wu_puthex(note, notecap, &k, tok, 4);
        wow32_setret(f, (DWORD)tok);
        return 1;
    }

    case WOWGDI_SETDIBITSTODEV: {
        WORD  hdc = wow32_argw(f, SDD_ARG_HDC);
        int   dx = (int)(short)wow32_argw(f, SDD_ARG_DSTX);
        int   dy = (int)(short)wow32_argw(f, SDD_ARG_DSTY);
        /* ⚠ w/h ARE `WORD`s IN THE Win16 PROTOTYPE, not ints -- a bitmap is never
             negative-width, and sign-extending one over 32767 would turn a blit
             into a negative and draw nothing. Widened UNSIGNED, unlike the
             coordinates either side of them, which are genuinely signed. */
        DWORD w  = (DWORD)wow32_argw(f, SDD_ARG_W);
        DWORD h  = (DWORD)wow32_argw(f, SDD_ARG_H);
        int   sx = (int)(short)wow32_argw(f, SDD_ARG_SRCX);
        int   sy = (int)(short)wow32_argw(f, SDD_ARG_SRCY);
        WORD  start  = wow32_argw(f, SDD_ARG_START);
        WORD  nscans = wow32_argw(f, SDD_ARG_NSCANS);
        volatile BYTE *bits = wow32_argptr(f, SDD_ARG_BITS);
        volatile BYTE *bmi  = wow32_argptr(f, SDD_ARG_BMI);
        WORD  usage = wow32_argw(f, SDD_ARG_USAGE);
        int   kind = -1;
        HGDIOBJ o = wowgdi_h32(hdc, &kind);
        int   k = 0, r;
        wu_puts(note, notecap, &k, "SetDIBitsToDevice(dc 0x");
        wu_puthex(note, notecap, &k, hdc, 4);
        wu_puts(note, notecap, &k, " dst(");
        wu_puthex(note, notecap, &k, (DWORD)dx, 4);
        wu_puts(note, notecap, &k, ",");
        wu_puthex(note, notecap, &k, (DWORD)dy, 4);
        wu_puts(note, notecap, &k, ") ");
        wu_puthex(note, notecap, &k, (DWORD)w, 4);
        wu_puts(note, notecap, &k, "x");
        wu_puthex(note, notecap, &k, (DWORD)h, 4);
        wu_puts(note, notecap, &k, ")");
        if (!o || (kind != WOWGDI_KIND_DC && kind != WOWGDI_KIND_WINDC)
               || !bmi || !bits) {
            wu_puts(note, notecap, &k, " -- ★ NOT ONE OF OUR DC TOKENS, or no"
                                       " bits/BITMAPINFO; answered 0");
            wow32_setret(f, 0);
            return 1;
        }
        r = SetDIBitsToDevice((HDC)o, dx, dy, w, h,
                              sx, sy, start, nscans,
                              (const void *)(ULONG_PTR)bits,
                              (const BITMAPINFO *)(ULONG_PTR)bmi, usage);
        wu_puts(note, notecap, &k, " -> ");
        wu_puthex(note, notecap, &k, (DWORD)r, 4);
        wow32_setret(f, (DWORD)(WORD)r);
        return 1;
    }

    default:
        return 0;
    }
}

#endif /* WOWGDI_H */
