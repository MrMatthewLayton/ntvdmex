/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * Off-VM battery for the text-mode font file's face name (src/host/font_file.h).
 *
 * The host loads the user's font privately and selects it by the name read here, so a
 * wrong name is a font that silently never appears. Pinned: the 'name' table's family
 * name with the Windows US English record preferred over others, a .ttc's first font,
 * a .fon's first RT_FONT face, and refusal of everything that is not one of those --
 * truncated, garbled, or with a name GDI could not take.
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#include <stdio.h>
#include <string.h>
#include "font_file.h"
#define CHECK(condition,message) do{ g_Total++; if(condition){printf("  PASS  %s\n",(message));} \
    else{printf("  FAIL  %s\n",(message)); g_Failures++;} }while(0)

static INT g_Total = 0;
static INT g_Failures = 0;
static BYTE g_Image[4096];

static VOID PutBig(BYTE *at, DWORD value, INT size)
{
    INT index;

    for (index = size - 1; index >= 0; --index)
    {
        at[index] = (BYTE)value;
        value >>= 8;
    }
}

static VOID PutLittle(BYTE *at, DWORD value, INT size)
{
    INT index;

    for (index = 0; index < size; ++index)
    {
        at[index] = (BYTE)value;
        value >>= 8;
    }
}

/* An sfnt at `base`: two tables ('head' dummy, then 'name'), the name table at base+64 with
 * three family records: Macintosh "MacName", Windows French "Nom", Windows US "Px437 IBM VGA8".
 * Returns the image length.
 */
static DWORD BuildSfnt(DWORD base, DWORD version)
{
    static const char macName[] = "MacName";
    static const char frenchName[] = "Nom";
    static const char usName[] = "Px437 IBM VGA8";
    BYTE *font = g_Image + base;
    BYTE *name = font + 64;
    DWORD storage = 6 + 3 * 12;
    DWORD stringAt = 0;
    DWORD index;

    PutBig(font, version, 4);
    PutBig(font + 4, 2, 2);
    PutBig(font + 12, 0x68656164, 4);           /* 'head', ignored */
    PutBig(font + 28, 0x6E616D65, 4);           /* 'name' */
    PutBig(font + 28 + 8, base + 64, 4);
    PutBig(font + 28 + 12, 200, 4);
    PutBig(name + 2, 3, 2);
    PutBig(name + 4, storage, 2);

    /* Macintosh, Roman, English, family */
    PutBig(name + 6, 1, 2);
    PutBig(name + 8, 0, 2);
    PutBig(name + 10, 0, 2);
    PutBig(name + 12, 1, 2);
    PutBig(name + 14, 7, 2);
    PutBig(name + 16, stringAt, 2);
    memcpy(name + storage + stringAt, macName, 7);
    stringAt += 7;

    /* Windows, Unicode BMP, French */
    PutBig(name + 18, 3, 2);
    PutBig(name + 20, 1, 2);
    PutBig(name + 22, 0x040C, 2);
    PutBig(name + 24, 1, 2);
    PutBig(name + 26, 6, 2);
    PutBig(name + 28, stringAt, 2);

    for (index = 0; index < 3; ++index)
        PutBig(name + storage + stringAt + index * 2, (BYTE)frenchName[index], 2);

    stringAt += 6;

    /* Windows, Unicode BMP, US English */
    PutBig(name + 30, 3, 2);
    PutBig(name + 32, 1, 2);
    PutBig(name + 34, 0x0409, 2);
    PutBig(name + 36, 1, 2);
    PutBig(name + 38, 28, 2);
    PutBig(name + 40, stringAt, 2);

    for (index = 0; index < 14; ++index)
        PutBig(name + storage + stringAt + index * 2, (BYTE)usName[index], 2);

    return base + 64 + 200;
}

/* A .fon: MZ, NE at 0x80, resource table at NE+0x40 with shift 4, one RT_NAMETABLE-like
 * resource first (type 0x800F) then RT_FONT at 0x200, whose dfFace points at "Terminal".
 */
static DWORD BuildFon(VOID)
{
    BYTE *ne = g_Image + 0x80;
    BYTE *table = ne + 0x40;
    BYTE *fnt = g_Image + 0x200;

    g_Image[0] = 'M';
    g_Image[1] = 'Z';
    PutLittle(g_Image + 0x3C, 0x80, 4);
    ne[0] = 'N';
    ne[1] = 'E';
    PutLittle(ne + 0x24, 0x40, 2);
    PutLittle(table, 4, 2);                                     /* alignment shift */
    PutLittle(table + 2, 0x800F, 2);                            /* a type that is not RT_FONT */
    PutLittle(table + 4, 1, 2);
    PutLittle(table + 2 + 8, 0x0100 >> 4, 2);
    PutLittle(table + 2 + 20, 0x8008, 2);                       /* RT_FONT */
    PutLittle(table + 2 + 22, 1, 2);
    PutLittle(table + 2 + 28, 0x200 >> 4, 2);
    PutLittle(fnt, 0x300, 2);
    PutLittle(fnt + 105, 0x100, 4);
    memcpy(fnt + 0x100, "Terminal", 9);
    return 0x200 + 0x100 + 9;
}

INT main(VOID)
{
    CHAR face[64];
    DWORD length;

    printf("== text-mode font file: face names ==\n");

    memset(g_Image, 0, sizeof g_Image);
    length = BuildSfnt(0, 0x00010000);
    CHECK(FontFileFaceName(g_Image, length, face, sizeof face) == FONT_FILE_TRUETYPE, "TrueType: recognised");
    CHECK(strcmp(face, "Px437 IBM VGA8") == 0, "TrueType: the Windows US English family name wins");

    memset(g_Image, 0, sizeof g_Image);
    length = BuildSfnt(0, 0x4F54544F);
    CHECK(FontFileFaceName(g_Image, length, face, sizeof face) == FONT_FILE_TRUETYPE, "OpenType (OTTO): recognised");
    CHECK(strcmp(face, "Px437 IBM VGA8") == 0, "OpenType: same name");

    memset(g_Image, 0, sizeof g_Image);
    length = BuildSfnt(0, 0x00010000);
    PutBig(g_Image + 64 + 30, 7, 2);                            /* the US record's platform -> unknown */
    CHECK(FontFileFaceName(g_Image, length, face, sizeof face) == FONT_FILE_TRUETYPE && strcmp(face, "Nom") == 0,
          "TrueType: another Windows language beats Macintosh");
    PutBig(g_Image + 64 + 18, 7, 2);                            /* the French one too */
    CHECK(FontFileFaceName(g_Image, length, face, sizeof face) == FONT_FILE_TRUETYPE && strcmp(face, "MacName") == 0,
          "TrueType: Macintosh Roman as the last resort");
    PutBig(g_Image + 64 + 6, 7, 2);
    CHECK(FontFileFaceName(g_Image, length, face, sizeof face) == FONT_FILE_NOT_FONT && face[0] == 0,
          "TrueType: no family record at all -> refused, face empty");

    memset(g_Image, 0, sizeof g_Image);
    PutBig(g_Image, 0x74746366, 4);                             /* 'ttcf' */
    PutBig(g_Image + 8, 1, 4);
    PutBig(g_Image + 12, 256, 4);
    length = BuildSfnt(256, 0x00010000);
    CHECK(FontFileFaceName(g_Image, length, face, sizeof face) == FONT_FILE_TRUETYPE && strcmp(face, "Px437 IBM VGA8") == 0,
          "TrueType collection: the first font's name");

    memset(g_Image, 0, sizeof g_Image);
    length = BuildSfnt(0, 0x00010000);
    CHECK(FontFileFaceName(g_Image, 100, face, sizeof face) == FONT_FILE_NOT_FONT, "TrueType truncated before its name table -> refused");
    PutBig(g_Image + 28 + 8, 0xFFFFFF00, 4);
    CHECK(FontFileFaceName(g_Image, length, face, sizeof face) == FONT_FILE_NOT_FONT, "TrueType: name table offset past the end -> refused");

    memset(g_Image, 0, sizeof g_Image);
    length = BuildSfnt(0, 0x00010000);
    PutBig(g_Image + 64 + 42 + 13, 0x00E9, 2);                  /* the US name's first char -> e-acute */
    CHECK(FontFileFaceName(g_Image, length, face, sizeof face) == FONT_FILE_TRUETYPE && strcmp(face, "Nom") == 0,
          "TrueType: a non-ASCII name is skipped for the next best");

    memset(g_Image, 0, sizeof g_Image);
    length = BuildSfnt(0, 0x00010000);
    PutBig(g_Image + 64 + 38, 64, 2);                           /* 32 characters: too long for LF_FACESIZE */
    PutBig(g_Image + 64 + 26, 0, 2);                            /* French: empty */
    PutBig(g_Image + 64 + 14, 0, 2);                            /* Macintosh: empty */
    CHECK(FontFileFaceName(g_Image, length, face, sizeof face) == FONT_FILE_NOT_FONT, "TrueType: a name longer than 31 characters -> refused");

    memset(g_Image, 0, sizeof g_Image);
    length = BuildFon();
    CHECK(FontFileFaceName(g_Image, length, face, sizeof face) == FONT_FILE_BITMAP, ".fon: recognised");
    CHECK(strcmp(face, "Terminal") == 0, ".fon: the RT_FONT's dfFace, past another resource type");
    PutLittle(g_Image + 0x200, 0x100, 2);
    CHECK(FontFileFaceName(g_Image, length, face, sizeof face) == FONT_FILE_NOT_FONT, ".fon: an FNT version that is not 2.0/3.0 -> refused");
    PutLittle(g_Image + 0x200, 0x200, 2);
    CHECK(FontFileFaceName(g_Image, length, face, sizeof face) == FONT_FILE_BITMAP, ".fon: FNT 2.0 accepted");
    CHECK(FontFileFaceName(g_Image, length - 1, face, sizeof face) == FONT_FILE_NOT_FONT, ".fon: name cut off by the end of the file -> refused");
    g_Image[0x80] = 'P';
    CHECK(FontFileFaceName(g_Image, length, face, sizeof face) == FONT_FILE_NOT_FONT, ".fon: not an NE -> refused");

    memset(g_Image, 0, sizeof g_Image);
    memcpy(g_Image, "RIFF....sfbk", 12);
    CHECK(FontFileFaceName(g_Image, 12, face, sizeof face) == FONT_FILE_NOT_FONT, "a SoundFont is not a font");
    CHECK(FontFileFaceName(g_Image, 0, face, sizeof face) == FONT_FILE_NOT_FONT, "an empty file is not a font");
    CHECK(FontFileFaceName(NULL, 10, face, sizeof face) == FONT_FILE_NOT_FONT, "no bytes -> refused");

    printf("== %d checks, %d failed\n", g_Total, g_Failures);
    return g_Failures ? 1 : 0;
}
