/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * INT 21h AH=52h's List of Lists, built to the MEASURED layout.
 *
 * GH #48. AH=52h returns ES:BX pointing INTO this structure; the word at ES:BX-2
 * is the first MCB segment and everything from ES:BX on is DOS's own bookkeeping.
 *
 * EVERY OFFSET BELOW CAME OFF THE ORACLE, NOT OUT OF A BOOK:
 * #48 is explicit that these structures "must not be written from documentation
 * alone -- the layout dumps have twice caught errors that a plausible reading
 * would have missed". So tests/probes/dos/p_sysvar.asm dumped them from genuine
 * MS-DOS 6.22 and this is that dump decoded:
 *
 * BUF=sysvars.raw 5302 6A131601 CC001601 59007000 23007000 0002 6D001601
 *                 0000 5003 0000 1E03 0000 03 05 <NUL header> ...
 *
 *   ES:BX-2  0253        first MCB segment
 *   +0x00    0116:136A   DPB chain
 *   +0x04    0116:00CC   SFT chain
 *   +0x08    0070:0059   CLOCK$ device
 *   +0x0C    0070:0023   CON device
 *   +0x10    0x0200      max bytes per sector (512)
 *   +0x12    0116:006D   disk buffer chain
 *   +0x16    0350:0000   CDS array
 *   +0x1A    031E:0000   FCB table
 *   +0x1E    0x0000      FCB keep count
 *   +0x20    3           number of BLOCK devices
 *   +0x21    5           LASTDRIVE
 *   +0x22    the NUL device header, INLINE -- not a pointer to one, and 18
 *            bytes long: next 0255:0000, attr 8004, strat 0DC6, intr 0DCC,
 *            name 'NUL     '
 *
 * BUF=sysvars.dpb0 000000020000010002E0002100200B090013006B007000F0008B131601...
 *   a 33-byte DOS 4+ DPB for A:, and the next one begins EXACTLY 33 bytes
 *   later at 0116:138B -- which the chain pointer at +0x19 confirms (0x136A +
 *   0x21 = 0x138B). That arithmetic is why 33 is a measurement here and not a
 *   recollection.
 *
 * BUF=sysvars.cds0 413A5C 00... 0040 6A131601 0000 FFFFFFFF 0200 ... 423A5C
 *   'A:\' at +0x00, then the next entry's 'B:\' at offset 88 -- so a CDS entry
 *   is 0x58 bytes, the flags word at +0x43 is 0x4000, and the far pointer at
 *   +0x45 is 0116:136A, i.e. THE FIRST DPB. An entry points at its own drive's
 *   DPB, which is the link a memory/disk walker follows.
 *
 * Pure -- byte buffers and integers only -- so tests/unit/sysvars_test.c can
 * pin every offset against those same dumps.
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#ifndef NTVDMEX_DOS_SYSVARS_H
#define NTVDMEX_DOS_SYSVARS_H

#include "../ntvdmex_types.h"
#include "dos_layout.h"

/* SysVars field offsets, relative to what AH=52h returns in ES:BX. */
#define DOS_SYSVARS_MCB_HEAD                (-2)
#define DOS_SYSVARS_DPB                     0x00
#define DOS_SYSVARS_SFT                     0x04
#define DOS_SYSVARS_CLOCK                   0x08
#define DOS_SYSVARS_CON                     0x0C
#define DOS_SYSVARS_MAX_SECTOR              0x10
#define DOS_SYSVARS_BUFFERS                 0x12
#define DOS_SYSVARS_CDS                     0x16
#define DOS_SYSVARS_FCB                     0x1A
#define DOS_SYSVARS_FCB_KEEP                0x1E
#define DOS_SYSVARS_BLOCK_DEVICES           0x20
#define DOS_SYSVARS_LASTDRIVE               0x21
#define DOS_SYSVARS_NUL                     0x22    /* The NUL device header, INLINE */
#define DOS_SYSVARS_NUL_LEN                 0x12    /* 18 bytes, measured */
#define DOS_SYSVARS_NUL_END                 (DOS_SYSVARS_NUL + DOS_SYSVARS_NUL_LEN) /* 0x34 */
#define DOS_SYSVARS_EXTENDED_KB             0x45    /* WORD: extended memory in KB (GH #47) */
#define DOS_SYSVARS_FIRST_MCB_COPY          0x68    /* The first MCB again, as 6.22 and PCem */
#define DOS_SYSVARS_WOW_TABLE               0x6A    /* krnl386's table of DOS variables */

/* DPB (DOS 4.0+), 33 bytes. Offsets measured from sysvars.dpb0. */
#define DOS_DPB_DRIVE                       0x00
#define DOS_DPB_UNIT                        0x01
#define DOS_DPB_SECTOR_SIZE                 0x02
#define DOS_DPB_CLUSTER_MAX                 0x04    /* Sectors per cluster MINUS ONE */
#define DOS_DPB_CLUSTER_SHIFT               0x05
#define DOS_DPB_RESERVED                    0x06
#define DOS_DPB_FAT_COUNT                   0x08
#define DOS_DPB_ROOT_ENTRIES                0x09
#define DOS_DPB_DATA_START                  0x0B
#define DOS_DPB_HIGHEST_CLUSTER             0x0D    /* Highest cluster number */
#define DOS_DPB_FAT_SECTORS                 0x0F    /* WORD on DOS 4+, was a byte before */
#define DOS_DPB_ROOT_START                  0x11
#define DOS_DPB_DEVICE_HEADER               0x13    /* Far pointer to the device header */
#define DOS_DPB_MEDIA                       0x17
#define DOS_DPB_ACCESSED                    0x18
#define DOS_DPB_NEXT                        0x19    /* Far pointer to the next DPB */
#define DOS_DPB_FREE_SEARCH                 0x1D
#define DOS_DPB_FREE_COUNT                  0x1F    /* FFFF = unknown, which is what 6.22 had */
#define DOS_DPB_LEN                         0x21    /* 33 -- confirmed by 0x136A + 0x21 = 0x138B */

/* CDS (DOS 4.0+), 88 bytes. Offsets measured from sysvars.cds0. */
#define DOS_CDS_PATH                        0x00    /* 67 bytes ASCIZ, e.g. "A:\" */
#define DOS_CDS_FLAGS                       0x43
#define DOS_CDS_DPB                         0x45    /* Far pointer to this drive's DPB */
#define DOS_CDS_CLUSTER                     0x49
#define DOS_CDS_UNKNOWN                     0x4B    /* 6.22 leaves FFFFFFFF here */
#define DOS_CDS_SLASH                       0x4F    /* offset of the root backslash: 2 */
#define DOS_CDS_LEN                         0x58    /* 88 -- 'B:\' begins exactly here */

/* Measured flag: bit 14 set = the drive is PHYSICAL/local. 6.22 had 0x4000 on
 * A:, and a drive letter with nothing behind it gets 0.
 */
#define DOS_CDS_FLAG_PHYSICAL               0x4000

/* Bit 15 = the drive is served by a redirector (network, and MSCDEX's CD-ROMs
 * look the same to DOS). Such entries carry no DPB: there is no FAT to walk.
 */
#define DOS_CDS_FLAG_NETWORK                0x8000

/* The values the builders below write. */
#define DOS_CHAIN_END                       0xFFFF  /* FFFF:FFFF ends a DPB or device chain */
#define DOS_DPB_DEFAULT_SECTOR_SIZE         512     /* What a sector size of 0 is taken as */
#define DOS_CDS_ARRAY_NONE                  0xFFFF  /* SysVars' CDS pointer when there is no array */
#define DOS_DPB_CLUSTER_LIMIT               0xFFFE  /* The highest cluster a WORD field can say */
#define DOS_MEDIA_FIXED                     0xF8
#define DOS_MEDIA_FLOPPY_144                0xF0
#define DOS_FIXED_ROOT_ENTRIES              512
#define DOS_FLOPPY_144_ROOT_ENTRIES         224
#define DOS_FLOPPY_144_CLUSTERS             2847    /* 6.22's own 1.44M DPB: one sector a cluster */
#define DOS_UNMEASURED_SECTORS_PER_CLUSTER  8       /* A fixed drive GetDiskFreeSpace refused */
#define DOS_UNMEASURED_CLUSTERS             0xFFF0
#define DOS_FAT12_HIGHEST_CLUSTER           0xFF5   /* FAT12 if the highest cluster is at most this */
#define DOS_FAT12_PAIR_BYTES                3       /* FAT12: two entries in three bytes */
#define DOS_FAT12_PAIR_ENTRIES              2
#define DOS_FAT16_ENTRY_BYTES               2
#define DOS_FORMAT_RESERVED_SECTORS         1       /* FORMAT's fixed choices: 1 reserved sector ... */
#define DOS_FORMAT_FAT_COUNT                2       /* ... and 2 FATs */
#define DOS_DIRECTORY_ENTRY_BYTES           32
#define DOS_DPB_FREE_COUNT_UNKNOWN          0xFFFF
#define DOS_CDS_NO_DPB                      0xFFFF  /* An unused drive letter: FFFF:FFFF */
#define DOS_CDS_REDIRECTED_DPB              0x0000  /* A redirected drive: 0000:0000 */
#define DOS_CDS_UNKNOWN_FILL                0xFFFF
#define DOS_CDS_ROOT_SLASH_INDEX            2       /* "A:\" -- the backslash is at index 2 */
#define DOS_DEVICE_NAME_LEN                 8

/* #48: THE DEVICE DRIVER CHAIN:
 * NUL (inline in SysVars) used to TERMINATE the chain: "we install no drivers". But
 * DOS's own drivers are not installed, they are IO.SYS, and every DOS has them --
 * MEM /D on 6.22 lists, in chain order (runs/s81_mem/oracle_memd.txt):
 *     CON AUX PRN CLOCK$ "A: - C:" COM1 LPT1 LPT2 LPT3 COM2 COM3 COM4
 * and the order of the first five is pinned by measured pointers, not by that
 * listing alone: SysVars+0x0C = CON at 0070:0023, +0x08 = CLOCK$ at 0070:0059, the
 * 6.22 SFT's AUX/PRN entries name 0070:0035 and 0070:0047 (lolprobe-msdos622.txt),
 * and its floppy DPB names the block driver at 0070:006B -- five 18-byte headers
 * back to back. So NUL now links to the same twelve, in that order, and the last
 * terminates (FFFF:FFFF).
 *
 * [CAUTION]: THE ATTRIBUTE WORDS ARE NOT MEASURED. They are the documented IO.SYS values
 * (RBIL "Format of device driver header", and the bits each device's behaviour
 * implies): CON 8013h (char | fast INT 29h output -- we serve INT 29h -- | stdout |
 * stdin), AUX/COMn 8000h, PRN/LPTn A0C0h (char | output-until-busy | IOCTL query |
 * generic IOCTL), CLOCK$ 8008h, the block driver 08C2h with byte 0 of its name = the
 * unit count. tests/probes/dos/p_devchn.asm reads every one of them back off the oracle.
 *
 * [CAUTION]: AND NONE OF THEM CAN BE CALLED AS A DRIVER. Our devices are served by the INT 21h
 * layer, not by request packets. A program that calls a header's strategy/interrupt
 * pair directly (rare: a few TSRs and diagnostics do) gets status 8103h -- error,
 * done, "unknown command" -- written by the strategy entry, and a bare RETF from the
 * interrupt entry. That is a truthful refusal; an entry of 0000 (what NUL had) is a
 * far call into whatever the segment holds at offset 0.
 */
#define DOS_DEVICE_HEADER_NEXT              0x00
#define DOS_DEVICE_HEADER_ATTRIBUTE         0x04
#define DOS_DEVICE_HEADER_STRATEGY          0x06
#define DOS_DEVICE_HEADER_INTERRUPT         0x08
#define DOS_DEVICE_HEADER_NAME              0x0A
#define DOS_DEVICE_HEADER_LEN               0x12    /* 18, as DOS_SYSVARS_NUL_LEN -- the stride 6.22's are at */
#define DOS_DEVICE_OFFSET(index)            ((UINT)(index) * DOS_DEVICE_HEADER_LEN) /* 0x00 .. 0xC6 */

/* The entry stubs, after the headers (12 x 18 = 0xD8 bytes). Strategy routines are
 * FAR-called with ES:BX = the request header, whose status word is at +3.
 */
#define DOS_DEVICE_STUB_UNKNOWN             0xE0    /* Mov word [es:bx+3],8103h ; retf -- 7 bytes */
#define DOS_DEVICE_STUB_RETF                0xE7    /* Retf -- every interrupt entry */
#define DOS_DEVICE_AREA_LEN                 0xE8
#define DOS_DEVICE_ATTRIBUTE_CON            0x8013
#define DOS_DEVICE_ATTRIBUTE_AUX            0x8000
#define DOS_DEVICE_ATTRIBUTE_PRN            0xA0C0
#define DOS_DEVICE_ATTRIBUTE_CLOCK          0x8008
#define DOS_DEVICE_ATTRIBUTE_BLOCK          0x08C2
#define DOS_DEVICE_ATTRIBUTE_NUL            0x8004  /* Measured: 6.22's NUL header, sysvars_test.c */

/* NUL's own two entries live in ITS segment (a header's strategy/interrupt are
 * offsets in the header's own segment, and NUL's is SysVars'): `mov word [es:bx+3],
 * 0100h ; retf` then `retf`, 8 bytes at DOS_SYSVARS_SEG:DOS_NULSTUB_OFF. NUL accepts
 * every request and does nothing, which is what "done, no error" says.
 */
#define DOS_NULSTUB_LEN                     8
#define DOS_NULSTUB_STRAT                   0       /* Offsets within the 8 bytes */
#define DOS_NULSTUB_INTR                    7

/* Build the NUL device header, inline at SysVars+0x22. Attribute 0x8004 is 6.22's
 * (bit 15 = character device, bit 2 = NUL). `next` links on to CON (#48; it
 * TERMINATED here, FFFF:FFFF, while no other header existed -- see above).
 */
#define DOS_NUL_NAME                        "NUL     "

enum { DOS_DEVICE_CON, DOS_DEVICE_AUX, DOS_DEVICE_PRN, DOS_DEVICE_CLOCK, DOS_DEVICE_BLOCK,
       DOS_DEVICE_COM1, DOS_DEVICE_LPT1, DOS_DEVICE_LPT2, DOS_DEVICE_LPT3, DOS_DEVICE_COM2,
       DOS_DEVICE_COM3, DOS_DEVICE_COM4, DOS_DEVICE_COUNT };

/* static INLINE: dos_int21.c includes this for AH=1Fh/32h's DPB (#48) and uses one
 * builder of the five, and a plain `static` would warn for every unused one.
 */
static inline VOID DosSysVarsWriteWord(_Out_ PBYTE buffer, _In_ UINT offset, _In_ UINT value)
{
    buffer[offset] = (BYTE)(value & BYTE_MASK);
    buffer[offset + 1] = (BYTE)((value >> BYTE_SHIFT) & BYTE_MASK);
}

static inline VOID DosSysVarsWriteFarPointer(
    _Out_ PBYTE buffer,
    _In_ UINT offset,
    _In_ UINT segment,
    _In_ UINT pointerOffset)
{
    DosSysVarsWriteWord(buffer, offset, pointerOffset);
    DosSysVarsWriteWord(buffer, offset + X86_FAR_POINTER_SEGMENT, segment);
}

/* #48: THE FAT LAYOUT A DPB DESCRIBES, DERIVED -- AND CHECKED AGAINST 6.22:
 * A DPB carries four numbers that only a boot sector knows: reserved sectors, FAT
 * count, sectors per FAT and the root directory's size -- and two it derives from
 * them: the root's first sector and the first data sector. This host has no boot
 * sector to read (no raw sectors at all: INT 13h, 25h, 26h report absent, GH #44).
 * These had been left ZERO, on the argument that a plausible number is the MEM.EXE
 * failure in another structure. Zero is not neutral either, though: FATSECS=0 with
 * DATASTART=0 describes a volume whose data area starts at the boot sector, i.e. the
 * FAT, the root and the files all overlapping -- a structure no FORMAT can produce,
 * and a divisor of zero for anything that sizes the FAT from it.
 *
 * So the layout is DERIVED, by FAT's own rules, from what IS measured (bytes per
 * sector, sectors per cluster, highest cluster -- GetDiskFreeSpace) plus the fixed
 * choices FORMAT makes (1 reserved sector, 2 FATs, the root size the caller passes):
 *     FAT12 if clusters < 4085 (highest <= 0xFF5), else FAT16
 *     sectors/FAT = ceil((highest + 1) entries * 1.5 or 2 bytes / bytes per sector)
 *     root start  = reserved + FATs * sectors/FAT
 *     data start  = root start + ceil(root entries * 32 / bytes per sector)
 *
 * [INFO]: THE CHECK THAT THIS IS DOS'S ARITHMETIC AND NOT A GUESS: fed 6.22's own floppy
 * (512 B, 1 sector/cluster, 224 root entries, highest cluster 2848 -- sysvars.dpb0)
 * it reproduces that DPB's FAT=9, root start=19, data start=33 exactly, and
 * tests/unit/sysvars_test.c holds the whole 33-byte build to the oracle's bytes.
 *
 * [CAUTION]: WHAT IT IS NOT: for a FIXED disk the numbers describe a self-consistent FAT16
 * volume of the measured size, NOT the real volume (NTFS, or a FAT whose boot
 * sector we never read). No sector they name can be read here, so a program that
 * follows them to the FAT gets the same INT 25h refusal it got before -- what it no
 * longer gets is a structurally impossible DPB. Unmeasured against a 6.22 HARD disk
 * DPB; tests/probes/dos/p_devchn.asm's `dpb.c.layout` row is the check.
 */
static inline VOID DosDpbFatLayout(
    _In_ UINT bytesPerSector,
    _In_ UINT rootEntries,
    _In_ UINT highestCluster,
    _Out_ PUINT fatSectors,
    _Out_ PUINT rootStart,
    _Out_ PUINT dataStart)
{
    DWORD sectorSize = bytesPerSector ? bytesPerSector : DOS_DPB_DEFAULT_SECTOR_SIZE;
    DWORD entries = (DWORD)highestCluster + 1;          /* clusters 0..highest */
    DWORD fatBytes = (highestCluster <= DOS_FAT12_HIGHEST_CLUSTER) ? (entries * DOS_FAT12_PAIR_BYTES + 1) / DOS_FAT12_PAIR_ENTRIES : entries * DOS_FAT16_ENTRY_BYTES;
    DWORD sectorsPerFat = (fatBytes + sectorSize - 1) / sectorSize;
    DWORD rootSector = DOS_FORMAT_RESERVED_SECTORS + DOS_FORMAT_FAT_COUNT * sectorsPerFat;                                  /* reserved + 2 FATs */
    DWORD dataSector = rootSector + ((DWORD)rootEntries * DOS_DIRECTORY_ENTRY_BYTES + sectorSize - 1) / sectorSize;

    *fatSectors = (UINT)sectorsPerFat;
    *rootStart = (UINT)rootSector;
    *dataStart = (UINT)dataSector;
}

/* Build one DPB. `nextSegment/nextOffset` link it on; pass 0xFFFF/0xFFFF to end the
 * chain -- a chain that does not terminate is how krnl386 once walked the IVT
 * forever (see DOS_SFT_* in dos_layout.h), and a DPB chain can do the same.
 *
 * [CAUTION]: WHICH FIELDS ARE REAL, AND WHICH ARE NOT. Say it here rather than let a caller
 * discover it. GetDiskFreeSpace gives us bytes/sector, sectors/cluster and the cluster count, so
 * DOS_DPB_SECTOR_SIZE, DOS_DPB_CLUSTER_MAX, DOS_DPB_CLUSTER_SHIFT and DOS_DPB_HIGHEST_CLUSTER are
 * MEASURED off the real volume. The rest of the FAT geometry -- DOS_DPB_RESERVED,
 * DOS_DPB_FAT_COUNT, DOS_DPB_ROOT_ENTRIES, DOS_DPB_DATA_START, DOS_DPB_FAT_SECTORS,
 * DOS_DPB_ROOT_START -- is only in the boot sector, which we cannot read, so it is DERIVED by
 * DosDpbFatLayout above (#48; was zero -- see there for why zero was not the honest answer it
 * looked like).
 */
static inline VOID DosDpbBuild(
    _Out_writes_bytes_(DOS_DPB_LEN) PBYTE dpb,
    _In_ UINT drive,
    _In_ UINT bytesPerSector,
    _In_ UINT sectorsPerCluster,
    _In_ UINT rootEntries,
    _In_ UINT highestCluster,
    _In_ UINT media,
    _In_ UINT deviceHeaderSegment,
    _In_ UINT deviceHeaderOffset,
    _In_ UINT nextSegment,
    _In_ UINT nextOffset)
{
    UINT byteIndex;
    UINT shift = 0;
    UINT remaining = sectorsPerCluster;
    UINT fatSectors;
    UINT rootStart;
    UINT dataStart;

    for (byteIndex = 0; byteIndex < DOS_DPB_LEN; ++byteIndex)
        dpb[byteIndex] = 0;

    while (remaining > 1)
    {
        remaining >>= 1;
        ++shift;
    }

    dpb[DOS_DPB_DRIVE] = (BYTE)drive;
    dpb[DOS_DPB_UNIT]  = (BYTE)drive;
    DosSysVarsWriteWord(dpb, DOS_DPB_SECTOR_SIZE, bytesPerSector);
    /* [CAUTION]: MINUS ONE. 6.22's floppy DPB has 0 here with one sector per cluster --
     * the field is the highest sector INDEX in a cluster, not the count.
     */
    dpb[DOS_DPB_CLUSTER_MAX]    = (BYTE)(sectorsPerCluster ? sectorsPerCluster - 1 : 0);
    dpb[DOS_DPB_CLUSTER_SHIFT]  = (BYTE)shift;
    DosSysVarsWriteWord(dpb, DOS_DPB_RESERVED, DOS_FORMAT_RESERVED_SECTORS);
    dpb[DOS_DPB_FAT_COUNT] = DOS_FORMAT_FAT_COUNT;
    DosSysVarsWriteWord(dpb, DOS_DPB_ROOT_ENTRIES, rootEntries);
    DosDpbFatLayout(bytesPerSector, rootEntries, highestCluster, &fatSectors, &rootStart, &dataStart);
    DosSysVarsWriteWord(dpb, DOS_DPB_DATA_START, dataStart);
    DosSysVarsWriteWord(dpb, DOS_DPB_FAT_SECTORS, fatSectors);
    DosSysVarsWriteWord(dpb, DOS_DPB_ROOT_START, rootStart);
    DosSysVarsWriteWord(dpb, DOS_DPB_HIGHEST_CLUSTER, highestCluster);
    DosSysVarsWriteFarPointer(dpb, DOS_DPB_DEVICE_HEADER, deviceHeaderSegment, deviceHeaderOffset);
    dpb[DOS_DPB_MEDIA] = (BYTE)media;
    dpb[DOS_DPB_ACCESSED] = 0;
    DosSysVarsWriteFarPointer(dpb, DOS_DPB_NEXT, nextSegment, nextOffset);
    /* FFFF = "free cluster count unknown", which is exactly what 6.22 reported
     * (sysvars.dpb0 has FF FF at +0x1F). Claiming a number we have not counted
     * would be the MEM.EXE failure mode in a different structure.
     */
    DosSysVarsWriteWord(dpb, DOS_DPB_FREE_COUNT, DOS_DPB_FREE_COUNT_UNKNOWN);
}

/* Build one CDS entry for drive 0..25. `flags` is the flags word: 0 for a drive
 * letter with nothing behind it (how DOS marks an unused slot in an array that is
 * always LASTDRIVE entries long), DOS_CDS_FLAG_PHYSICAL for a local drive with a DPB,
 * DOS_CDS_FLAG_PHYSICAL|DOS_CDS_FLAG_NETWORK for a redirected one (no DPB).
 */
static inline VOID DosCdsBuild(
    _Out_writes_bytes_(DOS_CDS_LEN) PBYTE cds,
    _In_ UINT drive,
    _In_ UINT flags,
    _In_ UINT dpbSegment,
    _In_ UINT dpbOffset)
{
    UINT byteIndex;

    for (byteIndex = 0; byteIndex < DOS_CDS_LEN; ++byteIndex)
        cds[byteIndex] = 0;

    cds[DOS_CDS_PATH + 0] = (BYTE)('A' + drive);
    cds[DOS_CDS_PATH + 1] = ':';
    cds[DOS_CDS_PATH + 2] = '\\';
    DosSysVarsWriteWord(cds, DOS_CDS_FLAGS, flags);

    if (!flags)
        DosSysVarsWriteFarPointer(cds, DOS_CDS_DPB, DOS_CDS_NO_DPB, DOS_CDS_NO_DPB);
    else if (flags & DOS_CDS_FLAG_NETWORK)
        DosSysVarsWriteFarPointer(cds, DOS_CDS_DPB, DOS_CDS_REDIRECTED_DPB, DOS_CDS_REDIRECTED_DPB);
    else
        DosSysVarsWriteFarPointer(cds, DOS_CDS_DPB, dpbSegment, dpbOffset);

    DosSysVarsWriteWord(cds, DOS_CDS_UNKNOWN, DOS_CDS_UNKNOWN_FILL);
    DosSysVarsWriteWord(cds, DOS_CDS_UNKNOWN + 2, DOS_CDS_UNKNOWN_FILL);
    DosSysVarsWriteWord(cds, DOS_CDS_SLASH, DOS_CDS_ROOT_SLASH_INDEX);              /* "A:\" -- the backslash is at index 2 */
}

static inline VOID DosDeviceHeaderBuild(
    _Out_writes_bytes_(DOS_DEVICE_HEADER_LEN) PBYTE header,
    _In_ UINT nextSegment,
    _In_ UINT nextOffset,
    _In_ UINT attribute,
    _In_ UINT strategyEntry,
    _In_ UINT interruptEntry,
    _In_reads_(DOS_DEVICE_NAME_LEN) PCSTR name)
{
    UINT characterIndex;

    DosSysVarsWriteFarPointer(header, DOS_DEVICE_HEADER_NEXT, nextSegment, nextOffset);
    DosSysVarsWriteWord(header, DOS_DEVICE_HEADER_ATTRIBUTE, attribute);
    DosSysVarsWriteWord(header, DOS_DEVICE_HEADER_STRATEGY, strategyEntry);
    DosSysVarsWriteWord(header, DOS_DEVICE_HEADER_INTERRUPT, interruptEntry);

    for (characterIndex = 0; characterIndex < DOS_DEVICE_NAME_LEN; ++characterIndex)
        header[DOS_DEVICE_HEADER_NAME + characterIndex] = (BYTE)name[characterIndex];
}

/* The DOS_DEV_SEG area, DOS_DEVICE_AREA_LEN bytes: twelve headers linked in 6.22's order and
 * terminated, then the stubs. `segment` is the segment the area will live at (each `next`
 * names it); `units` goes in the block driver's name byte 0 (= SysVars+0x20).
 */
static inline VOID DosDeviceChainBuild(
    _Out_writes_bytes_(DOS_DEVICE_AREA_LEN) PBYTE area,
    _In_ UINT segment,
    _In_ UINT units)
{
    static const struct
    {
        UINT Attribute;
        CHAR Name[DOS_DEVICE_NAME_LEN + 1];
    }

    devices[DOS_DEVICE_COUNT] = {
        { DOS_DEVICE_ATTRIBUTE_CON,   "CON     " }, { DOS_DEVICE_ATTRIBUTE_AUX,   "AUX     " },
        { DOS_DEVICE_ATTRIBUTE_PRN,   "PRN     " }, { DOS_DEVICE_ATTRIBUTE_CLOCK, "CLOCK$  " },
        { DOS_DEVICE_ATTRIBUTE_BLOCK, "" },
        { DOS_DEVICE_ATTRIBUTE_AUX,   "COM1    " }, { DOS_DEVICE_ATTRIBUTE_PRN,   "LPT1    " },
        { DOS_DEVICE_ATTRIBUTE_PRN,   "LPT2    " }, { DOS_DEVICE_ATTRIBUTE_PRN,   "LPT3    " },
        { DOS_DEVICE_ATTRIBUTE_AUX,   "COM2    " }, { DOS_DEVICE_ATTRIBUTE_AUX,   "COM3    " },
        { DOS_DEVICE_ATTRIBUTE_AUX,   "COM4    " },
    };
    static const BYTE stubs[] = {
        0x26, 0xC7, 0x47, 0x03, 0x03, 0x81, 0xCB,        /* DOS_DEVICE_STUB_UNKNOWN */
        0xCB,                                            /* DOS_DEVICE_STUB_RETF */
    };
    UINT index;

    for (index = 0; index < DOS_DEVICE_AREA_LEN; ++index)
        area[index] = 0;

    for (index = 0; index < DOS_DEVICE_COUNT; ++index)
    {
        BOOL isLast = (index + 1 == DOS_DEVICE_COUNT);
        /* the block driver's name is 8 zero bytes; Name[9] "" zero-fills the rest */
        DosDeviceHeaderBuild(area + DOS_DEVICE_OFFSET(index), isLast ? DOS_CHAIN_END : segment,
                             isLast ? DOS_CHAIN_END : DOS_DEVICE_OFFSET(index + 1), devices[index].Attribute,
                             DOS_DEVICE_STUB_UNKNOWN, DOS_DEVICE_STUB_RETF, devices[index].Name);
    }

    area[DOS_DEVICE_OFFSET(DOS_DEVICE_BLOCK) + DOS_DEVICE_HEADER_NAME] = (BYTE)units;

    for (index = 0; index < sizeof stubs; ++index)
        area[DOS_DEVICE_STUB_UNKNOWN + index] = stubs[index];
}

static inline VOID DosNulStubBuild(_Out_writes_bytes_(DOS_NULSTUB_LEN) PBYTE stub)
{
    static const BYTE stubBytes[DOS_NULSTUB_LEN] =
        { 0x26, 0xC7, 0x47, 0x03, 0x00, 0x01, 0xCB, 0xCB };
    UINT index;

    for (index = 0; index < DOS_NULSTUB_LEN; ++index)
        stub[index] = stubBytes[index];
}

static inline VOID DosNulHeaderBuild(
    _Out_writes_bytes_(DOS_DEVICE_HEADER_LEN) PBYTE header,
    _In_ UINT nextSegment,
    _In_ UINT nextOffset,
    _In_ UINT strategyEntry,
    _In_ UINT interruptEntry)
{
    DosDeviceHeaderBuild(header, nextSegment, nextOffset, DOS_DEVICE_ATTRIBUTE_NUL, strategyEntry, interruptEntry, DOS_NUL_NAME);
}

#endif /* NTVDMEX_DOS_SYSVARS_H */
