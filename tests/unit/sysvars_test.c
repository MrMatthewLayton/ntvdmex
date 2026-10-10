/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * The List of Lists layout, pinned against the 6.22 dump.  GH #48.
 *
 * The expectations here are not read from documentation. Each one is decoded from
 * a BUF= line that tests/probes/dos/p_sysvar.asm brought back from genuine MS-DOS
 * 6.22, and the raw dump is embedded below so the decoding can be re-checked
 * rather than trusted. #48 asks for exactly this: "the structures are
 * version-specific and must not be written from documentation alone -- the layout
 * dumps have twice caught errors that a plausible reading would have missed".
 *
 *   cc -std=c99 -I src -I src/dos -o sysvars_test tests/unit/sysvars_test.c
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#include <stdio.h>
#include <string.h>
#include "dos_sysvars.h"
#include "dos_layout.h"

/* What 6.22's dump holds (decoded in the comments beside g_OracleRaw / g_OracleDpb). */
#define SYSVARS_TEST_FIRST_MCB                  0x0253
#define SYSVARS_TEST_DPB_OFFSET                 0x136A
#define SYSVARS_TEST_DPB_SEGMENT                0x0116
#define SYSVARS_TEST_SFT_OFFSET                 0x00CC
#define SYSVARS_TEST_IO_SEGMENT                 0x0070
#define SYSVARS_TEST_SECTOR_SIZE                512
#define SYSVARS_TEST_CDS_SEGMENT                0x0350
#define SYSVARS_TEST_FCB_SEGMENT                0x031E
#define SYSVARS_TEST_BLOCK_DEVICES              3
#define SYSVARS_TEST_LASTDRIVE                  5
#define SYSVARS_TEST_NUL_LEN                    0x12
#define SYSVARS_TEST_NUL_END                    0x34
#define SYSVARS_TEST_NEXT_DPB_OFFSET            0x138B
#define SYSVARS_TEST_FAT_COUNT                  2
#define SYSVARS_TEST_ROOT_ENTRIES               224
#define SYSVARS_TEST_FAT_SECTORS                9
#define SYSVARS_TEST_MEDIA_1440K                0xF0
#define SYSVARS_TEST_UNKNOWN                    0xFFFF
#define SYSVARS_TEST_DRIVE_B                    1
#define SYSVARS_TEST_CDS_LEN                    88
#define SYSVARS_TEST_CDS_FLAGS                  0x43
#define SYSVARS_TEST_CDS_DPB                    0x45
#define SYSVARS_TEST_CDS_SLASH                  0x4F
#define SYSVARS_TEST_DATA_START                 33
#define SYSVARS_TEST_ROOT_START                 19
#define SYSVARS_TEST_HIGHEST_CLUSTER            0x0B20  /* 2848 */
#define SYSVARS_TEST_BLOCK_DRIVER               0x006B
#define SYSVARS_TEST_NUL_ATTRIBUTE              0x8004

/* The structures the checks build. */
#define SYSVARS_TEST_BUFFER_SIZE                256
#define SYSVARS_TEST_FILL                       0xAA    /* So a byte the builder skips is visible */
#define SYSVARS_TEST_DRIVE_A                    0
#define SYSVARS_TEST_DRIVE_C                    2
#define SYSVARS_TEST_DRIVE_Z                    25
#define SYSVARS_TEST_DPB_PAIR_SEGMENT           0x1234
#define SYSVARS_TEST_DPB_PAIR_OFFSET            0x0040
#define SYSVARS_TEST_FLAGS_NONE                 0
#define SYSVARS_TEST_FLAGS_REDIRECTED           0xC000
#define SYSVARS_TEST_NULL_POINTER               0x0000
#define SYSVARS_TEST_ROOT_SLASH                 2
#define SYSVARS_TEST_FIXED_SECTORS_PER_CLUSTER  8
#define SYSVARS_TEST_FIXED_ROOT_ENTRIES         512
#define SYSVARS_TEST_FIXED_HIGHEST              0x1000
#define SYSVARS_TEST_FIXED_MEDIA                0xF8
#define SYSVARS_TEST_FIXED_DEVICE_SEG           0x50
#define SYSVARS_TEST_FIXED_DEVICE_OFF           0x90
#define SYSVARS_TEST_CLUSTER_MAX_8              7
#define SYSVARS_TEST_CLUSTER_SHIFT_8            3
#define SYSVARS_TEST_ONE_SECTOR                 1
#define SYSVARS_TEST_NO_BYTE                    (-1)

/* DosDpbFatLayout's cases. */
#define SYSVARS_TEST_FAT16_HIGHEST              0xFFFE
#define SYSVARS_TEST_FAT16_SECTORS              256
#define SYSVARS_TEST_FAT16_ROOT_START           513
#define SYSVARS_TEST_FAT16_DATA_START           545
#define SYSVARS_TEST_FAT12_LAST                 0xFF5
#define SYSVARS_TEST_FAT12_LAST_SECTORS         12
#define SYSVARS_TEST_FAT16_FIRST                0xFF6
#define SYSVARS_TEST_FAT16_FIRST_SECTORS        16
#define SYSVARS_TEST_4K_SECTOR                  4096
#define SYSVARS_TEST_4K_HIGHEST                 0x8000
#define SYSVARS_TEST_4K_ROOT_SECTORS            4
#define SYSVARS_TEST_ZERO_SECTOR                0

/* The device chain. */
#define SYSVARS_TEST_DEVICE_SEGMENT             0x0060
#define SYSVARS_TEST_UNITS                      3
#define SYSVARS_TEST_CON_OFFSET                 0x23
#define SYSVARS_TEST_AUX_OFFSET                 0x35
#define SYSVARS_TEST_PRN_OFFSET                 0x47
#define SYSVARS_TEST_CHARACTER_DEVICE           0x8000  /* attribute bit 15 */
#define SYSVARS_TEST_ATTRIBUTE_CON              0x8013
#define SYSVARS_TEST_ATTRIBUTE_PRN              0xA0C0
#define SYSVARS_TEST_ATTRIBUTE_CLOCK            0x8008
#define SYSVARS_TEST_UNKNOWN_STUB_LEN           7
#define SYSVARS_TEST_RETF                       0xCB
#define SYSVARS_TEST_NUL_STRATEGY               0x10
#define SYSVARS_TEST_NUL_INTERRUPT              0x17

/* The memory map. */
#define SYSVARS_TEST_UMB_HEAD                   0x8C
#define SYSVARS_TEST_SYSVARS_OFFSET             0x26
#define SYSVARS_TEST_UMB_FIELD                  0x66    /* SysVars+0x66: first UMB MCB */
#define SYSVARS_TEST_FIRST_MCB_FIELD            0x68
#define SYSVARS_TEST_NEXT_WORD                  2       /* 0x8E: the word after 0x8C */
#define SYSVARS_TEST_INDOS_COLLISION            0x45    /* The word MEM.EXE reads at SysVars+0x45 */
#define SYSVARS_TEST_IO_PARAGRAPH               0x70
#define SYSVARS_TEST_HANDLER_SLOTS              0x100u
#define SYSVARS_TEST_KERNEL_AREA_END            0x700u
#define SYSVARS_TEST_KERNEL_DWORD               0x714u  /* The kernel's [0x714] */
#define SYSVARS_TEST_KERNEL_DWORD_END           0x718u
#define SYSVARS_TEST_PARAGRAPH                  16u
#define SYSVARS_TEST_MCB_HEAD_BYTES             2u

/* g_OracleRaw starts at ES:BX-2: SysVars+offset is two bytes further on. */
#define SYSVARS_TEST_RAW_START                  2
#define SYSVARS_TEST_BYTE(offset)               (g_OracleRaw[(offset) + SYSVARS_TEST_RAW_START]) /* SysVars+offset */

static INT g_Checks;
static INT g_Failures;

/* THE ORACLE'S OWN BYTES:
 * BUF=sysvars.raw, starting two bytes BEFORE what AH=52h returned in ES:BX.
 */
static const BYTE g_OracleRaw[] = {
 0x53,0x02,                                     /* ES:BX-2 first MCB segment */
 0x6A,0x13,0x16,0x01,  0xCC,0x00,0x16,0x01,     /* +00 DPB      +04 SFT */
 0x59,0x00,0x70,0x00,  0x23,0x00,0x70,0x00,     /* +08 CLOCK$   +0C CON */
 0x00,0x02,                                     /* +10 max bytes/sector */
 0x6D,0x00,0x16,0x01,                           /* +12 disk buffers */
 0x00,0x00,0x50,0x03,                           /* +16 CDS array */
 0x00,0x00,0x1E,0x03,                           /* +1A FCB table */
 0x00,0x00,                                     /* +1E FCB keep count */
 0x03,                                          /* +20 block devices */
 0x05,                                          /* +21 LASTDRIVE */
 0x00,0x00,0x55,0x02, 0x04,0x80, 0xC6,0x0D, 0xCC,0x0D,   /* +22 NUL header */
 0x4E,0x55,0x4C,0x20,0x20,0x20,0x20,0x20,       /*     "NUL     " */
};
/* BUF=sysvars.dpb0 -- the first DPB, at 0116:136A. */
static const BYTE g_OracleDpb[] = {
 0x00,0x00,0x00,0x02,0x00,0x00,0x01,0x00,0x02,0xE0,0x00,0x21,0x00,0x20,0x0B,0x09,
 0x00,0x13,0x00,0x6B,0x00,0x70,0x00,0xF0,0x00,0x8B,0x13,0x16,0x01,0x00,0x00,0xFF,
 0xFF,
 /* and the NEXT DPB begins immediately here, which is how DOS_DPB_LEN is known: */
 0x01,0x01,0x00,0x02,0xFE,0x00,0x01,0x00,0x02,0x40,0x00,0x09,0x00,0x60,0x01,
};

static VOID SysVarsTestExpect(PCSTR description, LONG actual, LONG expected)
{
    ++g_Checks;

    if (actual == expected)
        return;

    ++g_Failures;
    printf("  FAIL %-54s got 0x%lX, want 0x%lX\n", description, (long)actual, (long)expected);
}

static VOID SysVarsTestFail(PCSTR description)
{
    ++g_Failures;
    printf("  FAIL %-54s\n", description);
}

static UINT SysVarsTestWordAt(PCBYTE bytes)
{
    return bytes[0] | (bytes[1] << BYTE_SHIFT);
}

static UINT SysVarsTestWord(INT offset)
{
    return SysVarsTestWordAt(&SYSVARS_TEST_BYTE(offset));
}

INT main(VOID)
{
    BYTE buffer[SYSVARS_TEST_BUFFER_SIZE];

    printf("== INT 21h AH=52h List of Lists (dos_sysvars.h) -- decoded from 6.22\n");

    /* THE FIELD OFFSETS. Each check reads the oracle's own bytes at the
     * offset our header declares and asserts the value we decoded. If an offset
     * is wrong, the value read there will not be the one 6.22 reported.
     */
    SysVarsTestExpect("ES:BX-2 is the first MCB segment (0x0253)",
                      SysVarsTestWordAt(g_OracleRaw), SYSVARS_TEST_FIRST_MCB);
    SysVarsTestExpect("+00 DPB chain offset  = 0x136A", SysVarsTestWord(DOS_SYSVARS_DPB), SYSVARS_TEST_DPB_OFFSET);
    SysVarsTestExpect("+02 DPB chain segment = 0x0116", SysVarsTestWord(DOS_SYSVARS_DPB + X86_FAR_POINTER_SEGMENT), SYSVARS_TEST_DPB_SEGMENT);
    SysVarsTestExpect("+04 SFT chain offset  = 0x00CC", SysVarsTestWord(DOS_SYSVARS_SFT), SYSVARS_TEST_SFT_OFFSET);
    SysVarsTestExpect("+08 CLOCK$ device     = 0070:0059", SysVarsTestWord(DOS_SYSVARS_CLOCK + X86_FAR_POINTER_SEGMENT), SYSVARS_TEST_IO_SEGMENT);
    SysVarsTestExpect("+0C CON device        = 0070:0023", SysVarsTestWord(DOS_SYSVARS_CON + X86_FAR_POINTER_SEGMENT), SYSVARS_TEST_IO_SEGMENT);
    SysVarsTestExpect("+10 max bytes/sector  = 512", SysVarsTestWord(DOS_SYSVARS_MAX_SECTOR), SYSVARS_TEST_SECTOR_SIZE);
    SysVarsTestExpect("+16 CDS array segment = 0x0350", SysVarsTestWord(DOS_SYSVARS_CDS + X86_FAR_POINTER_SEGMENT), SYSVARS_TEST_CDS_SEGMENT);
    SysVarsTestExpect("+1A FCB table segment = 0x031E", SysVarsTestWord(DOS_SYSVARS_FCB + X86_FAR_POINTER_SEGMENT), SYSVARS_TEST_FCB_SEGMENT);
    SysVarsTestExpect("+20 block devices     = 3", SYSVARS_TEST_BYTE(DOS_SYSVARS_BLOCK_DEVICES), SYSVARS_TEST_BLOCK_DEVICES);
    SysVarsTestExpect("+21 LASTDRIVE         = 5", SYSVARS_TEST_BYTE(DOS_SYSVARS_LASTDRIVE), SYSVARS_TEST_LASTDRIVE);

    /* [INFO]: THE NUL HEADER IS INLINE, NOT A POINTER. The check that proves the
     * offset is right is the NAME: 'NUL     ' has to land where we say it does.
     * If DOS_SYSVARS_NUL were wrong by even a byte this reads as garbage.
     */
    ++g_Checks;

    if (memcmp(&SYSVARS_TEST_BYTE(DOS_SYSVARS_NUL) + DOS_DEVICE_HEADER_NAME, DOS_NUL_NAME, DOS_DEVICE_NAME_LEN) != 0)
    {
        SysVarsTestFail("+22 is the NUL header (name mismatch)");
    }

    SysVarsTestExpect("+22 NUL attribute = 0x8004", SysVarsTestWord(DOS_SYSVARS_NUL + DOS_DEVICE_HEADER_ATTRIBUTE), SYSVARS_TEST_NUL_ATTRIBUTE);
    SysVarsTestExpect("NUL header is 18 bytes", DOS_SYSVARS_NUL_LEN, SYSVARS_TEST_NUL_LEN);
    SysVarsTestExpect("SysVars proper is 0x34 bytes", DOS_SYSVARS_NUL_END, SYSVARS_TEST_NUL_END);

    /* THE DPB. Its length is a MEASUREMENT, not a recollection: 6.22's first
     * DPB sits at 0116:136A and its own next-pointer says 0116:138B.
     */
    SysVarsTestExpect("DPB next pointer = 0x138B",
                      SysVarsTestWordAt(g_OracleDpb + DOS_DPB_NEXT), SYSVARS_TEST_NEXT_DPB_OFFSET);
    SysVarsTestExpect("...so DPB_LEN = 0x138B - 0x136A = 33", DOS_DPB_LEN, SYSVARS_TEST_NEXT_DPB_OFFSET - SYSVARS_TEST_DPB_OFFSET);
    SysVarsTestExpect("DPB +02 bytes/sector = 512",
                      SysVarsTestWordAt(g_OracleDpb + DOS_DPB_SECTOR_SIZE), SYSVARS_TEST_SECTOR_SIZE);
    /* [CAUTION]: The field is the highest sector INDEX in a cluster, not the count: a
     * 1.44MB floppy has one sector per cluster and 6.22 stores 0, not 1.
     */
    SysVarsTestExpect("DPB +04 is sectors/cluster MINUS ONE (0)", g_OracleDpb[DOS_DPB_CLUSTER_MAX], SYSVARS_TEST_ONE_SECTOR - 1);
    SysVarsTestExpect("DPB +08 number of FATs = 2", g_OracleDpb[DOS_DPB_FAT_COUNT], SYSVARS_TEST_FAT_COUNT);
    SysVarsTestExpect("DPB +09 root entries = 224",
                      SysVarsTestWordAt(g_OracleDpb + DOS_DPB_ROOT_ENTRIES), SYSVARS_TEST_ROOT_ENTRIES);
    SysVarsTestExpect("DPB +0F sectors per FAT = 9 (a WORD on DOS 4+)",
                      SysVarsTestWordAt(g_OracleDpb + DOS_DPB_FAT_SECTORS), SYSVARS_TEST_FAT_SECTORS);
    SysVarsTestExpect("DPB +17 media descriptor = 0xF0 (1.44MB)", g_OracleDpb[DOS_DPB_MEDIA], SYSVARS_TEST_MEDIA_1440K);
    SysVarsTestExpect("DPB +1F free cluster count = FFFF (unknown)",
                      SysVarsTestWordAt(g_OracleDpb + DOS_DPB_FREE_COUNT), SYSVARS_TEST_UNKNOWN);
    /* The next DPB is for drive B: -- which is what makes the 33-byte stride
     * real rather than arithmetic that happens to work once.
     */
    SysVarsTestExpect("the next DPB is drive 1 (B:)", g_OracleDpb[DOS_DPB_LEN + DOS_DPB_DRIVE], SYSVARS_TEST_DRIVE_B);

    /* THE CDS. 'B:\' began exactly 88 bytes after 'A:\' in the dump. */
    SysVarsTestExpect("CDS_LEN = 88", DOS_CDS_LEN, SYSVARS_TEST_CDS_LEN);
    SysVarsTestExpect("CDS flags at +0x43", DOS_CDS_FLAGS, SYSVARS_TEST_CDS_FLAGS);
    SysVarsTestExpect("CDS DPB pointer at +0x45", DOS_CDS_DPB, SYSVARS_TEST_CDS_DPB);
    SysVarsTestExpect("CDS backslash offset field at +0x4F", DOS_CDS_SLASH, SYSVARS_TEST_CDS_SLASH);

    /* AND WHAT WE BUILD MUST MATCH THAT SHAPE. */
    memset(buffer, SYSVARS_TEST_FILL, sizeof(buffer));
    DosCdsBuild(buffer, SYSVARS_TEST_DRIVE_C /* C: */, DOS_CDS_FLAG_PHYSICAL, SYSVARS_TEST_DPB_PAIR_SEGMENT, SYSVARS_TEST_DPB_PAIR_OFFSET);
    ++g_Checks;

    if (memcmp(buffer, "C:\\", 4) != 0)
    {
        SysVarsTestFail("built CDS path is 'C:\\'");
    }

    SysVarsTestExpect("built CDS flags = 0x4000 (physical)",
                      SysVarsTestWordAt(buffer + DOS_CDS_FLAGS), DOS_CDS_FLAG_PHYSICAL);
    SysVarsTestExpect("built CDS DPB segment", SysVarsTestWordAt(buffer + DOS_CDS_DPB + X86_FAR_POINTER_SEGMENT), SYSVARS_TEST_DPB_PAIR_SEGMENT);
    SysVarsTestExpect("built CDS backslash offset = 2",
                      SysVarsTestWordAt(buffer + DOS_CDS_SLASH), SYSVARS_TEST_ROOT_SLASH);
    /* A drive letter with nothing behind it: flags 0, and the DPB pointer must
     * TERMINATE rather than dangle at whatever the array's base happens to be.
     */
    DosCdsBuild(buffer, SYSVARS_TEST_DRIVE_Z /* Z: */, SYSVARS_TEST_FLAGS_NONE, SYSVARS_TEST_DPB_PAIR_SEGMENT, SYSVARS_TEST_DPB_PAIR_OFFSET);
    SysVarsTestExpect("absent drive: flags = 0", SysVarsTestWordAt(buffer + DOS_CDS_FLAGS), SYSVARS_TEST_FLAGS_NONE);
    SysVarsTestExpect("absent drive: DPB pointer is FFFF (terminated, not dangling)",
                      SysVarsTestWordAt(buffer + DOS_CDS_DPB + X86_FAR_POINTER_SEGMENT), SYSVARS_TEST_UNKNOWN);
    /* A redirected drive (network share, MSCDEX CD-ROM): physical|network, no DPB. */
    DosCdsBuild(buffer, SYSVARS_TEST_DRIVE_Z /* Z: */, DOS_CDS_FLAG_PHYSICAL | DOS_CDS_FLAG_NETWORK, SYSVARS_TEST_DPB_PAIR_SEGMENT, SYSVARS_TEST_DPB_PAIR_OFFSET);
    SysVarsTestExpect("network drive: flags = 0xC000", SysVarsTestWordAt(buffer + DOS_CDS_FLAGS), SYSVARS_TEST_FLAGS_REDIRECTED);
    SysVarsTestExpect("network drive: DPB pointer is 0000 (no FAT behind a redirector)",
                      SysVarsTestWordAt(buffer + DOS_CDS_DPB + X86_FAR_POINTER_SEGMENT), SYSVARS_TEST_NULL_POINTER);

    memset(buffer, SYSVARS_TEST_FILL, sizeof(buffer));
    DosDpbBuild(buffer, SYSVARS_TEST_DRIVE_C, SYSVARS_TEST_SECTOR_SIZE, SYSVARS_TEST_FIXED_SECTORS_PER_CLUSTER,
                SYSVARS_TEST_FIXED_ROOT_ENTRIES, SYSVARS_TEST_FIXED_HIGHEST, SYSVARS_TEST_FIXED_MEDIA,
                SYSVARS_TEST_FIXED_DEVICE_SEG, SYSVARS_TEST_FIXED_DEVICE_OFF, DOS_CHAIN_END, DOS_CHAIN_END);
    SysVarsTestExpect("built DPB: 8 sectors/cluster stores 7", buffer[DOS_DPB_CLUSTER_MAX], SYSVARS_TEST_CLUSTER_MAX_8);
    SysVarsTestExpect("built DPB: ...and a shift of 3", buffer[DOS_DPB_CLUSTER_SHIFT], SYSVARS_TEST_CLUSTER_SHIFT_8);
    SysVarsTestExpect("built DPB: chain terminates at FFFF",
                      SysVarsTestWordAt(buffer + DOS_DPB_NEXT + X86_FAR_POINTER_SEGMENT), DOS_CHAIN_END);
    SysVarsTestExpect("built DPB: free count is FFFF, never a number we did not count",
                      SysVarsTestWordAt(buffer + DOS_DPB_FREE_COUNT), DOS_DPB_FREE_COUNT_UNKNOWN);

    /* #48: THE DERIVED FAT LAYOUT REPRODUCES 6.22's OWN FLOPPY DPB, ALL 33 BYTES.
     * Fed only what a 1.44M floppy's GetDiskFreeSpace + FORMAT's fixed choices give
     * (512 B/sector, 1 sector/cluster, 224 root entries, highest cluster 2848, media
     * F0) plus the oracle's own pointers (device 0070:006B, next 0116:138B), the
     * builder must produce the oracle's bytes exactly -- FAT=9, root start=19, data
     * start=33 included. That is what makes the derivation DOS's arithmetic and not a
     * plausible guess; the fixed-disk values follow the same rules.
     */
    memset(buffer, SYSVARS_TEST_FILL, sizeof(buffer));
    DosDpbBuild(buffer, SYSVARS_TEST_DRIVE_A, SYSVARS_TEST_SECTOR_SIZE, SYSVARS_TEST_ONE_SECTOR,
                SYSVARS_TEST_ROOT_ENTRIES, SYSVARS_TEST_HIGHEST_CLUSTER, SYSVARS_TEST_MEDIA_1440K,
                SYSVARS_TEST_IO_SEGMENT, SYSVARS_TEST_BLOCK_DRIVER, SYSVARS_TEST_DPB_SEGMENT,
                SYSVARS_TEST_NEXT_DPB_OFFSET);
    {   INT byteIndex, badByte = SYSVARS_TEST_NO_BYTE;

        for (byteIndex = 0; byteIndex < DOS_DPB_LEN; ++byteIndex) if (buffer[byteIndex] != g_OracleDpb[byteIndex])
        {
            badByte = byteIndex;
            break;
        }

        ++g_Checks;

        if (badByte >= 0)
        {
            ++g_Failures;
            printf("  FAIL %-54s at +0x%02X: got 0x%02X, want 0x%02X\n",
                   "built floppy DPB == 6.22's sysvars.dpb0, byte for byte", badByte,
                   buffer[badByte], g_OracleDpb[badByte]);
        }
    }
    SysVarsTestExpect("oracle DPB +0B data start = 33", SysVarsTestWordAt(g_OracleDpb + DOS_DPB_DATA_START), SYSVARS_TEST_DATA_START);
    SysVarsTestExpect("oracle DPB +11 root start = 19", SysVarsTestWordAt(g_OracleDpb + DOS_DPB_ROOT_START), SYSVARS_TEST_ROOT_START);
    {   UINT fatSectors, rootStart, dataStart;
        /* A fixed disk clamped to 0xFFFE clusters: FAT16, 65535 entries x 2 bytes. */
        DosDpbFatLayout(SYSVARS_TEST_SECTOR_SIZE, SYSVARS_TEST_FIXED_ROOT_ENTRIES, SYSVARS_TEST_FAT16_HIGHEST, &fatSectors, &rootStart, &dataStart);
        SysVarsTestExpect("FAT16 (highest 0xFFFE): 256 sectors per FAT", fatSectors, SYSVARS_TEST_FAT16_SECTORS);
        SysVarsTestExpect("FAT16: root starts after 1 reserved + 2 FATs = 513", rootStart, SYSVARS_TEST_FAT16_ROOT_START);
        SysVarsTestExpect("FAT16: data starts after 512 root entries (32 sectors) = 545", dataStart, SYSVARS_TEST_FAT16_DATA_START);
        /* The FAT12/FAT16 line is 4085 clusters: highest 0xFF5 is FAT12, 0xFF6 FAT16. */
        DosDpbFatLayout(SYSVARS_TEST_SECTOR_SIZE, SYSVARS_TEST_FIXED_ROOT_ENTRIES, SYSVARS_TEST_FAT12_LAST, &fatSectors, &rootStart, &dataStart);
        SysVarsTestExpect("highest 0xFF5 is FAT12: ceil(4086*1.5/512) = 12 sectors", fatSectors, SYSVARS_TEST_FAT12_LAST_SECTORS);
        DosDpbFatLayout(SYSVARS_TEST_SECTOR_SIZE, SYSVARS_TEST_FIXED_ROOT_ENTRIES, SYSVARS_TEST_FAT16_FIRST, &fatSectors, &rootStart, &dataStart);
        SysVarsTestExpect("highest 0xFF6 is FAT16: ceil(4087*2/512) = 16 sectors", fatSectors, SYSVARS_TEST_FAT16_FIRST_SECTORS);
        /* A 4 KB-sector volume: the root (512 x 32 = 16 KB) is 4 sectors. */
        DosDpbFatLayout(SYSVARS_TEST_4K_SECTOR, SYSVARS_TEST_FIXED_ROOT_ENTRIES, SYSVARS_TEST_4K_HIGHEST, &fatSectors, &rootStart, &dataStart);
        SysVarsTestExpect("4096 B/sector: data start = root start + 4", dataStart - rootStart, SYSVARS_TEST_4K_ROOT_SECTORS);
        /* A zero sector size must not divide by zero; it is read as 512. */
        DosDpbFatLayout(SYSVARS_TEST_ZERO_SECTOR, SYSVARS_TEST_ROOT_ENTRIES, SYSVARS_TEST_HIGHEST_CLUSTER, &fatSectors, &rootStart, &dataStart);
        SysVarsTestExpect("bytes/sector 0 is taken as 512 (the floppy again: data start 33)", dataStart, SYSVARS_TEST_DATA_START);
    }

    /* #48: THE DEVICE CHAIN. Order and stride are MEASURED (6.22: CON 0070:0023,
     * AUX :0035, PRN :0047, CLOCK$ :0059, the block driver :006B -- 18 bytes apart);
     * the attribute words are RBIL's and are not (p_devchn.asm is the check).
     */
    SysVarsTestExpect("6.22: AUX - CON = one header (0x35-0x23)", SYSVARS_TEST_AUX_OFFSET - SYSVARS_TEST_CON_OFFSET, DOS_DEVICE_OFFSET(DOS_DEVICE_AUX) - DOS_DEVICE_OFFSET(DOS_DEVICE_CON));
    SysVarsTestExpect("6.22: PRN - CON = two headers (0x47-0x23)", SYSVARS_TEST_PRN_OFFSET - SYSVARS_TEST_CON_OFFSET, DOS_DEVICE_OFFSET(DOS_DEVICE_PRN) - DOS_DEVICE_OFFSET(DOS_DEVICE_CON));
    SysVarsTestExpect("6.22: CLOCK$ - CON = three headers (+08 0x59 - +0C 0x23)",
                      SysVarsTestWord(DOS_SYSVARS_CLOCK) - SysVarsTestWord(DOS_SYSVARS_CON), DOS_DEVICE_OFFSET(DOS_DEVICE_CLOCK) - DOS_DEVICE_OFFSET(DOS_DEVICE_CON));
    SysVarsTestExpect("6.22: the floppy DPB's block driver - CON = four headers (0x6B-0x23)",
                      SysVarsTestWordAt(g_OracleDpb + DOS_DPB_DEVICE_HEADER) - SysVarsTestWord(DOS_SYSVARS_CON),
                      DOS_DEVICE_OFFSET(DOS_DEVICE_BLOCK) - DOS_DEVICE_OFFSET(DOS_DEVICE_CON));
    SysVarsTestExpect("a device header is 18 bytes, as NUL's", DOS_DEVICE_HEADER_LEN, DOS_SYSVARS_NUL_LEN);
    memset(buffer, SYSVARS_TEST_FILL, sizeof(buffer));
    DosDeviceChainBuild(buffer, SYSVARS_TEST_DEVICE_SEGMENT, SYSVARS_TEST_UNITS);
    {   static PCSTR expectedNames[DOS_DEVICE_COUNT] = { "CON     ", "AUX     ", "PRN     ",
            "CLOCK$  ", NULL, "COM1    ", "LPT1    ", "LPT2    ", "LPT3    ",
            "COM2    ", "COM3    ", "COM4    " };
        UINT headerOffset = 0;
        UINT segment = SYSVARS_TEST_DEVICE_SEGMENT;
        UINT headerCount = 0;
        /* walk it as a guest would: from CON, by the `next` pointers, until FFFF */
        for (;;)
        {
            PCBYTE header = buffer + headerOffset;
            UINT nextOffset = SysVarsTestWordAt(header + DOS_DEVICE_HEADER_NEXT);
            UINT nextSegment = SysVarsTestWordAt(header + DOS_DEVICE_HEADER_NEXT + X86_FAR_POINTER_SEGMENT);
            UINT attribute = SysVarsTestWordAt(header + DOS_DEVICE_HEADER_ATTRIBUTE);
            ++g_Checks;

            if (headerCount >= DOS_DEVICE_COUNT)
            {
                SysVarsTestFail("device chain runs past 12");
                break;
            }

            if (expectedNames[headerCount])
            {
                if (memcmp(header + DOS_DEVICE_HEADER_NAME, expectedNames[headerCount], DOS_DEVICE_NAME_LEN) != 0 || !(attribute & SYSVARS_TEST_CHARACTER_DEVICE))
                {
                    ++g_Failures;
                    printf("  FAIL device %u: name/char-attr wrong\n", headerCount);
                }
            }
            else if ((attribute & SYSVARS_TEST_CHARACTER_DEVICE) || header[DOS_DEVICE_HEADER_NAME] != SYSVARS_TEST_UNITS)
            {
                SysVarsTestFail("block driver: attr bit 15 clear, name[0] = units");
            }

            SysVarsTestExpect("strategy entry = the 'unknown command' stub", SysVarsTestWordAt(header + DOS_DEVICE_HEADER_STRATEGY), DOS_DEVICE_STUB_UNKNOWN);
            SysVarsTestExpect("interrupt entry = the RETF stub", SysVarsTestWordAt(header + DOS_DEVICE_HEADER_INTERRUPT), DOS_DEVICE_STUB_RETF);
            ++headerCount;

            if (nextOffset == DOS_CHAIN_END)
                break;

            SysVarsTestExpect("next segment is the area's own", nextSegment, segment);
            headerOffset = nextOffset;

            if (headerOffset + DOS_DEVICE_HEADER_LEN > DOS_DEVICE_STUB_UNKNOWN)
            {
                ++g_Failures;
                printf("  FAIL next off 0x%X\n", headerOffset);
                break;
            }
        }

        SysVarsTestExpect("the walk visits exactly 12 headers, then FFFF", headerCount, DOS_DEVICE_COUNT);
        SysVarsTestExpect("CON  attr 8013h", SysVarsTestWordAt(buffer + DOS_DEVICE_OFFSET(DOS_DEVICE_CON) + DOS_DEVICE_HEADER_ATTRIBUTE), SYSVARS_TEST_ATTRIBUTE_CON);
        SysVarsTestExpect("PRN  attr A0C0h", SysVarsTestWordAt(buffer + DOS_DEVICE_OFFSET(DOS_DEVICE_PRN) + DOS_DEVICE_HEADER_ATTRIBUTE), SYSVARS_TEST_ATTRIBUTE_PRN);
        SysVarsTestExpect("CLOCK$ attr 8008h (bit 3: the clock device)",
                          SysVarsTestWordAt(buffer + DOS_DEVICE_OFFSET(DOS_DEVICE_CLOCK) + DOS_DEVICE_HEADER_ATTRIBUTE), SYSVARS_TEST_ATTRIBUTE_CLOCK);
        /* the stubs: mov word [es:bx+3],8103h / retf, then retf */
        {   static const BYTE expectedStubs[] = { 0x26,0xC7,0x47,0x03,0x03,0x81,0xCB, 0xCB };
            ++g_Checks;

            if (memcmp(buffer + DOS_DEVICE_STUB_UNKNOWN, expectedStubs, sizeof expectedStubs) != 0)
            {
                SysVarsTestFail("device stubs encode 8103h-and-RETF / RETF");
            }

            SysVarsTestExpect("RETF stub is the byte after the unknown-command stub", DOS_DEVICE_STUB_RETF, DOS_DEVICE_STUB_UNKNOWN + SYSVARS_TEST_UNKNOWN_STUB_LEN);
            SysVarsTestExpect("the area ends just past the RETF stub", DOS_DEVICE_AREA_LEN, DOS_DEVICE_STUB_RETF + 1);
            SysVarsTestExpect("headers end before the stubs", DOS_DEVICE_OFFSET(DOS_DEVICE_COUNT) <= DOS_DEVICE_STUB_UNKNOWN, TRUE);
        }
    }

    memset(buffer, SYSVARS_TEST_FILL, sizeof(buffer));
    DosNulHeaderBuild(buffer, SYSVARS_TEST_DEVICE_SEGMENT, DOS_DEVICE_OFFSET(DOS_DEVICE_CON), SYSVARS_TEST_NUL_STRATEGY, SYSVARS_TEST_NUL_INTERRUPT);
    ++g_Checks;

    if (memcmp(buffer + DOS_DEVICE_HEADER_NAME, DOS_NUL_NAME, DOS_DEVICE_NAME_LEN) != 0)
    {
        SysVarsTestFail("built NUL header carries the name");
    }

    SysVarsTestExpect("built NUL attribute = 0x8004", SysVarsTestWordAt(buffer + DOS_DEVICE_HEADER_ATTRIBUTE), SYSVARS_TEST_NUL_ATTRIBUTE);
    SysVarsTestExpect("built NUL links on to CON (was FFFF:FFFF)", SysVarsTestWordAt(buffer + DOS_DEVICE_HEADER_NEXT + X86_FAR_POINTER_SEGMENT), SYSVARS_TEST_DEVICE_SEGMENT);
    SysVarsTestExpect("built NUL strategy entry", SysVarsTestWordAt(buffer + DOS_DEVICE_HEADER_STRATEGY), SYSVARS_TEST_NUL_STRATEGY);
    {   BYTE nulStub[DOS_NULSTUB_LEN];
        static const BYTE expectedStub[] = { 0x26,0xC7,0x47,0x03,0x00,0x01,0xCB,0xCB };
        DosNulStubBuild(nulStub);
        ++g_Checks;

        if (memcmp(nulStub, expectedStub, sizeof expectedStub) != 0)
        {
            SysVarsTestFail("NUL stubs encode done-0100h-and-RETF / RETF");
        }

        SysVarsTestExpect("NUL's interrupt entry is its RETF", nulStub[DOS_NULSTUB_INTR], SYSVARS_TEST_RETF);
    }

    /* [CAUTION]: THE TWO ABSOLUTE OFFSETS MEM.EXE READS IN THE SYSVARS SEGMENT (Importance = 2):
     * Neither is a List-of-Lists field: MEM keeps the SEGMENT AH=52h returns,
     * throws the OFFSET away, and reads fixed addresses in it. Both have cost a
     * session already, and both are pinned here because a future edit to
     * DOS_SYSVARS_OFF or DOS_SDA_OFF would silently break them again.
     *
     * Byte-for-byte from MS-DOS 6.22 (the List-of-Lists probe's
     * dump of the SysVars SEGMENT from offset 0, where SysVars itself is at
     * 0x0026):
     * 0080: 00 FF FF 00 00 00 00 00 00 00 00 00 FF FF 53 02
     *                                          ^^^^^ 0x8C = FFFF, no UMBs
     *                                                ^^^^^ 0x8E = first MCB
     * and 0x0253 is ALSO what 6.22 reports at SysVars-2 -- the same value in
     * both places, which is what says 0x8C/0x8E are the "first UMB"/"first MCB"
     * pair rather than two unrelated words.
     */
    SysVarsTestExpect("MEM's conventional/upper line lives at absolute 0x8C in the SysVars segment",
                      DOS_UMBHEAD_OFF, SYSVARS_TEST_UMB_HEAD);
    SysVarsTestExpect("...and 0xFFFF there means NO block is upper (6.22's own value)",
                      DOS_UMBHEAD_NONE, SYSVARS_TEST_UNKNOWN);
    /* s81 (#47, MEM /C): 0x8C IS NOT "A FIXED ADDRESS BESIDE SysVars". It is
     * SysVars+0x66, because 6.22 keeps SysVars at offset 0x26 of its segment -- the
     * evidence dump above says so. MEM /C reads the same word RELATIVELY, and with
     * SysVars at 0x90 it got the first MCB's reserved bytes (0000) instead.
     */
    SysVarsTestExpect("SysVars sits at 6.22's own offset in its segment", DOS_SYSVARS_OFF, SYSVARS_TEST_SYSVARS_OFFSET);
    SysVarsTestExpect("...so absolute 0x8C and SysVars+0x66 are ONE field",
                      DOS_UMBHEAD_OFF, DOS_SYSVARS_OFF + SYSVARS_TEST_UMB_FIELD);
    SysVarsTestExpect("...and 0x8E (first MCB on 6.22) is SysVars+0x68",
                      DOS_UMBHEAD_OFF + SYSVARS_TEST_NEXT_WORD, DOS_SYSVARS_OFF + SYSVARS_TEST_FIRST_MCB_FIELD);
    {   /* linear ranges: SysVars (MCB word at -2 through DOS_SYSVARS_LEN) must hit
           nothing else we place in low memory. */
        UINT sysVarsStart = DOS_SYSVARS_SEG * SYSVARS_TEST_PARAGRAPH + DOS_SYSVARS_OFF - SYSVARS_TEST_MCB_HEAD_BYTES;
        UINT sysVarsEnd = DOS_SYSVARS_SEG * SYSVARS_TEST_PARAGRAPH + DOS_SYSVARS_OFF + DOS_SYSVARS_LEN;
        UINT sdaStart = DOS_SDA_SEG * SYSVARS_TEST_PARAGRAPH + DOS_SDA_OFF;
        UINT sdaEnd = sdaStart + DOS_SDA_LEN;
        /* #207: SysVars' SEGMENT LIES BELOW THE FIRST MCB, AS ON 6.22 (0116 < 0253).
         * MEM /D prints "MSDOS System Data" = SysVars seg .. first MCB; with the chain
         * at 0x5F that was 0x5F - 0x72 paragraphs, printed as 4,294,96x. Every byte of
         * SysVars (from its -2 word) and of the SDA must sit below the first MCB header.
         */
        SysVarsTestExpect("first MCB (ES:BX-2) above the SysVars segment",
                          DOS_FIRST_MCB > DOS_SYSVARS_SEG, TRUE);
        SysVarsTestExpect("SysVars ends at or below the first MCB header", sysVarsEnd <= DOS_FIRST_MCB * SYSVARS_TEST_PARAGRAPH, TRUE);
        SysVarsTestExpect("the SDA ends at or below the first MCB header", sdaEnd <= DOS_FIRST_MCB * SYSVARS_TEST_PARAGRAPH, TRUE);
        SysVarsTestExpect("MEM /D's IO row (0070..SysVars seg) is not negative", DOS_SYSVARS_SEG >= SYSVARS_TEST_IO_PARAGRAPH, TRUE);
        {   /* #48: the device headers at DOS_DEV_SEG, and NUL's stub in SysVars' segment */
            UINT deviceStart = DOS_DEV_SEG * SYSVARS_TEST_PARAGRAPH;
            UINT deviceEnd = deviceStart + DOS_DEVICE_AREA_LEN;
            UINT nulStubStart = DOS_SYSVARS_SEG * SYSVARS_TEST_PARAGRAPH + DOS_NULSTUB_OFF;
            UINT nulStubEnd = nulStubStart + DOS_NULSTUB_LEN;
            SysVarsTestExpect("device area clear of DOS_HDLR_SEG's 0x00..0xFF", deviceStart >= DOS_HDLR_SEG * SYSVARS_TEST_PARAGRAPH + SYSVARS_TEST_HANDLER_SLOTS, TRUE);
            SysVarsTestExpect("device area ends below 0x700 (and [0x714])", deviceEnd <= SYSVARS_TEST_KERNEL_AREA_END, TRUE);
            SysVarsTestExpect("device area below the first MCB", deviceEnd <= DOS_FIRST_MCB * SYSVARS_TEST_PARAGRAPH, TRUE);
            SysVarsTestExpect("device area clear of the env block", deviceEnd <= DOS_ENV_SEG * SYSVARS_TEST_PARAGRAPH, TRUE);
            SysVarsTestExpect("NUL stub clear of the kernel's [0x714]", nulStubStart >= SYSVARS_TEST_KERNEL_DWORD_END || nulStubEnd <= SYSVARS_TEST_KERNEL_DWORD, TRUE);
            SysVarsTestExpect("NUL stub below SysVars' -2 word", nulStubEnd <= sysVarsStart, TRUE);
            SysVarsTestExpect("NUL stub is an offset NUL's own segment can name", DOS_NULSTUB_OFF < (UINT)SYSVARS_TEST_SYSVARS_OFFSET, TRUE);
            SysVarsTestExpect("env block above the SDA", DOS_ENV_SEG * SYSVARS_TEST_PARAGRAPH >= sdaEnd, TRUE);
        }
        ++g_Checks;

        if (sysVarsStart < SYSVARS_TEST_KERNEL_DWORD_END && sysVarsEnd > SYSVARS_TEST_KERNEL_DWORD)
        {
            SysVarsTestFail("SysVars must not touch the kernel's [0x714]");
        }

        ++g_Checks;

        if (sysVarsEnd > DOS_CTAB_SEG * SYSVARS_TEST_PARAGRAPH)
        {
            SysVarsTestFail("SysVars must end below DOS_CTAB_SEG");
        }

        ++g_Checks;

        if (sysVarsStart < sdaEnd && sdaStart < sysVarsEnd)
        {
            SysVarsTestFail("the SDA must not land on SysVars (this WAS GH #47)");
        }

        ++g_Checks;

        if (DOS_SDA_SEG * SYSVARS_TEST_PARAGRAPH + DOS_INDOS_OFF
            == DOS_SYSVARS_SEG * SYSVARS_TEST_PARAGRAPH + DOS_SYSVARS_OFF + SYSVARS_TEST_INDOS_COLLISION)
        {
            SysVarsTestFail("InDOS is back on SysVars+0x45 (GH #47 regressed)");
        }

        ++g_Checks;

        if (sdaEnd > DOS_CTAB_SEG * SYSVARS_TEST_PARAGRAPH || sdaStart < SYSVARS_TEST_KERNEL_DWORD_END)
        {
            SysVarsTestFail("the SDA must sit in free DOS data space (s81)");
        }

        ++g_Checks;   /* s81: the stubs planted in DOS_HDLR_SEG sat on the old SDA */

        if (sdaStart < DOS_HDLR_SEG * SYSVARS_TEST_PARAGRAPH + SYSVARS_TEST_HANDLER_SLOTS && sdaEnd > DOS_HDLR_SEG * SYSVARS_TEST_PARAGRAPH)
        {
            SysVarsTestFail("the SDA must not share DOS_HDLR_SEG's stub space");
        }

        ++g_Checks;

        if (DOS_CTAB_SEG * SYSVARS_TEST_PARAGRAPH + DOS_WOW_TBL_OFF < DOS_SYSVARS_SEG * SYSVARS_TEST_PARAGRAPH)
        {
            SysVarsTestFail("krnl386's table must be reachable from SysVars' segment");
        }
    }

    printf("== %d checks, %d failed\n", g_Checks, g_Failures);
    return g_Failures ? 1 : 0;
}
