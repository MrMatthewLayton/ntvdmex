/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * GDI.EXE's OWN ID SPACE.  GH #128, session 44.
 *
 * A SEVENTH id space, and the first one opened for its own sake rather than
 * because a guest stopped on it: `tools/ne/neneeds.py` says NOTEPAD.EXE reaches
 * three of GDI's thunks directly, and MS PAINT reaches **41**. So this file is
 * where the north star's other half begins, and the three below are its first
 * three lines rather than the whole of it.
 *
 * THE IDS:
 * Export ordinal against the id, argument bytes and return-stub offset the call
 * arrives with:
 *    68 DELETEDC       id 0x44   2 args  retstub 0x033a
 *    69 DELETEOBJECT   id 0x45   2 args  retstub 0x0354
 *    80 GETDEVICECAPS  id 0x50   4 args  retstub 0x05de
 * The ids are the export ordinals again -- checked here, as it is checked per
 * module, and never assumed: krnl386's are nothing like its ordinals.
 *
 * WHY THERE IS AN OBJECT MAP AND NOT A CAST (Importance = 2):
 * A Win32 `HDC`, `HBRUSH`, `HBITMAP` or `HFONT` is 32 bits and a Win16 program
 * has 16 to hold it in. Every one of them also travels back through 16-bit code
 * -- GDI's own `CreateDC` and `GetStockObject` are `native16` wrappers -- so the
 * value has to survive a round trip and still name the right object. Same answer
 * as the windows, the menus and the cursor/icon tokens, and for the same reason:
 * a truncated pointer would name the wrong object and would not fail loudly.
 *
 * THE PRODUCERS ARRIVED IN SESSION 45, AND THE FIRST ONE WAS NOT HERE (Importance = 2):
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
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#ifndef NTVDMEX_WOWGDI_H
#define NTVDMEX_WOWGDI_H

#include "wowgdi_calls.h"   /* the call table: thunk ids and argument offsets */
#include "wowconv.h"        /* the Win16/Win32 semantic deltas, pinned by tests/unit/wow_test.c */

/* The capability index Win32 inherited from Win16 unchanged -- "how many entries
 * in this device's colour table". It is the one index whose Win32 answer a Win16
 * caller cannot read; see the handler.
 */
#define WOWGDI_CAP_NUMCOLORS            24

#define WOWGDI_BITMAP16_SIZE            14
#define WOWGDI_LOGFONT16_SIZE           50
#define WOWGDI_LOGPEN16_SIZE            10
#define WOWGDI_LOGBRUSH16_SIZE          8
#define WOWGDI_TEXTMETRIC16_SIZE        31
#define WOWGDI_ESC_QUERYESCSUPPORT      8   /* The one escape we can answer fully */
#define WOWGDI_CBI_OFF_WIDTH            2
#define WOWGDI_CBI_OFF_HEIGHT           4
#define WOWGDI_CBI_OFF_WBYTES           6
#define WOWGDI_CBI_OFF_PLANES           8
#define WOWGDI_CBI_OFF_BPP              9
#define WOWGDI_CBI_OFF_BITS             10
#define WOWGDI_CBI_BITMAP16_SIZE        14

/* GDI tokens sit below the menu tokens (0x4000) and above the window handles,
 * so a stray handle of any kind is recognisable on sight in a log.
 */
#define WOWGDI_BASE                     0x2000
#define WOWGDI_STEP                     0x0008
#define WOWGDI_MAX                      256

/* [CAUTION]: A BOUND ON A COUNT THE GUEST CHOSE. CreatePolygonRgn's point count is a
 * guest WORD used to size a copy, so it is checked against this before it is
 * believed. 1024 points is far past anything the shelf draws and still a
 * fixed 8 KB of stack.
 */
#define WOWGDI_MAX_POLYPTS              1024

/* THREE KINDS, NOT TWO -- AND THE THIRD IS WHY THIS IS NOT A BOOLEAN (Importance = 2):
 * A handle's kind decides which call is allowed to dispose of it, and getting
 * that wrong is not a loud failure, it is a leak or a corrupted DC cache:
 *
 * OBJ    a brush/pen/bitmap/font        -> DeleteObject
 * DC     from CreateDC/CreateCompatibleDC -> DeleteDC
 * WINDC  BORROWED from GetDC/GetWindowDC  -> ReleaseDC, and ONLY ReleaseDC
 *
 * The third kind arrived with `GetDC`, which is USER's call and not GDI's --
 * so the first thing that ever hands this host a DC is in another id space
 * entirely. `DeleteDC` on a borrowed DC is a documented bug (the DC belongs to
 * the window's cache, not to the caller), and this map is the only place that
 * can still tell the difference, so it records it rather than reconstructing
 * it later from which call happens to arrive.
 */
#define WOWGDI_KIND_OBJ                 0
#define WOWGDI_KIND_DC                  1
#define WOWGDI_KIND_WINDC               2

/* A fourth: the system's own objects, which belong to nobody and must not be
 * destroyed. Win32 tolerates DeleteObject on one silently; a guest doing it is
 * still worth seeing, and it costs one value in this enum to be able to say so.
 */
#define WOWGDI_KIND_STOCK               3

/* s90: EnumFontsA hands a LOGFONT, not an ENUMLOGFONT -- the full name and style
 * past it are not ours to read, so this says not to.
 */
/* The Win16 structures EnumFontFamilies and EnumObjects hand their callbacks
 * (byte-packed, as Win16's GDI declares them).
 */
#define WOWGDI_LF16_HEIGHT              0
#define WOWGDI_LF16_WIDTH               2
#define WOWGDI_LF16_ESCAPEMENT          4
#define WOWGDI_LF16_ORIENTATION         6
#define WOWGDI_LF16_WEIGHT              8
#define WOWGDI_LF16_ITALIC              10
#define WOWGDI_LF16_UNDERLINE           11
#define WOWGDI_LF16_STRIKEOUT           12
#define WOWGDI_LF16_CHARSET             13
#define WOWGDI_LF16_OUTPRECISION        14
#define WOWGDI_LF16_CLIPPRECISION       15
#define WOWGDI_LF16_QUALITY             16
#define WOWGDI_LF16_PITCHANDFAMILY      17
#define WOWGDI_LF16_FACENAME            18
#define WOWGDI_LF16_FACESIZE            32
#define WOWGDI_ELF16_FULLNAME           50
#define WOWGDI_ELF16_FULLNAME_SIZE      64
#define WOWGDI_ELF16_STYLE              114
#define WOWGDI_ELF16_STYLE_SIZE         32
#define WOWGDI_NTM16_HEIGHT             0
#define WOWGDI_NTM16_ASCENT             2
#define WOWGDI_NTM16_DESCENT            4
#define WOWGDI_NTM16_INTERNALLEADING    6
#define WOWGDI_NTM16_EXTERNALLEADING    8
#define WOWGDI_NTM16_AVECHARWIDTH       10
#define WOWGDI_NTM16_MAXCHARWIDTH       12
#define WOWGDI_NTM16_WEIGHT             14
#define WOWGDI_NTM16_ITALIC             16
#define WOWGDI_NTM16_UNDERLINED         17
#define WOWGDI_NTM16_STRUCKOUT          18
#define WOWGDI_NTM16_FIRSTCHAR          19
#define WOWGDI_NTM16_LASTCHAR           20
#define WOWGDI_NTM16_DEFAULTCHAR        21
#define WOWGDI_NTM16_BREAKCHAR          22
#define WOWGDI_NTM16_PITCHANDFAMILY     23
#define WOWGDI_NTM16_CHARSET            24
#define WOWGDI_NTM16_OVERHANG           25
#define WOWGDI_NTM16_DIGITIZEDASPECTX   27
#define WOWGDI_NTM16_DIGITIZEDASPECTY   29
#define WOWGDI_NTM16_FLAGS              31
#define WOWGDI_NTM16_SIZEEM             35
#define WOWGDI_NTM16_CELLHEIGHT         37
#define WOWGDI_NTM16_AVGWIDTH           39
#define WOWGDI_LP16_STYLE               0
#define WOWGDI_LP16_WIDTH_X             2
#define WOWGDI_LP16_WIDTH_Y             4
#define WOWGDI_LP16_COLOR               6
#define WOWGDI_LB16_STYLE               0
#define WOWGDI_LB16_COLOR               2
#define WOWGDI_LB16_HATCH               6
#define WOWGDI_OBJECT_BLOB_CLEAR        16

/* A TEXTMETRIC16 is the first 31 bytes of a NEWTEXTMETRIC16 (WOWGDI_NTM16_*); a BITMAP16
 * is laid out as WOWGDI_CBI_OFF_*.
 */
#define WOWGDI_POINT16_SIZE             4
#define WOWGDI_POINT16_Y                2
#define WOWGDI_RECT16_TOP               2
#define WOWGDI_RECT16_RIGHT             4
#define WOWGDI_RECT16_BOTTOM            6
#define WOWGDI_PALETTEENTRY16_SIZE      4   /* Red, green, blue, flags */
#define WOWGDI_PE16_GREEN               1
#define WOWGDI_PE16_BLUE                2
#define WOWGDI_PE16_FLAGS               3
#define WOWGDI_PALETTE_MAX              256
#define WOWGDI_MAX_POINTS               64
#define WOWGDI_STOCK_OBJECT_LAST        16
#define WOWGDI_DEVICE_NAME_MAX          64
#define WOWGDI_DISPLAY_NAME_LENGTH      7   /* "DISPLAY" */
#define WOWGDI_LOWER_TO_UPPER           32
#define WOWGDI_ABC_MAX                  1024
#define WOWGDI_FAMILY_MAX               64
#define WOWGDI_WORD_BITS                16  /* A Win16 bitmap's scan line is word-aligned */
#define WOWGDI_CLR_INVALID              0xFFFFFFFFu
#define WOWGDI_CHAR_WIDTHS_MAX          256
#define WOWGDI_TEXT_MAX                 512
#define WOWGDI_MF_RECORD_MAX_WORDS      0x8000
#define WOWGDI_SEGMENT_SIZE             0x10000
#define WOWGDI_LOGPALETTE_ENTRIES       2   /* palNumEntries, after palVersion */
#define WOWGDI_HEX_SIZE_DIGITS          6

/* EnumMetaFile / PlayMetaFileRecord: ONE ENUMERATION'S STATE. (#295) (Importance = 1):
 * EnumMetaFile is the per-item chain of wowenum.h with two things no other
 * enumeration has: the items are VARIABLE-SIZED (a METARECORD), and the guest
 * gets a HANDLETABLE that must PERSIST across the calls -- record 1 creates a
 * pen into slot 0, record 2 selects slot 0. Real GDI keeps that table in one
 * block and passes the same pointer every time. Here every callback gets a
 * fresh stack blob (the only guest memory this host can hand out for one call,
 * wowcall.h), so persistence is done by hand:
 * blob   = [ the record, maybe truncated ][ WORD token[nObj] ]
 * lpmr   -> the record,   lpht -> the table (a second pointer into the blob)
 * after the callback returns, the table is READ BACK out of the guest stack
 * into g_WowGdiMetafile.tok[] -- before anything else runs on that stack -- and the next
 * record's blob starts from it.
 * So whatever wrote the table -- our PlayMetaFileRecord, or the guest itself --
 * is what the next record sees, which is what one persistent block gives.
 *
 * [CAUTION]: ONE SOURCE OF TRUTH: the table is 16-bit TOKENS only. A Win32 HGDIOBJ table
 * is built from them for each PlayMetaFileRecord and its changes written back
 * as tokens, so there is no second table to drift out of step with the first.
 *
 * [CAUTION]: UNMEASURED, and said rather than assumed: whether stock hands the SAME lpht
 * pointer every time (ours usually does -- the parked caller's SP does not
 * move -- but a guest must not depend on it), and whether stock deletes the
 * objects left in the table at the end. We do, as Wine's EnumMetaFile16 does
 * (it first re-selects the DC's original pen/brush/font so nothing deleted is
 * still selected); a guest that keeps a table handle past EnumMetaFile is
 * using a deleted object on Wine too.
 */
#define WOWMF_MAXOBJ 256        /* mtNoObjects above this: refused, loudly. A WORD
                                   field, but real metafiles hold a handful, and
                                   the token map itself has only WOWGDI_MAX slots. */
/* [CAUTION]: IS THE FINAL META_EOF RECORD (rdSize 3, rdFunction 0) HANDED TO THE CALLBACK?
 * UNKNOWN for Win16 and NOT GUESSED: this must be set from the stock run of
 * tests/probes/win16/w_mfenum (case mfe.count, and the last mfe.rec.N). 1 = passed,
 * which is what Win32's EnumMetaFile is documented to do ("each record ... until
 * the last record"); Wine's EnumMetaFile16 BREAKS at META_EOF without calling,
 * i.e. 0. Either way the walk ends at EOF -- nothing after it is a record.
 *
 * [INFO]: s92, MEASURED: stock does NOT pass it -- w_mfenum under stock ntvdm on the rig
 * makes 5 calls for a 5-record metafile, the last one 041B (Rectangle); ours made 6
 * with 0000 last. Wine had it right.
 */
#define WOWMF_PASS_EOF          0

/* One record as the guest handed it to PlayMetaFileRecord. A 16:16 pointer reaches
 * at most 64 KB past its offset, so this is the largest a record can be without a
 * huge pointer -- and a larger one is refused, not wrapped.
 */
#define WOWGDI_MF_RECORD_MAX    0x10000

/* WowGdiMetafileNext's two refusals, read by the walk in wowenum.h. */
#define WOWGDI_MF_MALFORMED     0xFFFF
#define WOWGDI_MF_NO_ROOM       0xFFFE

typedef struct _WOWGDI_OBJECT
{
    WORD Handle16;
    HGDIOBJ Object;
    INT Kind;
} WOWGDI_OBJECT;
typedef struct _WOWGDI_METAFILE
{
    INT     IsActive;
    PBYTE Bits;               /* GetMetaFileBitsEx snapshot, HeapAlloc'd */
    /* bytes; the walk's bound; the next record */
    DWORD Length;
    DWORD End;
    DWORD Offset;
    UINT ObjectCount;              /* mtNoObjects = the callback's nObj */
    WORD    Tokens[WOWMF_MAXOBJ];  /* the persistent HANDLETABLE, as tokens */
    WORD    Dc16;
    HDC     Dc;                 /* NULL when the guest's hdc is not one of ours */
    /* re-selected before the table is deleted */
    HGDIOBJ OriginalPen;
    HGDIOBJ OriginalBrush;
    HGDIOBJ OriginalFont;
    /* where the last callback's copies are (host lin) */
    DWORD TableLinear;
    DWORD RecordLinear;
    /* the record in flight, in `Bits` */
    DWORD RecordOffset;
    DWORD RecordBytes;
    INT     IsTruncated;          /* ...and whether its stack copy is cut short */
    DWORD   Records;
} WOWGDI_METAFILE, *PWOWGDI_METAFILE;

extern WOWGDI_METAFILE g_WowGdiMetafile;
extern BYTE g_WowGdiMetafileBlob[WOWCALL_MAX_BLOB];
extern WOWGDI_OBJECT g_WowGdiObjects[WOWGDI_MAX];
extern INT g_WowGdiObjectCount;

/* Defined in wowgdi.c (#335). */
WORD WowGdiH16(HGDIOBJ object, INT kind);
WORD WowGdiPeek(const volatile BYTE *bytes, INT offset);
INT WowGdiMetafileReadBack(VOID);
VOID WowGdiMetafileEnd(VOID);
INT WowGdiMetafileNext(INT room, PINT blobLength, PINT tableOffset, UINT *function);
HGDIOBJ WowGdiH32(WORD handle16, PINT kind);
VOID WowGdiForget(WORD handle16);
INT WowGdiCall(PWOW32_FRAME frame, PSTR note, INT noteCapacity);

#endif /* NTVDMEX_WOWGDI_H */
