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
 * Measured on MS-DOS 6.22 with a real 1.44MB floppy (tools/dostest/p_disk.asm):
 *     int13.08.params  AX=0000 BX=0004 CX=4F12 DX=0101 CF=0
 *     int13.15.type    AX=0100
 *     int13.02.read    AX=0001, boot signature 55AA
 * i.e. CH=79 (80 cylinders), CL=18 sectors/track, DH=1 (2 heads), DL=1 drive,
 * BL=4 (1.44MB), and AH=01 from 15h means "floppy, no change-line support".
 *
 * Pure -- no Windows types -- so tools/dostest/disk_test.c can pin the
 * arithmetic, which is where the off-by-one lives.
 */
#ifndef DOS_DISK_H
#define DOS_DISK_H

#include <stdint.h>

typedef struct {
    uint16_t bytes_per_sec;
    uint16_t sectors;          /* per track                                    */
    uint16_t heads;
    uint32_t total_sectors;
    uint16_t cylinders;        /* derived: total / (sectors * heads)           */
    uint8_t  drive_type;       /* AH=08h's BL                                  */
    int      valid;
} dos_disk_geom;

/* BIOS drive types, as AH=08h reports them in BL. 4 is the one that matters
   here and it is measured: 6.22 answered BX=0004 for a 1.44MB floppy. */
#define DOS_DRIVE_360K   0x01
#define DOS_DRIVE_1200K  0x02
#define DOS_DRIVE_720K   0x03
#define DOS_DRIVE_1440K  0x04

/* Read the geometry out of a boot sector's BPB. Returns 0 and leaves
   geom->valid = 0 if the sector is not a plausible BPB -- an image whose
   geometry we cannot read is treated as ABSENT rather than guessed at, because
   a guessed cylinder count silently returns the wrong sector. */
static int dos_disk_geom_from_bpb(const uint8_t *boot, uint32_t file_bytes,
                                  dos_disk_geom *g)
{
    uint32_t total;
    g->valid = 0;
    if (!boot) return 0;
    g->bytes_per_sec = (uint16_t)(boot[11] | (boot[12] << 8));
    g->sectors       = (uint16_t)(boot[24] | (boot[25] << 8));
    g->heads         = (uint16_t)(boot[26] | (boot[27] << 8));
    total            = (uint32_t)(boot[19] | (boot[20] << 8));
    if (total == 0)                                   /* the >64K-sector form */
        total = (uint32_t)boot[32] | ((uint32_t)boot[33] << 8)
              | ((uint32_t)boot[34] << 16) | ((uint32_t)boot[35] << 24);
    /* Every one of these must be sane before the arithmetic below means
       anything. 512 is not assumed -- it is required to be what the BPB says
       AND a power of two the rest of this layer can address. */
    if (g->bytes_per_sec != 512) return 0;
    if (g->sectors == 0 || g->sectors > 63) return 0;
    if (g->heads == 0 || g->heads > 255) return 0;
    if (total == 0) return 0;
    /* The image must actually CONTAIN the sectors its BPB claims. A truncated
       image that says 2880 is worse than no image: reads past the end would
       return whatever the read call left in the buffer. */
    if (file_bytes / 512u < total) return 0;
    g->total_sectors = total;
    g->cylinders = (uint16_t)(total / ((uint32_t)g->sectors * g->heads));
    if (g->cylinders == 0) return 0;
    g->drive_type = (g->sectors == 18 && g->heads == 2) ? DOS_DRIVE_1440K
                  : (g->sectors == 9  && g->heads == 2) ? DOS_DRIVE_720K
                  : (g->sectors == 15 && g->heads == 2) ? DOS_DRIVE_1200K
                                                        : DOS_DRIVE_360K;
    g->valid = 1;
    return 1;
}

/* CHS -> LBA.  ★ SECTOR NUMBERS ARE 1-BASED and that is the classic off-by-one
   in this interface: cylinder and head count from 0, the sector does not.
   Returns 0 if the address is outside the geometry, which the caller reports as
   AH=04 "sector not found" rather than reading somewhere else. */
static int dos_disk_chs_to_lba(const dos_disk_geom *g, uint16_t cyl,
                               uint16_t head, uint16_t sector, uint32_t *lba)
{
    if (!g->valid || sector == 0) return 0;
    if (cyl >= g->cylinders || head >= g->heads || sector > g->sectors) return 0;
    *lba = ((uint32_t)cyl * g->heads + head) * g->sectors + (sector - 1u);
    return 1;
}

/* AH=08h packs the cylinder count into CH plus the top two bits of CL, with the
   sector count in CL's low six. Both are "max", i.e. one less than the count,
   for cylinder and head -- but NOT for the sector, which is 1-based already.
   6.22 on a 1.44MB floppy: CX=4F12, so CH=0x4F=79 and CL=0x12=18. */
static uint16_t dos_disk_pack_cx(const dos_disk_geom *g)
{
    uint16_t maxcyl = (uint16_t)(g->cylinders - 1);
    return (uint16_t)(((maxcyl & 0xFF) << 8)
                    | ((maxcyl & 0x300) >> 2)
                    | (g->sectors & 0x3F));
}

#endif /* DOS_DISK_H */
