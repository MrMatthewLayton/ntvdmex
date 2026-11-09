/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * A FAT12/FAT16 disk image, read as a filesystem.
 *
 * The function definitions of dos_fatimg.h, which keeps their declarations and doc comments.
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#include "dos_fatimg.h"

/* BPB fields (offsets into sector 0). */
#define FAT_BPB_BYTES_PER_SECTOR        11
#define FAT_BPB_SECTORS_PER_CLUSTER     13
#define FAT_BPB_RESERVED_SECTORS        14
#define FAT_BPB_FAT_COUNT               16
#define FAT_BPB_ROOT_ENTRIES            17
#define FAT_BPB_TOTAL_SECTORS_16        19
#define FAT_BPB_MEDIA                   21
#define FAT_BPB_SECTORS_PER_FAT         22
#define FAT_BPB_TOTAL_SECTORS_32        32

/* Directory entry fields. */
#define FAT_ENTRY_ATTRIBUTES            11
#define FAT_ENTRY_TIME                  22
#define FAT_ENTRY_DATE                  24
#define FAT_ENTRY_FIRST_CLUSTER         26
#define FAT_ENTRY_SIZE                  28
#define FAT_ENTRY_BASE_NAME_SIZE        8
#define FAT_ENTRY_END                   0x00    /* First name byte: nothing after this */
#define FAT_ENTRY_DELETED               0xE5
#define FAT_ENTRY_KANJI_E5              0x05    /* A name that really starts with 0xE5 */
#define FAT_ENTRY_DOT                   '.'

#define FAT_FIRST_DATA_CLUSTER          2
#define FAT12_MAX_CLUSTERS              4084    /* "fewer than 4085 clusters" is FAT12 */
#define FAT16_MAX_CLUSTERS              65524
#define FAT12_END_OF_CHAIN              0xFF8u
#define FAT16_END_OF_CHAIN              0xFFF8u
#define FAT12_ENTRY_MASK                0xFFFu
#define FAT12_ODD_SHIFT                 4
#define FAT_PATH_SEPARATOR              '\\'
#define FAT_PATH_SEPARATOR_ALT          '/'
#define FAT_NAME_PAD                    ' '
#define FAT_CASE_BIT                    0x20

/* Visit the raw 32-byte entries of a directory in order; `visit` returns 0 to stop.
 * The root is a fixed run of RootEntries; any other directory is a cluster chain.
 */
typedef INT (*FAT_VISIT)(PCBYTE raw, PVOID context);

typedef struct _FAT_LIST_CONTEXT
{
    PFAT_ENTRY Entries;
    INT Capacity;
    INT Count;
} FAT_LIST_CONTEXT;

typedef struct _FAT_FIND_CONTEXT
{
    PCSTR Component;
    INT Length;
    PFAT_ENTRY Found;
    INT IsFound;
} FAT_FIND_CONTEXT;

typedef struct _FAT_LABEL_CONTEXT
{
    PSTR Label;
    INT IsFound;
} FAT_LABEL_CONTEXT;

static WORD FatWord(PCBYTE bytes)
{
    return (WORD)(bytes[0] | (bytes[1] << 8));
}

static DWORD FatDword(PCBYTE bytes)
{
    return (DWORD)bytes[0] | ((DWORD)bytes[1] << 8) | ((DWORD)bytes[2] << 16) | ((DWORD)bytes[3] << 24);
}

INT FatImageMount(PFAT_IMAGE image, PCBYTE data, DWORD size)
{
    DWORD rootBytes;
    DWORD dataSectors;
    DWORD firstDataSector;
    DWORD index;

    for (index = 0; index < sizeof(*image); ++index)
        ((PBYTE)image)[index] = 0;

    if (!data || size < FAT_IMAGE_SECTOR_SIZE)
        return 0;

    image->Data = data;
    image->Size = size;
    image->BytesPerSector = FatWord(data + FAT_BPB_BYTES_PER_SECTOR);
    image->SectorsPerCluster = data[FAT_BPB_SECTORS_PER_CLUSTER];
    image->ReservedSectors = FatWord(data + FAT_BPB_RESERVED_SECTORS);
    image->FatCount = data[FAT_BPB_FAT_COUNT];
    image->RootEntries = FatWord(data + FAT_BPB_ROOT_ENTRIES);
    image->TotalSectors = FatWord(data + FAT_BPB_TOTAL_SECTORS_16);
    image->Media = data[FAT_BPB_MEDIA];
    image->SectorsPerFat = FatWord(data + FAT_BPB_SECTORS_PER_FAT);

    if (!image->TotalSectors)
        image->TotalSectors = FatDword(data + FAT_BPB_TOTAL_SECTORS_32);

    /* Only what the spec and this layer agree on: 512-byte sectors (dos_disk.h's one size),
     * a power-of-two cluster, at least one FAT, a root directory, a FAT of some size.
     */
    if (image->BytesPerSector != FAT_IMAGE_SECTOR_SIZE || !image->SectorsPerCluster
        || (image->SectorsPerCluster & (image->SectorsPerCluster - 1)) || !image->FatCount
        || !image->RootEntries || !image->SectorsPerFat || !image->ReservedSectors || !image->TotalSectors)
        return 0;

    rootBytes = (DWORD)image->RootEntries * FAT_IMAGE_ENTRY_SIZE;
    image->FatOffset = (DWORD)image->ReservedSectors * FAT_IMAGE_SECTOR_SIZE;
    image->RootOffset = image->FatOffset + (DWORD)image->FatCount * image->SectorsPerFat * FAT_IMAGE_SECTOR_SIZE;
    firstDataSector = image->RootOffset / FAT_IMAGE_SECTOR_SIZE + (rootBytes + FAT_IMAGE_SECTOR_SIZE - 1) / FAT_IMAGE_SECTOR_SIZE;
    image->DataOffset = firstDataSector * FAT_IMAGE_SECTOR_SIZE;
    image->ClusterSize = (DWORD)image->SectorsPerCluster * FAT_IMAGE_SECTOR_SIZE;

    if (image->TotalSectors <= firstDataSector || image->DataOffset > size)
        return 0;

    dataSectors = image->TotalSectors - firstDataSector;
    image->ClusterCount = dataSectors / image->SectorsPerCluster;

    if (!image->ClusterCount || image->ClusterCount > FAT16_MAX_CLUSTERS)
        return 0;                                       /* FAT32 is not a floppy */

    image->IsFat16 = (BYTE)(image->ClusterCount > FAT12_MAX_CLUSTERS);

    /* The FAT must be able to describe every cluster, and must be inside the file. */
    if ((image->IsFat16 ? (image->ClusterCount + FAT_FIRST_DATA_CLUSTER) * 2
                        : ((image->ClusterCount + FAT_FIRST_DATA_CLUSTER) * 3 + 1) / 2)
        > (DWORD)image->SectorsPerFat * FAT_IMAGE_SECTOR_SIZE
        || image->RootOffset + rootBytes > size)
        return 0;

    return 1;
}

INT FatImageIsDataCluster(PCFAT_IMAGE image, DWORD cluster)
{
    return cluster >= FAT_FIRST_DATA_CLUSTER && cluster < image->ClusterCount + FAT_FIRST_DATA_CLUSTER;
}

DWORD FatImageNextCluster(PCFAT_IMAGE image, DWORD cluster)
{
    PCBYTE fat = image->Data + image->FatOffset;

    if (image->IsFat16)
        return FatWord(fat + cluster * 2);

    {
        DWORD offset = cluster + cluster / 2;
        WORD packed = FatWord(fat + offset);
        return (cluster & 1) ? (DWORD)(packed >> FAT12_ODD_SHIFT) : (DWORD)(packed & FAT12_ENTRY_MASK);
    }
}

static INT FatIsEndOfChain(PCFAT_IMAGE image, DWORD cluster)
{
    return cluster >= (image->IsFat16 ? FAT16_END_OF_CHAIN : FAT12_END_OF_CHAIN);
}

/* "NAME    EXT" -> "NAME.EXT". */
static VOID FatEntryName(PCBYTE raw, PSTR name)
{
    INT length = 0;
    INT index;
    INT end;

    for (end = FAT_ENTRY_BASE_NAME_SIZE; end > 0 && raw[end - 1] == FAT_NAME_PAD; --end)
    {
    }

    for (index = 0; index < end; ++index)
        name[length++] = (CHAR)((index == 0 && raw[0] == FAT_ENTRY_KANJI_E5) ? FAT_ENTRY_DELETED : raw[index]);

    for (end = FAT_IMAGE_RAW_NAME_SIZE; end > FAT_ENTRY_BASE_NAME_SIZE && raw[end - 1] == FAT_NAME_PAD; --end)
    {
    }

    if (end > FAT_ENTRY_BASE_NAME_SIZE)
    {
        name[length++] = '.';

        for (index = FAT_ENTRY_BASE_NAME_SIZE; index < end; ++index)
            name[length++] = (CHAR)raw[index];
    }

    name[length] = 0;
}

static VOID FatEntryRead(PCBYTE raw, PFAT_ENTRY entry)
{
    INT index;

    for (index = 0; index < FAT_IMAGE_RAW_NAME_SIZE; ++index)
        entry->RawName[index] = raw[index];

    FatEntryName(raw, entry->Name);
    entry->Attributes = raw[FAT_ENTRY_ATTRIBUTES];
    entry->Time = FatWord(raw + FAT_ENTRY_TIME);
    entry->Date = FatWord(raw + FAT_ENTRY_DATE);
    entry->FirstCluster = FatWord(raw + FAT_ENTRY_FIRST_CLUSTER);
    entry->Size = FatDword(raw + FAT_ENTRY_SIZE);
}

static VOID FatWalkDirectory(PCFAT_IMAGE image, WORD firstCluster, FAT_VISIT visit, PVOID context)
{
    DWORD entryIndex;

    if (!firstCluster)
    {
        for (entryIndex = 0; entryIndex < image->RootEntries; ++entryIndex)
            if (!visit(image->Data + image->RootOffset + entryIndex * FAT_IMAGE_ENTRY_SIZE, context))
                return;

        return;
    }

    {
        DWORD cluster = firstCluster;
        DWORD steps = 0;
        DWORD perCluster = image->ClusterSize / FAT_IMAGE_ENTRY_SIZE;

        while (FatImageIsDataCluster(image, cluster) && steps++ <= image->ClusterCount)
        {
            DWORD clusterOffset = image->DataOffset + (cluster - FAT_FIRST_DATA_CLUSTER) * image->ClusterSize;

            if (clusterOffset + image->ClusterSize > image->Size)
                return;

            for (entryIndex = 0; entryIndex < perCluster; ++entryIndex)
                if (!visit(image->Data + clusterOffset + entryIndex * FAT_IMAGE_ENTRY_SIZE, context))
                    return;

            cluster = FatImageNextCluster(image, cluster);
        }
    }
}

static INT FatListVisit(PCBYTE raw, PVOID context)
{
    FAT_LIST_CONTEXT *list = (FAT_LIST_CONTEXT *)context;
    BYTE attributes = raw[FAT_ENTRY_ATTRIBUTES];

    if (raw[0] == FAT_ENTRY_END)
        return 0;

    if (raw[0] == FAT_ENTRY_DELETED || raw[0] == FAT_ENTRY_DOT
        || (attributes & FAT_ATTRIBUTE_LONG_NAME) == FAT_ATTRIBUTE_LONG_NAME || (attributes & FAT_ATTRIBUTE_VOLUME))
        return 1;

    if (list->Entries && list->Count < list->Capacity)
        FatEntryRead(raw, &list->Entries[list->Count]);

    list->Count++;
    return 1;
}

INT FatImageListDirectory(PCFAT_IMAGE image, WORD firstCluster, PFAT_ENTRY entries, INT capacity)
{
    FAT_LIST_CONTEXT list;

    list.Entries = entries;
    list.Capacity = capacity;
    list.Count = 0;
    FatWalkDirectory(image, firstCluster, FatListVisit, &list);
    return list.Count;
}

/* One path component against an entry name, case-insensitively. */
static INT FatNameMatches(PCSTR component, INT length, PCSTR name)
{
    INT index;

    for (index = 0; index < length; ++index)
    {
        CHAR left = component[index];
        CHAR right = name[index];

        if (!right)
            return 0;

        if (left >= 'a' && left <= 'z')
            left = (CHAR)(left & ~FAT_CASE_BIT);

        if (right >= 'a' && right <= 'z')
            right = (CHAR)(right & ~FAT_CASE_BIT);

        if (left != right)
            return 0;
    }

    return name[length] == 0;
}

static INT FatFindVisit(PCBYTE raw, PVOID context)
{
    FAT_FIND_CONTEXT *find = (FAT_FIND_CONTEXT *)context;
    FAT_ENTRY entry;
    BYTE attributes = raw[FAT_ENTRY_ATTRIBUTES];

    if (raw[0] == FAT_ENTRY_END)
        return 0;

    if (raw[0] == FAT_ENTRY_DELETED || raw[0] == FAT_ENTRY_DOT
        || (attributes & FAT_ATTRIBUTE_LONG_NAME) == FAT_ATTRIBUTE_LONG_NAME || (attributes & FAT_ATTRIBUTE_VOLUME))
        return 1;

    FatEntryRead(raw, &entry);

    if (!FatNameMatches(find->Component, find->Length, entry.Name))
        return 1;

    *find->Found = entry;
    find->IsFound = 1;
    return 0;
}

INT FatImageLookup(PCFAT_IMAGE image, PCSTR path, PFAT_ENTRY entry)
{
    FAT_ENTRY current;
    PCSTR cursor = path;
    INT index;

    for (index = 0; index < (INT)sizeof(current); ++index)
        ((PBYTE)&current)[index] = 0;

    current.Attributes = FAT_ATTRIBUTE_DIRECTORY;          /* the root */

    while (*cursor)
    {
        FAT_FIND_CONTEXT find;
        INT length = 0;

        while (cursor[length] && cursor[length] != FAT_PATH_SEPARATOR && cursor[length] != FAT_PATH_SEPARATOR_ALT)
            ++length;

        if (!length)
        {
            ++cursor;                                       /* "A\\\\B": an empty component */
            continue;
        }

        if (!(current.Attributes & FAT_ATTRIBUTE_DIRECTORY))
            return 0;                                       /* a file in the middle of the path */

        find.Component = cursor;
        find.Length = length;
        find.Found = &current;
        find.IsFound = 0;
        FatWalkDirectory(image, current.FirstCluster, FatFindVisit, &find);

        if (!find.IsFound)
            return 0;

        cursor += length;
    }

    *entry = current;
    return 1;
}

DWORD FatImageReadFile(
    PCFAT_IMAGE image,
    PCFAT_ENTRY entry,
    DWORD offset,
    PBYTE buffer,
    DWORD count)
{
    DWORD cluster = entry->FirstCluster;
    DWORD skip;
    DWORD copied = 0;
    DWORD steps = 0;

    if (offset >= entry->Size || !count)
        return 0;

    if (count > entry->Size - offset)
        count = entry->Size - offset;

    for (skip = offset / image->ClusterSize; skip; --skip)
    {
        if (!FatImageIsDataCluster(image, cluster) || steps++ > image->ClusterCount)
            return 0;

        cluster = FatImageNextCluster(image, cluster);
    }

    offset %= image->ClusterSize;

    while (copied < count)
    {
        DWORD clusterOffset;
        DWORD chunk;
        DWORD index;

        if (!FatImageIsDataCluster(image, cluster) || steps++ > image->ClusterCount)
            break;                                          /* the chain ends before the size does */

        clusterOffset = image->DataOffset + (cluster - FAT_FIRST_DATA_CLUSTER) * image->ClusterSize;

        if (clusterOffset + image->ClusterSize > image->Size)
            break;

        chunk = image->ClusterSize - offset;

        if (chunk > count - copied)
            chunk = count - copied;

        for (index = 0; index < chunk; ++index)
            buffer[copied + index] = image->Data[clusterOffset + offset + index];

        copied += chunk;
        offset = 0;
        cluster = FatImageNextCluster(image, cluster);

        if (FatIsEndOfChain(image, cluster) && copied < count)
            break;
    }

    return copied;
}

DWORD FatImageFreeClusters(PCFAT_IMAGE image)
{
    DWORD cluster;
    DWORD freeClusters = 0;

    for (cluster = FAT_FIRST_DATA_CLUSTER; cluster < image->ClusterCount + FAT_FIRST_DATA_CLUSTER; ++cluster)
        if (!FatImageNextCluster(image, cluster))
            ++freeClusters;

    return freeClusters;
}

static INT FatLabelVisit(PCBYTE raw, PVOID context)
{
    FAT_LABEL_CONTEXT *label = (FAT_LABEL_CONTEXT *)context;
    BYTE attributes = raw[FAT_ENTRY_ATTRIBUTES];
    INT end;
    INT index;

    if (raw[0] == FAT_ENTRY_END)
        return 0;

    if (raw[0] == FAT_ENTRY_DELETED || (attributes & FAT_ATTRIBUTE_LONG_NAME) == FAT_ATTRIBUTE_LONG_NAME
        || !(attributes & FAT_ATTRIBUTE_VOLUME))
        return 1;

    for (end = FAT_IMAGE_RAW_NAME_SIZE; end > 0 && raw[end - 1] == FAT_NAME_PAD; --end)
    {
    }

    for (index = 0; index < end; ++index)
        label->Label[index] = (CHAR)raw[index];

    label->Label[end] = 0;
    label->IsFound = 1;
    return 0;
}

/* Append one component to `resolved` (length `*length`), or go up a level for "..". */
static INT FatResolveComponent(
    PCSTR component,
    INT componentLength,
    PSTR resolved,
    INT *length,
    INT capacity)
{
    INT index;

    if (componentLength == 1 && component[0] == '.')
        return 1;

    if (componentLength == 2 && component[0] == '.' && component[1] == '.')
    {
        while (*length > 0 && resolved[*length - 1] != FAT_PATH_SEPARATOR)
            --*length;

        if (*length > 0)
            --*length;                                      /* the separator before it */

        resolved[*length] = 0;
        return 1;
    }

    if (*length + (*length ? 1 : 0) + componentLength >= capacity)
        return 0;

    if (*length)
        resolved[(*length)++] = FAT_PATH_SEPARATOR;

    for (index = 0; index < componentLength; ++index)
    {
        CHAR character = component[index];
        resolved[(*length)++] = (character >= 'a' && character <= 'z') ? (CHAR)(character & ~FAT_CASE_BIT) : character;
    }

    resolved[*length] = 0;
    return 1;
}

static INT FatResolveAppend(PCSTR path, PSTR resolved, INT *length, INT capacity)
{
    while (*path)
    {
        INT componentLength = 0;

        while (path[componentLength] && path[componentLength] != FAT_PATH_SEPARATOR && path[componentLength] != FAT_PATH_SEPARATOR_ALT)
            ++componentLength;

        if (componentLength && !FatResolveComponent(path, componentLength, resolved, length, capacity))
            return 0;

        path += componentLength;

        if (*path)
            ++path;
    }

    return 1;
}

INT FatImageResolvePath(PCSTR current, PCSTR path, PSTR resolved, INT capacity)
{
    INT length = 0;

    if (capacity < 1)
        return 0;

    resolved[0] = 0;

    if (path[0] != FAT_PATH_SEPARATOR && path[0] != FAT_PATH_SEPARATOR_ALT
        && !FatResolveAppend(current, resolved, &length, capacity))
        return 0;

    return FatResolveAppend(path, resolved, &length, capacity);
}

INT FatImageVolumeLabel(PCFAT_IMAGE image, PSTR label)
{
    FAT_LABEL_CONTEXT context;

    label[0] = 0;
    context.Label = label;
    context.IsFound = 0;
    FatWalkDirectory(image, 0, FatLabelVisit, &context);
    return context.IsFound;
}
