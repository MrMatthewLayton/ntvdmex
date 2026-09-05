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
    dos_cds_build(buf, 2 /* C: */, 1, 0x1234, 0x0040);
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

    memset(buf, 0xAA, sizeof(buf));
    dos_dpb_build(buf, 2, 512, 8, 512, 0x1000, 0xF8, 0x50, 0x90, 0xFFFF, 0xFFFF);
    eq("built DPB: 8 sectors/cluster stores 7", buf[DPB_CLUSTMAX], 7);
    eq("built DPB: ...and a shift of 3", buf[DPB_CLUSTSHIFT], 3);
    eq("built DPB: chain terminates at FFFF",
       buf[DPB_NEXT + 2] | (buf[DPB_NEXT + 3] << 8), 0xFFFF);
    eq("built DPB: free count is FFFF, never a number we did not count",
       buf[DPB_FREECOUNT] | (buf[DPB_FREECOUNT + 1] << 8), 0xFFFF);

    memset(buf, 0xAA, sizeof(buf));
    dos_nul_build(buf, 0xFFFF, 0xFFFF);
    ++checks;
    if (memcmp(buf + 0x0A, "NUL     ", 8) != 0) {
        ++fails; printf("  FAIL %-54s\n", "built NUL header carries the name");
    }
    eq("built NUL attribute = 0x8004", buf[4] | (buf[5] << 8), 0x8004);
    eq("built NUL chain terminates", buf[2] | (buf[3] << 8), 0xFFFF);

    printf("== %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
