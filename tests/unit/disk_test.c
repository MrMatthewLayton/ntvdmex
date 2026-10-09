/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * INT 13h geometry and CHS<->LBA, pinned off-VM.  GH #44.
 *
 * Expectations come from tests/probes/dos/p_disk.asm run on MS-DOS 6.22 against a
 * real 1.44MB floppy, quoted in the check names. The arithmetic is where this
 * interface goes wrong -- sector numbers are 1-based while cylinder and head are
 * not -- so it gets pinned rather than trusted.
 *
 *   cc -std=c99 -I src -I src/dos -o disk_test tests/unit/disk_test.c && ./disk_test
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#include <stdio.h>
#include <string.h>
#include "dos_disk.h"

/* The two floppy images the checks build: their sizes, and the geometry 6.22 reported. */
#define DISK_TEST_1440K_IMAGE_SIZE      1474560u
#define DISK_TEST_720K_IMAGE_SIZE       737280u
#define DISK_TEST_TRUNCATED_IMAGE_SIZE  1024u
#define DISK_TEST_1440K_TOTAL_SECTORS   2880
#define DISK_TEST_720K_TOTAL_SECTORS    1440
#define DISK_TEST_FLOPPY_CYLINDERS      80
#define DISK_TEST_LAST_CYLINDER         79
#define DISK_TEST_LAST_HEAD             1
#define DISK_TEST_ODD_SECTOR_SIZE       1024u
#define DISK_TEST_ORACLE_CX_1440K       0x4F12  /* int13.08.params: CH=79, CL=18 */
#define DISK_TEST_ORACLE_CX_720K        0x4F09
#define DISK_TEST_LAST_LBA_1440K        2879
#define DISK_TEST_REFUSED               (-1L)   /* What lbaOrRefused reports for a refusal */
#define DISK_TEST_EMPTY                 0       /* A zeroed field: "none" */

static INT g_Checks, g_Failures;

static VOID DiskTestExpect(PCSTR description, LONG actual, LONG expected)
{
    ++g_Checks;
    if (actual == expected)
        return;
    ++g_Failures;
    printf("  FAIL %-56s got 0x%lX, want 0x%lX\n", description, (long)actual, (long)expected);
}

/* Write a little-endian WORD into a BPB field. */
static VOID DiskTestPutWord(PBYTE field, WORD value)
{
    field[0] = LOBYTE(value);
    field[1] = HIBYTE(value);
}

/* A 1.44MB FAT12 BPB, the shape p_disk measured on the oracle. */
static VOID DiskTestBuild1440kBpb(PBYTE bootSector)
{
    memset(bootSector, DISK_TEST_EMPTY, DOS_SECTOR_SIZE);
    DiskTestPutWord(bootSector + DOS_BPB_BYTES_PER_SECTOR, DOS_SECTOR_SIZE);
    DiskTestPutWord(bootSector + DOS_BPB_TOTAL_SECTORS_16, DISK_TEST_1440K_TOTAL_SECTORS);
    DiskTestPutWord(bootSector + DOS_BPB_SECTORS_PER_TRACK, DOS_FLOPPY_1440K_SECTORS);
    DiskTestPutWord(bootSector + DOS_BPB_HEADS, DOS_FLOPPY_HEADS);
}

/* CHS -> LBA as one number for a check: the LBA, or DISK_TEST_REFUSED. */
static LONG DiskTestLbaOrRefused(
    PCDOS_DISK_GEOMETRY geometry,
    WORD cylinder,
    WORD head,
    WORD sector)
{
    DWORD logicalBlock;

    return DosDiskChsToLba(geometry, cylinder, head, sector, &logicalBlock)
           ? (LONG)logicalBlock : DISK_TEST_REFUSED;
}

INT main(VOID)
{
    BYTE bootSector[DOS_SECTOR_SIZE];
    DOS_DISK_GEOMETRY geometry;

    printf("== INT 13h geometry (dos_disk.h) -- measured on 6.22\n");

    DiskTestBuild1440kBpb(bootSector);
    DiskTestExpect("1.44MB BPB is accepted",
                   DosDiskGeometryFromBpb(bootSector, DISK_TEST_1440K_IMAGE_SIZE, &geometry), TRUE);
    DiskTestExpect("...80 cylinders", geometry.Cylinders, DISK_TEST_FLOPPY_CYLINDERS);
    DiskTestExpect("...18 sectors, 2 heads",
                   MAKEWORD(geometry.Heads, geometry.SectorsPerTrack),
                   MAKEWORD(DOS_FLOPPY_HEADS, DOS_FLOPPY_1440K_SECTORS));
    /* Oracle: int13.08.params CX=4F12 -- CH=79 (max cyl), CL=18 (sectors). */
    DiskTestExpect("AH=08h packs CX = 0x4F12", DosDiskPackCx(&geometry), DISK_TEST_ORACLE_CX_1440K);
    /* Oracle: BX=0004, i.e. BL=4 = 1.44MB. */
    DiskTestExpect("AH=08h drive type = 4 (1.44MB)", geometry.DriveType, DOS_DRIVE_1440K);

    /* -- CHS -> LBA. THE SECTOR IS 1-BASED; cylinder and head are not. */
    DiskTestExpect("C0 H0 S1 is LBA 0 (the boot sector)",
                   DiskTestLbaOrRefused(&geometry, 0, 0, DOS_DISK_FIRST_SECTOR), 0);
    DiskTestExpect("C0 H0 S2 is LBA 1",
                   DiskTestLbaOrRefused(&geometry, 0, 0, DOS_DISK_FIRST_SECTOR + 1), 1);
    DiskTestExpect("C0 H1 S1 is LBA 18 (second head)",
                   DiskTestLbaOrRefused(&geometry, 0, 1, DOS_DISK_FIRST_SECTOR),
                   DOS_FLOPPY_1440K_SECTORS);
    DiskTestExpect("C1 H0 S1 is LBA 36 (second cylinder)",
                   DiskTestLbaOrRefused(&geometry, 1, 0, DOS_DISK_FIRST_SECTOR),
                   DOS_FLOPPY_1440K_SECTORS * DOS_FLOPPY_HEADS);
    DiskTestExpect("the last sector C79 H1 S18 is LBA 2879",
                   DiskTestLbaOrRefused(&geometry, DISK_TEST_LAST_CYLINDER, DISK_TEST_LAST_HEAD,
                                        DOS_FLOPPY_1440K_SECTORS),
                   DISK_TEST_LAST_LBA_1440K);

    /* [CAUTION]: SECTOR 0 DOES NOT EXIST. A host that accepts it reads one sector early
     * for every access and reports success -- silently wrong data, not an error.
     */
    DiskTestExpect("sector 0 is REFUSED",
                   DiskTestLbaOrRefused(&geometry, 0, 0, DOS_DISK_FIRST_SECTOR - 1),
                   DISK_TEST_REFUSED);
    DiskTestExpect("sector 19 is REFUSED (only 18 per track)",
                   DiskTestLbaOrRefused(&geometry, 0, 0, DOS_FLOPPY_1440K_SECTORS + 1),
                   DISK_TEST_REFUSED);
    DiskTestExpect("head 2 is REFUSED (only 2 heads)",
                   DiskTestLbaOrRefused(&geometry, 0, DOS_FLOPPY_HEADS, DOS_DISK_FIRST_SECTOR),
                   DISK_TEST_REFUSED);
    DiskTestExpect("cylinder 80 is REFUSED (only 80)",
                   DiskTestLbaOrRefused(&geometry, DISK_TEST_FLOPPY_CYLINDERS, 0,
                                        DOS_DISK_FIRST_SECTOR),
                   DISK_TEST_REFUSED);

    /* -- AN IMAGE WE CANNOT TRUST IS ABSENT, NOT GUESSED AT. */
    DiskTestBuild1440kBpb(bootSector);
    DiskTestExpect("a TRUNCATED image is refused",
                   DosDiskGeometryFromBpb(bootSector, DISK_TEST_TRUNCATED_IMAGE_SIZE, &geometry),
                   FALSE);
    DiskTestBuild1440kBpb(bootSector);
    DiskTestPutWord(bootSector + DOS_BPB_BYTES_PER_SECTOR, DISK_TEST_ODD_SECTOR_SIZE);
    DiskTestExpect("a non-512 sector size is refused",
                   DosDiskGeometryFromBpb(bootSector, DISK_TEST_1440K_IMAGE_SIZE, &geometry), FALSE);
    DiskTestBuild1440kBpb(bootSector);
    DiskTestPutWord(bootSector + DOS_BPB_SECTORS_PER_TRACK, DISK_TEST_EMPTY);
    DiskTestExpect("zero sectors per track is refused",
                   DosDiskGeometryFromBpb(bootSector, DISK_TEST_1440K_IMAGE_SIZE, &geometry), FALSE);
    DiskTestBuild1440kBpb(bootSector);
    DiskTestPutWord(bootSector + DOS_BPB_HEADS, DISK_TEST_EMPTY);
    DiskTestExpect("zero heads is refused",
                   DosDiskGeometryFromBpb(bootSector, DISK_TEST_1440K_IMAGE_SIZE, &geometry), FALSE);
    memset(bootSector, DISK_TEST_EMPTY, DOS_SECTOR_SIZE);
    DiskTestExpect("an all-zero sector is refused",
                   DosDiskGeometryFromBpb(bootSector, DISK_TEST_1440K_IMAGE_SIZE, &geometry), FALSE);

    /* A 720K disk is a different geometry from the same file size class, which
     * is exactly why the BPB is read rather than the size inspected.
     */
    DiskTestBuild1440kBpb(bootSector);
    DiskTestPutWord(bootSector + DOS_BPB_TOTAL_SECTORS_16, DISK_TEST_720K_TOTAL_SECTORS);
    DiskTestPutWord(bootSector + DOS_BPB_SECTORS_PER_TRACK, DOS_FLOPPY_720K_SECTORS);
    DiskTestExpect("720K BPB accepted",
                   DosDiskGeometryFromBpb(bootSector, DISK_TEST_720K_IMAGE_SIZE, &geometry), TRUE);
    DiskTestExpect("...80 cylinders too", geometry.Cylinders, DISK_TEST_FLOPPY_CYLINDERS);
    DiskTestExpect("...but drive type 3 (720K)", geometry.DriveType, DOS_DRIVE_720K);
    DiskTestExpect("...and CX packs as 0x4F09", DosDiskPackCx(&geometry), DISK_TEST_ORACLE_CX_720K);

    printf("== %d checks, %d failed\n", g_Checks, g_Failures);
    return g_Failures ? 1 : 0;
}
