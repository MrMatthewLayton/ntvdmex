/* disk_test.c -- INT 13h geometry and CHS<->LBA, pinned off-VM.  GH #44.
 *
 * Expectations come from tools/dostest/p_disk.asm run on MS-DOS 6.22 against a
 * real 1.44MB floppy, quoted in the check names. The arithmetic is where this
 * interface goes wrong -- sector numbers are 1-based while cylinder and head are
 * not -- so it gets pinned rather than trusted.
 *
 *   cc -std=c99 -I src/dos -o disk_test tools/dostest/disk_test.c && ./disk_test
 */
#include <stdio.h>
#include <string.h>
#include "dos_disk.h"

static int checks, fails;
static void eq(const char *what, long got, long want)
{
    ++checks;
    if (got == want) return;
    ++fails;
    printf("  FAIL %-56s got 0x%lX, want 0x%lX\n", what, got, want);
}

/* A 1.44MB FAT12 BPB, the shape p_disk measured on the oracle. */
static void bpb144(uint8_t *b)
{
    memset(b, 0, 512);
    b[11] = 0x00; b[12] = 0x02;      /* 512 bytes per sector   */
    b[19] = 0x40; b[20] = 0x0B;      /* 2880 total sectors     */
    b[24] = 18;   b[25] = 0;         /* 18 sectors per track   */
    b[26] = 2;    b[27] = 0;         /* 2 heads                */
}

int main(void)
{
    uint8_t b[512];
    dos_disk_geom g;
    uint32_t lba;

    printf("== INT 13h geometry (dos_disk.h) -- measured on 6.22\n");

    bpb144(b);
    eq("1.44MB BPB is accepted", dos_disk_geom_from_bpb(b, 1474560, &g), 1);
    eq("...80 cylinders", g.cylinders, 80);
    eq("...18 sectors, 2 heads", (g.sectors << 8) | g.heads, (18 << 8) | 2);
    /* Oracle: int13.08.params CX=4F12 -- CH=79 (max cyl), CL=18 (sectors). */
    eq("AH=08h packs CX = 0x4F12", dos_disk_pack_cx(&g), 0x4F12);
    /* Oracle: BX=0004, i.e. BL=4 = 1.44MB. */
    eq("AH=08h drive type = 4 (1.44MB)", g.drive_type, DOS_DRIVE_1440K);

    /* ── CHS -> LBA. ★ THE SECTOR IS 1-BASED; cylinder and head are not. */
    eq("C0 H0 S1 is LBA 0 (the boot sector)",
       dos_disk_chs_to_lba(&g, 0, 0, 1, &lba) ? (long)lba : -1, 0);
    eq("C0 H0 S2 is LBA 1", dos_disk_chs_to_lba(&g, 0, 0, 2, &lba) ? (long)lba : -1, 1);
    eq("C0 H1 S1 is LBA 18 (second head)",
       dos_disk_chs_to_lba(&g, 0, 1, 1, &lba) ? (long)lba : -1, 18);
    eq("C1 H0 S1 is LBA 36 (second cylinder)",
       dos_disk_chs_to_lba(&g, 1, 0, 1, &lba) ? (long)lba : -1, 36);
    eq("the last sector C79 H1 S18 is LBA 2879",
       dos_disk_chs_to_lba(&g, 79, 1, 18, &lba) ? (long)lba : -1, 2879);

    /* ⚠ SECTOR 0 DOES NOT EXIST. A host that accepts it reads one sector early
       for every access and reports success -- silently wrong data, not an error. */
    eq("sector 0 is REFUSED", dos_disk_chs_to_lba(&g, 0, 0, 0, &lba), 0);
    eq("sector 19 is REFUSED (only 18 per track)",
       dos_disk_chs_to_lba(&g, 0, 0, 19, &lba), 0);
    eq("head 2 is REFUSED (only 2 heads)", dos_disk_chs_to_lba(&g, 0, 2, 1, &lba), 0);
    eq("cylinder 80 is REFUSED (only 80)", dos_disk_chs_to_lba(&g, 80, 0, 1, &lba), 0);

    /* ── AN IMAGE WE CANNOT TRUST IS ABSENT, NOT GUESSED AT. */
    bpb144(b);
    eq("a TRUNCATED image is refused", dos_disk_geom_from_bpb(b, 1024, &g), 0);
    bpb144(b); b[11] = 0x00; b[12] = 0x04;      /* 1024-byte sectors */
    eq("a non-512 sector size is refused", dos_disk_geom_from_bpb(b, 1474560, &g), 0);
    bpb144(b); b[24] = 0;                        /* 0 sectors/track   */
    eq("zero sectors per track is refused", dos_disk_geom_from_bpb(b, 1474560, &g), 0);
    bpb144(b); b[26] = 0;                        /* 0 heads           */
    eq("zero heads is refused", dos_disk_geom_from_bpb(b, 1474560, &g), 0);
    memset(b, 0, 512);
    eq("an all-zero sector is refused", dos_disk_geom_from_bpb(b, 1474560, &g), 0);

    /* A 720K disk is a different geometry from the same file size class, which
       is exactly why the BPB is read rather than the size inspected. */
    bpb144(b); b[19] = 0xA0; b[20] = 0x05; b[24] = 9;   /* 1440 sectors, 9 spt */
    eq("720K BPB accepted", dos_disk_geom_from_bpb(b, 737280, &g), 1);
    eq("...80 cylinders too", g.cylinders, 80);
    eq("...but drive type 3 (720K)", g.drive_type, DOS_DRIVE_720K);
    eq("...and CX packs as 0x4F09", dos_disk_pack_cx(&g), 0x4F09);

    printf("== %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
