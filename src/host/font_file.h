/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * The face name inside a font file the user chose for text mode.
 *
 * WHY THE NAME IS READ FROM THE FILE (Importance = 2):
 * NTVDMEX ships no font. Settings > Display > Text Mode Font takes a FILE -- a font the user
 * owns a licence for -- and the host loads it for itself alone (AddFontResourceEx with
 * FR_PRIVATE: nothing is installed in Windows). GDI then selects it by FACE NAME, and
 * AddFontResourceEx does not say which names it added, so the name is read from the file:
 * - TrueType / OpenType (.ttf, .otf; a .ttc collection's first font): the 'name' table's
 *   font family name, name ID 1 -- the name GDI selects a face by. A Windows-platform
 *   record (platform 3) is preferred, US English first; a Macintosh Roman one (platform 1)
 *   is the fallback. Layouts per the OpenType specification: the table directory
 *   ("Organization of an OpenType Font"), the TTC header, and the 'name' table.
 * - Windows bitmap fonts (.fon): an NE executable whose first RT_FONT resource is a font
 *   file of the Windows 2.0/3.0 FNT format; its dfFace field is the offset of the face
 *   name (the FNT header layout the default font's reader in sysfont.c uses too).
 * A name GDI could not take (empty, longer than LF_FACESIZE - 1, or not ASCII) is refused,
 * so the caller falls back to the default font and says why, rather than selecting a
 * substitute.
 *
 * Pure C, no <windows.h>: tests/unit/fontfile_test.c pins it off-VM.
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#ifndef NTVDMEX_FONT_FILE_H
#define NTVDMEX_FONT_FILE_H

#include "../ntvdmex_types.h"

#define FONT_FILE_FACE_CAPACITY         32              /* LF_FACESIZE, terminator included */

/* sfnt: the offset table, the table directory, the TTC header (OpenType specification). */
#define FONT_FILE_SFNT_TRUETYPE         0x00010000u     /* sfntVersion of TrueType outlines */
#define FONT_FILE_SFNT_OPENTYPE         0x4F54544Fu     /* 'OTTO': CFF outlines */
#define FONT_FILE_SFNT_APPLE            0x74727565u     /* 'true': Apple's TrueType tag */
#define FONT_FILE_TTC_TAG               0x74746366u     /* 'ttcf' */
#define FONT_FILE_TTC_FIRST_OFFSET      12              /* tableDirectoryOffsets[0] */
#define FONT_FILE_SFNT_NUM_TABLES       4
#define FONT_FILE_SFNT_HEADER_SIZE      12
#define FONT_FILE_TABLE_RECORD_SIZE     16              /* Tag, checksum, offset, length */
#define FONT_FILE_TABLE_OFFSET          8
#define FONT_FILE_TABLE_LENGTH          12
#define FONT_FILE_NAME_TAG              0x6E616D65u     /* 'name' */
#define FONT_FILE_NAME_COUNT            2
#define FONT_FILE_NAME_STORAGE          4
#define FONT_FILE_NAME_HEADER_SIZE      6
#define FONT_FILE_NAME_RECORD_SIZE      12              /* platform, encoding, language, name, length, offset */
#define FONT_FILE_RECORD_LANGUAGE       4
#define FONT_FILE_RECORD_NAME_ID        6
#define FONT_FILE_RECORD_LENGTH         8
#define FONT_FILE_RECORD_OFFSET         10
#define FONT_FILE_NAME_ID_FAMILY        1
#define FONT_FILE_PLATFORM_MACINTOSH    1
#define FONT_FILE_PLATFORM_WINDOWS      3
#define FONT_FILE_LANGUAGE_US_ENGLISH   0x0409          /* Windows platform */
#define FONT_FILE_ASCII_LAST            0x7E
#define FONT_FILE_ASCII_FIRST           0x20

/* NE resources and the FNT header. */
#define FONT_FILE_MZ_NE_OFFSET          0x3C            /* e_lfanew */
#define FONT_FILE_NE_HEADER_SIZE        0x40
#define FONT_FILE_NE_RESOURCE_TABLE     0x24
#define FONT_FILE_RESOURCE_TYPE_SIZE    8               /* type id, count, reserved */
#define FONT_FILE_RESOURCE_ENTRY_SIZE   12
#define FONT_FILE_RT_FONT               0x8008
#define FONT_FILE_FNT_VERSION_2         0x200
#define FONT_FILE_FNT_VERSION_3         0x300
#define FONT_FILE_FNT_FACE              105             /* dfFace: the name's offset in the FNT */
#define FONT_FILE_FNT_MIN_SIZE          118             /* The 2.0 header */

/* What FontFileFaceName found. */
enum
{
    FONT_FILE_NOT_FONT = 0, FONT_FILE_TRUETYPE, FONT_FILE_BITMAP
};

/* How well a name record suits: higher wins. */
enum
{
    FONT_FILE_RANK_NONE = 0, FONT_FILE_RANK_MACINTOSH, FONT_FILE_RANK_WINDOWS, FONT_FILE_RANK_WINDOWS_US
};

static inline DWORD FontFileBig(const BYTE *bytes, DWORD length, DWORD offset, INT size)
{
    DWORD value = 0;
    INT index;

    if (offset > length || (DWORD)size > length - offset)
        return 0;

    for (index = 0; index < size; ++index)
        value = (value << 8) | bytes[offset + (DWORD)index];

    return value;
}

static inline DWORD FontFileLittle(const BYTE *bytes, DWORD length, DWORD offset, INT size)
{
    DWORD value = 0;
    INT index;

    if (offset > length || (DWORD)size > length - offset)
        return 0;

    for (index = size - 1; index >= 0; --index)
        value = (value << 8) | bytes[offset + (DWORD)index];

    return value;
}

static inline INT FontFileIsSfnt(DWORD version)
{
    return version == FONT_FILE_SFNT_TRUETYPE
        || version == FONT_FILE_SFNT_OPENTYPE
        || version == FONT_FILE_SFNT_APPLE;
}

/* One name record's string into `face`: UTF-16BE (Windows) or one byte a character (Macintosh
 * Roman), ASCII only. 1 = taken.
 */
static inline INT FontFileTakeName(
    const BYTE *bytes,
    DWORD length,
    DWORD start,
    DWORD stringLength,
    INT isWide,
    PSTR face,
    INT capacity)
{
    DWORD unit = isWide ? 2 : 1;
    DWORD count = stringLength / unit;
    DWORD index;

    if (!count || count > (DWORD)capacity - 1 || start > length || stringLength > length - start)
        return 0;

    for (index = 0; index < count; ++index)
    {
        DWORD character = FontFileBig(bytes, length, start + index * unit, (INT)unit);

        if (character < FONT_FILE_ASCII_FIRST || character > FONT_FILE_ASCII_LAST)
            return 0;

        face[index] = (CHAR)character;
    }

    face[count] = 0;
    return 1;
}

static inline INT FontFileSfntFace(
    const BYTE *bytes,
    DWORD length,
    DWORD fontOffset,
    PSTR face,
    INT capacity)
{
    DWORD tableCount = FontFileBig(bytes, length, fontOffset + FONT_FILE_SFNT_NUM_TABLES, 2);
    DWORD tableIndex;
    DWORD nameTable = 0;
    DWORD nameLength = 0;
    DWORD recordCount;
    DWORD storage;
    DWORD recordIndex;
    INT bestRank = FONT_FILE_RANK_NONE;

    if (!FontFileIsSfnt(FontFileBig(bytes, length, fontOffset, 4)))
        return 0;

    for (tableIndex = 0; tableIndex < tableCount; ++tableIndex)
    {
        DWORD record = fontOffset + FONT_FILE_SFNT_HEADER_SIZE + tableIndex * FONT_FILE_TABLE_RECORD_SIZE;

        if (record + FONT_FILE_TABLE_RECORD_SIZE > length)
            return 0;

        if (FontFileBig(bytes, length, record, 4) == FONT_FILE_NAME_TAG)
        {
            nameTable = FontFileBig(bytes, length, record + FONT_FILE_TABLE_OFFSET, 4);
            nameLength = FontFileBig(bytes, length, record + FONT_FILE_TABLE_LENGTH, 4);
            break;
        }
    }

    if (!nameTable || nameTable > length || nameLength > length - nameTable || nameLength < FONT_FILE_NAME_HEADER_SIZE)
        return 0;

    recordCount = FontFileBig(bytes, length, nameTable + FONT_FILE_NAME_COUNT, 2);
    storage = nameTable + FontFileBig(bytes, length, nameTable + FONT_FILE_NAME_STORAGE, 2);

    for (recordIndex = 0; recordIndex < recordCount; ++recordIndex)
    {
        DWORD record = nameTable + FONT_FILE_NAME_HEADER_SIZE + recordIndex * FONT_FILE_NAME_RECORD_SIZE;
        DWORD platform;
        DWORD language;
        DWORD stringLength;
        DWORD stringOffset;
        INT rank;

        if (record + FONT_FILE_NAME_RECORD_SIZE > nameTable + nameLength)
            break;

        if (FontFileBig(bytes, length, record + FONT_FILE_RECORD_NAME_ID, 2) != FONT_FILE_NAME_ID_FAMILY)
            continue;

        platform = FontFileBig(bytes, length, record, 2);
        language = FontFileBig(bytes, length, record + FONT_FILE_RECORD_LANGUAGE, 2);
        stringLength = FontFileBig(bytes, length, record + FONT_FILE_RECORD_LENGTH, 2);
        stringOffset = FontFileBig(bytes, length, record + FONT_FILE_RECORD_OFFSET, 2);

        if (platform == FONT_FILE_PLATFORM_WINDOWS)
            rank = language == FONT_FILE_LANGUAGE_US_ENGLISH ? FONT_FILE_RANK_WINDOWS_US : FONT_FILE_RANK_WINDOWS;
        else if (platform == FONT_FILE_PLATFORM_MACINTOSH)
            rank = FONT_FILE_RANK_MACINTOSH;
        else
            continue;

        if (rank <= bestRank)
            continue;

        if (FontFileTakeName(bytes, length, storage + stringOffset, stringLength, platform == FONT_FILE_PLATFORM_WINDOWS, face, capacity))
            bestRank = rank;
    }

    return bestRank != FONT_FILE_RANK_NONE;
}

static inline INT FontFileFonFace(const BYTE *bytes, DWORD length, PSTR face, INT capacity)
{
    DWORD neHeader = FontFileLittle(bytes, length, FONT_FILE_MZ_NE_OFFSET, 4);
    DWORD resourceTable;
    DWORD alignShift;
    DWORD offset;

    if (length < FONT_FILE_MZ_NE_OFFSET + 4
        || bytes[0] != 'M'
        || bytes[1] != 'Z'
        || neHeader > length
        || length - neHeader < FONT_FILE_NE_HEADER_SIZE
        || bytes[neHeader] != 'N'
        || bytes[neHeader + 1] != 'E')
        return 0;

    resourceTable = neHeader + FontFileLittle(bytes, length, neHeader + FONT_FILE_NE_RESOURCE_TABLE, 2);
    alignShift = FontFileLittle(bytes, length, resourceTable, 2);
    offset = resourceTable + 2;

    if (alignShift > 16)
        return 0;

    while (offset + FONT_FILE_RESOURCE_TYPE_SIZE <= length)
    {
        DWORD type = FontFileLittle(bytes, length, offset, 2);
        DWORD count = FontFileLittle(bytes, length, offset + 2, 2);
        DWORD fontOffset;
        DWORD version;
        DWORD faceOffset;
        DWORD index;

        if (!type)
            return 0;

        offset += FONT_FILE_RESOURCE_TYPE_SIZE;

        if (type != FONT_FILE_RT_FONT)
        {
            offset += count * FONT_FILE_RESOURCE_ENTRY_SIZE;
            continue;
        }

        if (!count || offset + FONT_FILE_RESOURCE_ENTRY_SIZE > length)
            return 0;

        fontOffset = FontFileLittle(bytes, length, offset, 2) << alignShift;

        if (fontOffset > length || length - fontOffset < FONT_FILE_FNT_MIN_SIZE)
            return 0;

        version = FontFileLittle(bytes, length, fontOffset, 2);

        if (version != FONT_FILE_FNT_VERSION_2 && version != FONT_FILE_FNT_VERSION_3)
            return 0;

        faceOffset = fontOffset + FontFileLittle(bytes, length, fontOffset + FONT_FILE_FNT_FACE, 4);

        for (index = 0; index < (DWORD)capacity; ++index)
        {
            BYTE character;

            if (faceOffset + index >= length)
                return 0;

            character = bytes[faceOffset + index];

            if (!character)
            {
                face[index] = 0;
                return index > 0;
            }

            if (character < FONT_FILE_ASCII_FIRST || character > FONT_FILE_ASCII_LAST)
                return 0;

            face[index] = (CHAR)character;
        }

        return 0;                                               /* longer than a face name may be */
    }

    return 0;
}

/* The face name GDI will select the font in `bytes` by, into `face` (`capacity` bytes, at
 * most FONT_FILE_FACE_CAPACITY are used). Returns FONT_FILE_*; `face` is "" when it is
 * FONT_FILE_NOT_FONT.
 */
static inline INT FontFileFaceName(const BYTE *bytes, DWORD length, PSTR face, INT capacity)
{
    DWORD version;

    if (!bytes || !face || capacity < 2)
        return FONT_FILE_NOT_FONT;

    if (capacity > FONT_FILE_FACE_CAPACITY)
        capacity = FONT_FILE_FACE_CAPACITY;

    face[0] = 0;
    version = FontFileBig(bytes, length, 0, 4);

    if (version == FONT_FILE_TTC_TAG)
    {
        if (FontFileSfntFace(bytes, length, FontFileBig(bytes, length, FONT_FILE_TTC_FIRST_OFFSET, 4), face, capacity))
            return FONT_FILE_TRUETYPE;
    }
    else if (FontFileIsSfnt(version))
    {
        if (FontFileSfntFace(bytes, length, 0, face, capacity))
            return FONT_FILE_TRUETYPE;
    }
    else if (FontFileFonFace(bytes, length, face, capacity))
    {
        return FONT_FILE_BITMAP;
    }

    face[0] = 0;
    return FONT_FILE_NOT_FONT;
}

#endif /* NTVDMEX_FONT_FILE_H */
