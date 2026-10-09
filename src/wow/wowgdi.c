/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * GDI.EXE's OWN ID SPACE.  GH #128, session 44.
 *
 * The code of wowgdi.h (#335): its functions and state, in their original order;
 * its own translation unit, declared in wowgdi.h.
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#include "host_state.h"
#include "log.h"
#include "ne.h"
#include "wow32.h"
#include "wowanchors.h"
#include "wowsched.h"
#include "wowcall.h"
#include "wowmsg.h"
#include "wowres.h"
#include "wowwin.h"
#include "wowgdi.h"
#include "wowuser.h"
#include "wowdlg.h"
#include "wowenum.h"

/* A little-endian WORD out of guest memory. wowuser.h has the same helper for
 * the USER side; this file is included independently, so it has its own.
 */
WORD WowGdiPeek(const volatile BYTE *bytes, INT offset)
{
    return (WORD)(bytes[offset] | (bytes[offset + 1] << BYTE_SHIFT));
}

WOWGDI_OBJECT g_WowGdiObjects[WOWGDI_MAX];
INT          g_WowGdiObjectCount = 0;

/* One token per object. `kind` is kept because DeleteDC, DeleteObject and
 * ReleaseDC are three different calls with three different rules, and handing an
 * object to the wrong one is a defect this map can catch instead of passing on
 * to Win32.
 */
WORD WowGdiH16(HGDIOBJ object, INT kind)
{
    INT index;

    if (!object)
        return 0;
    for (index = 0; index < g_WowGdiObjectCount; ++index)
        if (g_WowGdiObjects[index].Object == object)
        {
            if (g_WowGdiObjects[index].Kind == kind)
                return g_WowGdiObjects[index].Handle16;
            /* -- SAME ADDRESS, DIFFERENT KIND: THE ENTRY IS STALE, AND
             * THIS WAS SOLITAIRE'S BLACK CARDS. Win32 RECYCLES HGDIOBJ VALUES.
             * A memory DC is deleted, a bitmap is created, and the OS hands
             * back the SAME pointer -- so this loop found the dead DC's entry
             * and gave the new BITMAP the DC's token. The log shows one token
             * living both lives within a few calls:
             *
             *     SetTextColor(0x20e0, ...)            ; used as a DC
             *     GetTextExtent(0x20e0, ...)           ; used as a DC
             *     PatBlt(0x20e0, ...)                  ; used as a DC
             *     CreateCompatibleDC -> 0x20e8
             *     SelectObject(dc 0x20e8, obj 0x20e0)  ; now used as a BITMAP
             *
             * Selecting it put no real bitmap in the memory DC, so the DC kept
             * its default 1x1 monochrome one and every 71x96 card blitted out
             * of it came through BLACK. The blit SUCCEEDED and said so -- there
             * was nothing in the log that looked like a failure.
             *
             * [CAUTION]: The kind is not decoration: it is the only thing that can tell a
             * recycled handle from the object that used to live there. Retire
             * the dead entry and mint a fresh token below.
             */
            g_WowGdiObjects[index].Object = NULL;
            g_WowGdiObjects[index].Handle16 = 0;
        }
    for (index = 0; index < g_WowGdiObjectCount; ++index)                    /* reuse a freed slot */
        if (!g_WowGdiObjects[index].Handle16 && !g_WowGdiObjects[index].Object)
            break;
    if (index == g_WowGdiObjectCount)
    {
        if (g_WowGdiObjectCount >= WOWGDI_MAX)
            return 0;
        index = g_WowGdiObjectCount++;
    }
    g_WowGdiObjects[index].Object = object;
    g_WowGdiObjects[index].Kind = kind;
    g_WowGdiObjects[index].Handle16 = (WORD)(WOWGDI_BASE + index * WOWGDI_STEP);
    return g_WowGdiObjects[index].Handle16;
}

HGDIOBJ WowGdiH32(WORD handle16, PINT kind)
{
    INT index;

    if (kind)
        *kind = -1;
    if (!handle16)
        return NULL;
    for (index = 0; index < g_WowGdiObjectCount; ++index)
        if (g_WowGdiObjects[index].Handle16 == handle16)
        {
            if (kind)
                *kind = g_WowGdiObjects[index].Kind;
            return g_WowGdiObjects[index].Object;
        }
    return NULL;
}

/* Forget a token whose object has been destroyed. [CAUTION] The slot is cleared rather
 * than compacted: a token is its INDEX, so compacting would silently rename
 * every object above it.
 *
 * [CAUTION]: A CLEARED SLOT IS REUSED, and that is a deliberate trade. Paint takes a DC and
 * releases it on every single paint, so 256 slots without reuse are gone in
 * seconds and the map would start refusing handles -- a certain failure traded
 * for a possible one. The possible one is real and is named here rather than
 * left to be discovered: a guest that keeps a token past its ReleaseDC will,
 * after a reuse, name a DIFFERENT object instead of failing. That is exactly
 * what Win32 does with its own handles, so a guest that does it is already
 * broken on real Windows.
 */
VOID WowGdiForget(WORD handle16)
{
    INT index;

    for (index = 0; index < g_WowGdiObjectCount; ++index)
        if (g_WowGdiObjects[index].Handle16 == handle16)
        {
            g_WowGdiObjects[index].Object = NULL;
            g_WowGdiObjects[index].Handle16 = 0;
            return;
        }
}

/* [CAUTION]: CALLED ONLY WHEN THE STUB IS GDI'S. The caller checks, as it does for every
 * other table -- `0x45` is `SetWindowPos`-adjacent in USER's numbering and
 * `DeleteObject` here.
 */
/* s89: one Win32 font -> the Win16 ENUMLOGFONT16 + NEWTEXTMETRIC16 pair, byte-packed.
 * LOGFONT16 is LOGFONT with 16-bit ints (18 bytes + a 32-byte face); TEXTMETRIC16 is
 * eight ints, nine bytes, three ints (31); NEWTEXTMETRIC16 adds ntmFlags (DWORD),
 * ntmSizeEM, ntmCellHeight, ntmAvgWidth (41).
 */
static VOID WowGdiPut16(PBYTE bytes, INT offset, LONG value)
{
    bytes[offset] = (BYTE)value;
    bytes[offset + 1] = (BYTE)(value >> BYTE_SHIFT);
}

static INT g_WowGdiFontIsPlain;
static INT CALLBACK WowGdiFontCollect(
    const LOGFONTA *logFont,
    const TEXTMETRICA *textMetric,
    DWORD type,
    LPARAM unused)
{
    const ENUMLOGFONTA *enumLogFont = (const ENUMLOGFONTA *)logFont;
    const NEWTEXTMETRICA *newTextMetric = (const NEWTEXTMETRICA *)textMetric;
    PWOWENUM_FONT entry;
    PBYTE blob, metrics;
    INT index;

    (VOID)unused;
    if (g_WowEnumFontCount >= WOWENUM_MAXFONT)
        return 0;
    entry = &g_WowEnumFonts[g_WowEnumFontCount++];
    for (index = 0; index < (INT)sizeof entry->Blob; ++index)
        entry->Blob[index] = 0;
    blob = entry->Blob;
    WowGdiPut16(blob, WOWGDI_LF16_HEIGHT, logFont->lfHeight);
    WowGdiPut16(blob, WOWGDI_LF16_WIDTH, logFont->lfWidth);
    WowGdiPut16(blob, WOWGDI_LF16_ESCAPEMENT, logFont->lfEscapement);
    WowGdiPut16(blob, WOWGDI_LF16_ORIENTATION, logFont->lfOrientation);
    WowGdiPut16(blob, WOWGDI_LF16_WEIGHT, logFont->lfWeight);
    blob[WOWGDI_LF16_ITALIC] = logFont->lfItalic;
    blob[WOWGDI_LF16_UNDERLINE] = logFont->lfUnderline;
    blob[WOWGDI_LF16_STRIKEOUT] = logFont->lfStrikeOut;
    blob[WOWGDI_LF16_CHARSET] = logFont->lfCharSet;
    blob[WOWGDI_LF16_OUTPRECISION] = logFont->lfOutPrecision;
    blob[WOWGDI_LF16_CLIPPRECISION] = logFont->lfClipPrecision;
    blob[WOWGDI_LF16_QUALITY] = logFont->lfQuality;
    blob[WOWGDI_LF16_PITCHANDFAMILY] = logFont->lfPitchAndFamily;
    for (index = 0; index < WOWGDI_LF16_FACESIZE - 1 && logFont->lfFaceName[index]; ++index)
        blob[WOWGDI_LF16_FACENAME + index] = (BYTE)logFont->lfFaceName[index];
    if (!g_WowGdiFontIsPlain)
    {
        for (index = 0; index < WOWGDI_ELF16_FULLNAME_SIZE - 1 && enumLogFont->elfFullName[index]; ++index)
            blob[WOWGDI_ELF16_FULLNAME + index] = enumLogFont->elfFullName[index];
        for (index = 0; index < WOWGDI_ELF16_STYLE_SIZE - 1 && enumLogFont->elfStyle[index]; ++index)
            blob[WOWGDI_ELF16_STYLE + index] = enumLogFont->elfStyle[index];
    }
    metrics = blob + WOWENUM_ELF16;
    WowGdiPut16(metrics, WOWGDI_NTM16_HEIGHT, textMetric->tmHeight);
    WowGdiPut16(metrics, WOWGDI_NTM16_ASCENT, textMetric->tmAscent);
    WowGdiPut16(metrics, WOWGDI_NTM16_DESCENT, textMetric->tmDescent);
    WowGdiPut16(metrics, WOWGDI_NTM16_INTERNALLEADING, textMetric->tmInternalLeading);
    WowGdiPut16(metrics, WOWGDI_NTM16_EXTERNALLEADING, textMetric->tmExternalLeading);
    WowGdiPut16(metrics, WOWGDI_NTM16_AVECHARWIDTH, textMetric->tmAveCharWidth);
    WowGdiPut16(metrics, WOWGDI_NTM16_MAXCHARWIDTH, textMetric->tmMaxCharWidth);
    WowGdiPut16(metrics, WOWGDI_NTM16_WEIGHT, textMetric->tmWeight);
    metrics[WOWGDI_NTM16_ITALIC] = textMetric->tmItalic;
    metrics[WOWGDI_NTM16_UNDERLINED] = textMetric->tmUnderlined;
    metrics[WOWGDI_NTM16_STRUCKOUT] = textMetric->tmStruckOut;
    metrics[WOWGDI_NTM16_FIRSTCHAR] = (BYTE)textMetric->tmFirstChar;
    metrics[WOWGDI_NTM16_LASTCHAR] = (BYTE)textMetric->tmLastChar;
    metrics[WOWGDI_NTM16_DEFAULTCHAR] = (BYTE)textMetric->tmDefaultChar;
    metrics[WOWGDI_NTM16_BREAKCHAR] = (BYTE)textMetric->tmBreakChar;
    metrics[WOWGDI_NTM16_PITCHANDFAMILY] = textMetric->tmPitchAndFamily;
    metrics[WOWGDI_NTM16_CHARSET] = textMetric->tmCharSet;
    WowGdiPut16(metrics, WOWGDI_NTM16_OVERHANG, textMetric->tmOverhang);
    WowGdiPut16(metrics, WOWGDI_NTM16_DIGITIZEDASPECTX, textMetric->tmDigitizedAspectX);
    WowGdiPut16(metrics, WOWGDI_NTM16_DIGITIZEDASPECTY, textMetric->tmDigitizedAspectY);
    if ((type & TRUETYPE_FONTTYPE) && !g_WowGdiFontIsPlain)   /* the NEW part: TrueType's */
    {
        metrics[WOWGDI_NTM16_FLAGS] = (BYTE)newTextMetric->ntmFlags;
        metrics[WOWGDI_NTM16_FLAGS + 1] = (BYTE)(newTextMetric->ntmFlags >> BYTE_SHIFT);
        metrics[WOWGDI_NTM16_FLAGS + 2] = (BYTE)(newTextMetric->ntmFlags >> WORD_SHIFT);
        metrics[WOWGDI_NTM16_FLAGS + 3] = (BYTE)(newTextMetric->ntmFlags >> TOP_BYTE_SHIFT);
        WowGdiPut16(metrics, WOWGDI_NTM16_SIZEEM, (LONG)newTextMetric->ntmSizeEM);
        WowGdiPut16(metrics, WOWGDI_NTM16_CELLHEIGHT, (LONG)newTextMetric->ntmCellHeight);
        WowGdiPut16(metrics, WOWGDI_NTM16_AVGWIDTH, (LONG)newTextMetric->ntmAvgWidth);
    }
    entry->FontType = (WORD)type;
    return 1;
}

/* s90: one Win32 pen/brush -> LOGPEN16 (10) / LOGBRUSH16 (8), into g_WowEnumFonts[]. */
static INT CALLBACK WowGdiObjectCollect(LPVOID logObject, LPARAM type)
{
    PBYTE blob;
    INT index;

    if (g_WowEnumFontCount >= WOWENUM_MAXFONT)
        return 0;
    blob = g_WowEnumFonts[g_WowEnumFontCount].Blob;
    for (index = 0; index < WOWGDI_OBJECT_BLOB_CLEAR; ++index)
        blob[index] = 0;
    if (type == OBJ_PEN)
    {
        const LOGPEN *logPen = (const LOGPEN *)logObject;
        WowGdiPut16(blob, WOWGDI_LP16_STYLE, (LONG)logPen->lopnStyle);
        WowGdiPut16(blob, WOWGDI_LP16_WIDTH_X, logPen->lopnWidth.x);
        WowGdiPut16(blob, WOWGDI_LP16_WIDTH_Y, logPen->lopnWidth.y);
        WowGdiPut16(blob, WOWGDI_LP16_COLOR, (LONG)(logPen->lopnColor & WORD_MASK));
        WowGdiPut16(blob, WOWGDI_LP16_COLOR + WOW_WORD_BYTES, (LONG)(logPen->lopnColor >> WORD_SHIFT));
    }
    else
    {
        const LOGBRUSH *logBrush = (const LOGBRUSH *)logObject;
        WowGdiPut16(blob, WOWGDI_LB16_STYLE, (LONG)logBrush->lbStyle);
        WowGdiPut16(blob, WOWGDI_LB16_COLOR, (LONG)(logBrush->lbColor & WORD_MASK));
        WowGdiPut16(blob, WOWGDI_LB16_COLOR + WOW_WORD_BYTES, (LONG)(logBrush->lbColor >> WORD_SHIFT));
        WowGdiPut16(blob, WOWGDI_LB16_HATCH, (LONG)logBrush->lbHatch);
    }
    g_WowEnumFonts[g_WowEnumFontCount].FontType = (WORD)type;
    ++g_WowEnumFontCount;
    return 1;
}

WOWGDI_METAFILE g_WowGdiMetafile;
BYTE        g_WowGdiMetafileBlob[WOWCALL_MAX_BLOB];

static BYTE        g_WowGdiMetafileRecord[WOWGDI_MF_RECORD_MAX];
static HGDIOBJ     g_WowGdiMetafileHandles[WOWMF_MAXOBJ];

/* After a callback: what the guest's table says now. Only 0 and tokens that name
 * an object (not a DC) are believed; anything else keeps the previous entry and is
 * counted, so the caller can say so.
 */
INT WowGdiMetafileReadBack(VOID)
{
    UINT index;
    INT badCount = 0;

    if (!g_WowGdiMetafile.IsActive || !g_WowGdiMetafile.TableLinear)
        return 0;
    for (index = 0; index < g_WowGdiMetafile.ObjectCount; ++index)
    {
        WORD token = WowGdiPeek((const volatile BYTE *)(ULONG_PTR)g_WowGdiMetafile.TableLinear, (INT)(index * WOW_WORD_BYTES));
        INT  kind = -1;
        if (token == g_WowGdiMetafile.Tokens[index])
            continue;
        if (!token || (WowGdiH32(token, &kind) && kind != WOWGDI_KIND_DC && kind != WOWGDI_KIND_WINDC))
            g_WowGdiMetafile.Tokens[index] = token;
        else
            ++badCount;
    }
    g_WowGdiMetafile.TableLinear = 0;
    return badCount;
}

/* The end of an enumeration, however it ended (complete, stopped, refused). */
VOID WowGdiMetafileEnd(VOID)
{
    UINT index;

    if (!g_WowGdiMetafile.IsActive)
        return;
    if (g_WowGdiMetafile.Dc)
    {
        if (g_WowGdiMetafile.OriginalPen)
            SelectObject(g_WowGdiMetafile.Dc, g_WowGdiMetafile.OriginalPen);
        if (g_WowGdiMetafile.OriginalBrush)
            SelectObject(g_WowGdiMetafile.Dc, g_WowGdiMetafile.OriginalBrush);
        if (g_WowGdiMetafile.OriginalFont)
            SelectObject(g_WowGdiMetafile.Dc, g_WowGdiMetafile.OriginalFont);
    }
    for (index = 0; index < g_WowGdiMetafile.ObjectCount; ++index)
    {
        INT kind = -1;
        HGDIOBJ object = g_WowGdiMetafile.Tokens[index] ? WowGdiH32(g_WowGdiMetafile.Tokens[index], &kind) : NULL;
        if (object && kind == WOWGDI_KIND_OBJ)
        {
            DeleteObject(object);
            WowGdiForget(g_WowGdiMetafile.Tokens[index]);
        }
        g_WowGdiMetafile.Tokens[index] = 0;
    }
    if (g_WowGdiMetafile.Bits)
        HeapFree(GetProcessHeap(), 0, g_WowGdiMetafile.Bits);
    g_WowGdiMetafile.Bits = NULL;
    g_WowGdiMetafile.IsActive = 0;
    g_WowGdiMetafile.TableLinear = g_WowGdiMetafile.RecordLinear = 0;
}

/* The next record's blob, at most `room` bytes. Returns 0 at the end of the walk
 * (or at a malformed record, which ends it the same way -- logged by the caller
 * from *func == 0xFFFF). *tbloff = where the table starts in the blob.
 */
INT WowGdiMetafileNext(INT room, PINT blobLength, PINT tableOffset, UINT *function)
{
    unsigned long recordBytes = 0;
    UINT recordFunction = 0;
    INT tableBytes = (INT)(g_WowGdiMetafile.ObjectCount ? g_WowGdiMetafile.ObjectCount : 1) * WOW_WORD_BYTES;    /* lpht always points at
                                                           something, even nObj 0 */
    INT copyBytes, index;

    *function = 0;
    if (!g_WowGdiMetafile.IsActive || g_WowGdiMetafile.Offset >= g_WowGdiMetafile.End)
        return 0;
    if (!WowConvMetafileRecord(g_WowGdiMetafile.Bits, g_WowGdiMetafile.End, g_WowGdiMetafile.Offset, &recordBytes, &recordFunction))
    {
        *function = WOWGDI_MF_MALFORMED;
        return 0;
    }
    if (recordFunction == 0 && !WOWMF_PASS_EOF)
        return 0;
    if (room < tableBytes + WOWCONV_MF_RECHDR)
    {
        *function = WOWGDI_MF_NO_ROOM;
        return 0;
    }
    copyBytes = (INT)recordBytes;
    g_WowGdiMetafile.IsTruncated = 0;
    if (copyBytes > room - tableBytes)
    {
        copyBytes = (room - tableBytes) & ~1;
        g_WowGdiMetafile.IsTruncated = 1;
    }
    for (index = 0; index < copyBytes; ++index)
        g_WowGdiMetafileBlob[index] = g_WowGdiMetafile.Bits[g_WowGdiMetafile.Offset + (DWORD)index];
    for (index = 0; index < (INT)g_WowGdiMetafile.ObjectCount; ++index)
        WowGdiPut16(g_WowGdiMetafileBlob, copyBytes + index * WOW_WORD_BYTES, g_WowGdiMetafile.Tokens[index]);
    if (!g_WowGdiMetafile.ObjectCount)
        WowGdiPut16(g_WowGdiMetafileBlob, copyBytes, 0);
    g_WowGdiMetafile.RecordOffset = g_WowGdiMetafile.Offset;
    g_WowGdiMetafile.RecordBytes = (DWORD)recordBytes;
    g_WowGdiMetafile.Offset = (recordFunction == 0) ? g_WowGdiMetafile.End : g_WowGdiMetafile.Offset + (DWORD)recordBytes;    /* EOF is last */
    ++g_WowGdiMetafile.Records;
    *blobLength = copyBytes + tableBytes;
    *tableOffset = copyBytes;
    *function = recordFunction;
    return 1;
}

INT WowGdiCall(PWOW32_FRAME frame, PSTR note, INT noteCapacity)
{
    if (noteCapacity)
        note[0] = 0;
    switch (frame->Id)
    {

    /* 0x50 GetDeviceCaps(hDC, nIndex) (Importance = 1):
     * Straight through to the OS on the same claim the WS_* bits and the SM_*
     * indices travel on: Win32 inherited the DC capability indices from Win16
     * unchanged. [CAUTION] As with GetSystemMetrics, a wrong answer here does not
     * fail, it lays something out slightly wrong -- so the index AND the answer
     * are logged on every call, and a guest whose arithmetic looks wrong can be
     * checked against what it was actually told.
     */
    case WOWGDI_GETDEVICECAPS:
    {
        WORD dc16 = Wow32ArgWord(frame, WOWGDI_GDC_ARG_HDC);
        WORD itemIndex = Wow32ArgWord(frame, WOWGDI_GDC_ARG_INDEX);
        INT  kind = -1;
        HGDIOBJ object = WowGdiH32(dc16, &kind);
        INT noteLength = 0, value;
        INT isDc = (kind == WOWGDI_KIND_DC || kind == WOWGDI_KIND_WINDC);
        WowNotePut(note, noteCapacity, &noteLength, "GetDeviceCaps(0x");
        WowNoteHex(note, noteCapacity, &noteLength, dc16, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, ", 0x");
        WowNoteHex(note, noteCapacity, &noteLength, itemIndex, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, ")");
        if (!object || !isDc)
        {
            WowNotePut(note, noteCapacity, &noteLength, " -- ★ NOT ONE OF OUR GDI DC TOKENS (the"
                                       " calls that CREATE a DC are not serviced"
                                       " yet); answered 0");
            Wow32SetReturn(frame, 0);
            return 1;
        }
        value = GetDeviceCaps((HDC)object, (INT)itemIndex);

        /* -- NUMCOLORS: -1 IS A Win32 SENTINEL AND NO Win16 PROGRAM HAS
         * EVER SEEN ONE. (session 51 -- MINESWEEPER RENDERED IN BLACK AND
         * WHITE, and stock ntvdm runs the same binary in colour.)
         * Win32 answers NUMCOLORS with -1 for any device deeper than 8bpp.
         * That reaches the guest as 0xffff, and WINMINE.EXE asks NUMCOLORS once
         * at start-up and decides colour vs MONOCHROME on "more than 2",
         * compared SIGNED -- so 0xffff, i.e. -1, reads as monochrome. Answered
         * -1, every DIB it builds and hands to
         * SetDIBitsToDevice really is 1bpp, and this host draws it faithfully;
         * answered a colour-table size, the same run is in colour.
         * Nothing downstream was wrong. The wrong answer was here.
         *
         * [INFO]: SO IT IS ANSWERED IN Win16's OWN TERMS: the size of a colour table,
         * which is what the index MEANS. <= 8bpp gives the exact count; deeper
         * than that has no table at all, and 256 is both the largest a Windows
         * 3.1 driver ever reported and the largest a program of this era was
         * built to read. Every Win16 caller tests it for ">2" or "==2".
         *
         * [CAUTION]: AND THE SESSION-45 REFUTATION STILL STANDS -- it was about a
         * DIFFERENT GUEST and it is not contradicted. MS Paint asks NUMCOLORS
         * four times and never asks BITSPIXEL or PLANES, so -1 looked certain
         * to be why every bitmap it makes is `planes=1 bpp=1`; substituting
         * 256 changed NOTHING, because Paint's 1bpp bitmaps are MASKS (the
         * Win16 idiom is a monochrome pattern plus SetTextColor/SetBkColor at
         * blit time, and that run made 34 of each). Both are true: 256 is not
         * the lever for Paint's masks, AND -1 is the wrong answer to give a
         * 16-bit caller. What was refuted was a hypothesis about Paint, not
         * the value. => Re-run Paint after touching this.
         */
        if (itemIndex == WOWGDI_CAP_NUMCOLORS && value < 0)
        {
            INT bitsPerPixel = GetDeviceCaps((HDC)object, BITSPIXEL)
                    * GetDeviceCaps((HDC)object, PLANES);
            value = WowConvNumColors(bitsPerPixel);       /* tested in wow_test.c part 3 */
            WowNotePut(note, noteCapacity, &noteLength, " [NUMCOLORS -1 -> ");
            WowNoteHex(note, noteCapacity, &noteLength, (DWORD)value, WOW_HEX_WORD_DIGITS);
            WowNotePut(note, noteCapacity, &noteLength, "; a Win16 caller reads -1 as MONOCHROME]");
        }
        /* [CAUTION]: REFUTED, session 45, and recorded so it is not re-tried: forcing
         * HORZRES/HORZSIZE to stock's 2.0 (from our 2.625) changed MS Paint's
         * toolbox by NOTHING -- pbTool stayed 163x731 to the pixel. Paint
         * reads all four of HORZRES/HORZSIZE/VERTRES/VERTSIZE, and the ratio
         * matched the over-scale exactly, and it is still not the lever.
         */
        WowNotePut(note, noteCapacity, &noteLength, " = 0x");
        WowNoteHex(note, noteCapacity, &noteLength, (DWORD)value, WOW_HEX_WORD_DIGITS);
        Wow32SetReturn(frame, (DWORD)(WORD)value);
        return 1;
    }

    /* 0x22 BitBlt -- THE TOOL ICONS (Importance = 5):
     *
     * [CAUTION]: BOTH DCs ARE TOKENS and both must resolve, which is the whole reason a
     * blit could not work before the producers went in: the source is almost
     * always a memory DC with the toolbox bitmap selected into it.
     */
    case WOWGDI_BITBLT:
    {
        WORD destDc16 = Wow32ArgWord(frame, WOWGDI_BB_ARG_DSTDC);
        WORD sourceDc16 = Wow32ArgWord(frame, WOWGDI_BB_ARG_SRCDC);
        INT  positionX  = (INT)(SHORT)Wow32ArgWord(frame, WOWGDI_BB_ARG_X);
        INT  positionY  = (INT)(SHORT)Wow32ArgWord(frame, WOWGDI_BB_ARG_Y);
        INT  width = (INT)(SHORT)Wow32ArgWord(frame, WOWGDI_BB_ARG_WIDTH);
        INT  height = (INT)(SHORT)Wow32ArgWord(frame, WOWGDI_BB_ARG_HEIGHT);
        INT  sourceX = (INT)(SHORT)Wow32ArgWord(frame, WOWGDI_BB_ARG_SRCX);
        INT  sourceY = (INT)(SHORT)Wow32ArgWord(frame, WOWGDI_BB_ARG_SRCY);
        DWORD rasterOp = Wow32ArgDword(frame, WOWGDI_BB_ARG_ROP);
        INT  dcKind = -1, sourceKind = -1;
        HGDIOBJ destination = WowGdiH32(destDc16, &dcKind);
        HGDIOBJ source = WowGdiH32(sourceDc16, &sourceKind);
        INT  noteLength = 0, isOk;
        WowNotePut(note, noteCapacity, &noteLength, "BitBlt dst 0x");
        WowNoteHex(note, noteCapacity, &noteLength, destDc16, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, " (");
        WowNoteHex(note, noteCapacity, &noteLength, (DWORD)positionX, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, ",");
        WowNoteHex(note, noteCapacity, &noteLength, (DWORD)positionY, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, ") ");
        WowNoteHex(note, noteCapacity, &noteLength, (DWORD)width, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, "x");
        WowNoteHex(note, noteCapacity, &noteLength, (DWORD)height, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, " <- src 0x");
        WowNoteHex(note, noteCapacity, &noteLength, sourceDc16, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, " (");
        WowNoteHex(note, noteCapacity, &noteLength, (DWORD)sourceX, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, ",");
        WowNoteHex(note, noteCapacity, &noteLength, (DWORD)sourceY, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, ") rop=0x");
        WowNoteHex(note, noteCapacity, &noteLength, rasterOp, WOW_HEX_DWORD_DIGITS);
        if (!destination || (dcKind != WOWGDI_KIND_DC && dcKind != WOWGDI_KIND_WINDC))
        {
            WowNotePut(note, noteCapacity, &noteLength, " -- ★ THE DESTINATION IS NOT ONE OF OUR"
                                       " DC TOKENS; answered 0");
            Wow32SetReturn(frame, 0);
            return 1;
        }
        /* [CAUTION]: A NULL SOURCE IS LEGAL for the rops that do not read one (BLACKNESS,
         * WHITENESS, PATCOPY...), so a zero handle is passed through as NULL
         * rather than refused; a NON-zero handle that we cannot name is a
         * different thing and is refused.
         */
        if (sourceDc16 && (!source || (sourceKind != WOWGDI_KIND_DC && sourceKind != WOWGDI_KIND_WINDC)))
        {
            WowNotePut(note, noteCapacity, &noteLength, " -- ★ THE SOURCE IS NOT ONE OF OUR DC"
                                       " TOKENS; answered 0");
            Wow32SetReturn(frame, 0);
            return 1;
        }
        isOk = BitBlt((HDC)destination, positionX, positionY, width, height, sourceDc16 ? (HDC)source : NULL, sourceX, sourceY, rasterOp)
             ? 1 : 0;
        WowNotePut(note, noteCapacity, &noteLength, isOk ? " -> blitted" : " -- ★ the OS refused it");
        Wow32SetReturn(frame, (DWORD)isOk);
        return 1;
    }

    /* 0x23 StretchBlt -- see the note above (Importance = 5):
     *
     * [CAUTION]: THE STRETCH MODE IS THE DC'S, and Paint sets it (SetStretchBltMode,
     * COLORONCOLOR) before getting here -- which is why that call had to go in
     * with this one rather than after it.
     */
    case WOWGDI_STRETCHBLT:
    {
        WORD destDc16 = Wow32ArgWord(frame, WOWGDI_SB_ARG_DSTDC);
        WORD sourceDc16 = Wow32ArgWord(frame, WOWGDI_SB_ARG_SRCDC);
        INT  positionX  = (INT)(SHORT)Wow32ArgWord(frame, WOWGDI_SB_ARG_DSTX);
        INT  positionY  = (INT)(SHORT)Wow32ArgWord(frame, WOWGDI_SB_ARG_DSTY);
        INT  width = (INT)(SHORT)Wow32ArgWord(frame, WOWGDI_SB_ARG_DSTW);
        INT  height = (INT)(SHORT)Wow32ArgWord(frame, WOWGDI_SB_ARG_DSTH);
        INT  sourceX = (INT)(SHORT)Wow32ArgWord(frame, WOWGDI_SB_ARG_SRCX);
        INT  sourceY = (INT)(SHORT)Wow32ArgWord(frame, WOWGDI_SB_ARG_SRCY);
        INT  sourceWidth = (INT)(SHORT)Wow32ArgWord(frame, WOWGDI_SB_ARG_SRCW);
        INT  sourceHeight = (INT)(SHORT)Wow32ArgWord(frame, WOWGDI_SB_ARG_SRCH);
        DWORD rasterOp = Wow32ArgDword(frame, WOWGDI_SB_ARG_ROP);
        INT  dcKind = -1, sourceKind = -1;
        HGDIOBJ destination = WowGdiH32(destDc16, &dcKind);
        HGDIOBJ source = WowGdiH32(sourceDc16, &sourceKind);
        INT  noteLength = 0, isOk;
        WowNotePut(note, noteCapacity, &noteLength, "StretchBlt dst 0x");
        WowNoteHex(note, noteCapacity, &noteLength, destDc16, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, " (");
        WowNoteHex(note, noteCapacity, &noteLength, (DWORD)positionX, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, ",");
        WowNoteHex(note, noteCapacity, &noteLength, (DWORD)positionY, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, ") ");
        WowNoteHex(note, noteCapacity, &noteLength, (DWORD)width, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, "x");
        WowNoteHex(note, noteCapacity, &noteLength, (DWORD)height, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, " <- src 0x");
        WowNoteHex(note, noteCapacity, &noteLength, sourceDc16, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, " ");
        WowNoteHex(note, noteCapacity, &noteLength, (DWORD)sourceWidth, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, "x");
        WowNoteHex(note, noteCapacity, &noteLength, (DWORD)sourceHeight, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, " rop=0x");
        WowNoteHex(note, noteCapacity, &noteLength, rasterOp, WOW_HEX_DWORD_DIGITS);
        if (!destination || (dcKind != WOWGDI_KIND_DC && dcKind != WOWGDI_KIND_WINDC))
        {
            WowNotePut(note, noteCapacity, &noteLength, " -- ★ THE DESTINATION IS NOT ONE OF OUR"
                                       " DC TOKENS; answered 0");
            Wow32SetReturn(frame, 0);
            return 1;
        }
        if (sourceDc16 && (!source || (sourceKind != WOWGDI_KIND_DC && sourceKind != WOWGDI_KIND_WINDC)))
        {
            WowNotePut(note, noteCapacity, &noteLength, " -- ★ THE SOURCE IS NOT ONE OF OUR DC"
                                       " TOKENS; answered 0");
            Wow32SetReturn(frame, 0);
            return 1;
        }
        isOk = StretchBlt((HDC)destination, positionX, positionY, width, height, sourceDc16 ? (HDC)source : NULL,
                        sourceX, sourceY, sourceWidth, sourceHeight, rasterOp) ? 1 : 0;
        WowNotePut(note, noteCapacity, &noteLength, isOk ? " -> stretched"
                                      : " -- ★ the OS refused it");
        Wow32SetReturn(frame, (DWORD)isOk);
        return 1;
    }

    /* 0x4f GetDCOrg / 0x95 GetBrushOrg / 0x96 UnrealizeObject (Importance = 1): */
    case WOWGDI_GETDCORG:
    case WOWGDI_GETBRUSHORG:
    case WOWGDI_UNREALIZEOBJ:
    {
        WORD handle16 = Wow32ArgWord(frame, WOWGDI_ONE_ARG_HANDLE);
        INT  kind = -1;
        HGDIOBJ object = WowGdiH32(handle16, &kind);
        INT  isDc = (frame->Id != WOWGDI_UNREALIZEOBJ);
        INT  noteLength = 0;
        POINT point;
        WowNotePut(note, noteCapacity, &noteLength,
                frame->Id == WOWGDI_GETDCORG    ? "GetDCOrg(0x" :
                frame->Id == WOWGDI_GETBRUSHORG ? "GetBrushOrg(0x"
                                            : "UnrealizeObject(0x");
        WowNoteHex(note, noteCapacity, &noteLength, handle16, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, ")");
        if (!object)
        {
            WowNotePut(note, noteCapacity, &noteLength, " -- ★ NOT ONE OF OUR GDI TOKENS;"
                                       " answered 0");
            Wow32SetReturn(frame, 0);
            return 1;
        }
        if (isDc && kind != WOWGDI_KIND_DC && kind != WOWGDI_KIND_WINDC)
        {
            WowNotePut(note, noteCapacity, &noteLength, " -- ★ THAT IS NOT A DC; answered 0");
            Wow32SetReturn(frame, 0);
            return 1;
        }
        if (frame->Id == WOWGDI_UNREALIZEOBJ)
        {
            INT result = UnrealizeObject(object) ? 1 : 0;
            WowNotePut(note, noteCapacity, &noteLength, result ? " -> unrealized" : " -- ★ refused");
            Wow32SetReturn(frame, (DWORD)result);
            return 1;
        }
        point.x = point.y = 0;
        if (frame->Id == WOWGDI_GETBRUSHORG)
            GetBrushOrgEx((HDC)object, &point);
        else
            GetDCOrgEx((HDC)object, &point);
        WowNotePut(note, noteCapacity, &noteLength, " -> ");
        WowNoteHex(note, noteCapacity, &noteLength, (DWORD)point.x, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, ",");
        WowNoteHex(note, noteCapacity, &noteLength, (DWORD)point.y, WOW_HEX_WORD_DIGITS);
        Wow32SetReturn(frame, ((DWORD)(WORD)(SHORT)point.y << WORD_SHIFT)
                        | (DWORD)(WORD)(SHORT)point.x);
        return 1;
    }

    /* 0x67 PtVisible(hDC, x, y) (Importance = 1): */
    case WOWGDI_PTVISIBLE:
    {
        WORD dc16 = Wow32ArgWord(frame, WOWGDI_PV_ARG_HDC);
        INT  positionX = (INT)(SHORT)Wow32ArgWord(frame, WOWGDI_PV_ARG_X);
        INT  positionY = (INT)(SHORT)Wow32ArgWord(frame, WOWGDI_PV_ARG_Y);
        INT  kind = -1;
        HGDIOBJ object = WowGdiH32(dc16, &kind);
        INT  noteLength = 0, result;
        WowNotePut(note, noteCapacity, &noteLength, "PtVisible(0x");
        WowNoteHex(note, noteCapacity, &noteLength, dc16, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, ", ");
        WowNoteHex(note, noteCapacity, &noteLength, (DWORD)positionX, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, ",");
        WowNoteHex(note, noteCapacity, &noteLength, (DWORD)positionY, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, ")");
        if (!object || (kind != WOWGDI_KIND_DC && kind != WOWGDI_KIND_WINDC))
        {
            WowNotePut(note, noteCapacity, &noteLength, " -- ★ NOT ONE OF OUR DC TOKENS;"
                                       " answered 0");
            Wow32SetReturn(frame, 0);
            return 1;
        }
        result = PtVisible((HDC)object, positionX, positionY) ? 1 : 0;
        WowNotePut(note, noteCapacity, &noteLength, result ? " -> visible" : " -> NOT visible");
        Wow32SetReturn(frame, (DWORD)result);
        return 1;
    }

    /* 0x63 LPtoDP(hDC, lpPoints, nCount) -- see the note above (Importance = 4): */
    case WOWGDI_POLYGON:
    case WOWGDI_POLYLINE:
    case WOWGDI_DPTOLP:
    case WOWGDI_LPTODP:
    {
        WORD dc16 = Wow32ArgWord(frame, WOWGDI_LDP_ARG_HDC);
        WORD itemCount   = Wow32ArgWord(frame, WOWGDI_LDP_ARG_COUNT);
        volatile BYTE *pointBytes = Wow32ArgPointer(frame, WOWGDI_LDP_ARG_POINTS);
        INT  kind = -1;
        HGDIOBJ object = WowGdiH32(dc16, &kind);
        POINT points[WOWGDI_MAX_POINTS];
        INT  noteLength = 0, index, count = (INT)itemCount;
        INT isLine = (frame->Id == WOWGDI_POLYLINE);
        INT isPolygon = (frame->Id == WOWGDI_POLYGON) || isLine;
        WowNotePut(note, noteCapacity, &noteLength,
                isLine ? "Polyline(0x" : isPolygon ? "Polygon(0x"
                : frame->Id == WOWGDI_DPTOLP ? "DPtoLP(0x" : "LPtoDP(0x");
        WowNoteHex(note, noteCapacity, &noteLength, dc16, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, ", ");
        WowNoteHex(note, noteCapacity, &noteLength, (DWORD)itemCount, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, " points)");
        if (!object || (kind != WOWGDI_KIND_DC && kind != WOWGDI_KIND_WINDC) || !pointBytes)
        {
            WowNotePut(note, noteCapacity, &noteLength, " -- ★ NOT ONE OF OUR DC TOKENS, or no"
                                       " points; answered 0");
            Wow32SetReturn(frame, 0);
            return 1;
        }
        /* [CAUTION]: BOUNDED. The count comes from the guest and the scratch array does
         * not grow; a longer run is refused rather than overrunning.
         */
        if (count <= 0 || count > (INT)(sizeof points / sizeof points[0]))
        {
            WowNotePut(note, noteCapacity, &noteLength, " -- ★ COUNT OUT OF RANGE; answered 0");
            Wow32SetReturn(frame, 0);
            return 1;
        }
        for (index = 0; index < count; ++index)
        {
            points[index].x = (INT)(SHORT)Wow32PeekWord(pointBytes + index * WOWGDI_POINT16_SIZE);
            points[index].y = (INT)(SHORT)Wow32PeekWord(pointBytes + index * WOWGDI_POINT16_SIZE + WOWGDI_POINT16_Y);
        }
        /* [INFO]: Polygon DRAWS and writes nothing back; LPtoDP TRANSFORMS IN PLACE.
         * Same block, opposite data flow -- so they share the read and part
         * company here.
         */
        if (isPolygon)
        {
            INT result = (isLine ? Polyline((HDC)object, points, count)
                            : Polygon((HDC)object, points, count)) ? 1 : 0;
            WowNotePut(note, noteCapacity, &noteLength, result ? " -> drawn" : " -- ★ the OS refused it");
            Wow32SetReturn(frame, (DWORD)result);
            return 1;
        }
        /* [INFO]: DPtoLP is LPtoDP's inverse and nothing else differs -- same block,
         * same in-place write-back, opposite transform.
         */
        if (!(frame->Id == WOWGDI_DPTOLP ? DPtoLP((HDC)object, points, count)
                                     : LPtoDP((HDC)object, points, count)))
        {
            WowNotePut(note, noteCapacity, &noteLength, " -- ★ the OS refused it; answered 0");
            Wow32SetReturn(frame, 0);
            return 1;
        }
        for (index = 0; index < count; ++index)
        {
            Wow32PokeWord(pointBytes + index * WOWGDI_POINT16_SIZE,     (WORD)(SHORT)points[index].x);
            Wow32PokeWord(pointBytes + index * WOWGDI_POINT16_SIZE + WOWGDI_POINT16_Y, (WORD)(SHORT)points[index].y);
        }
        WowNotePut(note, noteCapacity, &noteLength, " -> ");
        WowNoteHex(note, noteCapacity, &noteLength, (DWORD)points[0].x, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, ",");
        WowNoteHex(note, noteCapacity, &noteLength, (DWORD)points[0].y, WOW_HEX_WORD_DIGITS);
        Wow32SetReturn(frame, 1);
        return 1;
    }

    /* 0x9c CreateDiscardableBitmap / 0x94 SetBrushOrg (Importance = 2):
     *
     * [INFO]: "Discardable" was a Win16 memory-pressure hint; Win32 keeps the call and
     * ignores the hint, which is the right answer -- the guest gets a real
     * bitmap and the hint had no observable semantics to preserve.
     */
    case WOWGDI_CREATEDISCARDBM:
    case WOWGDI_SETBRUSHORG:
    {
        INT  isBitmap = (frame->Id == WOWGDI_CREATEDISCARDBM);
        WORD dc16 = Wow32ArgWord(frame, WOWGDI_CCB_ARG_HDC);      /* same block shape as */
        INT  width = (INT)(SHORT)Wow32ArgWord(frame, WOWGDI_CCB_ARG_WIDTH);   /* CreateCompat- */
        INT  height = (INT)(SHORT)Wow32ArgWord(frame, WOWGDI_CCB_ARG_HEIGHT);  /* ibleBitmap */
        INT  kind = -1;
        HGDIOBJ object = WowGdiH32(dc16, &kind);
        INT  noteLength = 0;
        WowNotePut(note, noteCapacity, &noteLength, isBitmap ? "CreateDiscardableBitmap(0x"
                                        : "SetBrushOrg(0x");
        WowNoteHex(note, noteCapacity, &noteLength, dc16, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, ", ");
        WowNoteHex(note, noteCapacity, &noteLength, (DWORD)width, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, ",");
        WowNoteHex(note, noteCapacity, &noteLength, (DWORD)height, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, ")");
        if (!object || (kind != WOWGDI_KIND_DC && kind != WOWGDI_KIND_WINDC))
        {
            WowNotePut(note, noteCapacity, &noteLength, " -- ★ NOT ONE OF OUR DC TOKENS;"
                                       " answered 0");
            Wow32SetReturn(frame, 0);
            return 1;
        }
        if (!isBitmap)
        {
            POINT previous;
            previous.x = previous.y = 0;
            SetBrushOrgEx((HDC)object, width, height, &previous);
            Wow32SetReturn(frame, ((DWORD)(WORD)(SHORT)previous.y << WORD_SHIFT)
                            | (DWORD)(WORD)(SHORT)previous.x);
            return 1;
        }
        {   HBITMAP bitmap;
            WORD token;
            if (width <= 0 || height <= 0)
            {
                WowNotePut(note, noteCapacity, &noteLength, " -- ★ A DIMENSION IS NOT POSITIVE;"
                                           " answered 0");
                Wow32SetReturn(frame, 0);
                return 1;
            }
            bitmap = CreateDiscardableBitmap((HDC)object, width, height);
            token = bitmap ? WowGdiH16((HGDIOBJ)bitmap, WOWGDI_KIND_OBJ) : 0;
            if (!token)
            {
                if (bitmap)
                    DeleteObject((HGDIOBJ)bitmap);
                WowNotePut(note, noteCapacity, &noteLength, " -- ★ refused, or the token map is"
                                           " full; answered 0");
                Wow32SetReturn(frame, 0);
                return 1;
            }
            WowNotePut(note, noteCapacity, &noteLength, " -> bitmap token 0x");
            WowNoteHex(note, noteCapacity, &noteLength, token, WOW_HEX_WORD_DIGITS);
            Wow32SetReturn(frame, token);
            return 1;
        }
    }

    /* 0x1b Rectangle(hDC, left, top, right, bottom) (Importance = 2): */
    case WOWGDI_RECTANGLE:
    {
        WORD dc16 = Wow32ArgWord(frame, WOWGDI_RC_ARG_HDC);
        INT  left = (INT)(SHORT)Wow32ArgWord(frame, WOWGDI_RC_ARG_LEFT);
        INT  top = (INT)(SHORT)Wow32ArgWord(frame, WOWGDI_RC_ARG_TOP);
        INT  right = (INT)(SHORT)Wow32ArgWord(frame, WOWGDI_RC_ARG_RIGHT);
        INT  bottom = (INT)(SHORT)Wow32ArgWord(frame, WOWGDI_RC_ARG_BOTTOM);
        INT  kind = -1;
        HGDIOBJ object = WowGdiH32(dc16, &kind);
        INT  noteLength = 0;
        WowNotePut(note, noteCapacity, &noteLength, "Rectangle(0x");
        WowNoteHex(note, noteCapacity, &noteLength, dc16, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, ", ");
        WowNoteHex(note, noteCapacity, &noteLength, (DWORD)left, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, ",");
        WowNoteHex(note, noteCapacity, &noteLength, (DWORD)top, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, " ");
        WowNoteHex(note, noteCapacity, &noteLength, (DWORD)right, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, ",");
        WowNoteHex(note, noteCapacity, &noteLength, (DWORD)bottom, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, ")");
        if (!object || (kind != WOWGDI_KIND_DC && kind != WOWGDI_KIND_WINDC))
        {
            WowNotePut(note, noteCapacity, &noteLength, " -- ★ NOT ONE OF OUR DC TOKENS;"
                                       " answered 0");
            Wow32SetReturn(frame, 0);
            return 1;
        }
        Wow32SetReturn(frame, (DWORD)(Rectangle((HDC)object, left, top, right, bottom) ? 1 : 0));
        return 1;
    }

    /* 0x01 SetBkColor / 0x09 SetTextColor -- a COLORREF is a COLORREF (Importance = 1):
     *
     * [CAUTION]: The PREVIOUS colour is the return value and it is a DWORD, so this is
     * one of the calls where the 32-bit return really is 32 bits.
     */
    case WOWGDI_SETBKCOLOR:
    case WOWGDI_SETTEXTCOLOR:
    {
        INT  isText = (frame->Id == WOWGDI_SETTEXTCOLOR);
        WORD dc16 = Wow32ArgWord(frame, WOWGDI_COL_ARG_HDC);
        DWORD color = Wow32ArgDword(frame, WOWGDI_COL_ARG_COLOR);
        INT  kind = -1;
        HGDIOBJ object = WowGdiH32(dc16, &kind);
        INT  noteLength = 0;
        COLORREF previous;
        WowNotePut(note, noteCapacity, &noteLength, isText ? "SetTextColor(0x" : "SetBkColor(0x");
        WowNoteHex(note, noteCapacity, &noteLength, dc16, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, ", 0x");
        WowNoteHex(note, noteCapacity, &noteLength, color, WOW_HEX_DWORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, ")");
        if (!object || (kind != WOWGDI_KIND_DC && kind != WOWGDI_KIND_WINDC))
        {
            WowNotePut(note, noteCapacity, &noteLength, " -- ★ NOT ONE OF OUR DC TOKENS;"
                                       " answered 0");
            Wow32SetReturn(frame, 0);
            return 1;
        }
        previous = isText ? SetTextColor((HDC)object, (COLORREF)color)
                      : SetBkColor((HDC)object, (COLORREF)color);
        Wow32SetReturn(frame, (DWORD)previous);
        return 1;
    }

    /* The (hDC, int mode) family -- one int, same numbering both sides (Importance = 1):
     * 0x04 SetROP2, 0x07 SetStretchBltMode, 0x02 SetBkMode, 0x03 SetMapMode,
     * 0x06 SetPolyFillMode. All 4 argument bytes, all returning the previous
     * mode. [CAUTION] The last three arrived with MS Paint's shape tools: SetBkMode
     * is the first call of the commit sequence and SetMapMode goes with the
     * SetWindowExt/SetViewportExt pair, which is MM_ANISOTROPIC (8) being set
     * up.
     */
    case WOWGDI_SETROP2:
    case WOWGDI_SETSTRETCHMODE:
    case WOWGDI_SETBKMODE:
    case WOWGDI_SETMAPMODE:
    case WOWGDI_SETTEXTALIGN:
    case WOWGDI_SETPOLYFILLMODE:
    {
        WORD dc16  = Wow32ArgWord(frame, WOWGDI_MODE_ARG_HDC);
        INT  mode = (INT)(SHORT)Wow32ArgWord(frame, WOWGDI_MODE_ARG_MODE);
        INT  kind = -1;
        HGDIOBJ object = WowGdiH32(dc16, &kind);
        INT  noteLength = 0, previous;
        WowNotePut(note, noteCapacity, &noteLength,
                frame->Id == WOWGDI_SETROP2         ? "SetROP2(0x" :
                frame->Id == WOWGDI_SETSTRETCHMODE  ? "SetStretchBltMode(0x" :
                frame->Id == WOWGDI_SETBKMODE       ? "SetBkMode(0x" :
                frame->Id == WOWGDI_SETMAPMODE      ? "SetMapMode(0x" :
                frame->Id == WOWGDI_SETTEXTALIGN    ? "SetTextAlign(0x"
                                                : "SetPolyFillMode(0x");
        WowNoteHex(note, noteCapacity, &noteLength, dc16, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, ", ");
        WowNoteHex(note, noteCapacity, &noteLength, (DWORD)mode, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, ")");
        if (!object || (kind != WOWGDI_KIND_DC && kind != WOWGDI_KIND_WINDC))
        {
            WowNotePut(note, noteCapacity, &noteLength, " -- ★ NOT ONE OF OUR DC TOKENS;"
                                       " answered 0");
            Wow32SetReturn(frame, 0);
            return 1;
        }
        previous = frame->Id == WOWGDI_SETROP2        ? SetROP2((HDC)object, mode)
             : frame->Id == WOWGDI_SETSTRETCHMODE ? SetStretchBltMode((HDC)object, mode)
             : frame->Id == WOWGDI_SETBKMODE      ? SetBkMode((HDC)object, mode)
             : frame->Id == WOWGDI_SETMAPMODE     ? SetMapMode((HDC)object, mode)
             : frame->Id == WOWGDI_SETTEXTALIGN   ? (INT)SetTextAlign((HDC)object, (UINT)mode)
                                              : SetPolyFillMode((HDC)object, mode);
        Wow32SetReturn(frame, (DWORD)(WORD)previous);
        return 1;
    }

    /* 0x0b SetWindowOrg(hDC, x, y) (Importance = 1): */
    case WOWGDI_SETWINDOWORG:
    {
        WORD dc16 = Wow32ArgWord(frame, WOWGDI_ORG_ARG_HDC);
        INT  positionX = (INT)(SHORT)Wow32ArgWord(frame, WOWGDI_ORG_ARG_X);
        INT  positionY = (INT)(SHORT)Wow32ArgWord(frame, WOWGDI_ORG_ARG_Y);
        INT  kind = -1;
        HGDIOBJ object = WowGdiH32(dc16, &kind);
        INT  noteLength = 0;
        POINT previous;
        WowNotePut(note, noteCapacity, &noteLength, "SetWindowOrg(0x");
        WowNoteHex(note, noteCapacity, &noteLength, dc16, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, ", ");
        WowNoteHex(note, noteCapacity, &noteLength, (DWORD)positionX, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, ",");
        WowNoteHex(note, noteCapacity, &noteLength, (DWORD)positionY, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, ")");
        if (!object || (kind != WOWGDI_KIND_DC && kind != WOWGDI_KIND_WINDC))
        {
            WowNotePut(note, noteCapacity, &noteLength, " -- ★ NOT ONE OF OUR DC TOKENS;"
                                       " answered 0");
            Wow32SetReturn(frame, 0);
            return 1;
        }
        previous.x = previous.y = 0;
        SetWindowOrgEx((HDC)object, positionX, positionY, &previous);
        Wow32SetReturn(frame, ((DWORD)(WORD)(SHORT)previous.y << WORD_SHIFT)
                        | (DWORD)(WORD)(SHORT)previous.x);
        return 1;
    }

    /* 0x1e SaveDC / 0x27 RestoreDC -- the guest's own nesting level (Importance = 1):
     *
     * [CAUTION]: THE LEVEL IS THE GUEST'S NUMBER AND IT TRAVELS UNCHANGED. Win32 keeps
     * its own stack per DC and uses the same convention (positive = absolute,
     * negative = relative), and our DCs are the ones the guest saved on, so
     * the level it hands back is meaningful without translation.
     */
    case WOWGDI_SAVEDC:
    case WOWGDI_RESTOREDC:
    {
        INT  isSave = (frame->Id == WOWGDI_SAVEDC);
        WORD dc16 = isSave ? Wow32ArgWord(frame, WOWGDI_SDC_ARG_HDC)
                          : Wow32ArgWord(frame, WOWGDI_RDC2_ARG_HDC);
        INT  level = isSave ? 0 : (INT)(SHORT)Wow32ArgWord(frame, WOWGDI_RDC2_ARG_LEVEL);
        INT  kind = -1;
        HGDIOBJ object = WowGdiH32(dc16, &kind);
        INT  noteLength = 0, result;
        WowNotePut(note, noteCapacity, &noteLength, isSave ? "SaveDC(0x" : "RestoreDC(0x");
        WowNoteHex(note, noteCapacity, &noteLength, dc16, WOW_HEX_WORD_DIGITS);
        if (!isSave)
        {
            WowNotePut(note, noteCapacity, &noteLength, ", level ");
            WowNoteHex(note, noteCapacity, &noteLength, (DWORD)level, WOW_HEX_WORD_DIGITS);
        }
        WowNotePut(note, noteCapacity, &noteLength, ")");
        if (!object || (kind != WOWGDI_KIND_DC && kind != WOWGDI_KIND_WINDC))
        {
            WowNotePut(note, noteCapacity, &noteLength, " -- ★ NOT ONE OF OUR DC TOKENS;"
                                       " answered 0");
            Wow32SetReturn(frame, 0);
            return 1;
        }
        result = isSave ? SaveDC((HDC)object) : (RestoreDC((HDC)object, level) ? 1 : 0);
        WowNotePut(note, noteCapacity, &noteLength, " -> ");
        WowNoteHex(note, noteCapacity, &noteLength, (DWORD)result, WOW_HEX_WORD_DIGITS);
        Wow32SetReturn(frame, (DWORD)(WORD)result);
        return 1;
    }

    /* 0x13 LineTo / 0x14 MoveTo -- see the note above (Importance = 4): */
    case WOWGDI_LINETO:
    case WOWGDI_MOVETO:
    {
        INT  isLine = (frame->Id == WOWGDI_LINETO);
        WORD dc16 = Wow32ArgWord(frame, WOWGDI_XY_ARG_HDC);
        INT  positionX = (INT)(SHORT)Wow32ArgWord(frame, WOWGDI_XY_ARG_X);
        INT  positionY = (INT)(SHORT)Wow32ArgWord(frame, WOWGDI_XY_ARG_Y);
        INT  kind = -1;
        HGDIOBJ object = WowGdiH32(dc16, &kind);
        INT  noteLength = 0;
        POINT previous;
        WowNotePut(note, noteCapacity, &noteLength, isLine ? "LineTo(0x" : "MoveTo(0x");
        WowNoteHex(note, noteCapacity, &noteLength, dc16, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, ", ");
        WowNoteHex(note, noteCapacity, &noteLength, (DWORD)positionX, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, ",");
        WowNoteHex(note, noteCapacity, &noteLength, (DWORD)positionY, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, ")");
        if (!object || (kind != WOWGDI_KIND_DC && kind != WOWGDI_KIND_WINDC))
        {
            WowNotePut(note, noteCapacity, &noteLength, " -- ★ NOT ONE OF OUR DC TOKENS;"
                                       " answered 0");
            Wow32SetReturn(frame, 0);
            return 1;
        }
        if (isLine)
        {
            Wow32SetReturn(frame, (DWORD)(LineTo((HDC)object, positionX, positionY) ? 1 : 0));
            return 1;
        }
        previous.x = previous.y = 0;
        MoveToEx((HDC)object, positionX, positionY, &previous);
        /* [INFO]: y in the HIGH word, x in the LOW -- Win16's MAKELONG order. */
        Wow32SetReturn(frame, ((DWORD)(WORD)(SHORT)previous.y << WORD_SHIFT)
                        | (DWORD)(WORD)(SHORT)previous.x);
        return 1;
    }

    /* 0x1d PatBlt(hDC, x, y, nWidth, nHeight, dwRop) (Importance = 3):
     * The raster ops are the same numbers in both worlds (PATCOPY, PATINVERT,
     * DSTINVERT, BLACKNESS, WHITENESS), so the rop travels unchanged.
     */
    case WOWGDI_PATBLT:
    {
        WORD  dc16 = Wow32ArgWord(frame, WOWGDI_PB_ARG_HDC);
        INT   positionX = (INT)(SHORT)Wow32ArgWord(frame, WOWGDI_PB_ARG_X);
        INT   positionY = (INT)(SHORT)Wow32ArgWord(frame, WOWGDI_PB_ARG_Y);
        INT   width = (INT)(SHORT)Wow32ArgWord(frame, WOWGDI_PB_ARG_WIDTH);
        INT   height = (INT)(SHORT)Wow32ArgWord(frame, WOWGDI_PB_ARG_HEIGHT);
        DWORD rasterOp = Wow32ArgDword(frame, WOWGDI_PB_ARG_ROP);
        INT   kind = -1;
        HGDIOBJ object = WowGdiH32(dc16, &kind);
        INT   noteLength = 0;
        WowNotePut(note, noteCapacity, &noteLength, "PatBlt(0x");
        WowNoteHex(note, noteCapacity, &noteLength, dc16, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, ", ");
        WowNoteHex(note, noteCapacity, &noteLength, (DWORD)positionX, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, ",");
        WowNoteHex(note, noteCapacity, &noteLength, (DWORD)positionY, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, " ");
        WowNoteHex(note, noteCapacity, &noteLength, (DWORD)width, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, "x");
        WowNoteHex(note, noteCapacity, &noteLength, (DWORD)height, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, " rop=0x");
        WowNoteHex(note, noteCapacity, &noteLength, rasterOp, WOW_HEX_DWORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, ")");
        if (!object || (kind != WOWGDI_KIND_DC && kind != WOWGDI_KIND_WINDC))
        {
            WowNotePut(note, noteCapacity, &noteLength, " -- ★ NOT ONE OF OUR DC TOKENS;"
                                       " answered 0");
            Wow32SetReturn(frame, 0);
            return 1;
        }
        Wow32SetReturn(frame, (DWORD)(PatBlt((HDC)object, positionX, positionY, width, height, rasterOp) ? 1 : 0));
        return 1;
    }

    /* 0x57 GetStockObject(nIndex) (Importance = 2):
     * Straight through, on the same claim the SM_* indices and the DC
     * capability indices travel on: Win32 inherited the stock-object indices
     * from Win16 unchanged (WHITE_BRUSH 0 .. SYSTEM_FIXED_FONT 16). Win16
     * stopped at 16, so an index above that is a guest asking for something
     * its own Windows never had, and it is refused rather than quietly given
     * a Win32-only object.
     *
     * [CAUTION]: THE TOKEN IS MINTED AS STOCK so that a later DeleteObject on it can say
     * what it is instead of asking Win32 to destroy something the system owns.
     *
     * [INFO]: The map de-duplicates by object, so the same stock object always comes
     * back as the same token -- which matters, because guests compare them.
     */
    case WOWGDI_GETSTOCKOBJECT:
    {
        WORD itemIndex = Wow32ArgWord(frame, WOWGDI_GSO_ARG_INDEX);
        HGDIOBJ object;
        WORD token;
        INT noteLength = 0;
        WowNotePut(note, noteCapacity, &noteLength, "GetStockObject(0x");
        WowNoteHex(note, noteCapacity, &noteLength, itemIndex, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, ")");
        if (itemIndex > WOWGDI_STOCK_OBJECT_LAST)
        {
            WowNotePut(note, noteCapacity, &noteLength, " -- ★ NOT A Win16 STOCK OBJECT (they stop"
                                       " at 16); answered 0");
            Wow32SetReturn(frame, 0);
            return 1;
        }
        object = GetStockObject((INT)itemIndex);
        /* [CAUTION]: "NO SUCH OBJECT" AND "NO ROOM" ARE DIFFERENT FACTS, and the first
         * cut of this printed the second for both -- so a run reported "THE
         * GDI TOKEN MAP IS FULL" with nine of 256 slots used, because index 9
         * is a hole in the stock-object numbering (it sits between NULL_PEN
         * and OEM_FIXED_FONT) and the OS correctly returned NULL. A counter's
         * message is a claim; this one was false.
         */
        if (!object)
        {
            WowNotePut(note, noteCapacity, &noteLength, " -- ★ THE OS HAS NO SUCH STOCK OBJECT"
                                       " (index 9 is a hole in the numbering);"
                                       " answered 0");
            Wow32SetReturn(frame, 0);
            return 1;
        }
        token = WowGdiH16(object, WOWGDI_KIND_STOCK);
        if (!token)
        {
            WowNotePut(note, noteCapacity, &noteLength, " -- ★ THE GDI TOKEN MAP IS FULL;"
                                       " answered 0");
            Wow32SetReturn(frame, 0);
            return 1;
        }
        WowNotePut(note, noteCapacity, &noteLength, " -> token 0x");
        WowNoteHex(note, noteCapacity, &noteLength, token, WOW_HEX_WORD_DIGITS);
        Wow32SetReturn(frame, token);
        return 1;
    }

    /* 0x99 CreateDC(lpszDriver, lpszDevice, lpszOutput, lpInitData) (Importance = 3):
     *
     * [INFO]: ONLY "DISPLAY" IS ANSWERED, and that is a decision rather than a
     * shortcut. The screen is a device this host really does have, and Win32's
     * own `CreateDCA("DISPLAY", NULL, NULL, NULL)` is the same request for the
     * same thing. A printer driver name is not: it would name a Windows 3.1
     * driver that does not exist on XP, and handing the string to Win32 would
     * either fail obscurely or -- worse -- succeed against some unrelated
     * printer the user did not ask to be drawn on. Refused, by name, in the
     * log.
     *
     * [CAUTION]: lpInitData IS IGNORED WHEN IT IS NULL, WHICH IS THE ONLY CASE SEEN. It
     * carries a DEVMODE for printers; if one ever arrives here it will be
     * reported rather than silently dropped, because a DEVMODE that is not
     * honoured changes what gets drawn.
     */
    case WOWGDI_CREATEDC:
    case WOWGDI_CREATEDC2:
    {
        CHAR driverName[WOWGDI_DEVICE_NAME_MAX], deviceName[WOWGDI_DEVICE_NAME_MAX];
        HDC dc;
        WORD token;
        INT  noteLength = 0, isDisplay, index;
        INT  isCreateDc = (frame->Id == WOWGDI_CREATEDC);
        DWORD initData = Wow32ArgDword(frame, WOWGDI_CDC_ARG_INITDATA);
        Wow32ArgString(frame, WOWGDI_CDC_ARG_DRIVER, driverName, sizeof driverName);
        Wow32ArgString(frame, WOWGDI_CDC_ARG_DEVICE, deviceName, sizeof deviceName);
        WowNotePut(note, noteCapacity, &noteLength, isCreateDc ? "CreateIC driver=" : "CreateDC driver=");
        WowNoteQuoted(note, noteCapacity, &noteLength, driverName);
        if (deviceName[0])
        {
            WowNotePut(note, noteCapacity, &noteLength, " device=");
            WowNoteQuoted(note, noteCapacity, &noteLength, deviceName);
        }
        /* Case-insensitively "DISPLAY" -- the guest writes it lower case. */
        isDisplay = 1;
        for (index = 0; index < WOWGDI_DISPLAY_NAME_LENGTH; ++index)
        {
            CHAR letter = driverName[index];
            if (letter >= 'a' && letter <= 'z')
                letter = (CHAR)(letter - WOWGDI_LOWER_TO_UPPER);
            if (letter != "DISPLAY"[index])
            {
                isDisplay = 0;
                break;
            }
        }
        if (isDisplay && driverName[WOWGDI_DISPLAY_NAME_LENGTH])
            isDisplay = 0;
        if (!isDisplay)
        {
            WowNotePut(note, noteCapacity, &noteLength, " -- ★ NOT THE DISPLAY. That names a"
                                       " Windows 3.1 device driver which does not"
                                       " exist here, so it is refused rather than"
                                       " resolved against one of the host's own"
                                       " devices; answered 0");
            Wow32SetReturn(frame, 0);
            return 1;
        }
        if (initData)
            WowNotePut(note, noteCapacity, &noteLength, " -- ⚠ lpInitData IS NOT NULL and is being"
                                       " ignored (it carries a DEVMODE)");
        /* [INFO]: An INFORMATION context is asked for by name and answered with one:
         * it is cheaper than a DC and a guest that draws on one is doing
         * something Windows would refuse too, so the distinction is kept
         * rather than flattened into CreateDC.
         */
        dc = isCreateDc ? CreateICA("DISPLAY", NULL, NULL, NULL)
                  : CreateDCA("DISPLAY", NULL, NULL, NULL);
        token = dc ? WowGdiH16((HGDIOBJ)dc, WOWGDI_KIND_DC) : 0;
        if (!token)
        {
            if (dc)
                DeleteDC(dc);
            WowNotePut(note, noteCapacity, &noteLength, dc ? " -- ★ THE GDI TOKEN MAP IS FULL; the"
                                            " DC was deleted again and 0 answered"
                                          : " -- ★ THE OS REFUSED IT; answered 0");
            Wow32SetReturn(frame, 0);
            return 1;
        }
        WowNotePut(note, noteCapacity, &noteLength, " -> screen DC token 0x");
        WowNoteHex(note, noteCapacity, &noteLength, token, WOW_HEX_WORD_DIGITS);
        Wow32SetReturn(frame, token);
        return 1;
    }

    /* 0x34 CreateCompatibleDC(hDC) (Importance = 2):
     * The memory DC a paint program draws its canvas into. hDC == 0 is legal
     * and means "compatible with the screen", so a null token is passed
     * through as NULL rather than refused -- the same rule GetDC follows.
     *
     * [CAUTION]: MINTED AS DC, not WINDC: this one is OWNED by the guest and goes back
     * through DeleteDC. That distinction is the whole reason the map records a
     * kind (see the header).
     */
    case WOWGDI_CREATECOMPATDC:
    {
        WORD sourceDc16 = Wow32ArgWord(frame, WOWGDI_CCD_ARG_HDC);
        INT  kind = -1;
        HGDIOBJ object = sourceDc16 ? WowGdiH32(sourceDc16, &kind) : NULL;
        HDC dc;
        WORD token;
        INT noteLength = 0;
        WowNotePut(note, noteCapacity, &noteLength, "CreateCompatibleDC(0x");
        WowNoteHex(note, noteCapacity, &noteLength, sourceDc16, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, ")");
        if (sourceDc16 && (!object || (kind != WOWGDI_KIND_DC && kind != WOWGDI_KIND_WINDC)))
        {
            WowNotePut(note, noteCapacity, &noteLength, " -- ★ THAT IS NOT ONE OF OUR DC TOKENS;"
                                       " answered 0");
            Wow32SetReturn(frame, 0);
            return 1;
        }
        if (!sourceDc16)
            WowNotePut(note, noteCapacity, &noteLength, " (compatible with the SCREEN)");
        dc = CreateCompatibleDC(sourceDc16 ? (HDC)object : NULL);
        token = dc ? WowGdiH16((HGDIOBJ)dc, WOWGDI_KIND_DC) : 0;
        if (!token)
        {
            if (dc)
                DeleteDC(dc);             /* never issue a DC we cannot name */
            WowNotePut(note, noteCapacity, &noteLength, dc ? " -- ★ THE GDI TOKEN MAP IS FULL; the"
                                            " DC was deleted again and 0 answered"
                                          : " -- ★ THE OS REFUSED IT; answered 0");
            Wow32SetReturn(frame, 0);
            return 1;
        }
        WowNotePut(note, noteCapacity, &noteLength, " -> DC token 0x");
        WowNoteHex(note, noteCapacity, &noteLength, token, WOW_HEX_WORD_DIGITS);
        Wow32SetReturn(frame, token);
        return 1;
    }

    /* -- 0x30 CreateBitmap(nWidth, nHeight, cPlanes, cBitsPixel, lpvBits) -
     * Unlike CreateCompatibleBitmap this one names its own format, so it needs
     * no DC at all.
     *
     * [CAUTION]: lpvBits IS OPTIONAL AND IS BOUNDS-CHECKED WHEN PRESENT. Paint passes
     * NULL (an uninitialised bitmap), but a non-null pointer means GDI will
     * read `((width*planes*bpp + 15)/16)*2 * height` bytes out of a 16-bit
     * segment -- so the arithmetic is done here, in 32 bits, and a bitmap
     * whose bits would run past a 64K segment is refused rather than handed to
     * GDI to read whatever follows.
     *
     * [CAUTION]: THE DIMENSIONS ARE SIGNED 16-BIT, as in CreateCompatibleBitmap.
     */
    /* 0x26 Escape -- THE DRIVER BACK DOOR, ANSWERED HONESTLY (Importance = 1):
     * WRITE.EXE is the guest that asks (its printing path). On a screen DC the
     * truthful answer to a device escape is "this driver does not implement
     * it", and Escape says that with 0 -- an in-band answer, not a sentinel we
     * invented. QUERYESCSUPPORT is answered properly, which is the whole point
     * of having it: a caller that asks first is told before it tries.
     *
     * [CAUTION]: SAID OUT LOUD RATHER THAN LEFT IN A ZERO. Every escape number this host
     * declines is NAMED in the log, so the first guest that genuinely needs
     * one is a line to grep for rather than a mystery -- this project's own
     * `an unimplemented call still answers` rule, applied to a call whose
     * answer is legitimately 0.
     */
    case WOWGDI_ESCAPE:
    {
        WORD dc16  = Wow32ArgWord(frame, WOWGDI_ESC_ARG_HDC);
        INT  escape  = (INT)(SHORT)Wow32ArgWord(frame, WOWGDI_ESC_ARG_ESCAPE);
        INT  count  = (INT)(SHORT)Wow32ArgWord(frame, WOWGDI_ESC_ARG_COUNT);
        volatile BYTE *input = Wow32ArgPointer(frame, WOWGDI_ESC_ARG_IN);
        INT  noteLength = 0, wanted = 0;
        WowNotePut(note, noteCapacity, &noteLength, "Escape(dc 0x");
        WowNoteHex(note, noteCapacity, &noteLength, dc16, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, ", nEscape=");
        WowNoteHex(note, noteCapacity, &noteLength, (DWORD)escape, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, ", count=");
        WowNoteHex(note, noteCapacity, &noteLength, (DWORD)count, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, ")");
        if (escape == WOWGDI_ESC_QUERYESCSUPPORT)
        {
            /* lpInData points at the escape number being asked about. */
            if (input)
                wanted = (INT)(WORD)(input[0] | (input[1] << BYTE_SHIFT));
            WowNotePut(note, noteCapacity, &noteLength, " -- QUERYESCSUPPORT for ");
            WowNoteHex(note, noteCapacity, &noteLength, (DWORD)wanted, WOW_HEX_WORD_DIGITS);
            WowNotePut(note, noteCapacity, &noteLength, ": this DC supports QUERYESCSUPPORT and"
                                       " nothing else, so the caller is told NO"
                                       " before it tries");
            Wow32SetReturn(frame, (DWORD)(wanted == WOWGDI_ESC_QUERYESCSUPPORT ? 1 : 0));
            return 1;
        }
        WowNotePut(note, noteCapacity, &noteLength, " -- ★ NOT IMPLEMENTED BY THIS DRIVER, which"
                                   " is what 0 MEANS for Escape (a screen DC has"
                                   " no printing escapes). A real printing path"
                                   " would need STARTDOC/NEWFRAME/ENDDOC and a"
                                   " 16-bit abort procedure; this host has no"
                                   " printer DC to start one on");
        Wow32SetReturn(frame, 0);
        return 1;
    }

    /* 0x31 CreateBitmapIndirect(LPBITMAP):
     * CreateBitmap's five arguments arriving in a 14-byte Win16 BITMAP instead
     * of on the stack. Read field by field: Win32's BITMAP is 24 bytes with
     * LONGs, so a cast would take bmWidth from the wrong half of bmType.
     */
    case WOWGDI_CREATEBITMAPINDIRECT:
    {
        const volatile BYTE *bitmap16 = Wow32ArgPointer(frame, WOWGDI_CBI_ARG_BITMAP);
        INT  noteLength = 0, width, height;
        WORD planes, bitsPerPixel, token;
        DWORD bits;
        HBITMAP bitmap;
        WowNotePut(note, noteCapacity, &noteLength, "CreateBitmapIndirect ");
        if (!bitmap16)
        {
            WowNotePut(note, noteCapacity, &noteLength, "-- ★ the BITMAP far pointer does not"
                                       " resolve; answered 0");
            Wow32SetReturn(frame, 0);
            return 1;
        }
        width   = (INT)(SHORT)WowGdiPeek(bitmap16, WOWGDI_CBI_OFF_WIDTH);
        height   = (INT)(SHORT)WowGdiPeek(bitmap16, WOWGDI_CBI_OFF_HEIGHT);
        planes  = bitmap16[WOWGDI_CBI_OFF_PLANES];
        bitsPerPixel = bitmap16[WOWGDI_CBI_OFF_BPP];
        bits = (DWORD)WowGdiPeek(bitmap16, WOWGDI_CBI_OFF_BITS)
             | ((DWORD)WowGdiPeek(bitmap16, WOWGDI_CBI_OFF_BITS + WOW_WORD_BYTES) << WORD_SHIFT);
        WowNoteHex(note, noteCapacity, &noteLength, (DWORD)width, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, "x");
        WowNoteHex(note, noteCapacity, &noteLength, (DWORD)height, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, " planes=");
        WowNoteHex(note, noteCapacity, &noteLength, planes, WOW_HEX_BYTE_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, " bpp=");
        WowNoteHex(note, noteCapacity, &noteLength, bitsPerPixel, WOW_HEX_BYTE_DIGITS);
        if (width <= 0 || height <= 0 || !planes || !bitsPerPixel)
        {
            WowNotePut(note, noteCapacity, &noteLength, " -- ★ A DIMENSION OR FORMAT IS NOT"
                                       " POSITIVE; answered 0");
            Wow32SetReturn(frame, 0);
            return 1;
        }
        /* [CAUTION]: bmBits IS IGNORED HERE ON PURPOSE, and the log says so: the pointer
         * is a guest far pointer whose bytes we would have to resolve and
         * validate exactly as CreateBitmap does. No guest measured passes one
         * (WRITE passes 0), so the code that would follow it is not written
         * blind -- it is left to the first run that needs it.
         */
        if (bits) WowNotePut(note, noteCapacity, &noteLength, " [★ bmBits IS NON-ZERO and is NOT"
                                             " being read -- the bitmap comes"
                                             " back UNINITIALISED]");
        bitmap = CreateBitmap(width, height, planes, bitsPerPixel, NULL);
        token = WowGdiH16((HGDIOBJ)bitmap, WOWGDI_KIND_OBJ);
        WowNotePut(note, noteCapacity, &noteLength, " -> 0x");
        WowNoteHex(note, noteCapacity, &noteLength, token, WOW_HEX_WORD_DIGITS);
        Wow32SetReturn(frame, token);
        return 1;
    }

    /* 0x133 GetCharABCWidths -- AND THE STRUCTURE IS HALF THE SIZE (Importance = 2):
     * See wowconv.h: Win16's ABC is 6 bytes, Win32's is 12, so the array is
     * converted entry by entry into the guest's own buffer. Writing Win32's
     * directly would overrun a correctly sized guest buffer by 2x.
     *
     * [CAUTION]: ONLY A TRUETYPE FONT HAS ABC WIDTHS. Win32 fails the call for a raster
     * font, and FALSE is the right answer to pass on -- a caller that gets it
     * falls back to GetTextExtent, which this host implements.
     */
    case WOWGDI_GETCHARABCWIDTHS:
    {
        WORD dc16   = Wow32ArgWord(frame, WOWGDI_ABCW_ARG_HDC);
        WORD firstChar = Wow32ArgWord(frame, WOWGDI_ABCW_ARG_FIRST);
        WORD lastChar  = Wow32ArgWord(frame, WOWGDI_ABCW_ARG_LAST);
        volatile BYTE *output = Wow32ArgPointer(frame, WOWGDI_ABCW_ARG_ABC);
        HDC dc = (HDC)WowGdiH32(dc16, NULL);
        INT noteLength = 0, itemCount, index, isOk = 0;
        WowNotePut(note, noteCapacity, &noteLength, "GetCharABCWidths(dc 0x");
        WowNoteHex(note, noteCapacity, &noteLength, dc16, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, ", ");
        WowNoteHex(note, noteCapacity, &noteLength, firstChar, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, "..");
        WowNoteHex(note, noteCapacity, &noteLength, lastChar, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, ")");
        itemCount = (INT)lastChar - (INT)firstChar + 1;
        if (!dc || !output || itemCount <= 0 || itemCount > WOWGDI_ABC_MAX)
        {
            WowNotePut(note, noteCapacity, &noteLength, " -- ★ no DC, no buffer, or a range this"
                                       " host will not size a temporary for;"
                                       " answered FALSE");
            Wow32SetReturn(frame, 0);
            return 1;
        }
        {   static ABC abcWidths[WOWGDI_ABC_MAX];
            isOk = GetCharABCWidthsA(dc, firstChar, lastChar, abcWidths) ? 1 : 0;
            if (isOk)
            {
                for (index = 0; index < itemCount; ++index)
                {
                    long value[WOWCONV_ABC_C + 1];
                    BYTE abc16[WOWCONV_ABC16_SIZE];
                    INT byteIndex;
                    value[0] = (long)abcWidths[index].abcA;
                    value[1] = (long)abcWidths[index].abcB;
                    value[WOWCONV_ABC_C] = (long)abcWidths[index].abcC;
                    WowConvAbc32To16(value, abc16);
                    for (byteIndex = 0; byteIndex < WOWCONV_ABC16_SIZE; ++byteIndex)
                        output[index * WOWCONV_ABC16_SIZE + byteIndex] = abc16[byteIndex];
                }
                WowNotePut(note, noteCapacity, &noteLength, " -> 0x");
                WowNoteHex(note, noteCapacity, &noteLength, (DWORD)itemCount, WOW_HEX_WORD_DIGITS);
                WowNotePut(note, noteCapacity, &noteLength, " glyph(s), narrowed 12 bytes -> 6");
            }
            else
            {
                WowNotePut(note, noteCapacity, &noteLength, " -- FALSE from the OS (a raster font"
                                           " has no ABC widths; the caller falls"
                                           " back to GetTextExtent)");
            }
        }
        Wow32SetReturn(frame, (DWORD)isOk);
        return 1;
    }

    /* 0x16b GetPaletteEntries:
     * PALETTEENTRY is four bytes in both worlds, so this one is a copy -- and
     * knowing WHICH structures are identical is worth as much as knowing which
     * are not (see wowconv.h).
     */
    case WOWGDI_SETPALETTEENTRIES:
    {
        WORD palette16  = Wow32ArgWord(frame, WOWGDI_GPE_ARG_HPAL);
        WORD startIndex = Wow32ArgWord(frame, WOWGDI_GPE_ARG_START);
        WORD count   = Wow32ArgWord(frame, WOWGDI_GPE_ARG_COUNT);
        volatile BYTE *input = Wow32ArgPointer(frame, WOWGDI_GPE_ARG_ENTRIES);
        HPALETTE palette = (HPALETTE)WowGdiH32(palette16, NULL);
        static PALETTEENTRY paletteEntry[WOWGDI_PALETTE_MAX];
        INT  noteLength = 0, index;
        UINT returned = 0;
        WowNotePut(note, noteCapacity, &noteLength, "SetPaletteEntries(0x");
        WowNoteHex(note, noteCapacity, &noteLength, palette16, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, ", start=");
        WowNoteHex(note, noteCapacity, &noteLength, startIndex, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, ", n=");
        WowNoteHex(note, noteCapacity, &noteLength, count, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, ")");
        if (!palette || GetObjectType((HGDIOBJ)palette) != OBJ_PAL || !input)
        {
            WowNotePut(note, noteCapacity, &noteLength, " -- ★ NOT ONE OF OUR PALETTES (or no entries); 0");
            Wow32SetReturn(frame, 0);
            return 1;
        }
        if (count > WOWGDI_PALETTE_MAX)
            count = WOWGDI_PALETTE_MAX;
        for (index = 0; index < count; ++index)               /* four bytes in both: a copy */
        {
            paletteEntry[index].peRed = input[index * WOWGDI_PALETTEENTRY16_SIZE];
            paletteEntry[index].peGreen = input[index * WOWGDI_PALETTEENTRY16_SIZE + WOWGDI_PE16_GREEN];
            paletteEntry[index].peBlue = input[index * WOWGDI_PALETTEENTRY16_SIZE + WOWGDI_PE16_BLUE];
            paletteEntry[index].peFlags = input[index * WOWGDI_PALETTEENTRY16_SIZE + WOWGDI_PE16_FLAGS];
        }
        returned = SetPaletteEntries(palette, startIndex, count, paletteEntry);
        WowNotePut(note, noteCapacity, &noteLength, " -> 0x");
        WowNoteHex(note, noteCapacity, &noteLength, returned, WOW_HEX_WORD_DIGITS);
        Wow32SetReturn(frame, returned);
        return 1;
    }

    case WOWGDI_GETVIEWPORTEXT:
    case WOWGDI_GETWINDOWEXT:
    case WOWGDI_GETBITMAPDIMENSION:
    {
        WORD handle16 = Wow32ArgWord(frame, 0);
        INT  kind = -1, noteLength = 0, isOk = 0;
        HGDIOBJ object = WowGdiH32(handle16, &kind);
        SIZE size;
        PCSTR callName = frame->Id == WOWGDI_GETVIEWPORTEXT ? "GetViewportExt(0x"
                       : frame->Id == WOWGDI_GETWINDOWEXT   ? "GetWindowExt(0x"
                       : "GetBitmapDimension(0x";
        size.cx = size.cy = 0;
        WowNotePut(note, noteCapacity, &noteLength, callName);
        WowNoteHex(note, noteCapacity, &noteLength, handle16, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, ")");
        if (object && frame->Id == WOWGDI_GETBITMAPDIMENSION)
        {
            if (GetObjectType(object) == OBJ_BITMAP)
                isOk = GetBitmapDimensionEx((HBITMAP)object, &size);
        }
        else if (object && (kind == WOWGDI_KIND_DC || kind == WOWGDI_KIND_WINDC))
        {
            isOk = frame->Id == WOWGDI_GETVIEWPORTEXT ? GetViewportExtEx((HDC)object, &size)
                                                : GetWindowExtEx((HDC)object, &size);
        }
        if (!isOk)
        {
            size.cx = size.cy = 0;
            WowNotePut(note, noteCapacity, &noteLength, " -- ★ not ours; 0");
        }
        else { WowNotePut(note, noteCapacity, &noteLength, " -> ");
        WowNoteHex(note, noteCapacity, &noteLength, (DWORD)size.cx, WOW_HEX_WORD_DIGITS);
               WowNotePut(note, noteCapacity, &noteLength, "x");
               WowNoteHex(note, noteCapacity, &noteLength, (DWORD)size.cy, WOW_HEX_WORD_DIGITS); }
        Wow32SetReturn(frame, ((DWORD)(WORD)size.cy << WORD_SHIFT) | (WORD)size.cx);
        return 1;
    }

    case WOWGDI_GETPALETTEENTRIES:
    {
        WORD palette16  = Wow32ArgWord(frame, WOWGDI_GPE_ARG_HPAL);
        WORD startIndex = Wow32ArgWord(frame, WOWGDI_GPE_ARG_START);
        WORD count   = Wow32ArgWord(frame, WOWGDI_GPE_ARG_COUNT);
        volatile BYTE *output = Wow32ArgPointer(frame, WOWGDI_GPE_ARG_ENTRIES);
        HPALETTE palette = (HPALETTE)WowGdiH32(palette16, NULL);
        INT noteLength = 0;
        UINT returned = 0;
        WowNotePut(note, noteCapacity, &noteLength, "GetPaletteEntries(0x");
        WowNoteHex(note, noteCapacity, &noteLength, palette16, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, ", start=");
        WowNoteHex(note, noteCapacity, &noteLength, startIndex, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, ", n=");
        WowNoteHex(note, noteCapacity, &noteLength, count, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, ")");
        if (!palette)
        {
            WowNotePut(note, noteCapacity, &noteLength, " -- ★ NOT ONE OF OUR PALETTES; answered 0");
            Wow32SetReturn(frame, 0);
            return 1;
        }
        /* [CAUTION]: A NULL buffer is not an error: it is how a caller ASKS HOW MANY
         * entries the palette has, and Win32 answers the same way.
         */
        if (!output)
        {
            returned = GetPaletteEntries(palette, 0, 0, NULL);
            WowNotePut(note, noteCapacity, &noteLength, " -- a COUNT query -> 0x");
            WowNoteHex(note, noteCapacity, &noteLength, returned, WOW_HEX_WORD_DIGITS);
            Wow32SetReturn(frame, returned);
            return 1;
        }
        if (count > WOWGDI_PALETTE_MAX)
            count = WOWGDI_PALETTE_MAX;
        {   static PALETTEENTRY paletteEntry[WOWGDI_PALETTE_MAX];
            UINT index;
            returned = GetPaletteEntries(palette, startIndex, count, paletteEntry);
            for (index = 0; index < returned; ++index)
            {
                output[index * WOWGDI_PALETTEENTRY16_SIZE] = paletteEntry[index].peRed;
                output[index * WOWGDI_PALETTEENTRY16_SIZE + WOWGDI_PE16_GREEN] = paletteEntry[index].peGreen;
                output[index * WOWGDI_PALETTEENTRY16_SIZE + WOWGDI_PE16_BLUE] = paletteEntry[index].peBlue;
                output[index * WOWGDI_PALETTEENTRY16_SIZE + WOWGDI_PE16_FLAGS] = paletteEntry[index].peFlags;
            }
        }
        WowNotePut(note, noteCapacity, &noteLength, " -> 0x");
        WowNoteHex(note, noteCapacity, &noteLength, returned, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, " entries");
        Wow32SetReturn(frame, returned);
        return 1;
    }

    case WOWGDI_ENUMOBJECTS:
    {
        WORD  dc16  = Wow32ArgWord(frame, WOWGDI_EOB_ARG_HDC);
        WORD  objectType  = Wow32ArgWord(frame, WOWGDI_EOB_ARG_TYPE);
        DWORD procedure = Wow32ArgDword(frame, WOWGDI_EOB_ARG_PROC);
        DWORD lParam   = Wow32ArgDword(frame, WOWGDI_EOB_ARG_LPARAM);
        INT   kind = -1, noteLength = 0;
        HGDIOBJ object = WowGdiH32(dc16, &kind);
        HDC   dc;
        INT   isOwned = 0;
        WowNotePut(note, noteCapacity, &noteLength, "EnumObjects(0x");
        WowNoteHex(note, noteCapacity, &noteLength, dc16, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, objectType == OBJ_PEN ? ", OBJ_PEN" :
                                   objectType == OBJ_BRUSH ? ", OBJ_BRUSH" : ", ?");
        WowNotePut(note, noteCapacity, &noteLength, ")");
        /* GDI's own 16-bit side already refuses a type outside 1..2 before the
         * call reaches us (such calls never arrive); this is belt and braces,
         * answering what that refusal answers.
         */
        if (objectType != OBJ_PEN && objectType != OBJ_BRUSH)
        {
            WowNotePut(note, noteCapacity, &noteLength, " -- not a pen or brush; 0");
            Wow32SetReturn(frame, 0);
            return 1;
        }
        Wow32SetReturn(frame, 1);                 /* the walk revises it to 0 on a stop */
        if (object && (kind == WOWGDI_KIND_DC || kind == WOWGDI_KIND_WINDC))
            dc = (HDC)object;
        else
        {
            dc = GetDC(NULL);
            isOwned = 1;
        }
        g_WowEnumFontCount = 0;
        if (dc)
        {
            EnumObjects(dc, objectType, (GOBJENUMPROC)WowGdiObjectCollect, (LPARAM)objectType);
            if (isOwned)
                ReleaseDC(NULL, dc);
        }
        WowNotePut(note, noteCapacity, &noteLength, " -> 0x");
        WowNoteHex(note, noteCapacity, &noteLength, (DWORD)g_WowEnumFontCount, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, " object(s)");
        if (!g_WowEnumFontCount || !frame->IsCallbackAllowed)
        {
            if (!frame->IsCallbackAllowed)
                WowNotePut(note, noteCapacity, &noteLength, " -- callbacks are not armed");
            if (!g_WowEnumFontCount)
                Wow32SetReturn(frame, 0);
            return 1;
        }
        if (WowEnumBusy())
        {
            WowNotePut(note, noteCapacity, &noteLength, " -- ★ AN ENUMERATION IS ALREADY RUNNING; REFUSED");
            Wow32SetReturn(frame, 0);
            return 1;
        }
        if (!WowEnumBegin(WOWENUM_OBJECTS, procedure, frame->GuestDataSelector, lParam,
                           (DWORD)(ULONG_PTR)(frame->FrameBase + WOW32_OFF_RET), 0))
        {
            WowNotePut(note, noteCapacity, &noteLength, " -- ★ the callback is not a usable far pointer");
            Wow32SetReturn(frame, 0);
            return 1;
        }
        frame->IsEnumerationRequested = 1;
        return 1;
    }

    case WOWGDI_ENUMFONTS:
    case WOWGDI_ENUMFONTFAMILIES:
    {
        INT   isPlain = (frame->Id == WOWGDI_ENUMFONTS);
        WORD  dc16  = Wow32ArgWord(frame, WOWGDI_EFF_ARG_HDC);
        DWORD procedure = Wow32ArgDword(frame, WOWGDI_EFF_ARG_PROC);
        DWORD lParam   = Wow32ArgDword(frame, WOWGDI_EFF_ARG_LPARAM);
        CHAR  family[WOWGDI_FAMILY_MAX];
        INT   hasFamily = Wow32ArgString(frame, WOWGDI_EFF_ARG_FAMILY, family, sizeof family) && family[0];
        INT   kind = -1, noteLength = 0;
        HGDIOBJ object = WowGdiH32(dc16, &kind);
        HDC   dc;
        INT   isOwned = 0;
        WowNotePut(note, noteCapacity, &noteLength, isPlain ? "EnumFonts(0x" : "EnumFontFamilies(0x");
        WowNoteHex(note, noteCapacity, &noteLength, dc16, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, hasFamily ? ", \"" : ", NULL");
        if (hasFamily)
        {
            WowNotePut(note, noteCapacity, &noteLength, family);
            WowNotePut(note, noteCapacity, &noteLength, "\"");
        }
        WowNotePut(note, noteCapacity, &noteLength, ")");
        Wow32SetReturn(frame, 1);                 /* the walk revises it to 0 on a stop */
        if (object && (kind == WOWGDI_KIND_DC || kind == WOWGDI_KIND_WINDC))
            dc = (HDC)object;
        else
        {
            dc = GetDC(NULL);
            isOwned = 1;
        }
        g_WowEnumFontCount = 0;
        if (dc)
        {
            g_WowGdiFontIsPlain = isPlain;
            if (isPlain) EnumFontsA(dc, hasFamily ? family : NULL,
                                  (FONTENUMPROCA)WowGdiFontCollect, 0);
            else
                EnumFontFamiliesA(dc, hasFamily ? family : NULL, WowGdiFontCollect, 0);
            g_WowGdiFontIsPlain = 0;
            if (isOwned)
                ReleaseDC(NULL, dc);
        }
        WowNotePut(note, noteCapacity, &noteLength, " -> 0x");
        WowNoteHex(note, noteCapacity, &noteLength, (DWORD)g_WowEnumFontCount, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, " font(s)");
        if (!g_WowEnumFontCount || !frame->IsCallbackAllowed)
        {
            if (!frame->IsCallbackAllowed)
                WowNotePut(note, noteCapacity, &noteLength, " -- callbacks are not armed");
            return 1;
        }
        if (WowEnumBusy())
        {
            WowNotePut(note, noteCapacity, &noteLength, " -- ★ AN ENUMERATION IS ALREADY RUNNING; REFUSED");
            return 1;
        }
        if (!WowEnumBegin(WOWENUM_FONTS, procedure, frame->GuestDataSelector, lParam,
                           (DWORD)(ULONG_PTR)(frame->FrameBase + WOW32_OFF_RET), 0))
        {
            WowNotePut(note, noteCapacity, &noteLength, " -- ★ the callback is not a usable far pointer");
            return 1;
        }
        frame->IsEnumerationRequested = 1;
        return 1;
    }

    case WOWGDI_LINEDDA:
    {
        INT startX = (INT)(SHORT)Wow32ArgWord(frame, WOWGDI_LDDA_ARG_X1);
        INT startY = (INT)(SHORT)Wow32ArgWord(frame, WOWGDI_LDDA_ARG_Y1);
        INT endX = (INT)(SHORT)Wow32ArgWord(frame, WOWGDI_LDDA_ARG_X2);
        INT endY = (INT)(SHORT)Wow32ArgWord(frame, WOWGDI_LDDA_ARG_Y2);
        DWORD procedure = Wow32ArgDword(frame, WOWGDI_LDDA_ARG_PROC);
        DWORD data = Wow32ArgDword(frame, WOWGDI_LDDA_ARG_DATA);
        INT noteLength = 0;
        WowNotePut(note, noteCapacity, &noteLength, "LineDDA (");
        WowNoteHex(note, noteCapacity, &noteLength, (DWORD)startX, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, ",");
        WowNoteHex(note, noteCapacity, &noteLength, (DWORD)startY, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, ")-(");
        WowNoteHex(note, noteCapacity, &noteLength, (DWORD)endX, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, ",");
        WowNoteHex(note, noteCapacity, &noteLength, (DWORD)endY, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, ") proc=0x");
        WowNoteHex(note, noteCapacity, &noteLength, procedure, WOW_HEX_DWORD_DIGITS);
        Wow32SetReturn(frame, 0);                       /* the function returns void */
        if (!frame->IsCallbackAllowed)
        {
            WowNotePut(note, noteCapacity, &noteLength, " -- callbacks are not armed; no point was"
                                       " visited");
            return 1;
        }
        if (WowEnumBusy())
        {
            WowNotePut(note, noteCapacity, &noteLength, " -- ★ AN ENUMERATION IS ALREADY RUNNING;"
                                       " REFUSED rather than sharing a cursor");
            return 1;
        }
        /* [CAUTION]: THE DS IS THE CALLER'S OWN. A LineDDA callback is application code in
         * the application's data segment, and this host has no class or window
         * to take an instance from here -- so it is entered with the DS the
         * guest itself is running on, which is what a MakeProcInstance thunk
         * would have restored anyway.
         */
        if (!WowEnumBegin(WOWENUM_LINE, procedure,
                           frame->GuestDataSelector,
                           data, 0, 0))
        {
            WowNotePut(note, noteCapacity, &noteLength, " -- ★ the callback is not a usable far"
                                       " pointer");
            return 1;
        }
        WowEnumLine(startX, startY, endX, endY);
        frame->IsEnumerationRequested = 1;
        return 1;
    }

    case WOWGDI_CREATEBITMAP:
    {
        INT  width  = (INT)(SHORT)Wow32ArgWord(frame, WOWGDI_CBM_ARG_WIDTH);
        INT  handle16  = (INT)(SHORT)Wow32ArgWord(frame, WOWGDI_CBM_ARG_HEIGHT);
        WORD planes = Wow32ArgWord(frame, WOWGDI_CBM_ARG_PLANES);
        WORD bitsPerPixel = Wow32ArgWord(frame, WOWGDI_CBM_ARG_BPP);
        volatile BYTE *bits = Wow32ArgPointer(frame, WOWGDI_CBM_ARG_BITS);
        HBITMAP bitmap;
        WORD token;
        INT  noteLength = 0;
        WowNotePut(note, noteCapacity, &noteLength, "CreateBitmap ");
        WowNoteHex(note, noteCapacity, &noteLength, (DWORD)width, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, "x");
        WowNoteHex(note, noteCapacity, &noteLength, (DWORD)handle16, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, " planes=");
        WowNoteHex(note, noteCapacity, &noteLength, planes, WOW_HEX_BYTE_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, " bpp=");
        WowNoteHex(note, noteCapacity, &noteLength, bitsPerPixel, WOW_HEX_BYTE_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, bits ? " with bits" : " uninitialised");
        if (width <= 0 || handle16 <= 0 || !planes || !bitsPerPixel)
        {
            WowNotePut(note, noteCapacity, &noteLength, " -- ★ A DIMENSION OR FORMAT IS NOT"
                                       " POSITIVE; answered 0");
            Wow32SetReturn(frame, 0);
            return 1;
        }
        if (bits)
        {
            DWORD stride = ((((DWORD)width * planes * bitsPerPixel) + (WOWGDI_WORD_BITS - 1)) / WOWGDI_WORD_BITS) * WOW_WORD_BYTES;
            if (stride * (DWORD)handle16 > 0x10000ul)
            {
                WowNotePut(note, noteCapacity, &noteLength, " -- ★ THE BITS WOULD RUN PAST A 64K"
                                           " SEGMENT; refused rather than letting"
                                           " GDI read past them; answered 0");
                Wow32SetReturn(frame, 0);
                return 1;
            }
        }
        bitmap = CreateBitmap(width, handle16, (UINT)planes, (UINT)bitsPerPixel,
                          bits ? (const VOID *)(PCBYTE )bits : NULL);
        token = bitmap ? WowGdiH16((HGDIOBJ)bitmap, WOWGDI_KIND_OBJ) : 0;
        if (!token)
        {
            if (bitmap)
                DeleteObject((HGDIOBJ)bitmap);
            WowNotePut(note, noteCapacity, &noteLength, bitmap ? " -- ★ THE GDI TOKEN MAP IS FULL; the"
                                            " bitmap was freed and 0 answered"
                                          : " -- ★ THE OS REFUSED IT; answered 0");
            Wow32SetReturn(frame, 0);
            return 1;
        }
        WowNotePut(note, noteCapacity, &noteLength, " -> bitmap token 0x");
        WowNoteHex(note, noteCapacity, &noteLength, token, WOW_HEX_WORD_DIGITS);
        Wow32SetReturn(frame, token);
        return 1;
    }

    /* 0x33 CreateCompatibleBitmap(hDC, nWidth, nHeight) (Importance = 3):
     * MS Paint's canvas itself, and the allocation whose failure it reports as
     * "Not enough memory to edit image".
     *
     * [CAUTION]: THE DIMENSIONS ARE SIGNED 16-BIT. A Win16 program passes ints, and a
     * negative or zero one must not be widened into a huge Win32 request --
     * that would either fail obscurely or succeed at a size nobody asked for.
     *
     * [CAUTION]: AND IT IS COMPATIBLE WITH THE hDC IT IS GIVEN, which is why passing NULL
     * here is NOT the harmless default it is for CreateCompatibleDC: a bitmap
     * compatible with nothing is a 1x1 monochrome one, and a guest that drew
     * into it would get a black-and-white canvas and no explanation. So a null
     * or unknown DC is refused.
     */
    case WOWGDI_CREATECOMPATBM:
    {
        WORD dc16 = Wow32ArgWord(frame, WOWGDI_CCB_ARG_HDC);
        INT  width   = (INT)(SHORT)Wow32ArgWord(frame, WOWGDI_CCB_ARG_WIDTH);
        INT  handle16   = (INT)(SHORT)Wow32ArgWord(frame, WOWGDI_CCB_ARG_HEIGHT);
        INT  kind = -1;
        HGDIOBJ object = WowGdiH32(dc16, &kind);
        HBITMAP bitmap;
        WORD token;
        INT noteLength = 0;
        WowNotePut(note, noteCapacity, &noteLength, "CreateCompatibleBitmap(0x");
        WowNoteHex(note, noteCapacity, &noteLength, dc16, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, ", ");
        WowNoteHex(note, noteCapacity, &noteLength, (DWORD)width, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, "x");
        WowNoteHex(note, noteCapacity, &noteLength, (DWORD)handle16, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, ")");
        if (!object || (kind != WOWGDI_KIND_DC && kind != WOWGDI_KIND_WINDC))
        {
            WowNotePut(note, noteCapacity, &noteLength, " -- ★ THAT IS NOT ONE OF OUR DC TOKENS;"
                                       " answered 0");
            Wow32SetReturn(frame, 0);
            return 1;
        }
        if (width <= 0 || handle16 <= 0)
        {
            WowNotePut(note, noteCapacity, &noteLength, " -- ★ A DIMENSION IS NOT POSITIVE;"
                                       " answered 0 rather than widening it into"
                                       " a size nobody asked for");
            Wow32SetReturn(frame, 0);
            return 1;
        }
        bitmap = CreateCompatibleBitmap((HDC)object, width, handle16);
        token = bitmap ? WowGdiH16((HGDIOBJ)bitmap, WOWGDI_KIND_OBJ) : 0;
        if (!token)
        {
            if (bitmap)
                DeleteObject((HGDIOBJ)bitmap);
            WowNotePut(note, noteCapacity, &noteLength, bitmap ? " -- ★ THE GDI TOKEN MAP IS FULL; the"
                                            " bitmap was freed and 0 answered"
                                          : " -- ★ THE OS REFUSED IT; answered 0");
            Wow32SetReturn(frame, 0);
            return 1;
        }
        WowNotePut(note, noteCapacity, &noteLength, " -> bitmap token 0x");
        WowNoteHex(note, noteCapacity, &noteLength, token, WOW_HEX_WORD_DIGITS);
        Wow32SetReturn(frame, token);
        return 1;
    }

    /* 0x42 CreateSolidBrush(crColor) (Importance = 1):
     * A COLORREF is a DWORD and means the same thing in both worlds.
     */
    case WOWGDI_CREATESOLIDBRUSH:
    {
        DWORD color = Wow32ArgDword(frame, WOWGDI_CSB_ARG_COLOR);
        HBRUSH brush = CreateSolidBrush((COLORREF)color);
        WORD token = brush ? WowGdiH16((HGDIOBJ)brush, WOWGDI_KIND_OBJ) : 0;
        INT noteLength = 0;
        WowNotePut(note, noteCapacity, &noteLength, "CreateSolidBrush(0x");
        WowNoteHex(note, noteCapacity, &noteLength, color, WOW_HEX_DWORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, ")");
        if (!token)
        {
            if (brush)
                DeleteObject((HGDIOBJ)brush);
            WowNotePut(note, noteCapacity, &noteLength, " -- ★ refused or the token map is full;"
                                       " answered 0");
            Wow32SetReturn(frame, 0);
            return 1;
        }
        WowNotePut(note, noteCapacity, &noteLength, " -> brush token 0x");
        WowNoteHex(note, noteCapacity, &noteLength, token, WOW_HEX_WORD_DIGITS);
        Wow32SetReturn(frame, token);
        return 1;
    }

    /* 0x2d SelectObject(hDC, hObject) (Importance = 3):
     * The call that puts the canvas bitmap into the memory DC, and the reason
     * the two above are worth anything.
     *
     * [CAUTION]: IT RETURNS THE OBJECT THAT WAS THERE BEFORE, and that has to come back
     * as a token too -- guests keep it and select it back before deleting the
     * DC, which is the documented way to avoid destroying a bitmap that is
     * still selected. A truncated HGDIOBJ here would be handed straight back
     * to us later and would name nothing.
     *
     * [INFO]: The previous object is usually one we never issued (the 1x1 bitmap a new
     * memory DC starts with), so it is minted on the spot as an ordinary
     * object. That is correct: from the guest's side it is simply a handle to
     * give back, and the map now knows it if it returns.
     */
    case WOWGDI_SELECTOBJECT:
    {
        WORD dc16 = Wow32ArgWord(frame, WOWGDI_SEL_ARG_HDC);
        WORD object16 = Wow32ArgWord(frame, WOWGDI_SEL_ARG_OBJ);
        INT  dcKind = -1, objectKind = -1;
        HGDIOBJ dcObject = WowGdiH32(dc16, &dcKind);
        HGDIOBJ object = WowGdiH32(object16, &objectKind);
        HGDIOBJ previous;
        WORD token;
        INT noteLength = 0;
        WowNotePut(note, noteCapacity, &noteLength, "SelectObject(dc 0x");
        WowNoteHex(note, noteCapacity, &noteLength, dc16, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, ", obj 0x");
        WowNoteHex(note, noteCapacity, &noteLength, object16, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, ")");
        if (!dcObject || (dcKind != WOWGDI_KIND_DC && dcKind != WOWGDI_KIND_WINDC))
        {
            WowNotePut(note, noteCapacity, &noteLength, " -- ★ THE DC IS NOT ONE OF OUR TOKENS;"
                                       " answered 0");
            Wow32SetReturn(frame, 0);
            return 1;
        }
        if (!object)
        {
            WowNotePut(note, noteCapacity, &noteLength, " -- ★ THE OBJECT IS NOT ONE OF OUR TOKENS;"
                                       " answered 0");
            Wow32SetReturn(frame, 0);
            return 1;
        }
        if (objectKind == WOWGDI_KIND_DC || objectKind == WOWGDI_KIND_WINDC)
        {
            WowNotePut(note, noteCapacity, &noteLength, " -- ★ THAT IS A DC, NOT A DRAWING OBJECT;"
                                       " refused");
            Wow32SetReturn(frame, 0);
            return 1;
        }
        previous = SelectObject((HDC)dcObject, object);
        if (!previous)
        {
            WowNotePut(note, noteCapacity, &noteLength, " -- ★ THE OS REFUSED THE SELECTION;"
                                       " answered 0");
            Wow32SetReturn(frame, 0);
            return 1;
        }
        token = WowGdiH16(previous, WOWGDI_KIND_OBJ);
        WowNotePut(note, noteCapacity, &noteLength, " -> previous 0x");
        WowNoteHex(note, noteCapacity, &noteLength, token, WOW_HEX_WORD_DIGITS);
        if (!token)
            WowNotePut(note, noteCapacity, &noteLength, " -- ⚠ THE MAP IS FULL, so the guest cannot"
                                       " select it back");
        Wow32SetReturn(frame, token);
        return 1;
    }

    /* 0x52 GetObject -- see the long note above (Importance = 3): */
    case WOWGDI_GETOBJECT:
    {
        WORD handle16    = Wow32ArgWord(frame, WOWGDI_GOB_ARG_HANDLE);
        WORD wanted = Wow32ArgWord(frame, WOWGDI_GOB_ARG_COUNT);
        volatile BYTE *destination = Wow32ArgPointer(frame, WOWGDI_GOB_ARG_BUF);
        INT kind = -1;
        HGDIOBJ object = WowGdiH32(handle16, &kind);
        BYTE blob[WOWGDI_LOGFONT16_SIZE];
        DWORD type;
        INT itemCount = 0, noteLength = 0, index;
        PCSTR description = "?";

        WowNotePut(note, noteCapacity, &noteLength, "GetObject 0x");
        WowNoteHex(note, noteCapacity, &noteLength, handle16, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, " cb=0x");
        WowNoteHex(note, noteCapacity, &noteLength, wanted, WOW_HEX_WORD_DIGITS);
        if (!object)
        {
            WowNotePut(note, noteCapacity, &noteLength, " -- ★ NOT ONE OF OUR GDI TOKENS;"
                                       " answered 0");
            Wow32SetReturn(frame, 0);
            return 1;
        }
        for (index = 0; index < (INT)sizeof blob; ++index)
            blob[index] = 0;
        type = GetObjectType(object);
        if (type == OBJ_BITMAP)
        {
            BITMAP bitmap;
            if (!GetObject(object, (INT)sizeof bitmap, &bitmap))
                type = 0;
            else
            {
                description = "BITMAP";
                itemCount = WOWGDI_BITMAP16_SIZE;
                Wow32PokeWord(blob + 0,  (WORD)(SHORT)bitmap.bmType);
                Wow32PokeWord(blob + WOWGDI_CBI_OFF_WIDTH,  (WORD)(SHORT)bitmap.bmWidth);
                Wow32PokeWord(blob + WOWGDI_CBI_OFF_HEIGHT,  (WORD)(SHORT)bitmap.bmHeight);
                Wow32PokeWord(blob + WOWGDI_CBI_OFF_WBYTES,  (WORD)(SHORT)bitmap.bmWidthBytes);
                blob[WOWGDI_CBI_OFF_PLANES]  = (BYTE)bitmap.bmPlanes;
                /* [CAUTION]: REFUTED, session 45. We report bmBitsPixel as the OS
                 * gives it -- 0x20 on this rig -- and 32bpp is a depth Win16
                 * never had (it knew 1/4/8/16/24), and this is the ONLY
                 * pixel-format number MS Paint can see. Reporting 24 instead
                 * changed NOTHING: still 33 bitmaps at `planes=1 bpp=1`. So
                 * Paint's monochrome off-screen bitmaps do not come from here
                 * either. The answer is left as the OS gives it.
                 */
                blob[WOWGDI_CBI_OFF_BPP]  = (BYTE)bitmap.bmBitsPixel;
                /* [INFO]: THE FORMAT, NOT JUST THE TYPE. A guest can only learn a
                 * pixel format from here (MS Paint never asks GetDeviceCaps
                 * for BITSPIXEL or PLANES), so these two numbers are the only
                 * thing that can tell it whether to make a colour or a
                 * monochrome off-screen bitmap -- which is exactly the open
                 * question about its colour palette.
                 */
                {   WowNotePut(note, noteCapacity, &noteLength, " ");
                    WowNoteHex(note, noteCapacity, &noteLength, (DWORD)(WORD)bitmap.bmWidth, WOW_HEX_WORD_DIGITS);
                    WowNotePut(note, noteCapacity, &noteLength, "x");
                    WowNoteHex(note, noteCapacity, &noteLength, (DWORD)(WORD)bitmap.bmHeight, WOW_HEX_WORD_DIGITS);
                    WowNotePut(note, noteCapacity, &noteLength, " planes=");
                    WowNoteHex(note, noteCapacity, &noteLength, (DWORD)bitmap.bmPlanes, WOW_HEX_BYTE_DIGITS);
                    WowNotePut(note, noteCapacity, &noteLength, " bpp=");
                    WowNoteHex(note, noteCapacity, &noteLength, (DWORD)bitmap.bmBitsPixel, WOW_HEX_BYTE_DIGITS);
                }
                /* [CAUTION]: bmBits STAYS NULL, and that is correct rather than lazy: it
                 * is a 32-bit host pointer with no 16-bit address, and Windows
                 * itself returns NULL here for a device-dependent bitmap.
                 */
            }
        }
        else if (type == OBJ_FONT)
        {
            LOGFONTA logFont;
            if (!GetObjectA(object, (INT)sizeof logFont, &logFont))
                type = 0;
            else
            {
                description = "LOGFONT";
                itemCount = WOWGDI_LOGFONT16_SIZE;
                Wow32PokeWord(blob + 0,  (WORD)(SHORT)logFont.lfHeight);
                Wow32PokeWord(blob + WOWGDI_LF16_WIDTH,  (WORD)(SHORT)logFont.lfWidth);
                Wow32PokeWord(blob + WOWGDI_LF16_ESCAPEMENT,  (WORD)(SHORT)logFont.lfEscapement);
                Wow32PokeWord(blob + WOWGDI_LF16_ORIENTATION,  (WORD)(SHORT)logFont.lfOrientation);
                Wow32PokeWord(blob + WOWGDI_LF16_WEIGHT,  (WORD)(SHORT)logFont.lfWeight);
                blob[WOWGDI_LF16_ITALIC] = logFont.lfItalic;
                blob[WOWGDI_LF16_UNDERLINE] = logFont.lfUnderline;
                blob[WOWGDI_LF16_STRIKEOUT] = logFont.lfStrikeOut;
                blob[WOWGDI_LF16_CHARSET] = logFont.lfCharSet;
                blob[WOWGDI_LF16_OUTPRECISION] = logFont.lfOutPrecision;
                blob[WOWGDI_LF16_CLIPPRECISION] = logFont.lfClipPrecision;
                blob[WOWGDI_LF16_QUALITY] = logFont.lfQuality;
                blob[WOWGDI_LF16_PITCHANDFAMILY] = logFont.lfPitchAndFamily;
                for (index = 0; index < WOWGDI_LF16_FACESIZE && logFont.lfFaceName[index]; ++index)
                    blob[WOWGDI_LF16_FACENAME + index] = (BYTE)logFont.lfFaceName[index];
                /* [INFO]: THE FIELDS, NOT JUST THE TYPE. MS Paint sizes its whole
                 * toolbox from the system font's metrics, so lfHeight is
                 * load-bearing geometry and a log that only says "LOGFONT"
                 * cannot be compared against an oracle.
                 */
                {   INT innerIndex = 0;
                    for (innerIndex = 0; innerIndex < WOWGDI_LF16_FACESIZE && logFont.lfFaceName[innerIndex]; ++innerIndex)
                    {
                    }
                    WowNotePut(note, noteCapacity, &noteLength, " h=");
                    WowNoteHex(note, noteCapacity, &noteLength, (DWORD)(WORD)(SHORT)logFont.lfHeight, WOW_HEX_WORD_DIGITS);
                    WowNotePut(note, noteCapacity, &noteLength, " w=");
                    WowNoteHex(note, noteCapacity, &noteLength, (DWORD)(WORD)(SHORT)logFont.lfWidth, WOW_HEX_WORD_DIGITS);
                    WowNotePut(note, noteCapacity, &noteLength, " wt=");
                    WowNoteHex(note, noteCapacity, &noteLength, (DWORD)(WORD)(SHORT)logFont.lfWeight, WOW_HEX_WORD_DIGITS);
                    WowNotePut(note, noteCapacity, &noteLength, " ");
                    WowNoteQuoted(note, noteCapacity, &noteLength, logFont.lfFaceName);
                }
            }
        }
        else if (type == OBJ_PEN || type == OBJ_EXTPEN)
        {
            LOGPEN logPen;
            if (!GetObject(object, (INT)sizeof logPen, &logPen))
                type = 0;
            else
            {
                description = "LOGPEN";
                itemCount = WOWGDI_LOGPEN16_SIZE;
                Wow32PokeWord(blob + 0, (WORD)logPen.lopnStyle);
                Wow32PokeWord(blob + WOWGDI_LP16_WIDTH_X, (WORD)(SHORT)logPen.lopnWidth.x);
                Wow32PokeWord(blob + WOWGDI_LP16_WIDTH_Y, (WORD)(SHORT)logPen.lopnWidth.y);
                Wow32PokeWord(blob + WOWGDI_LP16_COLOR, (WORD)(logPen.lopnColor & WORD_MASK));
                Wow32PokeWord(blob + WOWGDI_LP16_COLOR + WOW_WORD_BYTES, (WORD)(logPen.lopnColor >> WORD_SHIFT));
            }
        }
        else if (type == OBJ_BRUSH)
        {
            LOGBRUSH logBrush;
            if (!GetObject(object, (INT)sizeof logBrush, &logBrush))
                type = 0;
            else
            {
                description = "LOGBRUSH";
                itemCount = WOWGDI_LOGBRUSH16_SIZE;
                Wow32PokeWord(blob + 0, (WORD)logBrush.lbStyle);
                Wow32PokeWord(blob + WOWGDI_LB16_COLOR, (WORD)(logBrush.lbColor & WORD_MASK));
                Wow32PokeWord(blob + WOWGDI_LB16_COLOR + WOW_WORD_BYTES, (WORD)(logBrush.lbColor >> WORD_SHIFT));
                Wow32PokeWord(blob + WOWGDI_LB16_HATCH, (WORD)logBrush.lbHatch);
            }
        }
        else
        {
            type = 0;
        }
        if (!type || !itemCount)
        {
            WowNotePut(note, noteCapacity, &noteLength, " -- ★ THE OS WILL NOT DESCRIBE THAT OBJECT"
                                       " (not a bitmap, font, pen or brush);"
                                       " answered 0");
            Wow32SetReturn(frame, 0);
            return 1;
        }
        WowNotePut(note, noteCapacity, &noteLength, " -> ");
        WowNotePut(note, noteCapacity, &noteLength, description);
        WowNotePut(note, noteCapacity, &noteLength, " (Win16 form is 0x");
        WowNoteHex(note, noteCapacity, &noteLength, (DWORD)itemCount, WOW_HEX_BYTE_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, " bytes)");
        /* [INFO]: A NULL buffer is a legal QUERY: Win16 answers the size needed. */
        if (!destination)
        {
            WowNotePut(note, noteCapacity, &noteLength, " -- lpObject is NULL, so this is a size"
                                       " query");
            Wow32SetReturn(frame, (DWORD)itemCount);
            return 1;
        }
        if ((INT)wanted < itemCount)
        {
            itemCount = (INT)wanted;                 /* a partial read is allowed */
            WowNotePut(note, noteCapacity, &noteLength, " -- PARTIAL, the guest asked for less");
        }
        for (index = 0; index < itemCount; ++index)
            destination[index] = blob[index];
        Wow32SetReturn(frame, (DWORD)itemCount);
        return 1;
    }

    /* 0x44 DeleteDC / 0x45 DeleteObject (Importance = 1):
     *
     * [CAUTION]: THE TOKEN IS FORGOTTEN AS WELL AS THE OBJECT DELETED. A Win32 handle is
     * reusable once freed, so a token left pointing at a dead HGDIOBJ would
     * eventually name somebody else's object -- the same hazard DestroyWindow
     * has, and the same answer.
     *
     * [CAUTION]: AND THE TWO ARE NOT INTERCHANGEABLE: DeleteObject on a DC leaks it and
     * DeleteDC on a brush fails, so the map records which it issued and this
     * says so rather than passing the mistake to Win32.
     */
    case WOWGDI_DELETEDC:
    case WOWGDI_DELETEOBJECT:
    {
        INT  isDeleteDc = (frame->Id == WOWGDI_DELETEDC);
        WORD handle16 = Wow32ArgWord(frame, WOWGDI_DOBJ_ARG_HANDLE);
        INT  kind = -1;
        HGDIOBJ object = WowGdiH32(handle16, &kind);
        INT noteLength = 0, isOk;
        INT wantedKind = isDeleteDc ? WOWGDI_KIND_DC : WOWGDI_KIND_OBJ;
        WowNotePut(note, noteCapacity, &noteLength, isDeleteDc ? "DeleteDC 0x" : "DeleteObject 0x");
        WowNoteHex(note, noteCapacity, &noteLength, handle16, WOW_HEX_WORD_DIGITS);
        if (!object)
        {
            WowNotePut(note, noteCapacity, &noteLength, " -- ★ NOT ONE OF OUR GDI TOKENS;"
                                       " answered 0");
            Wow32SetReturn(frame, 0);
            return 1;
        }
        /* [CAUTION]: A STOCK OBJECT IS NOT DESTROYED, AND THE CALL STILL SUCCEEDS. The
         * system owns it; Windows makes DeleteObject on one a no-op that
         * reports success, and answering anything else would make a guest
         * think its cleanup had failed. The token is KEPT, because the object
         * is still there and the guest may well ask for it again.
         */
        if (kind == WOWGDI_KIND_STOCK)
        {
            WowNotePut(note, noteCapacity, &noteLength, " -- ★ that is a STOCK object; the system"
                                       " owns it, so nothing was destroyed and"
                                       " success answered (as Windows does)");
            Wow32SetReturn(frame, 1);
            return 1;
        }
        /* [CAUTION]: A BORROWED DC IS THE INTERESTING REFUSAL. DeleteDC on a DC that came
         * from GetDC does not fail visibly on Win32 -- it damages the window's
         * DC cache and the symptom surfaces somewhere else entirely. Say which
         * call it should have been.
         */
        if (kind != wantedKind)
        {
            WowNotePut(note, noteCapacity, &noteLength,
                    kind == WOWGDI_KIND_WINDC
                        ? " -- ★ THAT DC WAS BORROWED FROM A WINDOW (GetDC);"
                          " it must go back through ReleaseDC, not this. Refused"
                    : kind == WOWGDI_KIND_DC
                        ? " -- ★ THAT IS A DC AND THIS IS DeleteObject; refused"
                        : " -- ★ THAT IS NOT A DC AND THIS IS DeleteDC; refused");
            Wow32SetReturn(frame, 0);
            return 1;
        }
        isOk = isDeleteDc ? (DeleteDC((HDC)object) ? 1 : 0) : (DeleteObject(object) ? 1 : 0);
        if (isOk)
            WowGdiForget(handle16);
        WowNotePut(note, noteCapacity, &noteLength, isOk ? " -> deleted, token released"
                                      : " -- ★ the OS refused the delete");
        Wow32SetReturn(frame, (DWORD)isOk);
        return 1;
    }

    /* 0x18 Ellipse / 0x15 ExcludeClipRect -- Rectangle's block, twice (Importance = 2):
     * Both are (hDC, left, top, right, bottom) in 10 bytes, which is what lets
     * them share RC_ARG_*. [CAUTION] They are NOT the same kind of call and the note
     * says which is which: Ellipse draws, ExcludeClipRect changes what any
     * later call is allowed to touch, and its answer is a region COMPLEXITY
     * code (NULLREGION/SIMPLEREGION/COMPLEXREGION), not a boolean.
     */
    case WOWGDI_ELLIPSE:
    case WOWGDI_EXCLUDECLIPRECT:
    {
        INT  isEllipse = (frame->Id == WOWGDI_ELLIPSE);
        WORD dc16 = Wow32ArgWord(frame, WOWGDI_RC_ARG_HDC);
        INT  left = (INT)(SHORT)Wow32ArgWord(frame, WOWGDI_RC_ARG_LEFT);
        INT  top = (INT)(SHORT)Wow32ArgWord(frame, WOWGDI_RC_ARG_TOP);
        INT  right = (INT)(SHORT)Wow32ArgWord(frame, WOWGDI_RC_ARG_RIGHT);
        INT  bottom = (INT)(SHORT)Wow32ArgWord(frame, WOWGDI_RC_ARG_BOTTOM);
        INT  kind = -1;
        HGDIOBJ object = WowGdiH32(dc16, &kind);
        INT  noteLength = 0;
        WowNotePut(note, noteCapacity, &noteLength, isEllipse ? "Ellipse(0x" : "ExcludeClipRect(0x");
        WowNoteHex(note, noteCapacity, &noteLength, dc16, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, ", ");
        WowNoteHex(note, noteCapacity, &noteLength, (DWORD)left, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, ",");
        WowNoteHex(note, noteCapacity, &noteLength, (DWORD)top, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, " ");
        WowNoteHex(note, noteCapacity, &noteLength, (DWORD)right, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, ",");
        WowNoteHex(note, noteCapacity, &noteLength, (DWORD)bottom, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, ")");
        if (!object || (kind != WOWGDI_KIND_DC && kind != WOWGDI_KIND_WINDC))
        {
            WowNotePut(note, noteCapacity, &noteLength, " -- ★ NOT ONE OF OUR DC TOKENS;"
                                       " answered 0");
            Wow32SetReturn(frame, 0);
            return 1;
        }
        if (isEllipse)
        {
            Wow32SetReturn(frame, (DWORD)(Ellipse((HDC)object, left, top, right, bottom) ? 1 : 0));
        }
        else
        {
            INT regionResult = ExcludeClipRect((HDC)object, left, top, right, bottom);
            WowNotePut(note, noteCapacity, &noteLength, " -> region complexity ");
            WowNoteHex(note, noteCapacity, &noteLength, (DWORD)regionResult, WOW_HEX_WORD_DIGITS);
            Wow32SetReturn(frame, (DWORD)(WORD)regionResult);
        }
        return 1;
    }

    /* 0x1c RoundRect(hDC, l, t, r, b, ellipseW, ellipseH) -- 14 bytes (Importance = 1): */
    case WOWGDI_ROUNDRECT:
    {
        WORD dc16 = Wow32ArgWord(frame, WOWGDI_RR_ARG_HDC);
        INT  left  = (INT)(SHORT)Wow32ArgWord(frame, WOWGDI_RR_ARG_LEFT);
        INT  top  = (INT)(SHORT)Wow32ArgWord(frame, WOWGDI_RR_ARG_TOP);
        INT  right  = (INT)(SHORT)Wow32ArgWord(frame, WOWGDI_RR_ARG_RIGHT);
        INT  bottom  = (INT)(SHORT)Wow32ArgWord(frame, WOWGDI_RR_ARG_BOTTOM);
        INT  ellipseWidth = (INT)(SHORT)Wow32ArgWord(frame, WOWGDI_RR_ARG_EW);
        INT  ellipseHeight = (INT)(SHORT)Wow32ArgWord(frame, WOWGDI_RR_ARG_EH);
        INT  kind = -1;
        HGDIOBJ object = WowGdiH32(dc16, &kind);
        INT  noteLength = 0;
        WowNotePut(note, noteCapacity, &noteLength, "RoundRect(0x");
        WowNoteHex(note, noteCapacity, &noteLength, dc16, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, ", ");
        WowNoteHex(note, noteCapacity, &noteLength, (DWORD)left, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, ",");
        WowNoteHex(note, noteCapacity, &noteLength, (DWORD)top, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, " ");
        WowNoteHex(note, noteCapacity, &noteLength, (DWORD)right, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, ",");
        WowNoteHex(note, noteCapacity, &noteLength, (DWORD)bottom, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, " corner ");
        WowNoteHex(note, noteCapacity, &noteLength, (DWORD)ellipseWidth, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, "x");
        WowNoteHex(note, noteCapacity, &noteLength, (DWORD)ellipseHeight, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, ")");
        if (!object || (kind != WOWGDI_KIND_DC && kind != WOWGDI_KIND_WINDC))
        {
            WowNotePut(note, noteCapacity, &noteLength, " -- ★ NOT ONE OF OUR DC TOKENS;"
                                       " answered 0");
            Wow32SetReturn(frame, 0);
            return 1;
        }
        Wow32SetReturn(frame, (DWORD)(RoundRect((HDC)object, left, top, right, bottom, ellipseWidth, ellipseHeight) ? 1 : 0));
        return 1;
    }

    /* 0x174 ExtFloodFill -- THE FILL TOOL (Importance = 5):
     *
     * [CAUTION]: THE FAILURE MODE THIS REPLACES IS SILENCE. Unimplemented, the call was
     * stepped over and answered with the harness sentinel 0 -- which is
     * ExtFloodFill's own "I filled nothing", so Paint had no way to tell a
     * host that cannot fill from a fill that had nothing to do. The tool
     * appeared to be selected, appeared to take the click, and did nothing.
     *
     * [INFO]: Paint calls it twice per fill: once with a solid brush selected and once
     * with a pattern brush from `CreatePatternBrush`, which is
     * how a Win16 program fills with one of the palette's patterns.
     */
    case WOWGDI_EXTFLOODFILL:
    {
        WORD  dc16  = Wow32ArgWord(frame, WOWGDI_FF_ARG_HDC);
        INT   positionX    = (INT)(SHORT)Wow32ArgWord(frame, WOWGDI_FF_ARG_X);
        INT   positionY    = (INT)(SHORT)Wow32ArgWord(frame, WOWGDI_FF_ARG_Y);
        DWORD color  = Wow32ArgDword(frame, WOWGDI_FF_ARG_COLOR);
        WORD  type = Wow32ArgWord(frame, WOWGDI_FF_ARG_TYPE);
        INT   kind = -1;
        HGDIOBJ object = WowGdiH32(dc16, &kind);
        INT   noteLength = 0, isOk;
        WowNotePut(note, noteCapacity, &noteLength, "ExtFloodFill(0x");
        WowNoteHex(note, noteCapacity, &noteLength, dc16, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, ", ");
        WowNoteHex(note, noteCapacity, &noteLength, (DWORD)positionX, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, ",");
        WowNoteHex(note, noteCapacity, &noteLength, (DWORD)positionY, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, " colour 0x");
        WowNoteHex(note, noteCapacity, &noteLength, color, WOW_HEX_DWORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, type ? " SURFACE)" : " BORDER)");
        if (!object || (kind != WOWGDI_KIND_DC && kind != WOWGDI_KIND_WINDC))
        {
            WowNotePut(note, noteCapacity, &noteLength, " -- ★ NOT ONE OF OUR DC TOKENS;"
                                       " answered 0");
            Wow32SetReturn(frame, 0);
            return 1;
        }
        isOk = ExtFloodFill((HDC)object, positionX, positionY, (COLORREF)color, (UINT)type) ? 1 : 0;
        WowNotePut(note, noteCapacity, &noteLength, isOk ? " -> filled" : " -> the OS filled NOTHING");
        Wow32SetReturn(frame, (DWORD)isOk);
        return 1;
    }

    /* 0x3d CreatePen -- THE CALL EVERY SHAPE TOOL WAITED ON (Importance = 5):
     * (nPenStyle, nWidth, crColor), 8 bytes, and the two the run made say so:
     * `(000000ff 0002 0006)` for the red box and `(0000ff00 0002 0006)` for
     * the green ellipse -- style 6 is `PS_INSIDEFRAME`, which is exactly what
     * a paint program wants so a thick outline stays inside the rectangle the
     * user dragged. [CAUTION] Both the style numbering and the COLORREF are unchanged
     * between Win16 and Win32, so nothing here is a translation.
     *
     * [INFO]: 0x3a CreateHatchBrush(nIndex, crColor) is its sibling in every respect
     * and is what the filled-shape and pattern tools ask for next; same
     * indices (HS_HORIZONTAL..HS_DIAGCROSS), same COLORREF.
     */
    case WOWGDI_CREATEPEN:
    case WOWGDI_CREATEHATCHBRUSH:
    {
        INT   isPen = (frame->Id == WOWGDI_CREATEPEN);
        DWORD color   = Wow32ArgDword(frame, isPen ? WOWGDI_CP_ARG_COLOR : WOWGDI_CH_ARG_COLOR);
        INT   width     = (INT)(SHORT)Wow32ArgWord(frame, isPen ? WOWGDI_CP_ARG_WIDTH : WOWGDI_CH_ARG_INDEX);
        INT   style = isPen ? (INT)(SHORT)Wow32ArgWord(frame, WOWGDI_CP_ARG_STYLE) : 0;
        INT   noteLength = 0;
        HGDIOBJ createdObject;
        WORD token;
        WowNotePut(note, noteCapacity, &noteLength, isPen ? "CreatePen(style " : "CreateHatchBrush(index ");
        WowNoteHex(note, noteCapacity, &noteLength, (DWORD)(isPen ? style : width), WOW_HEX_WORD_DIGITS);
        if (isPen)
        {
            WowNotePut(note, noteCapacity, &noteLength, ", width ");
            WowNoteHex(note, noteCapacity, &noteLength, (DWORD)width, WOW_HEX_WORD_DIGITS);
        }
        WowNotePut(note, noteCapacity, &noteLength, ", 0x");
        WowNoteHex(note, noteCapacity, &noteLength, color, WOW_HEX_DWORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, ")");
        createdObject = isPen ? (HGDIOBJ)CreatePen(style, width, (COLORREF)color)
                    : (HGDIOBJ)CreateHatchBrush(width, (COLORREF)color);
        token = createdObject ? WowGdiH16(createdObject, WOWGDI_KIND_OBJ) : 0;
        if (!token)
        {
            if (createdObject)
                DeleteObject(createdObject);
            WowNotePut(note, noteCapacity, &noteLength, " -- ★ the OS refused it (or the token map"
                                       " is full); answered 0");
            Wow32SetReturn(frame, 0);
            return 1;
        }
        WowNotePut(note, noteCapacity, &noteLength, isPen ? " -> pen token 0x" : " -> brush token 0x");
        WowNoteHex(note, noteCapacity, &noteLength, token, WOW_HEX_WORD_DIGITS);
        Wow32SetReturn(frame, token);
        return 1;
    }

    /* 0x1f SetPixel(hDC, x, y, crColor) -- GetPixel's twin, 10 bytes (Importance = 1):
     *
     * [CAUTION]: The answer is the colour ACTUALLY set, which on a device that cannot
     * represent the request is not the colour asked for -- so it is returned
     * as the OS gives it rather than echoed.
     */
    case WOWGDI_SETPIXEL:
    {
        WORD  dc16 = Wow32ArgWord(frame, WOWGDI_SP_ARG_HDC);
        INT   positionX = (INT)(SHORT)Wow32ArgWord(frame, WOWGDI_SP_ARG_X);
        INT   positionY = (INT)(SHORT)Wow32ArgWord(frame, WOWGDI_SP_ARG_Y);
        DWORD color = Wow32ArgDword(frame, WOWGDI_SP_ARG_COLOR);
        INT   kind = -1;
        HGDIOBJ object = WowGdiH32(dc16, &kind);
        INT   noteLength = 0;
        COLORREF actual;
        WowNotePut(note, noteCapacity, &noteLength, "SetPixel(0x");
        WowNoteHex(note, noteCapacity, &noteLength, dc16, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, ", ");
        WowNoteHex(note, noteCapacity, &noteLength, (DWORD)positionX, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, ",");
        WowNoteHex(note, noteCapacity, &noteLength, (DWORD)positionY, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, " 0x");
        WowNoteHex(note, noteCapacity, &noteLength, color, WOW_HEX_DWORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, ")");
        if (!object || (kind != WOWGDI_KIND_DC && kind != WOWGDI_KIND_WINDC))
        {
            WowNotePut(note, noteCapacity, &noteLength, " -- ★ NOT ONE OF OUR DC TOKENS;"
                                       " answered CLR_INVALID");
            Wow32SetReturn(frame, WOWGDI_CLR_INVALID);
            return 1;
        }
        actual = SetPixel((HDC)object, positionX, positionY, (COLORREF)color);
        Wow32SetReturn(frame, (DWORD)actual);
        return 1;
    }

    /* 0x3c CreatePatternBrush(hBitmap) -- one of OUR bitmap tokens (Importance = 1): */
    case WOWGDI_CREATEPATTERNBRUSH:
    {
        WORD bitmap16 = Wow32ArgWord(frame, WOWGDI_ONE_ARG_HANDLE);
        INT  kind = -1;
        HGDIOBJ object = WowGdiH32(bitmap16, &kind);
        INT  noteLength = 0;
        HBRUSH brush;
        WORD token;
        WowNotePut(note, noteCapacity, &noteLength, "CreatePatternBrush(0x");
        WowNoteHex(note, noteCapacity, &noteLength, bitmap16, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, ")");
        if (!object || (kind != WOWGDI_KIND_OBJ && kind != WOWGDI_KIND_STOCK))
        {
            WowNotePut(note, noteCapacity, &noteLength, " -- ★ NOT ONE OF OUR BITMAP TOKENS;"
                                       " answered 0");
            Wow32SetReturn(frame, 0);
            return 1;
        }
        brush = CreatePatternBrush((HBITMAP)object);
        token = brush ? WowGdiH16((HGDIOBJ)brush, WOWGDI_KIND_OBJ) : 0;
        if (!token)
        {
            if (brush)
                DeleteObject((HGDIOBJ)brush);
            WowNotePut(note, noteCapacity, &noteLength, " -- ★ no brush (or the token map is"
                                       " full); answered 0");
            Wow32SetReturn(frame, 0);
            return 1;
        }
        WowNotePut(note, noteCapacity, &noteLength, " -> brush token 0x");
        WowNoteHex(note, noteCapacity, &noteLength, token, WOW_HEX_WORD_DIGITS);
        Wow32SetReturn(frame, token);
        return 1;
    }

    /* 0x53 GetPixel(hDC, x, y) (Importance = 1):
     *
     * [CAUTION]: CLR_INVALID IS 0xFFFFFFFF AND IT IS NOT AN ERROR CODE THE GUEST CAN
     * MISREAD AS A COLOUR -- Win16 uses the same value for the same reason, so
     * a point outside the clip region travels through unchanged.
     */
    case WOWGDI_GETPIXEL:
    {
        WORD dc16 = Wow32ArgWord(frame, WOWGDI_XY_ARG_HDC);
        INT  positionX = (INT)(SHORT)Wow32ArgWord(frame, WOWGDI_XY_ARG_X);
        INT  positionY = (INT)(SHORT)Wow32ArgWord(frame, WOWGDI_XY_ARG_Y);
        INT  kind = -1;
        HGDIOBJ object = WowGdiH32(dc16, &kind);
        INT  noteLength = 0;
        COLORREF color;
        WowNotePut(note, noteCapacity, &noteLength, "GetPixel(0x");
        WowNoteHex(note, noteCapacity, &noteLength, dc16, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, ", ");
        WowNoteHex(note, noteCapacity, &noteLength, (DWORD)positionX, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, ",");
        WowNoteHex(note, noteCapacity, &noteLength, (DWORD)positionY, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, ")");
        if (!object || (kind != WOWGDI_KIND_DC && kind != WOWGDI_KIND_WINDC))
        {
            WowNotePut(note, noteCapacity, &noteLength, " -- ★ NOT ONE OF OUR DC TOKENS;"
                                       " answered CLR_INVALID");
            Wow32SetReturn(frame, WOWGDI_CLR_INVALID);
            return 1;
        }
        color = GetPixel((HDC)object, positionX, positionY);
        WowNotePut(note, noteCapacity, &noteLength, " = 0x");
        WowNoteHex(note, noteCapacity, &noteLength, (DWORD)color, WOW_HEX_DWORD_DIGITS);
        Wow32SetReturn(frame, (DWORD)color);
        return 1;
    }

    /* -- Three one-DC questions: 0x4b GetBkColor, 0x55 GetROP2,
     * 0x16e UpdateColors. [CAUTION] GetBkColor's answer is a DWORD COLORREF and the
     * other two are ints, which is the only difference between them here.
     */
    case WOWGDI_GETBKCOLOR:
    case WOWGDI_GETROP2:
    case WOWGDI_UPDATECOLORS:
    case WOWGDI_GETBKMODE:
    case WOWGDI_GETMAPMODE:
    case WOWGDI_GETPOLYFILLMODE:
    case WOWGDI_GETSTRETCHBLTMODE:
    case WOWGDI_GETTEXTCOLOR:
    {
        WORD dc16 = Wow32ArgWord(frame, WOWGDI_ONE_ARG_HANDLE);
        INT  kind = -1;
        HGDIOBJ object = WowGdiH32(dc16, &kind);
        INT  noteLength = 0;
        WowNotePut(note, noteCapacity, &noteLength,
                frame->Id == WOWGDI_GETBKCOLOR ? "GetBkColor(0x" :
                frame->Id == WOWGDI_GETROP2    ? "GetROP2(0x" :
                frame->Id == WOWGDI_GETBKMODE  ? "GetBkMode(0x" :
                frame->Id == WOWGDI_GETMAPMODE ? "GetMapMode(0x" :
                frame->Id == WOWGDI_GETPOLYFILLMODE ? "GetPolyFillMode(0x" :
                frame->Id == WOWGDI_GETSTRETCHBLTMODE ? "GetStretchBltMode(0x" :
                frame->Id == WOWGDI_GETTEXTCOLOR ? "GetTextColor(0x"
                                           : "UpdateColors(0x");
        WowNoteHex(note, noteCapacity, &noteLength, dc16, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, ")");
        if (!object || (kind != WOWGDI_KIND_DC && kind != WOWGDI_KIND_WINDC))
        {
            WowNotePut(note, noteCapacity, &noteLength, " -- ★ NOT ONE OF OUR DC TOKENS;"
                                       " answered 0");
            Wow32SetReturn(frame, 0);
            return 1;
        }
        if (frame->Id == WOWGDI_GETBKCOLOR || frame->Id == WOWGDI_GETTEXTCOLOR)
        {
            COLORREF color = frame->Id == WOWGDI_GETBKCOLOR ? GetBkColor((HDC)object)
                                                    : GetTextColor((HDC)object);
            WowNotePut(note, noteCapacity, &noteLength, " = 0x");
            WowNoteHex(note, noteCapacity, &noteLength, (DWORD)color, WOW_HEX_DWORD_DIGITS);
            Wow32SetReturn(frame, (DWORD)color);
        }
        else if (frame->Id != WOWGDI_UPDATECOLORS)
        {
            INT value = frame->Id == WOWGDI_GETROP2     ? GetROP2((HDC)object) :
                    frame->Id == WOWGDI_GETBKMODE   ? GetBkMode((HDC)object) :
                    frame->Id == WOWGDI_GETMAPMODE  ? GetMapMode((HDC)object) :
                    frame->Id == WOWGDI_GETPOLYFILLMODE ? GetPolyFillMode((HDC)object)
                                                : GetStretchBltMode((HDC)object);
            WowNotePut(note, noteCapacity, &noteLength, " = ");
            WowNoteHex(note, noteCapacity, &noteLength, (DWORD)value, WOW_HEX_WORD_DIGITS);
            Wow32SetReturn(frame, (DWORD)(WORD)value);
        }
        else
        {
            Wow32SetReturn(frame, (DWORD)(UpdateColors((HDC)object) ? 1 : 0));
        }
        return 1;
    }

    /* 0x40 CreateRectRgn / 0x2c SelectClipRgn -- clipping, as a pair (Importance = 1):
     *
     * [CAUTION]: A REGION IS AN ORDINARY GDI OBJECT and goes in the same token map with
     * KIND_OBJ, so the guest's own `DeleteObject` disposes of it with no new
     * case. [CAUTION] `SelectClipRgn(hDC, NULL)` is the documented way to REMOVE the
     * clip region, so a zero handle is not an error here -- it is the call
     * doing its other job, and passing our "not one of our tokens" refusal for
     * it would leave a guest permanently clipped.
     */
    case WOWGDI_CREATERECTRGN:
    {
        INT  left = (INT)(SHORT)Wow32ArgWord(frame, WOWGDI_RGN_ARG_LEFT);
        INT  top = (INT)(SHORT)Wow32ArgWord(frame, WOWGDI_RGN_ARG_TOP);
        INT  right = (INT)(SHORT)Wow32ArgWord(frame, WOWGDI_RGN_ARG_RIGHT);
        INT  bottom = (INT)(SHORT)Wow32ArgWord(frame, WOWGDI_RGN_ARG_BOTTOM);
        INT  noteLength = 0;
        HRGN region = CreateRectRgn(left, top, right, bottom);
        WORD token = region ? WowGdiH16((HGDIOBJ)region, WOWGDI_KIND_OBJ) : 0;
        WowNotePut(note, noteCapacity, &noteLength, "CreateRectRgn(");
        WowNoteHex(note, noteCapacity, &noteLength, (DWORD)left, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, ",");
        WowNoteHex(note, noteCapacity, &noteLength, (DWORD)top, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, " ");
        WowNoteHex(note, noteCapacity, &noteLength, (DWORD)right, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, ",");
        WowNoteHex(note, noteCapacity, &noteLength, (DWORD)bottom, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, ")");
        if (!token)
        {
            if (region)
                DeleteObject((HGDIOBJ)region);
            WowNotePut(note, noteCapacity, &noteLength, " -- ★ no region (or the token map is"
                                       " full); answered 0");
            Wow32SetReturn(frame, 0);
            return 1;
        }
        WowNotePut(note, noteCapacity, &noteLength, " -> region token 0x");
        WowNoteHex(note, noteCapacity, &noteLength, token, WOW_HEX_WORD_DIGITS);
        Wow32SetReturn(frame, token);
        return 1;
    }

    case WOWGDI_SELECTCLIPRGN:
    {
        WORD dc16  = Wow32ArgWord(frame, WOWGDI_SCR_ARG_HDC);
        WORD region16 = Wow32ArgWord(frame, WOWGDI_SCR_ARG_RGN);
        INT  dcKind = -1, regionKind = -1;
        HGDIOBJ dcObject = WowGdiH32(dc16, &dcKind);
        HGDIOBJ region = WowGdiH32(region16, &regionKind);
        INT  noteLength = 0, regionResult;
        WowNotePut(note, noteCapacity, &noteLength, "SelectClipRgn(0x");
        WowNoteHex(note, noteCapacity, &noteLength, dc16, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, ", 0x");
        WowNoteHex(note, noteCapacity, &noteLength, region16, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, ")");
        if (!dcObject || (dcKind != WOWGDI_KIND_DC && dcKind != WOWGDI_KIND_WINDC))
        {
            WowNotePut(note, noteCapacity, &noteLength, " -- ★ NOT ONE OF OUR DC TOKENS;"
                                       " answered 0");
            Wow32SetReturn(frame, 0);
            return 1;
        }
        if (region16 && !region)
        {
            WowNotePut(note, noteCapacity, &noteLength, " -- ★ NOT ONE OF OUR REGION TOKENS;"
                                       " answered 0 rather than clearing the"
                                       " clip region, which is what NULL means");
            Wow32SetReturn(frame, 0);
            return 1;
        }
        if (!region16)
            WowNotePut(note, noteCapacity, &noteLength, " -- NULL = remove the clip region");
        regionResult = SelectClipRgn((HDC)dcObject, (HRGN)region);
        WowNotePut(note, noteCapacity, &noteLength, " -> region complexity ");
        WowNoteHex(note, noteCapacity, &noteLength, (DWORD)regionResult, WOW_HEX_WORD_DIGITS);
        Wow32SetReturn(frame, (DWORD)(WORD)regionResult);
        return 1;
    }

    /* 0x2f CombineRgn(hDest, hSrc1, hSrc2, mode):
     *
     * [CAUTION]: hSrc2 IS LEGITIMATELY NULL for RGN_COPY, so an absent second source is
     * only an error when the mode actually needs one. Refusing it outright
     * would break the idiomatic "copy this region" call.
     */
    case WOWGDI_COMBINERGN:
    {
        WORD destHandle = Wow32ArgWord(frame, WOWGDI_CBR_ARG_DEST), source1Handle = Wow32ArgWord(frame, WOWGDI_CBR_ARG_SRC1);
        WORD source2Handle = Wow32ArgWord(frame, WOWGDI_CBR_ARG_SRC2), mode = Wow32ArgWord(frame, WOWGDI_CBR_ARG_MODE);
        INT objectKind = -1, source1Kind = -1, source2Kind = -1, noteLength = 0, regionResult;
        HGDIOBJ destination = WowGdiH32(destHandle, &objectKind), source1 = WowGdiH32(source1Handle, &source1Kind),
                source2 = WowGdiH32(source2Handle, &source2Kind);
        WowNotePut(note, noteCapacity, &noteLength, "CombineRgn(0x");
        WowNoteHex(note, noteCapacity, &noteLength, destHandle, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, ", 0x");
        WowNoteHex(note, noteCapacity, &noteLength, source1Handle, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, ", 0x");
        WowNoteHex(note, noteCapacity, &noteLength, source2Handle, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, ", mode ");
        WowNoteHex(note, noteCapacity, &noteLength, mode, WOW_HEX_BYTE_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, ")");
        if (!destination || !source1 || (source2Handle && !source2))
        {
            WowNotePut(note, noteCapacity, &noteLength, " -- ★ NOT ONE OF OUR REGION TOKENS;"
                                       " answered ERROR");
            Wow32SetReturn(frame, 0);                       /* ERROR */
            return 1;
        }
        regionResult = CombineRgn((HRGN)destination, (HRGN)source1, (HRGN)source2, (INT)(SHORT)mode);
        WowNotePut(note, noteCapacity, &noteLength, " -> complexity ");
        WowNoteHex(note, noteCapacity, &noteLength, (DWORD)regionResult, WOW_HEX_WORD_DIGITS);
        Wow32SetReturn(frame, (DWORD)(WORD)regionResult);
        return 1;
    }

    /* -- 0x41 CreateRectRgnIndirect(lpRect) -- CreateRectRgn with the four
     * numbers in a struct instead of on the stack.
     */
    case WOWGDI_CREATERECTRGNIND:
    {
        volatile BYTE *rectBytes = Wow32ArgPointer(frame, WOWGDI_CRRI_ARG_RECT);
        INT noteLength = 0;
        HRGN region;
        WORD token;
        WowNotePut(note, noteCapacity, &noteLength, "CreateRectRgnIndirect");
        if (!rectBytes)
        {
            WowNotePut(note, noteCapacity, &noteLength, " -- ★ NULL lpRect; answered 0");
            Wow32SetReturn(frame, 0);
            return 1;
        }
        region = CreateRectRgn((INT)(SHORT)WowGdiPeek(rectBytes, 0),
                            (INT)(SHORT)WowGdiPeek(rectBytes, WOWGDI_RECT16_TOP),
                            (INT)(SHORT)WowGdiPeek(rectBytes, WOWGDI_RECT16_RIGHT),
                            (INT)(SHORT)WowGdiPeek(rectBytes, WOWGDI_RECT16_BOTTOM));
        token = region ? WowGdiH16((HGDIOBJ)region, WOWGDI_KIND_OBJ) : 0;
        if (!token)
        {
            if (region)
                DeleteObject((HGDIOBJ)region);
            WowNotePut(note, noteCapacity, &noteLength, " -- ★ no region (or the token map is"
                                       " full); answered 0");
            Wow32SetReturn(frame, 0);
            return 1;
        }
        WowNotePut(note, noteCapacity, &noteLength, " -> region token 0x");
        WowNoteHex(note, noteCapacity, &noteLength, token, WOW_HEX_WORD_DIGITS);
        Wow32SetReturn(frame, token);
        return 1;
    }

    /* -- 0xac SetRectRgn(hrgn, l, t, r, b) -- redefine an EXISTING region.
     *
     * [INFO]: The point of it is that it does not allocate, so a guest animating a
     * clip does not churn the token map.
     */
    case WOWGDI_SETRECTRGN:
    {
        WORD region16 = Wow32ArgWord(frame, WOWGDI_SRR_ARG_RGN);
        INT  regionKind = -1, noteLength = 0;
        HGDIOBJ region = WowGdiH32(region16, &regionKind);
        WowNotePut(note, noteCapacity, &noteLength, "SetRectRgn(0x");
        WowNoteHex(note, noteCapacity, &noteLength, region16, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, ")");
        if (!region)
        {
            WowNotePut(note, noteCapacity, &noteLength, " -- ★ NOT ONE OF OUR REGION TOKENS;"
                                       " answered 0");
            Wow32SetReturn(frame, 0);
            return 1;
        }
        SetRectRgn((HRGN)region,
                   (INT)(SHORT)Wow32ArgWord(frame, WOWGDI_SRR_ARG_LEFT),
                   (INT)(SHORT)Wow32ArgWord(frame, WOWGDI_SRR_ARG_TOP),
                   (INT)(SHORT)Wow32ArgWord(frame, WOWGDI_SRR_ARG_RIGHT),
                   (INT)(SHORT)Wow32ArgWord(frame, WOWGDI_SRR_ARG_BOTTOM));
        Wow32SetReturn(frame, 1);
        return 1;
    }

    /* 0x3f CreatePolygonRgn(lpPoints, nCount, fnMode):
     *
     * [CAUTION]: A Win16 POINT IS TWO **WORDS**, not two LONGs, so the array has to be
     * widened one point at a time -- handing the guest's buffer straight to
     * Win32 would read every coordinate from the wrong half of the next pair
     * and produce a region that is wrong without being empty.
     *
     * [CAUTION]: AND THE COUNT IS BOUNDED before it is trusted: it is a guest number
     * used as an allocation size.
     */
    case WOWGDI_CREATEPOLYGONRGN:
    {
        volatile BYTE *pointBytes = Wow32ArgPointer(frame, WOWGDI_CPR_ARG_POINTS);
        WORD itemCount  = Wow32ArgWord(frame, WOWGDI_CPR_ARG_COUNT);
        WORD mode = Wow32ArgWord(frame, WOWGDI_CPR_ARG_MODE);
        INT  noteLength = 0, index;
        POINT points[WOWGDI_MAX_POLYPTS];
        HRGN region;
        WORD token;
        WowNotePut(note, noteCapacity, &noteLength, "CreatePolygonRgn(n=");
        WowNoteHex(note, noteCapacity, &noteLength, itemCount, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, ", mode ");
        WowNoteHex(note, noteCapacity, &noteLength, mode, WOW_HEX_BYTE_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, ")");
        if (!pointBytes || !itemCount || itemCount > WOWGDI_MAX_POLYPTS)
        {
            WowNotePut(note, noteCapacity, &noteLength, " -- ★ NULL points or count out of range"
                                       " (max 0x");
            WowNoteHex(note, noteCapacity, &noteLength, WOWGDI_MAX_POLYPTS, WOW_HEX_WORD_DIGITS);
            WowNotePut(note, noteCapacity, &noteLength, "); answered 0");
            Wow32SetReturn(frame, 0);
            return 1;
        }
        for (index = 0; index < (INT)itemCount; ++index)
        {
            points[index].x = (LONG)(SHORT)WowGdiPeek(pointBytes, index * WOWGDI_POINT16_SIZE);
            points[index].y = (LONG)(SHORT)WowGdiPeek(pointBytes, index * WOWGDI_POINT16_SIZE + WOWGDI_POINT16_Y);
        }
        region = CreatePolygonRgn(points, (INT)itemCount, (INT)(SHORT)mode);
        token = region ? WowGdiH16((HGDIOBJ)region, WOWGDI_KIND_OBJ) : 0;
        if (!token)
        {
            if (region)
                DeleteObject((HGDIOBJ)region);
            WowNotePut(note, noteCapacity, &noteLength, " -- ★ no region; answered 0");
            Wow32SetReturn(frame, 0);
            return 1;
        }
        WowNotePut(note, noteCapacity, &noteLength, " -> region token 0x");
        WowNoteHex(note, noteCapacity, &noteLength, token, WOW_HEX_WORD_DIGITS);
        Wow32SetReturn(frame, token);
        return 1;
    }

    /* -- 0x4d GetClipBox(hDC, lpRect) -- the bounding box of the clip region. */
    case WOWGDI_GETCLIPBOX:
    {
        WORD dc16 = Wow32ArgWord(frame, WOWGDI_GCX_ARG_HDC);
        volatile BYTE *rectBytes = Wow32ArgPointer(frame, WOWGDI_GCX_ARG_RECT);
        INT dcKind = -1, noteLength = 0, regionResult;
        HGDIOBJ dcObject = WowGdiH32(dc16, &dcKind);
        RECT rect32;
        WowNotePut(note, noteCapacity, &noteLength, "GetClipBox(0x");
        WowNoteHex(note, noteCapacity, &noteLength, dc16, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, ")");
        if (!rectBytes)
        {
            WowNotePut(note, noteCapacity, &noteLength, " -- ★ NULL lpRect; nothing written");
            Wow32SetReturn(frame, 0);                       /* ERROR */
            return 1;
        }
        if (!dcObject || (dcKind != WOWGDI_KIND_DC && dcKind != WOWGDI_KIND_WINDC))
        {
            INT index;
            /* Zero it rather than leave the caller's litter -- same reasoning as
             * GetClientRect, and for the same reason: a guest that clips to
             * stack litter draws nothing and looks like a paint bug.
             */
            for (index = 0; index < WOWCONV_RECT16_SIZE; ++index)
                rectBytes[index] = 0;
            WowNotePut(note, noteCapacity, &noteLength, " -- ★ NOT ONE OF OUR DC TOKENS; zeroed");
            Wow32SetReturn(frame, 0);
            return 1;
        }
        regionResult = GetClipBox((HDC)dcObject, &rect32);
        Wow32PokeWord(rectBytes + 0, (WORD)(SHORT)rect32.left);
        Wow32PokeWord(rectBytes + WOWGDI_RECT16_TOP, (WORD)(SHORT)rect32.top);
        Wow32PokeWord(rectBytes + WOWGDI_RECT16_RIGHT, (WORD)(SHORT)rect32.right);
        Wow32PokeWord(rectBytes + WOWGDI_RECT16_BOTTOM, (WORD)(SHORT)rect32.bottom);
        WowNotePut(note, noteCapacity, &noteLength, " -> ");
        WowNoteHex(note, noteCapacity, &noteLength, (DWORD)rect32.right, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, "x");
        WowNoteHex(note, noteCapacity, &noteLength, (DWORD)rect32.bottom, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, " complexity ");
        WowNoteHex(note, noteCapacity, &noteLength, (DWORD)regionResult, WOW_HEX_WORD_DIGITS);
        Wow32SetReturn(frame, (DWORD)(WORD)regionResult);
        return 1;
    }

    /* -- 0x5c GetTextFace(hDC, nCount, lpFaceName) -- the name of the font
     * currently selected. WRITE.EXE and CARDFILE both ask.
     *
     * [CAUTION]: nCount IS A BUFFER SIZE FROM THE GUEST and bounds the copy; Win32's own
     * call is given the smaller of it and our stack buffer.
     */
    case WOWGDI_GETTEXTFACE:
    {
        WORD dc16 = Wow32ArgWord(frame, WOWGDI_GTF_ARG_HDC);
        WORD count = Wow32ArgWord(frame, WOWGDI_GTF_ARG_COUNT);
        volatile BYTE *buffer16 = Wow32ArgPointer(frame, WOWGDI_GTF_ARG_BUF);
        INT dcKind = -1, noteLength = 0, returned, index;
        HGDIOBJ dcObject = WowGdiH32(dc16, &dcKind);
        CHAR faceName[LF_FACESIZE + 1];
        WowNotePut(note, noteCapacity, &noteLength, "GetTextFace(0x");
        WowNoteHex(note, noteCapacity, &noteLength, dc16, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, ", ");
        WowNoteHex(note, noteCapacity, &noteLength, count, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, ")");
        if (!buffer16 || !count || !dcObject
            || (dcKind != WOWGDI_KIND_DC && dcKind != WOWGDI_KIND_WINDC))
        {
            WowNotePut(note, noteCapacity, &noteLength, " -- ★ NULL buffer or not one of our DC"
                                       " tokens; answered 0");
            Wow32SetReturn(frame, 0);
            return 1;
        }
        returned = (INT)GetTextFaceA((HDC)dcObject, (INT)sizeof faceName, faceName);
        if (returned <= 0)
        {
            faceName[0] = 0;
            returned = 0;
        }
        if (returned > (INT)count - 1)
            returned = (INT)count - 1;
        for (index = 0; index < returned; ++index)
            buffer16[index] = (BYTE)faceName[index];
        buffer16[returned] = 0;
        WowNotePut(note, noteCapacity, &noteLength, " -> ");
        WowNoteQuoted(note, noteCapacity, &noteLength, faceName);
        Wow32SetReturn(frame, (DWORD)(WORD)returned);
        return 1;
    }

    /* -- 0x0a SetTextJustification(hDC, nBreakExtra, nBreakCount) -- how WRITE
     * justifies a line: spread nBreakExtra units over nBreakCount breaks.
     */
    case WOWGDI_SETTEXTJUST:
    {
        WORD dc16 = Wow32ArgWord(frame, WOWGDI_STJ_ARG_HDC);
        INT  extra  = (INT)(SHORT)Wow32ArgWord(frame, WOWGDI_STJ_ARG_EXTRA);
        INT  breakCount  = (INT)(SHORT)Wow32ArgWord(frame, WOWGDI_STJ_ARG_COUNT);
        INT  dcKind = -1, noteLength = 0, regionResult;
        HGDIOBJ dcObject = WowGdiH32(dc16, &dcKind);
        WowNotePut(note, noteCapacity, &noteLength, "SetTextJustification(0x");
        WowNoteHex(note, noteCapacity, &noteLength, dc16, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, ", extra ");
        WowNoteHex(note, noteCapacity, &noteLength, (DWORD)extra, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, ", breaks ");
        WowNoteHex(note, noteCapacity, &noteLength, (DWORD)breakCount, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, ")");
        if (!dcObject || (dcKind != WOWGDI_KIND_DC && dcKind != WOWGDI_KIND_WINDC))
        {
            WowNotePut(note, noteCapacity, &noteLength, " -- ★ NOT ONE OF OUR DC TOKENS;"
                                       " answered 0");
            Wow32SetReturn(frame, 0);
            return 1;
        }
        regionResult = SetTextJustification((HDC)dcObject, extra, breakCount);
        Wow32SetReturn(frame, (DWORD)(WORD)(regionResult ? 1 : 0));
        return 1;
    }

    /* -- 0x0c SetWindowExt / 0x0d SetViewportOrg / 0x0e SetViewportExt /
     * 0xa3 SetBitmapDimension -- one (handle, x, y) block, four calls. ------
     *
     * [CAUTION]: THREE TAKE A DC AND THE FOURTH TAKES A BITMAP, which is exactly the kind
     * of difference that a shared case hides. It is checked per-id here rather
     * than assumed, because handing a bitmap to SetViewportOrgEx would fail
     * quietly and leave a guest drawing at the wrong origin.
     *
     * [INFO]: All four answer with the PREVIOUS pair packed y:x in a DWORD, which is
     * the same convention SetWindowOrg already uses next door.
     */
    case WOWGDI_SETWINDOWEXT:
    case WOWGDI_SETVIEWPORTORG:
    case WOWGDI_SETVIEWPORTEXT:
    case WOWGDI_SETBITMAPDIM:
    {
        WORD handle16 = Wow32ArgWord(frame, WOWGDI_ORG_ARG_HDC);
        INT  positionX = (INT)(SHORT)Wow32ArgWord(frame, WOWGDI_ORG_ARG_X);
        INT  positionY = (INT)(SHORT)Wow32ArgWord(frame, WOWGDI_ORG_ARG_Y);
        INT  isBitmap = (frame->Id == WOWGDI_SETBITMAPDIM);
        INT  kind = -1;
        HGDIOBJ object = WowGdiH32(handle16, &kind);
        INT  noteLength = 0;
        SIZE size;
        POINT point;
        WowNotePut(note, noteCapacity, &noteLength,
                frame->Id == WOWGDI_SETWINDOWEXT   ? "SetWindowExt(0x" :
                frame->Id == WOWGDI_SETVIEWPORTORG ? "SetViewportOrg(0x" :
                frame->Id == WOWGDI_SETVIEWPORTEXT ? "SetViewportExt(0x"
                                               : "SetBitmapDimension(0x");
        WowNoteHex(note, noteCapacity, &noteLength, handle16, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, ", ");
        WowNoteHex(note, noteCapacity, &noteLength, (DWORD)positionX, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, ",");
        WowNoteHex(note, noteCapacity, &noteLength, (DWORD)positionY, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, ")");
        if (!object || (isBitmap ? (kind != WOWGDI_KIND_OBJ && kind != WOWGDI_KIND_STOCK)
                        : (kind != WOWGDI_KIND_DC && kind != WOWGDI_KIND_WINDC)))
        {
            WowNotePut(note, noteCapacity, &noteLength, isBitmap
                        ? " -- ★ NOT ONE OF OUR BITMAP TOKENS; answered 0"
                        : " -- ★ NOT ONE OF OUR DC TOKENS; answered 0");
            Wow32SetReturn(frame, 0);
            return 1;
        }
        size.cx = size.cy = 0;
        point.x = point.y = 0;
        if (frame->Id == WOWGDI_SETWINDOWEXT)
            SetWindowExtEx((HDC)object, positionX, positionY, &size);
        else if (frame->Id == WOWGDI_SETVIEWPORTEXT)
            SetViewportExtEx((HDC)object, positionX, positionY, &size);
        else if (frame->Id == WOWGDI_SETVIEWPORTORG)
        {
            SetViewportOrgEx((HDC)object, positionX, positionY, &point);
            size.cx = point.x;
            size.cy = point.y;
        }
        else
        {
            SetBitmapDimensionEx((HBITMAP)object, positionX, positionY, &size);
        }
        Wow32SetReturn(frame, ((DWORD)(WORD)(SHORT)size.cy << WORD_SHIFT)
                        | (DWORD)(WORD)(SHORT)size.cx);
        return 1;
    }

    /* -- 0x9a GetNearestColor(hDC, crColor) / 0x172 GetNearestPaletteIndex -
     *
     * [CAUTION]: THE FIRST ARGUMENT IS A DC IN ONE AND A PALETTE IN THE OTHER, and this
     * host has never made a palette -- Paint's `CreatePalette` reaches a stub
     * no run has yet named. So the palette arm answers 0 and SAYS SO, rather
     * than pretending an index; a wrong index is a wrong colour with no way to
     * tell afterwards that it was invented.
     */
    case WOWGDI_GETNEARESTCOLOR:
    case WOWGDI_GETNEARESTPALIDX:
    {
        INT   isIndex = (frame->Id == WOWGDI_GETNEARESTPALIDX);
        WORD  handle16   = Wow32ArgWord(frame, WOWGDI_COL_ARG_HDC);
        DWORD color = Wow32ArgDword(frame, WOWGDI_COL_ARG_COLOR);
        INT   kind = -1;
        HGDIOBJ object = WowGdiH32(handle16, &kind);
        INT   noteLength = 0;
        WowNotePut(note, noteCapacity, &noteLength, isIndex ? "GetNearestPaletteIndex(0x"
                                         : "GetNearestColor(0x");
        WowNoteHex(note, noteCapacity, &noteLength, handle16, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, ", 0x");
        WowNoteHex(note, noteCapacity, &noteLength, color, WOW_HEX_DWORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, ")");
        /* [CAUTION]: Session 46 answered this 0 because nothing could make a palette.
         * `CreatePalette` (0x168) is serviced now, so it is a real lookup --
         * and a handle that is still not ours is still refused rather than
         * answered with an invented index.
         */
        if (isIndex)
        {
            if (!object || (kind != WOWGDI_KIND_OBJ && kind != WOWGDI_KIND_STOCK))
            {
                WowNotePut(note, noteCapacity, &noteLength, " -- ★ NOT ONE OF OUR PALETTE TOKENS;"
                                           " answered 0 rather than inventing an"
                                           " index");
                Wow32SetReturn(frame, 0);
                return 1;
            }
            Wow32SetReturn(frame, (DWORD)GetNearestPaletteIndex((HPALETTE)object, (COLORREF)color));
            return 1;
        }
        if (!object || (kind != WOWGDI_KIND_DC && kind != WOWGDI_KIND_WINDC))
        {
            WowNotePut(note, noteCapacity, &noteLength, " -- ★ NOT ONE OF OUR DC TOKENS;"
                                       " answered the colour unchanged");
            Wow32SetReturn(frame, color);
            return 1;
        }
        color = (DWORD)GetNearestColor((HDC)object, (COLORREF)color);
        WowNotePut(note, noteCapacity, &noteLength, " = 0x");
        WowNoteHex(note, noteCapacity, &noteLength, color, WOW_HEX_DWORD_DIGITS);
        Wow32SetReturn(frame, color);
        return 1;
    }

    /* 0x21 TextOut / 0x5b GetTextExtent -- the same (string, count) (Importance = 2):
     *
     * [CAUTION]: THE STRING IS COUNTED, NOT TERMINATED. Both take an explicit length and
     * a Win16 program is entitled to hand over a fragment of a larger buffer,
     * so this must NOT stop at a NUL the way `Wow32ArgString` does -- it copies
     * exactly the count it was given, bounded by the scratch buffer.
     */
    case WOWGDI_INTERSECTCLIPRECT:
    {
        WORD token = Wow32ArgWord(frame, WOWGDI_ICR_ARG_HDC);
        INT  left = (INT)(SHORT)Wow32ArgWord(frame, WOWGDI_ICR_ARG_LEFT);
        INT  top = (INT)(SHORT)Wow32ArgWord(frame, WOWGDI_ICR_ARG_TOP);
        INT  right = (INT)(SHORT)Wow32ArgWord(frame, WOWGDI_ICR_ARG_RIGHT);
        INT  bottom = (INT)(SHORT)Wow32ArgWord(frame, WOWGDI_ICR_ARG_BOTTOM);
        INT  kind = -1, regionResult;
        HGDIOBJ object = WowGdiH32(token, &kind);
        INT  noteLength = 0;
        WowNotePut(note, noteCapacity, &noteLength, "IntersectClipRect(0x");
        WowNoteHex(note, noteCapacity, &noteLength, token, WOW_HEX_WORD_DIGITS);
        if (!object || (kind != WOWGDI_KIND_DC && kind != WOWGDI_KIND_WINDC))
        {
            WowNotePut(note, noteCapacity, &noteLength, ") -- ★ NOT ONE OF OUR DC TOKENS; 0");
            Wow32SetReturn(frame, 0);
            return 1;
        }
        regionResult = IntersectClipRect((HDC)object, left, top, right, bottom);
        WowNotePut(note, noteCapacity, &noteLength, ") -> region kind ");
        WowNoteHex(note, noteCapacity, &noteLength, (DWORD)regionResult, WOW_HEX_BYTE_DIGITS);
        Wow32SetReturn(frame, (DWORD)regionResult);
        return 1;
    }

    case WOWGDI_RECTVISIBLE:
    {
        WORD token = Wow32ArgWord(frame, WOWGDI_RV_ARG_HDC);
        volatile BYTE *rectBytes = Wow32ArgPointer(frame, WOWGDI_RV_ARG_RECT);
        INT kind = -1, index, isVisible;
        HGDIOBJ object = WowGdiH32(token, &kind);
        BYTE rect16[WOWCONV_RECT16_SIZE];
        RECT result;
        INT noteLength = 0;
        WowNotePut(note, noteCapacity, &noteLength, "RectVisible(0x");
        WowNoteHex(note, noteCapacity, &noteLength, token, WOW_HEX_WORD_DIGITS);
        if (!object || (kind != WOWGDI_KIND_DC && kind != WOWGDI_KIND_WINDC) || !rectBytes)
        {
            WowNotePut(note, noteCapacity, &noteLength, ") -- ★ NOT ONE OF OUR DC TOKENS, or no rect; 0");
            Wow32SetReturn(frame, 0);
            return 1;
        }
        for (index = 0; index < WOWCONV_RECT16_SIZE; ++index)
            rect16[index] = (BYTE)rectBytes[index];
        result.left  = WowConvRect16Get(rect16, WOWCONV_RECT16_LEFT);
        result.top    = WowConvRect16Get(rect16, WOWCONV_RECT16_TOP);
        result.right = WowConvRect16Get(rect16, WOWCONV_RECT16_RIGHT);
        result.bottom = WowConvRect16Get(rect16, WOWCONV_RECT16_BOTTOM);
        isVisible = RectVisible((HDC)object, &result) ? 1 : 0;
        WowNotePut(note, noteCapacity, &noteLength, isVisible ? ") -> VISIBLE" : ") -> not visible");
        Wow32SetReturn(frame, (DWORD)isVisible);
        return 1;
    }

    case WOWGDI_CREATEFONT:
    {
        CHAR faceName[LF_FACESIZE];
        HFONT font;
        WORD  token;
        INT   noteLength = 0;
        INT   handle16  = (INT)(SHORT)Wow32ArgWord(frame, WOWGDI_CF_ARG_HEIGHT);
        INT   width = (INT)(SHORT)Wow32ArgWord(frame, WOWGDI_CF_ARG_WIDTH);
        faceName[0] = 0;
        Wow32ArgString(frame, WOWGDI_CF_ARG_FACE, faceName, (INT)sizeof faceName);
        font = CreateFontA(handle16, width,
                         (INT)(SHORT)Wow32ArgWord(frame, WOWGDI_CF_ARG_ESCAPE),
                         (INT)(SHORT)Wow32ArgWord(frame, WOWGDI_CF_ARG_ORIENT),
                         (INT)(SHORT)Wow32ArgWord(frame, WOWGDI_CF_ARG_WEIGHT),
                         (DWORD)(Wow32ArgWord(frame, WOWGDI_CF_ARG_ITALIC) & BYTE_MASK),
                         (DWORD)(Wow32ArgWord(frame, WOWGDI_CF_ARG_UNDER)  & BYTE_MASK),
                         (DWORD)(Wow32ArgWord(frame, WOWGDI_CF_ARG_STRIKE) & BYTE_MASK),
                         (DWORD)(Wow32ArgWord(frame, WOWGDI_CF_ARG_CHARSET) & BYTE_MASK),
                         (DWORD)(Wow32ArgWord(frame, WOWGDI_CF_ARG_OUTPREC) & BYTE_MASK),
                         (DWORD)(Wow32ArgWord(frame, WOWGDI_CF_ARG_CLIPPREC) & BYTE_MASK),
                         (DWORD)(Wow32ArgWord(frame, WOWGDI_CF_ARG_QUALITY) & BYTE_MASK),
                         (DWORD)(Wow32ArgWord(frame, WOWGDI_CF_ARG_PITCH) & BYTE_MASK),
                         faceName[0] ? faceName : NULL);
        token = font ? WowGdiH16((HGDIOBJ)font, WOWGDI_KIND_OBJ) : 0;
        WowNotePut(note, noteCapacity, &noteLength, "CreateFont h=");
        WowNoteHex(note, noteCapacity, &noteLength, (DWORD)handle16, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, " \"");
        WowNotePut(note, noteCapacity, &noteLength, faceName[0] ? faceName : "(any)");
        WowNotePut(note, noteCapacity, &noteLength, "\" -> token 0x");
        WowNoteHex(note, noteCapacity, &noteLength, token, WOW_HEX_WORD_DIGITS);
        if (font && !token) { DeleteObject((HGDIOBJ)font);
                          WowNotePut(note, noteCapacity, &noteLength, " -- ★ TOKEN MAP FULL; font deleted"); }
        Wow32SetReturn(frame, (DWORD)token);
        return 1;
    }

    case WOWGDI_GETCHARWIDTH:
    {
        WORD token   = Wow32ArgWord(frame, WOWGDI_GCW_ARG_HDC);
        WORD firstChar = Wow32ArgWord(frame, WOWGDI_GCW_ARG_FIRST);
        WORD lastChar  = Wow32ArgWord(frame, WOWGDI_GCW_ARG_LAST);
        volatile BYTE *buffer16 = Wow32ArgPointer(frame, WOWGDI_GCW_ARG_BUF);
        INT kind = -1;
        HGDIOBJ object = WowGdiH32(token, &kind);
        INT widths32[WOWGDI_CHAR_WIDTHS_MAX];
        INT noteLength = 0, itemCount, index, isOk;
        WowNotePut(note, noteCapacity, &noteLength, "GetCharWidth(0x");
        WowNoteHex(note, noteCapacity, &noteLength, token, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, ", ");
        WowNoteHex(note, noteCapacity, &noteLength, firstChar, WOW_HEX_BYTE_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, "..");
        WowNoteHex(note, noteCapacity, &noteLength, lastChar, WOW_HEX_BYTE_DIGITS);
        if (!object || (kind != WOWGDI_KIND_DC && kind != WOWGDI_KIND_WINDC) || !buffer16
            || lastChar < firstChar || (INT)(lastChar - firstChar) >= WOWGDI_CHAR_WIDTHS_MAX)
        {
            WowNotePut(note, noteCapacity, &noteLength, ") -- ★ NOT ONE OF OUR DC TOKENS, no buffer, "
                                       "or a range past 256; 0");
            Wow32SetReturn(frame, 0);
            return 1;
        }
        itemCount  = (INT)(lastChar - firstChar) + 1;
        isOk = GetCharWidth32A((HDC)object, firstChar, lastChar, widths32) ? 1 : 0;
        if (!isOk)
            isOk = GetCharWidthA((HDC)object, firstChar, lastChar, widths32) ? 1 : 0;
        if (!isOk) { WowNotePut(note, noteCapacity, &noteLength, ") -- GDI refused; nothing written");
                   Wow32SetReturn(frame, 0);
                   return 1; }
        /* [CAUTION]: ONE WORD PER CHARACTER. See the note by the ids. */
        for (index = 0; index < itemCount; ++index)
            Wow32PokeWord(buffer16 + index * WOW_WORD_BYTES, (WORD)(SHORT)widths32[index]);
        WowNotePut(note, noteCapacity, &noteLength, ") -> ");
        WowNoteHex(note, noteCapacity, &noteLength, (DWORD)itemCount, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, " widths, one WORD each");
        Wow32SetReturn(frame, 1);
        return 1;
    }

    case WOWGDI_CREATEMETAFILE:
    {
        CHAR path[MAX_PATH];
        HDC  memoryDc;
        WORD token;
        INT  noteLength = 0, isNamed;
        path[0] = 0;
        isNamed = Wow32ArgString(frame, WOWGDI_CMF_ARG_FILE, path, (INT)sizeof path);
        memoryDc = CreateMetaFileA(isNamed && path[0] ? path : NULL);
        token = memoryDc ? WowGdiH16((HGDIOBJ)memoryDc, WOWGDI_KIND_DC) : 0;
        WowNotePut(note, noteCapacity, &noteLength, "CreateMetaFile(");
        WowNotePut(note, noteCapacity, &noteLength, (isNamed && path[0]) ? path : "in memory");
        WowNotePut(note, noteCapacity, &noteLength, ") -> DC token 0x");
        WowNoteHex(note, noteCapacity, &noteLength, token, WOW_HEX_WORD_DIGITS);
        Wow32SetReturn(frame, (DWORD)token);
        return 1;
    }

    case WOWGDI_CLOSEMETAFILE:
    {
        WORD token = Wow32ArgWord(frame, WOWGDI_MF1_ARG_H);
        INT  kind = -1;
        HGDIOBJ object = WowGdiH32(token, &kind);
        HMETAFILE metafile;
        WORD metafileToken;
        INT  noteLength = 0;
        WowNotePut(note, noteCapacity, &noteLength, "CloseMetaFile(0x");
        WowNoteHex(note, noteCapacity, &noteLength, token, WOW_HEX_WORD_DIGITS);
        if (!object || kind != WOWGDI_KIND_DC)
        {
            WowNotePut(note, noteCapacity, &noteLength, ") -- ★ NOT A METAFILE DC TOKEN; 0");
            Wow32SetReturn(frame, 0);
            return 1;
        }
        metafile = CloseMetaFile((HDC)object);
        WowGdiForget(token);              /* the DC is gone whatever happened */
        /* [CAUTION]: A METAFILE IS NOT A DC. It gets an OBJ token so that a guest which
         * passes it to a DC call is refused rather than obeyed.
         */
        metafileToken = metafile ? WowGdiH16((HGDIOBJ)metafile, WOWGDI_KIND_OBJ) : 0;
        WowNotePut(note, noteCapacity, &noteLength, ") -> metafile token 0x");
        WowNoteHex(note, noteCapacity, &noteLength, metafileToken, WOW_HEX_WORD_DIGITS);
        Wow32SetReturn(frame, (DWORD)metafileToken);
        return 1;
    }

    case WOWGDI_DELETEMETAFILE:
    {
        WORD token = Wow32ArgWord(frame, WOWGDI_MF1_ARG_H);
        INT  kind = -1;
        HGDIOBJ object = WowGdiH32(token, &kind);
        INT  noteLength = 0, result = 0;
        WowNotePut(note, noteCapacity, &noteLength, "DeleteMetaFile(0x");
        WowNoteHex(note, noteCapacity, &noteLength, token, WOW_HEX_WORD_DIGITS);
        if (object && kind == WOWGDI_KIND_OBJ)
        {
            result = DeleteMetaFile((HMETAFILE)object) ? 1 : 0;
            WowGdiForget(token);
        }
        WowNotePut(note, noteCapacity, &noteLength, result ? ") -> deleted"
                                     : ") -- ★ NOT ONE OF OUR METAFILE TOKENS; FALSE");
        Wow32SetReturn(frame, (DWORD)result);
        return 1;
    }

    /* -- 0x7b PlayMetaFile(hdc, hmf) -- s90, #295. Write and Paintbrush import it.
     * The metafile is one of OUR tokens (CloseMetaFile/CopyMetaFile made it), so
     * this is Win32's own PlayMetaFile on the two real handles. Checked to be a
     * metafile by the OS's own GetObjectType, not by token kind alone -- a pen
     * is an OBJ token too.
     */
    case WOWGDI_PLAYMETAFILE:
    {
        WORD dc16 = Wow32ArgWord(frame, WOWGDI_PMF_ARG_HDC);
        WORD token = Wow32ArgWord(frame, WOWGDI_PMF_ARG_HMF);
        INT  objectKind = -1, metafileKind = -1, noteLength = 0, result = 0;
        HGDIOBJ dc = WowGdiH32(dc16, &objectKind);
        HGDIOBJ metafile = WowGdiH32(token, &metafileKind);
        WowNotePut(note, noteCapacity, &noteLength, "PlayMetaFile(0x");
        WowNoteHex(note, noteCapacity, &noteLength, dc16, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, ", 0x");
        WowNoteHex(note, noteCapacity, &noteLength, token, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, ")");
        if (!dc || !(objectKind == WOWGDI_KIND_DC || objectKind == WOWGDI_KIND_WINDC))
        {
            WowNotePut(note, noteCapacity, &noteLength, " -- ★ NOT ONE OF OUR DC TOKENS; FALSE");
        }
        else if (!metafile || metafileKind != WOWGDI_KIND_OBJ || GetObjectType(metafile) != OBJ_METAFILE)
        {
            WowNotePut(note, noteCapacity, &noteLength, " -- ★ NOT ONE OF OUR METAFILE TOKENS; FALSE");
        }
        else
        {
            result = PlayMetaFile((HDC)dc, (HMETAFILE)metafile) ? 1 : 0;
            WowNotePut(note, noteCapacity, &noteLength, result ? " -> played" : " -> FAILED");
        }
        Wow32SetReturn(frame, (DWORD)result);
        return 1;
    }

    /* -- 0xaf EnumMetaFile(hdc, hmf, lpfn, lParam) -- #295. CARDFILE, PACKAGER
     * and WRITE import it (through OLECLI). The metafile is snapshot ONCE, as
     * bytes (GetMetaFileBitsEx), and walked by wowconv.h's pure parser; each
     * record is one callback on the wowenum.h chain -- see g_WowGdiMetafile above for the
     * handle table. The answer is 1 up front and revised to 0 by a stop.
     */
    case WOWGDI_ENUMMETAFILE:
    {
        WORD  dc16  = Wow32ArgWord(frame, WOWGDI_EMF_ARG_HDC);
        WORD  token  = Wow32ArgWord(frame, WOWGDI_EMF_ARG_HMF);
        DWORD procedure = Wow32ArgDword(frame, WOWGDI_EMF_ARG_PROC);
        DWORD lParam   = Wow32ArgDword(frame, WOWGDI_EMF_ARG_LPARAM);
        INT   objectKind = -1, metafileKind = -1, noteLength = 0;
        HGDIOBJ dc = WowGdiH32(dc16, &objectKind);
        HGDIOBJ metafile = WowGdiH32(token, &metafileKind);
        UINT  itemCount;
        PBYTE bits;
        UINT objectCount = 0;
        unsigned long firstRecord, recordsEnd = 0;
        WowNotePut(note, noteCapacity, &noteLength, "EnumMetaFile(0x");
        WowNoteHex(note, noteCapacity, &noteLength, dc16, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, ", 0x");
        WowNoteHex(note, noteCapacity, &noteLength, token, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, ")");
        /* [CAUTION]: EVERY REFUSAL BELOW ANSWERS 0, callbacks-off included. EnumObjects keeps
         * its TRUE when callbacks are off; a metafile enumerator that says "done"
         * having shown the guest no record would let it conclude the picture is
         * empty, which is the wrong answer rather than a missing one.
         */
        Wow32SetReturn(frame, 0);
        if (!metafile || metafileKind != WOWGDI_KIND_OBJ || GetObjectType(metafile) != OBJ_METAFILE)
        {
            WowNotePut(note, noteCapacity, &noteLength, " -- ★ NOT ONE OF OUR METAFILE TOKENS; 0");
            return 1;
        }
        if (!frame->IsCallbackAllowed)
        {
            WowNotePut(note, noteCapacity, &noteLength, " -- callbacks are not armed; 0");
            return 1;
        }
        if (WowEnumBusy() || g_WowGdiMetafile.IsActive)
        {
            WowNotePut(note, noteCapacity, &noteLength, " -- ★ AN ENUMERATION IS ALREADY RUNNING; REFUSED");
            return 1;
        }
        itemCount = GetMetaFileBitsEx((HMETAFILE)metafile, 0, NULL);
        bits = itemCount ? (PBYTE)HeapAlloc(GetProcessHeap(), 0, itemCount) : NULL;
        if (!bits || GetMetaFileBitsEx((HMETAFILE)metafile, itemCount, bits) != itemCount)
        {
            if (bits)
                HeapFree(GetProcessHeap(), 0, bits);
            WowNotePut(note, noteCapacity, &noteLength, " -- ★ GetMetaFileBitsEx FAILED; 0");
            return 1;
        }
        firstRecord = WowConvMetafileHeader(bits, itemCount, &objectCount, &recordsEnd);
        WowNotePut(note, noteCapacity, &noteLength, " bytes=0x");
        WowNoteHex(note, noteCapacity, &noteLength, itemCount, WOWGDI_HEX_SIZE_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, " nObj=0x");
        WowNoteHex(note, noteCapacity, &noteLength, objectCount, WOW_HEX_WORD_DIGITS);
        if (!firstRecord || objectCount > WOWMF_MAXOBJ)
        {
            HeapFree(GetProcessHeap(), 0, bits);
            WowNotePut(note, noteCapacity, &noteLength, !firstRecord ? " -- ★ NOT A WMF HEADER; 0"
                                              : " -- ★ MORE OBJECTS THAN WOWMF_MAXOBJ; REFUSED, 0");
            return 1;
        }
        if (!WowEnumBegin(WOWENUM_METAFILE, procedure, frame->GuestDataSelector, lParam,
                           (DWORD)(ULONG_PTR)(frame->FrameBase + WOW32_OFF_RET), dc16))
        {
            HeapFree(GetProcessHeap(), 0, bits);
            WowNotePut(note, noteCapacity, &noteLength, " -- ★ the callback is not a usable far pointer; 0");
            return 1;
        }
        memset(&g_WowGdiMetafile, 0, sizeof g_WowGdiMetafile);
        g_WowGdiMetafile.IsActive = 1;
        g_WowGdiMetafile.Bits = bits;
        g_WowGdiMetafile.Length = itemCount;
        g_WowGdiMetafile.End = recordsEnd;
        g_WowGdiMetafile.Offset = firstRecord;
        g_WowGdiMetafile.ObjectCount = objectCount;
        g_WowGdiMetafile.Dc16 = dc16;
        /* The guest's hdc is passed to every callback verbatim; it only has to be
         * one of ours for the objects to be put back at the end.
         */
        if (dc && (objectKind == WOWGDI_KIND_DC || objectKind == WOWGDI_KIND_WINDC))
        {
            g_WowGdiMetafile.Dc = (HDC)dc;
            g_WowGdiMetafile.OriginalPen   = GetCurrentObject((HDC)dc, OBJ_PEN);
            g_WowGdiMetafile.OriginalBrush = GetCurrentObject((HDC)dc, OBJ_BRUSH);
            g_WowGdiMetafile.OriginalFont  = GetCurrentObject((HDC)dc, OBJ_FONT);
        }
        Wow32SetReturn(frame, 1);
        frame->IsEnumerationRequested = 1;
        return 1;
    }

    /* -- 0xb0 PlayMetaFileRecord(hdc, lpht, lpmr, nHandles) -- #295. Win32's own
     * PlayMetaFileRecord on an HGDIOBJ table built from the guest's TOKENS; any
     * entry the record changed goes back as a token -- a new object gets one, a
     * META_DELETEOBJECT'd one (Win32 deletes it and zeroes the slot) has its token
     * forgotten. [CAUTION] Win16 declares this VOID (3.1 SDK, Wine's gdi.exe.spec); the
     * BOOL written to the return hole is Win32's and is unmeasured against stock.
     */
    case WOWGDI_PLAYMETAFILEREC:
    {
        WORD  dc16 = Wow32ArgWord(frame, WOWGDI_PMFR_ARG_HDC);
        WORD  handleCount  = Wow32ArgWord(frame, WOWGDI_PMFR_ARG_NHANDLES);
        DWORD recordFar = Wow32ArgDword(frame, WOWGDI_PMFR_ARG_MR);
        volatile BYTE *recordGuest = Wow32ArgPointer(frame, WOWGDI_PMFR_ARG_MR);
        volatile BYTE *handleTable = Wow32ArgPointer(frame, WOWGDI_PMFR_ARG_HT);
        INT   objectKind = -1, noteLength = 0, result = 0, index, created = 0, deleted = 0, isFull = 0;
        HGDIOBJ dc = WowGdiH32(dc16, &objectKind);
        DWORD recordWords = 0, bytes;
        PCBYTE record;
        WORD  tokens[WOWMF_MAXOBJ];
        HGDIOBJ objectsBefore[WOWMF_MAXOBJ];
        WowNotePut(note, noteCapacity, &noteLength, "PlayMetaFileRecord(0x");
        WowNoteHex(note, noteCapacity, &noteLength, dc16, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, ", n=0x");
        WowNoteHex(note, noteCapacity, &noteLength, handleCount, WOW_HEX_WORD_DIGITS);
        Wow32SetReturn(frame, 0);
        if (!dc || !(objectKind == WOWGDI_KIND_DC || objectKind == WOWGDI_KIND_WINDC))
        {
            WowNotePut(note, noteCapacity, &noteLength, ") -- ★ NOT ONE OF OUR DC TOKENS; FALSE");
            return 1;
        }
        if (!recordGuest || (handleCount && !handleTable) || handleCount > WOWMF_MAXOBJ)
        {
            WowNotePut(note, noteCapacity, &noteLength, handleCount > WOWMF_MAXOBJ
                ? ") -- ★ MORE HANDLES THAN WOWMF_MAXOBJ; REFUSED, FALSE"
                : ") -- ★ A NULL RECORD OR TABLE POINTER; FALSE");
            return 1;
        }
        recordWords = (DWORD)WowGdiPeek(recordGuest, 0) | ((DWORD)WowGdiPeek(recordGuest, WOW_WORD_BYTES) << WORD_SHIFT);
        WowNotePut(note, noteCapacity, &noteLength, ", fn=0x");
        WowNoteHex(note, noteCapacity, &noteLength, WowGdiPeek(recordGuest, WOWCONV_MF_FUNCTION_FIELD), WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, " size=0x");
        WowNoteHex(note, noteCapacity, &noteLength, recordWords, WOWGDI_HEX_SIZE_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, ")");
        if (recordWords < WOWCONV_MF_RECORD_MIN_WORDS)
        {
            WowNotePut(note, noteCapacity, &noteLength, " -- ★ rdSize < 3; FALSE");
            return 1;
        }
        /* [INFO]: A RECORD WHOSE CALLBACK COPY WAS CUT SHORT is played from the snapshot,
         * recognised by ADDRESS: it is the record the enumeration is in.
         */
        if (g_WowGdiMetafile.IsActive && g_WowGdiMetafile.IsTruncated && g_WowGdiMetafile.RecordLinear
            && (DWORD)(ULONG_PTR)recordGuest == g_WowGdiMetafile.RecordLinear && recordWords * WOWCONV_BYTES_PER_WORD == g_WowGdiMetafile.RecordBytes)
        {
            record = g_WowGdiMetafile.Bits + g_WowGdiMetafile.RecordOffset;
            WowNotePut(note, noteCapacity, &noteLength, " [the truncated record, played from the snapshot]");
        }
        else
        {
            if (recordWords > WOWGDI_MF_RECORD_MAX_WORDS || (recordFar & WORD_MASK) + recordWords * WOWCONV_BYTES_PER_WORD > WOWGDI_SEGMENT_SIZE)
            {
                WowNotePut(note, noteCapacity, &noteLength, " -- ★ THE RECORD RUNS PAST ITS SEGMENT"
                                           " (a huge record); REFUSED, FALSE");
                return 1;
            }
            bytes = recordWords * WOWCONV_BYTES_PER_WORD;
            for (index = 0; index < (INT)bytes; ++index)
                g_WowGdiMetafileRecord[index] = recordGuest[index];
            record = g_WowGdiMetafileRecord;
        }
        for (index = 0; index < (INT)handleCount; ++index)
        {
            INT regionKind = -1;
            HGDIOBJ object;
            tokens[index] = WowGdiPeek(handleTable, index * WOW_WORD_BYTES);
            object = tokens[index] ? WowGdiH32(tokens[index], &regionKind) : NULL;
            if (object && (regionKind == WOWGDI_KIND_DC || regionKind == WOWGDI_KIND_WINDC))
                object = NULL;
            g_WowGdiMetafileHandles[index] = objectsBefore[index] = object;
        }
        result = PlayMetaFileRecord((HDC)dc, (HANDLETABLE *)g_WowGdiMetafileHandles,
                               (METARECORD *)(VOID *)record, handleCount) ? 1 : 0;
        for (index = 0; index < (INT)handleCount; ++index)
        {
            if (g_WowGdiMetafileHandles[index] == objectsBefore[index])
                continue;
            if (g_WowGdiMetafileHandles[index])
            {
                WORD newToken = WowGdiH16(g_WowGdiMetafileHandles[index], WOWGDI_KIND_OBJ);
                if (!newToken)
                    ++isFull;
                Wow32PokeWord(handleTable + index * WOW_WORD_BYTES, newToken);
                ++created;
            }
            else
            {
                if (tokens[index])
                    WowGdiForget(tokens[index]);
                Wow32PokeWord(handleTable + index * WOW_WORD_BYTES, 0);
                ++deleted;
            }
        }
        WowNotePut(note, noteCapacity, &noteLength, result ? " -> played" : " -> FAILED");
        if (created) { WowNotePut(note, noteCapacity, &noteLength, ", +0x");
        WowNoteHex(note, noteCapacity, &noteLength, (DWORD)created, WOW_HEX_BYTE_DIGITS);
                    WowNotePut(note, noteCapacity, &noteLength, " object(s)"); }
        if (deleted) { WowNotePut(note, noteCapacity, &noteLength, ", -0x");
        WowNoteHex(note, noteCapacity, &noteLength, (DWORD)deleted, WOW_HEX_BYTE_DIGITS);
                    WowNotePut(note, noteCapacity, &noteLength, " deleted"); }
        if (isFull) WowNotePut(note, noteCapacity, &noteLength, " -- ★★ THE TOKEN MAP IS FULL: a new object got"
                                             " token 0 and is unreachable (leaked)");
        Wow32SetReturn(frame, (DWORD)result);
        return 1;
    }

    case WOWGDI_COPYMETAFILE:
    {
        WORD token = Wow32ArgWord(frame, WOWGDI_CPMF_ARG_HMF);
        INT  kind = -1;
        HGDIOBJ object = WowGdiH32(token, &kind);
        CHAR path[MAX_PATH];
        HMETAFILE metafile;
        WORD metafileToken;
        INT  noteLength = 0, isNamed;
        path[0] = 0;
        isNamed = Wow32ArgString(frame, WOWGDI_CPMF_ARG_FILE, path, (INT)sizeof path);
        WowNotePut(note, noteCapacity, &noteLength, "CopyMetaFile(0x");
        WowNoteHex(note, noteCapacity, &noteLength, token, WOW_HEX_WORD_DIGITS);
        if (!object || kind != WOWGDI_KIND_OBJ)
        {
            WowNotePut(note, noteCapacity, &noteLength, ") -- ★ NOT ONE OF OUR METAFILE TOKENS; 0");
            Wow32SetReturn(frame, 0);
            return 1;
        }
        metafile  = CopyMetaFileA((HMETAFILE)object, (isNamed && path[0]) ? path : NULL);
        metafileToken = metafile ? WowGdiH16((HGDIOBJ)metafile, WOWGDI_KIND_OBJ) : 0;
        WowNotePut(note, noteCapacity, &noteLength, ") -> 0x");
        WowNoteHex(note, noteCapacity, &noteLength, metafileToken, WOW_HEX_WORD_DIGITS);
        Wow32SetReturn(frame, (DWORD)metafileToken);
        return 1;
    }

    case WOWGDI_EXTTEXTOUT:
    {
        WORD dc16  = Wow32ArgWord(frame, WOWGDI_ETO_ARG_HDC);
        INT  positionX    = (INT)(SHORT)Wow32ArgWord(frame, WOWGDI_ETO_ARG_X);
        INT  positionY    = (INT)(SHORT)Wow32ArgWord(frame, WOWGDI_ETO_ARG_Y);
        WORD options = Wow32ArgWord(frame, WOWGDI_ETO_ARG_OPTS);
        WORD itemCount    = Wow32ArgWord(frame, WOWGDI_ETO_ARG_COUNT);
        volatile BYTE *string = Wow32ArgPointer(frame, WOWGDI_ETO_ARG_STR);
        volatile BYTE *rectBytes = Wow32ArgPointer(frame, WOWGDI_ETO_ARG_RECT);
        volatile BYTE *spacingBytes = Wow32ArgPointer(frame, WOWGDI_ETO_ARG_DX);
        INT  kind = -1;
        HGDIOBJ object = WowGdiH32(dc16, &kind);
        CHAR buffer[WOWGDI_TEXT_MAX];
        INT  spacing[WOWGDI_TEXT_MAX];
        RECT regionResult, *clipRect = NULL;
        INT  noteLength = 0, index, count = (INT)itemCount, isOk;
        WowNotePut(note, noteCapacity, &noteLength, "ExtTextOut(0x");
        WowNoteHex(note, noteCapacity, &noteLength, dc16, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, ", ");
        WowNoteHex(note, noteCapacity, &noteLength, (DWORD)positionX, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, ",");
        WowNoteHex(note, noteCapacity, &noteLength, (DWORD)positionY, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, " opts=0x");
        WowNoteHex(note, noteCapacity, &noteLength, options, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, " n=");
        WowNoteHex(note, noteCapacity, &noteLength, (DWORD)itemCount, WOW_HEX_WORD_DIGITS);
        if (!object || (kind != WOWGDI_KIND_DC && kind != WOWGDI_KIND_WINDC))
        {
            WowNotePut(note, noteCapacity, &noteLength, " -- ★ NOT ONE OF OUR DC TOKENS; answered 0");
            Wow32SetReturn(frame, 0);
            return 1;
        }
        if (count < 0)
            count = 0;
        if (count > (INT)sizeof buffer)
            count = (INT)sizeof buffer;
        for (index = 0; index < count; ++index)
            buffer[index] = string ? (CHAR)string[index] : ' ';
        if (rectBytes)
        {
            BYTE rect16[WOWCONV_RECT16_SIZE];
            for (index = 0; index < WOWCONV_RECT16_SIZE; ++index)
                rect16[index] = (BYTE)rectBytes[index];
            regionResult.left   = WowConvRect16Get(rect16, WOWCONV_RECT16_LEFT);
            regionResult.top    = WowConvRect16Get(rect16, WOWCONV_RECT16_TOP);
            regionResult.right  = WowConvRect16Get(rect16, WOWCONV_RECT16_RIGHT);
            regionResult.bottom = WowConvRect16Get(rect16, WOWCONV_RECT16_BOTTOM);
            clipRect = &regionResult;
            WowNotePut(note, noteCapacity, &noteLength, " rect=");
            WowNoteHex(note, noteCapacity, &noteLength, (DWORD)regionResult.left, WOW_HEX_WORD_DIGITS);
            WowNotePut(note, noteCapacity, &noteLength, ",");
            WowNoteHex(note, noteCapacity, &noteLength, (DWORD)regionResult.top, WOW_HEX_WORD_DIGITS);
            WowNotePut(note, noteCapacity, &noteLength, "-");
            WowNoteHex(note, noteCapacity, &noteLength, (DWORD)regionResult.right, WOW_HEX_WORD_DIGITS);
            WowNotePut(note, noteCapacity, &noteLength, ",");
            WowNoteHex(note, noteCapacity, &noteLength, (DWORD)regionResult.bottom, WOW_HEX_WORD_DIGITS);
        }
        if (spacingBytes && count)
        {
            for (index = 0; index < count; ++index)
                spacing[index] = (INT)(SHORT)((WORD)spacingBytes[index * WOW_WORD_BYTES] | ((WORD)spacingBytes[index * WOW_WORD_BYTES + 1] << BYTE_SHIFT));
            WowNotePut(note, noteCapacity, &noteLength, " +spacing");
        }
        isOk = ExtTextOutA((HDC)object, positionX, positionY, (UINT)options, clipRect,
                         count ? buffer : NULL, (UINT)count,
                         (spacingBytes && count) ? spacing : NULL) ? 1 : 0;
        WowNotePut(note, noteCapacity, &noteLength, isOk ? " -> drawn" : " -> REFUSED by GDI");
        Wow32SetReturn(frame, (DWORD)isOk);
        return 1;
    }

    case WOWGDI_TEXTOUT:
    case WOWGDI_GETTEXTEXTENT:
    {
        INT  isTextOut = (frame->Id == WOWGDI_TEXTOUT);
        WORD dc16 = Wow32ArgWord(frame, isTextOut ? WOWGDI_TO_ARG_HDC   : WOWGDI_TE_ARG_HDC);
        WORD itemCount   = Wow32ArgWord(frame, isTextOut ? WOWGDI_TO_ARG_COUNT : WOWGDI_TE_ARG_COUNT);
        volatile BYTE *text = Wow32ArgPointer(frame, isTextOut ? WOWGDI_TO_ARG_STR : WOWGDI_TE_ARG_STR);
        INT  positionX = isTextOut ? (INT)(SHORT)Wow32ArgWord(frame, WOWGDI_TO_ARG_X) : 0;
        INT  positionY = isTextOut ? (INT)(SHORT)Wow32ArgWord(frame, WOWGDI_TO_ARG_Y) : 0;
        INT  kind = -1;
        HGDIOBJ object = WowGdiH32(dc16, &kind);
        CHAR buffer[WOWGDI_TEXT_MAX];
        INT  noteLength = 0, index, count = (INT)itemCount;
        SIZE size;
        WowNotePut(note, noteCapacity, &noteLength, isTextOut ? "TextOut(0x" : "GetTextExtent(0x");
        WowNoteHex(note, noteCapacity, &noteLength, dc16, WOW_HEX_WORD_DIGITS);
        if (isTextOut)
        {
            WowNotePut(note, noteCapacity, &noteLength, ", ");
            WowNoteHex(note, noteCapacity, &noteLength, (DWORD)positionX, WOW_HEX_WORD_DIGITS);
            WowNotePut(note, noteCapacity, &noteLength, ",");
            WowNoteHex(note, noteCapacity, &noteLength, (DWORD)positionY, WOW_HEX_WORD_DIGITS);
        }
        WowNotePut(note, noteCapacity, &noteLength, ", ");
        WowNoteHex(note, noteCapacity, &noteLength, (DWORD)itemCount, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, " chars)");
        if (!object || (kind != WOWGDI_KIND_DC && kind != WOWGDI_KIND_WINDC) || !text)
        {
            WowNotePut(note, noteCapacity, &noteLength, " -- ★ NOT ONE OF OUR DC TOKENS, or no"
                                       " string; answered 0");
            Wow32SetReturn(frame, 0);
            return 1;
        }
        if (count < 0)
            count = 0;
        if (count > (INT)sizeof buffer)
            count = (INT)sizeof buffer;
        for (index = 0; index < count; ++index)
            buffer[index] = (CHAR)text[index];
        if (isTextOut)
        {
            INT result = TextOutA((HDC)object, positionX, positionY, buffer, count) ? 1 : 0;
            WowNotePut(note, noteCapacity, &noteLength, result ? " -> drawn" : " -- ★ the OS refused it");
            Wow32SetReturn(frame, (DWORD)result);
            return 1;
        }
        size.cx = size.cy = 0;
        GetTextExtentPoint32A((HDC)object, buffer, count, &size);
        WowNotePut(note, noteCapacity, &noteLength, " = ");
        WowNoteHex(note, noteCapacity, &noteLength, (DWORD)size.cx, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, "x");
        WowNoteHex(note, noteCapacity, &noteLength, (DWORD)size.cy, WOW_HEX_WORD_DIGITS);
        Wow32SetReturn(frame, ((DWORD)(WORD)size.cy << WORD_SHIFT) | (DWORD)(WORD)size.cx);
        return 1;
    }

    /* -- 0x5d GetTextMetrics -- 31 bytes of `short`, layout checked on
     * Notepad (see the note by the defines). --------------------------------
     *
     * [CAUTION]: THE WHOLE STRUCTURE IS WRITTEN, INCLUDING THE FIELDS NOTEPAD DOES NOT
     * READ. A guest that finds a stale byte at tmPitchAndFamily picks a font
     * for reasons nothing in the log explains.
     */
    case WOWGDI_GETTEXTMETRICS:
    {
        WORD dc16 = Wow32ArgWord(frame, WOWGDI_TM_ARG_HDC);
        volatile BYTE *destination = Wow32ArgPointer(frame, WOWGDI_TM_ARG_BUF);
        INT  kind = -1;
        HGDIOBJ object = WowGdiH32(dc16, &kind);
        TEXTMETRICA textMetric;
        INT noteLength = 0, index;
        BYTE blob[WOWGDI_TEXTMETRIC16_SIZE];
        WowNotePut(note, noteCapacity, &noteLength, "GetTextMetrics(0x");
        WowNoteHex(note, noteCapacity, &noteLength, dc16, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, ")");
        if (!object || (kind != WOWGDI_KIND_DC && kind != WOWGDI_KIND_WINDC) || !destination)
        {
            WowNotePut(note, noteCapacity, &noteLength, " -- ★ NOT ONE OF OUR DC TOKENS, or no"
                                       " buffer; answered 0");
            Wow32SetReturn(frame, 0);
            return 1;
        }
        if (!GetTextMetricsA((HDC)object, &textMetric))
        {
            WowNotePut(note, noteCapacity, &noteLength, " -- ★ the OS refused it; answered 0");
            Wow32SetReturn(frame, 0);
            return 1;
        }
        for (index = 0; index < (INT)sizeof blob; ++index)
            blob[index] = 0;
        Wow32PokeWord(blob +  0, (WORD)(SHORT)textMetric.tmHeight);
        Wow32PokeWord(blob + WOWGDI_NTM16_ASCENT, (WORD)(SHORT)textMetric.tmAscent);
        Wow32PokeWord(blob + WOWGDI_NTM16_DESCENT, (WORD)(SHORT)textMetric.tmDescent);
        Wow32PokeWord(blob + WOWGDI_NTM16_INTERNALLEADING, (WORD)(SHORT)textMetric.tmInternalLeading);
        Wow32PokeWord(blob + WOWGDI_NTM16_EXTERNALLEADING, (WORD)(SHORT)textMetric.tmExternalLeading);
        Wow32PokeWord(blob + WOWGDI_NTM16_AVECHARWIDTH, (WORD)(SHORT)textMetric.tmAveCharWidth);
        Wow32PokeWord(blob + WOWGDI_NTM16_MAXCHARWIDTH, (WORD)(SHORT)textMetric.tmMaxCharWidth);
        Wow32PokeWord(blob + WOWGDI_NTM16_WEIGHT, (WORD)(SHORT)textMetric.tmWeight);
        /* s91: THE WINDOWS 3.1 ORDER from +16 on -- the BYTE fields come before the
         * last three shorts, exactly as EnumFonts' NEWTEXTMETRIC16 (wowgdi_font_blob,
         * = stock in w_genum) lays them out. This wrote a Win32-like order (Overhang
         * at +16, the chars at +22) since s45: tests/probes/win16/w_tm vs stock showed
         * PitchAndFamily, CharSet and Overhang wrong for every font.
         */
        blob[WOWGDI_NTM16_ITALIC] = textMetric.tmItalic;
        blob[WOWGDI_NTM16_UNDERLINED] = textMetric.tmUnderlined;
        blob[WOWGDI_NTM16_STRUCKOUT] = textMetric.tmStruckOut;
        blob[WOWGDI_NTM16_FIRSTCHAR] = (BYTE)textMetric.tmFirstChar;
        blob[WOWGDI_NTM16_LASTCHAR] = (BYTE)textMetric.tmLastChar;
        blob[WOWGDI_NTM16_DEFAULTCHAR] = (BYTE)textMetric.tmDefaultChar;
        blob[WOWGDI_NTM16_BREAKCHAR] = (BYTE)textMetric.tmBreakChar;
        blob[WOWGDI_NTM16_PITCHANDFAMILY] = textMetric.tmPitchAndFamily;
        blob[WOWGDI_NTM16_CHARSET] = textMetric.tmCharSet;
        Wow32PokeWord(blob + WOWGDI_NTM16_OVERHANG, (WORD)(SHORT)textMetric.tmOverhang);
        Wow32PokeWord(blob + WOWGDI_NTM16_DIGITIZEDASPECTX, (WORD)(SHORT)textMetric.tmDigitizedAspectX);
        Wow32PokeWord(blob + WOWGDI_NTM16_DIGITIZEDASPECTY, (WORD)(SHORT)textMetric.tmDigitizedAspectY);
        for (index = 0; index < (INT)sizeof blob; ++index)
            destination[index] = blob[index];
        WowNotePut(note, noteCapacity, &noteLength, " h=");
        WowNoteHex(note, noteCapacity, &noteLength, (DWORD)(WORD)(SHORT)textMetric.tmHeight, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, " extlead=");
        WowNoteHex(note, noteCapacity, &noteLength, (DWORD)(WORD)(SHORT)textMetric.tmExternalLeading, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, " avew=");
        WowNoteHex(note, noteCapacity, &noteLength, (DWORD)(WORD)(SHORT)textMetric.tmAveCharWidth, WOW_HEX_WORD_DIGITS);
        Wow32SetReturn(frame, 1);
        return 1;
    }

    /* 0x39 CreateFontIndirect -- the Win16 LOGFONT, read the other way (Importance = 1):
     * The 50-byte layout is the one `GetObject` already writes: 18 bytes of
     * header and a 32-byte face name. This is the same table used backwards,
     * which is why there is no second reading of it to get wrong.
     */
    case WOWGDI_CREATEFONTIND:
    {
        volatile BYTE *argument = Wow32ArgPointer(frame, 0);
        LOGFONTA logFont;
        INT noteLength = 0, index;
        HFONT font;
        WORD token;
        WowNotePut(note, noteCapacity, &noteLength, "CreateFontIndirect(");
        if (!argument)
        {
            WowNotePut(note, noteCapacity, &noteLength, "NULL) -- ★ no LOGFONT; answered 0");
            Wow32SetReturn(frame, 0);
            return 1;
        }
        for (index = 0; index < (INT)sizeof logFont; ++index)
            ((PBYTE)&logFont)[index] = 0;
        logFont.lfHeight     = (LONG)(SHORT)Wow32PeekWord(argument + 0);
        logFont.lfWidth      = (LONG)(SHORT)Wow32PeekWord(argument + WOWGDI_LF16_WIDTH);
        logFont.lfEscapement = (LONG)(SHORT)Wow32PeekWord(argument + WOWGDI_LF16_ESCAPEMENT);
        logFont.lfOrientation= (LONG)(SHORT)Wow32PeekWord(argument + WOWGDI_LF16_ORIENTATION);
        logFont.lfWeight     = (LONG)(SHORT)Wow32PeekWord(argument + WOWGDI_LF16_WEIGHT);
        logFont.lfItalic     = argument[WOWGDI_LF16_ITALIC];
        logFont.lfUnderline     = argument[WOWGDI_LF16_UNDERLINE];
        logFont.lfStrikeOut  = argument[WOWGDI_LF16_STRIKEOUT];
        logFont.lfCharSet       = argument[WOWGDI_LF16_CHARSET];
        logFont.lfOutPrecision = argument[WOWGDI_LF16_OUTPRECISION];
        logFont.lfClipPrecision = argument[WOWGDI_LF16_CLIPPRECISION];
        logFont.lfQuality      = argument[WOWGDI_LF16_QUALITY];
        logFont.lfPitchAndFamily = argument[WOWGDI_LF16_PITCHANDFAMILY];
        for (index = 0; index < WOWGDI_LF16_FACESIZE - 1; ++index)
        {
            BYTE character = argument[WOWGDI_LF16_FACENAME + index];
            logFont.lfFaceName[index] = (CHAR)character;
            if (!character)
                break;
        }
        logFont.lfFaceName[WOWGDI_LF16_FACESIZE - 1] = 0;
        WowNoteQuoted(note, noteCapacity, &noteLength, logFont.lfFaceName);
        WowNotePut(note, noteCapacity, &noteLength, " h=");
        WowNoteHex(note, noteCapacity, &noteLength, (DWORD)(WORD)(SHORT)logFont.lfHeight, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, ")");
        font = CreateFontIndirectA(&logFont);
        token = font ? WowGdiH16((HGDIOBJ)font, WOWGDI_KIND_OBJ) : 0;
        if (!token)
        {
            if (font)
                DeleteObject((HGDIOBJ)font);
            WowNotePut(note, noteCapacity, &noteLength, " -- ★ no font (or the token map is full);"
                                       " answered 0");
            Wow32SetReturn(frame, 0);
            return 1;
        }
        WowNotePut(note, noteCapacity, &noteLength, " -> font token 0x");
        WowNoteHex(note, noteCapacity, &noteLength, token, WOW_HEX_WORD_DIGITS);
        Wow32SetReturn(frame, token);
        return 1;
    }

    /* -- 0x4a GetBitmapBits / 0x6a SetBitmapBits -- PBRUSH.DLL's own pair. -
     *
     * [CAUTION]: THE COUNT IS A DWORD AND IT IS THE GUEST'S. It is passed through
     * unchanged and Windows bounds it against the bitmap; clamping it here
     * would silently truncate an image the guest believes it wrote. What IS
     * checked is that the far pointer resolves, because a bad selector is our
     * problem rather than the guest's.
     */
    case WOWGDI_GETBITMAPBITS:
    case WOWGDI_SETBITMAPBITS:
    {
        INT   isGet = (frame->Id == WOWGDI_GETBITMAPBITS);
        WORD  bitmap16   = Wow32ArgWord(frame, WOWGDI_BB2_ARG_HBM);
        DWORD count   = Wow32ArgDword(frame, WOWGDI_BB2_ARG_COUNT);
        volatile BYTE *bits = Wow32ArgPointer(frame, WOWGDI_BB2_ARG_BITS);
        INT   kind = -1;
        HGDIOBJ object = WowGdiH32(bitmap16, &kind);
        INT   noteLength = 0;
        LONG  returned;
        WowNotePut(note, noteCapacity, &noteLength, isGet ? "GetBitmapBits(0x" : "SetBitmapBits(0x");
        WowNoteHex(note, noteCapacity, &noteLength, bitmap16, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, ", ");
        WowNoteHex(note, noteCapacity, &noteLength, count, WOW_HEX_DWORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, " bytes)");
        if (!object || (kind != WOWGDI_KIND_OBJ && kind != WOWGDI_KIND_STOCK) || !bits)
        {
            WowNotePut(note, noteCapacity, &noteLength, " -- ★ NOT ONE OF OUR BITMAP TOKENS, or no"
                                       " buffer; answered 0");
            Wow32SetReturn(frame, 0);
            return 1;
        }
        returned = isGet ? GetBitmapBits((HBITMAP)object, (LONG)count, (LPVOID)(ULONG_PTR)bits)
                    : SetBitmapBits((HBITMAP)object, (DWORD)count, (const VOID *)(ULONG_PTR)bits);
        WowNotePut(note, noteCapacity, &noteLength, " -> ");
        WowNoteHex(note, noteCapacity, &noteLength, (DWORD)returned, WOW_HEX_DWORD_DIGITS);
        Wow32SetReturn(frame, (DWORD)returned);
        return 1;
    }

    /* 0x1b8 SetDIBits / 0x1b9 GetDIBits / 0x1b7 StretchDIBits (Importance = 5):
     * **This is what `File > Save As` needs.** MS Paint shows the common
     * dialog, gets a filename back, and then has to turn its off-screen
     * bitmap into the rows of a .BMP -- which is `GetDIBits`, and it was
     * answered with the harness sentinel.
     *
     * [INFO]: NO CONVERSION IS NEEDED AND THAT IS A READING, NOT AN ASSUMPTION. A
     * `BITMAPINFOHEADER` is 40 bytes with the same fields in the same order in
     * both worlds (Win16 wrote the format Win32 inherited), the colour table
     * that follows it is RGBQUADs either way, and the bits are bytes. Both
     * pointers are into the VDM's own memory, which this host can address
     * directly, so they go to Win32 as they are.
     *
     * [CAUTION]: `lpvBits == NULL` IS A QUERY, not an error: GetDIBits with a null bits
     * pointer fills in the header and returns the scan-line count, and a guest
     * uses that to size its buffer. Refusing it would break the call BEFORE
     * the one that matters.
     */
    case WOWGDI_SETDIBITS:
    case WOWGDI_GETDIBITS:
    {
        INT   isGet = (frame->Id == WOWGDI_GETDIBITS);
        WORD  dc16   = Wow32ArgWord(frame, WOWGDI_DIB_ARG_HDC);
        WORD  bitmap16   = Wow32ArgWord(frame, WOWGDI_DIB_ARG_HBM);
        WORD  startIndex = Wow32ArgWord(frame, WOWGDI_DIB_ARG_START);
        WORD  lineCount = Wow32ArgWord(frame, WOWGDI_DIB_ARG_LINES);
        WORD  usage = Wow32ArgWord(frame, WOWGDI_DIB_ARG_USAGE);
        volatile BYTE *bits = Wow32ArgPointer(frame, WOWGDI_DIB_ARG_BITS);
        volatile BYTE *bitmapInfo  = Wow32ArgPointer(frame, WOWGDI_DIB_ARG_BMI);
        INT   dcKind = -1, bitmapKind = -1;
        HGDIOBJ dcObject = WowGdiH32(dc16, &dcKind);
        HGDIOBJ bitmapObject = WowGdiH32(bitmap16, &bitmapKind);
        INT   noteLength = 0, result;
        WowNotePut(note, noteCapacity, &noteLength, isGet ? "GetDIBits(dc 0x" : "SetDIBits(dc 0x");
        WowNoteHex(note, noteCapacity, &noteLength, dc16, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, ", bm 0x");
        WowNoteHex(note, noteCapacity, &noteLength, bitmap16, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, ", scan ");
        WowNoteHex(note, noteCapacity, &noteLength, (DWORD)startIndex, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, "+");
        WowNoteHex(note, noteCapacity, &noteLength, (DWORD)lineCount, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, bits ? ")" : ", bits=NULL = a size QUERY)");
        if (!dcObject || (dcKind != WOWGDI_KIND_DC && dcKind != WOWGDI_KIND_WINDC)
               || !bitmapObject || (bitmapKind != WOWGDI_KIND_OBJ && bitmapKind != WOWGDI_KIND_STOCK) || !bitmapInfo)
        {
            WowNotePut(note, noteCapacity, &noteLength, " -- ★ NOT OUR DC/BITMAP TOKENS, or no"
                                       " BITMAPINFO; answered 0");
            Wow32SetReturn(frame, 0);
            return 1;
        }
        result = isGet
            ? GetDIBits((HDC)dcObject, (HBITMAP)bitmapObject, startIndex, lineCount,
                        (LPVOID)(ULONG_PTR)bits, (BITMAPINFO *)(ULONG_PTR)bitmapInfo, usage)
            : SetDIBits((HDC)dcObject, (HBITMAP)bitmapObject, startIndex, lineCount,
                        (const VOID *)(ULONG_PTR)bits, (const BITMAPINFO *)(ULONG_PTR)bitmapInfo,
                        usage);
        WowNotePut(note, noteCapacity, &noteLength, " -> ");
        WowNoteHex(note, noteCapacity, &noteLength, (DWORD)result, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, " scan lines");
        Wow32SetReturn(frame, (DWORD)(WORD)result);
        return 1;
    }

    /* 0x168 CreatePalette -- and it is what makes SelectPalette real (Importance = 1):
     * A `LOGPALETTE` is `{WORD palVersion; WORD palNumEntries; PALETTEENTRY
     * palPalEntry[];}` with a 4-byte entry, and every field is the same width
     * in both worlds, so the guest's own structure goes to Win32 unconverted.
     *
     * [CAUTION]: Session 46 answered `GetNearestPaletteIndex` with 0 "because this host
     * has no palette objects". It has now, so that refusal is a real lookup
     * again -- which is the point of implementing the producer before the
     * consumers.
     */
    case WOWGDI_CREATEPALETTE:
    {
        volatile BYTE *argument = Wow32ArgPointer(frame, 0);
        INT noteLength = 0;
        HPALETTE palette;
        WORD token;
        WowNotePut(note, noteCapacity, &noteLength, "CreatePalette(");
        if (!argument)
        {
            WowNotePut(note, noteCapacity, &noteLength, "NULL) -- ★ no LOGPALETTE; answered 0");
            Wow32SetReturn(frame, 0);
            return 1;
        }
        WowNoteHex(note, noteCapacity, &noteLength, (DWORD)Wow32PeekWord(argument + WOWGDI_LOGPALETTE_ENTRIES), WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, " entries)");
        palette = CreatePalette((const LOGPALETTE *)(ULONG_PTR)argument);
        token = palette ? WowGdiH16((HGDIOBJ)palette, WOWGDI_KIND_OBJ) : 0;
        if (!token)
        {
            if (palette)
                DeleteObject((HGDIOBJ)palette);
            WowNotePut(note, noteCapacity, &noteLength, " -- ★ the OS refused it (or the token map"
                                       " is full); answered 0");
            Wow32SetReturn(frame, 0);
            return 1;
        }
        WowNotePut(note, noteCapacity, &noteLength, " -> palette token 0x");
        WowNoteHex(note, noteCapacity, &noteLength, token, WOW_HEX_WORD_DIGITS);
        Wow32SetReturn(frame, token);
        return 1;
    }

    case WOWGDI_STRETCHDIBITS:
    {
        WORD  dc16 = Wow32ArgWord(frame, WOWGDI_SDI_ARG_HDC);
        INT   destX = (INT)(SHORT)Wow32ArgWord(frame, WOWGDI_SDI_ARG_DSTX);
        INT   destY = (INT)(SHORT)Wow32ArgWord(frame, WOWGDI_SDI_ARG_DSTY);
        INT   destWidth = (INT)(SHORT)Wow32ArgWord(frame, WOWGDI_SDI_ARG_DSTW);
        INT   destHeight = (INT)(SHORT)Wow32ArgWord(frame, WOWGDI_SDI_ARG_DSTH);
        INT   sourceX = (INT)(SHORT)Wow32ArgWord(frame, WOWGDI_SDI_ARG_SRCX);
        INT   sourceY = (INT)(SHORT)Wow32ArgWord(frame, WOWGDI_SDI_ARG_SRCY);
        INT   sourceWidth = (INT)(SHORT)Wow32ArgWord(frame, WOWGDI_SDI_ARG_SRCW);
        INT   sourceHeight = (INT)(SHORT)Wow32ArgWord(frame, WOWGDI_SDI_ARG_SRCH);
        WORD  usage = Wow32ArgWord(frame, WOWGDI_SDI_ARG_USAGE);
        DWORD rasterOp = Wow32ArgDword(frame, WOWGDI_SDI_ARG_ROP);
        volatile BYTE *bits = Wow32ArgPointer(frame, WOWGDI_SDI_ARG_BITS);
        volatile BYTE *bitmapInfo  = Wow32ArgPointer(frame, WOWGDI_SDI_ARG_BMI);
        INT   kind = -1;
        HGDIOBJ object = WowGdiH32(dc16, &kind);
        INT   noteLength = 0, result;
        WowNotePut(note, noteCapacity, &noteLength, "StretchDIBits(0x");
        WowNoteHex(note, noteCapacity, &noteLength, dc16, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, " dst(");
        WowNoteHex(note, noteCapacity, &noteLength, (DWORD)destX, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, ",");
        WowNoteHex(note, noteCapacity, &noteLength, (DWORD)destY, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, ") ");
        WowNoteHex(note, noteCapacity, &noteLength, (DWORD)destWidth, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, "x");
        WowNoteHex(note, noteCapacity, &noteLength, (DWORD)destHeight, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, " <- src ");
        WowNoteHex(note, noteCapacity, &noteLength, (DWORD)sourceWidth, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, "x");
        WowNoteHex(note, noteCapacity, &noteLength, (DWORD)sourceHeight, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, " rop=0x");
        WowNoteHex(note, noteCapacity, &noteLength, rasterOp, WOW_HEX_DWORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, ")");
        if (!object || (kind != WOWGDI_KIND_DC && kind != WOWGDI_KIND_WINDC)
               || !bitmapInfo || !bits)
        {
            WowNotePut(note, noteCapacity, &noteLength, " -- ★ NOT ONE OF OUR DC TOKENS, or no"
                                       " bits/BITMAPINFO; answered 0");
            Wow32SetReturn(frame, 0);
            return 1;
        }
        result = StretchDIBits((HDC)object, destX, destY, destWidth, destHeight, sourceX, sourceY, sourceWidth, sourceHeight,
                          (const VOID *)(ULONG_PTR)bits,
                          (const BITMAPINFO *)(ULONG_PTR)bitmapInfo, usage, rasterOp);
        WowNotePut(note, noteCapacity, &noteLength, " -> ");
        WowNoteHex(note, noteCapacity, &noteLength, (DWORD)result, WOW_HEX_WORD_DIGITS);
        Wow32SetReturn(frame, (DWORD)(WORD)result);
        return 1;
    }

    /* CreateDIBitmap: a DIB in the guest's memory becomes a real DDB:
     * Like SetDIBits/GetDIBits above, the BITMAPINFOHEADER is 40 bytes and
     * byte-identical in both worlds and both the header and the bits live in
     * guest memory this host can address, so nothing is converted -- only the
     * DC token resolved and the new bitmap tokenised on the way out.
     *
     * [CAUTION]: `dwInit` DECIDES WHETHER lpbInit IS READ AT ALL (CBM_INIT = 4). With it
     * clear, Win32 must be handed NULLs: passing a pointer alongside a zero
     * flag is how a caller ends up with an uninitialised bitmap that looks
     * initialised.
     */
    case WOWGDI_CREATEDIBITMAP:
    {
        WORD  dc16   = Wow32ArgWord(frame, WOWGDI_CDIB_ARG_HDC);
        volatile BYTE *bitmapInfoHeader = Wow32ArgPointer(frame, WOWGDI_CDIB_ARG_BMIH);
        DWORD initData  = Wow32ArgDword(frame, WOWGDI_CDIB_ARG_INIT);
        volatile BYTE *bits = Wow32ArgPointer(frame, WOWGDI_CDIB_ARG_BITS);
        volatile BYTE *bitmapInfo  = Wow32ArgPointer(frame, WOWGDI_CDIB_ARG_BMI);
        WORD  usage = Wow32ArgWord(frame, WOWGDI_CDIB_ARG_USAGE);
        INT   kind = -1;
        HGDIOBJ object = WowGdiH32(dc16, &kind);
        HBITMAP bitmap;
        WORD  token;
        INT   noteLength = 0;
        WowNotePut(note, noteCapacity, &noteLength, "CreateDIBitmap(dc 0x");
        WowNoteHex(note, noteCapacity, &noteLength, dc16, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, ", init 0x");
        WowNoteHex(note, noteCapacity, &noteLength, initData, WOW_HEX_DWORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, ")");
        if (!object || (kind != WOWGDI_KIND_DC && kind != WOWGDI_KIND_WINDC) || !bitmapInfoHeader)
        {
            WowNotePut(note, noteCapacity, &noteLength, " -- ★ NOT ONE OF OUR DC TOKENS, or no"
                                       " BITMAPINFOHEADER; answered 0");
            Wow32SetReturn(frame, 0);
            return 1;
        }
        bitmap = CreateDIBitmap((HDC)object,
                            (const BITMAPINFOHEADER *)(ULONG_PTR)bitmapInfoHeader, initData,
                            (initData && bits) ? (const VOID *)(ULONG_PTR)bits : NULL,
                            (initData && bitmapInfo)  ? (const BITMAPINFO *)(ULONG_PTR)bitmapInfo : NULL,
                            usage);
        if (!bitmap)
        {
            WowNotePut(note, noteCapacity, &noteLength, " -- ★ GDI REFUSED; answered 0");
            Wow32SetReturn(frame, 0);
            return 1;
        }
        token = WowGdiH16((HGDIOBJ)bitmap, WOWGDI_KIND_OBJ);
        if (!token)
        {
            /* [CAUTION]: NO TOKEN LEFT MEANS THE BITMAP LEAKS IF WE JUST RETURN 0 -- the
             * guest never learns of it, so nobody will ever DeleteObject it.
             * Destroy it here and fail honestly.
             */
            DeleteObject(bitmap);
            WowNotePut(note, noteCapacity, &noteLength, " -- ★ HANDLE MAP FULL; destroyed and"
                                       " answered 0");
            Wow32SetReturn(frame, 0);
            return 1;
        }
        WowNotePut(note, noteCapacity, &noteLength, " -> 0x");
        WowNoteHex(note, noteCapacity, &noteLength, token, WOW_HEX_WORD_DIGITS);
        Wow32SetReturn(frame, (DWORD)token);
        return 1;
    }

    case WOWGDI_SETDIBITSTODEV:
    {
        WORD  dc16 = Wow32ArgWord(frame, WOWGDI_SDD_ARG_HDC);
        INT   destX = (INT)(SHORT)Wow32ArgWord(frame, WOWGDI_SDD_ARG_DSTX);
        INT   destY = (INT)(SHORT)Wow32ArgWord(frame, WOWGDI_SDD_ARG_DSTY);
        /* [CAUTION]: w/h ARE `WORD`s IN THE Win16 PROTOTYPE, not ints -- a bitmap is never
         * negative-width, and sign-extending one over 32767 would turn a blit
         * into a negative and draw nothing. Widened UNSIGNED, unlike the
         * coordinates either side of them, which are genuinely signed.
         */
        DWORD width  = (DWORD)Wow32ArgWord(frame, WOWGDI_SDD_ARG_W);
        DWORD handle16  = (DWORD)Wow32ArgWord(frame, WOWGDI_SDD_ARG_H);
        INT   sourceX = (INT)(SHORT)Wow32ArgWord(frame, WOWGDI_SDD_ARG_SRCX);
        INT   sourceY = (INT)(SHORT)Wow32ArgWord(frame, WOWGDI_SDD_ARG_SRCY);
        WORD  startIndex  = Wow32ArgWord(frame, WOWGDI_SDD_ARG_START);
        WORD  scanCount = Wow32ArgWord(frame, WOWGDI_SDD_ARG_NSCANS);
        volatile BYTE *bits = Wow32ArgPointer(frame, WOWGDI_SDD_ARG_BITS);
        volatile BYTE *bitmapInfo  = Wow32ArgPointer(frame, WOWGDI_SDD_ARG_BMI);
        WORD  usage = Wow32ArgWord(frame, WOWGDI_SDD_ARG_USAGE);
        INT   kind = -1;
        HGDIOBJ object = WowGdiH32(dc16, &kind);
        INT   noteLength = 0, result;
        WowNotePut(note, noteCapacity, &noteLength, "SetDIBitsToDevice(dc 0x");
        WowNoteHex(note, noteCapacity, &noteLength, dc16, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, " dst(");
        WowNoteHex(note, noteCapacity, &noteLength, (DWORD)destX, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, ",");
        WowNoteHex(note, noteCapacity, &noteLength, (DWORD)destY, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, ") ");
        WowNoteHex(note, noteCapacity, &noteLength, (DWORD)width, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, "x");
        WowNoteHex(note, noteCapacity, &noteLength, (DWORD)handle16, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, ")");
        if (!object || (kind != WOWGDI_KIND_DC && kind != WOWGDI_KIND_WINDC)
               || !bitmapInfo || !bits)
        {
            WowNotePut(note, noteCapacity, &noteLength, " -- ★ NOT ONE OF OUR DC TOKENS, or no"
                                       " bits/BITMAPINFO; answered 0");
            Wow32SetReturn(frame, 0);
            return 1;
        }
        result = SetDIBitsToDevice((HDC)object, destX, destY, width, handle16,
                              sourceX, sourceY, startIndex, scanCount,
                              (const VOID *)(ULONG_PTR)bits,
                              (const BITMAPINFO *)(ULONG_PTR)bitmapInfo, usage);
        WowNotePut(note, noteCapacity, &noteLength, " -> ");
        WowNoteHex(note, noteCapacity, &noteLength, (DWORD)result, WOW_HEX_WORD_DIGITS);
        Wow32SetReturn(frame, (DWORD)(WORD)result);
        return 1;
    }

    default:
        return 0;
    }
}
