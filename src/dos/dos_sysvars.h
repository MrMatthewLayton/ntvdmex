/* dos_sysvars.h -- INT 21h AH=52h's List of Lists, built to the MEASURED layout.
 *
 * GH #48. AH=52h returns ES:BX pointing INTO this structure; the word at ES:BX-2
 * is the first MCB segment and everything from ES:BX on is DOS's own bookkeeping.
 *
 * ── EVERY OFFSET BELOW CAME OFF THE ORACLE, NOT OUT OF A BOOK ────────────────
 * #48 is explicit that these structures "must not be written from documentation
 * alone -- the layout dumps have twice caught errors that a plausible reading
 * would have missed". So tests/probes/dos/p_sysvar.asm dumped them from genuine
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
 * Pure -- byte buffers and integers only -- so tests/unit/sysvars_test.c can
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
/* Bit 15 = the drive is served by a redirector (network, and MSCDEX's CD-ROMs
   look the same to DOS). Such entries carry no DPB: there is no FAT to walk. */
#define CDS_FLAG_NETWORK  0x8000

/* static INLINE: dos_int21.c includes this for AH=1Fh/32h's DPB (#48) and uses one
   builder of the five, and a plain `static` would warn for every unused one. */
static inline void sv_w(unsigned char *p, unsigned o, unsigned v)
{
    p[o] = (unsigned char)(v & 0xFF);
    p[o + 1] = (unsigned char)((v >> 8) & 0xFF);
}

static inline void sv_far(unsigned char *p, unsigned o, unsigned seg, unsigned off)
{
    sv_w(p, o, off);
    sv_w(p, o + 2, seg);
}

/* ── #48: THE FAT LAYOUT A DPB DESCRIBES, DERIVED -- AND CHECKED AGAINST 6.22. ──────
     A DPB carries four numbers that only a boot sector knows: reserved sectors, FAT
     count, sectors per FAT and the root directory's size -- and two it derives from
     them: the root's first sector and the first data sector. This host has no boot
     sector to read (no raw sectors at all: INT 13h, 25h, 26h report absent, GH #44).
     These had been left ZERO, on the argument that a plausible number is the MEM.EXE
     failure in another structure. Zero is not neutral either, though: FATSECS=0 with
     DATASTART=0 describes a volume whose data area starts at the boot sector, i.e. the
     FAT, the root and the files all overlapping -- a structure no FORMAT can produce,
     and a divisor of zero for anything that sizes the FAT from it.
   ⇒ So the layout is DERIVED, by FAT's own rules, from what IS measured (bytes per
     sector, sectors per cluster, highest cluster -- GetDiskFreeSpace) plus the fixed
     choices FORMAT makes (1 reserved sector, 2 FATs, the root size the caller passes):
         FAT12 if clusters < 4085 (highest <= 0xFF5), else FAT16
         sectors/FAT = ceil((highest + 1) entries * 1.5 or 2 bytes / bytes per sector)
         root start  = reserved + FATs * sectors/FAT
         data start  = root start + ceil(root entries * 32 / bytes per sector)
   ★ THE CHECK THAT THIS IS DOS'S ARITHMETIC AND NOT A GUESS: fed 6.22's own floppy
     (512 B, 1 sector/cluster, 224 root entries, highest cluster 2848 -- sysvars.dpb0)
     it reproduces that DPB's FAT=9, root start=19, data start=33 exactly, and
     tests/unit/sysvars_test.c holds the whole 33-byte build to the oracle's bytes.
   ⚠ WHAT IT IS NOT: for a FIXED disk the numbers describe a self-consistent FAT16
     volume of the measured size, NOT the real volume (NTFS, or a FAT whose boot
     sector we never read). No sector they name can be read here, so a program that
     follows them to the FAT gets the same INT 25h refusal it got before -- what it no
     longer gets is a structurally impossible DPB. Unmeasured against a 6.22 HARD disk
     DPB; tests/probes/dos/p_devchn.asm's `dpb.c.layout` row is the check. */
static inline void dos_dpb_fat_layout(unsigned bytes_per_sec, unsigned root_ents,
                                      unsigned highest_clust, unsigned *fatsecs,
                                      unsigned *rootstart, unsigned *datastart)
{
    unsigned long bps = bytes_per_sec ? bytes_per_sec : 512;
    unsigned long ents = (unsigned long)highest_clust + 1;          /* clusters 0..highest */
    unsigned long fatbytes = (highest_clust <= 0xFF5) ? (ents * 3 + 1) / 2 : ents * 2;
    unsigned long fs = (fatbytes + bps - 1) / bps;
    unsigned long rs = 1 + 2 * fs;                                  /* reserved + 2 FATs   */
    unsigned long ds = rs + ((unsigned long)root_ents * 32 + bps - 1) / bps;
    *fatsecs = (unsigned)fs; *rootstart = (unsigned)rs; *datastart = (unsigned)ds;
}

/* Build one DPB. `next_seg/next_off` link it on; pass 0xFFFF/0xFFFF to end the
   chain -- a chain that does not terminate is how krnl386 once walked the IVT
   forever (see DOS_SFT_* in dos_layout.h), and a DPB chain can do the same.
 ⚠ WHICH FIELDS ARE REAL, AND WHICH ARE NOT. Say it here rather than let a
   caller discover it. GetDiskFreeSpace gives us bytes/sector, sectors/cluster
   and the cluster count, so DPB_SECSIZE, DPB_CLUSTMAX, DPB_CLUSTSHIFT and
   DPB_CLUSTHIGH are MEASURED off the real volume. The rest of the FAT geometry
   -- DPB_RESERVED, DPB_NFATS, DPB_ROOTENTS, DPB_DATASTART, DPB_FATSECS,
   DPB_ROOTSTART -- is only in the boot sector, which we cannot read, so it is
   DERIVED by dos_dpb_fat_layout above (#48; was zero -- see there for why zero
   was not the honest answer it looked like). */
static inline void dos_dpb_build(unsigned char *p, unsigned drive,
                          unsigned bytes_per_sec, unsigned secs_per_clust,
                          unsigned root_ents, unsigned highest_clust,
                          unsigned media, unsigned devhdr_seg, unsigned devhdr_off,
                          unsigned next_seg, unsigned next_off)
{
    unsigned i, shift = 0, n = secs_per_clust, fs, rs, ds;
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
    dos_dpb_fat_layout(bytes_per_sec, root_ents, highest_clust, &fs, &rs, &ds);
    sv_w(p, DPB_DATASTART, ds);
    sv_w(p, DPB_FATSECS, fs);
    sv_w(p, DPB_ROOTSTART, rs);
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

/* Build one CDS entry for drive 0..25. `flags` is the flags word: 0 for a drive
   letter with nothing behind it (how DOS marks an unused slot in an array that is
   always LASTDRIVE entries long), CDS_FLAG_PHYSICAL for a local drive with a DPB,
   CDS_FLAG_PHYSICAL|CDS_FLAG_NETWORK for a redirected one (no DPB). */
static inline void dos_cds_build(unsigned char *p, unsigned drive, unsigned flags,
                          unsigned dpb_seg, unsigned dpb_off)
{
    unsigned i;
    for (i = 0; i < CDS_LEN; ++i) p[i] = 0;
    p[CDS_PATH + 0] = (unsigned char)('A' + drive);
    p[CDS_PATH + 1] = ':';
    p[CDS_PATH + 2] = '\\';
    sv_w(p, CDS_FLAGS, flags);
    if (!flags)                          sv_far(p, CDS_DPB, 0xFFFF, 0xFFFF);
    else if (flags & CDS_FLAG_NETWORK)   sv_far(p, CDS_DPB, 0x0000, 0x0000);
    else                                 sv_far(p, CDS_DPB, dpb_seg, dpb_off);
    sv_w(p, CDS_UNKNOWN, 0xFFFF);
    sv_w(p, CDS_UNKNOWN + 2, 0xFFFF);
    sv_w(p, CDS_SLASH, 2);              /* "A:\" -- the backslash is at index 2 */
}

/* ── #48: THE DEVICE DRIVER CHAIN. ─────────────────────────────────────────────────
     NUL (inline in SysVars) used to TERMINATE the chain: "we install no drivers". But
     DOS's own drivers are not installed, they are IO.SYS, and every DOS has them --
     MEM /D on 6.22 lists, in chain order (runs/s81_mem/oracle_memd.txt):
         CON AUX PRN CLOCK$ "A: - C:" COM1 LPT1 LPT2 LPT3 COM2 COM3 COM4
     and the order of the first five is pinned by measured pointers, not by that
     listing alone: SysVars+0x0C = CON at 0070:0023, +0x08 = CLOCK$ at 0070:0059, the
     6.22 SFT's AUX/PRN entries name 0070:0035 and 0070:0047 (lolprobe-msdos622.txt),
     and its floppy DPB names the block driver at 0070:006B -- five 18-byte headers
     back to back. So NUL now links to the same twelve, in that order, and the last
     terminates (FFFF:FFFF).
   ⚠ THE ATTRIBUTE WORDS ARE NOT MEASURED. They are the documented IO.SYS values
     (RBIL "Format of device driver header", and the bits each device's behaviour
     implies): CON 8013h (char | fast INT 29h output -- we serve INT 29h -- | stdout |
     stdin), AUX/COMn 8000h, PRN/LPTn A0C0h (char | output-until-busy | IOCTL query |
     generic IOCTL), CLOCK$ 8008h, the block driver 08C2h with byte 0 of its name = the
     unit count. tests/probes/dos/p_devchn.asm reads every one of them back off the oracle.
   ⚠ AND NONE OF THEM CAN BE CALLED AS A DRIVER. Our devices are served by the INT 21h
     layer, not by request packets. A program that calls a header's strategy/interrupt
     pair directly (rare: a few TSRs and diagnostics do) gets status 8103h -- error,
     done, "unknown command" -- written by the strategy entry, and a bare RETF from the
     interrupt entry. That is a truthful refusal; an entry of 0000 (what NUL had) is a
     far call into whatever the segment holds at offset 0. */
#define DEVHDR_NEXT      0x00
#define DEVHDR_ATTR      0x04
#define DEVHDR_STRAT     0x06
#define DEVHDR_INTR      0x08
#define DEVHDR_NAME      0x0A
#define DEVHDR_LEN       0x12        /* 18, as SV_NUL_LEN -- the stride 6.22's are at */
enum { DEV_CON, DEV_AUX, DEV_PRN, DEV_CLOCK, DEV_BLOCK, DEV_COM1, DEV_LPT1, DEV_LPT2,
       DEV_LPT3, DEV_COM2, DEV_COM3, DEV_COM4, DEV_COUNT };
#define DEV_OFF(i)       ((unsigned)(i) * DEVHDR_LEN)         /* 0x00 .. 0xC6        */
/* The entry stubs, after the headers (12 x 18 = 0xD8 bytes). Strategy routines are
   FAR-called with ES:BX = the request header, whose status word is at +3. */
#define DEV_STUB_UNKNOWN 0xE0        /* mov word [es:bx+3],8103h ; retf -- 7 bytes   */
#define DEV_STUB_RETF    0xE7        /* retf -- every interrupt entry                */
#define DEV_AREA_LEN     0xE8
#define DEV_ATTR_CON     0x8013
#define DEV_ATTR_AUX     0x8000
#define DEV_ATTR_PRN     0xA0C0
#define DEV_ATTR_CLOCK   0x8008
#define DEV_ATTR_BLOCK   0x08C2
#define DEV_ATTR_NUL     0x8004      /* measured: 6.22's NUL header, sysvars_test.c  */

static inline void dos_devhdr_build(unsigned char *p, unsigned next_seg, unsigned next_off,
                                    unsigned attr, unsigned strat, unsigned intr,
                                    const char *name8)
{
    unsigned i;
    sv_far(p, DEVHDR_NEXT, next_seg, next_off);
    sv_w(p, DEVHDR_ATTR, attr);
    sv_w(p, DEVHDR_STRAT, strat);
    sv_w(p, DEVHDR_INTR, intr);
    for (i = 0; i < 8; ++i) p[DEVHDR_NAME + i] = (unsigned char)name8[i];
}

/* The DOS_DEV_SEG area, DEV_AREA_LEN bytes: twelve headers linked in 6.22's order and
   terminated, then the stubs. `seg` is the segment the area will live at (each `next`
   names it); `units` goes in the block driver's name byte 0 (= SysVars+0x20). */
static inline void dos_devchain_build(unsigned char *p, unsigned seg, unsigned units)
{
    static const struct { unsigned attr; char name[9]; } dev[DEV_COUNT] = {
        { DEV_ATTR_CON,   "CON     " }, { DEV_ATTR_AUX,   "AUX     " },
        { DEV_ATTR_PRN,   "PRN     " }, { DEV_ATTR_CLOCK, "CLOCK$  " },
        { DEV_ATTR_BLOCK, "" },
        { DEV_ATTR_AUX,   "COM1    " }, { DEV_ATTR_PRN,   "LPT1    " },
        { DEV_ATTR_PRN,   "LPT2    " }, { DEV_ATTR_PRN,   "LPT3    " },
        { DEV_ATTR_AUX,   "COM2    " }, { DEV_ATTR_AUX,   "COM3    " },
        { DEV_ATTR_AUX,   "COM4    " },
    };
    static const unsigned char stubs[] = {
        0x26, 0xC7, 0x47, 0x03, 0x03, 0x81, 0xCB,        /* DEV_STUB_UNKNOWN */
        0xCB,                                            /* DEV_STUB_RETF    */
    };
    unsigned i;
    for (i = 0; i < DEV_AREA_LEN; ++i) p[i] = 0;
    for (i = 0; i < DEV_COUNT; ++i) {
        int last = (i + 1 == DEV_COUNT);
        /* the block driver's name is 8 zero bytes; char[9] "" zero-fills the rest */
        dos_devhdr_build(p + DEV_OFF(i), last ? 0xFFFF : seg,
                         last ? 0xFFFF : DEV_OFF(i + 1), dev[i].attr,
                         DEV_STUB_UNKNOWN, DEV_STUB_RETF, dev[i].name);
    }
    p[DEV_OFF(DEV_BLOCK) + DEVHDR_NAME] = (unsigned char)units;
    for (i = 0; i < sizeof stubs; ++i) p[DEV_STUB_UNKNOWN + i] = stubs[i];
}

/* NUL's own two entries live in ITS segment (a header's strategy/interrupt are
   offsets in the header's own segment, and NUL's is SysVars'): `mov word [es:bx+3],
   0100h ; retf` then `retf`, 8 bytes at DOS_SYSVARS_SEG:DOS_NULSTUB_OFF. NUL accepts
   every request and does nothing, which is what "done, no error" says. */
#define DOS_NULSTUB_LEN   8
#define DOS_NULSTUB_STRAT 0          /* offsets within the 8 bytes */
#define DOS_NULSTUB_INTR  7
static inline void dos_nulstub_build(unsigned char *p)
{
    static const unsigned char s[DOS_NULSTUB_LEN] =
        { 0x26, 0xC7, 0x47, 0x03, 0x00, 0x01, 0xCB, 0xCB };
    unsigned i;
    for (i = 0; i < DOS_NULSTUB_LEN; ++i) p[i] = s[i];
}

/* Build the NUL device header, inline at SysVars+0x22. Attribute 0x8004 is 6.22's
   (bit 15 = character device, bit 2 = NUL). `next` links on to CON (#48; it
   TERMINATED here, FFFF:FFFF, while no other header existed -- see above). */
static inline void dos_nul_build(unsigned char *p, unsigned next_seg, unsigned next_off,
                                 unsigned strat, unsigned intr)
{
    dos_devhdr_build(p, next_seg, next_off, DEV_ATTR_NUL, strat, intr, "NUL     ");
}

#endif /* DOS_SYSVARS_H */
