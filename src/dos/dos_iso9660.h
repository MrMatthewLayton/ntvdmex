/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * A read-only ISO 9660 reader: the CD-ROM image behind the MSCDEX drive.  GH #241.
 *
 * Settings > Drives > CD-ROM can name an .ISO file instead of the host's own CD drive.
 * This module is what reads it: the volume descriptors, the directory hierarchy and
 * the files, by ECMA-119 (= ISO 9660:1988). It sees the image only through a sector
 * callback, so tests/unit/iso9660_test.c builds images in memory and reads them back.
 *
 * What is read, and where it is defined (ECMA-119 2nd edition, 1987):
 *   8.1   volume descriptors start at logical sector 16, 2048 bytes each, type byte at 0
 *         and the standard identifier "CD001" at 1; type 1 = Primary, FFh = terminator.
 *   8.4   the Primary Volume Descriptor: volume id at 40 (32 bytes), volume space size
 *         at 80 (both-byte-order 32-bit), logical block size at 128 (both-byte-order
 *         16-bit), the root directory record at 156 (34 bytes), and the copyright,
 *         abstract and bibliographic file identifiers at 702, 739, 776 (37 bytes each).
 *   9.1   a directory record: length at 0, extent location at 2 and data length at 10
 *         (both-byte-order 32-bit, little-endian half first), recording date at 18
 *         (7 bytes), file flags at 25 (bit 0 hidden, bit 1 directory), identifier
 *         length at 32 and the identifier at 33. Identifier 00h is the directory
 *         itself, 01h its parent.
 *   6.8.1 a record never crosses a logical sector: a 0 length byte means "the rest of
 *         this sector is padding".
 *   7.5   a file identifier is "NAME.EXT;VERSION"; DOS sees NAME.EXT. Level 1 keeps it
 *         8.3, level 2 allows up to 30 characters.
 *
 * Not read: Joliet (a Supplementary Volume Descriptor with UCS-2 names -- DOS cannot use
 * long names anyway, and the primary hierarchy is always present), Rock Ridge,
 * multi-extent files (a file split across records, 9.1.6 flag bit 7) and interleaving.
 * A file recorded that way reads only its first extent.
 *
 * No Windows calls, only Windows types (src/ntvdmex_types.h).
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#ifndef NTVDMEX_DOS_ISO9660_H
#define NTVDMEX_DOS_ISO9660_H

#include "../ntvdmex_types.h"

#define ISO_SECTOR_SIZE                 2048u
#define ISO_FIRST_DESCRIPTOR_SECTOR     16u
#define ISO_DESCRIPTOR_LIMIT            32u     /* Descriptors scanned before giving up */
#define ISO_DESCRIPTOR_PRIMARY          1
#define ISO_DESCRIPTOR_SUPPLEMENTARY    2
#define ISO_DESCRIPTOR_TERMINATOR       0xFF
#define ISO_NAME_SIZE                   64      /* A level 2 identifier, its ";1" stripped, with room */
#define ISO_RECORD_MAX                  255     /* A directory record's length is one byte */
#define ISO_FILE_ID_SIZE                37      /* Copyright / abstract / bibliographic file ids */
#define ISO_VOLUME_ID_SIZE              32

/* Where a Primary Volume Descriptor keeps its fields (ECMA-119 8.4). */
#define ISO_PVD_STANDARD_ID             1
#define ISO_PVD_VOLUME_ID               40
#define ISO_PVD_VOLUME_SPACE            80
#define ISO_PVD_BLOCK_SIZE              128
#define ISO_PVD_ROOT_RECORD             156
#define ISO_PVD_COPYRIGHT_FILE          702
#define ISO_PVD_ABSTRACT_FILE           739
#define ISO_PVD_BIBLIOGRAPHIC_FILE      776

/* A directory record's fields (ECMA-119 9.1). */
#define ISO_RECORD_LENGTH               0
#define ISO_RECORD_EXTENT               2
#define ISO_RECORD_DATA_LENGTH          10
#define ISO_RECORD_DATE                 18
#define ISO_RECORD_FLAGS                25
#define ISO_RECORD_NAME_LENGTH          32
#define ISO_RECORD_NAME                 33
#define ISO_RECORD_MIN                  34      /* The fixed part plus a one-byte identifier */

#define ISO_FLAG_HIDDEN                 0x01
#define ISO_FLAG_DIRECTORY              0x02

/* The volume identifier (ECMA-119 8.4.5) as DOS's 11-byte volume label: cut to 11, trailing
 * spaces gone (DIR's "Volume in drive E is ..."). `out` holds ISO_LABEL_SIZE + 1 bytes.
 */
#define ISO_LABEL_SIZE                  11

/* Read `count` 2048-byte sectors from `sector` into `buffer`; nonzero = all read. */
typedef INT (*PISO_READ)(PVOID context, DWORD sector, DWORD count, PBYTE buffer);

typedef struct _ISO_ENTRY
{
    CHAR  Name[ISO_NAME_SIZE];      /* As DOS sees it: version and a lone trailing '.' gone */
    DWORD Extent;                   /* First logical sector */
    DWORD Size;                     /* Bytes */
    BYTE  IsDirectory;
    BYTE  IsHidden;
    BYTE  Date[7];                  /* ECMA-119 9.1.5: years since 1900, month, day, h, m, s, GMT offset */
    BYTE  RecordLength;
    BYTE  Record[ISO_RECORD_MAX];   /* The record as recorded (MSCDEX 150Fh copies it out) */
} ISO_ENTRY, *PISO_ENTRY;

typedef const ISO_ENTRY *PCISO_ENTRY;

typedef struct _ISO_VOLUME
{
    PISO_READ Read;
    PVOID     Context;
    DWORD     VolumeSectors;        /* PVD volume space size */
    DWORD     PrimarySector;        /* Where the PVD was found */
    ISO_ENTRY Root;
    BYTE      Primary[ISO_SECTOR_SIZE];
} ISO_VOLUME, *PISO_VOLUME;

typedef const ISO_VOLUME *PCISO_VOLUME;

/* Called once per entry by IsoEnumerate (never for "." and ".."); nonzero = stop. */
typedef INT (*PISO_VISIT)(PVOID context, PCISO_ENTRY entry);

/* Find the Primary Volume Descriptor and the root directory. Nonzero = a volume. */
INT IsoMount(_Out_ PISO_VOLUME volume, _In_ PISO_READ read, _In_opt_ PVOID context);

/* Volume descriptor `index` (0 = sector 16), raw, into `buffer`; returns its type byte, or
 * -1 if it cannot be read. MSCDEX 1505h hands these out.
 */
INT IsoReadDescriptor(
    _In_ PCISO_VOLUME volume,
    _In_ DWORD index,
    _Out_writes_bytes_(ISO_SECTOR_SIZE) PBYTE buffer);

/* `path` relative to the root, '\' or '/' separated, case-insensitive; "" or "\" is the
 * root itself. Nonzero = found.
 */
INT IsoLookup(_In_ PCISO_VOLUME volume, _In_ PCSTR path, _Out_ PISO_ENTRY entry);

/* Every entry of directory `directory`, in recorded order. Returns the count visited. */
INT IsoEnumerate(
    _In_ PCISO_VOLUME volume,
    _In_ PCISO_ENTRY directory,
    _In_ PISO_VISIT visit,
    _In_opt_ PVOID context);

/* Up to `count` bytes of `file` from `offset`; returns the bytes read (0 at the end). */
DWORD IsoReadFile(
    _In_ PCISO_VOLUME volume,
    _In_ PCISO_ENTRY file,
    _In_ DWORD offset,
    _Out_writes_bytes_(count) PBYTE buffer,
    _In_ DWORD count);
VOID IsoVolumeLabel(_In_ PCISO_VOLUME volume, _Out_writes_(ISO_LABEL_SIZE + 1) PSTR out);

/* One of the PVD's 37-byte file identifiers (ISO_PVD_COPYRIGHT_FILE ...), NUL-terminated
 * and trailing spaces removed, into `out` (ISO_FILE_ID_SIZE + 1 bytes).
 */
VOID IsoFileIdentifier(
    _In_ PCISO_VOLUME volume,
    _In_ DWORD offset,
    _Out_writes_(ISO_FILE_ID_SIZE + 1) PSTR out);

/* Fill `entry` from a raw directory record; nonzero = a well-formed record. */
INT IsoEntryFromRecord(
    _In_reads_bytes_(length) PCBYTE record,
    _In_ DWORD length,
    _Out_ PISO_ENTRY entry);

#endif
