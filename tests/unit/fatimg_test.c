/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * fatimg_test.c -- the FAT12/FAT16 image reader (src/dos/dos_fatimg.c, #240).
 *
 * Every image here is BUILT, byte by byte, from the spec's layout -- a 1.44 MB FAT12
 * floppy (512-byte sectors, 1 sector per cluster, 1 reserved, 2 FATs of 9 sectors, 224
 * root entries, media F0h) -- so each check knows exactly which cluster holds what. The
 * files are deliberately awkward: a fragmented chain that runs BACKWARDS through the
 * FAT, a subdirectory spanning two clusters, a deleted entry, a long-name entry, a
 * volume label, an empty file, a chain that loops, and a chain shorter than its size.
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#include <stdio.h>
#include <string.h>
#include "dos_fatimg.h"
#define CHECK(condition,message) do{ g_Total++; if(condition){printf("  PASS  %s\n",(message));} \
    else{printf("  FAIL  %s\n",(message)); g_Failures++;} }while(0)

#define TEST_SECTORS        2880
#define TEST_IMAGE_SIZE     (TEST_SECTORS * 512)
#define TEST_FAT_SECTORS    9
#define TEST_ROOT_ENTRIES   224
#define TEST_FAT_OFFSET     512
#define TEST_ROOT_OFFSET    (TEST_FAT_OFFSET + 2 * TEST_FAT_SECTORS * 512)
#define TEST_DATA_OFFSET    (TEST_ROOT_OFFSET + TEST_ROOT_ENTRIES * 32)

static INT g_Total = 0;
static INT g_Failures = 0;

static BYTE g_Image[TEST_IMAGE_SIZE];

static VOID TestPutWord(PBYTE bytes, WORD value)
{
    bytes[0] = (BYTE)value;
    bytes[1] = (BYTE)(value >> 8);
}

static VOID TestPutDword(PBYTE bytes, DWORD value)
{
    TestPutWord(bytes, (WORD)value);
    TestPutWord(bytes + 2, (WORD)(value >> 16));
}

/* FAT12: entry N at byte N + N/2, low or high 12 bits. Both copies of the FAT. */
static VOID TestSetFat(DWORD cluster, WORD value)
{
    INT copy;

    for (copy = 0; copy < 2; ++copy)
    {
        PBYTE fat = g_Image + TEST_FAT_OFFSET + copy * TEST_FAT_SECTORS * 512;
        DWORD offset = cluster + cluster / 2;
        WORD packed = (WORD)(fat[offset] | (fat[offset + 1] << 8));

        if (cluster & 1)
            packed = (WORD)((packed & 0x000F) | (value << 4));
        else
            packed = (WORD)((packed & 0xF000) | (value & 0x0FFF));

        TestPutWord(fat + offset, packed);
    }
}

static PBYTE TestCluster(DWORD cluster)
{
    return g_Image + TEST_DATA_OFFSET + (cluster - 2) * 512;
}

static VOID TestEntry(PBYTE slot, const char *raw11, BYTE attributes, WORD firstCluster, DWORD size)
{
    memcpy(slot, raw11, 11);
    slot[11] = attributes;
    TestPutWord(slot + 22, 0x6000);                 /* 12:00:00 */
    TestPutWord(slot + 24, 0x2C21);                 /* 2002-01-01 */
    TestPutWord(slot + 26, firstCluster);
    TestPutDword(slot + 28, size);
}

static VOID TestBuild(VOID)
{
    PBYTE root = g_Image + TEST_ROOT_OFFSET;
    INT index;

    memset(g_Image, 0, sizeof g_Image);
    g_Image[0] = 0xEB;
    g_Image[1] = 0x3C;
    g_Image[2] = 0x90;
    TestPutWord(g_Image + 11, 512);
    g_Image[13] = 1;
    TestPutWord(g_Image + 14, 1);
    g_Image[16] = 2;
    TestPutWord(g_Image + 17, TEST_ROOT_ENTRIES);
    TestPutWord(g_Image + 19, TEST_SECTORS);
    g_Image[21] = 0xF0;
    TestPutWord(g_Image + 22, TEST_FAT_SECTORS);
    TestPutWord(g_Image + 24, 18);
    TestPutWord(g_Image + 26, 2);
    TestSetFat(0, 0xFF0);
    TestSetFat(1, 0xFFF);

    /* The root: label, long-name entry, deleted entry, then the files. */
    TestEntry(root + 0 * 32, "MYDISK     ", FAT_ATTRIBUTE_VOLUME, 0, 0);
    TestEntry(root + 1 * 32, "A\0B\0C\0D\0E\0F", FAT_ATTRIBUTE_LONG_NAME, 0, 0);
    TestEntry(root + 2 * 32, "\xE5OLD    TXT", FAT_ATTRIBUTE_ARCHIVE, 0, 0);
    TestEntry(root + 3 * 32, "README  TXT", FAT_ATTRIBUTE_ARCHIVE, 2, 11);
    memcpy(TestCluster(2), "hello floppy", 11);
    TestSetFat(2, 0xFFF);

    /* BIG.DAT: 2000 bytes over clusters 10 -> 3 -> 20 -> 7 (fragmented, backwards). */
    TestEntry(root + 4 * 32, "BIG     DAT", FAT_ATTRIBUTE_READ_ONLY | FAT_ATTRIBUTE_HIDDEN, 10, 2000);
    TestSetFat(10, 3);
    TestSetFat(3, 20);
    TestSetFat(20, 7);
    TestSetFat(7, 0xFFF);

    for (index = 0; index < 2000; ++index)
    {
        static const DWORD chain[4] = { 10, 3, 20, 7 };
        TestCluster(chain[index / 512])[index % 512] = (BYTE)(index * 7 + 1);
    }

    /* SUB: a directory over clusters 4 -> 5 (two clusters = 32 entries). */
    TestEntry(root + 5 * 32, "SUB        ", FAT_ATTRIBUTE_DIRECTORY, 4, 0);
    TestSetFat(4, 5);
    TestSetFat(5, 0xFFF);
    TestEntry(TestCluster(4) + 0 * 32, ".          ", FAT_ATTRIBUTE_DIRECTORY, 4, 0);
    TestEntry(TestCluster(4) + 1 * 32, "..         ", FAT_ATTRIBUTE_DIRECTORY, 0, 0);

    for (index = 2; index < 16; ++index)
    {
        char raw[12];
        sprintf(raw, "FILE%02d  TXT", index);
        TestEntry(TestCluster(4) + index * 32, raw, FAT_ATTRIBUTE_ARCHIVE, 0, 0);
    }

    TestEntry(TestCluster(5) + 0 * 32, "INNER   TXT", FAT_ATTRIBUTE_ARCHIVE, 6, 5);
    memcpy(TestCluster(6), "inner", 5);
    TestSetFat(6, 0xFFF);

    /* EMPTY. with no extension; LOOP.BIN whose chain loops; SHORT.BIN whose chain ends early. */
    TestEntry(root + 6 * 32, "EMPTY      ", FAT_ATTRIBUTE_ARCHIVE, 0, 0);
    TestEntry(root + 7 * 32, "LOOP    BIN", FAT_ATTRIBUTE_ARCHIVE, 30, 100000);
    TestSetFat(30, 30);
    TestEntry(root + 8 * 32, "SHORT   BIN", FAT_ATTRIBUTE_ARCHIVE, 31, 1500);
    TestSetFat(31, 0xFFF);
}

int main(void)
{
    FAT_IMAGE image;
    FAT_ENTRY entry;
    FAT_ENTRY list[40];
    BYTE buffer[4096];
    CHAR label[FAT_IMAGE_NAME_SIZE];
    INT count;
    INT index;
    INT isGood;

    TestBuild();
    printf("== mount\n");
    CHECK(FatImageMount(&image, g_Image, sizeof g_Image), "a 1.44 MB FAT12 floppy mounts");
    CHECK(!image.IsFat16 && image.ClusterCount == 2847, "FAT12 by cluster count: 2847 clusters");
    CHECK(image.RootOffset == TEST_ROOT_OFFSET && image.DataOffset == TEST_DATA_OFFSET, "the root and the data area are where the BPB puts them");
    CHECK(image.Media == 0xF0, "media byte F0h");
    CHECK(!FatImageMount(&image, g_Image, 100), "an image shorter than a sector is refused");
    g_Image[11] = 0;
    g_Image[12] = 0;
    CHECK(!FatImageMount(&image, g_Image, sizeof g_Image), "a BPB with 0 bytes per sector is refused");
    g_Image[12] = 4;                                   /* 1024-byte sectors */
    CHECK(!FatImageMount(&image, g_Image, sizeof g_Image), "a sector size other than 512 is refused");
    TestBuild();
    g_Image[13] = 3;
    CHECK(!FatImageMount(&image, g_Image, sizeof g_Image), "a cluster that is not a power of two is refused");
    TestBuild();
    CHECK(!FatImageMount(&image, g_Image, TEST_ROOT_OFFSET), "an image cut off before its root directory is refused");
    CHECK(FatImageMount(&image, g_Image, sizeof g_Image), "...and the real one mounts again");

    printf("== FAT12 packing\n");
    CHECK(FatImageNextCluster(&image, 10) == 3 && FatImageNextCluster(&image, 3) == 20, "even and odd entries unpack (10->3, 3->20)");
    CHECK(FatImageNextCluster(&image, 7) >= 0xFF8, "end of chain");
    CHECK(FatImageNextCluster(&image, 100) == 0, "an unused cluster reads 0");

    printf("== directories\n");
    count = FatImageListDirectory(&image, 0, list, 40);
    CHECK(count == 6, "the root lists 6 entries: no label, long name or deleted entry");
    CHECK(!strcmp(list[0].Name, "README.TXT") && !strcmp(list[1].Name, "BIG.DAT") && !strcmp(list[2].Name, "SUB"),
          "...in on-disk order, with 8.3 names put back together");
    CHECK(!strcmp(list[3].Name, "EMPTY"), "a name with no extension has no dot");
    CHECK(list[1].Attributes == (FAT_ATTRIBUTE_READ_ONLY | FAT_ATTRIBUTE_HIDDEN) && list[1].Size == 2000 && list[1].FirstCluster == 10,
          "attributes, size and first cluster");
    CHECK(list[0].Time == 0x6000 && list[0].Date == 0x2C21, "the packed time and date, as stored");
    count = FatImageListDirectory(&image, 4, list, 40);
    CHECK(count == 15, "SUB lists 15: 14 in its first cluster after . and .., 1 in the second");
    CHECK(!strcmp(list[14].Name, "INNER.TXT"), "...the last one reached through the cluster chain");
    CHECK(FatImageListDirectory(&image, 4, NULL, 0) == 15, "the count alone, with no buffer");
    CHECK(FatImageVolumeLabel(&image, label) && !strcmp(label, "MYDISK"), "the volume label");

    printf("== lookup\n");
    CHECK(FatImageLookup(&image, "", &entry) && (entry.Attributes & FAT_ATTRIBUTE_DIRECTORY) && entry.FirstCluster == 0, "the empty path is the root");
    CHECK(FatImageLookup(&image, "readme.txt", &entry) && entry.Size == 11, "a root file, any case");
    CHECK(FatImageLookup(&image, "sub\\inner.TXT", &entry) && entry.FirstCluster == 6, "a file in a subdirectory");
    CHECK(FatImageLookup(&image, "SUB/INNER.TXT", &entry), "'/' separates components too");
    CHECK(FatImageLookup(&image, "SUB\\", &entry) && (entry.Attributes & FAT_ATTRIBUTE_DIRECTORY), "a directory with a trailing separator");
    CHECK(!FatImageLookup(&image, "NOPE.TXT", &entry), "a missing file");
    CHECK(!FatImageLookup(&image, "README.TXT\\X", &entry), "a file used as a directory");
    CHECK(!FatImageLookup(&image, "OLD.TXT", &entry) && !FatImageLookup(&image, "MYDISK", &entry), "deleted entries and the label are not files");
    CHECK(!FatImageLookup(&image, "README.TX", &entry) && !FatImageLookup(&image, "README.TXTX", &entry), "a name must match whole");

    printf("== reading\n");
    FatImageLookup(&image, "README.TXT", &entry);
    CHECK(FatImageReadFile(&image, &entry, 0, buffer, sizeof buffer) == 11 && !memcmp(buffer, "hello flopp", 11), "a one-cluster file, cut at its size");
    FatImageLookup(&image, "BIG.DAT", &entry);
    memset(buffer, 0, sizeof buffer);
    CHECK(FatImageReadFile(&image, &entry, 0, buffer, sizeof buffer) == 2000, "a fragmented file reads its whole size");
    isGood = 1;

    for (index = 0; index < 2000; ++index)
        if (buffer[index] != (BYTE)(index * 7 + 1))
            isGood = 0;

    CHECK(isGood, "...every byte from the right cluster, in chain order");
    CHECK(FatImageReadFile(&image, &entry, 1000, buffer, 100) == 100 && buffer[0] == (BYTE)(1000 * 7 + 1) && buffer[99] == (BYTE)(1099 * 7 + 1),
          "a read from the middle, across a cluster boundary");
    CHECK(FatImageReadFile(&image, &entry, 1990, buffer, 100) == 10, "a read past the end stops at the size");
    CHECK(FatImageReadFile(&image, &entry, 2000, buffer, 100) == 0, "a read at the end returns nothing");
    FatImageLookup(&image, "EMPTY", &entry);
    CHECK(FatImageReadFile(&image, &entry, 0, buffer, 100) == 0, "an empty file reads nothing");
    FatImageLookup(&image, "LOOP.BIN", &entry);
    CHECK(FatImageReadFile(&image, &entry, 0, buffer, sizeof buffer) <= sizeof buffer, "a looping chain ends (bounded by the cluster count)");
    FatImageLookup(&image, "SHORT.BIN", &entry);
    CHECK(FatImageReadFile(&image, &entry, 0, buffer, sizeof buffer) == 512, "a chain shorter than the size stops where the chain does");

    printf("== free space\n");
    CHECK(FatImageFreeClusters(&image) == 2847 - 10, "free clusters = all but the 10 in use (2-7, 10, 20, 30, 31)");

    printf("== FAT16\n");
    {
        /* A 16 MB FAT16 volume: 4 sectors per cluster, 1 FAT of 32 sectors, 512 root entries. */
        static BYTE big[16 * 1024 * 1024];
        FAT_IMAGE image16;
        memset(big, 0, sizeof big);
        TestPutWord(big + 11, 512);
        big[13] = 4;
        TestPutWord(big + 14, 1);
        big[16] = 1;
        TestPutWord(big + 17, 512);
        TestPutWord(big + 19, 0);
        TestPutDword(big + 32, (DWORD)(sizeof big / 512));
        big[21] = 0xF8;
        TestPutWord(big + 22, 32);
        TestPutWord(big + 512 + 2 * 2, 0xFFFF);        /* cluster 2: end of chain */
        CHECK(FatImageMount(&image16, big, sizeof big) && image16.IsFat16, "a FAT16 volume (8000+ clusters) mounts as FAT16");
        CHECK(FatImageNextCluster(&image16, 2) == 0xFFFF, "16-bit FAT entries");
    }

    printf("== DOS paths on the image drive\n");
    {
        CHAR resolved[64];
        CHAR tiny[6];
        CHECK(FatImageResolvePath("", "readme.txt", resolved, sizeof resolved) && !strcmp(resolved, "README.TXT"), "relative to the root, upper-cased");
        CHECK(FatImageResolvePath("SUB", "inner.txt", resolved, sizeof resolved) && !strcmp(resolved, "SUB\\INNER.TXT"), "relative to the current directory");
        CHECK(FatImageResolvePath("SUB", "\\x\\y", resolved, sizeof resolved) && !strcmp(resolved, "X\\Y"), "a leading backslash starts from the root");
        CHECK(FatImageResolvePath("SUB\\DEEP", "..\\F", resolved, sizeof resolved) && !strcmp(resolved, "SUB\\F"), "'..' goes up one");
        CHECK(FatImageResolvePath("SUB", "..\\..\\..\\F", resolved, sizeof resolved) && !strcmp(resolved, "F"), "'..' stops at the root");
        CHECK(FatImageResolvePath("SUB", ".\\.\\F", resolved, sizeof resolved) && !strcmp(resolved, "SUB\\F"), "'.' is dropped");
        CHECK(FatImageResolvePath("", "\\", resolved, sizeof resolved) && !strcmp(resolved, ""), "a backslash alone is the root");
        CHECK(FatImageResolvePath("SUB", "", resolved, sizeof resolved) && !strcmp(resolved, "SUB"), "an empty path is the current directory");
        CHECK(FatImageResolvePath("", "a//b\\\\c/", resolved, sizeof resolved) && !strcmp(resolved, "A\\B\\C"), "repeated and trailing separators collapse");
        CHECK(FatImageResolvePath("", "*.TXT", resolved, sizeof resolved) && !strcmp(resolved, "*.TXT"), "a wildcard is passed through for FindFirst");
        CHECK(!FatImageResolvePath("", "LONGNAME.TXT", tiny, sizeof tiny), "too long for the buffer: refused, not cut");
    }

    printf("\n%d checks, %d failed\n", g_Total, g_Failures);
    return g_Failures ? 1 : 0;
}
