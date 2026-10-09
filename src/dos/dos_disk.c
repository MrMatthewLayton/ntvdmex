/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * A disk image's geometry from its BPB, and CHS<->LBA.
 *
 * The function definitions of dos_disk.h, which keeps their declarations and doc comments (#335).
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#include "dos_disk.h"

BOOL DosDiskGeometryFromBpb(_In_reads_bytes_opt_(DOS_SECTOR_SIZE) PCBYTE bootSector,
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
     * anything. 512 is not assumed -- it is required to be what the BPB says
     * AND a power of two the rest of this layer can address.
     */
    if (geometry->BytesPerSector != DOS_SECTOR_SIZE) return FALSE;
    if (geometry->SectorsPerTrack == 0
        || geometry->SectorsPerTrack > DOS_DISK_MAX_SECTORS_PER_TRACK) return FALSE;
    if (geometry->Heads == 0 || geometry->Heads > DOS_DISK_MAX_HEADS) return FALSE;
    if (totalSectors == 0) return FALSE;
    /* The image must actually CONTAIN the sectors its BPB claims. A truncated
     * image that says 2880 is worse than no image: reads past the end would
     * return whatever the read call left in the buffer.
     */
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

BOOL DosDiskChsToLba(_In_ PCDOS_DISK_GEOMETRY geometry, _In_ WORD cylinder,
                            _In_ WORD head, _In_ WORD sector, _Out_ PDWORD logicalBlock)
{
    if (!geometry->IsValid || sector == DOS_DISK_NO_SECTOR) return FALSE;
    if (cylinder >= geometry->Cylinders || head >= geometry->Heads
        || sector > geometry->SectorsPerTrack) return FALSE;
    *logicalBlock = ((DWORD)cylinder * geometry->Heads + head) * geometry->SectorsPerTrack
                  + (sector - DOS_DISK_FIRST_SECTOR);
    return TRUE;
}

WORD DosDiskPackCx(_In_ PCDOS_DISK_GEOMETRY geometry)
{
    WORD lastCylinder = (WORD)(geometry->Cylinders - DOS_DISK_LAST_CYLINDER_OFFSET);
    return (WORD)(((lastCylinder & DOS_DISK_CX_CYLINDER_LOW_MASK) << DOS_DISK_CX_CYLINDER_LOW_SHIFT)
                | ((lastCylinder & DOS_DISK_CX_CYLINDER_HIGH_MASK) >> DOS_DISK_CX_CYLINDER_HIGH_SHIFT)
                | (geometry->SectorsPerTrack & DOS_DISK_CX_SECTOR_MASK));
}
