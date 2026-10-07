/* sysfont.h -- fill the VGA character tables from the machine's own fonts (#322).
 *
 * NTVDMEX ships no font data. At start-up, before the tables are copied into guest
 * memory (vdd_video_install_fonts), every one of the 256 code page 437 characters is
 * drawn with GDI and read back as 8 pixels per row (bit 7 = leftmost):
 *
 *   - TEXT -- letters, digits, punctuation, and the accented letters and symbols that
 *     also exist in Windows-1252 -- from FIXEDSYS (the system's fixed ANSI font).
 *   - DOS GRAPHICS -- box drawing, blocks, shading, the 01h-1Fh symbols, Greek and
 *     maths -- from TERMINAL (the OEM font, which has the whole of code page 437).
 *     Fixedsys has none of these.
 *   - The 8x8 table entirely from Terminal's own 8x8 size: Fixedsys has no 8-pixel
 *     size, and a table squashed out of a taller font is the defect that once garbled
 *     Skyroads' text (see vdd_video_install_fonts). A font is never derived.
 *
 * A glyph shorter than its cell is centred; box-drawing and block glyphs are then
 * extended to the cell's top/bottom edge wherever they touch their own, so lines join
 * from one character cell to the next. A glyph taller than its cell loses rows that
 * are blank in every glyph of that font first.
 *
 * ── #321: THE USER MAY CHOOSE ANOTHER INSTALLED FONT. ─────────────────────────────
 * NTVDMEX still ships none: the Settings page lists the fixed-pitch fonts installed on
 * this machine, and whatever the user picks (or installs themselves) is laid OVER the
 * default above, one character at a time. Each of the 256 codes is mapped through
 * Unicode, so a modern font supplies its own box drawing, Greek and symbols; a code the
 * font has no glyph for keeps the default glyph. An empty name is the default. */
#ifndef SYSFONT_H
#define SYSFONT_H

#include <windows.h>
#include "vga_font.h"

#define SYSFONT_MAX_HEIGHT 32
#define SYSFONT_CELL_WIDTH 8            /* a VGA character cell                       */
#define SYSFONT_TERMINAL_HEIGHT 12      /* Terminal's 8x12: the 8x14/8x16 source       */
#define SYSFONT_CENTRE      2
/* The 1-bpp DIB a glyph is drawn into: 16 pixels wide, a 4-byte row stride. */
#define SYSFONT_STAGE_WIDTH 16
#define SYSFONT_STAGE_STRIDE 4
#define SYSFONT_STAGE_COLOURS 2
#define SYSFONT_WHITE_LEVEL 255
/* Characters and code pages. */
#define SYSFONT_SPACE       0x20
#define SYSFONT_DELETE      0x7F
#define SYSFONT_BLANK_FF    0xFF        /* blank in every font                        */
#define SYSFONT_CODE_PAGE_437  437
#define SYSFONT_CODE_PAGE_1252 1252
#define SYSFONT_SYMBOLS_FIRST  0x2190   /* arrows, maths, boxes, blocks, shapes        */
#define SYSFONT_SYMBOLS_LAST   0x25FF
#define SYSFONT_BOX_BLOCK_FIRST 0x2500  /* box drawing and block elements             */
#define SYSFONT_BOX_BLOCK_LAST  0x259F
#define SYSFONT_NO_GLYPH    0xFFFF      /* GetGlyphIndices: GGI_MARK_NONEXISTING_GLYPHS */
/* A .FON file: an NE executable whose RT_FONT resources are FNT 2.0/3.0 fonts. */
#define SYSFONT_BYTE_SHIFT  8
#define SYSFONT_WORD_BYTES  2
#define SYSFONT_DWORD_BYTES 4
#define SYSFONT_FONTS_DIRECTORY_ROOM 32 /* "\\Fonts\\" and a file name                 */
#define SYSFONT_FON_MIN_SIZE 0x80
#define SYSFONT_FON_MAX_SIZE (4u << 20)
#define SYSFONT_MZ_NE_OFFSET 0x3C       /* e_lfanew                                   */
#define SYSFONT_NE_HEADER_SIZE 0x40
#define SYSFONT_NE_RESOURCE_TABLE 0x24
#define SYSFONT_RESOURCE_TYPE_SIZE 8    /* type id, count, reserved                   */
#define SYSFONT_RESOURCE_ENTRY_SIZE 12
#define SYSFONT_RT_FONT     0x8008
#define SYSFONT_FNT_VERSION_2 0x200
#define SYSFONT_FNT_VERSION_3 0x300
#define SYSFONT_FNT_CHARSET 85
#define SYSFONT_FNT_PIXEL_WIDTH 86
#define SYSFONT_FNT_PIXEL_HEIGHT 88
#define SYSFONT_FNT_FIRST_CHAR 95
#define SYSFONT_FNT_LAST_CHAR 96
#define SYSFONT_FNT2_HEADER_SIZE 118    /* then the character table                   */
#define SYSFONT_FNT3_HEADER_SIZE 148
#define SYSFONT_FNT2_ENTRY_SIZE 4       /* width WORD, offset WORD                    */
#define SYSFONT_FNT3_ENTRY_SIZE 6       /* width WORD, offset DWORD                   */
#define SYSFONT_FNT_ENTRY_OFFSET 2

typedef struct _SYSFONT_FACE {
    HFONT Font;
    INT   Width, Height;                       /* the cell GDI actually gave us */
    BYTE  Glyphs[VGA_FONT_CHARACTERS][SYSFONT_MAX_HEIGHT];   /* glyphs, Height rows each */
    INT   IsOk;
} SYSFONT_FACE, *PSYSFONT_FACE; typedef const SYSFONT_FACE *PCSYSFONT_FACE;

/* One complete set of character generators, so a build can go somewhere other than the
   live tables -- the Settings page previews a font without touching the machine. */
typedef struct _SYSFONT_TABLES {
    BYTE Table8[VGA_FONT_CHARACTERS][VGA_FONT8_HEIGHT], Table14[VGA_FONT_CHARACTERS][VGA_FONT14_HEIGHT], Table16[VGA_FONT_CHARACTERS][VGA_FONT16_HEIGHT];
} SYSFONT_TABLES, *PSYSFONT_TABLES;

/* What one build did. `Line` is the STAGE1 log line; the rest is about the chosen font,
   for the log and the Settings page. */
enum { SYSFONT_USER_NONE = 0, SYSFONT_USER_OK, SYSFONT_USER_MISSING, SYSFONT_USER_NOSIZE };
#define SYSFONT_LINE_SIZE 400
#define SYSFONT_TABLE_COUNT 3      /* 8x8, 8x14, 8x16                                 */
typedef struct _SYSFONT_REPORT {
    char Line[SYSFONT_LINE_SIZE];  /* char, not CHAR: the spelling moves code (#333)  */
    INT  User;                     /* SYSFONT_USER_*                                  */
    INT  UserGlyphs[SYSFONT_TABLE_COUNT]; /* glyphs taken from it for the 8x8, 8x14, 8x16 */
    INT  IsUserTrueType;           /* it is TrueType                                  */
    INT  UserCodePage;             /* an OEM raster font's code page when not 437, else 0 */
    INT  IsDegraded;               /* the DEFAULT is not the code page 437 one        */
} SYSFONT_REPORT, *PSYSFONT_REPORT; typedef const SYSFONT_REPORT *PCSYSFONT_REPORT;

/* Draw one byte in `f` into a 1bpp DIB and read back its rows. */
static VOID SysFontRender(HDC dc, BYTE *bits, SYSFONT_FACE *face, BYTE character,
                           BYTE *out)
{
    RECT rect = { 0, 0, SYSFONT_STAGE_WIDTH, SYSFONT_MAX_HEIGHT };
    INT row;
    HFONT previous = (HFONT)SelectObject(dc, face->Font);
    FillRect(dc, &rect, (HBRUSH)GetStockObject(BLACK_BRUSH));
    SetTextColor(dc, RGB(SYSFONT_WHITE_LEVEL, SYSFONT_WHITE_LEVEL, SYSFONT_WHITE_LEVEL));
    SetBkColor(dc, RGB(0, 0, 0));
    SetBkMode(dc, OPAQUE);
    TextOutA(dc, 0, 0, (LPCSTR)&character, 1);
    GdiFlush();
    for (row = 0; row < face->Height && row < SYSFONT_MAX_HEIGHT; ++row)
        out[row] = bits[row * SYSFONT_STAGE_STRIDE];                  /* 16 px wide -> 4-byte stride; byte 0 = x 0..7 */
    SelectObject(dc, previous);
}

/* Open a raster face and render all 256 bytes. `want_h` is a request; the face keeps
   whatever height GDI chose, and is refused unless it is 8 pixels wide. */
static INT SysFontOpen(HDC dc, BYTE *bits, SYSFONT_FACE *face, PCSTR name,
                        BYTE charset, INT wantedHeight)
{
    TEXTMETRICA textMetric;
    HFONT previous;
    UINT character;
    face->IsOk = 0;
    face->Font = CreateFontA(wantedHeight, SYSFONT_CELL_WIDTH, 0, 0, FW_NORMAL, 0, 0, 0, charset, OUT_RASTER_PRECIS,
                          CLIP_DEFAULT_PRECIS, NONANTIALIASED_QUALITY, FIXED_PITCH | FF_MODERN,
                          name);
    if (!face->Font) return 0;
    previous = (HFONT)SelectObject(dc, face->Font);
    GetTextMetricsA(dc, &textMetric);
    SelectObject(dc, previous);
    face->Width = textMetric.tmAveCharWidth;
    face->Height = textMetric.tmHeight;
    if (face->Width != SYSFONT_CELL_WIDTH || face->Height < 1 || face->Height > SYSFONT_MAX_HEIGHT) return 0;
    for (character = 0; character < VGA_FONT_CHARACTERS; ++character) SysFontRender(dc, bits, face, (BYTE)character, face->Glyphs[character]);
    face->IsOk = 1;
    return 1;
}

/* ── THE CODE PAGE 437 TERMINAL, READ FROM ITS FILE. GDI picks "Terminal" by the
     machine's OEM code page -- on a UK XP that is 850, whose font has accented letters
     where 437 has some box pieces and the Greek/maths symbols (measured on the rig:
     the charset probe showed Á Â À © where ╡ ╢ ╖ belong). XP keeps the 437 fonts on
     every locale, so the
     faces are read straight out of their files (CGA80WOA.FON 8x8, VGAOEM.FON 8x12): an NE file whose RT_FONT resources are
     Windows FNT 2.0/3.0 bitmaps. Still the user's own system font; nothing shipped. */
static DWORD SysFontRead(const BYTE *bytes, DWORD length, DWORD offset, INT size)
{
    DWORD value = 0; INT index;
    if (offset + (DWORD)size > length) return 0;
    for (index = size - 1; index >= 0; --index) value = (value << SYSFONT_BYTE_SHIFT) | bytes[offset + index];
    return value;
}

/* Load the 8 x `want_h` OEM face from Fonts\`file` into `f`. 1 on success. */
static INT SysFontOpenFon(SYSFONT_FACE *face, PCSTR fileName, INT wantedHeight)
{
    CHAR path[MAX_PATH];
    HANDLE file;
    DWORD size = 0, read = 0, neHeader, resourceTable, alignShift, offset;
    BYTE *bytes;
    INT isFound = 0;
    face->IsOk = 0; face->Font = NULL; face->Width = 0; face->Height = 0;
    if (!GetWindowsDirectoryA(path, MAX_PATH - SYSFONT_FONTS_DIRECTORY_ROOM)) return 0;
    lstrcatA(path, "\\Fonts\\"); lstrcatA(path, fileName);
    file = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (file == INVALID_HANDLE_VALUE) return 0;
    size = GetFileSize(file, NULL);
    if (size == INVALID_FILE_SIZE || size < SYSFONT_FON_MIN_SIZE || size > SYSFONT_FON_MAX_SIZE) { CloseHandle(file); return 0; }
    bytes = (BYTE *)HeapAlloc(GetProcessHeap(), 0, size);
    if (!bytes) { CloseHandle(file); return 0; }
    ReadFile(file, bytes, size, &read, NULL);
    CloseHandle(file);
    neHeader = SysFontRead(bytes, read, SYSFONT_MZ_NE_OFFSET, SYSFONT_DWORD_BYTES);
    if (read != size || bytes[0] != 'M' || bytes[1] != 'Z' || neHeader + SYSFONT_NE_HEADER_SIZE > size || bytes[neHeader] != 'N' || bytes[neHeader + 1] != 'E') {
        HeapFree(GetProcessHeap(), 0, bytes); return 0;
    }
    resourceTable = neHeader + SysFontRead(bytes, size, neHeader + SYSFONT_NE_RESOURCE_TABLE, SYSFONT_WORD_BYTES);              /* resource table */
    alignShift = SysFontRead(bytes, size, resourceTable, SYSFONT_WORD_BYTES);
    offset = resourceTable + SYSFONT_WORD_BYTES;
    while (!isFound && offset + SYSFONT_RESOURCE_TYPE_SIZE <= size) {
        DWORD type = SysFontRead(bytes, size, offset, SYSFONT_WORD_BYTES), count = SysFontRead(bytes, size, offset + SYSFONT_WORD_BYTES, SYSFONT_WORD_BYTES), index;
        if (!type) break;
        offset += SYSFONT_RESOURCE_TYPE_SIZE;
        for (index = 0; index < count && offset + SYSFONT_RESOURCE_ENTRY_SIZE <= size; ++index, offset += SYSFONT_RESOURCE_ENTRY_SIZE) {
            DWORD fontOffset = SysFontRead(bytes, size, offset, SYSFONT_WORD_BYTES) << alignShift, version, pixelWidth, pixelHeight, charset, firstChar, lastChar, charTable, character;
            if (type != SYSFONT_RT_FONT || isFound || fontOffset + SYSFONT_FNT3_HEADER_SIZE > size) continue;   /* RT_FONT only */
            version = SysFontRead(bytes, size, fontOffset, SYSFONT_WORD_BYTES);
            charset  = bytes[fontOffset + SYSFONT_FNT_CHARSET];
            pixelWidth  = SysFontRead(bytes, size, fontOffset + SYSFONT_FNT_PIXEL_WIDTH, SYSFONT_WORD_BYTES);
            pixelHeight  = SysFontRead(bytes, size, fontOffset + SYSFONT_FNT_PIXEL_HEIGHT, SYSFONT_WORD_BYTES);
            firstChar = bytes[fontOffset + SYSFONT_FNT_FIRST_CHAR]; lastChar = bytes[fontOffset + SYSFONT_FNT_LAST_CHAR];
            if ((version != SYSFONT_FNT_VERSION_2 && version != SYSFONT_FNT_VERSION_3) || charset != OEM_CHARSET || pixelWidth != SYSFONT_CELL_WIDTH
                || (INT)pixelHeight != wantedHeight || pixelHeight > SYSFONT_MAX_HEIGHT) continue;
            charTable = fontOffset + (version == SYSFONT_FNT_VERSION_3 ? SYSFONT_FNT3_HEADER_SIZE : SYSFONT_FNT2_HEADER_SIZE);
            for (character = 0; character < VGA_FONT_CHARACTERS; ++character) {
                DWORD entry, glyphOffset, row;
                for (row = 0; row < SYSFONT_MAX_HEIGHT; ++row) face->Glyphs[character][row] = 0;
                if (character < firstChar || character > lastChar) continue;
                entry  = charTable + (character - firstChar) * (version == SYSFONT_FNT_VERSION_3 ? SYSFONT_FNT3_ENTRY_SIZE : SYSFONT_FNT2_ENTRY_SIZE);
                glyphOffset = fontOffset + (version == SYSFONT_FNT_VERSION_3 ? SysFontRead(bytes, size, entry + SYSFONT_FNT_ENTRY_OFFSET, SYSFONT_DWORD_BYTES) : SysFontRead(bytes, size, entry + SYSFONT_FNT_ENTRY_OFFSET, SYSFONT_WORD_BYTES));
                for (row = 0; row < pixelHeight && glyphOffset + row < size; ++row) face->Glyphs[character][row] = bytes[glyphOffset + row];
            }
            face->Width = SYSFONT_CELL_WIDTH; face->Height = (INT)pixelHeight; face->IsOk = 1; isFound = 1;
        }
    }
    HeapFree(GetProcessHeap(), 0, bytes);
    return isFound;
}

/* Is CP437 code `c` text Fixedsys can draw? Writes the Windows-1252 byte to *ansi. */
static INT SysFontIsText(UINT character, BYTE *ansi)
{
    CHAR oem = (CHAR)character;
    WCHAR wide = 0;
    BOOL isDefaultUsed = FALSE;
    CHAR out = 0;
    if (character < SYSFONT_SPACE || character == SYSFONT_DELETE) return 0;               /* CP437 symbols there, not text */
    if (character < SYSFONT_DELETE) { *ansi = (BYTE)character; return 1; }
    if (MultiByteToWideChar(SYSFONT_CODE_PAGE_437, MB_USEGLYPHCHARS, &oem, 1, &wide, 1) != 1) return 0;
    if (wide >= SYSFONT_SYMBOLS_FIRST && wide <= SYSFONT_SYMBOLS_LAST) return 0;       /* arrows, maths, boxes, blocks, shapes */
    if (WideCharToMultiByte(SYSFONT_CODE_PAGE_1252, WC_NO_BEST_FIT_CHARS, &wide, 1, &out, 1, NULL, &isDefaultUsed) != 1
        || isDefaultUsed)
        return 0;
    *ansi = (BYTE)out;
    return 1;
}

static INT SysFontIsBoxBlock(UINT character)
{
    CHAR oem = (CHAR)character;
    WCHAR wide = 0;
    if (MultiByteToWideChar(SYSFONT_CODE_PAGE_437, MB_USEGLYPHCHARS, &oem, 1, &wide, 1) != 1) return 0;
    return wide >= SYSFONT_BOX_BLOCK_FIRST && wide <= SYSFONT_BOX_BLOCK_LAST;
}

/* Rows blank in every glyph of a face, from the top. */
static INT SysFontBlankTop(const SYSFONT_FACE *face)
{
    INT row; UINT character;
    for (row = 0; row < face->Height; ++row)
        for (character = 0; character < VGA_FONT_CHARACTERS; ++character) if (face->Glyphs[character][row]) return row;
    return face->Height;
}

/* Put one glyph of `f` into a cell `H` rows high. */
static VOID SysFontFit(const SYSFONT_FACE *face, BYTE character, INT isBoxBlock,
                        BYTE *cell, INT cellHeight, INT cropTop)
{
    const BYTE *glyph = face->Glyphs[character];
    INT faceHeight = face->Height, top, row;
    for (row = 0; row < cellHeight; ++row) cell[row] = 0;
    if (faceHeight > cellHeight) {                                       /* too tall: drop blank rows */
        INT cropRows = cropTop < faceHeight - cellHeight ? cropTop : faceHeight - cellHeight;
        for (row = 0; row < cellHeight; ++row) cell[row] = glyph[cropRows + row];
        return;
    }
    top = (cellHeight - faceHeight) / SYSFONT_CENTRE;
    for (row = 0; row < faceHeight; ++row) cell[top + row] = glyph[row];
    if (isBoxBlock) {                                     /* reach the cell edges */
        if (glyph[0])      for (row = 0; row < top; ++row)            cell[row] = glyph[0];
        if (glyph[faceHeight - 1]) for (row = top + faceHeight; row < cellHeight; ++row)       cell[row] = glyph[faceHeight - 1];
    }
}

/* ── #321: lay the user's font over one table (`tab`: 256 glyphs of `H` rows). ────────
     Returns the number of glyphs it supplied, or -SYSFONT_USER_* when it supplied none.
     ⚠ GDI SUBSTITUTES SILENTLY: ask for a face that is not installed and CreateFont hands
       back the nearest match. The face actually selected is compared with the one asked
       for, so an uninstalled font is reported, never drawn as something else.
     A TrueType face is asked for at exactly the cell height and 8 pixels wide, drawn
     without antialiasing, and asked per character whether it HAS a glyph. A raster face
     can only offer the sizes it was made in, so it must have an 8-pixel-wide size no
     taller than the cell; its own code page decides which characters it can supply. */
static INT SysFontUser(HDC dc, BYTE *bits, SYSFONT_FACE *face, PCSTR faceName,
                        INT cellHeight, BYTE *table, SYSFONT_REPORT *report)
{
    TEXTMETRICA textMetric;
    CHAR selectedName[LF_FACESIZE];
    HFONT previous;
    UINT character;
    INT isTrueType, isOem, cropTop, count = 0, wantedHeight;
    static BYTE hasGlyph[VGA_FONT_CHARACTERS];
    face->IsOk = 0;
    /* A raster face that has no 8-wide size at the cell height may have a shorter one
       (Terminal: 8x8 and 8x12, but 12x16) -- try each height down to 8 and centre it,
       the way the default uses Terminal's 8x12 in the 8x16 table. */
    for (wantedHeight = cellHeight; ; --wantedHeight) {
        face->Font = CreateFontA(wantedHeight, SYSFONT_CELL_WIDTH, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                              OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, NONANTIALIASED_QUALITY,
                              FIXED_PITCH | FF_MODERN, faceName);
        if (!face->Font) return -SYSFONT_USER_MISSING;
        previous = (HFONT)SelectObject(dc, face->Font);
        selectedName[0] = 0;
        GetTextFaceA(dc, sizeof selectedName, selectedName);
        GetTextMetricsA(dc, &textMetric);
        SelectObject(dc, previous);
        if (lstrcmpiA(selectedName, faceName)) { DeleteObject(face->Font); face->Font = NULL; return -SYSFONT_USER_MISSING; }
        isTrueType  = (textMetric.tmPitchAndFamily & TMPF_TRUETYPE) != 0;
        face->Width = textMetric.tmAveCharWidth;
        face->Height = textMetric.tmHeight;
        if (face->Height >= 1 && face->Height <= SYSFONT_MAX_HEIGHT && (isTrueType || (face->Width == SYSFONT_CELL_WIDTH && face->Height <= cellHeight))) break;
        DeleteObject(face->Font); face->Font = NULL;
        if (isTrueType || wantedHeight <= VGA_FONT8_HEIGHT) return -SYSFONT_USER_NOSIZE;
    }
    isOem = textMetric.tmCharSet == OEM_CHARSET;
    report->IsUserTrueType = isTrueType;
    /* An OEM raster font draws the machine's OEM code page: on a UK XP that is 850, whose
       letters sit where 437 has box pieces. Drawn as chosen, but said. */
    if (!isTrueType && isOem && GetOEMCP() != SYSFONT_CODE_PAGE_437) report->UserCodePage = (INT)GetOEMCP();
    previous = (HFONT)SelectObject(dc, face->Font);
    for (character = 0; character < VGA_FONT_CHARACTERS; ++character) {
        RECT rect = { 0, 0, SYSFONT_STAGE_WIDTH, SYSFONT_MAX_HEIGHT };
        CHAR oem = (CHAR)character;
        WCHAR wide = 0;
        WORD glyphIndex = SYSFONT_NO_GLYPH;
        BYTE ansi = 0;
        INT row;
        hasGlyph[character] = 0;
        for (row = 0; row < SYSFONT_MAX_HEIGHT; ++row) face->Glyphs[character][row] = 0;
        if (character == 0 || character == SYSFONT_SPACE || character == SYSFONT_BLANK_FF) continue;         /* blank in every font */
        FillRect(dc, &rect, (HBRUSH)GetStockObject(BLACK_BRUSH));
        SetTextColor(dc, RGB(SYSFONT_WHITE_LEVEL, SYSFONT_WHITE_LEVEL, SYSFONT_WHITE_LEVEL));
        SetBkColor(dc, RGB(0, 0, 0));
        SetBkMode(dc, OPAQUE);
        if (isTrueType) {
            if (MultiByteToWideChar(SYSFONT_CODE_PAGE_437, MB_USEGLYPHCHARS, &oem, 1, &wide, 1) != 1) continue;
            if (GetGlyphIndicesW(dc, &wide, 1, &glyphIndex, GGI_MARK_NONEXISTING_GLYPHS) == GDI_ERROR
                || glyphIndex == SYSFONT_NO_GLYPH) continue;
            TextOutW(dc, 0, 0, &wide, 1);
        } else if (isOem) {
            TextOutA(dc, 0, 0, &oem, 1);                         /* its own OEM code page */
        } else {
            if (!SysFontIsText(character, &ansi)) continue;
            TextOutA(dc, 0, 0, (LPCSTR)&ansi, 1);
        }
        GdiFlush();
        for (row = 0; row < face->Height; ++row) face->Glyphs[character][row] = bits[row * SYSFONT_STAGE_STRIDE];
        hasGlyph[character] = 1;
    }
    SelectObject(dc, previous);
    DeleteObject(face->Font); face->Font = NULL;
    cropTop = face->Height > cellHeight ? SysFontBlankTop(face) : 0;
    for (character = 0; character < VGA_FONT_CHARACTERS; ++character) {
        if (!hasGlyph[character]) continue;
        SysFontFit(face, (BYTE)character, SysFontIsBoxBlock(character), table + character * (UINT)cellHeight, cellHeight, cropTop);
        ++count;
    }
    face->IsOk = 1;
    return count;
}

/* Build all three tables into `t`: the default (above), then the chosen `face` over it
   when one is given. Fills `r` and returns r->line, the STAGE1 log line. */
static PCSTR SysFontBuildInto(PCSTR faceName, SYSFONT_TABLES *tables, SYSFONT_REPORT *report)
{
    PSTR summary = report->Line;
    static SYSFONT_FACE fixedsys, terminal8, terminal12, terminal16, userFace;        /* static: ~8 KB each */
    struct { BITMAPINFOHEADER h; RGBQUAD pal[SYSFONT_STAGE_COLOURS]; } bitmapInfo;  /* 1bpp needs BOTH entries */
    PVOID bits = NULL;
    HDC dc = CreateCompatibleDC(NULL);
    HBITMAP bitmap, previousBitmap;
    UINT character;
    INT cropFixedsys = 0;
    PCSTR source8 = "GDI by name", source12 = "GDI by name";
    ZeroMemory(report, sizeof *report);
    ZeroMemory(tables, sizeof *tables);
    if (!dc) { lstrcpyA(summary, "sysfont: no DC -- tables left empty"); return summary; }
    ZeroMemory(&bitmapInfo, sizeof bitmapInfo);
    bitmapInfo.h.biSize = sizeof bitmapInfo.h;
    bitmapInfo.h.biWidth = SYSFONT_STAGE_WIDTH;
    bitmapInfo.h.biHeight = -SYSFONT_MAX_HEIGHT;                      /* top-down */
    bitmapInfo.h.biPlanes = 1;
    bitmapInfo.h.biBitCount = 1;
    bitmapInfo.h.biCompression = BI_RGB;
    bitmapInfo.pal[1].rgbRed = bitmapInfo.pal[1].rgbGreen = bitmapInfo.pal[1].rgbBlue = SYSFONT_WHITE_LEVEL;
    bitmap = CreateDIBSection(dc, (BITMAPINFO *)&bitmapInfo, DIB_RGB_COLORS, &bits, NULL, 0);
    if (!bitmap || !bits) {
        DeleteDC(dc); lstrcpyA(summary, "sysfont: no DIB -- tables left empty"); return summary;
    }
    previousBitmap = (HBITMAP)SelectObject(dc, bitmap);

    SysFontOpen(dc, (BYTE *)bits, &fixedsys,  "Fixedsys", ANSI_CHARSET, 0);
    /* Terminal at code page 437, from its files; by name only if a file is missing. */
    /* Measured on the rig's UK XP: CGA80WOA.FON is the 437 Terminal 8x8 (by name GDI
       gave CGA80850.FON, code page 850); VGAOEM.FON and EGA80WOA.FON are 437 8x12.
       DOSAPP.FON there has no 8-wide face at all. */
    if (SysFontOpenFon(&terminal8, "CGA80WOA.FON", VGA_FONT8_HEIGHT)) source8 = "CGA80WOA.FON";
    else if (SysFontOpenFon(&terminal8, "DOSAPP.FON", VGA_FONT8_HEIGHT)) source8 = "DOSAPP.FON";
    else SysFontOpen(dc, (BYTE *)bits, &terminal8, "Terminal", OEM_CHARSET, VGA_FONT8_HEIGHT);
    if (SysFontOpenFon(&terminal12, "VGAOEM.FON", SYSFONT_TERMINAL_HEIGHT))        source12 = "VGAOEM.FON";
    else if (SysFontOpenFon(&terminal12, "EGA80WOA.FON", SYSFONT_TERMINAL_HEIGHT)) source12 = "EGA80WOA.FON";
    else SysFontOpen(dc, (BYTE *)bits, &terminal12, "Terminal", OEM_CHARSET, VGA_FONT14_HEIGHT);
    terminal16.IsOk = 0;                                         /* no 8x16 Terminal exists */
    /* A Terminal size GDI could not match exactly (it substitutes the nearest) must not
       be cropped into a smaller cell -- that would be a derived font again. Refused; the
       summary says MISSING, and that table's graphics stay blank rather than wrong. */
    if (terminal8.IsOk  && terminal8.Height  != VGA_FONT8_HEIGHT)  terminal8.IsOk  = 0;
    if (terminal12.IsOk && terminal12.Height > VGA_FONT14_HEIGHT)  terminal12.IsOk = 0;
    if (terminal16.IsOk && terminal16.Height > VGA_FONT16_HEIGHT)  terminal16.IsOk = 0;
    if (fixedsys.IsOk) cropFixedsys = SysFontBlankTop(&fixedsys);

    /* Each table takes the TALLEST accepted Terminal face that fits it. Measured on
       XP: GDI offers 8x8, 8x12 and -- for a 16-pixel request -- 12x16, which is refused
       (not 8 wide); the 8x16 table then uses the 8x12, centred and edge-extended. */
    {   SYSFONT_FACE *face14 = terminal12.IsOk ? &terminal12 : terminal8.IsOk ? &terminal8 : NULL;
        SYSFONT_FACE *face16 = terminal16.IsOk ? &terminal16 : terminal12.IsOk ? &terminal12 : terminal8.IsOk ? &terminal8 : NULL;
        for (character = 0; character < VGA_FONT_CHARACTERS; ++character) {
            BYTE ansi = 0;
            INT isText = fixedsys.IsOk && SysFontIsText(character, &ansi);
            INT isBoxBlock = SysFontIsBoxBlock(character);
            if (terminal8.IsOk) SysFontFit(&terminal8, (BYTE)character, isBoxBlock, tables->Table8[character], VGA_FONT8_HEIGHT, 0);
            if (isText)     SysFontFit(&fixedsys, ansi, 0, tables->Table14[character], VGA_FONT14_HEIGHT, cropFixedsys);
            else if (face14) SysFontFit(face14, (BYTE)character, isBoxBlock, tables->Table14[character], VGA_FONT14_HEIGHT, 0);
            if (isText)     SysFontFit(&fixedsys, ansi, 0, tables->Table16[character], VGA_FONT16_HEIGHT, cropFixedsys);
            else if (face16) SysFontFit(face16, (BYTE)character, isBoxBlock, tables->Table16[character], VGA_FONT16_HEIGHT, 0);
        }
    }

    if (faceName && faceName[0]) {                              /* #321: the user's choice over it */
        INT index, glyphCounts[SYSFONT_TABLE_COUNT];
        glyphCounts[0] = SysFontUser(dc, (BYTE *)bits, &userFace, faceName, VGA_FONT8_HEIGHT,  &tables->Table8[0][0],  report);
        glyphCounts[1] = SysFontUser(dc, (BYTE *)bits, &userFace, faceName, VGA_FONT14_HEIGHT, &tables->Table14[0][0], report);
        glyphCounts[2] = SysFontUser(dc, (BYTE *)bits, &userFace, faceName, VGA_FONT16_HEIGHT, &tables->Table16[0][0], report);
        report->User = SYSFONT_USER_MISSING;
        for (index = 0; index < SYSFONT_TABLE_COUNT; ++index) {
            report->UserGlyphs[index] = glyphCounts[index] > 0 ? glyphCounts[index] : 0;
            if (glyphCounts[index] >= 0) report->User = SYSFONT_USER_OK;
            else if (report->User != SYSFONT_USER_OK && glyphCounts[index] == -SYSFONT_USER_NOSIZE)
                report->User = SYSFONT_USER_NOSIZE;
        }
    }

    SelectObject(dc, previousBitmap);
    DeleteObject(bitmap);
    if (fixedsys.Font) DeleteObject(fixedsys.Font);
    if (terminal8.Font) DeleteObject(terminal8.Font);
    if (terminal12.Font) DeleteObject(terminal12.Font);
    DeleteDC(dc);
    report->IsDegraded = !fixedsys.IsOk || !terminal8.IsOk || !terminal12.IsOk
                  || source8[0] == 'G' || source12[0] == 'G';     /* "GDI by name" */
    wsprintfA(summary, "sysfont: Fixedsys %s %dx%d (blank top %d); Terminal-437 8x8 %s %dx%d from %s, "
                   "8x12 (for 8x14 and 8x16) %s %dx%d from %s",
              fixedsys.IsOk ? "ok" : "MISSING", fixedsys.Width, fixedsys.Height, cropFixedsys,
              terminal8.IsOk ? "ok" : "MISSING", terminal8.Width, terminal8.Height, source8,
              terminal12.IsOk ? "ok" : "MISSING", terminal12.Width, terminal12.Height, source12);
    if (faceName && faceName[0]) {
        static PCSTR const reasons[] = { "", "", "NOT INSTALLED -- default used",
                                           "has no 8-pixel-wide size -- default used" };
        INT length = lstrlenA(summary);
        if (report->User == SYSFONT_USER_OK)
            wsprintfA(summary + length, "; TextFont \"%.60s\" (%s): glyphs 8x8 %d, 8x14 %d, 8x16 %d of 253, "
                      "the rest default%s", faceName, report->IsUserTrueType ? "TrueType" : "raster",
                      report->UserGlyphs[0], report->UserGlyphs[1], report->UserGlyphs[2],
                      report->UserCodePage ? " -- an OEM font NOT in code page 437" : "");
        else
            wsprintfA(summary + length, "; TextFont \"%.60s\" %s", faceName, reasons[report->User]);
    }
    return summary;
}

/* The DEFAULT is degraded when a face is missing or a Terminal face had to come from
   GDI by name: on a machine whose OEM code page is not 437 that draws accented letters
   where box pieces belong. Said in the log and on the Settings page rather than drawn
   wrong in silence. */
static INT SysFontIsDefaultDegraded(const SYSFONT_REPORT *report) { return report->IsDegraded; }

/* Build into the LIVE tables (g_VgaFont8x8/8x14/8x16). Staged first and copied in one
   pass, so a frame drawn mid-build never mixes two fonts for long. */
static PCSTR SysFontBuild(PCSTR faceName, SYSFONT_REPORT *report)
{
    static SYSFONT_TABLES stage;
    UINT character, row;
    SysFontBuildInto(faceName, &stage, report);
    for (character = 0; character < VGA_FONT_CHARACTERS; ++character) {
        for (row = 0; row < VGA_FONT8_HEIGHT;  ++row) g_VgaFont8x8[character][row]  = stage.Table8[character][row];
        for (row = 0; row < VGA_FONT14_HEIGHT; ++row) g_VgaFont8x14[character][row] = stage.Table14[character][row];
        for (row = 0; row < VGA_FONT16_HEIGHT; ++row) g_VgaFont8x16[character][row] = stage.Table16[character][row];
    }
    return report->Line;
}

#endif /* SYSFONT_H */
