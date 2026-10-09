/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * GDI.EXE's call table: the thunk ids WOW32's GDI dispatcher answers, and their argument blocks.
 *
 * Split out of wowgdi.h (STYLE.md 5a): the call table only -- every thunk id the
 * dispatcher answers and every argument offset, each with its note. wowgdi.h holds the
 * host's own constants, types and prototypes, and includes this file.
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#ifndef NTVDMEX_WOWGDI_CALLS_H
#define NTVDMEX_WOWGDI_CALLS_H

#define WOWGDI_DELETEDC                 0x0044
#define WOWGDI_DELETEOBJECT             0x0045
#define WOWGDI_GETDEVICECAPS            0x0050

#define WOWGDI_GDC_ARG_INDEX            0
#define WOWGDI_GDC_ARG_HDC              2
#define WOWGDI_DOBJ_ARG_HANDLE          0

/* THE PRODUCERS -- WHAT MS PAINT ASKS FOR ONCE IT HAS A WINDOW (Importance = 3):
 * With `GetDC` in place (it is USER's call, not GDI's -- see wowuser.h) Paint
 * gets as far as needing a canvas, and says so in its own words when it does
 * not get one: "Not enough memory to edit image."
 *
 * [INFO]: 0x57 IS `GetStockObject`, AND ONLY A RUN COULD HAVE SAID SO. Its export
 * (GDI ordinal 87) is `native16` -- 16-bit code that only reaches the BOP from
 * inside its own body -- so `neneeds.py` classifies the import as free. Session
 * 44 wrote down that its id would have to come from a run; this is that run, 24
 * calls of it, arriving as id 0x57 with 2 argument bytes -- and 0x57 = 87, the
 * ids running with the ordinals as they do either side of it (0x58 =
 * GDI.88 GETSTRETCHBLTMODE).
 *
 * The rest are ordinary exports, from `neneeds.py --stubs`:
 *   ord 45 SELECTOBJECT           id 0x2d   4 args  retstub 0x0a0b
 *   ord 51 CREATECOMPATIBLEBITMAP id 0x33   6 args  retstub 0x01db
 *   ord 52 CREATECOMPATIBLEDC     id 0x34   2 args  retstub 0x01e8
 *   ord 66 CREATESOLIDBRUSH       id 0x42   4 args  retstub 0x0306
 * Blocks reversed as always, and each adds up to what its stub declares:
 * (HDC)=2, (HDC,HGDIOBJ)=4, (COLORREF)=4, (HDC,int,int)=6.
 */
/* THE FIRST DRAWING CALLS (Importance = 4):
 * Named by the run in which WM_PAINT was relayed to a guest for the first
 * time (session 45): MS Paint answered it with ten MoveTo/LineTo pairs and two
 * PatBlts. All three are ordinary exports, and `neneeds.py --stubs` agrees
 * with the return addresses the run printed:
 *   ord 19 LINETO   id 0x13   6 args  retstub 0x07dc
 *   ord 20 MOVETO   id 0x14   6 args  retstub 0x0810
 *   ord 29 PATBLT   id 0x1d  14 args  retstub 0x0885
 * Reversed as always: (HDC,int,int) puts y at +0, x at +2 and the DC at +4;
 * PatBlt's (HDC,int,int,int,int,DWORD) puts the 4-byte rop at +0 and the DC at
 * +12, which is the 14 bytes its stub declares.
 *
 * [CAUTION]: THE COORDINATES ARE SIGNED 16-BIT and must be sign-extended, not widened:
 * a Win16 program draws at negative coordinates routinely (scrolled content),
 * and 0xFFF0 read as 65520 would put the line off the far edge instead of 16
 * pixels to the left.
 *
 * [INFO]: MoveTo RETURNS THE PREVIOUS POSITION packed as y:x in a DWORD, which is why
 * it is not simply a void call -- guests save and restore it.
 */
/* THE REST OF PAINT'S DRAWING SET (Importance = 4):
 * Named from the calls themselves, and every one of them confirms its own
 * reading out of the values it carried:
 *
 *   0x22 BITBLT      20 args  (0020 00cc | 0000 | 0000 | 2090 | 029d | 04d1 |
 *                              0000 | 0000 | 20c0)
 *        [INFO]: the rop is 0x00CC0020 = SRCCOPY, and the two DC fields are both
 *        tokens of ours -- which is what pins which end is source.
 *   0x1b RECTANGLE   10 args  (0204 | 0079 | 0000 | 0000 | 20c0)
 *        [INFO]: = Rectangle(hdc, 0,0, 121, 516) -- exactly the toolbox's own size,
 *        so this is its border.
 *   0x04 SETROP2      4 args  (000d | 20c0)  -- 13 = R2_COPYPEN
 *   0x07 SETSTRETCHBLTMODE  4 (0003 | 20c0)  -- 3 = COLORONCOLOR
 *   0x01 SETBKCOLOR   6 args      0x09 SETTEXTCOLOR  6 args
 *   0x0b SETWINDOWORG 6 args      0x1e SAVEDC 2       0x27 RESTOREDC 4
 *
 * [CAUTION]: 0x04 and 0x07 are internal stubs (their exports are native16), so only the
 * run could name them -- and the two constants are what makes it a reading
 * rather than a guess at the ordinal.
 *
 * [INFO]: The raster ops, the ROP2 codes, the stretch modes and COLORREF all mean the
 * same thing in both worlds, so those travel unchanged; only the handles and
 * the signed 16-bit coordinates need work.
 */
#define WOWGDI_SETBKCOLOR               0x0001
#define WOWGDI_SETTEXTCOLOR             0x0009
#define WOWGDI_COL_ARG_COLOR            0
#define WOWGDI_COL_ARG_HDC              4

#define WOWGDI_SETROP2                  0x0004
#define WOWGDI_SETSTRETCHMODE           0x0007
#define WOWGDI_MODE_ARG_MODE            0
#define WOWGDI_MODE_ARG_HDC             2

#define WOWGDI_SETWINDOWORG             0x000b
#define WOWGDI_ORG_ARG_Y                0
#define WOWGDI_ORG_ARG_X                2
#define WOWGDI_ORG_ARG_HDC              4

#define WOWGDI_RECTANGLE                0x001b
#define WOWGDI_RC_ARG_BOTTOM            0
#define WOWGDI_RC_ARG_RIGHT             2
#define WOWGDI_RC_ARG_TOP               4
#define WOWGDI_RC_ARG_LEFT              6
#define WOWGDI_RC_ARG_HDC               8

#define WOWGDI_SAVEDC                   0x001e
#define WOWGDI_SDC_ARG_HDC              0
#define WOWGDI_RESTOREDC                0x0027
#define WOWGDI_RDC2_ARG_LEVEL           0
#define WOWGDI_RDC2_ARG_HDC             2

/* 0x23 StretchBlt -- THE TOOL ICONS THEMSELVES (Importance = 5):
 * GDI ordinal 35, a direct export, 24 argument bytes, and its own call names
 * every field:
 *
 * (0020 00cc | 0117 | 003a | 0000 | 0000 | 20c8 |
 *              0204 | 0079 | 0000 | 0000 | 20c0)
 *
 * = StretchBlt(dst, 0,0, 0x79 x 0x204, src, 0,0, 0x3a x 0x117, SRCCOPY)
 * -- 58x279 stretched into 121x516. 58x279 is exactly the pToolbox DIB this
 * host loads, and 121x516 is exactly the toolbox window's size as measured
 * against stock. Two numbers this session already knew independently, both
 * turning up in one argument block.
 */
#define WOWGDI_STRETCHBLT               0x0023
#define WOWGDI_SB_ARG_ROP               0       /* DWORD */
#define WOWGDI_SB_ARG_SRCH              4
#define WOWGDI_SB_ARG_SRCW              6
#define WOWGDI_SB_ARG_SRCY              8
#define WOWGDI_SB_ARG_SRCX              10
#define WOWGDI_SB_ARG_SRCDC             12
#define WOWGDI_SB_ARG_DSTH              14
#define WOWGDI_SB_ARG_DSTW              16
#define WOWGDI_SB_ARG_DSTY              18
#define WOWGDI_SB_ARG_DSTX              20
#define WOWGDI_SB_ARG_DSTDC             22

#define WOWGDI_BITBLT                   0x0022
#define WOWGDI_BB_ARG_ROP               0       /* DWORD */
#define WOWGDI_BB_ARG_SRCY              4
#define WOWGDI_BB_ARG_SRCX              6
#define WOWGDI_BB_ARG_SRCDC             8
#define WOWGDI_BB_ARG_HEIGHT            10
#define WOWGDI_BB_ARG_WIDTH             12
#define WOWGDI_BB_ARG_Y                 14
#define WOWGDI_BB_ARG_X                 16
#define WOWGDI_BB_ARG_DSTDC             18

/* THE STROKE LOOP (Importance = 4):
 * Named from the run in which the mouse first reached MS Paint: a single drag
 * across the canvas stepped over these 21 times each.
 *   0x63 LPtoDP                 8 args  (HDC, LPPOINT, int)  -- internal stub,
 *        named by its own call: `(0001 | 6df6 09c7 | 20c0)` is count 1, a far
 *        LPPOINT, and one of OUR DC tokens, which is what pins the order.
 *   0x9c CreateDiscardableBitmap ord 156, 6 args (HDC, int, int)
 *   0x94 SetBrushOrg            ord 148, 6 args (HDC, int, int)
 *
 * [CAUTION]: A Win16 POINT is two `int`s = 4 bytes against Win32's 8, so LPtoDP is a
 * conversion in BOTH directions -- it transforms in place, so every point has
 * to be read out, converted, and written back narrowed.
 */
/* [INFO]: 0x67 PtVisible(hDC, x, y) -- ord 103, 6 args, and the brush loop asks it
 * eight times per stroke: "is this point inside the clip region?". Answered 0
 * it means "no", so a guest politely declines to draw.
 */
/* [INFO]: Three one-argument calls the brush loop makes, all direct exports:
 * ord  79 GETDCORG        id 0x4f  (HDC)  -> the DC's origin, packed y:x
 * ord 149 GETBRUSHORG     id 0x95  (HDC)  -> the brush origin, packed y:x
 * ord 150 UNREALIZEOBJECT id 0x96  (HGDIOBJ) -> reset a brush's origin
 *
 * [CAUTION]: UnrealizeObject takes an OBJECT, not a DC -- it is how a guest tells GDI to
 * re-align a pattern brush before the next fill, which is exactly what a paint
 * program does between strokes.
 */
#define WOWGDI_GETDCORG                 0x004f
#define WOWGDI_GETBRUSHORG              0x0095
#define WOWGDI_UNREALIZEOBJ             0x0096
#define WOWGDI_ONE_ARG_HANDLE           0

#define WOWGDI_PTVISIBLE                0x0067
#define WOWGDI_PV_ARG_Y                 0
#define WOWGDI_PV_ARG_X                 2
#define WOWGDI_PV_ARG_HDC               4

/* [INFO]: 0x24 Polygon(hDC, lpPoints, nCount) -- GDI ordinal 36, native16, so the id
 * came from the run: `(0006 | 38fa 09c6 | 20d8)` is six points, a far LPPOINT
 * and one of our DC tokens -- the same block shape as LPtoDP, which is what
 * 8 argument bytes buys you. ord 35 STRETCHBLT -> 0x23 either side confirms
 * the numbering. [CAUTION] Polyline is ordinal 37 and would be 0x25 by the same
 * reading; no run has produced one, so it is not written on that basis.
 */
#define WOWGDI_POLYGON                  0x0024

#define WOWGDI_LPTODP                   0x0063
#define WOWGDI_LDP_ARG_COUNT            0
#define WOWGDI_LDP_ARG_POINTS           2
#define WOWGDI_LDP_ARG_HDC              6

#define WOWGDI_CREATEDISCARDBM          0x009c
#define WOWGDI_SETBRUSHORG              0x0094

#define WOWGDI_LINETO                   0x0013
#define WOWGDI_MOVETO                   0x0014
#define WOWGDI_XY_ARG_Y                 0
#define WOWGDI_XY_ARG_X                 2
#define WOWGDI_XY_ARG_HDC               4

#define WOWGDI_PATBLT                   0x001d
#define WOWGDI_PB_ARG_ROP               0
#define WOWGDI_PB_ARG_HEIGHT            4
#define WOWGDI_PB_ARG_WIDTH             6
#define WOWGDI_PB_ARG_Y                 8
#define WOWGDI_PB_ARG_X                 10
#define WOWGDI_PB_ARG_HDC               12

#define WOWGDI_SELECTOBJECT             0x002d
#define WOWGDI_SEL_ARG_OBJ              0
#define WOWGDI_SEL_ARG_HDC              2

/* 0x30 CreateBitmap, named from the call it actually made (Importance = 3):
 * GDI ordinal 48 `CREATEBITMAP` is `native16`, so -- as with `CreateDC` and
 * `GetStockObject` -- only a run could give its internal id. It arrived as
 *
 *   FUNC=0x00000030 args=0x0c retstub=0x01b4 (0000 0000 | 0001 | 0001 | 03ce | 0690)
 *
 * Twelve argument bytes is `(int, int, BYTE, BYTE, const void FAR*)`, which is
 * CreateBitmap's list and nothing else's, and reversed it reads
 * `CreateBitmap(0x0690, 0x03ce, 1, 1, NULL)` -- 1680 x 974, monochrome. The
 * numbers are the confirmation: Paint had just read `width`=0x0690 and
 * `height`=0x03ce out of WIN.INI's [Paintbrush] section, four calls earlier in
 * the same log. The ids run with the ordinals here (0x30 = 48) and the two
 * direct exports either side agree -- 51 -> 0x33, 52 -> 0x34 -- so the
 * numbering is continuous across native16 and wow32 entries alike.
 *
 * [CAUTION]: Which is NOT a rule to lean on: `CreateDC` is ordinal 53 and id 0x99. Name
 * each internal stub from its own call, never from arithmetic.
 */
#define WOWGDI_CREATEBITMAP             0x0030
#define WOWGDI_CBM_ARG_BITS             0       /* Const void FAR* -- NULL = uninitialised */
#define WOWGDI_CBM_ARG_BPP              4
#define WOWGDI_CBM_ARG_PLANES           6
#define WOWGDI_CBM_ARG_HEIGHT           8
#define WOWGDI_CBM_ARG_WIDTH            10

#define WOWGDI_CREATECOMPATBM           0x0033
#define WOWGDI_CCB_ARG_HEIGHT           0
#define WOWGDI_CCB_ARG_WIDTH            2
#define WOWGDI_CCB_ARG_HDC              4

#define WOWGDI_CREATECOMPATDC           0x0034
#define WOWGDI_CCD_ARG_HDC              0

#define WOWGDI_CREATESOLIDBRUSH         0x0042
#define WOWGDI_CSB_ARG_COLOR            0

#define WOWGDI_GETSTOCKOBJECT           0x0057
#define WOWGDI_GSO_ARG_INDEX            0

/* 0x52 GetObject(hObject, nCount, lpObject) (Importance = 3):
 * GDI ordinal 82, `native16`, so the id came from the run again:
 *
 *   FUNC=0x00000052 args=0x08 retstub=0x062c (4ee4 09c7 | 0032 | 2060)
 *
 * Eight argument bytes reversed give lpObject at +0 (a far pointer into
 * Paint's own data), nCount at +4 and the object at +6 -- and 0x2060 is one of
 * OUR GDI tokens, which is the confirmation that the last field is the handle.
 *
 * [CAUTION]: EVERY ONE OF THESE STRUCTURES IS A DIFFERENT SIZE IN Win16, so this is a
 * CONVERSION and not a copy -- the same trap OPENFILENAME was. Win16 keeps
 * coordinates and dimensions in `int` (2 bytes) where Win32 uses `LONG`:
 *     BITMAP    14 bytes here, 24 on Win32
 *     LOGFONT   50            , 60
 *     LOGPEN    10            , 16
 *     LOGBRUSH   8            , 16
 *
 * [INFO]: Paint's `nCount` of 0x32 = 50 is exactly Win16's LOGFONT (5 ints + 8 bytes
 * + a 32-byte face name), which is what makes the reading self-checking
 * rather than a size recalled from somewhere.
 *
 * [CAUTION]: THE TYPE COMES FROM THE OBJECT, NOT FROM nCount. Dispatching on the byte
 * count would be guessing at what the guest meant and would break the moment
 * one asked for a partial structure -- which Win16 explicitly allows. The OS
 * is asked what the handle actually is, the full Win16 form is built, and then
 * `min(nCount, that)` bytes are handed over, which is what Windows does.
 *
 * [CAUTION]: ONLY THE FONT CASE HAS BEEN SEEN IN A RUN. The other three are written from
 * the same size arithmetic and are marked in the log as they go, so the first
 * run that exercises one says so rather than passing silently.
 */
#define WOWGDI_GETOBJECT                0x0052
#define WOWGDI_GOB_ARG_BUF              0
#define WOWGDI_GOB_ARG_COUNT            4
#define WOWGDI_GOB_ARG_HANDLE           6

/* 0x99 -- AND IT IS `CreateIC`, NOT `CreateDC` (Importance = 3):
 *
 * [CAUTION]: **THIS BLOCK'S ORIGINAL CONCLUSION WAS WRONG AND IS CORRECTED IN PLACE**
 * (session 47). The call below is real and every argument reading of it holds;
 * what was wrong was the NAME. `0x99` is `CreateIC`, **GDI ordinal 153**, and
 * 153 IS 0x99 -- the id tracked the ordinal all along. `CreateDC` is ordinal 53
 * and its id is `0x35`, which is now answered next to it.
 * The mistake survived because it is invisible: an information context and a
 * device context answer every query identically, so servicing an IC as a DC
 * works perfectly and only a name in a log was wrong. It was found by teaching
 * `neneeds.py` to see through GDI's export wrappers, which resolves BOTH
 * ordinals from the binary and puts 53 -> 0x35 and 153 -> 0x99 side by side.
 *
 * **A wrong name is not harmless: the paragraph below drew a general rule
 * ("the id is not the ordinal") from a case where it was not true.**
 *
 * The original reading, which stands except for the name:
 *
 *   FUNC=0x00000099 stub=0x037f args=0x10 retstub=0x026a from=0x09df:0x13bb
 *     (0000 0000 | 0000 0000 | 0000 0000 | 0880 09c6)
 *     [INFO]: arg[6] 0x09c6:0x0880 = "display"
 *
 * Sixteen argument bytes is four far pointers, which is exactly
 * `CreateDC(lpszDriver, lpszDevice, lpszOutput, lpInitData)`, and reversed as
 * always the driver -- pushed first -- is at +12. So this is
 * `CreateDC("display", NULL, NULL, NULL)`: MS Paint asking for a screen DC to
 * size its canvas against.
 *
 * [INFO]: AND THE ARGUMENT BLOCK IS SHARED, which is why one case answers both: an IC
 * and a DC take the same four far pointers in the same order.
 */
#define WOWGDI_CREATEDC                 0x0099  /* <- ord 153 CreateIC (see above) */
#define WOWGDI_CDC_ARG_INITDATA         0
#define WOWGDI_CDC_ARG_OUTPUT           4
#define WOWGDI_CDC_ARG_DEVICE           8
#define WOWGDI_CDC_ARG_DRIVER           12

/* THE TOOLS THAT DID NOT WORK -- ENUMERATED, NOT GUESSED (Importance = 5):
 * "Some drawing functions work, others (like fill) do not" is not a mystery
 * once you enumerate what PBRUSH.EXE imports and which of those ordinals reach
 * the BOP. Each id and argument byte count below was taken from the call as it
 * arrives (e.g. ELLIPSE, ord 24: id 0x18, 10 argument bytes, retstub 0x0417).
 *
 * [INFO]: THE FILL IS `ExtFloodFill`, GDI ordinal 372, arriving as **id 0x174** with
 * 12 argument bytes -- and only for a valid fill TYPE (0 or 1); GDI.EXE checks
 * that before it comes out to us. That extra step is why a scan for plain
 * tail-jump exports calls it `native16` and reports it "free": it is not
 * free. Paint's fill tool calls ExtFloodFill TWICE -- once for a solid colour
 * and once after `CreatePatternBrush` -- which is why the
 * pattern brush is in this batch and not a later one.
 *
 * [CAUTION]: A Win16 fill type is Win32's fill type (0 = FLOODFILLBORDER, 1 = SURFACE),
 * the same claim the ROPs and COLORREFs already travel on.
 */
#define WOWGDI_ELLIPSE                  0x0018  /* Ord 24, 10 args -- RC_ARG_* layout */
#define WOWGDI_EXCLUDECLIPRECT          0x0015  /* Ord 21, 10 args -- RC_ARG_* layout */

#define WOWGDI_ROUNDRECT                0x001c  /* Ord 28, 14 args */
#define WOWGDI_RR_ARG_EH                0
#define WOWGDI_RR_ARG_EW                2
#define WOWGDI_RR_ARG_BOTTOM            4
#define WOWGDI_RR_ARG_RIGHT             6
#define WOWGDI_RR_ARG_TOP               8
#define WOWGDI_RR_ARG_LEFT              10
#define WOWGDI_RR_ARG_HDC               12

#define WOWGDI_EXTFLOODFILL             0x0174  /* Ord 372, 12 args -- THE FILL */
#define WOWGDI_FF_ARG_TYPE              0
#define WOWGDI_FF_ARG_COLOR             2       /* DWORD */
#define WOWGDI_FF_ARG_Y                 6
#define WOWGDI_FF_ARG_X                 8
#define WOWGDI_FF_ARG_HDC               10

#define WOWGDI_CREATEPATTERNBRUSH       0x003c  /* Ord 60, 2 args (HBITMAP) */
#define WOWGDI_GETPIXEL                 0x0053  /* Ord 83, 6 args -- XY_ARG_* layout */
#define WOWGDI_GETBKCOLOR               0x004b  /* Ord 75, 2 args -- PBRUSH.DLL's */
#define WOWGDI_GETROP2                  0x0055  /* Ord 85, 2 args */
#define WOWGDI_UPDATECOLORS             0x016e  /* Ord 366, 2 args */

/* s89 (#270): the rest of the one-DC getters, named by GDI.EXE's own export
 * table (wowmap.py: each DIRECT, 2 argument bytes). GetBkMode was the one the
 * Win16 tests caught answering the harness's 0 where the SDK default is OPAQUE.
 */
#define WOWGDI_GETBKMODE                0x004c  /* Ord 76 */
#define WOWGDI_GETMAPMODE               0x0051  /* Ord 81 */
#define WOWGDI_GETPOLYFILLMODE          0x0054  /* Ord 84 */
#define WOWGDI_GETSTRETCHBLTMODE        0x0058  /* Ord 88 */
#define WOWGDI_GETTEXTCOLOR             0x005a  /* Ord 90, a DWORD COLORREF */

#define WOWGDI_CREATERECTRGN            0x0040  /* Ord 64, 8 args */
#define WOWGDI_RGN_ARG_BOTTOM           0
#define WOWGDI_RGN_ARG_RIGHT            2
#define WOWGDI_RGN_ARG_TOP              4
#define WOWGDI_RGN_ARG_LEFT             6

#define WOWGDI_SELECTCLIPRGN            0x002c  /* Ord 44, 4 args */
#define WOWGDI_SCR_ARG_RGN              0
#define WOWGDI_SCR_ARG_HDC              2

/* -- THE REST OF THE REGION API, AND THE TWO TEXT CALLS THAT GO WITH IT.
 * (session 55) Every one of these is a Win32 function of the same name and
 * the same meaning, so the body is a translation of handles and a call --
 * there is nothing to invent, which is exactly why they are worth doing in a
 * batch. The argument offsets follow this file's one rule: the FIRST
 * parameter sits at the HIGHEST offset, because the block is the pushed
 * arguments and the base is the last push.
 */
#define WOWGDI_COMBINERGN               0x002f  /* Ord 47, 8 args */
#define WOWGDI_CBR_ARG_MODE             0
#define WOWGDI_CBR_ARG_SRC2             2
#define WOWGDI_CBR_ARG_SRC1             4
#define WOWGDI_CBR_ARG_DEST             6

#define WOWGDI_CREATERECTRGNIND         0x0041  /* Ord 65, 4 args */
#define WOWGDI_CRRI_ARG_RECT            0       /* Far pointer to a Win16 RECT */

#define WOWGDI_SETRECTRGN               0x00ac  /* Ord 172, 10 args */
#define WOWGDI_SRR_ARG_BOTTOM           0
#define WOWGDI_SRR_ARG_RIGHT            2
#define WOWGDI_SRR_ARG_TOP              4
#define WOWGDI_SRR_ARG_LEFT             6
#define WOWGDI_SRR_ARG_RGN              8

#define WOWGDI_CREATEPOLYGONRGN         0x003f  /* Ord 63, 8 args */
#define WOWGDI_CPR_ARG_MODE             0
#define WOWGDI_CPR_ARG_COUNT            2
#define WOWGDI_CPR_ARG_POINTS           4       /* Far pointer to an array of POINT16 */

#define WOWGDI_GETCLIPBOX               0x004d  /* Ord 77, 6 args */
#define WOWGDI_GCX_ARG_RECT             0       /* Far pointer, written back */
#define WOWGDI_GCX_ARG_HDC              4

#define WOWGDI_GETTEXTFACE              0x005c  /* Ord 92, 8 args */
#define WOWGDI_GTF_ARG_BUF              0       /* Far pointer, written back */
#define WOWGDI_GTF_ARG_COUNT            4
#define WOWGDI_GTF_ARG_HDC              6

#define WOWGDI_SETTEXTJUST              0x000a  /* Ord 10, 6 args */
#define WOWGDI_STJ_ARG_COUNT            0
#define WOWGDI_STJ_ARG_EXTRA            2
#define WOWGDI_STJ_ARG_HDC              4

/* The three mapping-mode setters. All 6 args, all the same (hDC, x, y) block as
 * SetWindowOrg, and all returning the PREVIOUS pair packed y:x in a DWORD.
 */
#define WOWGDI_SETWINDOWEXT             0x000c  /* Ord 12 */
#define WOWGDI_SETVIEWPORTORG           0x000d  /* Ord 13 */
#define WOWGDI_SETVIEWPORTEXT           0x000e  /* Ord 14 */
#define WOWGDI_SETBITMAPDIM             0x00a3  /* Ord 163 -- same block, but a BITMAP */

#define WOWGDI_GETNEARESTCOLOR          0x009a  /* Ord 154, 6 args -- COL_ARG_* layout */
#define WOWGDI_GETNEARESTPALIDX         0x0172  /* Ord 370, 6 args -- HPALETTE + COLORREF */

/* AND THIS IS WHY THE BOX AND THE ELLIPSE DREW NOTHING (Importance = 5):
 * The tools were selected, the rubber band tracked the drag in `R2_XORPEN`
 * and the guest then set `R2_COPYPEN` to commit -- and the commit is six
 * calls, of which the run showed TWO stepped over:
 *
 *   SetBkMode(0x20c0, 0002)          id 0x02, 4 args   -- UNIMPLEMENTED
 *   SetROP2(0x20c0, 000d)            id 0x04           -- serviced
 *   SelectObject(0x20c0, 0x2028)     id 0x2d           -- serviced (the brush)
 *   CreatePen(006, 0002, 0x000000ff) id 0x3d, 8 args   -- UNIMPLEMENTED
 *   SelectObject(0x20c0, 0x2000)     ...               -- and it gave up
 *
 * Paint asked for a 2-pixel `PS_INSIDEFRAME` pen in the colour it had been
 * given, got 0, and correctly declined to draw with a pen that does not
 * exist. Nothing was wrong with Ellipse or Rectangle -- 48 Ellipse calls in
 * that same drag returned 1. **A tool that cannot make a pen has nothing to
 * draw with.**
 *
 * [INFO]: TWO INDEPENDENT READINGS AGREE ON 0x3d. The run logged
 * `FUNC=0x3d ... (0000ff00 00000000 00000002 00000006)` -- a green pen for the
 * ellipse and a red one for the box -- and GDI's export table says ordinal 61
 * is CREATEPEN, whose three arguments are 8 bytes. [CAUTION] Here the id happens to
 * EQUAL the ordinal; elsewhere it does not (CreateDC is ordinal 53 and id
 * 0x99), so each of these was taken from a run rather than assumed.
 *
 * [CAUTION]: `neneeds.py` calls all of these "free (16-bit)", because GDI's export is a
 * validating wrapper rather than a bare tail-jump, which the scan does not
 * follow. That is the `native16` trap again: **the run finds
 * them, the static list does not.**
 */
#define WOWGDI_CREATEPEN                0x003d  /* Ord 61, 8 args (style, width, colour) */
#define WOWGDI_CP_ARG_COLOR             0       /* DWORD */
#define WOWGDI_CP_ARG_WIDTH             4
#define WOWGDI_CP_ARG_STYLE             6

#define WOWGDI_CREATEHATCHBRUSH         0x003a  /* Ord 58, 6 args (index, colour) */
#define WOWGDI_CH_ARG_COLOR             0       /* DWORD */
#define WOWGDI_CH_ARG_INDEX             4

#define WOWGDI_SETBKMODE                0x0002  /* Ord 2, 4 args -- MODE_ARG_* layout */
#define WOWGDI_SETMAPMODE               0x0003  /* Ord 3, 4 args */
#define WOWGDI_SETPOLYFILLMODE          0x0006  /* Ord 6, 4 args */

#define WOWGDI_SETPIXEL                 0x001f  /* Ord 31, 10 args */
#define WOWGDI_SP_ARG_COLOR             0       /* DWORD */
#define WOWGDI_SP_ARG_Y                 4
#define WOWGDI_SP_ARG_X                 6
#define WOWGDI_SP_ARG_HDC               8

#define WOWGDI_POLYLINE                 0x0025  /* Ord 37, 8 args -- LDP_ARG_* layout */

/* THE SECOND SWEEP: EVERYTHING ELSE THESE TWO PROGRAMS IMPORT (Importance = 5):
 * Session 47 taught `tools/ne/neneeds.py` to see through GDI's validating
 * export wrappers, and the list of what MS Paint and Notepad reach went from
 * "41 need us" to **76**. Everything below is on that list, and every id and
 * argument count is checked against the call as it arrives -- no id here was
 * inferred from an ordinal, and several of them differ from it.
 *
 * [INFO]: THE CORRECTION IT FORCED: **`0x99` IS `CreateIC` (ordinal 153), NOT
 * `CreateDC`.** `CreateDC` is ordinal 53 and its id is `0x35`. Session 45 named
 * `0x99` from a run, and 153 = 0x99 -- the id tracked the ordinal after all,
 * just not the ordinal we thought. It went unnoticed because an information
 * context and a device context answer every query identically, so servicing an
 * IC as a DC works and only the NAME was wrong. => both are answered here, and
 * the log says which one the guest asked for.
 */
#define WOWGDI_CREATEDC2                0x0035  /* Ord 53, 16 args -- the REAL CreateDC */

/* THE SHELF, BATCH TWO: what MPLAYER and CHARMAP still want from GDI (Importance = 2): */
#define WOWGDI_INTERSECTCLIPRECT        0x0016
#define WOWGDI_ICR_ARG_BOTTOM           0
#define WOWGDI_ICR_ARG_RIGHT            2
#define WOWGDI_ICR_ARG_TOP              4
#define WOWGDI_ICR_ARG_LEFT             6
#define WOWGDI_ICR_ARG_HDC              8

#define WOWGDI_RECTVISIBLE              0x0068
#define WOWGDI_RV_ARG_RECT              0
#define WOWGDI_RV_ARG_HDC               4

/* CreateFont's fourteen parameters, LAST push first. CHARMAP's whole job is
 * showing one face at a large size, so this is the call it lives or dies on.
 */
#define WOWGDI_CREATEFONT               0x0038
#define WOWGDI_CF_ARG_FACE              0
#define WOWGDI_CF_ARG_PITCH             4
#define WOWGDI_CF_ARG_QUALITY           6
#define WOWGDI_CF_ARG_CLIPPREC          8
#define WOWGDI_CF_ARG_OUTPREC           10
#define WOWGDI_CF_ARG_CHARSET           12
#define WOWGDI_CF_ARG_STRIKE            14
#define WOWGDI_CF_ARG_UNDER             16
#define WOWGDI_CF_ARG_ITALIC            18
#define WOWGDI_CF_ARG_WEIGHT            20
#define WOWGDI_CF_ARG_ORIENT            22
#define WOWGDI_CF_ARG_ESCAPE            24
#define WOWGDI_CF_ARG_WIDTH             26
#define WOWGDI_CF_ARG_HEIGHT            28

/* [CAUTION]: GetCharWidth writes ONE WORD PER CHARACTER into the guest's buffer. Win32's
 * writes an INT each; converting is not optional -- handing back 32-bit values
 * would overrun the guest's array by a factor of two, silently, into whatever
 * it declared next.
 */
#define WOWGDI_GETCHARWIDTH             0x015e
#define WOWGDI_GCW_ARG_BUF              0
#define WOWGDI_GCW_ARG_LAST             4
#define WOWGDI_GCW_ARG_FIRST            6
#define WOWGDI_GCW_ARG_HDC              8

/* METAFILES: SOUNDREC's last three, and PACKAGER wants them too (Importance = 2):
 * A metafile DC is a recording, not a surface: CreateMetaFile hands back a DC
 * that remembers calls, CloseMetaFile turns the recording into a metafile
 * handle, DeleteMetaFile throws it away. All three are real Win32 calls; the
 * only work here is that a DC token and a METAFILE token are different kinds
 * and must not be confused -- closing a metafile DC yields an object that is
 * NOT a DC, and handing it back under a DC token would let the guest pass it
 * to TextOut.
 *
 * [CAUTION]: lpszFile is usually NULL (a memory metafile). NULL is not "missing", it is
 * the common case, and passing "" instead would try to create a file called
 * nothing in the current directory.
 */
#define WOWGDI_CREATEMETAFILE           0x007d
#define WOWGDI_CMF_ARG_FILE             0
#define WOWGDI_CLOSEMETAFILE            0x007e
#define WOWGDI_DELETEMETAFILE           0x007f
#define WOWGDI_COPYMETAFILE             0x0097

/* s90 (#295): PlayMetaFile(hdc, hmf), GDI.123, 4 bytes reversed: +0 hmf, +2 hdc. */
#define WOWGDI_PLAYMETAFILE             0x007b
#define WOWGDI_PMF_ARG_HMF              0
#define WOWGDI_PMF_ARG_HDC              2

/* #295: EnumMetaFile(hdc, hmf, lpfn, lParam), GDI.175, 12 bytes (the inventory's
 * thunk width), reversed: +0 lParam, +4 lpfn, +8 hmf, +10 hdc. A CALLBACK:
 * int FAR PASCAL proc(HDC, HANDLETABLE FAR*, METARECORD FAR*, int nObj, LPARAM).
 */
#define WOWGDI_ENUMMETAFILE             0x00af
#define WOWGDI_EMF_ARG_LPARAM           0
#define WOWGDI_EMF_ARG_PROC             4
#define WOWGDI_EMF_ARG_HMF              8
#define WOWGDI_EMF_ARG_HDC              10

/* #295: PlayMetaFileRecord(hdc, lpht, lpmr, nHandles), GDI.176, 12 bytes,
 * reversed: +0 nHandles, +2 lpmr, +6 lpht, +10 hdc.
 */
#define WOWGDI_PLAYMETAFILEREC          0x00b0
#define WOWGDI_PMFR_ARG_NHANDLES        0
#define WOWGDI_PMFR_ARG_MR              2
#define WOWGDI_PMFR_ARG_HT              6
#define WOWGDI_PMFR_ARG_HDC             10
#define WOWGDI_MF1_ARG_H                0
#define WOWGDI_CPMF_ARG_FILE            0
#define WOWGDI_CPMF_ARG_HMF             4

#define WOWGDI_TEXTOUT                  0x0021  /* Ord 33, 12 args */

/* -- ExtTextOut(hdc, x, y, opts, lprc, str, count, lpDx) -- ord 351, 22 args.
 * CLOCK's only outstanding GDI service, and the one TextOut cannot stand in
 * for: the digital face draws each string CLIPPED and OPAQUE to a rectangle so
 * the previous second is erased in the same call, and the analogue face uses
 * the same entry with no rect. Substituting TextOut would drop the clip and the
 * background fill, which is a wrong picture rather than a missing one.
 *
 * [CAUTION]: THE RECT POINTER IS OPTIONAL AND OFTEN NULL, and it must stay null: passing
 * an empty RECT with ETO_CLIPPED clips everything away, i.e. draws nothing at
 * all -- silently, and looking exactly like "the guest never called us".
 *
 * [CAUTION]: lpDx (per-character spacing) is honoured only when the guest supplies it.
 * Fabricating even spacing would be inventing a layout the program did not ask
 * for; NULL means "use the font's own", which is what Win32 does too.
 */
#define WOWGDI_EXTTEXTOUT               0x015f
#define WOWGDI_ETO_ARG_DX               0
#define WOWGDI_ETO_ARG_COUNT            4
#define WOWGDI_ETO_ARG_STR              6
#define WOWGDI_ETO_ARG_RECT             10
#define WOWGDI_ETO_ARG_OPTS             14
#define WOWGDI_ETO_ARG_Y                16
#define WOWGDI_ETO_ARG_X                18
#define WOWGDI_ETO_ARG_HDC              20
#define WOWGDI_TO_ARG_COUNT             0
#define WOWGDI_TO_ARG_STR               2       /* Far */
#define WOWGDI_TO_ARG_Y                 6
#define WOWGDI_TO_ARG_X                 8
#define WOWGDI_TO_ARG_HDC               10

#define WOWGDI_GETTEXTEXTENT            0x005b  /* Ord 91, 8 args */
#define WOWGDI_TE_ARG_COUNT             0
#define WOWGDI_TE_ARG_STR               2       /* Far */
#define WOWGDI_TE_ARG_HDC               6

/* 0x5d GetTextMetrics, and the Win16 TEXTMETRIC, CHECKED ON NOTEPAD (Importance = 1):
 * Its fields are `short` where Win32's are `LONG`, in the same order (the
 * documented Win16 layout), and the order is checked by behaviour, not taken
 * on trust: Notepad's LINE HEIGHT comes out as tmHeight + tmExternalLeading
 * (tm+0 + tm+8) and its TAB STOP as 8 x tmAveCharWidth (tm+10) -- visibly
 * wrong on screen if either offset were. That pins the first six fields and
 * therefore the whole `short` prefix. Two independent uses, one layout.
 */
#define WOWGDI_GETTEXTMETRICS           0x005d  /* Ord 93, 6 args */
#define WOWGDI_TM_ARG_BUF               0       /* Far */
#define WOWGDI_TM_ARG_HDC               4

#define WOWGDI_SETTEXTALIGN             0x015a  /* Ord 346, 4 args -- MODE_ARG_* layout */
#define WOWGDI_CREATEFONTIND            0x0039  /* Ord 57, 4 args (far LOGFONT) */
#define WOWGDI_CREATEPALETTE            0x0168  /* Ord 360, 4 args (far LOGPALETTE) */

#define WOWGDI_DPTOLP                   0x0043  /* Ord 67, 8 args -- LDP_ARG_* layout */

/* 0x4a GetBitmapBits / 0x6a SetBitmapBits -- PBRUSH.DLL's OWN PAIR (Importance = 1):
 * (hBitmap, dwCount, lpBits): 2 + 4 + 4 = 10 bytes. The "virtual bitmap
 * manager" DLL that owns MS Paint's off-screen image is built on these two,
 * which is why they are the only thing it still needed.
 */
#define WOWGDI_GETBITMAPBITS            0x004a
#define WOWGDI_SETBITMAPBITS            0x006a
#define WOWGDI_BB2_ARG_BITS             0       /* Far */
#define WOWGDI_BB2_ARG_COUNT            4       /* DWORD */
#define WOWGDI_BB2_ARG_HBM              8

/* THE DIB TRIO -- WHAT `File > Save As` DIES ON (Importance = 5):
 * A `BITMAPINFOHEADER` is 40 bytes and byte-identical in both worlds, and the
 * bits and the header both live in guest memory this host can address
 * directly, so these are the rare calls that need NO conversion at all -- only
 * the handles and the signed 16-bit coordinates.
 *
 * [CAUTION]: THE ARGUMENT BLOCK IS THE ONLY PLACE TO GET WRONG, and the counts pin it:
 * SetDIBits/GetDIBits declare 18 bytes = 2+2+2+2+4+4+2 and StretchDIBits 32.
 */
#define WOWGDI_SETDIBITS                0x01b8  /* Ord 440, 18 args */
#define WOWGDI_GETDIBITS                0x01b9  /* Ord 441, 18 args */
#define WOWGDI_DIB_ARG_USAGE            0
#define WOWGDI_DIB_ARG_BMI              2       /* Far */
#define WOWGDI_DIB_ARG_BITS             6       /* Far */
#define WOWGDI_DIB_ARG_LINES            10
#define WOWGDI_DIB_ARG_START            12
#define WOWGDI_DIB_ARG_HBM              14
#define WOWGDI_DIB_ARG_HDC              16

#define WOWGDI_STRETCHDIBITS            0x01b7  /* Ord 439, 32 args */
#define WOWGDI_SDI_ARG_ROP              0       /* DWORD */
#define WOWGDI_SDI_ARG_USAGE            4
#define WOWGDI_SDI_ARG_BMI              6       /* Far */
#define WOWGDI_SDI_ARG_BITS             10      /* Far */
#define WOWGDI_SDI_ARG_SRCH             14
#define WOWGDI_SDI_ARG_SRCW             16
#define WOWGDI_SDI_ARG_SRCY             18
#define WOWGDI_SDI_ARG_SRCX             20
#define WOWGDI_SDI_ARG_DSTH             22
#define WOWGDI_SDI_ARG_DSTW             24
#define WOWGDI_SDI_ARG_DSTY             26
#define WOWGDI_SDI_ARG_DSTX             28
#define WOWGDI_SDI_ARG_HDC              30

/* -- MINESWEEPER'S TWO. It keeps its digits, mines and smiley faces as DIBs in
 * its own resources and puts them on screen with these; nothing else it draws
 * needs GDI at all.
 * HBITMAP CreateDIBitmap(HDC, LPBITMAPINFOHEADER, DWORD dwInit, LPSTR lpbInit,
 *                      LPBITMAPINFO, UINT wUsage)                     = 20
 */
#define WOWGDI_CREATEDIBITMAP           0x01ba  /* Ord 442, 20 args */
#define WOWGDI_CDIB_ARG_USAGE           0
#define WOWGDI_CDIB_ARG_BMI             2       /* Far */
#define WOWGDI_CDIB_ARG_BITS            6       /* Far */
#define WOWGDI_CDIB_ARG_INIT            10      /* DWORD */
#define WOWGDI_CDIB_ARG_BMIH            14      /* Far */
#define WOWGDI_CDIB_ARG_HDC             18

/* int SetDIBitsToDevice(HDC, int xDest, int yDest, WORD wWidth, WORD wHeight,
 * int XSrc, int YSrc, UINT nStartScan, UINT nNumScans,
 * LPSTR lpBits, LPBITMAPINFO, UINT wUsage)        = 28
 */
#define WOWGDI_SETDIBITSTODEV           0x01bb  /* Ord 443, 28 args */
#define WOWGDI_SDD_ARG_USAGE            0
#define WOWGDI_SDD_ARG_BMI              2       /* Far */
#define WOWGDI_SDD_ARG_BITS             6       /* Far */
#define WOWGDI_SDD_ARG_NSCANS           10
#define WOWGDI_SDD_ARG_START            12
#define WOWGDI_SDD_ARG_SRCY             14
#define WOWGDI_SDD_ARG_SRCX             16
#define WOWGDI_SDD_ARG_H                18
#define WOWGDI_SDD_ARG_W                20
#define WOWGDI_SDD_ARG_DSTY             22
#define WOWGDI_SDD_ARG_DSTX             24
#define WOWGDI_SDD_ARG_HDC              26

/* -- int Escape(HDC, int nEscape, int nCount, LPSTR lpInData, LPSTR lpOut) = 14
 * The device-driver back door, and on a SCREEN DC the honest answer to almost
 * all of it is "this driver does not do that", which Escape spells 0. That is
 * not a stub: 0 is the documented in-band answer for an escape the driver does
 * not implement, and a caller that asks QUERYESCSUPPORT first -- which is what
 * the escape exists for -- gets told before it tries.
 *
 * [CAUTION]: A PRINTING PATH WOULD NEED THE REST (STARTDOC/NEWFRAME/ENDDOC and an abort
 * procedure that calls back into 16-bit code). This host has no printer DC to
 * start one on, so what is NOT done is named here rather than half-built.
 */
#define WOWGDI_ESCAPE                   0x0026  /* Ord 38, 14 args */
#define WOWGDI_ESC_ARG_OUT              0       /* Far */
#define WOWGDI_ESC_ARG_IN               4       /* Far */
#define WOWGDI_ESC_ARG_COUNT            8
#define WOWGDI_ESC_ARG_ESCAPE           10
#define WOWGDI_ESC_ARG_HDC              12

/* HBITMAP CreateBitmapIndirect(LPBITMAP) = 4:
 * CreateBitmap's arguments, in a structure, and the structure is a Win16 one:
 * +0  short bmType        +8  BYTE bmPlanes
 * +2  short bmWidth       +9  BYTE bmBitsPixel
 * +4  short bmHeight     +10  LPVOID bmBits (far)
 * +6  short bmWidthBytes
 * = 14 bytes. Win32's BITMAP is 24 with LONGs and a 32-bit pointer, so this is
 * read field by field rather than cast -- the same rule as every other shared
 * structure in this host.
 */
#define WOWGDI_CREATEBITMAPINDIRECT     0x0031  /* Ord 49, 4 args */
#define WOWGDI_CBI_ARG_BITMAP           0       /* Far */

/* BOOL GetCharABCWidths(HDC, UINT first, UINT last, LPABC) = 10:
 *
 * [CAUTION]: THE ABC STRUCTURE IS A DIFFERENT SIZE IN THE TWO WORLDS, and this is the
 * `RECT is 8 bytes not 16` trap again: Win16's ABC is `{ int abcA; UINT abcB;
 * int abcC; }` = SIX bytes; Win32's is three LONGs = TWELVE. Handing the
 * guest's six-byte-per-glyph array to Win32 would overrun it by a factor of
 * two before the first character was measured. Converted per glyph in
 * wowconv.h, where the battery can pin it.
 */
#define WOWGDI_GETCHARABCWIDTHS         0x0133  /* Ord 307, 10 args */
#define WOWGDI_ABCW_ARG_ABC             0       /* Far */
#define WOWGDI_ABCW_ARG_LAST            4
#define WOWGDI_ABCW_ARG_FIRST           6
#define WOWGDI_ABCW_ARG_HDC             8

/* -- UINT GetPaletteEntries(HPALETTE, UINT start, UINT n, LPPALETTEENTRY) = 10
 *
 * [INFO]: PALETTEENTRY IS FOUR BYTES IN BOTH -- peRed, peGreen, peBlue, peFlags --
 * so this one really is a copy, and saying which structures are identical
 * matters as much as saying which are not.
 */
/* s90 (#297): SetPaletteEntries -- the same frame as GetPaletteEntries (GPE_ARG_*). */
#define WOWGDI_SETPALETTEENTRIES        0x016c  /* Ord 364, 10 args */

/* s90 (#295): the packed-DWORD extent getters. Win16 returns MAKELONG(cx, cy); Win32
 * has only the *Ex forms. One argument each (an HDC, or an HBITMAP).
 */
#define WOWGDI_GETVIEWPORTEXT           0x005e  /* Ord 94 */
#define WOWGDI_GETWINDOWEXT             0x0060  /* Ord 96 */
#define WOWGDI_GETBITMAPDIMENSION       0x00a2  /* Ord 162 */
#define WOWGDI_GETPALETTEENTRIES        0x016b  /* Ord 363, 10 args */
#define WOWGDI_GPE_ARG_ENTRIES          0       /* Far */
#define WOWGDI_GPE_ARG_COUNT            4
#define WOWGDI_GPE_ARG_START            6
#define WOWGDI_GPE_ARG_HPAL             8

/* void LineDDA(int x1, int y1, int x2, int y2, FARPROC, LPARAM) = 16 (Importance = 1):
 * The only enumeration in GDI whose callback takes NO STRUCTURE: it is called
 * with (x, y, lpData) for every point on the line, and the caller does the
 * drawing. That is why this one is implemented and EnumFonts/EnumObjects are
 * not -- theirs pass LOGFONT/TEXTMETRIC/LOGPEN pointers whose Win16 layouts
 * this host has not measured. See the header note in src/wow/wowenum.h.
 *
 * [CAUTION]: IT RETURNS NOTHING. A caller reads no value, so there is no hole to revise
 * and a callback's answer cannot stop it -- but the chain honours the veto
 * anyway, because a Win16 program that returns 0 from a DDA callback expects
 * to stop being called, whatever the function's own return says.
 */
#define WOWGDI_LINEDDA                  0x0064  /* Ord 100, 16 args */

/* s89 (#162, Charmap's font list): EnumFontFamilies(hdc, lpszFamily, proc, lParam),
 * GDI.330, a WRAPPER stub of 14 argument bytes (wowmap.py). Frame, reversed:
 * +0 lParam, +4 proc, +8 lpszFamily (far, NULL = one per family), +12 hdc.
 */
#define WOWGDI_ENUMFONTFAMILIES         0x014a

/* s90 (#296): EnumFonts(hdc, lpszFace, proc, lParam), GDI.70, the same 14-byte
 * frame as EnumFontFamilies (EFF_ARG_*); its callback takes LOGFONT + TEXTMETRIC,
 * which are the leading parts of the ENUMLOGFONT/NEWTEXTMETRIC blob it shares.
 */
#define WOWGDI_ENUMFONTS                0x0046

/* s90 (#296): EnumObjects(hdc, nObjectType, proc, lParam), GDI.71, 12 bytes,
 * reversed: +0 lParam, +4 proc, +8 type (1 OBJ_PEN, 2 OBJ_BRUSH), +10 hdc.
 * Callback: int EnumObjectsProc(LPVOID lpLogObject, LPARAM). Win16's structures
 * (windows.h 3.1): LOGPEN {UINT style; POINT width; COLORREF color} = 10 bytes,
 * LOGBRUSH {UINT style; COLORREF color; int hatch} = 8 -- UINT/int/POINT being
 * 16-bit is the whole difference from Win32's.
 */
#define WOWGDI_ENUMOBJECTS              0x0047
#define WOWGDI_EOB_ARG_LPARAM           0
#define WOWGDI_EOB_ARG_PROC             4
#define WOWGDI_EOB_ARG_TYPE             8
#define WOWGDI_EOB_ARG_HDC              10
#define WOWGDI_EFF_ARG_LPARAM           0
#define WOWGDI_EFF_ARG_PROC             4
#define WOWGDI_EFF_ARG_FAMILY           8
#define WOWGDI_EFF_ARG_HDC              12
#define WOWGDI_LDDA_ARG_DATA            0       /* DWORD */
#define WOWGDI_LDDA_ARG_PROC            4       /* Far */
#define WOWGDI_LDDA_ARG_Y2              8
#define WOWGDI_LDDA_ARG_X2              10
#define WOWGDI_LDDA_ARG_Y1              12
#define WOWGDI_LDDA_ARG_X1              14

#endif /* NTVDMEX_WOWGDI_CALLS_H */
