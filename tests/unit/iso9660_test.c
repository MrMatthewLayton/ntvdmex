/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * The ISO 9660 reader behind the MSCDEX drive, pinned off-VM.  GH #241.
 *
 * Builds small images in memory, laid out by ECMA-119 (the PVD at sector 16, a
 * terminator after it, both-byte-order fields, records that never cross a sector), and
 * reads them back through src/dos/dos_iso9660.c.
 *
 *   cc -std=c99 -I src -I src/dos -o iso9660_test tests/unit/iso9660_test.c src/dos/dos_iso9660.c
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#include <stdio.h>
#include <string.h>
#include "dos_iso9660.h"

#define ISO_TEST_SECTORS        32
#define ISO_TEST_ROOT_SECTOR    18  /* Two sectors: 18, 19 */
#define ISO_TEST_DATA_SECTOR    20  /* The DATA subdirectory */
#define ISO_TEST_README_SECTOR  21
#define ISO_TEST_BIG_SECTOR     22  /* Two sectors: 22, 23 */
#define ISO_TEST_DEEP_SECTOR    24  /* DATA\DEEP.DAT */
#define ISO_TEST_BIG_SIZE       3000u

#define CHECK(condition, message) do { ++g_Checks; if (condition) printf("  PASS  %s\n", (message)); \
    else { printf("  FAIL  %s\n", (message)); ++g_Failures; } } while (0)

typedef struct _ISO_TEST_LIST
{
    CHAR Names[8][ISO_NAME_SIZE];
    INT  Count;
} ISO_TEST_LIST;

static INT g_Checks;
static INT g_Failures;
static BYTE g_Image[ISO_TEST_SECTORS * ISO_SECTOR_SIZE];
static INT g_FailReads;

static INT IsoTestRead(PVOID context, DWORD sector, DWORD count, PBYTE buffer)
{
    (VOID)context;

    if (g_FailReads || sector + count > ISO_TEST_SECTORS)
        return 0;

    memcpy(buffer, g_Image + sector * ISO_SECTOR_SIZE, count * ISO_SECTOR_SIZE);
    return 1;
}

static VOID IsoTestBoth32(PBYTE at, DWORD value)
{
    INT index;

    for (index = 0; index < 4; ++index)
    {
        at[index] = (BYTE)(value >> (index * 8));
        at[7 - index] = (BYTE)(value >> (index * 8));             /* the big-endian half */
    }
}

/* One directory record at `at`; returns its length. */
static DWORD IsoTestRecord(
    PBYTE at,
    DWORD extent,
    DWORD size,
    BYTE flags,
    PCSTR name,
    DWORD nameLength)
{
    DWORD length = ISO_RECORD_NAME + nameLength;

    if (length & 1)
        ++length;                                                 /* 9.1.12: padded to an even length */

    memset(at, 0, length);
    at[ISO_RECORD_LENGTH] = (BYTE)length;
    IsoTestBoth32(at + ISO_RECORD_EXTENT, extent);
    IsoTestBoth32(at + ISO_RECORD_DATA_LENGTH, size);
    at[ISO_RECORD_DATE] = 94;                                     /* 1994 */
    at[ISO_RECORD_DATE + 1] = 6;
    at[ISO_RECORD_DATE + 2] = 15;
    at[ISO_RECORD_FLAGS] = flags;
    at[ISO_RECORD_NAME_LENGTH] = (BYTE)nameLength;
    memcpy(at + ISO_RECORD_NAME, name, nameLength);
    return length;
}

static VOID IsoTestBuild(VOID)
{
    PBYTE pvd = g_Image + 16 * ISO_SECTOR_SIZE;
    PBYTE terminator = g_Image + 17 * ISO_SECTOR_SIZE;
    PBYTE root = g_Image + ISO_TEST_ROOT_SECTOR * ISO_SECTOR_SIZE;
    PBYTE data = g_Image + ISO_TEST_DATA_SECTOR * ISO_SECTOR_SIZE;
    DWORD offset = 0;
    DWORD index;

    memset(g_Image, 0, sizeof g_Image);
    pvd[0] = ISO_DESCRIPTOR_PRIMARY;
    memcpy(pvd + 1, "CD001", 5);
    pvd[6] = 1;
    memcpy(pvd + ISO_PVD_VOLUME_ID, "TESTDISC                        ", ISO_VOLUME_ID_SIZE);
    IsoTestBoth32(pvd + ISO_PVD_VOLUME_SPACE, ISO_TEST_SECTORS);
    pvd[ISO_PVD_BLOCK_SIZE] = 0x00;                               /* 2048, both-byte-order 16-bit */
    pvd[ISO_PVD_BLOCK_SIZE + 1] = 0x08;
    pvd[ISO_PVD_BLOCK_SIZE + 2] = 0x08;
    pvd[ISO_PVD_BLOCK_SIZE + 3] = 0x00;
    IsoTestRecord(pvd + ISO_PVD_ROOT_RECORD, ISO_TEST_ROOT_SECTOR, 2 * ISO_SECTOR_SIZE, ISO_FLAG_DIRECTORY, "\0", 1);
    memset(pvd + ISO_PVD_COPYRIGHT_FILE, ' ', 3 * ISO_FILE_ID_SIZE);
    memcpy(pvd + ISO_PVD_COPYRIGHT_FILE, "COPYRIGH.TXT;1", 14);
    memcpy(pvd + ISO_PVD_ABSTRACT_FILE, "ABSTRACT.TXT;1", 14);
    terminator[0] = ISO_DESCRIPTOR_TERMINATOR;
    memcpy(terminator + 1, "CD001", 5);

    /* The root: ".", "..", DATA, README.TXT -- then PADDING to the sector's end, and
     * BIG.BIN in the second sector (ECMA-119 6.8.1).
     */
    offset += IsoTestRecord(root + offset, ISO_TEST_ROOT_SECTOR, 2 * ISO_SECTOR_SIZE, ISO_FLAG_DIRECTORY, "\0", 1);
    offset += IsoTestRecord(root + offset, ISO_TEST_ROOT_SECTOR, 2 * ISO_SECTOR_SIZE, ISO_FLAG_DIRECTORY, "\1", 1);
    offset += IsoTestRecord(root + offset, ISO_TEST_DATA_SECTOR, ISO_SECTOR_SIZE, ISO_FLAG_DIRECTORY, "DATA", 4);
    offset += IsoTestRecord(root + offset, ISO_TEST_README_SECTOR, 11, 0, "README.TXT;1", 12);
    IsoTestRecord(root + ISO_SECTOR_SIZE, ISO_TEST_BIG_SECTOR, ISO_TEST_BIG_SIZE, ISO_FLAG_HIDDEN, "BIG.BIN;1", 9);

    offset = 0;
    offset += IsoTestRecord(data + offset, ISO_TEST_DATA_SECTOR, ISO_SECTOR_SIZE, ISO_FLAG_DIRECTORY, "\0", 1);
    offset += IsoTestRecord(data + offset, ISO_TEST_ROOT_SECTOR, 2 * ISO_SECTOR_SIZE, ISO_FLAG_DIRECTORY, "\1", 1);
    offset += IsoTestRecord(data + offset, ISO_TEST_DEEP_SECTOR, 4, 0, "DEEP.DAT;1", 10);
    IsoTestRecord(data + offset, ISO_TEST_DEEP_SECTOR, 0, 0, "NOEXT.;1", 8);

    memcpy(g_Image + ISO_TEST_README_SECTOR * ISO_SECTOR_SIZE, "hello world", 11);

    for (index = 0; index < ISO_TEST_BIG_SIZE; ++index)
        g_Image[ISO_TEST_BIG_SECTOR * ISO_SECTOR_SIZE + index] = (BYTE)(index * 7);

    memcpy(g_Image + ISO_TEST_DEEP_SECTOR * ISO_SECTOR_SIZE, "DEEP", 4);
}

static INT IsoTestCollect(PVOID context, PCISO_ENTRY entry)
{
    ISO_TEST_LIST *list = (ISO_TEST_LIST *)context;

    if (list->Count < 8)
        strcpy(list->Names[list->Count++], entry->Name);

    return 0;
}

INT main(VOID)
{
    static ISO_VOLUME volume;
    ISO_ENTRY entry;
    BYTE buffer[ISO_TEST_BIG_SIZE];
    BYTE descriptor[ISO_SECTOR_SIZE];
    CHAR identifier[ISO_FILE_ID_SIZE + 1];
    ISO_TEST_LIST list;
    DWORD index;
    INT isSame = 1;

    IsoTestBuild();
    CHECK(IsoMount(&volume, IsoTestRead, NULL), "mount: the PVD at sector 16 is found");
    CHECK(volume.PrimarySector == 16 && volume.VolumeSectors == ISO_TEST_SECTORS, "mount: PVD sector and volume space size");
    CHECK(volume.Root.IsDirectory && volume.Root.Extent == ISO_TEST_ROOT_SECTOR, "mount: the root directory record");
    { CHAR label[ISO_LABEL_SIZE + 1];
      IsoVolumeLabel(&volume, label);
      CHECK(strcmp(label, "TESTDISC") == 0, "label: the volume id, trailing spaces gone");
      memcpy(volume.Primary + ISO_PVD_VOLUME_ID, "NTVDMEX TEST DISC               ", ISO_VOLUME_ID_SIZE);
      IsoVolumeLabel(&volume, label);
      CHECK(strcmp(label, "NTVDMEX TES") == 0, "label: cut to DOS's 11 characters");
      memcpy(volume.Primary + ISO_PVD_VOLUME_ID, "TESTDISC                        ", ISO_VOLUME_ID_SIZE); }

    CHECK(IsoReadDescriptor(&volume, 0, descriptor) == ISO_DESCRIPTOR_PRIMARY, "descriptor 0 = primary (MSCDEX 1505h DX=0)");
    CHECK(IsoReadDescriptor(&volume, 1, descriptor) == ISO_DESCRIPTOR_TERMINATOR, "descriptor 1 = the terminator");

    memset(&list, 0, sizeof list);
    CHECK(IsoEnumerate(&volume, &volume.Root, IsoTestCollect, &list) == 3, "enumerate root: 3 entries, '.' and '..' skipped");
    CHECK(!strcmp(list.Names[0], "DATA") && !strcmp(list.Names[1], "README.TXT") && !strcmp(list.Names[2], "BIG.BIN"),
          "enumerate root: in order, ';1' gone, and the entry after the padding is found");

    CHECK(IsoLookup(&volume, "readme.txt", &entry) && !entry.IsDirectory && entry.Size == 11, "lookup: case-insensitive, size");
    CHECK(IsoReadFile(&volume, &entry, 0, buffer, sizeof buffer) == 11 && !memcmp(buffer, "hello world", 11), "read: a small file");
    CHECK(IsoReadFile(&volume, &entry, 6, buffer, 100) == 5 && !memcmp(buffer, "world", 5), "read: from an offset, clipped at the end");
    CHECK(IsoReadFile(&volume, &entry, 11, buffer, 1) == 0, "read: at the end -> 0");

    CHECK(IsoLookup(&volume, "\\BIG.BIN", &entry) && entry.IsHidden && entry.Size == ISO_TEST_BIG_SIZE, "lookup: leading '\\', hidden flag");
    CHECK(IsoReadFile(&volume, &entry, 0, buffer, ISO_TEST_BIG_SIZE) == ISO_TEST_BIG_SIZE, "read: across two sectors");

    for (index = 0; index < ISO_TEST_BIG_SIZE; ++index)
        if (buffer[index] != (BYTE)(index * 7))
            isSame = 0;

    CHECK(isSame, "read: ...every byte right");
    CHECK(IsoReadFile(&volume, &entry, 2040, buffer, 16) == 16 && buffer[8] == (BYTE)(2048 * 7), "read: a span that crosses the sector boundary");

    CHECK(IsoLookup(&volume, "DATA/deep.dat", &entry) && entry.Size == 4 && entry.Date[0] == 94, "lookup: a nested file, '/' separator, the date");
    CHECK(IsoLookup(&volume, "DATA\\NOEXT", &entry), "lookup: 'NOEXT.;1' is DOS's 'NOEXT'");
    CHECK(IsoLookup(&volume, "DATA", &entry) && entry.IsDirectory, "lookup: a directory");
    CHECK(IsoLookup(&volume, "", &entry) && entry.Extent == ISO_TEST_ROOT_SECTOR, "lookup: '' is the root");
    CHECK(!IsoLookup(&volume, "MISSING.TXT", &entry), "lookup: absent -> 0");
    CHECK(!IsoLookup(&volume, "README.TXT\\X", &entry), "lookup: a file has no children");
    CHECK(IsoLookup(&volume, "README.TXT", &entry) && entry.RecordLength >= ISO_RECORD_MIN && entry.Record[ISO_RECORD_NAME_LENGTH] == 12,
          "the raw record is kept (MSCDEX 150Fh)");

    IsoFileIdentifier(&volume, ISO_PVD_COPYRIGHT_FILE, identifier);
    CHECK(!strcmp(identifier, "COPYRIGH.TXT;1"), "PVD copyright file id, trailing spaces gone (MSCDEX 1502h)");
    IsoFileIdentifier(&volume, ISO_PVD_BIBLIOGRAPHIC_FILE, identifier);
    CHECK(identifier[0] == 0, "PVD bibliographic file id: blank -> empty");

    g_Image[16 * ISO_SECTOR_SIZE + 1] = 'X';
    CHECK(!IsoMount(&volume, IsoTestRead, NULL), "mount: no 'CD001' -> not ISO 9660");
    IsoTestBuild();
    g_FailReads = 1;
    CHECK(!IsoMount(&volume, IsoTestRead, NULL), "mount: an unreadable image -> 0");
    g_FailReads = 0;

    printf("\n== %d checks, %d failed\n", g_Checks, g_Failures);
    return g_Failures ? 1 : 0;
}
