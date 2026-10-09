/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * Off-VM battery for the machine-describing BDA fields and the EBDA
 * (src/dos/bios_bda.h, GH #253).
 *
 * The point of #253 is that five answers describe ONE machine: INT 11h and 0040:0010,
 * INT 12h and 0040:0013, and the EBDA as seen by 0040:000E, INT 15h AH=C1h and the C0h
 * feature byte. The interrupt arms live in main.c and are checked on the rig (p_bios,
 * p_int15); what is checked here is that the BDA side is written from the same values
 * and that the EBDA it declares does not overlap anything DOS hands out.
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#include <stdio.h>
#include <string.h>
#include "bios_bda.h"
#include "dos_mcb.h"

#define BDA_TEST_MEMORY_SIZE        0x100000    /* 1 MB flat guest memory */
#define BDA_TEST_POISON             0xA5        /* So a field the init forgets is visible */
#define BDA_TEST_EQUIPMENT          0x4423
#define BDA_TEST_NEW_EQUIPMENT      0x5423
#define BDA_TEST_BASE_KB_640        639u
#define BDA_TEST_EBDA_SEGMENT_640   0x9FC0u
#define BDA_TEST_EBDA_KB            1u
#define BDA_TEST_CMOS_KB            640u        /* The base memory the CMOS reports */
#define BDA_TEST_EBDA_LINEAR_640    0x9FC00
#define BDA_TEST_EBDA_BYTES         1024
#define BDA_TEST_640K_LINEAR        0xA0000
#define BDA_TEST_LPT3_SLOT          0x40C       /* The neighbours: the LPT3 slot below ... */
#define BDA_TEST_ABOVE_MEMORY_KB    0x415       /* ... and 0040:0015 above */
#define BDA_TEST_RESERVE            0x40        /* Paragraphs reserved at the top */
#define BDA_TEST_KB_512             512
#define BDA_TEST_TOP_512            0x7FC0u
#define BDA_TEST_BASE_KB_512        511u
#define BDA_TEST_EBDA_LINEAR_512    0x7FC00
#define BDA_TEST_KB_256             256
#define BDA_TEST_TOP_256            0x3FC0u
#define BDA_TEST_BASE_KB_256        255u
#define BDA_TEST_KB_640             640
#define BDA_TEST_KB_BELOW_MIN       10
#define BDA_TEST_KB_MIN             64
#define BDA_TEST_KB_ABOVE_MAX       4096

static BYTE g_Memory[BDA_TEST_MEMORY_SIZE];          /* 1 MB flat guest memory */

static INT g_Total = 0, g_Failures = 0;

static VOID BdaTestCheck(BOOL passed, PCSTR description)
{
    g_Total++;
    if (passed)
    {
        printf("  PASS  %s\n", description);
    }
    else
    {
        printf("  FAIL  %s\n", description);
        g_Failures++;
    }
}

static UINT BdaTestReadWord(DWORD linear)
{
    return (UINT)(g_Memory[linear] | (g_Memory[linear + 1] << BYTE_SHIFT));
}

INT main(VOID)
{
    WORD firstMcb;
    UINT byteIndex, dirtyBytes;

    printf("== BDA / EBDA battery (#253) ==\n");

    /* -- THE NUMBERS, worked from the map rather than restated. 0x9FC0 paragraphs is
     * 639 KB exactly; 0xA000 - 0x9FC0 = 0x40 paragraphs = 1 KB. Measured on 6.22
     * (int12.memk = 027Fh, mcb.chain.ends.at = 9FC0h).
     */
    BdaTestCheck(BIOS_BASE_MEM_KB == BDA_TEST_BASE_KB_640, "INT 12h / 0040:0013 constant = 639 KB");
    BdaTestCheck(BIOS_EBDA_SEG == BDA_TEST_EBDA_SEGMENT_640, "EBDA segment = 9FC0h = DOS_MEM_TOP");
    BdaTestCheck(BIOS_EBDA_KB == BDA_TEST_EBDA_KB, "EBDA size = 1 KB");
    BdaTestCheck(BIOS_BASE_MEM_KB + BIOS_EBDA_KB == BDA_TEST_CMOS_KB, "base memory + EBDA = the 640 KB the CMOS reports");

    /* -- THE WRITE. Poison everything first so a field the init forgets is visible. */
    memset(g_Memory, BDA_TEST_POISON, sizeof g_Memory);
    firstMcb = DosMcbInitialize(g_Memory);
    BiosBdaInitialize(g_Memory, BDA_TEST_EQUIPMENT);
    BdaTestCheck(BdaTestReadWord(BIOS_BDA_BASE + BIOS_BDA_EBDA_SEGMENT) == BDA_TEST_EBDA_SEGMENT_640, "0040:000E = 9FC0h (the EBDA, not 'LPT4: none')");
    BdaTestCheck(BdaTestReadWord(BIOS_BDA_BASE + BIOS_BDA_EQUIPMENT) == BDA_TEST_EQUIPMENT, "0040:0010 = the equipment word passed in (INT 11h's)");
    BdaTestCheck(BdaTestReadWord(BIOS_BDA_BASE + BIOS_BDA_MEMORY_KB) == BDA_TEST_BASE_KB_640,    "0040:0013 = 639 (INT 12h's)");
    BdaTestCheck(g_Memory[BDA_TEST_EBDA_LINEAR_640] == BDA_TEST_EBDA_KB,      "EBDA:0000 = its own size in KB");
    dirtyBytes = 0;
    for (byteIndex = 1; byteIndex < BDA_TEST_EBDA_BYTES; ++byteIndex)
        if (g_Memory[BDA_TEST_EBDA_LINEAR_640 + byteIndex])
            ++dirtyBytes;
    BdaTestCheck(dirtyBytes == 0, "EBDA:0001..03FF zeroed");
    BdaTestCheck(g_Memory[BDA_TEST_LPT3_SLOT] == BDA_TEST_POISON && g_Memory[BDA_TEST_LPT3_SLOT + 1] == BDA_TEST_POISON
                 && g_Memory[BDA_TEST_ABOVE_MEMORY_KB] == BDA_TEST_POISON,
                 "neighbours untouched (LPT3 slot below, 0040:0015 above)");
    BdaTestCheck(g_Memory[BDA_TEST_640K_LINEAR] == BDA_TEST_POISON && g_Memory[BDA_TEST_EBDA_LINEAR_640 - 1] == BDA_TEST_POISON, "nothing written outside 9FC0:0000..03FF");

    /* -- THE LIVE HALF: a settings change rewrites 0010 and nothing else. */
    BiosBdaSetEquipment(g_Memory, BDA_TEST_NEW_EQUIPMENT);
    BdaTestCheck(BdaTestReadWord(BIOS_BDA_BASE + BIOS_BDA_EQUIPMENT) == BDA_TEST_NEW_EQUIPMENT
                 && BdaTestReadWord(BIOS_BDA_BASE + BIOS_BDA_MEMORY_KB) == BDA_TEST_BASE_KB_640
                 && BdaTestReadWord(BIOS_BDA_BASE + BIOS_BDA_EBDA_SEGMENT) == BDA_TEST_EBDA_SEGMENT_640,
                 "set_equipment: 0010 follows, 000E/0013 unchanged");

    /* -- NOTHING DOS OWNS OVERLAPS THE EBDA. The chain's last block must end exactly
     * where the EBDA begins, and a top reservation (the CDS) must come from BELOW it --
     * the reason the EBDA could be given its kilobyte at all.
     */
    BdaTestCheck(DosMcbCheckChain(g_Memory, firstMcb, DOS_MEM_TOP) == DOS_MCB_CHAIN_OK, "MCB chain ends at the EBDA, consistent");
    {   WORD reserved = DosMcbReserveTop(g_Memory, firstMcb, BDA_TEST_RESERVE);
        BdaTestCheck(reserved != DOS_MCB_NO_SEGMENT && (DWORD)reserved + BDA_TEST_RESERVE <= BIOS_EBDA_SEG,
                     "reserve_top: carved below 9FC0h, never into the EBDA");
        BdaTestCheck(g_Memory[BDA_TEST_EBDA_LINEAR_640] == BDA_TEST_EBDA_KB, "reserve_top: EBDA size byte survives");
    }

    /* -- #136: CONVENTIONAL MEMORY AS A SETTING. 640 must be EXACTLY today's machine;
     * anything less moves INT 12h, the EBDA and the MCB top together.
     */
    BdaTestCheck(BiosConventionalTopParagraph(BDA_TEST_KB_640) == DOS_MEM_TOP, "conv 640 KB -> top 9FC0h = DOS_MEM_TOP (default unchanged)");
    BdaTestCheck(BiosBaseKbOfTop(BiosConventionalTopParagraph(BDA_TEST_KB_640)) == BIOS_BASE_MEM_KB, "conv 640 KB -> INT 12h 639 (unchanged)");
    BdaTestCheck(BiosConventionalTopParagraph(BDA_TEST_KB_512) == BDA_TEST_TOP_512, "conv 512 KB -> top 7FC0h");
    BdaTestCheck(BiosBaseKbOfTop(BDA_TEST_TOP_512) == BDA_TEST_BASE_KB_512, "conv 512 KB -> INT 12h 511");
    BdaTestCheck(BiosConventionalTopParagraph(BDA_TEST_KB_256) == BDA_TEST_TOP_256 && BiosBaseKbOfTop(BDA_TEST_TOP_256) == BDA_TEST_BASE_KB_256, "conv 256 KB -> 3FC0h / 255");
    BdaTestCheck(BiosConventionalTopParagraph(BDA_TEST_KB_BELOW_MIN) == BiosConventionalTopParagraph(BDA_TEST_KB_MIN), "conv below 64 clamps to 64");
    BdaTestCheck(BiosConventionalTopParagraph(BDA_TEST_KB_ABOVE_MAX) == DOS_MEM_TOP, "conv above 640 clamps to 640");
    memset(g_Memory, BDA_TEST_POISON, sizeof g_Memory);
    firstMcb = DosMcbInitializeWithTop(g_Memory, BDA_TEST_TOP_512);
    BiosBdaInitializeWithTop(g_Memory, BDA_TEST_EQUIPMENT, BDA_TEST_TOP_512);
    BdaTestCheck(DosMcbCheckChain(g_Memory, firstMcb, BDA_TEST_TOP_512) == DOS_MCB_CHAIN_OK, "512 KB: MCB chain ends at 7FC0h, consistent");
    BdaTestCheck(BdaTestReadWord(BIOS_BDA_BASE + BIOS_BDA_EBDA_SEGMENT) == BDA_TEST_TOP_512 && BdaTestReadWord(BIOS_BDA_BASE + BIOS_BDA_MEMORY_KB) == BDA_TEST_BASE_KB_512, "512 KB: 0040:000E = 7FC0h, 0040:0013 = 511");
    BdaTestCheck(g_Memory[BDA_TEST_EBDA_LINEAR_512] == BDA_TEST_EBDA_KB && g_Memory[BDA_TEST_EBDA_LINEAR_640] == BDA_TEST_POISON, "512 KB: EBDA at 7FC0:0000, nothing written at 9FC0h");
    {   WORD reserved = DosMcbReserveTop(g_Memory, firstMcb, BDA_TEST_RESERVE);
        BdaTestCheck(reserved != DOS_MCB_NO_SEGMENT && (DWORD)reserved + BDA_TEST_RESERVE <= BDA_TEST_TOP_512, "512 KB: reserve_top carved below the EBDA");
    }
    memset(g_Memory, BDA_TEST_POISON, sizeof g_Memory);
    firstMcb = DosMcbInitialize(g_Memory);
    {   static BYTE secondMemory[BDA_TEST_MEMORY_SIZE];
        WORD secondFirstMcb;
        memset(secondMemory, BDA_TEST_POISON, sizeof secondMemory);
        secondFirstMcb = DosMcbInitializeWithTop(secondMemory, DOS_MEM_TOP);
        BiosBdaInitialize(g_Memory, BDA_TEST_EQUIPMENT);
        BiosBdaInitializeWithTop(secondMemory, BDA_TEST_EQUIPMENT, DOS_MEM_TOP);
        BdaTestCheck(secondFirstMcb == firstMcb && memcmp(g_Memory, secondMemory, sizeof secondMemory) == 0,
                     "init_top(DOS_MEM_TOP) is byte-identical to the old init (whole 1 MB)");
    }

    printf("\n%d checks, %d failed\n", g_Total, g_Failures);
    return g_Failures ? 1 : 0;
}
