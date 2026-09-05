/* dos_sysvars.h -- INT 21h AH=52h's List of Lists, built to the MEASURED layout.
 *
 * GH #48. AH=52h returns ES:BX pointing INTO this structure; the word at ES:BX-2
 * is the first MCB segment and everything from ES:BX on is DOS's own bookkeeping.
 *
 * ── EVERY OFFSET BELOW CAME OFF THE ORACLE, NOT OUT OF A BOOK ────────────────
 * #48 is explicit that these structures "must not be written from documentation
 * alone -- the layout dumps have twice caught errors that a plausible reading
 * would have missed". So tools/dostest/p_sysvar.asm dumped them from genuine
 * MS-DOS 6.22 and this is that dump decoded:
 *
 *   BUF=sysvars.raw 5302 6A131601 CC001601 59007000 23007000 0002 6D001601
 *                   0000 5003 0000 1E03 0000 03 05 <NUL header> ...
 *
 *     ES:BX-2  0253        first MCB segment
 *     +0x00    0116:136A   DPB chain
 *     +0x04    0116:00CC   SFT chain
 *     +0x08    0070:0059   CLOCK$ device
 *     +0x0C    0070:0023   CON device
 *     +0x10    0x0200      max bytes per sector (512)
 *     +0x12    0116:006D   disk buffer chain
 *     +0x16    0350:0000   CDS array
 *     +0x1A    031E:0000   FCB table
 *     +0x1E    0x0000      FCB keep count
 *     +0x20    3           number of BLOCK devices
 *     +0x21    5           LASTDRIVE
 *     +0x22    the NUL device header, INLINE -- not a pointer to one, and 18
 *              bytes long: next 0255:0000, attr 8004, strat 0DC6, intr 0DCC,
 *              name 'NUL     '
 *
 *   BUF=sysvars.dpb0 000000020000010002E0002100200B090013006B007000F0008B131601...
 *     a 33-byte DOS 4+ DPB for A:, and the next one begins EXACTLY 33 bytes
 *     later at 0116:138B -- which the chain pointer at +0x19 confirms (0x136A +
 *     0x21 = 0x138B). That arithmetic is why 33 is a measurement here and not a
 *     recollection.
 *
 *   BUF=sysvars.cds0 413A5C 00... 0040 6A131601 0000 FFFFFFFF 0200 ... 423A5C
 *     'A:\' at +0x00, then the next entry's 'B:\' at offset 88 -- so a CDS entry
 *     is 0x58 bytes, the flags word at +0x43 is 0x4000, and the far pointer at
 *     +0x45 is 0116:136A, i.e. THE FIRST DPB. An entry points at its own drive's
 *     DPB, which is the link a memory/disk walker follows.
 *
 * Pure -- byte buffers and integers only -- so tools/dostest/sysvars_test.c can
 * pin every offset against those same dumps.
 */
#ifndef DOS_SYSVARS_H
#define DOS_SYSVARS_H

#include "dos_layout.h"

/* ---- SysVars field offsets, relative to what AH=52h returns in ES:BX. */
#define SV_MCB_HEAD      (-2)
#define SV_DPB           0x00
#define SV_SFT           0x04
#define SV_CLOCK         0x08
#define SV_CON           0x0C
#define SV_MAXSEC        0x10
#define SV_BUFFERS       0x12
#define SV_CDS           0x16
#define SV_FCB           0x1A
#define SV_FCB_KEEP      0x1E
#define SV_NBLOCKDEV     0x20
#define SV_LASTDRIVE     0x21
#define SV_NUL           0x22        /* the NUL device header, INLINE */
#define SV_NUL_LEN       0x12        /* 18 bytes, measured */
#define SV_LEN           (SV_NUL + SV_NUL_LEN)   /* 0x34 */

/* ---- DPB (DOS 4.0+), 33 bytes. Offsets measured from sysvars.dpb0. */
#define DPB_DRIVE        0x00
#define DPB_UNIT         0x01
#define DPB_SECSIZE      0x02
#define DPB_CLUSTMAX     0x04        /* sectors per cluster MINUS ONE */
#define DPB_CLUSTSHIFT   0x05
#define DPB_RESERVED     0x06
#define DPB_NFATS        0x08
#define DPB_ROOTENTS     0x09
#define DPB_DATASTART    0x0B
#define DPB_CLUSTHIGH    0x0D        /* highest cluster number                */
#define DPB_FATSECS      0x0F        /* WORD on DOS 4+, was a byte before     */
#define DPB_ROOTSTART    0x11
#define DPB_DEVHDR       0x13        /* far pointer to the device header      */
#define DPB_MEDIA        0x17
#define DPB_ACCESSED     0x18
#define DPB_NEXT         0x19        /* far pointer to the next DPB           */
#define DPB_FREESEARCH   0x1D
#define DPB_FREECOUNT    0x1F        /* FFFF = unknown, which is what 6.22 had */
#define DPB_LEN          0x21        /* 33 -- confirmed by 0x136A + 0x21 = 0x138B */

/* ---- CDS (DOS 4.0+), 88 bytes. Offsets measured from sysvars.cds0. */
#define CDS_PATH         0x00        /* 67 bytes ASCIZ, e.g. "A:\"            */
#define CDS_FLAGS        0x43
#define CDS_DPB          0x45        /* far pointer to this drive's DPB       */
#define CDS_CLUSTER      0x49
#define CDS_UNKNOWN      0x4B        /* 6.22 leaves FFFFFFFF here             */
#define CDS_SLASH        0x4F        /* offset of the root backslash: 2       */
#define CDS_LEN          0x58        /* 88 -- 'B:\' begins exactly here       */

/* Measured flag: bit 14 set = the drive is PHYSICAL/local. 6.22 had 0x4000 on
   A:, and a drive letter with nothing behind it gets 0. */
#define CDS_FLAG_PHYSICAL 0x4000

static void sv_w(unsigned char *p, unsigned o, unsigned v)
{
    p[o] = (unsigned char)(v & 0xFF);
    p[o + 1] = (unsigned char)((v >> 8) & 0xFF);
}

static void sv_far(unsigned char *p, unsigned o, unsigned seg, unsigned off)
{
    sv_w(p, o, off);
    sv_w(p, o + 2, seg);
}

/* Build one DPB. `next_seg/next_off` link it on; pass 0xFFFF/0xFFFF to end the
   chain -- a chain that does not terminate is how krnl386 once walked the IVT
   forever (see DOS_SFT_* in dos_layout.h), and a DPB chain can do the same.
 ⚠ WHICH FIELDS ARE REAL, AND WHICH ARE NOT. Say it here rather than let a
   caller discover it. GetDiskFreeSpace gives us bytes/sector, sectors/cluster
   and the cluster count, so DPB_SECSIZE, DPB_CLUSTMAX, DPB_CLUSTSHIFT and
   DPB_CLUSTHIGH are MEASURED off the real volume. The rest of the FAT geometry
   -- DPB_RESERVED, DPB_NFATS, DPB_ROOTENTS, DPB_DATASTART, DPB_FATSECS -- is
   only in the boot sector, which we cannot read: this host exposes no raw
   sectors at all (INT 13h and INT 25h/26h both report absent, GH #44). So
   DATASTART and FATSECS are left ZERO and the others take FAT16 defaults.
   That is a real limitation, not a rounding: a program that walks a DPB to
   locate the FAT gets nothing usable. It cannot do anything with the answer
   either, having no way to read the sectors, so the chain is useful for what
   actually walks it (memory and drive enumeration) and honest about the rest.
   Filling DATASTART/FATSECS with plausible numbers would be strictly worse --
   that is the MEM.EXE failure (GH #47) in a different structure. */
static void dos_dpb_build(unsigned char *p, unsigned drive,
                          unsigned bytes_per_sec, unsigned secs_per_clust,
                          unsigned root_ents, unsigned highest_clust,
                          unsigned media, unsigned devhdr_seg, unsigned devhdr_off,
                          unsigned next_seg, unsigned next_off)
{
    unsigned i, shift = 0, n = secs_per_clust;
    for (i = 0; i < DPB_LEN; ++i) p[i] = 0;
    while (n > 1) { n >>= 1; ++shift; }
    p[DPB_DRIVE] = (unsigned char)drive;
    p[DPB_UNIT]  = (unsigned char)drive;
    sv_w(p, DPB_SECSIZE, bytes_per_sec);
    /* ⚠ MINUS ONE. 6.22's floppy DPB has 0 here with one sector per cluster --
       the field is the highest sector INDEX in a cluster, not the count. */
    p[DPB_CLUSTMAX]    = (unsigned char)(secs_per_clust ? secs_per_clust - 1 : 0);
    p[DPB_CLUSTSHIFT]  = (unsigned char)shift;
    sv_w(p, DPB_RESERVED, 1);
    p[DPB_NFATS] = 2;
    sv_w(p, DPB_ROOTENTS, root_ents);
    sv_w(p, DPB_CLUSTHIGH, highest_clust);
    sv_far(p, DPB_DEVHDR, devhdr_seg, devhdr_off);
    p[DPB_MEDIA] = (unsigned char)media;
    p[DPB_ACCESSED] = 0;
    sv_far(p, DPB_NEXT, next_seg, next_off);
    /* FFFF = "free cluster count unknown", which is exactly what 6.22 reported
       (sysvars.dpb0 has FF FF at +0x1F). Claiming a number we have not counted
       would be the MEM.EXE failure mode in a different structure. */
    sv_w(p, DPB_FREECOUNT, 0xFFFF);
}

/* Build one CDS entry for drive 0..25. `exists` decides the flags word: a drive
   letter with nothing behind it gets 0, which is how DOS marks an unused slot in
   an array that is always LASTDRIVE entries long. */
static void dos_cds_build(unsigned char *p, unsigned drive, int exists,
                          unsigned dpb_seg, unsigned dpb_off)
{
    unsigned i;
    for (i = 0; i < CDS_LEN; ++i) p[i] = 0;
    p[CDS_PATH + 0] = (unsigned char)('A' + drive);
    p[CDS_PATH + 1] = ':';
    p[CDS_PATH + 2] = '\\';
    sv_w(p, CDS_FLAGS, exists ? CDS_FLAG_PHYSICAL : 0);
    if (exists) sv_far(p, CDS_DPB, dpb_seg, dpb_off);
    else        sv_far(p, CDS_DPB, 0xFFFF, 0xFFFF);
    sv_w(p, CDS_UNKNOWN, 0xFFFF);
    sv_w(p, CDS_UNKNOWN + 2, 0xFFFF);
    sv_w(p, CDS_SLASH, 2);              /* "A:\" -- the backslash is at index 2 */
}

/* Build the NUL device header, inline at SysVars+0x22. `next` terminates the
   device chain: we install no block or character drivers, so FFFF:FFFF is the
   truthful answer and stops any walker cleanly. Attribute 0x8004 is 6.22's
   (bit 15 = character device, bit 2 = NUL). */
static void dos_nul_build(unsigned char *p, unsigned next_seg, unsigned next_off)
{
    static const char name[8] = { 'N','U','L',' ',' ',' ',' ',' ' };
    unsigned i;
    sv_far(p, 0x00, next_seg, next_off);
    sv_w(p, 0x04, 0x8004);
    sv_w(p, 0x06, 0);                   /* strategy entry -- none to call      */
    sv_w(p, 0x08, 0);                   /* interrupt entry -- none to call     */
    for (i = 0; i < 8; ++i) p[0x0A + i] = (unsigned char)name[i];
}

#endif /* DOS_SYSVARS_H */
