/* dos_disk.h -- geometry and CHS<->LBA for the INT 13h / INT 25h layer.  GH #44.
 *
 * This host has no raw sectors: DOS calls map onto Win32 file APIs on the host's
 * own filesystem, so there is nothing underneath INT 13h to read. #44 asks us to
 * "decide deliberately how these map onto our host-file-backed model", and the
 * decision is: **a drive is a disk IMAGE FILE, or it is absent.** Nothing is
 * synthesised. A guest that reads sector 0 gets the real boot sector of a real
 * image, or an honest "drive not ready".
 *
 * ⚠ THE GEOMETRY COMES OUT OF THE IMAGE'S OWN BPB, NEVER FROM ITS SIZE. The same
 *   1,474,560 bytes can be 80x2x18 or 40x2x36, and a guest that seeks by CHS
 *   against the wrong one reads the wrong sector and reports success. fat12.py
 *   already makes this point for the same reason.
 *
 * Measured on MS-DOS 6.22 with a real 1.44MB floppy (tests/probes/dos/p_disk.asm):
 *     int13.08.params  AX=0000 BX=0004 CX=4F12 DX=0101 CF=0
 *     int13.15.type    AX=0100
 *     int13.02.read    AX=0001, boot signature 55AA
 * i.e. CH=79 (80 cylinders), CL=18 sectors/track, DH=1 (2 heads), DL=1 drive,
 * BL=4 (1.44MB), and AH=01 from 15h means "floppy, no change-line support".
 *
 * No Windows calls, only Windows types (src/ntvdmex_types.h), so
 * tests/unit/disk_test.c can pin the arithmetic, which is where the off-by-one lives.
 */
#ifndef NTVDMEX_DOS_DISK_H
#define NTVDMEX_DOS_DISK_H

#include "../ntvdmex_types.h"

typedef struct _DOS_DISK_GEOMETRY {
    WORD     BytesPerSector;
    WORD     SectorsPerTrack;
    WORD     Heads;
    DWORD    TotalSectors;
    WORD     Cylinders;        /* derived: total / (sectors per track * heads)  */
    BYTE     DriveType;        /* AH=08h's BL                                   */
    BOOL     IsValid;
} DOS_DISK_GEOMETRY, *PDOS_DISK_GEOMETRY;

typedef const DOS_DISK_GEOMETRY *PCDOS_DISK_GEOMETRY;

/* BIOS drive types, as AH=08h reports them in BL. 4 is the one that matters
   here and it is measured: 6.22 answered BX=0004 for a 1.44MB floppy. */
#define DOS_DRIVE_360K   0x01
#define DOS_DRIVE_1200K  0x02
#define DOS_DRIVE_720K   0x03
#define DOS_DRIVE_1440K  0x04

/* The one sector size this layer addresses -- and what a BPB must declare. */
#define DOS_SECTOR_SIZE  512

/* Where the BIOS Parameter Block's fields sit in a boot sector. Each is little-endian:
   the low byte at the offset, the next at +1, and so on. */
#define DOS_BPB_BYTES_PER_SECTOR      11
#define DOS_BPB_TOTAL_SECTORS_16      19    /* 0 here = the 32-bit field is used    */
#define DOS_BPB_SECTORS_PER_TRACK     24
#define DOS_BPB_HEADS                 26
#define DOS_BPB_TOTAL_SECTORS_32      32    /* the >64K-sector form                 */
#define DOS_BPB_BYTE1                 1     /* the field's second byte, and so on   */
#define DOS_BPB_BYTE2                 2
#define DOS_BPB_BYTE3                 3

/* The limits a CHS address can express: 6 bits of sector, 8 bits of head. */
#define DOS_DISK_MAX_SECTORS_PER_TRACK  63
#define DOS_DISK_MAX_HEADS              255

/* The standard floppy geometries the drive type is told apart by. */
#define DOS_FLOPPY_HEADS              2
#define DOS_FLOPPY_1440K_SECTORS      18
#define DOS_FLOPPY_1200K_SECTORS      15
#define DOS_FLOPPY_720K_SECTORS       9

/* ★ SECTOR NUMBERS ARE 1-BASED; cylinder and head count from 0, so the last cylinder is
   the count less one. */
#define DOS_DISK_NO_SECTOR            0     /* sector 0 does not exist              */
#define DOS_DISK_FIRST_SECTOR         1u
#define DOS_DISK_LAST_CYLINDER_OFFSET 1

/* AH=08h's CX packing: CH = the low 8 bits of the last cylinder, CL bits 7-6 = its
   bits 9-8, CL bits 5-0 = the sectors per track. */
#define DOS_DISK_CX_CYLINDER_LOW_MASK   0xFF
#define DOS_DISK_CX_CYLINDER_LOW_SHIFT  8
#define DOS_DISK_CX_CYLINDER_HIGH_MASK  0x300
#define DOS_DISK_CX_CYLINDER_HIGH_SHIFT 2
#define DOS_DISK_CX_SECTOR_MASK         0x3F

/* Read the geometry out of a boot sector's BPB. Returns FALSE and leaves
   geometry->IsValid FALSE if the sector is not a plausible BPB -- an image whose
   geometry we cannot read is treated as ABSENT rather than guessed at, because
   a guessed cylinder count silently returns the wrong sector. */
static BOOL DosDiskGeometryFromBpb(_In_reads_bytes_opt_(DOS_SECTOR_SIZE) PCBYTE bootSector,
                                   _In_ DWORD imageSize,
                                   _Out_ PDOS_DISK_GEOMETRY geometry)
{
    DWORD totalSectors;
    geometry->IsValid = FALSE;
    if (!bootSector) return FALSE;
    geometry->BytesPerSector  = (WORD)(bootSector[DOS_BPB_BYTES_PER_SECTOR]
        | (bootSector[DOS_BPB_BYTES_PER_SECTOR + DOS_BPB_BYTE1] << BYTE_SHIFT));
    geometry->SectorsPerTrack = (WORD)(bootSector[DOS_BPB_SECTORS_PER_TRACK]
        | (bootSector[DOS_BPB_SECTORS_PER_TRACK + DOS_BPB_BYTE1] << BYTE_SHIFT));
    geometry->Heads           = (WORD)(bootSector[DOS_BPB_HEADS]
        | (bootSector[DOS_BPB_HEADS + DOS_BPB_BYTE1] << BYTE_SHIFT));
    totalSectors              = (DWORD)(bootSector[DOS_BPB_TOTAL_SECTORS_16]
        | (bootSector[DOS_BPB_TOTAL_SECTORS_16 + DOS_BPB_BYTE1] << BYTE_SHIFT));
    if (totalSectors == 0)                            /* the >64K-sector form */
        totalSectors = (DWORD)bootSector[DOS_BPB_TOTAL_SECTORS_32]
              | ((DWORD)bootSector[DOS_BPB_TOTAL_SECTORS_32 + DOS_BPB_BYTE1] << BYTE_SHIFT)
              | ((DWORD)bootSector[DOS_BPB_TOTAL_SECTORS_32 + DOS_BPB_BYTE2] << WORD_SHIFT)
              | ((DWORD)bootSector[DOS_BPB_TOTAL_SECTORS_32 + DOS_BPB_BYTE3] << TOP_BYTE_SHIFT);
    /* Every one of these must be sane before the arithmetic below means
       anything. 512 is not assumed -- it is required to be what the BPB says
       AND a power of two the rest of this layer can address. */
    if (geometry->BytesPerSector != DOS_SECTOR_SIZE) return FALSE;
    if (geometry->SectorsPerTrack == 0
        || geometry->SectorsPerTrack > DOS_DISK_MAX_SECTORS_PER_TRACK) return FALSE;
    if (geometry->Heads == 0 || geometry->Heads > DOS_DISK_MAX_HEADS) return FALSE;
    if (totalSectors == 0) return FALSE;
    /* The image must actually CONTAIN the sectors its BPB claims. A truncated
       image that says 2880 is worse than no image: reads past the end would
       return whatever the read call left in the buffer. */
    if (imageSize / (DWORD)DOS_SECTOR_SIZE < totalSectors) return FALSE;
    geometry->TotalSectors = totalSectors;
    geometry->Cylinders = (WORD)(totalSectors
                                 / ((DWORD)geometry->SectorsPerTrack * geometry->Heads));
    if (geometry->Cylinders == 0) return FALSE;
    geometry->DriveType =
          (geometry->SectorsPerTrack == DOS_FLOPPY_1440K_SECTORS
           && geometry->Heads == DOS_FLOPPY_HEADS) ? DOS_DRIVE_1440K
        : (geometry->SectorsPerTrack == DOS_FLOPPY_720K_SECTORS
           && geometry->Heads == DOS_FLOPPY_HEADS) ? DOS_DRIVE_720K
        : (geometry->SectorsPerTrack == DOS_FLOPPY_1200K_SECTORS
           && geometry->Heads == DOS_FLOPPY_HEADS) ? DOS_DRIVE_1200K
                                                   : DOS_DRIVE_360K;
    geometry->IsValid = TRUE;
    return TRUE;
}

/* CHS -> LBA.  ★ SECTOR NUMBERS ARE 1-BASED and that is the classic off-by-one
   in this interface: cylinder and head count from 0, the sector does not.
   Returns FALSE if the address is outside the geometry, which the caller reports as
   AH=04 "sector not found" rather than reading somewhere else. */
static BOOL DosDiskChsToLba(_In_ PCDOS_DISK_GEOMETRY geometry, _In_ WORD cylinder,
                            _In_ WORD head, _In_ WORD sector, _Out_ PDWORD logicalBlock)
{
    if (!geometry->IsValid || sector == DOS_DISK_NO_SECTOR) return FALSE;
    if (cylinder >= geometry->Cylinders || head >= geometry->Heads
        || sector > geometry->SectorsPerTrack) return FALSE;
    *logicalBlock = ((DWORD)cylinder * geometry->Heads + head) * geometry->SectorsPerTrack
                  + (sector - DOS_DISK_FIRST_SECTOR);
    return TRUE;
}

/* AH=08h packs the cylinder count into CH plus the top two bits of CL, with the
   sector count in CL's low six. Both are "max", i.e. one less than the count,
   for cylinder and head -- but NOT for the sector, which is 1-based already.
   6.22 on a 1.44MB floppy: CX=4F12, so CH=0x4F=79 and CL=0x12=18. */
static WORD DosDiskPackCx(_In_ PCDOS_DISK_GEOMETRY geometry)
{
    WORD lastCylinder = (WORD)(geometry->Cylinders - DOS_DISK_LAST_CYLINDER_OFFSET);
    return (WORD)(((lastCylinder & DOS_DISK_CX_CYLINDER_LOW_MASK) << DOS_DISK_CX_CYLINDER_LOW_SHIFT)
                | ((lastCylinder & DOS_DISK_CX_CYLINDER_HIGH_MASK) >> DOS_DISK_CX_CYLINDER_HIGH_SHIFT)
                | (geometry->SectorsPerTrack & DOS_DISK_CX_SECTOR_MASK));
}

#endif /* NTVDMEX_DOS_DISK_H */
