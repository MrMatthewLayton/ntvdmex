/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * A read-only ISO 9660 reader.  GH #241.
 *
 * The function definitions of dos_iso9660.h, which keeps their declarations and doc comments.
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#include "dos_iso9660.h"

#define ISO_STANDARD_ID_LENGTH  5
#define ISO_PATH_PART_SIZE      ISO_NAME_SIZE
#define ISO_CASE_BIT            0x20
#define ISO_VERSION_MARK        ';'
#define ISO_SELF_ID             0x00
#define ISO_PARENT_ID           0x01

typedef struct _ISO_FIND
{
    PCSTR     Name;
    INT       IsFound;
    PISO_ENTRY Result;
} ISO_FIND;

static const CHAR g_IsoStandardId[ISO_STANDARD_ID_LENGTH] = { 'C', 'D', '0', '0', '1' };

/* The little-endian half of a both-byte-order field (ECMA-119 7.2.3 / 7.3.3). */
static DWORD IsoDword(PCBYTE bytes)
{
    return (DWORD)bytes[0] | ((DWORD)bytes[1] << 8) | ((DWORD)bytes[2] << 16) | ((DWORD)bytes[3] << 24);
}

static CHAR IsoUpper(CHAR character)
{
    return (character >= 'a' && character <= 'z') ? (CHAR)(character & ~ISO_CASE_BIT) : character;
}

INT IsoEntryFromRecord(PCBYTE record, DWORD length, PISO_ENTRY entry)
{
    DWORD nameLength;
    DWORD index;
    DWORD out = 0;

    if (length < ISO_RECORD_MIN || record[ISO_RECORD_LENGTH] < ISO_RECORD_MIN || record[ISO_RECORD_LENGTH] > length)
        return 0;

    nameLength = record[ISO_RECORD_NAME_LENGTH];

    if (ISO_RECORD_NAME + nameLength > record[ISO_RECORD_LENGTH])
        return 0;

    entry->Extent = IsoDword(record + ISO_RECORD_EXTENT);
    entry->Size = IsoDword(record + ISO_RECORD_DATA_LENGTH);
    entry->IsDirectory = (BYTE)((record[ISO_RECORD_FLAGS] & ISO_FLAG_DIRECTORY) != 0);
    entry->IsHidden = (BYTE)((record[ISO_RECORD_FLAGS] & ISO_FLAG_HIDDEN) != 0);

    for (index = 0; index < sizeof entry->Date; ++index)
        entry->Date[index] = record[ISO_RECORD_DATE + index];

    entry->RecordLength = record[ISO_RECORD_LENGTH];

    for (index = 0; index < entry->RecordLength; ++index)
        entry->Record[index] = record[index];

    /* NAME.EXT;1 -> NAME.EXT, and a level-1 name with no extension ("README.;1") loses
     * the dot too (ECMA-119 7.5.1: the separators are always recorded).
     */
    for (index = 0; index < nameLength && out < ISO_NAME_SIZE - 1; ++index)
    {
        CHAR character = (CHAR)record[ISO_RECORD_NAME + index];

        if (character == ISO_VERSION_MARK)
            break;

        entry->Name[out++] = character;
    }

    if (out > 1 && entry->Name[out - 1] == '.')
        --out;

    entry->Name[out] = 0;

    if (nameLength == 1 && (record[ISO_RECORD_NAME] == ISO_SELF_ID || record[ISO_RECORD_NAME] == ISO_PARENT_ID))
        entry->Name[0] = 0;                                       /* "." and ".." have no name here */

    return 1;
}

INT IsoReadDescriptor(PCISO_VOLUME volume, DWORD index, PBYTE buffer)
{
    if (!volume->Read(volume->Context, ISO_FIRST_DESCRIPTOR_SECTOR + index, 1, buffer))
        return -1;

    return buffer[0];
}

INT IsoMount(PISO_VOLUME volume, PISO_READ read, PVOID context)
{
    DWORD index;

    volume->Read = read;
    volume->Context = context;
    volume->VolumeSectors = 0;
    volume->PrimarySector = 0;

    for (index = 0; index < ISO_DESCRIPTOR_LIMIT; ++index)
    {
        DWORD byteIndex;
        INT type = IsoReadDescriptor(volume, index, volume->Primary);

        if (type < 0)
            return 0;

        for (byteIndex = 0; byteIndex < ISO_STANDARD_ID_LENGTH; ++byteIndex)
            if (volume->Primary[ISO_PVD_STANDARD_ID + byteIndex] != (BYTE)g_IsoStandardId[byteIndex])
                return 0;                                         /* not ISO 9660 at all */

        if (type == ISO_DESCRIPTOR_TERMINATOR)
            return 0;

        if (type != ISO_DESCRIPTOR_PRIMARY)
            continue;

        /* 8.4.12: only 2048-byte logical blocks are read here (every CD-ROM in practice). */
        if ((DWORD)(volume->Primary[ISO_PVD_BLOCK_SIZE] | (volume->Primary[ISO_PVD_BLOCK_SIZE + 1] << 8)) != ISO_SECTOR_SIZE)
            return 0;

        volume->VolumeSectors = IsoDword(volume->Primary + ISO_PVD_VOLUME_SPACE);
        volume->PrimarySector = ISO_FIRST_DESCRIPTOR_SECTOR + index;

        if (!IsoEntryFromRecord(volume->Primary + ISO_PVD_ROOT_RECORD, ISO_RECORD_MIN, &volume->Root) || !volume->Root.IsDirectory)
            return 0;

        return 1;
    }

    return 0;
}

INT IsoEnumerate(PCISO_VOLUME volume, PCISO_ENTRY directory, PISO_VISIT visit, PVOID context)
{
    BYTE sector[ISO_SECTOR_SIZE];
    DWORD sectors = (directory->Size + ISO_SECTOR_SIZE - 1) / ISO_SECTOR_SIZE;
    DWORD sectorIndex;
    INT visited = 0;

    for (sectorIndex = 0; sectorIndex < sectors; ++sectorIndex)
    {
        DWORD offset = 0;

        if (!volume->Read(volume->Context, directory->Extent + sectorIndex, 1, sector))
            return visited;

        while (offset < ISO_SECTOR_SIZE)
        {
            ISO_ENTRY entry;
            DWORD length = sector[offset];

            if (!length)
                break;                                            /* 6.8.1: padding to the sector's end */

            if (offset + length > ISO_SECTOR_SIZE || !IsoEntryFromRecord(sector + offset, ISO_SECTOR_SIZE - offset, &entry))
                break;

            offset += length;

            if (!entry.Name[0])
                continue;                                         /* "." and ".." */

            ++visited;

            if (visit(context, &entry))
                return visited;
        }
    }

    return visited;
}

static INT IsoNameEquals(PCSTR left, PCSTR right)
{
    while (*left && *right)
    {
        if (IsoUpper(*left) != IsoUpper(*right))
            return 0;

        ++left;
        ++right;
    }

    return *left == *right;
}

static INT IsoFindVisit(PVOID context, PCISO_ENTRY entry)
{
    ISO_FIND *find = (ISO_FIND *)context;

    if (!IsoNameEquals(entry->Name, find->Name))
        return 0;

    *find->Result = *entry;
    find->IsFound = 1;
    return 1;
}

INT IsoLookup(PCISO_VOLUME volume, PCSTR path, PISO_ENTRY entry)
{
    CHAR part[ISO_PATH_PART_SIZE];
    ISO_ENTRY current = volume->Root;

    while (*path)
    {
        INT length = 0;
        ISO_FIND find;

        while (*path == '\\' || *path == '/')
            ++path;

        if (!*path)
            break;

        while (*path && *path != '\\' && *path != '/')
        {
            if (length < ISO_PATH_PART_SIZE - 1)
                part[length++] = *path;

            ++path;
        }

        part[length] = 0;

        if (!current.IsDirectory)
            return 0;                                             /* a file has no children */

        {   ISO_ENTRY child;
            find.Name = part;
            find.IsFound = 0;
            find.Result = &child;                               /* never `current`: it is being walked */
            IsoEnumerate(volume, &current, IsoFindVisit, &find);

            if (!find.IsFound)
                return 0;

            current = child; }
    }

    *entry = current;
    return 1;
}

DWORD IsoReadFile(PCISO_VOLUME volume, PCISO_ENTRY file, DWORD offset, PBYTE buffer, DWORD count)
{
    BYTE sector[ISO_SECTOR_SIZE];
    DWORD done = 0;

    if (offset >= file->Size)
        return 0;

    if (count > file->Size - offset)
        count = file->Size - offset;

    while (done < count)
    {
        DWORD position = offset + done;
        DWORD within = position % ISO_SECTOR_SIZE;
        DWORD take = ISO_SECTOR_SIZE - within;
        DWORD index;

        if (take > count - done)
            take = count - done;

        if (!volume->Read(volume->Context, file->Extent + position / ISO_SECTOR_SIZE, 1, sector))
            break;

        for (index = 0; index < take; ++index)
            buffer[done + index] = sector[within + index];

        done += take;
    }

    return done;
}

VOID IsoVolumeLabel(PCISO_VOLUME volume, PSTR out)
{
    INT length = 0;

    while (length < ISO_LABEL_SIZE && volume->Primary[ISO_PVD_VOLUME_ID + length])
    {
        out[length] = (CHAR)volume->Primary[ISO_PVD_VOLUME_ID + length];
        ++length;
    }

    while (length > 0 && out[length - 1] == ' ')
        --length;

    out[length] = 0;
}

VOID IsoFileIdentifier(PCISO_VOLUME volume, DWORD offset, PSTR out)
{
    INT length = ISO_FILE_ID_SIZE;
    INT index;

    for (index = 0; index < ISO_FILE_ID_SIZE; ++index)
        out[index] = (CHAR)volume->Primary[offset + index];

    while (length > 0 && (out[length - 1] == ' ' || out[length - 1] == 0))
        --length;

    out[length] = 0;
}
