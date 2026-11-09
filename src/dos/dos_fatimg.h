/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * A FAT12/FAT16 disk image, read as a filesystem (#240).
 *
 * WHY THIS EXISTS. Settings > Drives > Floppy can mount an image file as drive A:. Until
 * #240 only the programs that read the disk SECTOR BY SECTOR (INT 13h, INT 25h) saw it;
 * a program that opened A:\SETUP.EXE through DOS reached the host's own A: instead. The
 * DOS layer serves files through Win32, so the image's files are read out of it here and
 * handed to Win32 as an ordinary (read-only) directory -- see dos_int21.c, DosImage*.
 *
 * Pure: no Win32, no host state. The caller owns the bytes of the image; nothing here
 * keeps a pointer past the call that was given it, except the FAT_IMAGE it fills.
 *
 * Spec: Microsoft, "FAT: General Overview of On-Disk Format" (the FAT32 File System
 * Specification, version 1.03, 2000) -- the BPB at offset 11 of sector 0, the FAT12
 * 12-bit packing ("FATOffset = N + (N / 2)"), FAT type by COUNT OF CLUSTERS (< 4085 is
 * FAT12, < 65525 FAT16), the 32-byte directory entry (name 0, attributes 11, time 22,
 * date 24, first cluster 26, size 28), 0xE5 = deleted, 0x00 = end of directory, 0x05 =
 * a name whose first byte is really 0xE5, and attribute 0x0F = a long-name entry. Not
 * held in docs/ref/.
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#ifndef NTVDMEX_DOS_FATIMG_H
#define NTVDMEX_DOS_FATIMG_H

#include "../ntvdmex_types.h"

#define FAT_IMAGE_SECTOR_SIZE       512
#define FAT_IMAGE_ENTRY_SIZE        32
#define FAT_IMAGE_RAW_NAME_SIZE     11      /* "NAME EXT", space-padded */
#define FAT_IMAGE_NAME_SIZE         13      /* "NAME.EXT" and its NUL */
#define FAT_IMAGE_PATH_SIZE         260

#define FAT_ATTRIBUTE_READ_ONLY     0x01
#define FAT_ATTRIBUTE_HIDDEN        0x02
#define FAT_ATTRIBUTE_SYSTEM        0x04
#define FAT_ATTRIBUTE_VOLUME        0x08
#define FAT_ATTRIBUTE_DIRECTORY     0x10
#define FAT_ATTRIBUTE_ARCHIVE       0x20
#define FAT_ATTRIBUTE_LONG_NAME     0x0F    /* All four low bits: a VFAT long-name entry */

typedef struct _FAT_IMAGE
{
    PCBYTE Data;
    DWORD  Size;
    WORD   BytesPerSector;
    BYTE   SectorsPerCluster;
    BYTE   FatCount;
    BYTE   Media;
    BYTE   IsFat16;
    WORD   ReservedSectors;
    WORD   RootEntries;
    WORD   SectorsPerFat;
    DWORD  TotalSectors;
    DWORD  FatOffset;       /* Bytes from the start of the image */
    DWORD  RootOffset;
    DWORD  DataOffset;
    DWORD  ClusterSize;     /* Bytes */
    DWORD  ClusterCount;    /* Data clusters, numbered 2 .. ClusterCount + 1 */
} FAT_IMAGE, *PFAT_IMAGE;
typedef const FAT_IMAGE *PCFAT_IMAGE;

typedef struct _FAT_ENTRY
{
    CHAR  Name[FAT_IMAGE_NAME_SIZE];            /* "NAME.EXT", no padding */
    BYTE  RawName[FAT_IMAGE_RAW_NAME_SIZE];
    BYTE  Attributes;
    WORD  Time;                                 /* DOS packed time and date, as stored */
    WORD  Date;
    WORD  FirstCluster;                         /* 0 = empty file, or the root directory */
    DWORD Size;
} FAT_ENTRY, *PFAT_ENTRY;
typedef const FAT_ENTRY *PCFAT_ENTRY;

/* Read the BPB of `data` and lay the image out. 1 if it is a FAT12/FAT16 volume this
 * reader can serve (512-byte sectors, the layout inside `size`), else 0.
 */
INT FatImageMount(_Out_ PFAT_IMAGE image, _In_ PCBYTE data, _In_ DWORD size);

/* The FAT entry for `cluster`: the next cluster, or >= 0xFF8 (FAT12) / 0xFFF8 (FAT16)
 * at the end of the chain. A value outside the data area is returned as it is.
 */
DWORD FatImageNextCluster(_In_ PCFAT_IMAGE image, _In_ DWORD cluster);

/* 1 if `cluster` is a data cluster (2 .. ClusterCount + 1). */
INT FatImageIsDataCluster(_In_ PCFAT_IMAGE image, _In_ DWORD cluster);

/* The entries of a directory -- the root if `firstCluster` is 0 -- in their on-disk
 * order, without deleted, long-name and volume-label entries, "." or "..". Up to
 * `capacity` go into `entries`; the number found (which may be larger) is returned.
 * Stops at the end-of-directory mark. A chain that loops is cut at the cluster count.
 */
INT FatImageListDirectory(
    _In_ PCFAT_IMAGE image,
    _In_ WORD firstCluster,
    _Out_writes_opt_(capacity) PFAT_ENTRY entries,
    _In_ INT capacity);

/* Find `path` ("SUB\\FILE.TXT", '\\' or '/' between components, any case, no drive and
 * no leading separator) from the root. An empty path is the root itself (a directory
 * entry with FirstCluster 0). 1 and `*entry` on success, else 0.
 */
INT FatImageLookup(_In_ PCFAT_IMAGE image, _In_ PCSTR path, _Out_ PFAT_ENTRY entry);

/* Up to `count` bytes of a file from `offset`, following its chain; never past `Size`.
 * Returns the number of bytes copied (0 at or past the end, or for a broken chain).
 */
DWORD FatImageReadFile(
    _In_ PCFAT_IMAGE image,
    _In_ PCFAT_ENTRY entry,
    _In_ DWORD offset,
    _Out_writes_(count) PBYTE buffer,
    _In_ DWORD count);

/* Free data clusters (FAT entry 0). */
DWORD FatImageFreeClusters(_In_ PCFAT_IMAGE image);

/* The volume label from the root directory, "NAME.EXT"-free ("MYDISK"), trailing spaces
 * dropped. 1 if there is one, else 0 and an empty string.
 */
INT FatImageVolumeLabel(_In_ PCFAT_IMAGE image, _Out_writes_(FAT_IMAGE_NAME_SIZE) PSTR label);

/* A DOS path on the image drive, resolved against its current directory: `current` is
 * "SUB\\DIR" or "" (the root; no leading or trailing separator), `path` what the guest
 * passed after the drive letter -- "\\X\\Y" from the root, "X\\Y" from `current`. "." is
 * dropped and ".." goes up, stopping at the root as DOS's own resolver does. The result
 * has the same form as `current`, upper-cased ("" = the root). 0 if it does not fit.
 */
INT FatImageResolvePath(
    _In_ PCSTR current,
    _In_ PCSTR path,
    _Out_writes_(capacity) PSTR resolved,
    _In_ INT capacity);

#endif
