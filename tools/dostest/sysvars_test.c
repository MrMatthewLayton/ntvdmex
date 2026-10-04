/* sysvars_test.c -- the List of Lists layout, pinned against the 6.22 dump.  GH #48.
 *
 * The expectations here are not read from documentation. Each one is decoded from
 * a BUF= line that tools/dostest/p_sysvar.asm brought back from genuine MS-DOS
 * 6.22, and the raw dump is embedded below so the decoding can be re-checked
 * rather than trusted. #48 asks for exactly this: "the structures are
 * version-specific and must not be written from documentation alone -- the layout
 * dumps have twice caught errors that a plausible reading would have missed".
 *
 *   cc -std=c99 -I src/dos -o sysvars_test tools/dostest/sysvars_test.c
 */
#include <stdio.h>
#include <string.h>
#include "dos_sysvars.h"
#include "dos_layout.h"

static int checks, fails;

static void eq(const char *what, long got, long want)
{
    ++checks;
    if (got == want) return;
    ++fails;
    printf("  FAIL %-54s got 0x%lX, want 0x%lX\n", what, got, want);
}

/* ── THE ORACLE'S OWN BYTES. ─────────────────────────────────────────────────
   BUF=sysvars.raw, starting two bytes BEFORE what AH=52h returned in ES:BX. */
static const unsigned char ORACLE_RAW[] = {
 0x53,0x02,                                     /* ES:BX-2 first MCB segment    */
 0x6A,0x13,0x16,0x01,  0xCC,0x00,0x16,0x01,     /* +00 DPB      +04 SFT         */
 0x59,0x00,0x70,0x00,  0x23,0x00,0x70,0x00,     /* +08 CLOCK$   +0C CON         */
 0x00,0x02,                                     /* +10 max bytes/sector         */
 0x6D,0x00,0x16,0x01,                           /* +12 disk buffers             */
 0x00,0x00,0x50,0x03,                           /* +16 CDS array                */
 0x00,0x00,0x1E,0x03,                           /* +1A FCB table                */
 0x00,0x00,                                     /* +1E FCB keep count           */
 0x03,                                          /* +20 block devices            */
 0x05,                                          /* +21 LASTDRIVE                */
 0x00,0x00,0x55,0x02, 0x04,0x80, 0xC6,0x0D, 0xCC,0x0D,   /* +22 NUL header      */
 0x4E,0x55,0x4C,0x20,0x20,0x20,0x20,0x20,       /*     "NUL     "               */
};
/* BUF=sysvars.dpb0 -- the first DPB, at 0116:136A. */
static const unsigned char ORACLE_DPB[] = {
 0x00,0x00,0x00,0x02,0x00,0x00,0x01,0x00,0x02,0xE0,0x00,0x21,0x00,0x20,0x0B,0x09,
 0x00,0x13,0x00,0x6B,0x00,0x70,0x00,0xF0,0x00,0x8B,0x13,0x16,0x01,0x00,0x00,0xFF,
 0xFF,
 /* and the NEXT DPB begins immediately here, which is how DPB_LEN is known: */
 0x01,0x01,0x00,0x02,0xFE,0x00,0x01,0x00,0x02,0x40,0x00,0x09,0x00,0x60,0x01,
};

#define SV(o) (ORACLE_RAW[(o) + 2])                 /* SysVars+o                */
static unsigned svw(int o) { return SV(o) | (SV(o + 1) << 8); }

int main(void)
{
    unsigned char buf[256];
    printf("== INT 21h AH=52h List of Lists (dos_sysvars.h) -- decoded from 6.22\n");

    /* ── THE FIELD OFFSETS. Each check reads the oracle's own bytes at the
       offset our header declares and asserts the value we decoded. If an offset
       is wrong, the value read there will not be the one 6.22 reported. */
    eq("ES:BX-2 is the first MCB segment (0x0253)",
       ORACLE_RAW[0] | (ORACLE_RAW[1] << 8), 0x0253);
    eq("+00 DPB chain offset  = 0x136A", svw(SV_DPB), 0x136A);
    eq("+02 DPB chain segment = 0x0116", svw(SV_DPB + 2), 0x0116);
    eq("+04 SFT chain offset  = 0x00CC", svw(SV_SFT), 0x00CC);
    eq("+08 CLOCK$ device     = 0070:0059", svw(SV_CLOCK + 2), 0x0070);
    eq("+0C CON device        = 0070:0023", svw(SV_CON + 2), 0x0070);
    eq("+10 max bytes/sector  = 512", svw(SV_MAXSEC), 512);
    eq("+16 CDS array segment = 0x0350", svw(SV_CDS + 2), 0x0350);
    eq("+1A FCB table segment = 0x031E", svw(SV_FCB + 2), 0x031E);
    eq("+20 block devices     = 3", SV(SV_NBLOCKDEV), 3);
    eq("+21 LASTDRIVE         = 5", SV(SV_LASTDRIVE), 5);

    /* ★ THE NUL HEADER IS INLINE, NOT A POINTER. The check that proves the
       offset is right is the NAME: 'NUL     ' has to land where we say it does.
       If SV_NUL were wrong by even a byte this reads as garbage. */
    ++checks;
    if (memcmp(&SV(SV_NUL) + 0x0A, "NUL     ", 8) != 0) {
        ++fails; printf("  FAIL %-54s\n", "+22 is the NUL header (name mismatch)");
    }
    eq("+22 NUL attribute = 0x8004", svw(SV_NUL + 4), 0x8004);
    eq("NUL header is 18 bytes", SV_NUL_LEN, 0x12);
    eq("SysVars proper is 0x34 bytes", SV_LEN, 0x34);

    /* ── THE DPB. Its length is a MEASUREMENT, not a recollection: 6.22's first
       DPB sits at 0116:136A and its own next-pointer says 0116:138B. */
    eq("DPB next pointer = 0x138B",
       ORACLE_DPB[DPB_NEXT] | (ORACLE_DPB[DPB_NEXT + 1] << 8), 0x138B);
    eq("...so DPB_LEN = 0x138B - 0x136A = 33", DPB_LEN, 0x138B - 0x136A);
    eq("DPB +02 bytes/sector = 512",
       ORACLE_DPB[DPB_SECSIZE] | (ORACLE_DPB[DPB_SECSIZE + 1] << 8), 512);
    /* ⚠ The field is the highest sector INDEX in a cluster, not the count: a
       1.44MB floppy has one sector per cluster and 6.22 stores 0, not 1. */
    eq("DPB +04 is sectors/cluster MINUS ONE (0)", ORACLE_DPB[DPB_CLUSTMAX], 0);
    eq("DPB +08 number of FATs = 2", ORACLE_DPB[DPB_NFATS], 2);
    eq("DPB +09 root entries = 224",
       ORACLE_DPB[DPB_ROOTENTS] | (ORACLE_DPB[DPB_ROOTENTS + 1] << 8), 224);
    eq("DPB +0F sectors per FAT = 9 (a WORD on DOS 4+)",
       ORACLE_DPB[DPB_FATSECS] | (ORACLE_DPB[DPB_FATSECS + 1] << 8), 9);
    eq("DPB +17 media descriptor = 0xF0 (1.44MB)", ORACLE_DPB[DPB_MEDIA], 0xF0);
    eq("DPB +1F free cluster count = FFFF (unknown)",
       ORACLE_DPB[DPB_FREECOUNT] | (ORACLE_DPB[DPB_FREECOUNT + 1] << 8), 0xFFFF);
    /* The next DPB is for drive B: -- which is what makes the 33-byte stride
       real rather than arithmetic that happens to work once. */
    eq("the next DPB is drive 1 (B:)", ORACLE_DPB[DPB_LEN + DPB_DRIVE], 1);

    /* ── THE CDS. 'B:\' began exactly 88 bytes after 'A:\' in the dump. */
    eq("CDS_LEN = 88", CDS_LEN, 88);
    eq("CDS flags at +0x43", CDS_FLAGS, 0x43);
    eq("CDS DPB pointer at +0x45", CDS_DPB, 0x45);
    eq("CDS backslash offset field at +0x4F", CDS_SLASH, 0x4F);

    /* ── AND WHAT WE BUILD MUST MATCH THAT SHAPE. */
    memset(buf, 0xAA, sizeof(buf));
    dos_cds_build(buf, 2 /* C: */, CDS_FLAG_PHYSICAL, 0x1234, 0x0040);
    ++checks;
    if (memcmp(buf, "C:\\", 4) != 0) {
        ++fails; printf("  FAIL %-54s\n", "built CDS path is 'C:\\'");
    }
    eq("built CDS flags = 0x4000 (physical)",
       buf[CDS_FLAGS] | (buf[CDS_FLAGS + 1] << 8), CDS_FLAG_PHYSICAL);
    eq("built CDS DPB segment", buf[CDS_DPB + 2] | (buf[CDS_DPB + 3] << 8), 0x1234);
    eq("built CDS backslash offset = 2",
       buf[CDS_SLASH] | (buf[CDS_SLASH + 1] << 8), 2);
    /* A drive letter with nothing behind it: flags 0, and the DPB pointer must
       TERMINATE rather than dangle at whatever the array's base happens to be. */
    dos_cds_build(buf, 25 /* Z: */, 0, 0x1234, 0x0040);
    eq("absent drive: flags = 0", buf[CDS_FLAGS] | (buf[CDS_FLAGS + 1] << 8), 0);
    eq("absent drive: DPB pointer is FFFF (terminated, not dangling)",
       buf[CDS_DPB + 2] | (buf[CDS_DPB + 3] << 8), 0xFFFF);
    /* A redirected drive (network share, MSCDEX CD-ROM): physical|network, no DPB. */
    dos_cds_build(buf, 25 /* Z: */, CDS_FLAG_PHYSICAL | CDS_FLAG_NETWORK, 0x1234, 0x0040);
    eq("network drive: flags = 0xC000", buf[CDS_FLAGS] | (buf[CDS_FLAGS + 1] << 8), 0xC000);
    eq("network drive: DPB pointer is 0000 (no FAT behind a redirector)",
       buf[CDS_DPB + 2] | (buf[CDS_DPB + 3] << 8), 0x0000);

    memset(buf, 0xAA, sizeof(buf));
    dos_dpb_build(buf, 2, 512, 8, 512, 0x1000, 0xF8, 0x50, 0x90, 0xFFFF, 0xFFFF);
    eq("built DPB: 8 sectors/cluster stores 7", buf[DPB_CLUSTMAX], 7);
    eq("built DPB: ...and a shift of 3", buf[DPB_CLUSTSHIFT], 3);
    eq("built DPB: chain terminates at FFFF",
       buf[DPB_NEXT + 2] | (buf[DPB_NEXT + 3] << 8), 0xFFFF);
    eq("built DPB: free count is FFFF, never a number we did not count",
       buf[DPB_FREECOUNT] | (buf[DPB_FREECOUNT + 1] << 8), 0xFFFF);

    /* ── ★ #48: THE DERIVED FAT LAYOUT REPRODUCES 6.22's OWN FLOPPY DPB, ALL 33 BYTES.
         Fed only what a 1.44M floppy's GetDiskFreeSpace + FORMAT's fixed choices give
         (512 B/sector, 1 sector/cluster, 224 root entries, highest cluster 2848, media
         F0) plus the oracle's own pointers (device 0070:006B, next 0116:138B), the
         builder must produce the oracle's bytes exactly -- FAT=9, root start=19, data
         start=33 included. That is what makes the derivation DOS's arithmetic and not a
         plausible guess; the fixed-disk values follow the same rules. */
    memset(buf, 0xAA, sizeof(buf));
    dos_dpb_build(buf, 0, 512, 1, 224, 0x0B20, 0xF0, 0x0070, 0x006B, 0x0116, 0x138B);
    {   int k, bad = -1;
        for (k = 0; k < DPB_LEN; ++k) if (buf[k] != ORACLE_DPB[k]) { bad = k; break; }
        ++checks;
        if (bad >= 0) {
            ++fails;
            printf("  FAIL %-54s at +0x%02X: got 0x%02X, want 0x%02X\n",
                   "built floppy DPB == 6.22's sysvars.dpb0, byte for byte", bad,
                   buf[bad], ORACLE_DPB[bad]);
        }
    }
    eq("oracle DPB +0B data start = 33", ORACLE_DPB[DPB_DATASTART] | (ORACLE_DPB[DPB_DATASTART + 1] << 8), 33);
    eq("oracle DPB +11 root start = 19", ORACLE_DPB[DPB_ROOTSTART] | (ORACLE_DPB[DPB_ROOTSTART + 1] << 8), 19);
    {   unsigned fs, rs, ds;
        /* A fixed disk clamped to 0xFFFE clusters: FAT16, 65535 entries x 2 bytes. */
        dos_dpb_fat_layout(512, 512, 0xFFFE, &fs, &rs, &ds);
        eq("FAT16 (highest 0xFFFE): 256 sectors per FAT", fs, 256);
        eq("FAT16: root starts after 1 reserved + 2 FATs = 513", rs, 513);
        eq("FAT16: data starts after 512 root entries (32 sectors) = 545", ds, 545);
        /* The FAT12/FAT16 line is 4085 clusters: highest 0xFF5 is FAT12, 0xFF6 FAT16. */
        dos_dpb_fat_layout(512, 512, 0xFF5, &fs, &rs, &ds);
        eq("highest 0xFF5 is FAT12: ceil(4086*1.5/512) = 12 sectors", fs, 12);
        dos_dpb_fat_layout(512, 512, 0xFF6, &fs, &rs, &ds);
        eq("highest 0xFF6 is FAT16: ceil(4087*2/512) = 16 sectors", fs, 16);
        /* A 4 KB-sector volume: the root (512 x 32 = 16 KB) is 4 sectors. */
        dos_dpb_fat_layout(4096, 512, 0x8000, &fs, &rs, &ds);
        eq("4096 B/sector: data start = root start + 4", ds - rs, 4);
        /* A zero sector size must not divide by zero; it is read as 512. */
        dos_dpb_fat_layout(0, 224, 0x0B20, &fs, &rs, &ds);
        eq("bytes/sector 0 is taken as 512 (the floppy again: data start 33)", ds, 33);
    }

    /* ── #48: THE DEVICE CHAIN. Order and stride are MEASURED (6.22: CON 0070:0023,
         AUX :0035, PRN :0047, CLOCK$ :0059, the block driver :006B -- 18 bytes apart);
         the attribute words are RBIL's and are not (p_devchn.asm is the check). */
    eq("6.22: AUX - CON = one header (0x35-0x23)", 0x35 - 0x23, DEV_OFF(DEV_AUX) - DEV_OFF(DEV_CON));
    eq("6.22: PRN - CON = two headers (0x47-0x23)", 0x47 - 0x23, DEV_OFF(DEV_PRN) - DEV_OFF(DEV_CON));
    eq("6.22: CLOCK$ - CON = three headers (+08 0x59 - +0C 0x23)",
       svw(SV_CLOCK) - svw(SV_CON), DEV_OFF(DEV_CLOCK) - DEV_OFF(DEV_CON));
    eq("6.22: the floppy DPB's block driver - CON = four headers (0x6B-0x23)",
       (ORACLE_DPB[DPB_DEVHDR] | (ORACLE_DPB[DPB_DEVHDR + 1] << 8)) - svw(SV_CON),
       DEV_OFF(DEV_BLOCK) - DEV_OFF(DEV_CON));
    eq("a device header is 18 bytes, as NUL's", DEVHDR_LEN, SV_NUL_LEN);
    memset(buf, 0xAA, sizeof(buf));
    dos_devchain_build(buf, 0x0060, 3);
    {   static const char *want[DEV_COUNT] = { "CON     ", "AUX     ", "PRN     ",
            "CLOCK$  ", 0, "COM1    ", "LPT1    ", "LPT2    ", "LPT3    ",
            "COM2    ", "COM3    ", "COM4    " };
        unsigned off = 0, seg = 0x0060, n = 0;
        /* walk it as a guest would: from CON, by the `next` pointers, until FFFF */
        for (;;) {
            const unsigned char *h = buf + off;
            unsigned nx = h[0] | (h[1] << 8), ns = h[2] | (h[3] << 8);
            unsigned at = h[4] | (h[5] << 8);
            ++checks;
            if (n >= DEV_COUNT) { ++fails; printf("  FAIL %-54s\n", "device chain runs past 12"); break; }
            if (want[n]) {
                if (memcmp(h + DEVHDR_NAME, want[n], 8) != 0 || !(at & 0x8000)) {
                    ++fails; printf("  FAIL device %u: name/char-attr wrong\n", n);
                }
            } else if ((at & 0x8000) || h[DEVHDR_NAME] != 3) {
                ++fails; printf("  FAIL %-54s\n", "block driver: attr bit 15 clear, name[0] = units");
            }
            eq("strategy entry = the 'unknown command' stub", h[6] | (h[7] << 8), DEV_STUB_UNKNOWN);
            eq("interrupt entry = the RETF stub", h[8] | (h[9] << 8), DEV_STUB_RETF);
            ++n;
            if (nx == 0xFFFF) break;
            eq("next segment is the area's own", ns, seg);
            off = nx;
            if (off + DEVHDR_LEN > DEV_STUB_UNKNOWN) { ++fails; printf("  FAIL next off 0x%X\n", off); break; }
        }
        eq("the walk visits exactly 12 headers, then FFFF", n, DEV_COUNT);
        eq("CON  attr 8013h", buf[DEV_OFF(DEV_CON) + 4] | (buf[DEV_OFF(DEV_CON) + 5] << 8), 0x8013);
        eq("PRN  attr A0C0h", buf[DEV_OFF(DEV_PRN) + 4] | (buf[DEV_OFF(DEV_PRN) + 5] << 8), 0xA0C0);
        eq("CLOCK$ attr 8008h (bit 3: the clock device)",
           buf[DEV_OFF(DEV_CLOCK) + 4] | (buf[DEV_OFF(DEV_CLOCK) + 5] << 8), 0x8008);
        /* the stubs: mov word [es:bx+3],8103h / retf, then retf */
        {   static const unsigned char st[] = { 0x26,0xC7,0x47,0x03,0x03,0x81,0xCB, 0xCB };
            ++checks;
            if (memcmp(buf + DEV_STUB_UNKNOWN, st, sizeof st) != 0) {
                ++fails; printf("  FAIL %-54s\n", "device stubs encode 8103h-and-RETF / RETF");
            }
            eq("RETF stub is the byte after the unknown-command stub", DEV_STUB_RETF, DEV_STUB_UNKNOWN + 7);
            eq("the area ends just past the RETF stub", DEV_AREA_LEN, DEV_STUB_RETF + 1);
            eq("headers end before the stubs", DEV_OFF(DEV_COUNT) <= DEV_STUB_UNKNOWN, 1);
        }
    }

    memset(buf, 0xAA, sizeof(buf));
    dos_nul_build(buf, 0x0060, DEV_OFF(DEV_CON), 0x10, 0x17);
    ++checks;
    if (memcmp(buf + 0x0A, "NUL     ", 8) != 0) {
        ++fails; printf("  FAIL %-54s\n", "built NUL header carries the name");
    }
    eq("built NUL attribute = 0x8004", buf[4] | (buf[5] << 8), 0x8004);
    eq("built NUL links on to CON (was FFFF:FFFF)", buf[2] | (buf[3] << 8), 0x0060);
    eq("built NUL strategy entry", buf[6] | (buf[7] << 8), 0x10);
    {   unsigned char ns[DOS_NULSTUB_LEN];
        static const unsigned char want[] = { 0x26,0xC7,0x47,0x03,0x00,0x01,0xCB,0xCB };
        dos_nulstub_build(ns);
        ++checks;
        if (memcmp(ns, want, sizeof want) != 0) {
            ++fails; printf("  FAIL %-54s\n", "NUL stubs encode done-0100h-and-RETF / RETF");
        }
        eq("NUL's interrupt entry is its RETF", ns[DOS_NULSTUB_INTR], 0xCB);
    }

    /* ── ⚠⚠ THE TWO ABSOLUTE OFFSETS MEM.EXE READS IN THE SYSVARS SEGMENT. ──────
         Neither is a List-of-Lists field: MEM keeps the SEGMENT AH=52h returns,
         throws the OFFSET away, and reads fixed addresses in it. Both have cost a
         session already, and both are pinned here because a future edit to
         DOS_SYSVARS_OFF or DOS_SDA_OFF would silently break them again.

         Byte-for-byte from MS-DOS 6.22 (docs/research/evidence/lolprobe-msdos622.txt,
         a dump of the SysVars SEGMENT from offset 0, where SysVars itself is at
         0x0026):
           0080: 00 FF FF 00 00 00 00 00 00 00 00 00 FF FF 53 02
                                                    ^^^^^ 0x8C = FFFF, no UMBs
                                                          ^^^^^ 0x8E = first MCB
         and 0x0253 is ALSO what 6.22 reports at SysVars-2 -- the same value in
         both places, which is what says 0x8C/0x8E are the "first UMB"/"first MCB"
         pair rather than two unrelated words. */
    eq("MEM's conventional/upper line lives at absolute 0x8C in the SysVars segment",
       DOS_UMBHEAD_OFF, 0x8C);
    eq("...and 0xFFFF there means NO block is upper (6.22's own value)",
       DOS_UMBHEAD_NONE, 0xFFFF);
    /* ── ★★ s81 (#47, MEM /C): 0x8C IS NOT "A FIXED ADDRESS BESIDE SysVars". It is
         SysVars+0x66, because 6.22 keeps SysVars at offset 0x26 of its segment -- the
         evidence dump above says so. MEM /C reads the same word RELATIVELY, and with
         SysVars at 0x90 it got the first MCB's reserved bytes (0000) instead. */
    eq("SysVars sits at 6.22's own offset in its segment", DOS_SYSVARS_OFF, 0x26);
    eq("...so absolute 0x8C and SysVars+0x66 are ONE field",
       DOS_UMBHEAD_OFF, DOS_SYSVARS_OFF + 0x66);
    eq("...and 0x8E (first MCB on 6.22) is SysVars+0x68",
       DOS_UMBHEAD_OFF + 2, DOS_SYSVARS_OFF + 0x68);
    {   /* linear ranges: SysVars (MCB word at -2 through DOS_SYSVARS_LEN) must hit
           nothing else we place in low memory. */
        unsigned sv0 = DOS_SYSVARS_SEG * 16u + DOS_SYSVARS_OFF - 2u;
        unsigned sv1 = DOS_SYSVARS_SEG * 16u + DOS_SYSVARS_OFF + DOS_SYSVARS_LEN;
        unsigned sda0 = DOS_SDA_SEG * 16u + DOS_SDA_OFF, sda1 = sda0 + DOS_SDA_LEN;
        /* ── ★ #207: SysVars' SEGMENT LIES BELOW THE FIRST MCB, AS ON 6.22 (0116 < 0253).
             MEM /D prints "MSDOS System Data" = SysVars seg .. first MCB; with the chain
             at 0x5F that was 0x5F - 0x72 paragraphs, printed as 4,294,96x. Every byte of
             SysVars (from its -2 word) and of the SDA must sit below the first MCB header. */
        eq("first MCB (ES:BX-2) above the SysVars segment",
           DOS_FIRST_MCB > DOS_SYSVARS_SEG, 1);
        eq("SysVars ends at or below the first MCB header", sv1 <= DOS_FIRST_MCB * 16u, 1);
        eq("the SDA ends at or below the first MCB header", sda1 <= DOS_FIRST_MCB * 16u, 1);
        eq("MEM /D's IO row (0070..SysVars seg) is not negative", DOS_SYSVARS_SEG >= 0x70, 1);
        {   /* #48: the device headers at DOS_DEV_SEG, and NUL's stub in SysVars' segment */
            unsigned d0 = DOS_DEV_SEG * 16u, d1 = d0 + DEV_AREA_LEN;
            unsigned n0 = DOS_SYSVARS_SEG * 16u + DOS_NULSTUB_OFF, n1 = n0 + DOS_NULSTUB_LEN;
            eq("device area clear of DOS_HDLR_SEG's 0x00..0xFF", d0 >= DOS_HDLR_SEG * 16u + 0x100u, 1);
            eq("device area ends below 0x700 (and [0x714])", d1 <= 0x700u, 1);
            eq("device area below the first MCB", d1 <= DOS_FIRST_MCB * 16u, 1);
            eq("device area clear of the env block", d1 <= DOS_ENV_SEG * 16u, 1);
            eq("NUL stub clear of the kernel's [0x714]", n0 >= 0x718u || n1 <= 0x714u, 1);
            eq("NUL stub below SysVars' -2 word", n1 <= sv0, 1);
            eq("NUL stub is an offset NUL's own segment can name", DOS_NULSTUB_OFF < 0x26u, 1);
            eq("env block above the SDA", DOS_ENV_SEG * 16u >= sda1, 1);
        }
        ++checks;
        if (sv0 < 0x718u && sv1 > 0x714u) {
            ++fails; printf("  FAIL %-54s\n", "SysVars must not touch the kernel's [0x714]");
        }
        ++checks;
        if (sv1 > DOS_CTAB_SEG * 16u) {
            ++fails; printf("  FAIL %-54s\n", "SysVars must end below DOS_CTAB_SEG");
        }
        ++checks;
        if (sv0 < sda1 && sda0 < sv1) {
            ++fails; printf("  FAIL %-54s\n", "the SDA must not land on SysVars (this WAS GH #47)");
        }
        ++checks;
        if (DOS_SDA_SEG * 16u + DOS_INDOS_OFF
            == DOS_SYSVARS_SEG * 16u + DOS_SYSVARS_OFF + 0x45) {
            ++fails; printf("  FAIL %-54s\n", "InDOS is back on SysVars+0x45 (GH #47 regressed)");
        }
        ++checks;
        if (sda1 > DOS_CTAB_SEG * 16u || sda0 < 0x718u) {
            ++fails; printf("  FAIL %-54s\n", "the SDA must sit in free DOS data space (s81)");
        }
        ++checks;   /* s81: the stubs planted in DOS_HDLR_SEG sat on the old SDA */
        if (sda0 < DOS_HDLR_SEG * 16u + 0x100u && sda1 > DOS_HDLR_SEG * 16u) {
            ++fails; printf("  FAIL %-54s\n", "the SDA must not share DOS_HDLR_SEG's stub space");
        }
        ++checks;
        if (DOS_CTAB_SEG * 16u + DOS_WOW_TBL_OFF < DOS_SYSVARS_SEG * 16u) {
            ++fails; printf("  FAIL %-54s\n", "krnl386's table must be reachable from SysVars' segment");
        }
    }

    printf("== %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
