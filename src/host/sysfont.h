/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * Fill the VGA character tables from the machine's own fonts (#322).
 *
 * NTVDMEX ships no font data. At start-up, before the tables are copied into guest
 * memory (vdd_video_install_fonts), every one of the 256 code page 437 characters is
 * drawn with GDI and read back as 8 pixels per row (bit 7 = leftmost):
 *
 * - TEXT -- letters, digits, punctuation, and the accented letters and symbols that
 *   also exist in Windows-1252 -- from FIXEDSYS (the system's fixed ANSI font).
 * - DOS GRAPHICS -- box drawing, blocks, shading, the 01h-1Fh symbols, Greek and
 *   maths -- from TERMINAL (the OEM font, which has the whole of code page 437).
 *   Fixedsys has none of these.
 * - The 8x8 table entirely from Terminal's own 8x8 size: Fixedsys has no 8-pixel
 *   size, and a table squashed out of a taller font is the defect that once garbled
 *   Skyroads' text (see vdd_video_install_fonts). A font is never derived.
 *
 * A glyph shorter than its cell is centred; box-drawing and block glyphs are then
 * extended to the cell's top/bottom edge wherever they touch their own, so lines join
 * from one character cell to the next. A glyph taller than its cell loses rows that
 * are blank in every glyph of that font first.
 *
 * #321: THE USER MAY CHOOSE ANOTHER INSTALLED FONT:
 * NTVDMEX still ships none: the Settings page lists the fixed-pitch fonts installed on
 * this machine, and whatever the user picks (or installs themselves) is laid OVER the
 * default above, one character at a time. Each of the 256 codes is mapped through
 * Unicode, so a modern font supplies its own box drawing, Greek and symbols; a code the
 * font has no glyph for keeps the default glyph. An empty name is the default.
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#ifndef SYSFONT_H
#define SYSFONT_H

#include <windows.h>

#include "../ntvdmex_x86.h"     /* defines only: X86_WORD_SIZE / DWORD_SIZE */
#include "../ntvdmex_ascii.h"   /* defines only: ASCII_SPACE */
#include "vga_font.h"

#define SYSFONT_MAX_HEIGHT              32
#define SYSFONT_GLYPH                   0               /* SysFontFit: an ordinary character ... */
#define SYSFONT_BOX_BLOCK               1               /* ... or a box/block one that reaches its edges */
#define SYSFONT_CELL_WIDTH              8               /* A VGA character cell */
#define SYSFONT_TERMINAL_HEIGHT         12              /* Terminal's 8x12: the 8x14/8x16 source */
#define SYSFONT_CENTRE                  2

/* The 1-bpp DIB a glyph is drawn into: 16 pixels wide, a 4-byte row stride. */
#define SYSFONT_STAGE_WIDTH             16
#define SYSFONT_STAGE_STRIDE            4
#define SYSFONT_STAGE_COLOURS           2
#define SYSFONT_WHITE_LEVEL             255

/* Characters and code pages. */
#define SYSFONT_DELETE                  0x7F
#define SYSFONT_BLANK_FF                0xFF            /* Blank in every font */
#define SYSFONT_CODE_PAGE_437           437
#define SYSFONT_CODE_PAGE_1252          1252
#define SYSFONT_SYMBOLS_FIRST           0x2190          /* Arrows, maths, boxes, blocks, shapes */
#define SYSFONT_SYMBOLS_LAST            0x25FF
#define SYSFONT_BOX_BLOCK_FIRST         0x2500          /* Box drawing and block elements */
#define SYSFONT_BOX_BLOCK_LAST          0x259F
#define SYSFONT_NO_GLYPH                0xFFFF          /* GetGlyphIndices: GGI_MARK_NONEXISTING_GLYPHS */

/* A .FON file: an NE executable whose RT_FONT resources are FNT 2.0/3.0 fonts. */
#define SYSFONT_FONTS_DIRECTORY         "\\Fonts\\"     /* Under the Windows directory */
#define SYSFONT_FILE_CGA80WOA           "CGA80WOA.FON"
#define SYSFONT_FILE_DOSAPP             "DOSAPP.FON"
#define SYSFONT_FILE_VGAOEM             "VGAOEM.FON"
#define SYSFONT_FILE_EGA80WOA           "EGA80WOA.FON"
#define SYSFONT_FONTS_DIRECTORY_ROOM    32              /* "\\Fonts\\" and a file name */
#define SYSFONT_FON_MIN_SIZE            0x80
#define SYSFONT_FON_MAX_SIZE            (4u << 20)
#define SYSFONT_MZ_NE_OFFSET            0x3C            /* e_lfanew */
#define SYSFONT_NE_HEADER_SIZE          0x40
#define SYSFONT_NE_RESOURCE_TABLE       0x24
#define SYSFONT_RESOURCE_TYPE_SIZE      8               /* Type id, count, reserved */
#define SYSFONT_RESOURCE_ENTRY_SIZE     12
#define SYSFONT_RT_FONT                 0x8008
#define SYSFONT_FNT_VERSION_2           0x200
#define SYSFONT_FNT_VERSION_3           0x300
#define SYSFONT_FNT_CHARSET             85
#define SYSFONT_FNT_PIXEL_WIDTH         86
#define SYSFONT_FNT_PIXEL_HEIGHT        88
#define SYSFONT_FNT_FIRST_CHAR          95
#define SYSFONT_FNT_LAST_CHAR           96
#define SYSFONT_FNT2_HEADER_SIZE        118             /* Then the character table */
#define SYSFONT_FNT3_HEADER_SIZE        148
#define SYSFONT_FNT2_ENTRY_SIZE         4               /* Width WORD, offset WORD */
#define SYSFONT_FNT3_ENTRY_SIZE         6               /* Width WORD, offset DWORD */
#define SYSFONT_FNT_ENTRY_OFFSET        2
#define SYSFONT_LINE_SIZE               400
#define SYSFONT_TABLE_COUNT             3               /* 8x8, 8x14, 8x16 */

typedef struct _SYSFONT_FACE
{
    HFONT Font;
    /* the cell GDI actually gave us */
    INT Width;
    INT Height;
    BYTE  Glyphs[VGA_FONT_CHARACTERS][SYSFONT_MAX_HEIGHT];   /* glyphs, Height rows each */
    INT   IsOk;
} SYSFONT_FACE, *PSYSFONT_FACE; typedef const SYSFONT_FACE *PCSYSFONT_FACE;

/* One complete set of character generators, so a build can go somewhere other than the
 * live tables -- the Settings page previews a font without touching the machine.
 */
typedef struct _SYSFONT_TABLES
{
    BYTE Table8[VGA_FONT_CHARACTERS][VGA_FONT8_HEIGHT];
    BYTE Table14[VGA_FONT_CHARACTERS][VGA_FONT14_HEIGHT];
    BYTE Table16[VGA_FONT_CHARACTERS][VGA_FONT16_HEIGHT];
} SYSFONT_TABLES, *PSYSFONT_TABLES;

/* What one build did. `Line` is the STAGE1 log line; the rest is about the chosen font,
 * for the log and the Settings page.
 */
enum
{
    SYSFONT_USER_NONE = 0, SYSFONT_USER_OK, SYSFONT_USER_MISSING, SYSFONT_USER_NOSIZE
};
typedef struct _SYSFONT_REPORT
{
    char Line[SYSFONT_LINE_SIZE];  /* char, not CHAR: the spelling moves code (#333) */
    INT  User;                     /* SYSFONT_USER_* */
    INT  UserGlyphs[SYSFONT_TABLE_COUNT]; /* glyphs taken from it for the 8x8, 8x14, 8x16 */
    INT  IsUserTrueType;           /* it is TrueType */
    INT  UserCodePage;             /* an OEM raster font's code page when not 437, else 0 */
    INT  IsDegraded;               /* the DEFAULT is not the code page 437 one */
} SYSFONT_REPORT, *PSYSFONT_REPORT; typedef const SYSFONT_REPORT *PCSYSFONT_REPORT;

/* Build all three tables into `t`: the default (above), then the chosen `face` over it
 * when one is given. Fills `r` and returns r->line, the STAGE1 log line.
 */
PCSTR SysFontBuildInto(PCSTR faceName, SYSFONT_TABLES *tables, SYSFONT_REPORT *report);

/* The DEFAULT is degraded when a face is missing or a Terminal face had to come from
 * GDI by name: on a machine whose OEM code page is not 437 that draws accented letters
 * where box pieces belong. Said in the log and on the Settings page rather than drawn
 * wrong in silence.
 */
INT SysFontIsDefaultDegraded(const SYSFONT_REPORT *report);

/* Build into the LIVE tables (g_VgaFont8x8/8x14/8x16). Staged first and copied in one
 * pass, so a frame drawn mid-build never mixes two fonts for long.
 */
PCSTR SysFontBuild(PCSTR faceName, SYSFONT_REPORT *report);

#endif /* SYSFONT_H */
