#ifndef NTVDMEX_WOWGDI_H
#define NTVDMEX_WOWGDI_H
#include "wowconv.h"   /* the Win16/Win32 semantic deltas, pinned by tests/unit/wow_test.c */
/*
 * wowgdi.h -- ★★ GDI.EXE's OWN ID SPACE.  GH #128, session 44.
 *
 * A SEVENTH id space, and the first one opened for its own sake rather than
 * because a guest stopped on it: `tools/ne/neneeds.py` says NOTEPAD.EXE reaches
 * three of GDI's thunks directly, and MS PAINT reaches **41**. So this file is
 * where the north star's other half begins, and the three below are its first
 * three lines rather than the whole of it.
 *
 * ── THE IDS ─────────────────────────────────────────────────────────────────
 * Export ordinal against the id, argument bytes and return-stub offset the call
 * arrives with:
 *      68 DELETEDC       id 0x44   2 args  retstub 0x033a
 *      69 DELETEOBJECT   id 0x45   2 args  retstub 0x0354
 *      80 GETDEVICECAPS  id 0x50   4 args  retstub 0x05de
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

#define WOWGDI_GDC_ARG_INDEX   0
#define WOWGDI_GDC_ARG_HDC     2
/* The capability index Win32 inherited from Win16 unchanged -- "how many entries
   in this device's colour table". It is the one index whose Win32 answer a Win16
   caller cannot read; see the handler. */
#define WOWGDI_CAP_NUMCOLORS  24
#define WOWGDI_DOBJ_ARG_HANDLE 0

/* ── ★★★ THE PRODUCERS -- WHAT MS PAINT ASKS FOR ONCE IT HAS A WINDOW. ───────
     With `GetDC` in place (it is USER's call, not GDI's -- see wowuser.h) Paint
     gets as far as needing a canvas, and says so in its own words when it does
     not get one: "Not enough memory to edit image."

   ★★ 0x57 IS `GetStockObject`, AND ONLY A RUN COULD HAVE SAID SO. Its export
     (GDI ordinal 87) is `native16` -- 16-bit code that only reaches the BOP from
     inside its own body -- so `neneeds.py` classifies the import as free. Session
     44 wrote down that its id would have to come from a run; this is that run, 24
     calls of it, arriving as id 0x57 with 2 argument bytes -- and 0x57 = 87, the
     ids running with the ordinals as they do either side of it (0x58 =
     GDI.88 GETSTRETCHBLTMODE).

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
#define WOWGDI_COL_ARG_COLOR   0
#define WOWGDI_COL_ARG_HDC     4

#define WOWGDI_SETROP2          0x0004
#define WOWGDI_SETSTRETCHMODE   0x0007
#define WOWGDI_MODE_ARG_MODE   0
#define WOWGDI_MODE_ARG_HDC    2

#define WOWGDI_SETWINDOWORG     0x000b
#define WOWGDI_ORG_ARG_Y       0
#define WOWGDI_ORG_ARG_X       2
#define WOWGDI_ORG_ARG_HDC     4

#define WOWGDI_RECTANGLE        0x001b
#define WOWGDI_RC_ARG_BOTTOM   0
#define WOWGDI_RC_ARG_RIGHT    2
#define WOWGDI_RC_ARG_TOP      4
#define WOWGDI_RC_ARG_LEFT     6
#define WOWGDI_RC_ARG_HDC      8

#define WOWGDI_SAVEDC           0x001e
#define WOWGDI_SDC_ARG_HDC     0
#define WOWGDI_RESTOREDC        0x0027
#define WOWGDI_RDC2_ARG_LEVEL  0
#define WOWGDI_RDC2_ARG_HDC    2

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
#define WOWGDI_SB_ARG_ROP      0        /* DWORD */
#define WOWGDI_SB_ARG_SRCH     4
#define WOWGDI_SB_ARG_SRCW     6
#define WOWGDI_SB_ARG_SRCY     8
#define WOWGDI_SB_ARG_SRCX    10
#define WOWGDI_SB_ARG_SRCDC   12
#define WOWGDI_SB_ARG_DSTH    14
#define WOWGDI_SB_ARG_DSTW    16
#define WOWGDI_SB_ARG_DSTY    18
#define WOWGDI_SB_ARG_DSTX    20
#define WOWGDI_SB_ARG_DSTDC   22

#define WOWGDI_BITBLT           0x0022
#define WOWGDI_BB_ARG_ROP      0        /* DWORD */
#define WOWGDI_BB_ARG_SRCY     4
#define WOWGDI_BB_ARG_SRCX     6
#define WOWGDI_BB_ARG_SRCDC    8
#define WOWGDI_BB_ARG_HEIGHT  10
#define WOWGDI_BB_ARG_WIDTH   12
#define WOWGDI_BB_ARG_Y       14
#define WOWGDI_BB_ARG_X       16
#define WOWGDI_BB_ARG_DSTDC   18

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
#define WOWGDI_ONE_ARG_HANDLE  0

#define WOWGDI_PTVISIBLE        0x0067
#define WOWGDI_PV_ARG_Y        0
#define WOWGDI_PV_ARG_X        2
#define WOWGDI_PV_ARG_HDC      4

/* ★ 0x24 Polygon(hDC, lpPoints, nCount) -- GDI ordinal 36, native16, so the id
     came from the run: `(0006 | 38fa 09c6 | 20d8)` is six points, a far LPPOINT
     and one of our DC tokens -- the same block shape as LPtoDP, which is what
     8 argument bytes buys you. ord 35 STRETCHBLT -> 0x23 either side confirms
     the numbering. ⚠ Polyline is ordinal 37 and would be 0x25 by the same
     reading; no run has produced one, so it is not written on that basis. */
#define WOWGDI_POLYGON          0x0024

#define WOWGDI_LPTODP           0x0063
#define WOWGDI_LDP_ARG_COUNT   0
#define WOWGDI_LDP_ARG_POINTS  2
#define WOWGDI_LDP_ARG_HDC     6

#define WOWGDI_CREATEDISCARDBM  0x009c
#define WOWGDI_SETBRUSHORG      0x0094

#define WOWGDI_LINETO           0x0013
#define WOWGDI_MOVETO           0x0014
#define WOWGDI_XY_ARG_Y        0
#define WOWGDI_XY_ARG_X        2
#define WOWGDI_XY_ARG_HDC      4

#define WOWGDI_PATBLT           0x001d
#define WOWGDI_PB_ARG_ROP      0
#define WOWGDI_PB_ARG_HEIGHT   4
#define WOWGDI_PB_ARG_WIDTH    6
#define WOWGDI_PB_ARG_Y        8
#define WOWGDI_PB_ARG_X       10
#define WOWGDI_PB_ARG_HDC     12

#define WOWGDI_SELECTOBJECT     0x002d
#define WOWGDI_SEL_ARG_OBJ     0
#define WOWGDI_SEL_ARG_HDC     2

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
#define WOWGDI_CBM_ARG_BITS    0        /* const void FAR* -- NULL = uninitialised */
#define WOWGDI_CBM_ARG_BPP     4
#define WOWGDI_CBM_ARG_PLANES  6
#define WOWGDI_CBM_ARG_HEIGHT  8
#define WOWGDI_CBM_ARG_WIDTH  10

#define WOWGDI_CREATECOMPATBM   0x0033
#define WOWGDI_CCB_ARG_HEIGHT  0
#define WOWGDI_CCB_ARG_WIDTH   2
#define WOWGDI_CCB_ARG_HDC     4

#define WOWGDI_CREATECOMPATDC   0x0034
#define WOWGDI_CCD_ARG_HDC     0

#define WOWGDI_CREATESOLIDBRUSH 0x0042
#define WOWGDI_CSB_ARG_COLOR   0

#define WOWGDI_GETSTOCKOBJECT   0x0057
#define WOWGDI_GSO_ARG_INDEX   0

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
#define WOWGDI_GOB_ARG_BUF     0
#define WOWGDI_GOB_ARG_COUNT   4
#define WOWGDI_GOB_ARG_HANDLE  6

#define WOWGDI_BITMAP16_SIZE    14
#define WOWGDI_LOGFONT16_SIZE   50
#define WOWGDI_LOGPEN16_SIZE    10
#define WOWGDI_LOGBRUSH16_SIZE   8

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
#define WOWGDI_CDC_ARG_INITDATA 0
#define WOWGDI_CDC_ARG_OUTPUT   4
#define WOWGDI_CDC_ARG_DEVICE   8
#define WOWGDI_CDC_ARG_DRIVER  12

/* ── ★★★★★ THE TOOLS THAT DID NOT WORK -- ENUMERATED, NOT GUESSED. ───────────
     "Some drawing functions work, others (like fill) do not" is not a mystery
     once you enumerate what PBRUSH.EXE imports and which of those ordinals reach
     the BOP. Each id and argument byte count below was taken from the call as it
     arrives (e.g. ELLIPSE, ord 24: id 0x18, 10 argument bytes, retstub 0x0417).

   ★★★ THE FILL IS `ExtFloodFill`, GDI ordinal 372, arriving as **id 0x174** with
     12 argument bytes -- and only for a valid fill TYPE (0 or 1); GDI.EXE checks
     that before it comes out to us. That extra step is why a scan for plain
     tail-jump exports calls it `native16` and reports it "free": it is not
     free. Paint's fill tool calls ExtFloodFill TWICE -- once for a solid colour
     and once after `CreatePatternBrush` -- which is why the
     pattern brush is in this batch and not a later one.

   ⚠ A Win16 fill type is Win32's fill type (0 = FLOODFILLBORDER, 1 = SURFACE),
     the same claim the ROPs and COLORREFs already travel on. */
#define WOWGDI_ELLIPSE          0x0018   /* ord 24, 10 args -- RC_ARG_* layout */
#define WOWGDI_EXCLUDECLIPRECT  0x0015   /* ord 21, 10 args -- RC_ARG_* layout */

#define WOWGDI_ROUNDRECT        0x001c   /* ord 28, 14 args                    */
#define WOWGDI_RR_ARG_EH       0
#define WOWGDI_RR_ARG_EW       2
#define WOWGDI_RR_ARG_BOTTOM   4
#define WOWGDI_RR_ARG_RIGHT    6
#define WOWGDI_RR_ARG_TOP      8
#define WOWGDI_RR_ARG_LEFT    10
#define WOWGDI_RR_ARG_HDC     12

#define WOWGDI_EXTFLOODFILL     0x0174   /* ord 372, 12 args -- ★ THE FILL     */
#define WOWGDI_FF_ARG_TYPE     0
#define WOWGDI_FF_ARG_COLOR    2                /* DWORD                              */
#define WOWGDI_FF_ARG_Y        6
#define WOWGDI_FF_ARG_X        8
#define WOWGDI_FF_ARG_HDC     10

#define WOWGDI_CREATEPATTERNBRUSH 0x003c /* ord 60,  2 args (HBITMAP)          */
#define WOWGDI_GETPIXEL         0x0053   /* ord 83,  6 args -- XY_ARG_* layout */
#define WOWGDI_GETBKCOLOR       0x004b   /* ord 75,  2 args -- PBRUSH.DLL's    */
#define WOWGDI_GETROP2          0x0055   /* ord 85,  2 args                    */
#define WOWGDI_UPDATECOLORS     0x016e   /* ord 366, 2 args                    */
/* s89 (#270): the rest of the one-DC getters, named by GDI.EXE's own export
   table (wowmap.py: each DIRECT, 2 argument bytes). GetBkMode was the one the
   Win16 tests caught answering the harness's 0 where the SDK default is OPAQUE. */
#define WOWGDI_GETBKMODE        0x004c   /* ord 76                              */
#define WOWGDI_GETMAPMODE       0x0051   /* ord 81                              */
#define WOWGDI_GETPOLYFILLMODE  0x0054   /* ord 84                              */
#define WOWGDI_GETSTRETCHBLTMODE 0x0058  /* ord 88                              */
#define WOWGDI_GETTEXTCOLOR     0x005a   /* ord 90, a DWORD COLORREF            */

#define WOWGDI_CREATERECTRGN    0x0040   /* ord 64,  8 args                    */
#define WOWGDI_RGN_ARG_BOTTOM  0
#define WOWGDI_RGN_ARG_RIGHT   2
#define WOWGDI_RGN_ARG_TOP     4
#define WOWGDI_RGN_ARG_LEFT    6

#define WOWGDI_SELECTCLIPRGN    0x002c   /* ord 44,  4 args                    */
#define WOWGDI_SCR_ARG_RGN     0
#define WOWGDI_SCR_ARG_HDC     2

/* ── THE REST OF THE REGION API, AND THE TWO TEXT CALLS THAT GO WITH IT.
     (session 55) Every one of these is a Win32 function of the same name and
     the same meaning, so the body is a translation of handles and a call --
     there is nothing to invent, which is exactly why they are worth doing in a
     batch. The argument offsets follow this file's one rule: the FIRST
     parameter sits at the HIGHEST offset, because the block is the pushed
     arguments and the base is the last push. */
#define WOWGDI_COMBINERGN       0x002f   /* ord 47,  8 args                    */
#define WOWGDI_CBR_ARG_MODE    0
#define WOWGDI_CBR_ARG_SRC2    2
#define WOWGDI_CBR_ARG_SRC1    4
#define WOWGDI_CBR_ARG_DEST    6

#define WOWGDI_CREATERECTRGNIND 0x0041   /* ord 65,  4 args                    */
#define WOWGDI_CRRI_ARG_RECT   0                /* far pointer to a Win16 RECT        */

#define WOWGDI_SETRECTRGN       0x00ac   /* ord 172, 10 args                   */
#define WOWGDI_SRR_ARG_BOTTOM  0
#define WOWGDI_SRR_ARG_RIGHT   2
#define WOWGDI_SRR_ARG_TOP     4
#define WOWGDI_SRR_ARG_LEFT    6
#define WOWGDI_SRR_ARG_RGN     8

#define WOWGDI_CREATEPOLYGONRGN 0x003f   /* ord 63,  8 args                    */
#define WOWGDI_CPR_ARG_MODE    0
#define WOWGDI_CPR_ARG_COUNT   2
#define WOWGDI_CPR_ARG_POINTS  4                /* far pointer to an array of POINT16 */

#define WOWGDI_GETCLIPBOX       0x004d   /* ord 77,  6 args                    */
#define WOWGDI_GCX_ARG_RECT    0                /* far pointer, written back          */
#define WOWGDI_GCX_ARG_HDC     4

#define WOWGDI_GETTEXTFACE      0x005c   /* ord 92,  8 args                    */
#define WOWGDI_GTF_ARG_BUF     0                /* far pointer, written back          */
#define WOWGDI_GTF_ARG_COUNT   4
#define WOWGDI_GTF_ARG_HDC     6

#define WOWGDI_SETTEXTJUST      0x000a   /* ord 10,  6 args                    */
#define WOWGDI_STJ_ARG_COUNT   0
#define WOWGDI_STJ_ARG_EXTRA   2
#define WOWGDI_STJ_ARG_HDC     4

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
     ellipse and a red one for the box -- and GDI's export table says ordinal 61
     is CREATEPEN, whose three arguments are 8 bytes. ⚠ Here the id happens to
     EQUAL the ordinal; elsewhere it does not (CreateDC is ordinal 53 and id
     0x99), so each of these was taken from a run rather than assumed.
   ⚠ `neneeds.py` calls all of these "free (16-bit)", because GDI's export is a
     validating wrapper rather than a bare tail-jump, which the scan does not
     follow. That is the `native16` trap again: **the run finds
     them, the static list does not.** */
#define WOWGDI_CREATEPEN        0x003d   /* ord 61,  8 args (style, width, colour) */
#define WOWGDI_CP_ARG_COLOR    0                /* DWORD                                  */
#define WOWGDI_CP_ARG_WIDTH    4
#define WOWGDI_CP_ARG_STYLE    6

#define WOWGDI_CREATEHATCHBRUSH 0x003a   /* ord 58,  6 args (index, colour)         */
#define WOWGDI_CH_ARG_COLOR    0                /* DWORD                                  */
#define WOWGDI_CH_ARG_INDEX    4

#define WOWGDI_SETBKMODE        0x0002   /* ord 2,   4 args -- MODE_ARG_* layout    */
#define WOWGDI_SETMAPMODE       0x0003   /* ord 3,   4 args                         */
#define WOWGDI_SETPOLYFILLMODE  0x0006   /* ord 6,   4 args                         */

#define WOWGDI_SETPIXEL         0x001f   /* ord 31, 10 args                         */
#define WOWGDI_SP_ARG_COLOR    0                /* DWORD                                  */
#define WOWGDI_SP_ARG_Y        4
#define WOWGDI_SP_ARG_X        6
#define WOWGDI_SP_ARG_HDC      8

#define WOWGDI_POLYLINE         0x0025   /* ord 37,  8 args -- LDP_ARG_* layout     */

/* ── ★★★★★ THE SECOND SWEEP: EVERYTHING ELSE THESE TWO PROGRAMS IMPORT. ─────
     Session 47 taught `tools/ne/neneeds.py` to see through GDI's validating
     export wrappers, and the list of what MS Paint and Notepad reach went from
     "41 need us" to **76**. Everything below is on that list, and every id and
     argument count is checked against the call as it arrives -- no id here was
     inferred from an ordinal, and several of them differ from it.
   ★★ THE CORRECTION IT FORCED: **`0x99` IS `CreateIC` (ordinal 153), NOT
     `CreateDC`.** `CreateDC` is ordinal 53 and its id is `0x35`. Session 45 named
     `0x99` from a run, and 153 = 0x99 -- the id tracked the ordinal after all,
     just not the ordinal we thought. It went unnoticed because an information
     context and a device context answer every query identically, so servicing an
     IC as a DC works and only the NAME was wrong. ⇒ both are answered here, and
     the log says which one the guest asked for. */
#define WOWGDI_CREATEDC2        0x0035   /* ord 53,  16 args -- the REAL CreateDC  */

/* ── ★★ THE SHELF, BATCH TWO: what MPLAYER and CHARMAP still want from GDI. ── */
#define WOWGDI_INTERSECTCLIPRECT 0x0016
#define WOWGDI_ICR_ARG_BOTTOM   0
#define WOWGDI_ICR_ARG_RIGHT    2
#define WOWGDI_ICR_ARG_TOP      4
#define WOWGDI_ICR_ARG_LEFT     6
#define WOWGDI_ICR_ARG_HDC      8

#define WOWGDI_RECTVISIBLE       0x0068
#define WOWGDI_RV_ARG_RECT      0
#define WOWGDI_RV_ARG_HDC       4

/* CreateFont's fourteen parameters, LAST push first. CHARMAP's whole job is
   showing one face at a large size, so this is the call it lives or dies on. */
#define WOWGDI_CREATEFONT        0x0038
#define WOWGDI_CF_ARG_FACE      0
#define WOWGDI_CF_ARG_PITCH     4
#define WOWGDI_CF_ARG_QUALITY   6
#define WOWGDI_CF_ARG_CLIPPREC  8
#define WOWGDI_CF_ARG_OUTPREC  10
#define WOWGDI_CF_ARG_CHARSET  12
#define WOWGDI_CF_ARG_STRIKE   14
#define WOWGDI_CF_ARG_UNDER    16
#define WOWGDI_CF_ARG_ITALIC   18
#define WOWGDI_CF_ARG_WEIGHT   20
#define WOWGDI_CF_ARG_ORIENT   22
#define WOWGDI_CF_ARG_ESCAPE   24
#define WOWGDI_CF_ARG_WIDTH    26
#define WOWGDI_CF_ARG_HEIGHT   28

/* ⚠ GetCharWidth writes ONE WORD PER CHARACTER into the guest's buffer. Win32's
     writes an INT each; converting is not optional -- handing back 32-bit values
     would overrun the guest's array by a factor of two, silently, into whatever
     it declared next. */
#define WOWGDI_GETCHARWIDTH      0x015e
#define WOWGDI_GCW_ARG_BUF      0
#define WOWGDI_GCW_ARG_LAST     4
#define WOWGDI_GCW_ARG_FIRST    6
#define WOWGDI_GCW_ARG_HDC      8

/* ── ★★ METAFILES: SOUNDREC's last three, and PACKAGER wants them too. ───────
     A metafile DC is a recording, not a surface: CreateMetaFile hands back a DC
     that remembers calls, CloseMetaFile turns the recording into a metafile
     handle, DeleteMetaFile throws it away. All three are real Win32 calls; the
     only work here is that a DC token and a METAFILE token are different kinds
     and must not be confused -- closing a metafile DC yields an object that is
     NOT a DC, and handing it back under a DC token would let the guest pass it
     to TextOut.
   ⚠ lpszFile is usually NULL (a memory metafile). NULL is not "missing", it is
     the common case, and passing "" instead would try to create a file called
     nothing in the current directory. */
#define WOWGDI_CREATEMETAFILE   0x007d
#define WOWGDI_CMF_ARG_FILE     0
#define WOWGDI_CLOSEMETAFILE    0x007e
#define WOWGDI_DELETEMETAFILE   0x007f
#define WOWGDI_COPYMETAFILE     0x0097
/* s90 (#295): PlayMetaFile(hdc, hmf), GDI.123, 4 bytes reversed: +0 hmf, +2 hdc. */
#define WOWGDI_PLAYMETAFILE     0x007b
#define WOWGDI_PMF_ARG_HMF      0
#define WOWGDI_PMF_ARG_HDC      2
/* #295: EnumMetaFile(hdc, hmf, lpfn, lParam), GDI.175, 12 bytes (the inventory's
   thunk width), reversed: +0 lParam, +4 lpfn, +8 hmf, +10 hdc. A CALLBACK:
   int FAR PASCAL proc(HDC, HANDLETABLE FAR*, METARECORD FAR*, int nObj, LPARAM). */
#define WOWGDI_ENUMMETAFILE     0x00af
#define WOWGDI_EMF_ARG_LPARAM   0
#define WOWGDI_EMF_ARG_PROC     4
#define WOWGDI_EMF_ARG_HMF      8
#define WOWGDI_EMF_ARG_HDC      10
/* #295: PlayMetaFileRecord(hdc, lpht, lpmr, nHandles), GDI.176, 12 bytes,
   reversed: +0 nHandles, +2 lpmr, +6 lpht, +10 hdc. */
#define WOWGDI_PLAYMETAFILEREC  0x00b0
#define WOWGDI_PMFR_ARG_NHANDLES 0
#define WOWGDI_PMFR_ARG_MR       2
#define WOWGDI_PMFR_ARG_HT       6
#define WOWGDI_PMFR_ARG_HDC      10
#define WOWGDI_MF1_ARG_H        0
#define WOWGDI_CPMF_ARG_FILE    0
#define WOWGDI_CPMF_ARG_HMF     4

#define WOWGDI_TEXTOUT          0x0021   /* ord 33,  12 args */
/* ── ★★ ExtTextOut(hdc, x, y, opts, lprc, str, count, lpDx) -- ord 351, 22 args.
     CLOCK's only outstanding GDI service, and the one TextOut cannot stand in
     for: the digital face draws each string CLIPPED and OPAQUE to a rectangle so
     the previous second is erased in the same call, and the analogue face uses
     the same entry with no rect. Substituting TextOut would drop the clip and the
     background fill, which is a wrong picture rather than a missing one.
   ⚠ THE RECT POINTER IS OPTIONAL AND OFTEN NULL, and it must stay null: passing
     an empty RECT with ETO_CLIPPED clips everything away, i.e. draws nothing at
     all -- silently, and looking exactly like "the guest never called us".
   ⚠ lpDx (per-character spacing) is honoured only when the guest supplies it.
     Fabricating even spacing would be inventing a layout the program did not ask
     for; NULL means "use the font's own", which is what Win32 does too. */
#define WOWGDI_EXTTEXTOUT       0x015f
#define WOWGDI_ETO_ARG_DX       0
#define WOWGDI_ETO_ARG_COUNT    4
#define WOWGDI_ETO_ARG_STR      6
#define WOWGDI_ETO_ARG_RECT    10
#define WOWGDI_ETO_ARG_OPTS    14
#define WOWGDI_ETO_ARG_Y       16
#define WOWGDI_ETO_ARG_X       18
#define WOWGDI_ETO_ARG_HDC     20
#define WOWGDI_TO_ARG_COUNT    0
#define WOWGDI_TO_ARG_STR      2                /* far */
#define WOWGDI_TO_ARG_Y        6
#define WOWGDI_TO_ARG_X        8
#define WOWGDI_TO_ARG_HDC     10

#define WOWGDI_GETTEXTEXTENT    0x005b   /* ord 91,   8 args */
#define WOWGDI_TE_ARG_COUNT    0
#define WOWGDI_TE_ARG_STR      2                /* far */
#define WOWGDI_TE_ARG_HDC      6

/* ── ★ 0x5d GetTextMetrics, and the Win16 TEXTMETRIC, CHECKED ON NOTEPAD. ─────
     Its fields are `short` where Win32's are `LONG`, in the same order (the
     documented Win16 layout), and the order is checked by behaviour, not taken
     on trust: Notepad's LINE HEIGHT comes out as tmHeight + tmExternalLeading
     (tm+0 + tm+8) and its TAB STOP as 8 x tmAveCharWidth (tm+10) -- visibly
     wrong on screen if either offset were. That pins the first six fields and
     therefore the whole `short` prefix. Two independent uses, one layout. */
#define WOWGDI_GETTEXTMETRICS   0x005d   /* ord 93,   6 args */
#define WOWGDI_TM_ARG_BUF      0                /* far */
#define WOWGDI_TM_ARG_HDC      4
#define WOWGDI_TEXTMETRIC16_SIZE  31

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
#define WOWGDI_BB2_ARG_BITS    0                /* far */
#define WOWGDI_BB2_ARG_COUNT   4                /* DWORD */
#define WOWGDI_BB2_ARG_HBM     8

/* ── ★★★★★ THE DIB TRIO -- WHAT `File > Save As` DIES ON. ───────────────────
     A `BITMAPINFOHEADER` is 40 bytes and byte-identical in both worlds, and the
     bits and the header both live in guest memory this host can address
     directly, so these are the rare calls that need NO conversion at all -- only
     the handles and the signed 16-bit coordinates.
   ⚠ THE ARGUMENT BLOCK IS THE ONLY PLACE TO GET WRONG, and the counts pin it:
     SetDIBits/GetDIBits declare 18 bytes = 2+2+2+2+4+4+2 and StretchDIBits 32. */
#define WOWGDI_SETDIBITS        0x01b8   /* ord 440, 18 args */
#define WOWGDI_GETDIBITS        0x01b9   /* ord 441, 18 args */
#define WOWGDI_DIB_ARG_USAGE   0
#define WOWGDI_DIB_ARG_BMI     2                /* far */
#define WOWGDI_DIB_ARG_BITS    6                /* far */
#define WOWGDI_DIB_ARG_LINES  10
#define WOWGDI_DIB_ARG_START  12
#define WOWGDI_DIB_ARG_HBM    14
#define WOWGDI_DIB_ARG_HDC    16

#define WOWGDI_STRETCHDIBITS    0x01b7   /* ord 439, 32 args */
#define WOWGDI_SDI_ARG_ROP     0                /* DWORD */
#define WOWGDI_SDI_ARG_USAGE   4
#define WOWGDI_SDI_ARG_BMI     6                /* far */
#define WOWGDI_SDI_ARG_BITS   10                /* far */
#define WOWGDI_SDI_ARG_SRCH   14
#define WOWGDI_SDI_ARG_SRCW   16
#define WOWGDI_SDI_ARG_SRCY   18
#define WOWGDI_SDI_ARG_SRCX   20
#define WOWGDI_SDI_ARG_DSTH   22
#define WOWGDI_SDI_ARG_DSTW   24
#define WOWGDI_SDI_ARG_DSTY   26
#define WOWGDI_SDI_ARG_DSTX   28
#define WOWGDI_SDI_ARG_HDC    30

/* ── ★ MINESWEEPER'S TWO. It keeps its digits, mines and smiley faces as DIBs in
     its own resources and puts them on screen with these; nothing else it draws
     needs GDI at all.
   HBITMAP CreateDIBitmap(HDC, LPBITMAPINFOHEADER, DWORD dwInit, LPSTR lpbInit,
                          LPBITMAPINFO, UINT wUsage)                     = 20 */
#define WOWGDI_CREATEDIBITMAP   0x01ba   /* ord 442, 20 args */
#define WOWGDI_CDIB_ARG_USAGE   0
#define WOWGDI_CDIB_ARG_BMI     2               /* far */
#define WOWGDI_CDIB_ARG_BITS    6               /* far */
#define WOWGDI_CDIB_ARG_INIT   10               /* DWORD */
#define WOWGDI_CDIB_ARG_BMIH   14               /* far */
#define WOWGDI_CDIB_ARG_HDC    18

/* int SetDIBitsToDevice(HDC, int xDest, int yDest, WORD wWidth, WORD wHeight,
                         int XSrc, int YSrc, UINT nStartScan, UINT nNumScans,
                         LPSTR lpBits, LPBITMAPINFO, UINT wUsage)        = 28 */
#define WOWGDI_SETDIBITSTODEV   0x01bb   /* ord 443, 28 args */
#define WOWGDI_SDD_ARG_USAGE    0
#define WOWGDI_SDD_ARG_BMI      2               /* far */
#define WOWGDI_SDD_ARG_BITS     6               /* far */
#define WOWGDI_SDD_ARG_NSCANS  10
#define WOWGDI_SDD_ARG_START   12
#define WOWGDI_SDD_ARG_SRCY    14
#define WOWGDI_SDD_ARG_SRCX    16
#define WOWGDI_SDD_ARG_H       18
#define WOWGDI_SDD_ARG_W       20
#define WOWGDI_SDD_ARG_DSTY    22
#define WOWGDI_SDD_ARG_DSTX    24
#define WOWGDI_SDD_ARG_HDC     26

/* ── ★ int Escape(HDC, int nEscape, int nCount, LPSTR lpInData, LPSTR lpOut) = 14
     The device-driver back door, and on a SCREEN DC the honest answer to almost
     all of it is "this driver does not do that", which Escape spells 0. That is
     not a stub: 0 is the documented in-band answer for an escape the driver does
     not implement, and a caller that asks QUERYESCSUPPORT first -- which is what
     the escape exists for -- gets told before it tries.
   ⚠ A PRINTING PATH WOULD NEED THE REST (STARTDOC/NEWFRAME/ENDDOC and an abort
     procedure that calls back into 16-bit code). This host has no printer DC to
     start one on, so what is NOT done is named here rather than half-built. */
#define WOWGDI_ESCAPE           0x0026   /* ord 38, 14 args */
#define WOWGDI_ESC_ARG_OUT      0               /* far */
#define WOWGDI_ESC_ARG_IN       4               /* far */
#define WOWGDI_ESC_ARG_COUNT    8
#define WOWGDI_ESC_ARG_ESCAPE  10
#define WOWGDI_ESC_ARG_HDC     12
#define WOWGDI_ESC_QUERYESCSUPPORT 8            /* the one escape we can answer fully */

/* ── HBITMAP CreateBitmapIndirect(LPBITMAP) = 4 ──────────────────────────────
     CreateBitmap's arguments, in a structure, and the structure is a Win16 one:
       +0  short bmType        +8  BYTE bmPlanes
       +2  short bmWidth       +9  BYTE bmBitsPixel
       +4  short bmHeight     +10  LPVOID bmBits (far)
       +6  short bmWidthBytes
     = 14 bytes. Win32's BITMAP is 24 with LONGs and a 32-bit pointer, so this is
     read field by field rather than cast -- the same rule as every other shared
     structure in this host. */
#define WOWGDI_CREATEBITMAPINDIRECT 0x0031   /* ord 49, 4 args */
#define WOWGDI_CBI_ARG_BITMAP   0               /* far */
#define CBI_OFF_WIDTH    2
#define CBI_OFF_HEIGHT   4
#define CBI_OFF_WBYTES   6
#define CBI_OFF_PLANES   8
#define CBI_OFF_BPP      9
#define CBI_OFF_BITS    10
#define CBI_BITMAP16_SIZE 14

/* ── BOOL GetCharABCWidths(HDC, UINT first, UINT last, LPABC) = 10 ───────────
   ⚠⚠ THE ABC STRUCTURE IS A DIFFERENT SIZE IN THE TWO WORLDS, and this is the
     `RECT is 8 bytes not 16` trap again: Win16's ABC is `{ int abcA; UINT abcB;
     int abcC; }` = SIX bytes; Win32's is three LONGs = TWELVE. Handing the
     guest's six-byte-per-glyph array to Win32 would overrun it by a factor of
     two before the first character was measured. Converted per glyph in
     wowconv.h, where the battery can pin it. */
#define WOWGDI_GETCHARABCWIDTHS 0x0133   /* ord 307, 10 args */
#define WOWGDI_ABCW_ARG_ABC     0               /* far */
#define WOWGDI_ABCW_ARG_LAST    4
#define WOWGDI_ABCW_ARG_FIRST   6
#define WOWGDI_ABCW_ARG_HDC     8

/* ── UINT GetPaletteEntries(HPALETTE, UINT start, UINT n, LPPALETTEENTRY) = 10
     ★ PALETTEENTRY IS FOUR BYTES IN BOTH -- peRed, peGreen, peBlue, peFlags --
     so this one really is a copy, and saying which structures are identical
     matters as much as saying which are not. */
/* s90 (#297): SetPaletteEntries -- the same frame as GetPaletteEntries (GPE_ARG_*). */
#define WOWGDI_SETPALETTEENTRIES 0x016c  /* ord 364, 10 args */
/* s90 (#295): the packed-DWORD extent getters. Win16 returns MAKELONG(cx, cy); Win32
   has only the *Ex forms. One argument each (an HDC, or an HBITMAP). */
#define WOWGDI_GETVIEWPORTEXT    0x005e  /* ord 94  */
#define WOWGDI_GETWINDOWEXT      0x0060  /* ord 96  */
#define WOWGDI_GETBITMAPDIMENSION 0x00a2 /* ord 162 */
#define WOWGDI_GETPALETTEENTRIES 0x016b  /* ord 363, 10 args */
#define WOWGDI_GPE_ARG_ENTRIES  0               /* far */
#define WOWGDI_GPE_ARG_COUNT    4
#define WOWGDI_GPE_ARG_START    6
#define WOWGDI_GPE_ARG_HPAL     8

/* ── ★ void LineDDA(int x1, int y1, int x2, int y2, FARPROC, LPARAM) = 16 ────
     The only enumeration in GDI whose callback takes NO STRUCTURE: it is called
     with (x, y, lpData) for every point on the line, and the caller does the
     drawing. That is why this one is implemented and EnumFonts/EnumObjects are
     not -- theirs pass LOGFONT/TEXTMETRIC/LOGPEN pointers whose Win16 layouts
     this host has not measured. See the header note in src/wow/wowenum.h.
   ⚠ IT RETURNS NOTHING. A caller reads no value, so there is no hole to revise
     and a callback's answer cannot stop it -- but the chain honours the veto
     anyway, because a Win16 program that returns 0 from a DDA callback expects
     to stop being called, whatever the function's own return says. */
#define WOWGDI_LINEDDA          0x0064   /* ord 100, 16 args */
/* s89 (#162, Charmap's font list): EnumFontFamilies(hdc, lpszFamily, proc, lParam),
   GDI.330, a WRAPPER stub of 14 argument bytes (wowmap.py). Frame, reversed:
   +0 lParam, +4 proc, +8 lpszFamily (far, NULL = one per family), +12 hdc. */
#define WOWGDI_ENUMFONTFAMILIES 0x014a
/* s90 (#296): EnumFonts(hdc, lpszFace, proc, lParam), GDI.70, the same 14-byte
   frame as EnumFontFamilies (EFF_ARG_*); its callback takes LOGFONT + TEXTMETRIC,
   which are the leading parts of the ENUMLOGFONT/NEWTEXTMETRIC blob it shares. */
#define WOWGDI_ENUMFONTS        0x0046
/* s90 (#296): EnumObjects(hdc, nObjectType, proc, lParam), GDI.71, 12 bytes,
   reversed: +0 lParam, +4 proc, +8 type (1 OBJ_PEN, 2 OBJ_BRUSH), +10 hdc.
   Callback: int EnumObjectsProc(LPVOID lpLogObject, LPARAM). Win16's structures
   (windows.h 3.1): LOGPEN {UINT style; POINT width; COLORREF color} = 10 bytes,
   LOGBRUSH {UINT style; COLORREF color; int hatch} = 8 -- UINT/int/POINT being
   16-bit is the whole difference from Win32's. */
#define WOWGDI_ENUMOBJECTS      0x0047
#define WOWGDI_EOB_ARG_LPARAM  0
#define WOWGDI_EOB_ARG_PROC    4
#define WOWGDI_EOB_ARG_TYPE    8
#define WOWGDI_EOB_ARG_HDC     10
#define WOWGDI_EFF_ARG_LPARAM  0
#define WOWGDI_EFF_ARG_PROC    4
#define WOWGDI_EFF_ARG_FAMILY  8
#define WOWGDI_EFF_ARG_HDC     12
#define WOWGDI_LDDA_ARG_DATA    0               /* DWORD */
#define WOWGDI_LDDA_ARG_PROC    4               /* far   */
#define WOWGDI_LDDA_ARG_Y2      8
#define WOWGDI_LDDA_ARG_X2     10
#define WOWGDI_LDDA_ARG_Y1     12
#define WOWGDI_LDDA_ARG_X1     14

/* GDI tokens sit below the menu tokens (0x4000) and above the window handles,
   so a stray handle of any kind is recognisable on sight in a log. */
#define WOWGDI_BASE      0x2000
#define WOWGDI_STEP      0x0008
#define WOWGDI_MAX       256

/* ⚠ A BOUND ON A COUNT THE GUEST CHOSE. CreatePolygonRgn's point count is a
     guest WORD used to size a copy, so it is checked against this before it is
     believed. 1024 points is far past anything the shelf draws and still a
     fixed 8 KB of stack. */
#define WOWGDI_MAX_POLYPTS 1024

/* A little-endian WORD out of guest memory. wowuser.h has the same helper for
   the USER side; this file is included independently, so it has its own. */
static WORD WowGdiPeek(const volatile BYTE *bytes, INT offset)
{
    return (WORD)(bytes[offset] | (bytes[offset + 1] << 8));
}

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

typedef struct _WOWGDI_OBJECT { WORD Handle16; HGDIOBJ Object; INT Kind; } WOWGDI_OBJECT;
static WOWGDI_OBJECT g_WowGdiObjects[WOWGDI_MAX];
static INT          g_WowGdiObjectCount = 0;

/* One token per object. `kind` is kept because DeleteDC, DeleteObject and
   ReleaseDC are three different calls with three different rules, and handing an
   object to the wrong one is a defect this map can catch instead of passing on
   to Win32. */
static WORD WowGdiH16(HGDIOBJ object, INT kind)
{
    INT index;
    if (!object) return 0;
    for (index = 0; index < g_WowGdiObjectCount; ++index)
        if (g_WowGdiObjects[index].Object == object) {
            if (g_WowGdiObjects[index].Kind == kind) return g_WowGdiObjects[index].Handle16;
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
            g_WowGdiObjects[index].Object = NULL;
            g_WowGdiObjects[index].Handle16 = 0;
        }
    for (index = 0; index < g_WowGdiObjectCount; ++index)                    /* reuse a freed slot */
        if (!g_WowGdiObjects[index].Handle16 && !g_WowGdiObjects[index].Object) break;
    if (index == g_WowGdiObjectCount) {
        if (g_WowGdiObjectCount >= WOWGDI_MAX) return 0;
        index = g_WowGdiObjectCount++;
    }
    g_WowGdiObjects[index].Object = object;
    g_WowGdiObjects[index].Kind = kind;
    g_WowGdiObjects[index].Handle16 = (WORD)(WOWGDI_BASE + index * WOWGDI_STEP);
    return g_WowGdiObjects[index].Handle16;
}

static HGDIOBJ WowGdiH32(WORD handle16, PINT kind)
{
    INT index;
    if (kind) *kind = -1;
    if (!handle16) return NULL;
    for (index = 0; index < g_WowGdiObjectCount; ++index)
        if (g_WowGdiObjects[index].Handle16 == handle16) {
            if (kind) *kind = g_WowGdiObjects[index].Kind;
            return g_WowGdiObjects[index].Object;
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
static VOID WowGdiForget(WORD handle16)
{
    INT index;
    for (index = 0; index < g_WowGdiObjectCount; ++index)
        if (g_WowGdiObjects[index].Handle16 == handle16) { g_WowGdiObjects[index].Object = NULL; g_WowGdiObjects[index].Handle16 = 0; return; }
}

/*
 * ⚠ CALLED ONLY WHEN THE STUB IS GDI'S. The caller checks, as it does for every
 *   other table -- `0x45` is `SetWindowPos`-adjacent in USER's numbering and
 *   `DeleteObject` here.
 */
/* s89: one Win32 font -> the Win16 ENUMLOGFONT16 + NEWTEXTMETRIC16 pair, byte-packed.
   LOGFONT16 is LOGFONT with 16-bit ints (18 bytes + a 32-byte face); TEXTMETRIC16 is
   eight ints, nine bytes, three ints (31); NEWTEXTMETRIC16 adds ntmFlags (DWORD),
   ntmSizeEM, ntmCellHeight, ntmAvgWidth (41). */
static VOID WowGdiPut16(PBYTE bytes, INT offset, LONG value) { bytes[offset] = (BYTE)value; bytes[offset + 1] = (BYTE)(value >> 8); }
/* s90: EnumFontsA hands a LOGFONT, not an ENUMLOGFONT -- the full name and style
   past it are not ours to read, so this says not to. */
static INT g_WowGdiFontIsPlain;
static INT CALLBACK WowGdiFontCollect(const LOGFONTA *logFont, const TEXTMETRICA *textMetric,
                                        DWORD type, LPARAM unused)
{
    const ENUMLOGFONTA *enumLogFont = (const ENUMLOGFONTA *)logFont;
    const NEWTEXTMETRICA *newTextMetric = (const NEWTEXTMETRICA *)textMetric;
    PWOWENUM_FONT entry;
    PBYTE blob, metrics;
    INT index;
    (VOID)unused;
    if (g_WowEnumFontCount >= WOWENUM_MAXFONT) return 0;
    entry = &g_WowEnumFonts[g_WowEnumFontCount++];
    for (index = 0; index < (INT)sizeof entry->Blob; ++index) entry->Blob[index] = 0;
    blob = entry->Blob;
    WowGdiPut16(blob, 0, logFont->lfHeight);  WowGdiPut16(blob, 2, logFont->lfWidth);
    WowGdiPut16(blob, 4, logFont->lfEscapement); WowGdiPut16(blob, 6, logFont->lfOrientation);
    WowGdiPut16(blob, 8, logFont->lfWeight);
    blob[10] = logFont->lfItalic; blob[11] = logFont->lfUnderline; blob[12] = logFont->lfStrikeOut;
    blob[13] = logFont->lfCharSet; blob[14] = logFont->lfOutPrecision; blob[15] = logFont->lfClipPrecision;
    blob[16] = logFont->lfQuality; blob[17] = logFont->lfPitchAndFamily;
    for (index = 0; index < 31 && logFont->lfFaceName[index]; ++index) blob[18 + index] = (BYTE)logFont->lfFaceName[index];
    if (!g_WowGdiFontIsPlain) {
        for (index = 0; index < 63 && enumLogFont->elfFullName[index]; ++index) blob[50 + index] = enumLogFont->elfFullName[index];
        for (index = 0; index < 31 && enumLogFont->elfStyle[index]; ++index) blob[114 + index] = enumLogFont->elfStyle[index];
    }
    metrics = blob + WOWENUM_ELF16;
    WowGdiPut16(metrics, 0, textMetric->tmHeight);  WowGdiPut16(metrics, 2, textMetric->tmAscent);
    WowGdiPut16(metrics, 4, textMetric->tmDescent); WowGdiPut16(metrics, 6, textMetric->tmInternalLeading);
    WowGdiPut16(metrics, 8, textMetric->tmExternalLeading); WowGdiPut16(metrics, 10, textMetric->tmAveCharWidth);
    WowGdiPut16(metrics, 12, textMetric->tmMaxCharWidth); WowGdiPut16(metrics, 14, textMetric->tmWeight);
    metrics[16] = textMetric->tmItalic; metrics[17] = textMetric->tmUnderlined; metrics[18] = textMetric->tmStruckOut;
    metrics[19] = (BYTE)textMetric->tmFirstChar; metrics[20] = (BYTE)textMetric->tmLastChar;
    metrics[21] = (BYTE)textMetric->tmDefaultChar; metrics[22] = (BYTE)textMetric->tmBreakChar;
    metrics[23] = textMetric->tmPitchAndFamily; metrics[24] = textMetric->tmCharSet;
    WowGdiPut16(metrics, 25, textMetric->tmOverhang); WowGdiPut16(metrics, 27, textMetric->tmDigitizedAspectX);
    WowGdiPut16(metrics, 29, textMetric->tmDigitizedAspectY);
    if ((type & TRUETYPE_FONTTYPE) && !g_WowGdiFontIsPlain) { /* the NEW part: TrueType's */
        metrics[31] = (BYTE)newTextMetric->ntmFlags; metrics[32] = (BYTE)(newTextMetric->ntmFlags >> 8);
        metrics[33] = (BYTE)(newTextMetric->ntmFlags >> 16); metrics[34] = (BYTE)(newTextMetric->ntmFlags >> 24);
        WowGdiPut16(metrics, 35, (LONG)newTextMetric->ntmSizeEM); WowGdiPut16(metrics, 37, (LONG)newTextMetric->ntmCellHeight);
        WowGdiPut16(metrics, 39, (LONG)newTextMetric->ntmAvgWidth);
    }
    entry->FontType = (WORD)type;
    return 1;
}

/* s90: one Win32 pen/brush -> LOGPEN16 (10) / LOGBRUSH16 (8), into g_WowEnumFonts[]. */
static INT CALLBACK WowGdiObjectCollect(LPVOID logObject, LPARAM type)
{
    PBYTE blob;
    INT index;
    if (g_WowEnumFontCount >= WOWENUM_MAXFONT) return 0;
    blob = g_WowEnumFonts[g_WowEnumFontCount].Blob;
    for (index = 0; index < 16; ++index) blob[index] = 0;
    if (type == OBJ_PEN) {
        const LOGPEN *logPen = (const LOGPEN *)logObject;
        WowGdiPut16(blob, 0, (LONG)logPen->lopnStyle);
        WowGdiPut16(blob, 2, logPen->lopnWidth.x); WowGdiPut16(blob, 4, logPen->lopnWidth.y);
        WowGdiPut16(blob, 6, (LONG)(logPen->lopnColor & 0xFFFF));
        WowGdiPut16(blob, 8, (LONG)(logPen->lopnColor >> 16));
    } else {
        const LOGBRUSH *logBrush = (const LOGBRUSH *)logObject;
        WowGdiPut16(blob, 0, (LONG)logBrush->lbStyle);
        WowGdiPut16(blob, 2, (LONG)(logBrush->lbColor & 0xFFFF));
        WowGdiPut16(blob, 4, (LONG)(logBrush->lbColor >> 16));
        WowGdiPut16(blob, 6, (LONG)logBrush->lbHatch);
    }
    g_WowEnumFonts[g_WowEnumFontCount].FontType = (WORD)type;
    ++g_WowEnumFontCount;
    return 1;
}

/* ── ★ EnumMetaFile / PlayMetaFileRecord: ONE ENUMERATION'S STATE. (#295) ─────
     EnumMetaFile is the per-item chain of wowenum.h with two things no other
     enumeration has: the items are VARIABLE-SIZED (a METARECORD), and the guest
     gets a HANDLETABLE that must PERSIST across the calls -- record 1 creates a
     pen into slot 0, record 2 selects slot 0. Real GDI keeps that table in one
     block and passes the same pointer every time. Here every callback gets a
     fresh stack blob (the only guest memory this host can hand out for one call,
     wowcall.h), so persistence is done by hand:
       blob   = [ the record, maybe truncated ][ WORD token[nObj] ]
       lpmr   -> the record,   lpht -> the table (a second pointer into the blob)
       after the callback returns, the table is READ BACK out of the guest stack
       into g_WowGdiMetafile.tok[] -- before anything else runs on that stack -- and the next
       record's blob starts from it.
     So whatever wrote the table -- our PlayMetaFileRecord, or the guest itself --
     is what the next record sees, which is what one persistent block gives.
     ⚠ ONE SOURCE OF TRUTH: the table is 16-bit TOKENS only. A Win32 HGDIOBJ table
       is built from them for each PlayMetaFileRecord and its changes written back
       as tokens, so there is no second table to drift out of step with the first.
     ⚠ UNMEASURED, and said rather than assumed: whether stock hands the SAME lpht
       pointer every time (ours usually does -- the parked caller's SP does not
       move -- but a guest must not depend on it), and whether stock deletes the
       objects left in the table at the end. We do, as Wine's EnumMetaFile16 does
       (it first re-selects the DC's original pen/brush/font so nothing deleted is
       still selected); a guest that keeps a table handle past EnumMetaFile is
       using a deleted object on Wine too. */
#define WOWMF_MAXOBJ 256        /* mtNoObjects above this: refused, loudly. A WORD
                                   field, but real metafiles hold a handful, and
                                   the token map itself has only WOWGDI_MAX slots. */
/* ⚠⚠ IS THE FINAL META_EOF RECORD (rdSize 3, rdFunction 0) HANDED TO THE CALLBACK?
     UNKNOWN for Win16 and NOT GUESSED: this must be set from the stock run of
     tests/probes/win16/w_mfenum (case mfe.count, and the last mfe.rec.N). 1 = passed,
     which is what Win32's EnumMetaFile is documented to do ("each record ... until
     the last record"); Wine's EnumMetaFile16 BREAKS at META_EOF without calling,
     i.e. 0. Either way the walk ends at EOF -- nothing after it is a record.
   ★ s92, MEASURED: stock does NOT pass it -- w_mfenum under stock ntvdm on the rig
     makes 5 calls for a 5-record metafile, the last one 041B (Rectangle); ours made 6
     with 0000 last. Wine had it right. */
#define WOWMF_PASS_EOF 0
typedef struct _WOWGDI_METAFILE {
    INT     IsActive;
    PBYTE Bits;               /* GetMetaFileBitsEx snapshot, HeapAlloc'd       */
    DWORD   Length, End, Offset;      /* bytes; the walk's bound; the next record       */
    UINT ObjectCount;              /* mtNoObjects = the callback's nObj              */
    WORD    Tokens[WOWMF_MAXOBJ];  /* the persistent HANDLETABLE, as tokens          */
    WORD    Dc16;
    HDC     Dc;                 /* NULL when the guest's hdc is not one of ours   */
    HGDIOBJ OriginalPen, OriginalBrush, OriginalFont;/* re-selected before the table is deleted        */
    DWORD   TableLinear, RecordLinear;   /* where the last callback's copies are (host lin)*/
    DWORD   RecordOffset, RecordBytes; /* the record in flight, in `Bits`               */
    INT     IsTruncated;          /* ...and whether its stack copy is cut short     */
    DWORD   Records;
} WOWGDI_METAFILE, *PWOWGDI_METAFILE;
static WOWGDI_METAFILE g_WowGdiMetafile;
static BYTE        g_WowGdiMetafileBlob[WOWCALL_MAX_BLOB];
/* One record as the guest handed it to PlayMetaFileRecord. A 16:16 pointer reaches
   at most 64 KB past its offset, so this is the largest a record can be without a
   huge pointer -- and a larger one is refused, not wrapped. */
static BYTE        g_WowGdiMetafileRecord[0x10000];
static HGDIOBJ     g_WowGdiMetafileHandles[WOWMF_MAXOBJ];

/* After a callback: what the guest's table says now. Only 0 and tokens that name
   an object (not a DC) are believed; anything else keeps the previous entry and is
   counted, so the caller can say so. */
static INT WowGdiMetafileReadBack(VOID)
{
    UINT index;
    INT badCount = 0;
    if (!g_WowGdiMetafile.IsActive || !g_WowGdiMetafile.TableLinear) return 0;
    for (index = 0; index < g_WowGdiMetafile.ObjectCount; ++index) {
        WORD token = WowGdiPeek((const volatile BYTE *)(ULONG_PTR)g_WowGdiMetafile.TableLinear, (INT)(index * 2));
        INT  kind = -1;
        if (token == g_WowGdiMetafile.Tokens[index]) continue;
        if (!token || (WowGdiH32(token, &kind) && kind != WOWGDI_KIND_DC && kind != WOWGDI_KIND_WINDC))
            g_WowGdiMetafile.Tokens[index] = token;
        else ++badCount;
    }
    g_WowGdiMetafile.TableLinear = 0;
    return badCount;
}

/* The end of an enumeration, however it ended (complete, stopped, refused). */
static VOID WowGdiMetafileEnd(VOID)
{
    UINT index;
    if (!g_WowGdiMetafile.IsActive) return;
    if (g_WowGdiMetafile.Dc) {
        if (g_WowGdiMetafile.OriginalPen)   SelectObject(g_WowGdiMetafile.Dc, g_WowGdiMetafile.OriginalPen);
        if (g_WowGdiMetafile.OriginalBrush) SelectObject(g_WowGdiMetafile.Dc, g_WowGdiMetafile.OriginalBrush);
        if (g_WowGdiMetafile.OriginalFont)  SelectObject(g_WowGdiMetafile.Dc, g_WowGdiMetafile.OriginalFont);
    }
    for (index = 0; index < g_WowGdiMetafile.ObjectCount; ++index) {
        INT kind = -1;
        HGDIOBJ object = g_WowGdiMetafile.Tokens[index] ? WowGdiH32(g_WowGdiMetafile.Tokens[index], &kind) : NULL;
        if (object && kind == WOWGDI_KIND_OBJ) { DeleteObject(object); WowGdiForget(g_WowGdiMetafile.Tokens[index]); }
        g_WowGdiMetafile.Tokens[index] = 0;
    }
    if (g_WowGdiMetafile.Bits) HeapFree(GetProcessHeap(), 0, g_WowGdiMetafile.Bits);
    g_WowGdiMetafile.Bits = NULL;
    g_WowGdiMetafile.IsActive = 0;
    g_WowGdiMetafile.TableLinear = g_WowGdiMetafile.RecordLinear = 0;
}

/* The next record's blob, at most `room` bytes. Returns 0 at the end of the walk
   (or at a malformed record, which ends it the same way -- logged by the caller
   from *func == 0xFFFF). *tbloff = where the table starts in the blob. */
static INT WowGdiMetafileNext(INT room, PINT blobLength, PINT tableOffset, UINT *function)
{
    unsigned long recordBytes = 0;
    UINT recordFunction = 0;
    INT tableBytes = (INT)(g_WowGdiMetafile.ObjectCount ? g_WowGdiMetafile.ObjectCount : 1) * 2;    /* lpht always points at
                                                           something, even nObj 0 */
    INT copyBytes, index;
    *function = 0;
    if (!g_WowGdiMetafile.IsActive || g_WowGdiMetafile.Offset >= g_WowGdiMetafile.End) return 0;
    if (!WowConvMetafileRecord(g_WowGdiMetafile.Bits, g_WowGdiMetafile.End, g_WowGdiMetafile.Offset, &recordBytes, &recordFunction)) {
        *function = 0xFFFF;
        return 0;
    }
    if (recordFunction == 0 && !WOWMF_PASS_EOF) return 0;
    if (room < tableBytes + WOWCONV_MF_RECHDR) { *function = 0xFFFE; return 0; }
    copyBytes = (INT)recordBytes;
    g_WowGdiMetafile.IsTruncated = 0;
    if (copyBytes > room - tableBytes) { copyBytes = (room - tableBytes) & ~1; g_WowGdiMetafile.IsTruncated = 1; }
    for (index = 0; index < copyBytes; ++index) g_WowGdiMetafileBlob[index] = g_WowGdiMetafile.Bits[g_WowGdiMetafile.Offset + (DWORD)index];
    for (index = 0; index < (INT)g_WowGdiMetafile.ObjectCount; ++index) WowGdiPut16(g_WowGdiMetafileBlob, copyBytes + index * 2, g_WowGdiMetafile.Tokens[index]);
    if (!g_WowGdiMetafile.ObjectCount) WowGdiPut16(g_WowGdiMetafileBlob, copyBytes, 0);
    g_WowGdiMetafile.RecordOffset = g_WowGdiMetafile.Offset;
    g_WowGdiMetafile.RecordBytes = (DWORD)recordBytes;
    g_WowGdiMetafile.Offset = (recordFunction == 0) ? g_WowGdiMetafile.End : g_WowGdiMetafile.Offset + (DWORD)recordBytes;    /* EOF is last */
    ++g_WowGdiMetafile.Records;
    *blobLength = copyBytes + tableBytes;
    *tableOffset = copyBytes;
    *function = recordFunction;
    return 1;
}

static INT WowGdiCall(PWOW32_FRAME f, PSTR note, INT notecap)
{
    if (notecap) note[0] = 0;
    switch (f->Id) {

    /* ── ★ 0x50 GetDeviceCaps(hDC, nIndex) ──────────────────────────────────
         Straight through to the OS on the same claim the WS_* bits and the SM_*
         indices travel on: Win32 inherited the DC capability indices from Win16
         unchanged. ⚠ As with GetSystemMetrics, a wrong answer here does not
         fail, it lays something out slightly wrong -- so the index AND the answer
         are logged on every call, and a guest whose arithmetic looks wrong can be
         checked against what it was actually told. */
    case WOWGDI_GETDEVICECAPS: {
        WORD hdc = Wow32ArgWord(f, WOWGDI_GDC_ARG_HDC);
        WORD idx = Wow32ArgWord(f, WOWGDI_GDC_ARG_INDEX);
        INT  kind = -1;
        HGDIOBJ o = WowGdiH32(hdc, &kind);
        INT k = 0, v;
        INT isdc = (kind == WOWGDI_KIND_DC || kind == WOWGDI_KIND_WINDC);
        WowNotePut(note, notecap, &k, "GetDeviceCaps(0x");
        WowNoteHex(note, notecap, &k, hdc, 4);
        WowNotePut(note, notecap, &k, ", 0x");
        WowNoteHex(note, notecap, &k, idx, 4);
        WowNotePut(note, notecap, &k, ")");
        if (!o || !isdc) {
            WowNotePut(note, notecap, &k, " -- ★ NOT ONE OF OUR GDI DC TOKENS (the"
                                       " calls that CREATE a DC are not serviced"
                                       " yet); answered 0");
            Wow32SetReturn(f, 0);
            return 1;
        }
        v = GetDeviceCaps((HDC)o, (INT)idx);

        /* ── ★★★★★ NUMCOLORS: -1 IS A Win32 SENTINEL AND NO Win16 PROGRAM HAS
             EVER SEEN ONE. (session 51 -- MINESWEEPER RENDERED IN BLACK AND
             WHITE, and stock ntvdm runs the same binary in colour.)
             Win32 answers NUMCOLORS with -1 for any device deeper than 8bpp.
             That reaches the guest as 0xffff, and WINMINE.EXE asks NUMCOLORS once
             at start-up and decides colour vs MONOCHROME on "more than 2",
             compared SIGNED -- so 0xffff, i.e. -1, reads as monochrome. Answered
             -1, every DIB it builds and hands to
             SetDIBitsToDevice really is 1bpp, and this host draws it faithfully;
             answered a colour-table size, the same run is in colour.
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
            INT bpp = GetDeviceCaps((HDC)o, BITSPIXEL)
                    * GetDeviceCaps((HDC)o, PLANES);
            v = WowConvNumColors(bpp);       /* ★ tested in wow_test.c part 3 */
            WowNotePut(note, notecap, &k, " [NUMCOLORS -1 -> ");
            WowNoteHex(note, notecap, &k, (DWORD)v, 4);
            WowNotePut(note, notecap, &k, "; a Win16 caller reads -1 as MONOCHROME]");
        }
        /* ⚠ REFUTED, session 45, and recorded so it is not re-tried: forcing
             HORZRES/HORZSIZE to stock's 2.0 (from our 2.625) changed MS Paint's
             toolbox by NOTHING -- pbTool stayed 163x731 to the pixel. Paint
             reads all four of HORZRES/HORZSIZE/VERTRES/VERTSIZE, and the ratio
             matched the over-scale exactly, and it is still not the lever. */
        WowNotePut(note, notecap, &k, " = 0x");
        WowNoteHex(note, notecap, &k, (DWORD)v, 4);
        Wow32SetReturn(f, (DWORD)(WORD)v);
        return 1;
    }

    /* ── ★★★★★ 0x22 BitBlt -- THE TOOL ICONS. ───────────────────────────────
       ⚠ BOTH DCs ARE TOKENS and both must resolve, which is the whole reason a
         blit could not work before the producers went in: the source is almost
         always a memory DC with the toolbox bitmap selected into it. */
    case WOWGDI_BITBLT: {
        WORD hdst = Wow32ArgWord(f, WOWGDI_BB_ARG_DSTDC);
        WORD hsrc = Wow32ArgWord(f, WOWGDI_BB_ARG_SRCDC);
        INT  x  = (INT)(SHORT)Wow32ArgWord(f, WOWGDI_BB_ARG_X);
        INT  y  = (INT)(SHORT)Wow32ArgWord(f, WOWGDI_BB_ARG_Y);
        INT  cx = (INT)(SHORT)Wow32ArgWord(f, WOWGDI_BB_ARG_WIDTH);
        INT  cy = (INT)(SHORT)Wow32ArgWord(f, WOWGDI_BB_ARG_HEIGHT);
        INT  sx = (INT)(SHORT)Wow32ArgWord(f, WOWGDI_BB_ARG_SRCX);
        INT  sy = (INT)(SHORT)Wow32ArgWord(f, WOWGDI_BB_ARG_SRCY);
        DWORD rop = Wow32ArgDword(f, WOWGDI_BB_ARG_ROP);
        INT  dk = -1, sk = -1;
        HGDIOBJ d = WowGdiH32(hdst, &dk);
        HGDIOBJ s = WowGdiH32(hsrc, &sk);
        INT  k = 0, ok;
        WowNotePut(note, notecap, &k, "BitBlt dst 0x");
        WowNoteHex(note, notecap, &k, hdst, 4);
        WowNotePut(note, notecap, &k, " (");
        WowNoteHex(note, notecap, &k, (DWORD)x, 4);
        WowNotePut(note, notecap, &k, ",");
        WowNoteHex(note, notecap, &k, (DWORD)y, 4);
        WowNotePut(note, notecap, &k, ") ");
        WowNoteHex(note, notecap, &k, (DWORD)cx, 4);
        WowNotePut(note, notecap, &k, "x");
        WowNoteHex(note, notecap, &k, (DWORD)cy, 4);
        WowNotePut(note, notecap, &k, " <- src 0x");
        WowNoteHex(note, notecap, &k, hsrc, 4);
        WowNotePut(note, notecap, &k, " (");
        WowNoteHex(note, notecap, &k, (DWORD)sx, 4);
        WowNotePut(note, notecap, &k, ",");
        WowNoteHex(note, notecap, &k, (DWORD)sy, 4);
        WowNotePut(note, notecap, &k, ") rop=0x");
        WowNoteHex(note, notecap, &k, rop, 8);
        if (!d || (dk != WOWGDI_KIND_DC && dk != WOWGDI_KIND_WINDC)) {
            WowNotePut(note, notecap, &k, " -- ★ THE DESTINATION IS NOT ONE OF OUR"
                                       " DC TOKENS; answered 0");
            Wow32SetReturn(f, 0);
            return 1;
        }
        /* ⚠ A NULL SOURCE IS LEGAL for the rops that do not read one (BLACKNESS,
             WHITENESS, PATCOPY...), so a zero handle is passed through as NULL
             rather than refused; a NON-zero handle that we cannot name is a
             different thing and is refused. */
        if (hsrc && (!s || (sk != WOWGDI_KIND_DC && sk != WOWGDI_KIND_WINDC))) {
            WowNotePut(note, notecap, &k, " -- ★ THE SOURCE IS NOT ONE OF OUR DC"
                                       " TOKENS; answered 0");
            Wow32SetReturn(f, 0);
            return 1;
        }
        ok = BitBlt((HDC)d, x, y, cx, cy, hsrc ? (HDC)s : NULL, sx, sy, rop)
             ? 1 : 0;
        WowNotePut(note, notecap, &k, ok ? " -> blitted" : " -- ★ the OS refused it");
        Wow32SetReturn(f, (DWORD)ok);
        return 1;
    }

    /* ── ★★★★★ 0x23 StretchBlt -- see the note above. ───────────────────────
       ⚠ THE STRETCH MODE IS THE DC'S, and Paint sets it (SetStretchBltMode,
         COLORONCOLOR) before getting here -- which is why that call had to go in
         with this one rather than after it. */
    case WOWGDI_STRETCHBLT: {
        WORD hdst = Wow32ArgWord(f, WOWGDI_SB_ARG_DSTDC);
        WORD hsrc = Wow32ArgWord(f, WOWGDI_SB_ARG_SRCDC);
        INT  x  = (INT)(SHORT)Wow32ArgWord(f, WOWGDI_SB_ARG_DSTX);
        INT  y  = (INT)(SHORT)Wow32ArgWord(f, WOWGDI_SB_ARG_DSTY);
        INT  cx = (INT)(SHORT)Wow32ArgWord(f, WOWGDI_SB_ARG_DSTW);
        INT  cy = (INT)(SHORT)Wow32ArgWord(f, WOWGDI_SB_ARG_DSTH);
        INT  sx = (INT)(SHORT)Wow32ArgWord(f, WOWGDI_SB_ARG_SRCX);
        INT  sy = (INT)(SHORT)Wow32ArgWord(f, WOWGDI_SB_ARG_SRCY);
        INT  sw = (INT)(SHORT)Wow32ArgWord(f, WOWGDI_SB_ARG_SRCW);
        INT  sh = (INT)(SHORT)Wow32ArgWord(f, WOWGDI_SB_ARG_SRCH);
        DWORD rop = Wow32ArgDword(f, WOWGDI_SB_ARG_ROP);
        INT  dk = -1, sk = -1;
        HGDIOBJ d = WowGdiH32(hdst, &dk);
        HGDIOBJ s = WowGdiH32(hsrc, &sk);
        INT  k = 0, ok;
        WowNotePut(note, notecap, &k, "StretchBlt dst 0x");
        WowNoteHex(note, notecap, &k, hdst, 4);
        WowNotePut(note, notecap, &k, " (");
        WowNoteHex(note, notecap, &k, (DWORD)x, 4);
        WowNotePut(note, notecap, &k, ",");
        WowNoteHex(note, notecap, &k, (DWORD)y, 4);
        WowNotePut(note, notecap, &k, ") ");
        WowNoteHex(note, notecap, &k, (DWORD)cx, 4);
        WowNotePut(note, notecap, &k, "x");
        WowNoteHex(note, notecap, &k, (DWORD)cy, 4);
        WowNotePut(note, notecap, &k, " <- src 0x");
        WowNoteHex(note, notecap, &k, hsrc, 4);
        WowNotePut(note, notecap, &k, " ");
        WowNoteHex(note, notecap, &k, (DWORD)sw, 4);
        WowNotePut(note, notecap, &k, "x");
        WowNoteHex(note, notecap, &k, (DWORD)sh, 4);
        WowNotePut(note, notecap, &k, " rop=0x");
        WowNoteHex(note, notecap, &k, rop, 8);
        if (!d || (dk != WOWGDI_KIND_DC && dk != WOWGDI_KIND_WINDC)) {
            WowNotePut(note, notecap, &k, " -- ★ THE DESTINATION IS NOT ONE OF OUR"
                                       " DC TOKENS; answered 0");
            Wow32SetReturn(f, 0);
            return 1;
        }
        if (hsrc && (!s || (sk != WOWGDI_KIND_DC && sk != WOWGDI_KIND_WINDC))) {
            WowNotePut(note, notecap, &k, " -- ★ THE SOURCE IS NOT ONE OF OUR DC"
                                       " TOKENS; answered 0");
            Wow32SetReturn(f, 0);
            return 1;
        }
        ok = StretchBlt((HDC)d, x, y, cx, cy, hsrc ? (HDC)s : NULL,
                        sx, sy, sw, sh, rop) ? 1 : 0;
        WowNotePut(note, notecap, &k, ok ? " -> stretched"
                                      : " -- ★ the OS refused it");
        Wow32SetReturn(f, (DWORD)ok);
        return 1;
    }

    /* ── ★ 0x4f GetDCOrg / 0x95 GetBrushOrg / 0x96 UnrealizeObject ──────────*/
    case WOWGDI_GETDCORG:
    case WOWGDI_GETBRUSHORG:
    case WOWGDI_UNREALIZEOBJ: {
        WORD h = Wow32ArgWord(f, WOWGDI_ONE_ARG_HANDLE);
        INT  kind = -1;
        HGDIOBJ o = WowGdiH32(h, &kind);
        INT  isdc = (f->Id != WOWGDI_UNREALIZEOBJ);
        INT  k = 0;
        POINT pt;
        WowNotePut(note, notecap, &k,
                f->Id == WOWGDI_GETDCORG    ? "GetDCOrg(0x" :
                f->Id == WOWGDI_GETBRUSHORG ? "GetBrushOrg(0x"
                                            : "UnrealizeObject(0x");
        WowNoteHex(note, notecap, &k, h, 4);
        WowNotePut(note, notecap, &k, ")");
        if (!o) {
            WowNotePut(note, notecap, &k, " -- ★ NOT ONE OF OUR GDI TOKENS;"
                                       " answered 0");
            Wow32SetReturn(f, 0);
            return 1;
        }
        if (isdc && kind != WOWGDI_KIND_DC && kind != WOWGDI_KIND_WINDC) {
            WowNotePut(note, notecap, &k, " -- ★ THAT IS NOT A DC; answered 0");
            Wow32SetReturn(f, 0);
            return 1;
        }
        if (f->Id == WOWGDI_UNREALIZEOBJ) {
            INT r = UnrealizeObject(o) ? 1 : 0;
            WowNotePut(note, notecap, &k, r ? " -> unrealized" : " -- ★ refused");
            Wow32SetReturn(f, (DWORD)r);
            return 1;
        }
        pt.x = pt.y = 0;
        if (f->Id == WOWGDI_GETBRUSHORG) GetBrushOrgEx((HDC)o, &pt);
        else                             GetDCOrgEx((HDC)o, &pt);
        WowNotePut(note, notecap, &k, " -> ");
        WowNoteHex(note, notecap, &k, (DWORD)pt.x, 4);
        WowNotePut(note, notecap, &k, ",");
        WowNoteHex(note, notecap, &k, (DWORD)pt.y, 4);
        Wow32SetReturn(f, ((DWORD)(WORD)(SHORT)pt.y << 16)
                        | (DWORD)(WORD)(SHORT)pt.x);
        return 1;
    }

    /* ── ★ 0x67 PtVisible(hDC, x, y) ────────────────────────────────────────*/
    case WOWGDI_PTVISIBLE: {
        WORD hdc = Wow32ArgWord(f, WOWGDI_PV_ARG_HDC);
        INT  x = (INT)(SHORT)Wow32ArgWord(f, WOWGDI_PV_ARG_X);
        INT  y = (INT)(SHORT)Wow32ArgWord(f, WOWGDI_PV_ARG_Y);
        INT  kind = -1;
        HGDIOBJ o = WowGdiH32(hdc, &kind);
        INT  k = 0, r;
        WowNotePut(note, notecap, &k, "PtVisible(0x");
        WowNoteHex(note, notecap, &k, hdc, 4);
        WowNotePut(note, notecap, &k, ", ");
        WowNoteHex(note, notecap, &k, (DWORD)x, 4);
        WowNotePut(note, notecap, &k, ",");
        WowNoteHex(note, notecap, &k, (DWORD)y, 4);
        WowNotePut(note, notecap, &k, ")");
        if (!o || (kind != WOWGDI_KIND_DC && kind != WOWGDI_KIND_WINDC)) {
            WowNotePut(note, notecap, &k, " -- ★ NOT ONE OF OUR DC TOKENS;"
                                       " answered 0");
            Wow32SetReturn(f, 0);
            return 1;
        }
        r = PtVisible((HDC)o, x, y) ? 1 : 0;
        WowNotePut(note, notecap, &k, r ? " -> visible" : " -> NOT visible");
        Wow32SetReturn(f, (DWORD)r);
        return 1;
    }

    /* ── ★★★★ 0x63 LPtoDP(hDC, lpPoints, nCount) -- see the note above. ─────*/
    case WOWGDI_POLYGON:
    case WOWGDI_POLYLINE:
    case WOWGDI_DPTOLP:
    case WOWGDI_LPTODP: {
        WORD hdc = Wow32ArgWord(f, WOWGDI_LDP_ARG_HDC);
        WORD n   = Wow32ArgWord(f, WOWGDI_LDP_ARG_COUNT);
        volatile BYTE *p = Wow32ArgPointer(f, WOWGDI_LDP_ARG_POINTS);
        INT  kind = -1;
        HGDIOBJ o = WowGdiH32(hdc, &kind);
        POINT pt[64];
        INT  k = 0, i, cnt = (INT)n;
        INT isline = (f->Id == WOWGDI_POLYLINE);
        INT ispoly = (f->Id == WOWGDI_POLYGON) || isline;
        WowNotePut(note, notecap, &k,
                isline ? "Polyline(0x" : ispoly ? "Polygon(0x"
                : f->Id == WOWGDI_DPTOLP ? "DPtoLP(0x" : "LPtoDP(0x");
        WowNoteHex(note, notecap, &k, hdc, 4);
        WowNotePut(note, notecap, &k, ", ");
        WowNoteHex(note, notecap, &k, (DWORD)n, 4);
        WowNotePut(note, notecap, &k, " points)");
        if (!o || (kind != WOWGDI_KIND_DC && kind != WOWGDI_KIND_WINDC) || !p) {
            WowNotePut(note, notecap, &k, " -- ★ NOT ONE OF OUR DC TOKENS, or no"
                                       " points; answered 0");
            Wow32SetReturn(f, 0);
            return 1;
        }
        /* ⚠ BOUNDED. The count comes from the guest and the scratch array does
             not grow; a longer run is refused rather than overrunning. */
        if (cnt <= 0 || cnt > (INT)(sizeof pt / sizeof pt[0])) {
            WowNotePut(note, notecap, &k, " -- ★ COUNT OUT OF RANGE; answered 0");
            Wow32SetReturn(f, 0);
            return 1;
        }
        for (i = 0; i < cnt; ++i) {
            pt[i].x = (INT)(SHORT)Wow32PeekWord(p + i * 4);
            pt[i].y = (INT)(SHORT)Wow32PeekWord(p + i * 4 + 2);
        }
        /* ★ Polygon DRAWS and writes nothing back; LPtoDP TRANSFORMS IN PLACE.
             Same block, opposite data flow -- so they share the read and part
             company here. */
        if (ispoly) {
            INT r = (isline ? Polyline((HDC)o, pt, cnt)
                            : Polygon((HDC)o, pt, cnt)) ? 1 : 0;
            WowNotePut(note, notecap, &k, r ? " -> drawn" : " -- ★ the OS refused it");
            Wow32SetReturn(f, (DWORD)r);
            return 1;
        }
        /* ★ DPtoLP is LPtoDP's inverse and nothing else differs -- same block,
             same in-place write-back, opposite transform. */
        if (!(f->Id == WOWGDI_DPTOLP ? DPtoLP((HDC)o, pt, cnt)
                                     : LPtoDP((HDC)o, pt, cnt))) {
            WowNotePut(note, notecap, &k, " -- ★ the OS refused it; answered 0");
            Wow32SetReturn(f, 0);
            return 1;
        }
        for (i = 0; i < cnt; ++i) {
            Wow32PokeWord(p + i * 4,     (WORD)(SHORT)pt[i].x);
            Wow32PokeWord(p + i * 4 + 2, (WORD)(SHORT)pt[i].y);
        }
        WowNotePut(note, notecap, &k, " -> ");
        WowNoteHex(note, notecap, &k, (DWORD)pt[0].x, 4);
        WowNotePut(note, notecap, &k, ",");
        WowNoteHex(note, notecap, &k, (DWORD)pt[0].y, 4);
        Wow32SetReturn(f, 1);
        return 1;
    }

    /* ── ★★ 0x9c CreateDiscardableBitmap / 0x94 SetBrushOrg ─────────────────
       ★ "Discardable" was a Win16 memory-pressure hint; Win32 keeps the call and
         ignores the hint, which is the right answer -- the guest gets a real
         bitmap and the hint had no observable semantics to preserve. */
    case WOWGDI_CREATEDISCARDBM:
    case WOWGDI_SETBRUSHORG: {
        INT  isbm = (f->Id == WOWGDI_CREATEDISCARDBM);
        WORD hdc = Wow32ArgWord(f, WOWGDI_CCB_ARG_HDC);      /* same block shape as   */
        INT  a = (INT)(SHORT)Wow32ArgWord(f, WOWGDI_CCB_ARG_WIDTH);   /* CreateCompat- */
        INT  b = (INT)(SHORT)Wow32ArgWord(f, WOWGDI_CCB_ARG_HEIGHT);  /* ibleBitmap    */
        INT  kind = -1;
        HGDIOBJ o = WowGdiH32(hdc, &kind);
        INT  k = 0;
        WowNotePut(note, notecap, &k, isbm ? "CreateDiscardableBitmap(0x"
                                        : "SetBrushOrg(0x");
        WowNoteHex(note, notecap, &k, hdc, 4);
        WowNotePut(note, notecap, &k, ", ");
        WowNoteHex(note, notecap, &k, (DWORD)a, 4);
        WowNotePut(note, notecap, &k, ",");
        WowNoteHex(note, notecap, &k, (DWORD)b, 4);
        WowNotePut(note, notecap, &k, ")");
        if (!o || (kind != WOWGDI_KIND_DC && kind != WOWGDI_KIND_WINDC)) {
            WowNotePut(note, notecap, &k, " -- ★ NOT ONE OF OUR DC TOKENS;"
                                       " answered 0");
            Wow32SetReturn(f, 0);
            return 1;
        }
        if (!isbm) {
            POINT prev;
            prev.x = prev.y = 0;
            SetBrushOrgEx((HDC)o, a, b, &prev);
            Wow32SetReturn(f, ((DWORD)(WORD)(SHORT)prev.y << 16)
                            | (DWORD)(WORD)(SHORT)prev.x);
            return 1;
        }
        {   HBITMAP bm;
            WORD tok;
            if (a <= 0 || b <= 0) {
                WowNotePut(note, notecap, &k, " -- ★ A DIMENSION IS NOT POSITIVE;"
                                           " answered 0");
                Wow32SetReturn(f, 0);
                return 1;
            }
            bm = CreateDiscardableBitmap((HDC)o, a, b);
            tok = bm ? WowGdiH16((HGDIOBJ)bm, WOWGDI_KIND_OBJ) : 0;
            if (!tok) {
                if (bm) DeleteObject((HGDIOBJ)bm);
                WowNotePut(note, notecap, &k, " -- ★ refused, or the token map is"
                                           " full; answered 0");
                Wow32SetReturn(f, 0);
                return 1;
            }
            WowNotePut(note, notecap, &k, " -> bitmap token 0x");
            WowNoteHex(note, notecap, &k, tok, 4);
            Wow32SetReturn(f, tok);
            return 1;
        }
    }

    /* ── ★★ 0x1b Rectangle(hDC, left, top, right, bottom) ───────────────────*/
    case WOWGDI_RECTANGLE: {
        WORD hdc = Wow32ArgWord(f, WOWGDI_RC_ARG_HDC);
        INT  l = (INT)(SHORT)Wow32ArgWord(f, WOWGDI_RC_ARG_LEFT);
        INT  t = (INT)(SHORT)Wow32ArgWord(f, WOWGDI_RC_ARG_TOP);
        INT  r = (INT)(SHORT)Wow32ArgWord(f, WOWGDI_RC_ARG_RIGHT);
        INT  b = (INT)(SHORT)Wow32ArgWord(f, WOWGDI_RC_ARG_BOTTOM);
        INT  kind = -1;
        HGDIOBJ o = WowGdiH32(hdc, &kind);
        INT  k = 0;
        WowNotePut(note, notecap, &k, "Rectangle(0x");
        WowNoteHex(note, notecap, &k, hdc, 4);
        WowNotePut(note, notecap, &k, ", ");
        WowNoteHex(note, notecap, &k, (DWORD)l, 4);
        WowNotePut(note, notecap, &k, ",");
        WowNoteHex(note, notecap, &k, (DWORD)t, 4);
        WowNotePut(note, notecap, &k, " ");
        WowNoteHex(note, notecap, &k, (DWORD)r, 4);
        WowNotePut(note, notecap, &k, ",");
        WowNoteHex(note, notecap, &k, (DWORD)b, 4);
        WowNotePut(note, notecap, &k, ")");
        if (!o || (kind != WOWGDI_KIND_DC && kind != WOWGDI_KIND_WINDC)) {
            WowNotePut(note, notecap, &k, " -- ★ NOT ONE OF OUR DC TOKENS;"
                                       " answered 0");
            Wow32SetReturn(f, 0);
            return 1;
        }
        Wow32SetReturn(f, (DWORD)(Rectangle((HDC)o, l, t, r, b) ? 1 : 0));
        return 1;
    }

    /* ── ★ 0x01 SetBkColor / 0x09 SetTextColor -- a COLORREF is a COLORREF. ──
       ⚠ The PREVIOUS colour is the return value and it is a DWORD, so this is
         one of the calls where the 32-bit return really is 32 bits. */
    case WOWGDI_SETBKCOLOR:
    case WOWGDI_SETTEXTCOLOR: {
        INT  istext = (f->Id == WOWGDI_SETTEXTCOLOR);
        WORD hdc = Wow32ArgWord(f, WOWGDI_COL_ARG_HDC);
        DWORD col = Wow32ArgDword(f, WOWGDI_COL_ARG_COLOR);
        INT  kind = -1;
        HGDIOBJ o = WowGdiH32(hdc, &kind);
        INT  k = 0;
        COLORREF prev;
        WowNotePut(note, notecap, &k, istext ? "SetTextColor(0x" : "SetBkColor(0x");
        WowNoteHex(note, notecap, &k, hdc, 4);
        WowNotePut(note, notecap, &k, ", 0x");
        WowNoteHex(note, notecap, &k, col, 8);
        WowNotePut(note, notecap, &k, ")");
        if (!o || (kind != WOWGDI_KIND_DC && kind != WOWGDI_KIND_WINDC)) {
            WowNotePut(note, notecap, &k, " -- ★ NOT ONE OF OUR DC TOKENS;"
                                       " answered 0");
            Wow32SetReturn(f, 0);
            return 1;
        }
        prev = istext ? SetTextColor((HDC)o, (COLORREF)col)
                      : SetBkColor((HDC)o, (COLORREF)col);
        Wow32SetReturn(f, (DWORD)prev);
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
        WORD hdc  = Wow32ArgWord(f, WOWGDI_MODE_ARG_HDC);
        INT  mode = (INT)(SHORT)Wow32ArgWord(f, WOWGDI_MODE_ARG_MODE);
        INT  kind = -1;
        HGDIOBJ o = WowGdiH32(hdc, &kind);
        INT  k = 0, prev;
        WowNotePut(note, notecap, &k,
                f->Id == WOWGDI_SETROP2         ? "SetROP2(0x" :
                f->Id == WOWGDI_SETSTRETCHMODE  ? "SetStretchBltMode(0x" :
                f->Id == WOWGDI_SETBKMODE       ? "SetBkMode(0x" :
                f->Id == WOWGDI_SETMAPMODE      ? "SetMapMode(0x" :
                f->Id == WOWGDI_SETTEXTALIGN    ? "SetTextAlign(0x"
                                                : "SetPolyFillMode(0x");
        WowNoteHex(note, notecap, &k, hdc, 4);
        WowNotePut(note, notecap, &k, ", ");
        WowNoteHex(note, notecap, &k, (DWORD)mode, 4);
        WowNotePut(note, notecap, &k, ")");
        if (!o || (kind != WOWGDI_KIND_DC && kind != WOWGDI_KIND_WINDC)) {
            WowNotePut(note, notecap, &k, " -- ★ NOT ONE OF OUR DC TOKENS;"
                                       " answered 0");
            Wow32SetReturn(f, 0);
            return 1;
        }
        prev = f->Id == WOWGDI_SETROP2        ? SetROP2((HDC)o, mode)
             : f->Id == WOWGDI_SETSTRETCHMODE ? SetStretchBltMode((HDC)o, mode)
             : f->Id == WOWGDI_SETBKMODE      ? SetBkMode((HDC)o, mode)
             : f->Id == WOWGDI_SETMAPMODE     ? SetMapMode((HDC)o, mode)
             : f->Id == WOWGDI_SETTEXTALIGN   ? (INT)SetTextAlign((HDC)o, (UINT)mode)
                                              : SetPolyFillMode((HDC)o, mode);
        Wow32SetReturn(f, (DWORD)(WORD)prev);
        return 1;
    }

    /* ── ★ 0x0b SetWindowOrg(hDC, x, y) ─────────────────────────────────────*/
    case WOWGDI_SETWINDOWORG: {
        WORD hdc = Wow32ArgWord(f, WOWGDI_ORG_ARG_HDC);
        INT  x = (INT)(SHORT)Wow32ArgWord(f, WOWGDI_ORG_ARG_X);
        INT  y = (INT)(SHORT)Wow32ArgWord(f, WOWGDI_ORG_ARG_Y);
        INT  kind = -1;
        HGDIOBJ o = WowGdiH32(hdc, &kind);
        INT  k = 0;
        POINT prev;
        WowNotePut(note, notecap, &k, "SetWindowOrg(0x");
        WowNoteHex(note, notecap, &k, hdc, 4);
        WowNotePut(note, notecap, &k, ", ");
        WowNoteHex(note, notecap, &k, (DWORD)x, 4);
        WowNotePut(note, notecap, &k, ",");
        WowNoteHex(note, notecap, &k, (DWORD)y, 4);
        WowNotePut(note, notecap, &k, ")");
        if (!o || (kind != WOWGDI_KIND_DC && kind != WOWGDI_KIND_WINDC)) {
            WowNotePut(note, notecap, &k, " -- ★ NOT ONE OF OUR DC TOKENS;"
                                       " answered 0");
            Wow32SetReturn(f, 0);
            return 1;
        }
        prev.x = prev.y = 0;
        SetWindowOrgEx((HDC)o, x, y, &prev);
        Wow32SetReturn(f, ((DWORD)(WORD)(SHORT)prev.y << 16)
                        | (DWORD)(WORD)(SHORT)prev.x);
        return 1;
    }

    /* ── ★ 0x1e SaveDC / 0x27 RestoreDC -- the guest's own nesting level. ────
       ⚠ THE LEVEL IS THE GUEST'S NUMBER AND IT TRAVELS UNCHANGED. Win32 keeps
         its own stack per DC and uses the same convention (positive = absolute,
         negative = relative), and our DCs are the ones the guest saved on, so
         the level it hands back is meaningful without translation. */
    case WOWGDI_SAVEDC:
    case WOWGDI_RESTOREDC: {
        INT  issave = (f->Id == WOWGDI_SAVEDC);
        WORD hdc = issave ? Wow32ArgWord(f, WOWGDI_SDC_ARG_HDC)
                          : Wow32ArgWord(f, WOWGDI_RDC2_ARG_HDC);
        INT  lvl = issave ? 0 : (INT)(SHORT)Wow32ArgWord(f, WOWGDI_RDC2_ARG_LEVEL);
        INT  kind = -1;
        HGDIOBJ o = WowGdiH32(hdc, &kind);
        INT  k = 0, r;
        WowNotePut(note, notecap, &k, issave ? "SaveDC(0x" : "RestoreDC(0x");
        WowNoteHex(note, notecap, &k, hdc, 4);
        if (!issave) {
            WowNotePut(note, notecap, &k, ", level ");
            WowNoteHex(note, notecap, &k, (DWORD)lvl, 4);
        }
        WowNotePut(note, notecap, &k, ")");
        if (!o || (kind != WOWGDI_KIND_DC && kind != WOWGDI_KIND_WINDC)) {
            WowNotePut(note, notecap, &k, " -- ★ NOT ONE OF OUR DC TOKENS;"
                                       " answered 0");
            Wow32SetReturn(f, 0);
            return 1;
        }
        r = issave ? SaveDC((HDC)o) : (RestoreDC((HDC)o, lvl) ? 1 : 0);
        WowNotePut(note, notecap, &k, " -> ");
        WowNoteHex(note, notecap, &k, (DWORD)r, 4);
        Wow32SetReturn(f, (DWORD)(WORD)r);
        return 1;
    }

    /* ── ★★★★ 0x13 LineTo / 0x14 MoveTo -- see the note above. ──────────────*/
    case WOWGDI_LINETO:
    case WOWGDI_MOVETO: {
        INT  isline = (f->Id == WOWGDI_LINETO);
        WORD hdc = Wow32ArgWord(f, WOWGDI_XY_ARG_HDC);
        INT  x = (INT)(SHORT)Wow32ArgWord(f, WOWGDI_XY_ARG_X);
        INT  y = (INT)(SHORT)Wow32ArgWord(f, WOWGDI_XY_ARG_Y);
        INT  kind = -1;
        HGDIOBJ o = WowGdiH32(hdc, &kind);
        INT  k = 0;
        POINT prev;
        WowNotePut(note, notecap, &k, isline ? "LineTo(0x" : "MoveTo(0x");
        WowNoteHex(note, notecap, &k, hdc, 4);
        WowNotePut(note, notecap, &k, ", ");
        WowNoteHex(note, notecap, &k, (DWORD)x, 4);
        WowNotePut(note, notecap, &k, ",");
        WowNoteHex(note, notecap, &k, (DWORD)y, 4);
        WowNotePut(note, notecap, &k, ")");
        if (!o || (kind != WOWGDI_KIND_DC && kind != WOWGDI_KIND_WINDC)) {
            WowNotePut(note, notecap, &k, " -- ★ NOT ONE OF OUR DC TOKENS;"
                                       " answered 0");
            Wow32SetReturn(f, 0);
            return 1;
        }
        if (isline) {
            Wow32SetReturn(f, (DWORD)(LineTo((HDC)o, x, y) ? 1 : 0));
            return 1;
        }
        prev.x = prev.y = 0;
        MoveToEx((HDC)o, x, y, &prev);
        /* ★ y in the HIGH word, x in the LOW -- Win16's MAKELONG order. */
        Wow32SetReturn(f, ((DWORD)(WORD)(SHORT)prev.y << 16)
                        | (DWORD)(WORD)(SHORT)prev.x);
        return 1;
    }

    /* ── ★★★ 0x1d PatBlt(hDC, x, y, nWidth, nHeight, dwRop) ─────────────────
         The raster ops are the same numbers in both worlds (PATCOPY, PATINVERT,
         DSTINVERT, BLACKNESS, WHITENESS), so the rop travels unchanged. */
    case WOWGDI_PATBLT: {
        WORD  hdc = Wow32ArgWord(f, WOWGDI_PB_ARG_HDC);
        INT   x = (INT)(SHORT)Wow32ArgWord(f, WOWGDI_PB_ARG_X);
        INT   y = (INT)(SHORT)Wow32ArgWord(f, WOWGDI_PB_ARG_Y);
        INT   cx = (INT)(SHORT)Wow32ArgWord(f, WOWGDI_PB_ARG_WIDTH);
        INT   cy = (INT)(SHORT)Wow32ArgWord(f, WOWGDI_PB_ARG_HEIGHT);
        DWORD rop = Wow32ArgDword(f, WOWGDI_PB_ARG_ROP);
        INT   kind = -1;
        HGDIOBJ o = WowGdiH32(hdc, &kind);
        INT   k = 0;
        WowNotePut(note, notecap, &k, "PatBlt(0x");
        WowNoteHex(note, notecap, &k, hdc, 4);
        WowNotePut(note, notecap, &k, ", ");
        WowNoteHex(note, notecap, &k, (DWORD)x, 4);
        WowNotePut(note, notecap, &k, ",");
        WowNoteHex(note, notecap, &k, (DWORD)y, 4);
        WowNotePut(note, notecap, &k, " ");
        WowNoteHex(note, notecap, &k, (DWORD)cx, 4);
        WowNotePut(note, notecap, &k, "x");
        WowNoteHex(note, notecap, &k, (DWORD)cy, 4);
        WowNotePut(note, notecap, &k, " rop=0x");
        WowNoteHex(note, notecap, &k, rop, 8);
        WowNotePut(note, notecap, &k, ")");
        if (!o || (kind != WOWGDI_KIND_DC && kind != WOWGDI_KIND_WINDC)) {
            WowNotePut(note, notecap, &k, " -- ★ NOT ONE OF OUR DC TOKENS;"
                                       " answered 0");
            Wow32SetReturn(f, 0);
            return 1;
        }
        Wow32SetReturn(f, (DWORD)(PatBlt((HDC)o, x, y, cx, cy, rop) ? 1 : 0));
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
        WORD idx = Wow32ArgWord(f, WOWGDI_GSO_ARG_INDEX);
        HGDIOBJ o;
        WORD tok;
        INT k = 0;
        WowNotePut(note, notecap, &k, "GetStockObject(0x");
        WowNoteHex(note, notecap, &k, idx, 4);
        WowNotePut(note, notecap, &k, ")");
        if (idx > 16) {
            WowNotePut(note, notecap, &k, " -- ★ NOT A Win16 STOCK OBJECT (they stop"
                                       " at 16); answered 0");
            Wow32SetReturn(f, 0);
            return 1;
        }
        o = GetStockObject((INT)idx);
        /* ⚠ "NO SUCH OBJECT" AND "NO ROOM" ARE DIFFERENT FACTS, and the first
             cut of this printed the second for both -- so a run reported "THE
             GDI TOKEN MAP IS FULL" with nine of 256 slots used, because index 9
             is a hole in the stock-object numbering (it sits between NULL_PEN
             and OEM_FIXED_FONT) and the OS correctly returned NULL. A counter's
             message is a claim; this one was false. */
        if (!o) {
            WowNotePut(note, notecap, &k, " -- ★ THE OS HAS NO SUCH STOCK OBJECT"
                                       " (index 9 is a hole in the numbering);"
                                       " answered 0");
            Wow32SetReturn(f, 0);
            return 1;
        }
        tok = WowGdiH16(o, WOWGDI_KIND_STOCK);
        if (!tok) {
            WowNotePut(note, notecap, &k, " -- ★ THE GDI TOKEN MAP IS FULL;"
                                       " answered 0");
            Wow32SetReturn(f, 0);
            return 1;
        }
        WowNotePut(note, notecap, &k, " -> token 0x");
        WowNoteHex(note, notecap, &k, tok, 4);
        Wow32SetReturn(f, tok);
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
        CHAR drv[64], dev[64];
        HDC dc;
        WORD tok;
        INT  k = 0, isdisp, i;
        INT  isic = (f->Id == WOWGDI_CREATEDC);
        DWORD init = Wow32ArgDword(f, WOWGDI_CDC_ARG_INITDATA);
        Wow32ArgString(f, WOWGDI_CDC_ARG_DRIVER, drv, sizeof drv);
        Wow32ArgString(f, WOWGDI_CDC_ARG_DEVICE, dev, sizeof dev);
        WowNotePut(note, notecap, &k, isic ? "CreateIC driver=" : "CreateDC driver=");
        WowNoteQuoted(note, notecap, &k, drv);
        if (dev[0]) { WowNotePut(note, notecap, &k, " device="); WowNoteQuoted(note, notecap, &k, dev); }
        /* Case-insensitively "DISPLAY" -- the guest writes it lower case. */
        isdisp = 1;
        for (i = 0; i < 7; ++i) {
            CHAR c = drv[i];
            if (c >= 'a' && c <= 'z') c = (CHAR)(c - 32);
            if (c != "DISPLAY"[i]) { isdisp = 0; break; }
        }
        if (isdisp && drv[7]) isdisp = 0;
        if (!isdisp) {
            WowNotePut(note, notecap, &k, " -- ★ NOT THE DISPLAY. That names a"
                                       " Windows 3.1 device driver which does not"
                                       " exist here, so it is refused rather than"
                                       " resolved against one of the host's own"
                                       " devices; answered 0");
            Wow32SetReturn(f, 0);
            return 1;
        }
        if (init)
            WowNotePut(note, notecap, &k, " -- ⚠ lpInitData IS NOT NULL and is being"
                                       " ignored (it carries a DEVMODE)");
        /* ★ An INFORMATION context is asked for by name and answered with one:
             it is cheaper than a DC and a guest that draws on one is doing
             something Windows would refuse too, so the distinction is kept
             rather than flattened into CreateDC. */
        dc = isic ? CreateICA("DISPLAY", NULL, NULL, NULL)
                  : CreateDCA("DISPLAY", NULL, NULL, NULL);
        tok = dc ? WowGdiH16((HGDIOBJ)dc, WOWGDI_KIND_DC) : 0;
        if (!tok) {
            if (dc) DeleteDC(dc);
            WowNotePut(note, notecap, &k, dc ? " -- ★ THE GDI TOKEN MAP IS FULL; the"
                                            " DC was deleted again and 0 answered"
                                          : " -- ★ THE OS REFUSED IT; answered 0");
            Wow32SetReturn(f, 0);
            return 1;
        }
        WowNotePut(note, notecap, &k, " -> screen DC token 0x");
        WowNoteHex(note, notecap, &k, tok, 4);
        Wow32SetReturn(f, tok);
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
        WORD src = Wow32ArgWord(f, WOWGDI_CCD_ARG_HDC);
        INT  kind = -1;
        HGDIOBJ o = src ? WowGdiH32(src, &kind) : NULL;
        HDC dc;
        WORD tok;
        INT k = 0;
        WowNotePut(note, notecap, &k, "CreateCompatibleDC(0x");
        WowNoteHex(note, notecap, &k, src, 4);
        WowNotePut(note, notecap, &k, ")");
        if (src && (!o || (kind != WOWGDI_KIND_DC && kind != WOWGDI_KIND_WINDC))) {
            WowNotePut(note, notecap, &k, " -- ★ THAT IS NOT ONE OF OUR DC TOKENS;"
                                       " answered 0");
            Wow32SetReturn(f, 0);
            return 1;
        }
        if (!src) WowNotePut(note, notecap, &k, " (compatible with the SCREEN)");
        dc = CreateCompatibleDC(src ? (HDC)o : NULL);
        tok = dc ? WowGdiH16((HGDIOBJ)dc, WOWGDI_KIND_DC) : 0;
        if (!tok) {
            if (dc) DeleteDC(dc);         /* never issue a DC we cannot name */
            WowNotePut(note, notecap, &k, dc ? " -- ★ THE GDI TOKEN MAP IS FULL; the"
                                            " DC was deleted again and 0 answered"
                                          : " -- ★ THE OS REFUSED IT; answered 0");
            Wow32SetReturn(f, 0);
            return 1;
        }
        WowNotePut(note, notecap, &k, " -> DC token 0x");
        WowNoteHex(note, notecap, &k, tok, 4);
        Wow32SetReturn(f, tok);
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
    /* ── ★ 0x26 Escape -- THE DRIVER BACK DOOR, ANSWERED HONESTLY ────────────
         WRITE.EXE is the guest that asks (its printing path). On a screen DC the
         truthful answer to a device escape is "this driver does not implement
         it", and Escape says that with 0 -- an in-band answer, not a sentinel we
         invented. QUERYESCSUPPORT is answered properly, which is the whole point
         of having it: a caller that asks first is told before it tries.
       ⚠ SAID OUT LOUD RATHER THAN LEFT IN A ZERO. Every escape number this host
         declines is NAMED in the log, so the first guest that genuinely needs
         one is a line to grep for rather than a mystery -- this project's own
         `an unimplemented call still answers` rule, applied to a call whose
         answer is legitimately 0. */
    case WOWGDI_ESCAPE: {
        WORD hdc  = Wow32ArgWord(f, WOWGDI_ESC_ARG_HDC);
        INT  esc  = (INT)(SHORT)Wow32ArgWord(f, WOWGDI_ESC_ARG_ESCAPE);
        INT  cnt  = (INT)(SHORT)Wow32ArgWord(f, WOWGDI_ESC_ARG_COUNT);
        volatile BYTE *in = Wow32ArgPointer(f, WOWGDI_ESC_ARG_IN);
        INT  k = 0, want = 0;
        WowNotePut(note, notecap, &k, "Escape(dc 0x");
        WowNoteHex(note, notecap, &k, hdc, 4);
        WowNotePut(note, notecap, &k, ", nEscape=");
        WowNoteHex(note, notecap, &k, (DWORD)esc, 4);
        WowNotePut(note, notecap, &k, ", count=");
        WowNoteHex(note, notecap, &k, (DWORD)cnt, 4);
        WowNotePut(note, notecap, &k, ")");
        if (esc == WOWGDI_ESC_QUERYESCSUPPORT) {
            /* lpInData points at the escape number being asked about. */
            if (in) want = (INT)(WORD)(in[0] | (in[1] << 8));
            WowNotePut(note, notecap, &k, " -- QUERYESCSUPPORT for ");
            WowNoteHex(note, notecap, &k, (DWORD)want, 4);
            WowNotePut(note, notecap, &k, ": this DC supports QUERYESCSUPPORT and"
                                       " nothing else, so the caller is told NO"
                                       " before it tries");
            Wow32SetReturn(f, (DWORD)(want == WOWGDI_ESC_QUERYESCSUPPORT ? 1 : 0));
            return 1;
        }
        WowNotePut(note, notecap, &k, " -- ★ NOT IMPLEMENTED BY THIS DRIVER, which"
                                   " is what 0 MEANS for Escape (a screen DC has"
                                   " no printing escapes). A real printing path"
                                   " would need STARTDOC/NEWFRAME/ENDDOC and a"
                                   " 16-bit abort procedure; this host has no"
                                   " printer DC to start one on");
        Wow32SetReturn(f, 0);
        return 1;
    }

    /* ── 0x31 CreateBitmapIndirect(LPBITMAP) ─────────────────────────────────
         CreateBitmap's five arguments arriving in a 14-byte Win16 BITMAP instead
         of on the stack. Read field by field: Win32's BITMAP is 24 bytes with
         LONGs, so a cast would take bmWidth from the wrong half of bmType. */
    case WOWGDI_CREATEBITMAPINDIRECT: {
        const volatile BYTE *bp = Wow32ArgPointer(f, WOWGDI_CBI_ARG_BITMAP);
        INT  k = 0, w, h;
        WORD pl, bpp, tok;
        DWORD bits;
        HBITMAP bm;
        WowNotePut(note, notecap, &k, "CreateBitmapIndirect ");
        if (!bp) {
            WowNotePut(note, notecap, &k, "-- ★ the BITMAP far pointer does not"
                                       " resolve; answered 0");
            Wow32SetReturn(f, 0);
            return 1;
        }
        w   = (INT)(SHORT)WowGdiPeek(bp, CBI_OFF_WIDTH);
        h   = (INT)(SHORT)WowGdiPeek(bp, CBI_OFF_HEIGHT);
        pl  = bp[CBI_OFF_PLANES];
        bpp = bp[CBI_OFF_BPP];
        bits = (DWORD)WowGdiPeek(bp, CBI_OFF_BITS)
             | ((DWORD)WowGdiPeek(bp, CBI_OFF_BITS + 2) << 16);
        WowNoteHex(note, notecap, &k, (DWORD)w, 4);
        WowNotePut(note, notecap, &k, "x");
        WowNoteHex(note, notecap, &k, (DWORD)h, 4);
        WowNotePut(note, notecap, &k, " planes=");
        WowNoteHex(note, notecap, &k, pl, 2);
        WowNotePut(note, notecap, &k, " bpp=");
        WowNoteHex(note, notecap, &k, bpp, 2);
        if (w <= 0 || h <= 0 || !pl || !bpp) {
            WowNotePut(note, notecap, &k, " -- ★ A DIMENSION OR FORMAT IS NOT"
                                       " POSITIVE; answered 0");
            Wow32SetReturn(f, 0);
            return 1;
        }
        /* ⚠ bmBits IS IGNORED HERE ON PURPOSE, and the log says so: the pointer
             is a guest far pointer whose bytes we would have to resolve and
             validate exactly as CreateBitmap does. No guest measured passes one
             (WRITE passes 0), so the code that would follow it is not written
             blind -- it is left to the first run that needs it. */
        if (bits) WowNotePut(note, notecap, &k, " [★ bmBits IS NON-ZERO and is NOT"
                                             " being read -- the bitmap comes"
                                             " back UNINITIALISED]");
        bm = CreateBitmap(w, h, pl, bpp, NULL);
        tok = WowGdiH16((HGDIOBJ)bm, WOWGDI_KIND_OBJ);
        WowNotePut(note, notecap, &k, " -> 0x");
        WowNoteHex(note, notecap, &k, tok, 4);
        Wow32SetReturn(f, tok);
        return 1;
    }

    /* ── ★★ 0x133 GetCharABCWidths -- AND THE STRUCTURE IS HALF THE SIZE ─────
         See wowconv.h: Win16's ABC is 6 bytes, Win32's is 12, so the array is
         converted entry by entry into the guest's own buffer. Writing Win32's
         directly would overrun a correctly sized guest buffer by 2x.
       ⚠ ONLY A TRUETYPE FONT HAS ABC WIDTHS. Win32 fails the call for a raster
         font, and FALSE is the right answer to pass on -- a caller that gets it
         falls back to GetTextExtent, which this host implements. */
    case WOWGDI_GETCHARABCWIDTHS: {
        WORD hdc   = Wow32ArgWord(f, WOWGDI_ABCW_ARG_HDC);
        WORD first = Wow32ArgWord(f, WOWGDI_ABCW_ARG_FIRST);
        WORD last  = Wow32ArgWord(f, WOWGDI_ABCW_ARG_LAST);
        volatile BYTE *out = Wow32ArgPointer(f, WOWGDI_ABCW_ARG_ABC);
        HDC dc = (HDC)WowGdiH32(hdc, NULL);
        INT k = 0, n, i, ok = 0;
        WowNotePut(note, notecap, &k, "GetCharABCWidths(dc 0x");
        WowNoteHex(note, notecap, &k, hdc, 4);
        WowNotePut(note, notecap, &k, ", ");
        WowNoteHex(note, notecap, &k, first, 4);
        WowNotePut(note, notecap, &k, "..");
        WowNoteHex(note, notecap, &k, last, 4);
        WowNotePut(note, notecap, &k, ")");
        n = (INT)last - (INT)first + 1;
        if (!dc || !out || n <= 0 || n > 1024) {
            WowNotePut(note, notecap, &k, " -- ★ no DC, no buffer, or a range this"
                                       " host will not size a temporary for;"
                                       " answered FALSE");
            Wow32SetReturn(f, 0);
            return 1;
        }
        {   static ABC abc[1024];
            ok = GetCharABCWidthsA(dc, first, last, abc) ? 1 : 0;
            if (ok) {
                for (i = 0; i < n; ++i) {
                    long v[3];
                    BYTE six[WOWCONV_ABC16_SIZE];
                    INT b;
                    v[0] = (long)abc[i].abcA;
                    v[1] = (long)abc[i].abcB;
                    v[2] = (long)abc[i].abcC;
                    WowConvAbc32To16(v, six);
                    for (b = 0; b < WOWCONV_ABC16_SIZE; ++b)
                        out[i * WOWCONV_ABC16_SIZE + b] = six[b];
                }
                WowNotePut(note, notecap, &k, " -> 0x");
                WowNoteHex(note, notecap, &k, (DWORD)n, 4);
                WowNotePut(note, notecap, &k, " glyph(s), narrowed 12 bytes -> 6");
            } else {
                WowNotePut(note, notecap, &k, " -- FALSE from the OS (a raster font"
                                           " has no ABC widths; the caller falls"
                                           " back to GetTextExtent)");
            }
        }
        Wow32SetReturn(f, (DWORD)ok);
        return 1;
    }

    /* ── 0x16b GetPaletteEntries ──────────────────────────────────────────────
         PALETTEENTRY is four bytes in both worlds, so this one is a copy -- and
         knowing WHICH structures are identical is worth as much as knowing which
         are not (see wowconv.h). */
    case WOWGDI_SETPALETTEENTRIES: {
        WORD hpal  = Wow32ArgWord(f, WOWGDI_GPE_ARG_HPAL);
        WORD start = Wow32ArgWord(f, WOWGDI_GPE_ARG_START);
        WORD cnt   = Wow32ArgWord(f, WOWGDI_GPE_ARG_COUNT);
        volatile BYTE *in = Wow32ArgPointer(f, WOWGDI_GPE_ARG_ENTRIES);
        HPALETTE pal = (HPALETTE)WowGdiH32(hpal, NULL);
        static PALETTEENTRY pe[256];
        INT  k = 0, i;
        UINT got = 0;
        WowNotePut(note, notecap, &k, "SetPaletteEntries(0x");
        WowNoteHex(note, notecap, &k, hpal, 4);
        WowNotePut(note, notecap, &k, ", start="); WowNoteHex(note, notecap, &k, start, 4);
        WowNotePut(note, notecap, &k, ", n="); WowNoteHex(note, notecap, &k, cnt, 4);
        WowNotePut(note, notecap, &k, ")");
        if (!pal || GetObjectType((HGDIOBJ)pal) != OBJ_PAL || !in) {
            WowNotePut(note, notecap, &k, " -- ★ NOT ONE OF OUR PALETTES (or no entries); 0");
            Wow32SetReturn(f, 0); return 1;
        }
        if (cnt > 256) cnt = 256;
        for (i = 0; i < cnt; ++i) {             /* four bytes in both: a copy */
            pe[i].peRed = in[i * 4]; pe[i].peGreen = in[i * 4 + 1];
            pe[i].peBlue = in[i * 4 + 2]; pe[i].peFlags = in[i * 4 + 3];
        }
        got = SetPaletteEntries(pal, start, cnt, pe);
        WowNotePut(note, notecap, &k, " -> 0x"); WowNoteHex(note, notecap, &k, got, 4);
        Wow32SetReturn(f, got);
        return 1;
    }

    case WOWGDI_GETVIEWPORTEXT:
    case WOWGDI_GETWINDOWEXT:
    case WOWGDI_GETBITMAPDIMENSION: {
        WORD h = Wow32ArgWord(f, 0);
        INT  kind = -1, k = 0, ok = 0;
        HGDIOBJ o = WowGdiH32(h, &kind);
        SIZE sz;
        PCSTR nm = f->Id == WOWGDI_GETVIEWPORTEXT ? "GetViewportExt(0x"
                       : f->Id == WOWGDI_GETWINDOWEXT   ? "GetWindowExt(0x"
                       : "GetBitmapDimension(0x";
        sz.cx = sz.cy = 0;
        WowNotePut(note, notecap, &k, nm); WowNoteHex(note, notecap, &k, h, 4);
        WowNotePut(note, notecap, &k, ")");
        if (o && f->Id == WOWGDI_GETBITMAPDIMENSION) {
            if (GetObjectType(o) == OBJ_BITMAP) ok = GetBitmapDimensionEx((HBITMAP)o, &sz);
        } else if (o && (kind == WOWGDI_KIND_DC || kind == WOWGDI_KIND_WINDC)) {
            ok = f->Id == WOWGDI_GETVIEWPORTEXT ? GetViewportExtEx((HDC)o, &sz)
                                                : GetWindowExtEx((HDC)o, &sz);
        }
        if (!ok) { sz.cx = sz.cy = 0; WowNotePut(note, notecap, &k, " -- ★ not ours; 0"); }
        else { WowNotePut(note, notecap, &k, " -> "); WowNoteHex(note, notecap, &k, (DWORD)sz.cx, 4);
               WowNotePut(note, notecap, &k, "x"); WowNoteHex(note, notecap, &k, (DWORD)sz.cy, 4); }
        Wow32SetReturn(f, ((DWORD)(WORD)sz.cy << 16) | (WORD)sz.cx);
        return 1;
    }

    case WOWGDI_GETPALETTEENTRIES: {
        WORD hpal  = Wow32ArgWord(f, WOWGDI_GPE_ARG_HPAL);
        WORD start = Wow32ArgWord(f, WOWGDI_GPE_ARG_START);
        WORD cnt   = Wow32ArgWord(f, WOWGDI_GPE_ARG_COUNT);
        volatile BYTE *out = Wow32ArgPointer(f, WOWGDI_GPE_ARG_ENTRIES);
        HPALETTE pal = (HPALETTE)WowGdiH32(hpal, NULL);
        INT k = 0;
        UINT got = 0;
        WowNotePut(note, notecap, &k, "GetPaletteEntries(0x");
        WowNoteHex(note, notecap, &k, hpal, 4);
        WowNotePut(note, notecap, &k, ", start=");
        WowNoteHex(note, notecap, &k, start, 4);
        WowNotePut(note, notecap, &k, ", n=");
        WowNoteHex(note, notecap, &k, cnt, 4);
        WowNotePut(note, notecap, &k, ")");
        if (!pal) {
            WowNotePut(note, notecap, &k, " -- ★ NOT ONE OF OUR PALETTES; answered 0");
            Wow32SetReturn(f, 0);
            return 1;
        }
        /* ⚠ A NULL buffer is not an error: it is how a caller ASKS HOW MANY
             entries the palette has, and Win32 answers the same way. */
        if (!out) {
            got = GetPaletteEntries(pal, 0, 0, NULL);
            WowNotePut(note, notecap, &k, " -- a COUNT query -> 0x");
            WowNoteHex(note, notecap, &k, got, 4);
            Wow32SetReturn(f, got);
            return 1;
        }
        if (cnt > 256) cnt = 256;
        {   static PALETTEENTRY pe[256];
            UINT i;
            got = GetPaletteEntries(pal, start, cnt, pe);
            for (i = 0; i < got; ++i) {
                out[i * 4 + 0] = pe[i].peRed;
                out[i * 4 + 1] = pe[i].peGreen;
                out[i * 4 + 2] = pe[i].peBlue;
                out[i * 4 + 3] = pe[i].peFlags;
            }
        }
        WowNotePut(note, notecap, &k, " -> 0x");
        WowNoteHex(note, notecap, &k, got, 4);
        WowNotePut(note, notecap, &k, " entries");
        Wow32SetReturn(f, got);
        return 1;
    }

    case WOWGDI_ENUMOBJECTS: {
        WORD  hdc  = Wow32ArgWord(f, WOWGDI_EOB_ARG_HDC);
        WORD  typ  = Wow32ArgWord(f, WOWGDI_EOB_ARG_TYPE);
        DWORD proc = Wow32ArgDword(f, WOWGDI_EOB_ARG_PROC);
        DWORD lp   = Wow32ArgDword(f, WOWGDI_EOB_ARG_LPARAM);
        INT   kind = -1, k = 0;
        HGDIOBJ o = WowGdiH32(hdc, &kind);
        HDC   dc;
        INT   own = 0;
        WowNotePut(note, notecap, &k, "EnumObjects(0x");
        WowNoteHex(note, notecap, &k, hdc, 4);
        WowNotePut(note, notecap, &k, typ == OBJ_PEN ? ", OBJ_PEN" :
                                   typ == OBJ_BRUSH ? ", OBJ_BRUSH" : ", ?");
        WowNotePut(note, notecap, &k, ")");
        /* GDI's own 16-bit side already refuses a type outside 1..2 before the
           call reaches us (such calls never arrive); this is belt and braces,
           answering what that refusal answers. */
        if (typ != OBJ_PEN && typ != OBJ_BRUSH) {
            WowNotePut(note, notecap, &k, " -- not a pen or brush; 0");
            Wow32SetReturn(f, 0);
            return 1;
        }
        Wow32SetReturn(f, 1);                 /* the walk revises it to 0 on a stop */
        if (o && (kind == WOWGDI_KIND_DC || kind == WOWGDI_KIND_WINDC)) dc = (HDC)o;
        else { dc = GetDC(NULL); own = 1; }
        g_WowEnumFontCount = 0;
        if (dc) {
            EnumObjects(dc, typ, (GOBJENUMPROC)WowGdiObjectCollect, (LPARAM)typ);
            if (own) ReleaseDC(NULL, dc);
        }
        WowNotePut(note, notecap, &k, " -> 0x");
        WowNoteHex(note, notecap, &k, (DWORD)g_WowEnumFontCount, 4);
        WowNotePut(note, notecap, &k, " object(s)");
        if (!g_WowEnumFontCount || !f->IsCallbackAllowed) {
            if (!f->IsCallbackAllowed) WowNotePut(note, notecap, &k, " -- callbacks are not armed");
            if (!g_WowEnumFontCount) Wow32SetReturn(f, 0);
            return 1;
        }
        if (WowEnumBusy()) {
            WowNotePut(note, notecap, &k, " -- ★ AN ENUMERATION IS ALREADY RUNNING; REFUSED");
            Wow32SetReturn(f, 0);
            return 1;
        }
        if (!WowEnumBegin(WOWENUM_OBJECTS, proc, f->GuestDataSelector, lp,
                           (DWORD)(ULONG_PTR)(f->FrameBase + WOW32_OFF_RET), 0)) {
            WowNotePut(note, notecap, &k, " -- ★ the callback is not a usable far pointer");
            Wow32SetReturn(f, 0);
            return 1;
        }
        f->IsEnumerationRequested = 1;
        return 1;
    }

    case WOWGDI_ENUMFONTS:
    case WOWGDI_ENUMFONTFAMILIES: {
        INT   plain = (f->Id == WOWGDI_ENUMFONTS);
        WORD  hdc  = Wow32ArgWord(f, WOWGDI_EFF_ARG_HDC);
        DWORD proc = Wow32ArgDword(f, WOWGDI_EFF_ARG_PROC);
        DWORD lp   = Wow32ArgDword(f, WOWGDI_EFF_ARG_LPARAM);
        CHAR  fam[64];
        INT   havefam = Wow32ArgString(f, WOWGDI_EFF_ARG_FAMILY, fam, sizeof fam) && fam[0];
        INT   kind = -1, k = 0;
        HGDIOBJ o = WowGdiH32(hdc, &kind);
        HDC   dc;
        INT   own = 0;
        WowNotePut(note, notecap, &k, plain ? "EnumFonts(0x" : "EnumFontFamilies(0x");
        WowNoteHex(note, notecap, &k, hdc, 4);
        WowNotePut(note, notecap, &k, havefam ? ", \"" : ", NULL");
        if (havefam) { WowNotePut(note, notecap, &k, fam); WowNotePut(note, notecap, &k, "\""); }
        WowNotePut(note, notecap, &k, ")");
        Wow32SetReturn(f, 1);                 /* the walk revises it to 0 on a stop */
        if (o && (kind == WOWGDI_KIND_DC || kind == WOWGDI_KIND_WINDC)) dc = (HDC)o;
        else { dc = GetDC(NULL); own = 1; }
        g_WowEnumFontCount = 0;
        if (dc) {
            g_WowGdiFontIsPlain = plain;
            if (plain) EnumFontsA(dc, havefam ? fam : NULL,
                                  (FONTENUMPROCA)WowGdiFontCollect, 0);
            else EnumFontFamiliesA(dc, havefam ? fam : NULL, WowGdiFontCollect, 0);
            g_WowGdiFontIsPlain = 0;
            if (own) ReleaseDC(NULL, dc);
        }
        WowNotePut(note, notecap, &k, " -> 0x");
        WowNoteHex(note, notecap, &k, (DWORD)g_WowEnumFontCount, 4);
        WowNotePut(note, notecap, &k, " font(s)");
        if (!g_WowEnumFontCount || !f->IsCallbackAllowed) {
            if (!f->IsCallbackAllowed) WowNotePut(note, notecap, &k, " -- callbacks are not armed");
            return 1;
        }
        if (WowEnumBusy()) {
            WowNotePut(note, notecap, &k, " -- ★ AN ENUMERATION IS ALREADY RUNNING; REFUSED");
            return 1;
        }
        if (!WowEnumBegin(WOWENUM_FONTS, proc, f->GuestDataSelector, lp,
                           (DWORD)(ULONG_PTR)(f->FrameBase + WOW32_OFF_RET), 0)) {
            WowNotePut(note, notecap, &k, " -- ★ the callback is not a usable far pointer");
            return 1;
        }
        f->IsEnumerationRequested = 1;
        return 1;
    }

    case WOWGDI_LINEDDA: {
        INT x1 = (INT)(SHORT)Wow32ArgWord(f, WOWGDI_LDDA_ARG_X1);
        INT y1 = (INT)(SHORT)Wow32ArgWord(f, WOWGDI_LDDA_ARG_Y1);
        INT x2 = (INT)(SHORT)Wow32ArgWord(f, WOWGDI_LDDA_ARG_X2);
        INT y2 = (INT)(SHORT)Wow32ArgWord(f, WOWGDI_LDDA_ARG_Y2);
        DWORD proc = Wow32ArgDword(f, WOWGDI_LDDA_ARG_PROC);
        DWORD data = Wow32ArgDword(f, WOWGDI_LDDA_ARG_DATA);
        INT k = 0;
        WowNotePut(note, notecap, &k, "LineDDA (");
        WowNoteHex(note, notecap, &k, (DWORD)x1, 4);
        WowNotePut(note, notecap, &k, ",");
        WowNoteHex(note, notecap, &k, (DWORD)y1, 4);
        WowNotePut(note, notecap, &k, ")-(");
        WowNoteHex(note, notecap, &k, (DWORD)x2, 4);
        WowNotePut(note, notecap, &k, ",");
        WowNoteHex(note, notecap, &k, (DWORD)y2, 4);
        WowNotePut(note, notecap, &k, ") proc=0x");
        WowNoteHex(note, notecap, &k, proc, 8);
        Wow32SetReturn(f, 0);                       /* the function returns void */
        if (!f->IsCallbackAllowed) {
            WowNotePut(note, notecap, &k, " -- callbacks are not armed; no point was"
                                       " visited");
            return 1;
        }
        if (WowEnumBusy()) {
            WowNotePut(note, notecap, &k, " -- ★ AN ENUMERATION IS ALREADY RUNNING;"
                                       " REFUSED rather than sharing a cursor");
            return 1;
        }
        /* ⚠ THE DS IS THE CALLER'S OWN. A LineDDA callback is application code in
             the application's data segment, and this host has no class or window
             to take an instance from here -- so it is entered with the DS the
             guest itself is running on, which is what a MakeProcInstance thunk
             would have restored anyway. */
        if (!WowEnumBegin(WOWENUM_LINE, proc,
                           f->GuestDataSelector,
                           data, 0, 0)) {
            WowNotePut(note, notecap, &k, " -- ★ the callback is not a usable far"
                                       " pointer");
            return 1;
        }
        WowEnumLine(x1, y1, x2, y2);
        f->IsEnumerationRequested = 1;
        return 1;
    }

    case WOWGDI_CREATEBITMAP: {
        INT  w  = (INT)(SHORT)Wow32ArgWord(f, WOWGDI_CBM_ARG_WIDTH);
        INT  h  = (INT)(SHORT)Wow32ArgWord(f, WOWGDI_CBM_ARG_HEIGHT);
        WORD pl = Wow32ArgWord(f, WOWGDI_CBM_ARG_PLANES);
        WORD bp = Wow32ArgWord(f, WOWGDI_CBM_ARG_BPP);
        volatile BYTE *bits = Wow32ArgPointer(f, WOWGDI_CBM_ARG_BITS);
        HBITMAP bm;
        WORD tok;
        INT  k = 0;
        WowNotePut(note, notecap, &k, "CreateBitmap ");
        WowNoteHex(note, notecap, &k, (DWORD)w, 4);
        WowNotePut(note, notecap, &k, "x");
        WowNoteHex(note, notecap, &k, (DWORD)h, 4);
        WowNotePut(note, notecap, &k, " planes=");
        WowNoteHex(note, notecap, &k, pl, 2);
        WowNotePut(note, notecap, &k, " bpp=");
        WowNoteHex(note, notecap, &k, bp, 2);
        WowNotePut(note, notecap, &k, bits ? " with bits" : " uninitialised");
        if (w <= 0 || h <= 0 || !pl || !bp) {
            WowNotePut(note, notecap, &k, " -- ★ A DIMENSION OR FORMAT IS NOT"
                                       " POSITIVE; answered 0");
            Wow32SetReturn(f, 0);
            return 1;
        }
        if (bits) {
            DWORD stride = ((((DWORD)w * pl * bp) + 15) / 16) * 2;
            if (stride * (DWORD)h > 0x10000ul) {
                WowNotePut(note, notecap, &k, " -- ★ THE BITS WOULD RUN PAST A 64K"
                                           " SEGMENT; refused rather than letting"
                                           " GDI read past them; answered 0");
                Wow32SetReturn(f, 0);
                return 1;
            }
        }
        bm = CreateBitmap(w, h, (UINT)pl, (UINT)bp,
                          bits ? (const VOID *)(PCBYTE )bits : NULL);
        tok = bm ? WowGdiH16((HGDIOBJ)bm, WOWGDI_KIND_OBJ) : 0;
        if (!tok) {
            if (bm) DeleteObject((HGDIOBJ)bm);
            WowNotePut(note, notecap, &k, bm ? " -- ★ THE GDI TOKEN MAP IS FULL; the"
                                            " bitmap was freed and 0 answered"
                                          : " -- ★ THE OS REFUSED IT; answered 0");
            Wow32SetReturn(f, 0);
            return 1;
        }
        WowNotePut(note, notecap, &k, " -> bitmap token 0x");
        WowNoteHex(note, notecap, &k, tok, 4);
        Wow32SetReturn(f, tok);
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
        WORD hdc = Wow32ArgWord(f, WOWGDI_CCB_ARG_HDC);
        INT  w   = (INT)(SHORT)Wow32ArgWord(f, WOWGDI_CCB_ARG_WIDTH);
        INT  h   = (INT)(SHORT)Wow32ArgWord(f, WOWGDI_CCB_ARG_HEIGHT);
        INT  kind = -1;
        HGDIOBJ o = WowGdiH32(hdc, &kind);
        HBITMAP bm;
        WORD tok;
        INT k = 0;
        WowNotePut(note, notecap, &k, "CreateCompatibleBitmap(0x");
        WowNoteHex(note, notecap, &k, hdc, 4);
        WowNotePut(note, notecap, &k, ", ");
        WowNoteHex(note, notecap, &k, (DWORD)w, 4);
        WowNotePut(note, notecap, &k, "x");
        WowNoteHex(note, notecap, &k, (DWORD)h, 4);
        WowNotePut(note, notecap, &k, ")");
        if (!o || (kind != WOWGDI_KIND_DC && kind != WOWGDI_KIND_WINDC)) {
            WowNotePut(note, notecap, &k, " -- ★ THAT IS NOT ONE OF OUR DC TOKENS;"
                                       " answered 0");
            Wow32SetReturn(f, 0);
            return 1;
        }
        if (w <= 0 || h <= 0) {
            WowNotePut(note, notecap, &k, " -- ★ A DIMENSION IS NOT POSITIVE;"
                                       " answered 0 rather than widening it into"
                                       " a size nobody asked for");
            Wow32SetReturn(f, 0);
            return 1;
        }
        bm = CreateCompatibleBitmap((HDC)o, w, h);
        tok = bm ? WowGdiH16((HGDIOBJ)bm, WOWGDI_KIND_OBJ) : 0;
        if (!tok) {
            if (bm) DeleteObject((HGDIOBJ)bm);
            WowNotePut(note, notecap, &k, bm ? " -- ★ THE GDI TOKEN MAP IS FULL; the"
                                            " bitmap was freed and 0 answered"
                                          : " -- ★ THE OS REFUSED IT; answered 0");
            Wow32SetReturn(f, 0);
            return 1;
        }
        WowNotePut(note, notecap, &k, " -> bitmap token 0x");
        WowNoteHex(note, notecap, &k, tok, 4);
        Wow32SetReturn(f, tok);
        return 1;
    }

    /* ── ★ 0x42 CreateSolidBrush(crColor) ───────────────────────────────────
         A COLORREF is a DWORD and means the same thing in both worlds. */
    case WOWGDI_CREATESOLIDBRUSH: {
        DWORD col = Wow32ArgDword(f, WOWGDI_CSB_ARG_COLOR);
        HBRUSH br = CreateSolidBrush((COLORREF)col);
        WORD tok = br ? WowGdiH16((HGDIOBJ)br, WOWGDI_KIND_OBJ) : 0;
        INT k = 0;
        WowNotePut(note, notecap, &k, "CreateSolidBrush(0x");
        WowNoteHex(note, notecap, &k, col, 8);
        WowNotePut(note, notecap, &k, ")");
        if (!tok) {
            if (br) DeleteObject((HGDIOBJ)br);
            WowNotePut(note, notecap, &k, " -- ★ refused or the token map is full;"
                                       " answered 0");
            Wow32SetReturn(f, 0);
            return 1;
        }
        WowNotePut(note, notecap, &k, " -> brush token 0x");
        WowNoteHex(note, notecap, &k, tok, 4);
        Wow32SetReturn(f, tok);
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
        WORD hdc = Wow32ArgWord(f, WOWGDI_SEL_ARG_HDC);
        WORD hob = Wow32ArgWord(f, WOWGDI_SEL_ARG_OBJ);
        INT  dkind = -1, okind = -1;
        HGDIOBJ d = WowGdiH32(hdc, &dkind);
        HGDIOBJ o = WowGdiH32(hob, &okind);
        HGDIOBJ prev;
        WORD tok;
        INT k = 0;
        WowNotePut(note, notecap, &k, "SelectObject(dc 0x");
        WowNoteHex(note, notecap, &k, hdc, 4);
        WowNotePut(note, notecap, &k, ", obj 0x");
        WowNoteHex(note, notecap, &k, hob, 4);
        WowNotePut(note, notecap, &k, ")");
        if (!d || (dkind != WOWGDI_KIND_DC && dkind != WOWGDI_KIND_WINDC)) {
            WowNotePut(note, notecap, &k, " -- ★ THE DC IS NOT ONE OF OUR TOKENS;"
                                       " answered 0");
            Wow32SetReturn(f, 0);
            return 1;
        }
        if (!o) {
            WowNotePut(note, notecap, &k, " -- ★ THE OBJECT IS NOT ONE OF OUR TOKENS;"
                                       " answered 0");
            Wow32SetReturn(f, 0);
            return 1;
        }
        if (okind == WOWGDI_KIND_DC || okind == WOWGDI_KIND_WINDC) {
            WowNotePut(note, notecap, &k, " -- ★ THAT IS A DC, NOT A DRAWING OBJECT;"
                                       " refused");
            Wow32SetReturn(f, 0);
            return 1;
        }
        prev = SelectObject((HDC)d, o);
        if (!prev) {
            WowNotePut(note, notecap, &k, " -- ★ THE OS REFUSED THE SELECTION;"
                                       " answered 0");
            Wow32SetReturn(f, 0);
            return 1;
        }
        tok = WowGdiH16(prev, WOWGDI_KIND_OBJ);
        WowNotePut(note, notecap, &k, " -> previous 0x");
        WowNoteHex(note, notecap, &k, tok, 4);
        if (!tok)
            WowNotePut(note, notecap, &k, " -- ⚠ THE MAP IS FULL, so the guest cannot"
                                       " select it back");
        Wow32SetReturn(f, tok);
        return 1;
    }

    /* ── ★★★ 0x52 GetObject -- see the long note above. ─────────────────────*/
    case WOWGDI_GETOBJECT: {
        WORD h    = Wow32ArgWord(f, WOWGDI_GOB_ARG_HANDLE);
        WORD want = Wow32ArgWord(f, WOWGDI_GOB_ARG_COUNT);
        volatile BYTE *dst = Wow32ArgPointer(f, WOWGDI_GOB_ARG_BUF);
        INT kind = -1;
        HGDIOBJ o = WowGdiH32(h, &kind);
        BYTE b[WOWGDI_LOGFONT16_SIZE];
        DWORD type;
        INT n = 0, k = 0, i;
        PCSTR what = "?";

        WowNotePut(note, notecap, &k, "GetObject 0x");
        WowNoteHex(note, notecap, &k, h, 4);
        WowNotePut(note, notecap, &k, " cb=0x");
        WowNoteHex(note, notecap, &k, want, 4);
        if (!o) {
            WowNotePut(note, notecap, &k, " -- ★ NOT ONE OF OUR GDI TOKENS;"
                                       " answered 0");
            Wow32SetReturn(f, 0);
            return 1;
        }
        for (i = 0; i < (INT)sizeof b; ++i) b[i] = 0;
        type = GetObjectType(o);
        if (type == OBJ_BITMAP) {
            BITMAP bm;
            if (!GetObject(o, (INT)sizeof bm, &bm)) type = 0;
            else {
                what = "BITMAP";
                n = WOWGDI_BITMAP16_SIZE;
                Wow32PokeWord(b + 0,  (WORD)(SHORT)bm.bmType);
                Wow32PokeWord(b + 2,  (WORD)(SHORT)bm.bmWidth);
                Wow32PokeWord(b + 4,  (WORD)(SHORT)bm.bmHeight);
                Wow32PokeWord(b + 6,  (WORD)(SHORT)bm.bmWidthBytes);
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
                {   WowNotePut(note, notecap, &k, " ");
                    WowNoteHex(note, notecap, &k, (DWORD)(WORD)bm.bmWidth, 4);
                    WowNotePut(note, notecap, &k, "x");
                    WowNoteHex(note, notecap, &k, (DWORD)(WORD)bm.bmHeight, 4);
                    WowNotePut(note, notecap, &k, " planes=");
                    WowNoteHex(note, notecap, &k, (DWORD)bm.bmPlanes, 2);
                    WowNotePut(note, notecap, &k, " bpp=");
                    WowNoteHex(note, notecap, &k, (DWORD)bm.bmBitsPixel, 2);
                }
                /* ⚠ bmBits STAYS NULL, and that is correct rather than lazy: it
                     is a 32-bit host pointer with no 16-bit address, and Windows
                     itself returns NULL here for a device-dependent bitmap. */
            }
        } else if (type == OBJ_FONT) {
            LOGFONTA lf;
            if (!GetObjectA(o, (INT)sizeof lf, &lf)) type = 0;
            else {
                what = "LOGFONT";
                n = WOWGDI_LOGFONT16_SIZE;
                Wow32PokeWord(b + 0,  (WORD)(SHORT)lf.lfHeight);
                Wow32PokeWord(b + 2,  (WORD)(SHORT)lf.lfWidth);
                Wow32PokeWord(b + 4,  (WORD)(SHORT)lf.lfEscapement);
                Wow32PokeWord(b + 6,  (WORD)(SHORT)lf.lfOrientation);
                Wow32PokeWord(b + 8,  (WORD)(SHORT)lf.lfWeight);
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
                {   INT j = 0;
                    for (j = 0; j < 32 && lf.lfFaceName[j]; ++j) { }
                    WowNotePut(note, notecap, &k, " h=");
                    WowNoteHex(note, notecap, &k, (DWORD)(WORD)(SHORT)lf.lfHeight, 4);
                    WowNotePut(note, notecap, &k, " w=");
                    WowNoteHex(note, notecap, &k, (DWORD)(WORD)(SHORT)lf.lfWidth, 4);
                    WowNotePut(note, notecap, &k, " wt=");
                    WowNoteHex(note, notecap, &k, (DWORD)(WORD)(SHORT)lf.lfWeight, 4);
                    WowNotePut(note, notecap, &k, " ");
                    WowNoteQuoted(note, notecap, &k, lf.lfFaceName);
                }
            }
        } else if (type == OBJ_PEN || type == OBJ_EXTPEN) {
            LOGPEN lp;
            if (!GetObject(o, (INT)sizeof lp, &lp)) type = 0;
            else {
                what = "LOGPEN";
                n = WOWGDI_LOGPEN16_SIZE;
                Wow32PokeWord(b + 0, (WORD)lp.lopnStyle);
                Wow32PokeWord(b + 2, (WORD)(SHORT)lp.lopnWidth.x);
                Wow32PokeWord(b + 4, (WORD)(SHORT)lp.lopnWidth.y);
                Wow32PokeWord(b + 6, (WORD)(lp.lopnColor & 0xFFFF));
                Wow32PokeWord(b + 8, (WORD)(lp.lopnColor >> 16));
            }
        } else if (type == OBJ_BRUSH) {
            LOGBRUSH lb;
            if (!GetObject(o, (INT)sizeof lb, &lb)) type = 0;
            else {
                what = "LOGBRUSH";
                n = WOWGDI_LOGBRUSH16_SIZE;
                Wow32PokeWord(b + 0, (WORD)lb.lbStyle);
                Wow32PokeWord(b + 2, (WORD)(lb.lbColor & 0xFFFF));
                Wow32PokeWord(b + 4, (WORD)(lb.lbColor >> 16));
                Wow32PokeWord(b + 6, (WORD)lb.lbHatch);
            }
        } else {
            type = 0;
        }
        if (!type || !n) {
            WowNotePut(note, notecap, &k, " -- ★ THE OS WILL NOT DESCRIBE THAT OBJECT"
                                       " (not a bitmap, font, pen or brush);"
                                       " answered 0");
            Wow32SetReturn(f, 0);
            return 1;
        }
        WowNotePut(note, notecap, &k, " -> ");
        WowNotePut(note, notecap, &k, what);
        WowNotePut(note, notecap, &k, " (Win16 form is 0x");
        WowNoteHex(note, notecap, &k, (DWORD)n, 2);
        WowNotePut(note, notecap, &k, " bytes)");
        /* ★ A NULL buffer is a legal QUERY: Win16 answers the size needed. */
        if (!dst) {
            WowNotePut(note, notecap, &k, " -- lpObject is NULL, so this is a size"
                                       " query");
            Wow32SetReturn(f, (DWORD)n);
            return 1;
        }
        if ((INT)want < n) {
            n = (INT)want;                 /* a partial read is allowed */
            WowNotePut(note, notecap, &k, " -- PARTIAL, the guest asked for less");
        }
        for (i = 0; i < n; ++i) dst[i] = b[i];
        Wow32SetReturn(f, (DWORD)n);
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
        INT  wantdc = (f->Id == WOWGDI_DELETEDC);
        WORD h = Wow32ArgWord(f, WOWGDI_DOBJ_ARG_HANDLE);
        INT  kind = -1;
        HGDIOBJ o = WowGdiH32(h, &kind);
        INT k = 0, ok;
        INT want = wantdc ? WOWGDI_KIND_DC : WOWGDI_KIND_OBJ;
        WowNotePut(note, notecap, &k, wantdc ? "DeleteDC 0x" : "DeleteObject 0x");
        WowNoteHex(note, notecap, &k, h, 4);
        if (!o) {
            WowNotePut(note, notecap, &k, " -- ★ NOT ONE OF OUR GDI TOKENS;"
                                       " answered 0");
            Wow32SetReturn(f, 0);
            return 1;
        }
        /* ⚠ A STOCK OBJECT IS NOT DESTROYED, AND THE CALL STILL SUCCEEDS. The
             system owns it; Windows makes DeleteObject on one a no-op that
             reports success, and answering anything else would make a guest
             think its cleanup had failed. The token is KEPT, because the object
             is still there and the guest may well ask for it again. */
        if (kind == WOWGDI_KIND_STOCK) {
            WowNotePut(note, notecap, &k, " -- ★ that is a STOCK object; the system"
                                       " owns it, so nothing was destroyed and"
                                       " success answered (as Windows does)");
            Wow32SetReturn(f, 1);
            return 1;
        }
        /* ⚠ A BORROWED DC IS THE INTERESTING REFUSAL. DeleteDC on a DC that came
             from GetDC does not fail visibly on Win32 -- it damages the window's
             DC cache and the symptom surfaces somewhere else entirely. Say which
             call it should have been. */
        if (kind != want) {
            WowNotePut(note, notecap, &k,
                    kind == WOWGDI_KIND_WINDC
                        ? " -- ★ THAT DC WAS BORROWED FROM A WINDOW (GetDC);"
                          " it must go back through ReleaseDC, not this. Refused"
                    : kind == WOWGDI_KIND_DC
                        ? " -- ★ THAT IS A DC AND THIS IS DeleteObject; refused"
                        : " -- ★ THAT IS NOT A DC AND THIS IS DeleteDC; refused");
            Wow32SetReturn(f, 0);
            return 1;
        }
        ok = wantdc ? (DeleteDC((HDC)o) ? 1 : 0) : (DeleteObject(o) ? 1 : 0);
        if (ok) WowGdiForget(h);
        WowNotePut(note, notecap, &k, ok ? " -> deleted, token released"
                                      : " -- ★ the OS refused the delete");
        Wow32SetReturn(f, (DWORD)ok);
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
        INT  isell = (f->Id == WOWGDI_ELLIPSE);
        WORD hdc = Wow32ArgWord(f, WOWGDI_RC_ARG_HDC);
        INT  l = (INT)(SHORT)Wow32ArgWord(f, WOWGDI_RC_ARG_LEFT);
        INT  t = (INT)(SHORT)Wow32ArgWord(f, WOWGDI_RC_ARG_TOP);
        INT  r = (INT)(SHORT)Wow32ArgWord(f, WOWGDI_RC_ARG_RIGHT);
        INT  b = (INT)(SHORT)Wow32ArgWord(f, WOWGDI_RC_ARG_BOTTOM);
        INT  kind = -1;
        HGDIOBJ o = WowGdiH32(hdc, &kind);
        INT  k = 0;
        WowNotePut(note, notecap, &k, isell ? "Ellipse(0x" : "ExcludeClipRect(0x");
        WowNoteHex(note, notecap, &k, hdc, 4);
        WowNotePut(note, notecap, &k, ", ");
        WowNoteHex(note, notecap, &k, (DWORD)l, 4);
        WowNotePut(note, notecap, &k, ",");
        WowNoteHex(note, notecap, &k, (DWORD)t, 4);
        WowNotePut(note, notecap, &k, " ");
        WowNoteHex(note, notecap, &k, (DWORD)r, 4);
        WowNotePut(note, notecap, &k, ",");
        WowNoteHex(note, notecap, &k, (DWORD)b, 4);
        WowNotePut(note, notecap, &k, ")");
        if (!o || (kind != WOWGDI_KIND_DC && kind != WOWGDI_KIND_WINDC)) {
            WowNotePut(note, notecap, &k, " -- ★ NOT ONE OF OUR DC TOKENS;"
                                       " answered 0");
            Wow32SetReturn(f, 0);
            return 1;
        }
        if (isell) {
            Wow32SetReturn(f, (DWORD)(Ellipse((HDC)o, l, t, r, b) ? 1 : 0));
        } else {
            INT rc = ExcludeClipRect((HDC)o, l, t, r, b);
            WowNotePut(note, notecap, &k, " -> region complexity ");
            WowNoteHex(note, notecap, &k, (DWORD)rc, 4);
            Wow32SetReturn(f, (DWORD)(WORD)rc);
        }
        return 1;
    }

    /* ── ★ 0x1c RoundRect(hDC, l, t, r, b, ellipseW, ellipseH) -- 14 bytes. ──*/
    case WOWGDI_ROUNDRECT: {
        WORD hdc = Wow32ArgWord(f, WOWGDI_RR_ARG_HDC);
        INT  l  = (INT)(SHORT)Wow32ArgWord(f, WOWGDI_RR_ARG_LEFT);
        INT  t  = (INT)(SHORT)Wow32ArgWord(f, WOWGDI_RR_ARG_TOP);
        INT  r  = (INT)(SHORT)Wow32ArgWord(f, WOWGDI_RR_ARG_RIGHT);
        INT  b  = (INT)(SHORT)Wow32ArgWord(f, WOWGDI_RR_ARG_BOTTOM);
        INT  ew = (INT)(SHORT)Wow32ArgWord(f, WOWGDI_RR_ARG_EW);
        INT  eh = (INT)(SHORT)Wow32ArgWord(f, WOWGDI_RR_ARG_EH);
        INT  kind = -1;
        HGDIOBJ o = WowGdiH32(hdc, &kind);
        INT  k = 0;
        WowNotePut(note, notecap, &k, "RoundRect(0x");
        WowNoteHex(note, notecap, &k, hdc, 4);
        WowNotePut(note, notecap, &k, ", ");
        WowNoteHex(note, notecap, &k, (DWORD)l, 4);
        WowNotePut(note, notecap, &k, ",");
        WowNoteHex(note, notecap, &k, (DWORD)t, 4);
        WowNotePut(note, notecap, &k, " ");
        WowNoteHex(note, notecap, &k, (DWORD)r, 4);
        WowNotePut(note, notecap, &k, ",");
        WowNoteHex(note, notecap, &k, (DWORD)b, 4);
        WowNotePut(note, notecap, &k, " corner ");
        WowNoteHex(note, notecap, &k, (DWORD)ew, 4);
        WowNotePut(note, notecap, &k, "x");
        WowNoteHex(note, notecap, &k, (DWORD)eh, 4);
        WowNotePut(note, notecap, &k, ")");
        if (!o || (kind != WOWGDI_KIND_DC && kind != WOWGDI_KIND_WINDC)) {
            WowNotePut(note, notecap, &k, " -- ★ NOT ONE OF OUR DC TOKENS;"
                                       " answered 0");
            Wow32SetReturn(f, 0);
            return 1;
        }
        Wow32SetReturn(f, (DWORD)(RoundRect((HDC)o, l, t, r, b, ew, eh) ? 1 : 0));
        return 1;
    }

    /* ── ★★★★★ 0x174 ExtFloodFill -- THE FILL TOOL. ─────────────────────────
       ⚠ THE FAILURE MODE THIS REPLACES IS SILENCE. Unimplemented, the call was
         stepped over and answered with the harness sentinel 0 -- which is
         ExtFloodFill's own "I filled nothing", so Paint had no way to tell a
         host that cannot fill from a fill that had nothing to do. The tool
         appeared to be selected, appeared to take the click, and did nothing.
       ★ Paint calls it twice per fill: once with a solid brush selected and once
         with a pattern brush from `CreatePatternBrush`, which is
         how a Win16 program fills with one of the palette's patterns. */
    case WOWGDI_EXTFLOODFILL: {
        WORD  hdc  = Wow32ArgWord(f, WOWGDI_FF_ARG_HDC);
        INT   x    = (INT)(SHORT)Wow32ArgWord(f, WOWGDI_FF_ARG_X);
        INT   y    = (INT)(SHORT)Wow32ArgWord(f, WOWGDI_FF_ARG_Y);
        DWORD col  = Wow32ArgDword(f, WOWGDI_FF_ARG_COLOR);
        WORD  type = Wow32ArgWord(f, WOWGDI_FF_ARG_TYPE);
        INT   kind = -1;
        HGDIOBJ o = WowGdiH32(hdc, &kind);
        INT   k = 0, ok;
        WowNotePut(note, notecap, &k, "ExtFloodFill(0x");
        WowNoteHex(note, notecap, &k, hdc, 4);
        WowNotePut(note, notecap, &k, ", ");
        WowNoteHex(note, notecap, &k, (DWORD)x, 4);
        WowNotePut(note, notecap, &k, ",");
        WowNoteHex(note, notecap, &k, (DWORD)y, 4);
        WowNotePut(note, notecap, &k, " colour 0x");
        WowNoteHex(note, notecap, &k, col, 8);
        WowNotePut(note, notecap, &k, type ? " SURFACE)" : " BORDER)");
        if (!o || (kind != WOWGDI_KIND_DC && kind != WOWGDI_KIND_WINDC)) {
            WowNotePut(note, notecap, &k, " -- ★ NOT ONE OF OUR DC TOKENS;"
                                       " answered 0");
            Wow32SetReturn(f, 0);
            return 1;
        }
        ok = ExtFloodFill((HDC)o, x, y, (COLORREF)col, (UINT)type) ? 1 : 0;
        WowNotePut(note, notecap, &k, ok ? " -> filled" : " -> the OS filled NOTHING");
        Wow32SetReturn(f, (DWORD)ok);
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
        INT   ispen = (f->Id == WOWGDI_CREATEPEN);
        DWORD col   = Wow32ArgDword(f, ispen ? WOWGDI_CP_ARG_COLOR : WOWGDI_CH_ARG_COLOR);
        INT   a     = (INT)(SHORT)Wow32ArgWord(f, ispen ? WOWGDI_CP_ARG_WIDTH : WOWGDI_CH_ARG_INDEX);
        INT   style = ispen ? (INT)(SHORT)Wow32ArgWord(f, WOWGDI_CP_ARG_STYLE) : 0;
        INT   k = 0;
        HGDIOBJ obj;
        WORD tok;
        WowNotePut(note, notecap, &k, ispen ? "CreatePen(style " : "CreateHatchBrush(index ");
        WowNoteHex(note, notecap, &k, (DWORD)(ispen ? style : a), 4);
        if (ispen) {
            WowNotePut(note, notecap, &k, ", width ");
            WowNoteHex(note, notecap, &k, (DWORD)a, 4);
        }
        WowNotePut(note, notecap, &k, ", 0x");
        WowNoteHex(note, notecap, &k, col, 8);
        WowNotePut(note, notecap, &k, ")");
        obj = ispen ? (HGDIOBJ)CreatePen(style, a, (COLORREF)col)
                    : (HGDIOBJ)CreateHatchBrush(a, (COLORREF)col);
        tok = obj ? WowGdiH16(obj, WOWGDI_KIND_OBJ) : 0;
        if (!tok) {
            if (obj) DeleteObject(obj);
            WowNotePut(note, notecap, &k, " -- ★ the OS refused it (or the token map"
                                       " is full); answered 0");
            Wow32SetReturn(f, 0);
            return 1;
        }
        WowNotePut(note, notecap, &k, ispen ? " -> pen token 0x" : " -> brush token 0x");
        WowNoteHex(note, notecap, &k, tok, 4);
        Wow32SetReturn(f, tok);
        return 1;
    }

    /* ── ★ 0x1f SetPixel(hDC, x, y, crColor) -- GetPixel's twin, 10 bytes. ───
       ⚠ The answer is the colour ACTUALLY set, which on a device that cannot
         represent the request is not the colour asked for -- so it is returned
         as the OS gives it rather than echoed. */
    case WOWGDI_SETPIXEL: {
        WORD  hdc = Wow32ArgWord(f, WOWGDI_SP_ARG_HDC);
        INT   x = (INT)(SHORT)Wow32ArgWord(f, WOWGDI_SP_ARG_X);
        INT   y = (INT)(SHORT)Wow32ArgWord(f, WOWGDI_SP_ARG_Y);
        DWORD col = Wow32ArgDword(f, WOWGDI_SP_ARG_COLOR);
        INT   kind = -1;
        HGDIOBJ o = WowGdiH32(hdc, &kind);
        INT   k = 0;
        COLORREF got;
        WowNotePut(note, notecap, &k, "SetPixel(0x");
        WowNoteHex(note, notecap, &k, hdc, 4);
        WowNotePut(note, notecap, &k, ", ");
        WowNoteHex(note, notecap, &k, (DWORD)x, 4);
        WowNotePut(note, notecap, &k, ",");
        WowNoteHex(note, notecap, &k, (DWORD)y, 4);
        WowNotePut(note, notecap, &k, " 0x");
        WowNoteHex(note, notecap, &k, col, 8);
        WowNotePut(note, notecap, &k, ")");
        if (!o || (kind != WOWGDI_KIND_DC && kind != WOWGDI_KIND_WINDC)) {
            WowNotePut(note, notecap, &k, " -- ★ NOT ONE OF OUR DC TOKENS;"
                                       " answered CLR_INVALID");
            Wow32SetReturn(f, 0xFFFFFFFFu);
            return 1;
        }
        got = SetPixel((HDC)o, x, y, (COLORREF)col);
        Wow32SetReturn(f, (DWORD)got);
        return 1;
    }

    /* ── ★ 0x3c CreatePatternBrush(hBitmap) -- one of OUR bitmap tokens. ─────*/
    case WOWGDI_CREATEPATTERNBRUSH: {
        WORD hbm = Wow32ArgWord(f, WOWGDI_ONE_ARG_HANDLE);
        INT  kind = -1;
        HGDIOBJ o = WowGdiH32(hbm, &kind);
        INT  k = 0;
        HBRUSH br;
        WORD tok;
        WowNotePut(note, notecap, &k, "CreatePatternBrush(0x");
        WowNoteHex(note, notecap, &k, hbm, 4);
        WowNotePut(note, notecap, &k, ")");
        if (!o || (kind != WOWGDI_KIND_OBJ && kind != WOWGDI_KIND_STOCK)) {
            WowNotePut(note, notecap, &k, " -- ★ NOT ONE OF OUR BITMAP TOKENS;"
                                       " answered 0");
            Wow32SetReturn(f, 0);
            return 1;
        }
        br = CreatePatternBrush((HBITMAP)o);
        tok = br ? WowGdiH16((HGDIOBJ)br, WOWGDI_KIND_OBJ) : 0;
        if (!tok) {
            if (br) DeleteObject((HGDIOBJ)br);
            WowNotePut(note, notecap, &k, " -- ★ no brush (or the token map is"
                                       " full); answered 0");
            Wow32SetReturn(f, 0);
            return 1;
        }
        WowNotePut(note, notecap, &k, " -> brush token 0x");
        WowNoteHex(note, notecap, &k, tok, 4);
        Wow32SetReturn(f, tok);
        return 1;
    }

    /* ── ★ 0x53 GetPixel(hDC, x, y) ─────────────────────────────────────────
       ⚠ CLR_INVALID IS 0xFFFFFFFF AND IT IS NOT AN ERROR CODE THE GUEST CAN
         MISREAD AS A COLOUR -- Win16 uses the same value for the same reason, so
         a point outside the clip region travels through unchanged. */
    case WOWGDI_GETPIXEL: {
        WORD hdc = Wow32ArgWord(f, WOWGDI_XY_ARG_HDC);
        INT  x = (INT)(SHORT)Wow32ArgWord(f, WOWGDI_XY_ARG_X);
        INT  y = (INT)(SHORT)Wow32ArgWord(f, WOWGDI_XY_ARG_Y);
        INT  kind = -1;
        HGDIOBJ o = WowGdiH32(hdc, &kind);
        INT  k = 0;
        COLORREF c;
        WowNotePut(note, notecap, &k, "GetPixel(0x");
        WowNoteHex(note, notecap, &k, hdc, 4);
        WowNotePut(note, notecap, &k, ", ");
        WowNoteHex(note, notecap, &k, (DWORD)x, 4);
        WowNotePut(note, notecap, &k, ",");
        WowNoteHex(note, notecap, &k, (DWORD)y, 4);
        WowNotePut(note, notecap, &k, ")");
        if (!o || (kind != WOWGDI_KIND_DC && kind != WOWGDI_KIND_WINDC)) {
            WowNotePut(note, notecap, &k, " -- ★ NOT ONE OF OUR DC TOKENS;"
                                       " answered CLR_INVALID");
            Wow32SetReturn(f, 0xFFFFFFFFu);
            return 1;
        }
        c = GetPixel((HDC)o, x, y);
        WowNotePut(note, notecap, &k, " = 0x");
        WowNoteHex(note, notecap, &k, (DWORD)c, 8);
        Wow32SetReturn(f, (DWORD)c);
        return 1;
    }

    /* ── ★ Three one-DC questions: 0x4b GetBkColor, 0x55 GetROP2,
         0x16e UpdateColors. ⚠ GetBkColor's answer is a DWORD COLORREF and the
         other two are ints, which is the only difference between them here. */
    case WOWGDI_GETBKCOLOR:
    case WOWGDI_GETROP2:
    case WOWGDI_UPDATECOLORS:
    case WOWGDI_GETBKMODE:
    case WOWGDI_GETMAPMODE:
    case WOWGDI_GETPOLYFILLMODE:
    case WOWGDI_GETSTRETCHBLTMODE:
    case WOWGDI_GETTEXTCOLOR: {
        WORD hdc = Wow32ArgWord(f, WOWGDI_ONE_ARG_HANDLE);
        INT  kind = -1;
        HGDIOBJ o = WowGdiH32(hdc, &kind);
        INT  k = 0;
        WowNotePut(note, notecap, &k,
                f->Id == WOWGDI_GETBKCOLOR ? "GetBkColor(0x" :
                f->Id == WOWGDI_GETROP2    ? "GetROP2(0x" :
                f->Id == WOWGDI_GETBKMODE  ? "GetBkMode(0x" :
                f->Id == WOWGDI_GETMAPMODE ? "GetMapMode(0x" :
                f->Id == WOWGDI_GETPOLYFILLMODE ? "GetPolyFillMode(0x" :
                f->Id == WOWGDI_GETSTRETCHBLTMODE ? "GetStretchBltMode(0x" :
                f->Id == WOWGDI_GETTEXTCOLOR ? "GetTextColor(0x"
                                           : "UpdateColors(0x");
        WowNoteHex(note, notecap, &k, hdc, 4);
        WowNotePut(note, notecap, &k, ")");
        if (!o || (kind != WOWGDI_KIND_DC && kind != WOWGDI_KIND_WINDC)) {
            WowNotePut(note, notecap, &k, " -- ★ NOT ONE OF OUR DC TOKENS;"
                                       " answered 0");
            Wow32SetReturn(f, 0);
            return 1;
        }
        if (f->Id == WOWGDI_GETBKCOLOR || f->Id == WOWGDI_GETTEXTCOLOR) {
            COLORREF c = f->Id == WOWGDI_GETBKCOLOR ? GetBkColor((HDC)o)
                                                    : GetTextColor((HDC)o);
            WowNotePut(note, notecap, &k, " = 0x");
            WowNoteHex(note, notecap, &k, (DWORD)c, 8);
            Wow32SetReturn(f, (DWORD)c);
        } else if (f->Id != WOWGDI_UPDATECOLORS) {
            INT v = f->Id == WOWGDI_GETROP2     ? GetROP2((HDC)o) :
                    f->Id == WOWGDI_GETBKMODE   ? GetBkMode((HDC)o) :
                    f->Id == WOWGDI_GETMAPMODE  ? GetMapMode((HDC)o) :
                    f->Id == WOWGDI_GETPOLYFILLMODE ? GetPolyFillMode((HDC)o)
                                                : GetStretchBltMode((HDC)o);
            WowNotePut(note, notecap, &k, " = ");
            WowNoteHex(note, notecap, &k, (DWORD)v, 4);
            Wow32SetReturn(f, (DWORD)(WORD)v);
        } else {
            Wow32SetReturn(f, (DWORD)(UpdateColors((HDC)o) ? 1 : 0));
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
        INT  l = (INT)(SHORT)Wow32ArgWord(f, WOWGDI_RGN_ARG_LEFT);
        INT  t = (INT)(SHORT)Wow32ArgWord(f, WOWGDI_RGN_ARG_TOP);
        INT  r = (INT)(SHORT)Wow32ArgWord(f, WOWGDI_RGN_ARG_RIGHT);
        INT  b = (INT)(SHORT)Wow32ArgWord(f, WOWGDI_RGN_ARG_BOTTOM);
        INT  k = 0;
        HRGN rgn = CreateRectRgn(l, t, r, b);
        WORD tok = rgn ? WowGdiH16((HGDIOBJ)rgn, WOWGDI_KIND_OBJ) : 0;
        WowNotePut(note, notecap, &k, "CreateRectRgn(");
        WowNoteHex(note, notecap, &k, (DWORD)l, 4);
        WowNotePut(note, notecap, &k, ",");
        WowNoteHex(note, notecap, &k, (DWORD)t, 4);
        WowNotePut(note, notecap, &k, " ");
        WowNoteHex(note, notecap, &k, (DWORD)r, 4);
        WowNotePut(note, notecap, &k, ",");
        WowNoteHex(note, notecap, &k, (DWORD)b, 4);
        WowNotePut(note, notecap, &k, ")");
        if (!tok) {
            if (rgn) DeleteObject((HGDIOBJ)rgn);
            WowNotePut(note, notecap, &k, " -- ★ no region (or the token map is"
                                       " full); answered 0");
            Wow32SetReturn(f, 0);
            return 1;
        }
        WowNotePut(note, notecap, &k, " -> region token 0x");
        WowNoteHex(note, notecap, &k, tok, 4);
        Wow32SetReturn(f, tok);
        return 1;
    }

    case WOWGDI_SELECTCLIPRGN: {
        WORD hdc  = Wow32ArgWord(f, WOWGDI_SCR_ARG_HDC);
        WORD hrgn = Wow32ArgWord(f, WOWGDI_SCR_ARG_RGN);
        INT  dkind = -1, rkind = -1;
        HGDIOBJ d = WowGdiH32(hdc, &dkind);
        HGDIOBJ r = WowGdiH32(hrgn, &rkind);
        INT  k = 0, rc;
        WowNotePut(note, notecap, &k, "SelectClipRgn(0x");
        WowNoteHex(note, notecap, &k, hdc, 4);
        WowNotePut(note, notecap, &k, ", 0x");
        WowNoteHex(note, notecap, &k, hrgn, 4);
        WowNotePut(note, notecap, &k, ")");
        if (!d || (dkind != WOWGDI_KIND_DC && dkind != WOWGDI_KIND_WINDC)) {
            WowNotePut(note, notecap, &k, " -- ★ NOT ONE OF OUR DC TOKENS;"
                                       " answered 0");
            Wow32SetReturn(f, 0);
            return 1;
        }
        if (hrgn && !r) {
            WowNotePut(note, notecap, &k, " -- ★ NOT ONE OF OUR REGION TOKENS;"
                                       " answered 0 rather than clearing the"
                                       " clip region, which is what NULL means");
            Wow32SetReturn(f, 0);
            return 1;
        }
        if (!hrgn) WowNotePut(note, notecap, &k, " -- NULL = remove the clip region");
        rc = SelectClipRgn((HDC)d, (HRGN)r);
        WowNotePut(note, notecap, &k, " -> region complexity ");
        WowNoteHex(note, notecap, &k, (DWORD)rc, 4);
        Wow32SetReturn(f, (DWORD)(WORD)rc);
        return 1;
    }

    /* ── 0x2f CombineRgn(hDest, hSrc1, hSrc2, mode). ─────────────────────────
       ⚠ hSrc2 IS LEGITIMATELY NULL for RGN_COPY, so an absent second source is
         only an error when the mode actually needs one. Refusing it outright
         would break the idiomatic "copy this region" call. */
    case WOWGDI_COMBINERGN: {
        WORD hd = Wow32ArgWord(f, WOWGDI_CBR_ARG_DEST), h1 = Wow32ArgWord(f, WOWGDI_CBR_ARG_SRC1);
        WORD h2 = Wow32ArgWord(f, WOWGDI_CBR_ARG_SRC2), md = Wow32ArgWord(f, WOWGDI_CBR_ARG_MODE);
        INT kd = -1, k1 = -1, k2 = -1, k = 0, rc;
        HGDIOBJ d = WowGdiH32(hd, &kd), s1 = WowGdiH32(h1, &k1),
                s2 = WowGdiH32(h2, &k2);
        WowNotePut(note, notecap, &k, "CombineRgn(0x");
        WowNoteHex(note, notecap, &k, hd, 4);
        WowNotePut(note, notecap, &k, ", 0x"); WowNoteHex(note, notecap, &k, h1, 4);
        WowNotePut(note, notecap, &k, ", 0x"); WowNoteHex(note, notecap, &k, h2, 4);
        WowNotePut(note, notecap, &k, ", mode "); WowNoteHex(note, notecap, &k, md, 2);
        WowNotePut(note, notecap, &k, ")");
        if (!d || !s1 || (h2 && !s2)) {
            WowNotePut(note, notecap, &k, " -- ★ NOT ONE OF OUR REGION TOKENS;"
                                       " answered ERROR");
            Wow32SetReturn(f, 0);                       /* ERROR */
            return 1;
        }
        rc = CombineRgn((HRGN)d, (HRGN)s1, (HRGN)s2, (INT)(SHORT)md);
        WowNotePut(note, notecap, &k, " -> complexity ");
        WowNoteHex(note, notecap, &k, (DWORD)rc, 4);
        Wow32SetReturn(f, (DWORD)(WORD)rc);
        return 1;
    }

    /* ── 0x41 CreateRectRgnIndirect(lpRect) -- CreateRectRgn with the four
         numbers in a struct instead of on the stack. */
    case WOWGDI_CREATERECTRGNIND: {
        volatile BYTE *rp = Wow32ArgPointer(f, WOWGDI_CRRI_ARG_RECT);
        INT k = 0;
        HRGN rgn;
        WORD tok;
        WowNotePut(note, notecap, &k, "CreateRectRgnIndirect");
        if (!rp) {
            WowNotePut(note, notecap, &k, " -- ★ NULL lpRect; answered 0");
            Wow32SetReturn(f, 0);
            return 1;
        }
        rgn = CreateRectRgn((INT)(SHORT)WowGdiPeek(rp, 0),
                            (INT)(SHORT)WowGdiPeek(rp, 2),
                            (INT)(SHORT)WowGdiPeek(rp, 4),
                            (INT)(SHORT)WowGdiPeek(rp, 6));
        tok = rgn ? WowGdiH16((HGDIOBJ)rgn, WOWGDI_KIND_OBJ) : 0;
        if (!tok) {
            if (rgn) DeleteObject((HGDIOBJ)rgn);
            WowNotePut(note, notecap, &k, " -- ★ no region (or the token map is"
                                       " full); answered 0");
            Wow32SetReturn(f, 0);
            return 1;
        }
        WowNotePut(note, notecap, &k, " -> region token 0x");
        WowNoteHex(note, notecap, &k, tok, 4);
        Wow32SetReturn(f, tok);
        return 1;
    }

    /* ── 0xac SetRectRgn(hrgn, l, t, r, b) -- redefine an EXISTING region.
       ★ The point of it is that it does not allocate, so a guest animating a
         clip does not churn the token map. */
    case WOWGDI_SETRECTRGN: {
        WORD hrgn = Wow32ArgWord(f, WOWGDI_SRR_ARG_RGN);
        INT  kk = -1, k = 0;
        HGDIOBJ r = WowGdiH32(hrgn, &kk);
        WowNotePut(note, notecap, &k, "SetRectRgn(0x");
        WowNoteHex(note, notecap, &k, hrgn, 4);
        WowNotePut(note, notecap, &k, ")");
        if (!r) {
            WowNotePut(note, notecap, &k, " -- ★ NOT ONE OF OUR REGION TOKENS;"
                                       " answered 0");
            Wow32SetReturn(f, 0);
            return 1;
        }
        SetRectRgn((HRGN)r,
                   (INT)(SHORT)Wow32ArgWord(f, WOWGDI_SRR_ARG_LEFT),
                   (INT)(SHORT)Wow32ArgWord(f, WOWGDI_SRR_ARG_TOP),
                   (INT)(SHORT)Wow32ArgWord(f, WOWGDI_SRR_ARG_RIGHT),
                   (INT)(SHORT)Wow32ArgWord(f, WOWGDI_SRR_ARG_BOTTOM));
        Wow32SetReturn(f, 1);
        return 1;
    }

    /* ── 0x3f CreatePolygonRgn(lpPoints, nCount, fnMode). ────────────────────
       ⚠ A Win16 POINT IS TWO **WORDS**, not two LONGs, so the array has to be
         widened one point at a time -- handing the guest's buffer straight to
         Win32 would read every coordinate from the wrong half of the next pair
         and produce a region that is wrong without being empty.
       ⚠ AND THE COUNT IS BOUNDED before it is trusted: it is a guest number
         used as an allocation size. */
    case WOWGDI_CREATEPOLYGONRGN: {
        volatile BYTE *pp = Wow32ArgPointer(f, WOWGDI_CPR_ARG_POINTS);
        WORD n  = Wow32ArgWord(f, WOWGDI_CPR_ARG_COUNT);
        WORD md = Wow32ArgWord(f, WOWGDI_CPR_ARG_MODE);
        INT  k = 0, i;
        POINT pts[WOWGDI_MAX_POLYPTS];
        HRGN rgn;
        WORD tok;
        WowNotePut(note, notecap, &k, "CreatePolygonRgn(n=");
        WowNoteHex(note, notecap, &k, n, 4);
        WowNotePut(note, notecap, &k, ", mode "); WowNoteHex(note, notecap, &k, md, 2);
        WowNotePut(note, notecap, &k, ")");
        if (!pp || !n || n > WOWGDI_MAX_POLYPTS) {
            WowNotePut(note, notecap, &k, " -- ★ NULL points or count out of range"
                                       " (max 0x");
            WowNoteHex(note, notecap, &k, WOWGDI_MAX_POLYPTS, 4);
            WowNotePut(note, notecap, &k, "); answered 0");
            Wow32SetReturn(f, 0);
            return 1;
        }
        for (i = 0; i < (INT)n; ++i) {
            pts[i].x = (LONG)(SHORT)WowGdiPeek(pp, i * 4 + 0);
            pts[i].y = (LONG)(SHORT)WowGdiPeek(pp, i * 4 + 2);
        }
        rgn = CreatePolygonRgn(pts, (INT)n, (INT)(SHORT)md);
        tok = rgn ? WowGdiH16((HGDIOBJ)rgn, WOWGDI_KIND_OBJ) : 0;
        if (!tok) {
            if (rgn) DeleteObject((HGDIOBJ)rgn);
            WowNotePut(note, notecap, &k, " -- ★ no region; answered 0");
            Wow32SetReturn(f, 0);
            return 1;
        }
        WowNotePut(note, notecap, &k, " -> region token 0x");
        WowNoteHex(note, notecap, &k, tok, 4);
        Wow32SetReturn(f, tok);
        return 1;
    }

    /* ── 0x4d GetClipBox(hDC, lpRect) -- the bounding box of the clip region. */
    case WOWGDI_GETCLIPBOX: {
        WORD hdc = Wow32ArgWord(f, WOWGDI_GCX_ARG_HDC);
        volatile BYTE *rp = Wow32ArgPointer(f, WOWGDI_GCX_ARG_RECT);
        INT dkind = -1, k = 0, rc;
        HGDIOBJ d = WowGdiH32(hdc, &dkind);
        RECT rc32;
        WowNotePut(note, notecap, &k, "GetClipBox(0x");
        WowNoteHex(note, notecap, &k, hdc, 4);
        WowNotePut(note, notecap, &k, ")");
        if (!rp) {
            WowNotePut(note, notecap, &k, " -- ★ NULL lpRect; nothing written");
            Wow32SetReturn(f, 0);                       /* ERROR */
            return 1;
        }
        if (!d || (dkind != WOWGDI_KIND_DC && dkind != WOWGDI_KIND_WINDC)) {
            INT i;
            /* Zero it rather than leave the caller's litter -- same reasoning as
               GetClientRect, and for the same reason: a guest that clips to
               stack litter draws nothing and looks like a paint bug. */
            for (i = 0; i < 8; ++i) rp[i] = 0;
            WowNotePut(note, notecap, &k, " -- ★ NOT ONE OF OUR DC TOKENS; zeroed");
            Wow32SetReturn(f, 0);
            return 1;
        }
        rc = GetClipBox((HDC)d, &rc32);
        Wow32PokeWord(rp + 0, (WORD)(SHORT)rc32.left);
        Wow32PokeWord(rp + 2, (WORD)(SHORT)rc32.top);
        Wow32PokeWord(rp + 4, (WORD)(SHORT)rc32.right);
        Wow32PokeWord(rp + 6, (WORD)(SHORT)rc32.bottom);
        WowNotePut(note, notecap, &k, " -> ");
        WowNoteHex(note, notecap, &k, (DWORD)rc32.right, 4);
        WowNotePut(note, notecap, &k, "x");
        WowNoteHex(note, notecap, &k, (DWORD)rc32.bottom, 4);
        WowNotePut(note, notecap, &k, " complexity ");
        WowNoteHex(note, notecap, &k, (DWORD)rc, 4);
        Wow32SetReturn(f, (DWORD)(WORD)rc);
        return 1;
    }

    /* ── 0x5c GetTextFace(hDC, nCount, lpFaceName) -- the name of the font
         currently selected. WRITE.EXE and CARDFILE both ask.
       ⚠ nCount IS A BUFFER SIZE FROM THE GUEST and bounds the copy; Win32's own
         call is given the smaller of it and our stack buffer. */
    case WOWGDI_GETTEXTFACE: {
        WORD hdc = Wow32ArgWord(f, WOWGDI_GTF_ARG_HDC);
        WORD cnt = Wow32ArgWord(f, WOWGDI_GTF_ARG_COUNT);
        volatile BYTE *bp = Wow32ArgPointer(f, WOWGDI_GTF_ARG_BUF);
        INT dkind = -1, k = 0, got, i;
        HGDIOBJ d = WowGdiH32(hdc, &dkind);
        CHAR face[LF_FACESIZE + 1];
        WowNotePut(note, notecap, &k, "GetTextFace(0x");
        WowNoteHex(note, notecap, &k, hdc, 4);
        WowNotePut(note, notecap, &k, ", ");
        WowNoteHex(note, notecap, &k, cnt, 4);
        WowNotePut(note, notecap, &k, ")");
        if (!bp || !cnt || !d
            || (dkind != WOWGDI_KIND_DC && dkind != WOWGDI_KIND_WINDC)) {
            WowNotePut(note, notecap, &k, " -- ★ NULL buffer or not one of our DC"
                                       " tokens; answered 0");
            Wow32SetReturn(f, 0);
            return 1;
        }
        got = (INT)GetTextFaceA((HDC)d, (INT)sizeof face, face);
        if (got <= 0) { face[0] = 0; got = 0; }
        if (got > (INT)cnt - 1) got = (INT)cnt - 1;
        for (i = 0; i < got; ++i) bp[i] = (BYTE)face[i];
        bp[got] = 0;
        WowNotePut(note, notecap, &k, " -> ");
        WowNoteQuoted(note, notecap, &k, face);
        Wow32SetReturn(f, (DWORD)(WORD)got);
        return 1;
    }

    /* ── 0x0a SetTextJustification(hDC, nBreakExtra, nBreakCount) -- how WRITE
         justifies a line: spread nBreakExtra units over nBreakCount breaks. */
    case WOWGDI_SETTEXTJUST: {
        WORD hdc = Wow32ArgWord(f, WOWGDI_STJ_ARG_HDC);
        INT  ex  = (INT)(SHORT)Wow32ArgWord(f, WOWGDI_STJ_ARG_EXTRA);
        INT  ct  = (INT)(SHORT)Wow32ArgWord(f, WOWGDI_STJ_ARG_COUNT);
        INT  dkind = -1, k = 0, rc;
        HGDIOBJ d = WowGdiH32(hdc, &dkind);
        WowNotePut(note, notecap, &k, "SetTextJustification(0x");
        WowNoteHex(note, notecap, &k, hdc, 4);
        WowNotePut(note, notecap, &k, ", extra ");
        WowNoteHex(note, notecap, &k, (DWORD)ex, 4);
        WowNotePut(note, notecap, &k, ", breaks ");
        WowNoteHex(note, notecap, &k, (DWORD)ct, 4);
        WowNotePut(note, notecap, &k, ")");
        if (!d || (dkind != WOWGDI_KIND_DC && dkind != WOWGDI_KIND_WINDC)) {
            WowNotePut(note, notecap, &k, " -- ★ NOT ONE OF OUR DC TOKENS;"
                                       " answered 0");
            Wow32SetReturn(f, 0);
            return 1;
        }
        rc = SetTextJustification((HDC)d, ex, ct);
        Wow32SetReturn(f, (DWORD)(WORD)(rc ? 1 : 0));
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
        WORD h = Wow32ArgWord(f, WOWGDI_ORG_ARG_HDC);
        INT  x = (INT)(SHORT)Wow32ArgWord(f, WOWGDI_ORG_ARG_X);
        INT  y = (INT)(SHORT)Wow32ArgWord(f, WOWGDI_ORG_ARG_Y);
        INT  isbm = (f->Id == WOWGDI_SETBITMAPDIM);
        INT  kind = -1;
        HGDIOBJ o = WowGdiH32(h, &kind);
        INT  k = 0;
        SIZE sz; POINT pt;
        WowNotePut(note, notecap, &k,
                f->Id == WOWGDI_SETWINDOWEXT   ? "SetWindowExt(0x" :
                f->Id == WOWGDI_SETVIEWPORTORG ? "SetViewportOrg(0x" :
                f->Id == WOWGDI_SETVIEWPORTEXT ? "SetViewportExt(0x"
                                               : "SetBitmapDimension(0x");
        WowNoteHex(note, notecap, &k, h, 4);
        WowNotePut(note, notecap, &k, ", ");
        WowNoteHex(note, notecap, &k, (DWORD)x, 4);
        WowNotePut(note, notecap, &k, ",");
        WowNoteHex(note, notecap, &k, (DWORD)y, 4);
        WowNotePut(note, notecap, &k, ")");
        if (!o || (isbm ? (kind != WOWGDI_KIND_OBJ && kind != WOWGDI_KIND_STOCK)
                        : (kind != WOWGDI_KIND_DC && kind != WOWGDI_KIND_WINDC))) {
            WowNotePut(note, notecap, &k, isbm
                        ? " -- ★ NOT ONE OF OUR BITMAP TOKENS; answered 0"
                        : " -- ★ NOT ONE OF OUR DC TOKENS; answered 0");
            Wow32SetReturn(f, 0);
            return 1;
        }
        sz.cx = sz.cy = 0; pt.x = pt.y = 0;
        if (f->Id == WOWGDI_SETWINDOWEXT)        SetWindowExtEx((HDC)o, x, y, &sz);
        else if (f->Id == WOWGDI_SETVIEWPORTEXT) SetViewportExtEx((HDC)o, x, y, &sz);
        else if (f->Id == WOWGDI_SETVIEWPORTORG) {
            SetViewportOrgEx((HDC)o, x, y, &pt);
            sz.cx = pt.x; sz.cy = pt.y;
        } else {
            SetBitmapDimensionEx((HBITMAP)o, x, y, &sz);
        }
        Wow32SetReturn(f, ((DWORD)(WORD)(SHORT)sz.cy << 16)
                        | (DWORD)(WORD)(SHORT)sz.cx);
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
        INT   isidx = (f->Id == WOWGDI_GETNEARESTPALIDX);
        WORD  h   = Wow32ArgWord(f, WOWGDI_COL_ARG_HDC);
        DWORD col = Wow32ArgDword(f, WOWGDI_COL_ARG_COLOR);
        INT   kind = -1;
        HGDIOBJ o = WowGdiH32(h, &kind);
        INT   k = 0;
        WowNotePut(note, notecap, &k, isidx ? "GetNearestPaletteIndex(0x"
                                         : "GetNearestColor(0x");
        WowNoteHex(note, notecap, &k, h, 4);
        WowNotePut(note, notecap, &k, ", 0x");
        WowNoteHex(note, notecap, &k, col, 8);
        WowNotePut(note, notecap, &k, ")");
        /* ⚠ Session 46 answered this 0 because nothing could make a palette.
             `CreatePalette` (0x168) is serviced now, so it is a real lookup --
             and a handle that is still not ours is still refused rather than
             answered with an invented index. */
        if (isidx) {
            if (!o || (kind != WOWGDI_KIND_OBJ && kind != WOWGDI_KIND_STOCK)) {
                WowNotePut(note, notecap, &k, " -- ★ NOT ONE OF OUR PALETTE TOKENS;"
                                           " answered 0 rather than inventing an"
                                           " index");
                Wow32SetReturn(f, 0);
                return 1;
            }
            Wow32SetReturn(f, (DWORD)GetNearestPaletteIndex((HPALETTE)o, (COLORREF)col));
            return 1;
        }
        if (!o || (kind != WOWGDI_KIND_DC && kind != WOWGDI_KIND_WINDC)) {
            WowNotePut(note, notecap, &k, " -- ★ NOT ONE OF OUR DC TOKENS;"
                                       " answered the colour unchanged");
            Wow32SetReturn(f, col);
            return 1;
        }
        col = (DWORD)GetNearestColor((HDC)o, (COLORREF)col);
        WowNotePut(note, notecap, &k, " = 0x");
        WowNoteHex(note, notecap, &k, col, 8);
        Wow32SetReturn(f, col);
        return 1;
    }

    /* ── ★★ 0x21 TextOut / 0x5b GetTextExtent -- the same (string, count). ────
       ⚠ THE STRING IS COUNTED, NOT TERMINATED. Both take an explicit length and
         a Win16 program is entitled to hand over a fragment of a larger buffer,
         so this must NOT stop at a NUL the way `Wow32ArgString` does -- it copies
         exactly the count it was given, bounded by the scratch buffer. */
    case WOWGDI_INTERSECTCLIPRECT: {
        WORD tok = Wow32ArgWord(f, WOWGDI_ICR_ARG_HDC);
        INT  l = (INT)(SHORT)Wow32ArgWord(f, WOWGDI_ICR_ARG_LEFT);
        INT  t = (INT)(SHORT)Wow32ArgWord(f, WOWGDI_ICR_ARG_TOP);
        INT  r = (INT)(SHORT)Wow32ArgWord(f, WOWGDI_ICR_ARG_RIGHT);
        INT  b = (INT)(SHORT)Wow32ArgWord(f, WOWGDI_ICR_ARG_BOTTOM);
        INT  kind = -1, rc;
        HGDIOBJ o = WowGdiH32(tok, &kind);
        INT  k = 0;
        WowNotePut(note, notecap, &k, "IntersectClipRect(0x");
        WowNoteHex(note, notecap, &k, tok, 4);
        if (!o || (kind != WOWGDI_KIND_DC && kind != WOWGDI_KIND_WINDC)) {
            WowNotePut(note, notecap, &k, ") -- ★ NOT ONE OF OUR DC TOKENS; 0");
            Wow32SetReturn(f, 0); return 1;
        }
        rc = IntersectClipRect((HDC)o, l, t, r, b);
        WowNotePut(note, notecap, &k, ") -> region kind ");
        WowNoteHex(note, notecap, &k, (DWORD)rc, 2);
        Wow32SetReturn(f, (DWORD)rc);
        return 1;
    }

    case WOWGDI_RECTVISIBLE: {
        WORD tok = Wow32ArgWord(f, WOWGDI_RV_ARG_HDC);
        volatile BYTE *rp = Wow32ArgPointer(f, WOWGDI_RV_ARG_RECT);
        INT kind = -1, i, vis;
        HGDIOBJ o = WowGdiH32(tok, &kind);
        BYTE r8[8];
        RECT r;
        INT k = 0;
        WowNotePut(note, notecap, &k, "RectVisible(0x");
        WowNoteHex(note, notecap, &k, tok, 4);
        if (!o || (kind != WOWGDI_KIND_DC && kind != WOWGDI_KIND_WINDC) || !rp) {
            WowNotePut(note, notecap, &k, ") -- ★ NOT ONE OF OUR DC TOKENS, or no rect; 0");
            Wow32SetReturn(f, 0); return 1;
        }
        for (i = 0; i < 8; ++i) r8[i] = (BYTE)rp[i];
        r.left  = WowConvRect16Get(r8, 0); r.top    = WowConvRect16Get(r8, 1);
        r.right = WowConvRect16Get(r8, 2); r.bottom = WowConvRect16Get(r8, 3);
        vis = RectVisible((HDC)o, &r) ? 1 : 0;
        WowNotePut(note, notecap, &k, vis ? ") -> VISIBLE" : ") -> not visible");
        Wow32SetReturn(f, (DWORD)vis);
        return 1;
    }

    case WOWGDI_CREATEFONT: {
        CHAR face[LF_FACESIZE];
        HFONT hf;
        WORD  tok;
        INT   k = 0;
        INT   h  = (INT)(SHORT)Wow32ArgWord(f, WOWGDI_CF_ARG_HEIGHT);
        INT   wd = (INT)(SHORT)Wow32ArgWord(f, WOWGDI_CF_ARG_WIDTH);
        face[0] = 0;
        Wow32ArgString(f, WOWGDI_CF_ARG_FACE, face, (INT)sizeof face);
        hf = CreateFontA(h, wd,
                         (INT)(SHORT)Wow32ArgWord(f, WOWGDI_CF_ARG_ESCAPE),
                         (INT)(SHORT)Wow32ArgWord(f, WOWGDI_CF_ARG_ORIENT),
                         (INT)(SHORT)Wow32ArgWord(f, WOWGDI_CF_ARG_WEIGHT),
                         (DWORD)(Wow32ArgWord(f, WOWGDI_CF_ARG_ITALIC) & 0xFF),
                         (DWORD)(Wow32ArgWord(f, WOWGDI_CF_ARG_UNDER)  & 0xFF),
                         (DWORD)(Wow32ArgWord(f, WOWGDI_CF_ARG_STRIKE) & 0xFF),
                         (DWORD)(Wow32ArgWord(f, WOWGDI_CF_ARG_CHARSET) & 0xFF),
                         (DWORD)(Wow32ArgWord(f, WOWGDI_CF_ARG_OUTPREC) & 0xFF),
                         (DWORD)(Wow32ArgWord(f, WOWGDI_CF_ARG_CLIPPREC) & 0xFF),
                         (DWORD)(Wow32ArgWord(f, WOWGDI_CF_ARG_QUALITY) & 0xFF),
                         (DWORD)(Wow32ArgWord(f, WOWGDI_CF_ARG_PITCH) & 0xFF),
                         face[0] ? face : NULL);
        tok = hf ? WowGdiH16((HGDIOBJ)hf, WOWGDI_KIND_OBJ) : 0;
        WowNotePut(note, notecap, &k, "CreateFont h=");
        WowNoteHex(note, notecap, &k, (DWORD)h, 4);
        WowNotePut(note, notecap, &k, " \"");
        WowNotePut(note, notecap, &k, face[0] ? face : "(any)");
        WowNotePut(note, notecap, &k, "\" -> token 0x");
        WowNoteHex(note, notecap, &k, tok, 4);
        if (hf && !tok) { DeleteObject((HGDIOBJ)hf);
                          WowNotePut(note, notecap, &k, " -- ★ TOKEN MAP FULL; font deleted"); }
        Wow32SetReturn(f, (DWORD)tok);
        return 1;
    }

    case WOWGDI_GETCHARWIDTH: {
        WORD tok   = Wow32ArgWord(f, WOWGDI_GCW_ARG_HDC);
        WORD first = Wow32ArgWord(f, WOWGDI_GCW_ARG_FIRST);
        WORD last  = Wow32ArgWord(f, WOWGDI_GCW_ARG_LAST);
        volatile BYTE *bp = Wow32ArgPointer(f, WOWGDI_GCW_ARG_BUF);
        INT kind = -1;
        HGDIOBJ o = WowGdiH32(tok, &kind);
        INT w32[256];
        INT k = 0, n, i, ok;
        WowNotePut(note, notecap, &k, "GetCharWidth(0x");
        WowNoteHex(note, notecap, &k, tok, 4);
        WowNotePut(note, notecap, &k, ", "); WowNoteHex(note, notecap, &k, first, 2);
        WowNotePut(note, notecap, &k, "..");  WowNoteHex(note, notecap, &k, last, 2);
        if (!o || (kind != WOWGDI_KIND_DC && kind != WOWGDI_KIND_WINDC) || !bp
            || last < first || (INT)(last - first) >= 256) {
            WowNotePut(note, notecap, &k, ") -- ★ NOT ONE OF OUR DC TOKENS, no buffer, "
                                       "or a range past 256; 0");
            Wow32SetReturn(f, 0); return 1;
        }
        n  = (INT)(last - first) + 1;
        ok = GetCharWidth32A((HDC)o, first, last, w32) ? 1 : 0;
        if (!ok) ok = GetCharWidthA((HDC)o, first, last, w32) ? 1 : 0;
        if (!ok) { WowNotePut(note, notecap, &k, ") -- GDI refused; nothing written");
                   Wow32SetReturn(f, 0); return 1; }
        /* ⚠ ONE WORD PER CHARACTER. See the note by the ids. */
        for (i = 0; i < n; ++i) Wow32PokeWord(bp + i * 2, (WORD)(SHORT)w32[i]);
        WowNotePut(note, notecap, &k, ") -> "); WowNoteHex(note, notecap, &k, (DWORD)n, 4);
        WowNotePut(note, notecap, &k, " widths, one WORD each");
        Wow32SetReturn(f, 1);
        return 1;
    }

    case WOWGDI_CREATEMETAFILE: {
        CHAR path[260];
        HDC  mdc;
        WORD tok;
        INT  k = 0, named;
        path[0] = 0;
        named = Wow32ArgString(f, WOWGDI_CMF_ARG_FILE, path, (INT)sizeof path);
        mdc = CreateMetaFileA(named && path[0] ? path : NULL);
        tok = mdc ? WowGdiH16((HGDIOBJ)mdc, WOWGDI_KIND_DC) : 0;
        WowNotePut(note, notecap, &k, "CreateMetaFile(");
        WowNotePut(note, notecap, &k, (named && path[0]) ? path : "in memory");
        WowNotePut(note, notecap, &k, ") -> DC token 0x");
        WowNoteHex(note, notecap, &k, tok, 4);
        Wow32SetReturn(f, (DWORD)tok);
        return 1;
    }

    case WOWGDI_CLOSEMETAFILE: {
        WORD tok = Wow32ArgWord(f, WOWGDI_MF1_ARG_H);
        INT  kind = -1;
        HGDIOBJ o = WowGdiH32(tok, &kind);
        HMETAFILE mf;
        WORD out;
        INT  k = 0;
        WowNotePut(note, notecap, &k, "CloseMetaFile(0x");
        WowNoteHex(note, notecap, &k, tok, 4);
        if (!o || kind != WOWGDI_KIND_DC) {
            WowNotePut(note, notecap, &k, ") -- ★ NOT A METAFILE DC TOKEN; 0");
            Wow32SetReturn(f, 0); return 1;
        }
        mf = CloseMetaFile((HDC)o);
        WowGdiForget(tok);              /* the DC is gone whatever happened */
        /* ⚠ A METAFILE IS NOT A DC. It gets an OBJ token so that a guest which
             passes it to a DC call is refused rather than obeyed. */
        out = mf ? WowGdiH16((HGDIOBJ)mf, WOWGDI_KIND_OBJ) : 0;
        WowNotePut(note, notecap, &k, ") -> metafile token 0x");
        WowNoteHex(note, notecap, &k, out, 4);
        Wow32SetReturn(f, (DWORD)out);
        return 1;
    }

    case WOWGDI_DELETEMETAFILE: {
        WORD tok = Wow32ArgWord(f, WOWGDI_MF1_ARG_H);
        INT  kind = -1;
        HGDIOBJ o = WowGdiH32(tok, &kind);
        INT  k = 0, r = 0;
        WowNotePut(note, notecap, &k, "DeleteMetaFile(0x");
        WowNoteHex(note, notecap, &k, tok, 4);
        if (o && kind == WOWGDI_KIND_OBJ) {
            r = DeleteMetaFile((HMETAFILE)o) ? 1 : 0;
            WowGdiForget(tok);
        }
        WowNotePut(note, notecap, &k, r ? ") -> deleted"
                                     : ") -- ★ NOT ONE OF OUR METAFILE TOKENS; FALSE");
        Wow32SetReturn(f, (DWORD)r);
        return 1;
    }

    /* ── ★ 0x7b PlayMetaFile(hdc, hmf) ── s90, #295. Write and Paintbrush import it.
         The metafile is one of OUR tokens (CloseMetaFile/CopyMetaFile made it), so
         this is Win32's own PlayMetaFile on the two real handles. Checked to be a
         metafile by the OS's own GetObjectType, not by token kind alone -- a pen
         is an OBJ token too. */
    case WOWGDI_PLAYMETAFILE: {
        WORD hdc = Wow32ArgWord(f, WOWGDI_PMF_ARG_HDC);
        WORD tok = Wow32ArgWord(f, WOWGDI_PMF_ARG_HMF);
        INT  kd = -1, km = -1, k = 0, r = 0;
        HGDIOBJ dc = WowGdiH32(hdc, &kd);
        HGDIOBJ mf = WowGdiH32(tok, &km);
        WowNotePut(note, notecap, &k, "PlayMetaFile(0x");
        WowNoteHex(note, notecap, &k, hdc, 4);
        WowNotePut(note, notecap, &k, ", 0x");
        WowNoteHex(note, notecap, &k, tok, 4);
        WowNotePut(note, notecap, &k, ")");
        if (!dc || !(kd == WOWGDI_KIND_DC || kd == WOWGDI_KIND_WINDC)) {
            WowNotePut(note, notecap, &k, " -- ★ NOT ONE OF OUR DC TOKENS; FALSE");
        } else if (!mf || km != WOWGDI_KIND_OBJ || GetObjectType(mf) != OBJ_METAFILE) {
            WowNotePut(note, notecap, &k, " -- ★ NOT ONE OF OUR METAFILE TOKENS; FALSE");
        } else {
            r = PlayMetaFile((HDC)dc, (HMETAFILE)mf) ? 1 : 0;
            WowNotePut(note, notecap, &k, r ? " -> played" : " -> FAILED");
        }
        Wow32SetReturn(f, (DWORD)r);
        return 1;
    }

    /* ── ★ 0xaf EnumMetaFile(hdc, hmf, lpfn, lParam) ── #295. CARDFILE, PACKAGER
         and WRITE import it (through OLECLI). The metafile is snapshot ONCE, as
         bytes (GetMetaFileBitsEx), and walked by wowconv.h's pure parser; each
         record is one callback on the wowenum.h chain -- see g_WowGdiMetafile above for the
         handle table. The answer is 1 up front and revised to 0 by a stop. */
    case WOWGDI_ENUMMETAFILE: {
        WORD  hdc  = Wow32ArgWord(f, WOWGDI_EMF_ARG_HDC);
        WORD  tok  = Wow32ArgWord(f, WOWGDI_EMF_ARG_HMF);
        DWORD proc = Wow32ArgDword(f, WOWGDI_EMF_ARG_PROC);
        DWORD lp   = Wow32ArgDword(f, WOWGDI_EMF_ARG_LPARAM);
        INT   kd = -1, km = -1, k = 0;
        HGDIOBJ dc = WowGdiH32(hdc, &kd);
        HGDIOBJ mf = WowGdiH32(tok, &km);
        UINT  n;
        PBYTE bits;
        UINT nobj = 0;
        unsigned long first, end = 0;
        WowNotePut(note, notecap, &k, "EnumMetaFile(0x");
        WowNoteHex(note, notecap, &k, hdc, 4);
        WowNotePut(note, notecap, &k, ", 0x");
        WowNoteHex(note, notecap, &k, tok, 4);
        WowNotePut(note, notecap, &k, ")");
        /* ⚠ EVERY REFUSAL BELOW ANSWERS 0, callbacks-off included. EnumObjects keeps
             its TRUE when callbacks are off; a metafile enumerator that says "done"
             having shown the guest no record would let it conclude the picture is
             empty, which is the wrong answer rather than a missing one. */
        Wow32SetReturn(f, 0);
        if (!mf || km != WOWGDI_KIND_OBJ || GetObjectType(mf) != OBJ_METAFILE) {
            WowNotePut(note, notecap, &k, " -- ★ NOT ONE OF OUR METAFILE TOKENS; 0");
            return 1;
        }
        if (!f->IsCallbackAllowed) { WowNotePut(note, notecap, &k, " -- callbacks are not armed; 0"); return 1; }
        if (WowEnumBusy() || g_WowGdiMetafile.IsActive) {
            WowNotePut(note, notecap, &k, " -- ★ AN ENUMERATION IS ALREADY RUNNING; REFUSED");
            return 1;
        }
        n = GetMetaFileBitsEx((HMETAFILE)mf, 0, NULL);
        bits = n ? (PBYTE)HeapAlloc(GetProcessHeap(), 0, n) : NULL;
        if (!bits || GetMetaFileBitsEx((HMETAFILE)mf, n, bits) != n) {
            if (bits) HeapFree(GetProcessHeap(), 0, bits);
            WowNotePut(note, notecap, &k, " -- ★ GetMetaFileBitsEx FAILED; 0");
            return 1;
        }
        first = WowConvMetafileHeader(bits, n, &nobj, &end);
        WowNotePut(note, notecap, &k, " bytes=0x"); WowNoteHex(note, notecap, &k, n, 6);
        WowNotePut(note, notecap, &k, " nObj=0x"); WowNoteHex(note, notecap, &k, nobj, 4);
        if (!first || nobj > WOWMF_MAXOBJ) {
            HeapFree(GetProcessHeap(), 0, bits);
            WowNotePut(note, notecap, &k, !first ? " -- ★ NOT A WMF HEADER; 0"
                                              : " -- ★ MORE OBJECTS THAN WOWMF_MAXOBJ; REFUSED, 0");
            return 1;
        }
        if (!WowEnumBegin(WOWENUM_METAFILE, proc, f->GuestDataSelector, lp,
                           (DWORD)(ULONG_PTR)(f->FrameBase + WOW32_OFF_RET), hdc)) {
            HeapFree(GetProcessHeap(), 0, bits);
            WowNotePut(note, notecap, &k, " -- ★ the callback is not a usable far pointer; 0");
            return 1;
        }
        memset(&g_WowGdiMetafile, 0, sizeof g_WowGdiMetafile);
        g_WowGdiMetafile.IsActive = 1;
        g_WowGdiMetafile.Bits = bits; g_WowGdiMetafile.Length = n; g_WowGdiMetafile.End = end; g_WowGdiMetafile.Offset = first;
        g_WowGdiMetafile.ObjectCount = nobj;
        g_WowGdiMetafile.Dc16 = hdc;
        /* The guest's hdc is passed to every callback verbatim; it only has to be
           one of ours for the objects to be put back at the end. */
        if (dc && (kd == WOWGDI_KIND_DC || kd == WOWGDI_KIND_WINDC)) {
            g_WowGdiMetafile.Dc = (HDC)dc;
            g_WowGdiMetafile.OriginalPen   = GetCurrentObject((HDC)dc, OBJ_PEN);
            g_WowGdiMetafile.OriginalBrush = GetCurrentObject((HDC)dc, OBJ_BRUSH);
            g_WowGdiMetafile.OriginalFont  = GetCurrentObject((HDC)dc, OBJ_FONT);
        }
        Wow32SetReturn(f, 1);
        f->IsEnumerationRequested = 1;
        return 1;
    }

    /* ── ★ 0xb0 PlayMetaFileRecord(hdc, lpht, lpmr, nHandles) ── #295. Win32's own
         PlayMetaFileRecord on an HGDIOBJ table built from the guest's TOKENS; any
         entry the record changed goes back as a token -- a new object gets one, a
         META_DELETEOBJECT'd one (Win32 deletes it and zeroes the slot) has its token
         forgotten. ⚠ Win16 declares this VOID (3.1 SDK, Wine's gdi.exe.spec); the
         BOOL written to the return hole is Win32's and is unmeasured against stock. */
    case WOWGDI_PLAYMETAFILEREC: {
        WORD  hdc = Wow32ArgWord(f, WOWGDI_PMFR_ARG_HDC);
        WORD  nh  = Wow32ArgWord(f, WOWGDI_PMFR_ARG_NHANDLES);
        DWORD mfp = Wow32ArgDword(f, WOWGDI_PMFR_ARG_MR);
        volatile BYTE *mr = Wow32ArgPointer(f, WOWGDI_PMFR_ARG_MR);
        volatile BYTE *ht = Wow32ArgPointer(f, WOWGDI_PMFR_ARG_HT);
        INT   kd = -1, k = 0, r = 0, i, made = 0, gone = 0, full = 0;
        HGDIOBJ dc = WowGdiH32(hdc, &kd);
        DWORD w = 0, bytes;
        PCBYTE rec;
        WORD  tk[WOWMF_MAXOBJ];
        HGDIOBJ before[WOWMF_MAXOBJ];
        WowNotePut(note, notecap, &k, "PlayMetaFileRecord(0x");
        WowNoteHex(note, notecap, &k, hdc, 4);
        WowNotePut(note, notecap, &k, ", n=0x"); WowNoteHex(note, notecap, &k, nh, 4);
        Wow32SetReturn(f, 0);
        if (!dc || !(kd == WOWGDI_KIND_DC || kd == WOWGDI_KIND_WINDC)) {
            WowNotePut(note, notecap, &k, ") -- ★ NOT ONE OF OUR DC TOKENS; FALSE");
            return 1;
        }
        if (!mr || (nh && !ht) || nh > WOWMF_MAXOBJ) {
            WowNotePut(note, notecap, &k, nh > WOWMF_MAXOBJ
                ? ") -- ★ MORE HANDLES THAN WOWMF_MAXOBJ; REFUSED, FALSE"
                : ") -- ★ A NULL RECORD OR TABLE POINTER; FALSE");
            return 1;
        }
        w = (DWORD)WowGdiPeek(mr, 0) | ((DWORD)WowGdiPeek(mr, 2) << 16);
        WowNotePut(note, notecap, &k, ", fn=0x"); WowNoteHex(note, notecap, &k, WowGdiPeek(mr, 4), 4);
        WowNotePut(note, notecap, &k, " size=0x"); WowNoteHex(note, notecap, &k, w, 6);
        WowNotePut(note, notecap, &k, ")");
        if (w < 3) { WowNotePut(note, notecap, &k, " -- ★ rdSize < 3; FALSE"); return 1; }
        /* ★ A RECORD WHOSE CALLBACK COPY WAS CUT SHORT is played from the snapshot,
             recognised by ADDRESS: it is the record the enumeration is in. */
        if (g_WowGdiMetafile.IsActive && g_WowGdiMetafile.IsTruncated && g_WowGdiMetafile.RecordLinear
            && (DWORD)(ULONG_PTR)mr == g_WowGdiMetafile.RecordLinear && w * 2 == g_WowGdiMetafile.RecordBytes) {
            rec = g_WowGdiMetafile.Bits + g_WowGdiMetafile.RecordOffset;
            WowNotePut(note, notecap, &k, " [the truncated record, played from the snapshot]");
        } else {
            if (w > 0x8000 || (mfp & 0xFFFF) + w * 2 > 0x10000) {
                WowNotePut(note, notecap, &k, " -- ★ THE RECORD RUNS PAST ITS SEGMENT"
                                           " (a huge record); REFUSED, FALSE");
                return 1;
            }
            bytes = w * 2;
            for (i = 0; i < (INT)bytes; ++i) g_WowGdiMetafileRecord[i] = mr[i];
            rec = g_WowGdiMetafileRecord;
        }
        for (i = 0; i < (INT)nh; ++i) {
            INT kk = -1;
            HGDIOBJ o;
            tk[i] = WowGdiPeek(ht, i * 2);
            o = tk[i] ? WowGdiH32(tk[i], &kk) : NULL;
            if (o && (kk == WOWGDI_KIND_DC || kk == WOWGDI_KIND_WINDC)) o = NULL;
            g_WowGdiMetafileHandles[i] = before[i] = o;
        }
        r = PlayMetaFileRecord((HDC)dc, (HANDLETABLE *)g_WowGdiMetafileHandles,
                               (METARECORD *)(VOID *)rec, nh) ? 1 : 0;
        for (i = 0; i < (INT)nh; ++i) {
            if (g_WowGdiMetafileHandles[i] == before[i]) continue;
            if (g_WowGdiMetafileHandles[i]) {
                WORD t = WowGdiH16(g_WowGdiMetafileHandles[i], WOWGDI_KIND_OBJ);
                if (!t) ++full;
                Wow32PokeWord(ht + i * 2, t);
                ++made;
            } else {
                if (tk[i]) WowGdiForget(tk[i]);
                Wow32PokeWord(ht + i * 2, 0);
                ++gone;
            }
        }
        WowNotePut(note, notecap, &k, r ? " -> played" : " -> FAILED");
        if (made) { WowNotePut(note, notecap, &k, ", +0x"); WowNoteHex(note, notecap, &k, (DWORD)made, 2);
                    WowNotePut(note, notecap, &k, " object(s)"); }
        if (gone) { WowNotePut(note, notecap, &k, ", -0x"); WowNoteHex(note, notecap, &k, (DWORD)gone, 2);
                    WowNotePut(note, notecap, &k, " deleted"); }
        if (full) WowNotePut(note, notecap, &k, " -- ★★ THE TOKEN MAP IS FULL: a new object got"
                                             " token 0 and is unreachable (leaked)");
        Wow32SetReturn(f, (DWORD)r);
        return 1;
    }

    case WOWGDI_COPYMETAFILE: {
        WORD tok = Wow32ArgWord(f, WOWGDI_CPMF_ARG_HMF);
        INT  kind = -1;
        HGDIOBJ o = WowGdiH32(tok, &kind);
        CHAR path[260];
        HMETAFILE mf;
        WORD out;
        INT  k = 0, named;
        path[0] = 0;
        named = Wow32ArgString(f, WOWGDI_CPMF_ARG_FILE, path, (INT)sizeof path);
        WowNotePut(note, notecap, &k, "CopyMetaFile(0x");
        WowNoteHex(note, notecap, &k, tok, 4);
        if (!o || kind != WOWGDI_KIND_OBJ) {
            WowNotePut(note, notecap, &k, ") -- ★ NOT ONE OF OUR METAFILE TOKENS; 0");
            Wow32SetReturn(f, 0); return 1;
        }
        mf  = CopyMetaFileA((HMETAFILE)o, (named && path[0]) ? path : NULL);
        out = mf ? WowGdiH16((HGDIOBJ)mf, WOWGDI_KIND_OBJ) : 0;
        WowNotePut(note, notecap, &k, ") -> 0x"); WowNoteHex(note, notecap, &k, out, 4);
        Wow32SetReturn(f, (DWORD)out);
        return 1;
    }

    case WOWGDI_EXTTEXTOUT: {
        WORD hdc  = Wow32ArgWord(f, WOWGDI_ETO_ARG_HDC);
        INT  x    = (INT)(SHORT)Wow32ArgWord(f, WOWGDI_ETO_ARG_X);
        INT  y    = (INT)(SHORT)Wow32ArgWord(f, WOWGDI_ETO_ARG_Y);
        WORD opts = Wow32ArgWord(f, WOWGDI_ETO_ARG_OPTS);
        WORD n    = Wow32ArgWord(f, WOWGDI_ETO_ARG_COUNT);
        volatile BYTE *sp = Wow32ArgPointer(f, WOWGDI_ETO_ARG_STR);
        volatile BYTE *rp = Wow32ArgPointer(f, WOWGDI_ETO_ARG_RECT);
        volatile BYTE *dp = Wow32ArgPointer(f, WOWGDI_ETO_ARG_DX);
        INT  kind = -1;
        HGDIOBJ o = WowGdiH32(hdc, &kind);
        CHAR buf[512];
        INT  dx[512];
        RECT rc, *prc = NULL;
        INT  k = 0, i, cnt = (INT)n, ok;
        WowNotePut(note, notecap, &k, "ExtTextOut(0x");
        WowNoteHex(note, notecap, &k, hdc, 4);
        WowNotePut(note, notecap, &k, ", "); WowNoteHex(note, notecap, &k, (DWORD)x, 4);
        WowNotePut(note, notecap, &k, ",");  WowNoteHex(note, notecap, &k, (DWORD)y, 4);
        WowNotePut(note, notecap, &k, " opts=0x"); WowNoteHex(note, notecap, &k, opts, 4);
        WowNotePut(note, notecap, &k, " n="); WowNoteHex(note, notecap, &k, (DWORD)n, 4);
        if (!o || (kind != WOWGDI_KIND_DC && kind != WOWGDI_KIND_WINDC)) {
            WowNotePut(note, notecap, &k, " -- ★ NOT ONE OF OUR DC TOKENS; answered 0");
            Wow32SetReturn(f, 0);
            return 1;
        }
        if (cnt < 0) cnt = 0;
        if (cnt > (INT)sizeof buf) cnt = (INT)sizeof buf;
        for (i = 0; i < cnt; ++i) buf[i] = sp ? (CHAR)sp[i] : ' ';
        if (rp) {
            BYTE r8[8];
            for (i = 0; i < 8; ++i) r8[i] = (BYTE)rp[i];
            rc.left   = WowConvRect16Get(r8, 0);
            rc.top    = WowConvRect16Get(r8, 1);
            rc.right  = WowConvRect16Get(r8, 2);
            rc.bottom = WowConvRect16Get(r8, 3);
            prc = &rc;
            WowNotePut(note, notecap, &k, " rect=");
            WowNoteHex(note, notecap, &k, (DWORD)rc.left, 4);
            WowNotePut(note, notecap, &k, ",");
            WowNoteHex(note, notecap, &k, (DWORD)rc.top, 4);
            WowNotePut(note, notecap, &k, "-");
            WowNoteHex(note, notecap, &k, (DWORD)rc.right, 4);
            WowNotePut(note, notecap, &k, ",");
            WowNoteHex(note, notecap, &k, (DWORD)rc.bottom, 4);
        }
        if (dp && cnt) {
            for (i = 0; i < cnt; ++i)
                dx[i] = (INT)(SHORT)((WORD)dp[i * 2] | ((WORD)dp[i * 2 + 1] << 8));
            WowNotePut(note, notecap, &k, " +spacing");
        }
        ok = ExtTextOutA((HDC)o, x, y, (UINT)opts, prc,
                         cnt ? buf : NULL, (UINT)cnt,
                         (dp && cnt) ? dx : NULL) ? 1 : 0;
        WowNotePut(note, notecap, &k, ok ? " -> drawn" : " -> REFUSED by GDI");
        Wow32SetReturn(f, (DWORD)ok);
        return 1;
    }

    case WOWGDI_TEXTOUT:
    case WOWGDI_GETTEXTEXTENT: {
        INT  isout = (f->Id == WOWGDI_TEXTOUT);
        WORD hdc = Wow32ArgWord(f, isout ? WOWGDI_TO_ARG_HDC   : WOWGDI_TE_ARG_HDC);
        WORD n   = Wow32ArgWord(f, isout ? WOWGDI_TO_ARG_COUNT : WOWGDI_TE_ARG_COUNT);
        volatile BYTE *s = Wow32ArgPointer(f, isout ? WOWGDI_TO_ARG_STR : WOWGDI_TE_ARG_STR);
        INT  x = isout ? (INT)(SHORT)Wow32ArgWord(f, WOWGDI_TO_ARG_X) : 0;
        INT  y = isout ? (INT)(SHORT)Wow32ArgWord(f, WOWGDI_TO_ARG_Y) : 0;
        INT  kind = -1;
        HGDIOBJ o = WowGdiH32(hdc, &kind);
        CHAR buf[512];
        INT  k = 0, i, cnt = (INT)n;
        SIZE sz;
        WowNotePut(note, notecap, &k, isout ? "TextOut(0x" : "GetTextExtent(0x");
        WowNoteHex(note, notecap, &k, hdc, 4);
        if (isout) {
            WowNotePut(note, notecap, &k, ", ");
            WowNoteHex(note, notecap, &k, (DWORD)x, 4);
            WowNotePut(note, notecap, &k, ",");
            WowNoteHex(note, notecap, &k, (DWORD)y, 4);
        }
        WowNotePut(note, notecap, &k, ", ");
        WowNoteHex(note, notecap, &k, (DWORD)n, 4);
        WowNotePut(note, notecap, &k, " chars)");
        if (!o || (kind != WOWGDI_KIND_DC && kind != WOWGDI_KIND_WINDC) || !s) {
            WowNotePut(note, notecap, &k, " -- ★ NOT ONE OF OUR DC TOKENS, or no"
                                       " string; answered 0");
            Wow32SetReturn(f, 0);
            return 1;
        }
        if (cnt < 0) cnt = 0;
        if (cnt > (INT)sizeof buf) cnt = (INT)sizeof buf;
        for (i = 0; i < cnt; ++i) buf[i] = (CHAR)s[i];
        if (isout) {
            INT r = TextOutA((HDC)o, x, y, buf, cnt) ? 1 : 0;
            WowNotePut(note, notecap, &k, r ? " -> drawn" : " -- ★ the OS refused it");
            Wow32SetReturn(f, (DWORD)r);
            return 1;
        }
        sz.cx = sz.cy = 0;
        GetTextExtentPoint32A((HDC)o, buf, cnt, &sz);
        WowNotePut(note, notecap, &k, " = ");
        WowNoteHex(note, notecap, &k, (DWORD)sz.cx, 4);
        WowNotePut(note, notecap, &k, "x");
        WowNoteHex(note, notecap, &k, (DWORD)sz.cy, 4);
        Wow32SetReturn(f, ((DWORD)(WORD)sz.cy << 16) | (DWORD)(WORD)sz.cx);
        return 1;
    }

    /* ── ★★ 0x5d GetTextMetrics -- 31 bytes of `short`, layout checked on
         Notepad (see the note by the defines). ────────────────────────────────
       ⚠ THE WHOLE STRUCTURE IS WRITTEN, INCLUDING THE FIELDS NOTEPAD DOES NOT
         READ. A guest that finds a stale byte at tmPitchAndFamily picks a font
         for reasons nothing in the log explains. */
    case WOWGDI_GETTEXTMETRICS: {
        WORD hdc = Wow32ArgWord(f, WOWGDI_TM_ARG_HDC);
        volatile BYTE *dst = Wow32ArgPointer(f, WOWGDI_TM_ARG_BUF);
        INT  kind = -1;
        HGDIOBJ o = WowGdiH32(hdc, &kind);
        TEXTMETRICA tm;
        INT k = 0, i;
        BYTE b[WOWGDI_TEXTMETRIC16_SIZE];
        WowNotePut(note, notecap, &k, "GetTextMetrics(0x");
        WowNoteHex(note, notecap, &k, hdc, 4);
        WowNotePut(note, notecap, &k, ")");
        if (!o || (kind != WOWGDI_KIND_DC && kind != WOWGDI_KIND_WINDC) || !dst) {
            WowNotePut(note, notecap, &k, " -- ★ NOT ONE OF OUR DC TOKENS, or no"
                                       " buffer; answered 0");
            Wow32SetReturn(f, 0);
            return 1;
        }
        if (!GetTextMetricsA((HDC)o, &tm)) {
            WowNotePut(note, notecap, &k, " -- ★ the OS refused it; answered 0");
            Wow32SetReturn(f, 0);
            return 1;
        }
        for (i = 0; i < (INT)sizeof b; ++i) b[i] = 0;
        Wow32PokeWord(b +  0, (WORD)(SHORT)tm.tmHeight);
        Wow32PokeWord(b +  2, (WORD)(SHORT)tm.tmAscent);
        Wow32PokeWord(b +  4, (WORD)(SHORT)tm.tmDescent);
        Wow32PokeWord(b +  6, (WORD)(SHORT)tm.tmInternalLeading);
        Wow32PokeWord(b +  8, (WORD)(SHORT)tm.tmExternalLeading);
        Wow32PokeWord(b + 10, (WORD)(SHORT)tm.tmAveCharWidth);
        Wow32PokeWord(b + 12, (WORD)(SHORT)tm.tmMaxCharWidth);
        Wow32PokeWord(b + 14, (WORD)(SHORT)tm.tmWeight);
        /* s91: THE WINDOWS 3.1 ORDER from +16 on -- the BYTE fields come before the
             last three shorts, exactly as EnumFonts' NEWTEXTMETRIC16 (wowgdi_font_blob,
             = stock in w_genum) lays them out. This wrote a Win32-like order (Overhang
             at +16, the chars at +22) since s45: tests/probes/win16/w_tm vs stock showed
             PitchAndFamily, CharSet and Overhang wrong for every font. */
        b[16] = tm.tmItalic;          b[17] = tm.tmUnderlined;
        b[18] = tm.tmStruckOut;       b[19] = (BYTE)tm.tmFirstChar;
        b[20] = (BYTE)tm.tmLastChar;  b[21] = (BYTE)tm.tmDefaultChar;
        b[22] = (BYTE)tm.tmBreakChar; b[23] = tm.tmPitchAndFamily;
        b[24] = tm.tmCharSet;
        Wow32PokeWord(b + 25, (WORD)(SHORT)tm.tmOverhang);
        Wow32PokeWord(b + 27, (WORD)(SHORT)tm.tmDigitizedAspectX);
        Wow32PokeWord(b + 29, (WORD)(SHORT)tm.tmDigitizedAspectY);
        for (i = 0; i < (INT)sizeof b; ++i) dst[i] = b[i];
        WowNotePut(note, notecap, &k, " h=");
        WowNoteHex(note, notecap, &k, (DWORD)(WORD)(SHORT)tm.tmHeight, 4);
        WowNotePut(note, notecap, &k, " extlead=");
        WowNoteHex(note, notecap, &k, (DWORD)(WORD)(SHORT)tm.tmExternalLeading, 4);
        WowNotePut(note, notecap, &k, " avew=");
        WowNoteHex(note, notecap, &k, (DWORD)(WORD)(SHORT)tm.tmAveCharWidth, 4);
        Wow32SetReturn(f, 1);
        return 1;
    }

    /* ── ★ 0x39 CreateFontIndirect -- the Win16 LOGFONT, read the other way. ──
         The 50-byte layout is the one `GetObject` already writes: 18 bytes of
         header and a 32-byte face name. This is the same table used backwards,
         which is why there is no second reading of it to get wrong. */
    case WOWGDI_CREATEFONTIND: {
        volatile BYTE *p = Wow32ArgPointer(f, 0);
        LOGFONTA lf;
        INT k = 0, i;
        HFONT fn;
        WORD tok;
        WowNotePut(note, notecap, &k, "CreateFontIndirect(");
        if (!p) {
            WowNotePut(note, notecap, &k, "NULL) -- ★ no LOGFONT; answered 0");
            Wow32SetReturn(f, 0);
            return 1;
        }
        for (i = 0; i < (INT)sizeof lf; ++i) ((PBYTE)&lf)[i] = 0;
        lf.lfHeight     = (LONG)(SHORT)Wow32PeekWord(p + 0);
        lf.lfWidth      = (LONG)(SHORT)Wow32PeekWord(p + 2);
        lf.lfEscapement = (LONG)(SHORT)Wow32PeekWord(p + 4);
        lf.lfOrientation= (LONG)(SHORT)Wow32PeekWord(p + 6);
        lf.lfWeight     = (LONG)(SHORT)Wow32PeekWord(p + 8);
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
        WowNoteQuoted(note, notecap, &k, lf.lfFaceName);
        WowNotePut(note, notecap, &k, " h=");
        WowNoteHex(note, notecap, &k, (DWORD)(WORD)(SHORT)lf.lfHeight, 4);
        WowNotePut(note, notecap, &k, ")");
        fn = CreateFontIndirectA(&lf);
        tok = fn ? WowGdiH16((HGDIOBJ)fn, WOWGDI_KIND_OBJ) : 0;
        if (!tok) {
            if (fn) DeleteObject((HGDIOBJ)fn);
            WowNotePut(note, notecap, &k, " -- ★ no font (or the token map is full);"
                                       " answered 0");
            Wow32SetReturn(f, 0);
            return 1;
        }
        WowNotePut(note, notecap, &k, " -> font token 0x");
        WowNoteHex(note, notecap, &k, tok, 4);
        Wow32SetReturn(f, tok);
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
        INT   isget = (f->Id == WOWGDI_GETBITMAPBITS);
        WORD  hbm   = Wow32ArgWord(f, WOWGDI_BB2_ARG_HBM);
        DWORD cnt   = Wow32ArgDword(f, WOWGDI_BB2_ARG_COUNT);
        volatile BYTE *bits = Wow32ArgPointer(f, WOWGDI_BB2_ARG_BITS);
        INT   kind = -1;
        HGDIOBJ o = WowGdiH32(hbm, &kind);
        INT   k = 0;
        LONG  got;
        WowNotePut(note, notecap, &k, isget ? "GetBitmapBits(0x" : "SetBitmapBits(0x");
        WowNoteHex(note, notecap, &k, hbm, 4);
        WowNotePut(note, notecap, &k, ", ");
        WowNoteHex(note, notecap, &k, cnt, 8);
        WowNotePut(note, notecap, &k, " bytes)");
        if (!o || (kind != WOWGDI_KIND_OBJ && kind != WOWGDI_KIND_STOCK) || !bits) {
            WowNotePut(note, notecap, &k, " -- ★ NOT ONE OF OUR BITMAP TOKENS, or no"
                                       " buffer; answered 0");
            Wow32SetReturn(f, 0);
            return 1;
        }
        got = isget ? GetBitmapBits((HBITMAP)o, (LONG)cnt, (LPVOID)(ULONG_PTR)bits)
                    : SetBitmapBits((HBITMAP)o, (DWORD)cnt, (const VOID *)(ULONG_PTR)bits);
        WowNotePut(note, notecap, &k, " -> ");
        WowNoteHex(note, notecap, &k, (DWORD)got, 8);
        Wow32SetReturn(f, (DWORD)got);
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
        INT   isget = (f->Id == WOWGDI_GETDIBITS);
        WORD  hdc   = Wow32ArgWord(f, WOWGDI_DIB_ARG_HDC);
        WORD  hbm   = Wow32ArgWord(f, WOWGDI_DIB_ARG_HBM);
        WORD  start = Wow32ArgWord(f, WOWGDI_DIB_ARG_START);
        WORD  lines = Wow32ArgWord(f, WOWGDI_DIB_ARG_LINES);
        WORD  usage = Wow32ArgWord(f, WOWGDI_DIB_ARG_USAGE);
        volatile BYTE *bits = Wow32ArgPointer(f, WOWGDI_DIB_ARG_BITS);
        volatile BYTE *bmi  = Wow32ArgPointer(f, WOWGDI_DIB_ARG_BMI);
        INT   dk = -1, bk = -1;
        HGDIOBJ d = WowGdiH32(hdc, &dk);
        HGDIOBJ b = WowGdiH32(hbm, &bk);
        INT   k = 0, r;
        WowNotePut(note, notecap, &k, isget ? "GetDIBits(dc 0x" : "SetDIBits(dc 0x");
        WowNoteHex(note, notecap, &k, hdc, 4);
        WowNotePut(note, notecap, &k, ", bm 0x");
        WowNoteHex(note, notecap, &k, hbm, 4);
        WowNotePut(note, notecap, &k, ", scan ");
        WowNoteHex(note, notecap, &k, (DWORD)start, 4);
        WowNotePut(note, notecap, &k, "+");
        WowNoteHex(note, notecap, &k, (DWORD)lines, 4);
        WowNotePut(note, notecap, &k, bits ? ")" : ", bits=NULL = a size QUERY)");
        if (!d || (dk != WOWGDI_KIND_DC && dk != WOWGDI_KIND_WINDC)
               || !b || (bk != WOWGDI_KIND_OBJ && bk != WOWGDI_KIND_STOCK) || !bmi) {
            WowNotePut(note, notecap, &k, " -- ★ NOT OUR DC/BITMAP TOKENS, or no"
                                       " BITMAPINFO; answered 0");
            Wow32SetReturn(f, 0);
            return 1;
        }
        r = isget
            ? GetDIBits((HDC)d, (HBITMAP)b, start, lines,
                        (LPVOID)(ULONG_PTR)bits, (BITMAPINFO *)(ULONG_PTR)bmi, usage)
            : SetDIBits((HDC)d, (HBITMAP)b, start, lines,
                        (const VOID *)(ULONG_PTR)bits, (const BITMAPINFO *)(ULONG_PTR)bmi,
                        usage);
        WowNotePut(note, notecap, &k, " -> ");
        WowNoteHex(note, notecap, &k, (DWORD)r, 4);
        WowNotePut(note, notecap, &k, " scan lines");
        Wow32SetReturn(f, (DWORD)(WORD)r);
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
        volatile BYTE *p = Wow32ArgPointer(f, 0);
        INT k = 0;
        HPALETTE pal;
        WORD tok;
        WowNotePut(note, notecap, &k, "CreatePalette(");
        if (!p) {
            WowNotePut(note, notecap, &k, "NULL) -- ★ no LOGPALETTE; answered 0");
            Wow32SetReturn(f, 0);
            return 1;
        }
        WowNoteHex(note, notecap, &k, (DWORD)Wow32PeekWord(p + 2), 4);
        WowNotePut(note, notecap, &k, " entries)");
        pal = CreatePalette((const LOGPALETTE *)(ULONG_PTR)p);
        tok = pal ? WowGdiH16((HGDIOBJ)pal, WOWGDI_KIND_OBJ) : 0;
        if (!tok) {
            if (pal) DeleteObject((HGDIOBJ)pal);
            WowNotePut(note, notecap, &k, " -- ★ the OS refused it (or the token map"
                                       " is full); answered 0");
            Wow32SetReturn(f, 0);
            return 1;
        }
        WowNotePut(note, notecap, &k, " -> palette token 0x");
        WowNoteHex(note, notecap, &k, tok, 4);
        Wow32SetReturn(f, tok);
        return 1;
    }

    case WOWGDI_STRETCHDIBITS: {
        WORD  hdc = Wow32ArgWord(f, WOWGDI_SDI_ARG_HDC);
        INT   dx = (INT)(SHORT)Wow32ArgWord(f, WOWGDI_SDI_ARG_DSTX);
        INT   dy = (INT)(SHORT)Wow32ArgWord(f, WOWGDI_SDI_ARG_DSTY);
        INT   dw = (INT)(SHORT)Wow32ArgWord(f, WOWGDI_SDI_ARG_DSTW);
        INT   dh = (INT)(SHORT)Wow32ArgWord(f, WOWGDI_SDI_ARG_DSTH);
        INT   sx = (INT)(SHORT)Wow32ArgWord(f, WOWGDI_SDI_ARG_SRCX);
        INT   sy = (INT)(SHORT)Wow32ArgWord(f, WOWGDI_SDI_ARG_SRCY);
        INT   sw = (INT)(SHORT)Wow32ArgWord(f, WOWGDI_SDI_ARG_SRCW);
        INT   sh = (INT)(SHORT)Wow32ArgWord(f, WOWGDI_SDI_ARG_SRCH);
        WORD  usage = Wow32ArgWord(f, WOWGDI_SDI_ARG_USAGE);
        DWORD rop = Wow32ArgDword(f, WOWGDI_SDI_ARG_ROP);
        volatile BYTE *bits = Wow32ArgPointer(f, WOWGDI_SDI_ARG_BITS);
        volatile BYTE *bmi  = Wow32ArgPointer(f, WOWGDI_SDI_ARG_BMI);
        INT   kind = -1;
        HGDIOBJ o = WowGdiH32(hdc, &kind);
        INT   k = 0, r;
        WowNotePut(note, notecap, &k, "StretchDIBits(0x");
        WowNoteHex(note, notecap, &k, hdc, 4);
        WowNotePut(note, notecap, &k, " dst(");
        WowNoteHex(note, notecap, &k, (DWORD)dx, 4);
        WowNotePut(note, notecap, &k, ",");
        WowNoteHex(note, notecap, &k, (DWORD)dy, 4);
        WowNotePut(note, notecap, &k, ") ");
        WowNoteHex(note, notecap, &k, (DWORD)dw, 4);
        WowNotePut(note, notecap, &k, "x");
        WowNoteHex(note, notecap, &k, (DWORD)dh, 4);
        WowNotePut(note, notecap, &k, " <- src ");
        WowNoteHex(note, notecap, &k, (DWORD)sw, 4);
        WowNotePut(note, notecap, &k, "x");
        WowNoteHex(note, notecap, &k, (DWORD)sh, 4);
        WowNotePut(note, notecap, &k, " rop=0x");
        WowNoteHex(note, notecap, &k, rop, 8);
        WowNotePut(note, notecap, &k, ")");
        if (!o || (kind != WOWGDI_KIND_DC && kind != WOWGDI_KIND_WINDC)
               || !bmi || !bits) {
            WowNotePut(note, notecap, &k, " -- ★ NOT ONE OF OUR DC TOKENS, or no"
                                       " bits/BITMAPINFO; answered 0");
            Wow32SetReturn(f, 0);
            return 1;
        }
        r = StretchDIBits((HDC)o, dx, dy, dw, dh, sx, sy, sw, sh,
                          (const VOID *)(ULONG_PTR)bits,
                          (const BITMAPINFO *)(ULONG_PTR)bmi, usage, rop);
        WowNotePut(note, notecap, &k, " -> ");
        WowNoteHex(note, notecap, &k, (DWORD)r, 4);
        Wow32SetReturn(f, (DWORD)(WORD)r);
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
        WORD  hdc   = Wow32ArgWord(f, WOWGDI_CDIB_ARG_HDC);
        volatile BYTE *bmih = Wow32ArgPointer(f, WOWGDI_CDIB_ARG_BMIH);
        DWORD init  = Wow32ArgDword(f, WOWGDI_CDIB_ARG_INIT);
        volatile BYTE *bits = Wow32ArgPointer(f, WOWGDI_CDIB_ARG_BITS);
        volatile BYTE *bmi  = Wow32ArgPointer(f, WOWGDI_CDIB_ARG_BMI);
        WORD  usage = Wow32ArgWord(f, WOWGDI_CDIB_ARG_USAGE);
        INT   kind = -1;
        HGDIOBJ o = WowGdiH32(hdc, &kind);
        HBITMAP bm;
        WORD  tok;
        INT   k = 0;
        WowNotePut(note, notecap, &k, "CreateDIBitmap(dc 0x");
        WowNoteHex(note, notecap, &k, hdc, 4);
        WowNotePut(note, notecap, &k, ", init 0x");
        WowNoteHex(note, notecap, &k, init, 8);
        WowNotePut(note, notecap, &k, ")");
        if (!o || (kind != WOWGDI_KIND_DC && kind != WOWGDI_KIND_WINDC) || !bmih) {
            WowNotePut(note, notecap, &k, " -- ★ NOT ONE OF OUR DC TOKENS, or no"
                                       " BITMAPINFOHEADER; answered 0");
            Wow32SetReturn(f, 0);
            return 1;
        }
        bm = CreateDIBitmap((HDC)o,
                            (const BITMAPINFOHEADER *)(ULONG_PTR)bmih, init,
                            (init && bits) ? (const VOID *)(ULONG_PTR)bits : NULL,
                            (init && bmi)  ? (const BITMAPINFO *)(ULONG_PTR)bmi : NULL,
                            usage);
        if (!bm) {
            WowNotePut(note, notecap, &k, " -- ★ GDI REFUSED; answered 0");
            Wow32SetReturn(f, 0);
            return 1;
        }
        tok = WowGdiH16((HGDIOBJ)bm, WOWGDI_KIND_OBJ);
        if (!tok) {
            /* ⚠ NO TOKEN LEFT MEANS THE BITMAP LEAKS IF WE JUST RETURN 0 -- the
                 guest never learns of it, so nobody will ever DeleteObject it.
                 Destroy it here and fail honestly. */
            DeleteObject(bm);
            WowNotePut(note, notecap, &k, " -- ★ HANDLE MAP FULL; destroyed and"
                                       " answered 0");
            Wow32SetReturn(f, 0);
            return 1;
        }
        WowNotePut(note, notecap, &k, " -> 0x");
        WowNoteHex(note, notecap, &k, tok, 4);
        Wow32SetReturn(f, (DWORD)tok);
        return 1;
    }

    case WOWGDI_SETDIBITSTODEV: {
        WORD  hdc = Wow32ArgWord(f, WOWGDI_SDD_ARG_HDC);
        INT   dx = (INT)(SHORT)Wow32ArgWord(f, WOWGDI_SDD_ARG_DSTX);
        INT   dy = (INT)(SHORT)Wow32ArgWord(f, WOWGDI_SDD_ARG_DSTY);
        /* ⚠ w/h ARE `WORD`s IN THE Win16 PROTOTYPE, not ints -- a bitmap is never
             negative-width, and sign-extending one over 32767 would turn a blit
             into a negative and draw nothing. Widened UNSIGNED, unlike the
             coordinates either side of them, which are genuinely signed. */
        DWORD w  = (DWORD)Wow32ArgWord(f, WOWGDI_SDD_ARG_W);
        DWORD h  = (DWORD)Wow32ArgWord(f, WOWGDI_SDD_ARG_H);
        INT   sx = (INT)(SHORT)Wow32ArgWord(f, WOWGDI_SDD_ARG_SRCX);
        INT   sy = (INT)(SHORT)Wow32ArgWord(f, WOWGDI_SDD_ARG_SRCY);
        WORD  start  = Wow32ArgWord(f, WOWGDI_SDD_ARG_START);
        WORD  nscans = Wow32ArgWord(f, WOWGDI_SDD_ARG_NSCANS);
        volatile BYTE *bits = Wow32ArgPointer(f, WOWGDI_SDD_ARG_BITS);
        volatile BYTE *bmi  = Wow32ArgPointer(f, WOWGDI_SDD_ARG_BMI);
        WORD  usage = Wow32ArgWord(f, WOWGDI_SDD_ARG_USAGE);
        INT   kind = -1;
        HGDIOBJ o = WowGdiH32(hdc, &kind);
        INT   k = 0, r;
        WowNotePut(note, notecap, &k, "SetDIBitsToDevice(dc 0x");
        WowNoteHex(note, notecap, &k, hdc, 4);
        WowNotePut(note, notecap, &k, " dst(");
        WowNoteHex(note, notecap, &k, (DWORD)dx, 4);
        WowNotePut(note, notecap, &k, ",");
        WowNoteHex(note, notecap, &k, (DWORD)dy, 4);
        WowNotePut(note, notecap, &k, ") ");
        WowNoteHex(note, notecap, &k, (DWORD)w, 4);
        WowNotePut(note, notecap, &k, "x");
        WowNoteHex(note, notecap, &k, (DWORD)h, 4);
        WowNotePut(note, notecap, &k, ")");
        if (!o || (kind != WOWGDI_KIND_DC && kind != WOWGDI_KIND_WINDC)
               || !bmi || !bits) {
            WowNotePut(note, notecap, &k, " -- ★ NOT ONE OF OUR DC TOKENS, or no"
                                       " bits/BITMAPINFO; answered 0");
            Wow32SetReturn(f, 0);
            return 1;
        }
        r = SetDIBitsToDevice((HDC)o, dx, dy, w, h,
                              sx, sy, start, nscans,
                              (const VOID *)(ULONG_PTR)bits,
                              (const BITMAPINFO *)(ULONG_PTR)bmi, usage);
        WowNotePut(note, notecap, &k, " -> ");
        WowNoteHex(note, notecap, &k, (DWORD)r, 4);
        Wow32SetReturn(f, (DWORD)(WORD)r);
        return 1;
    }

    default:
        return 0;
    }
}

#endif /* NTVDMEX_WOWGDI_H */
