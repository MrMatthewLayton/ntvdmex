/* bda_test.c -- off-VM battery for the machine-describing BDA fields and the EBDA
 * (src/dos/bios_bda.h, GH #253).
 *
 * The point of #253 is that five answers describe ONE machine: INT 11h and 0040:0010,
 * INT 12h and 0040:0013, and the EBDA as seen by 0040:000E, INT 15h AH=C1h and the C0h
 * feature byte. The interrupt arms live in main.c and are checked on the rig (p_bios,
 * p_int15); what is checked here is that the BDA side is written from the same values
 * and that the EBDA it declares does not overlap anything DOS hands out.
 */
#include <stdio.h>
#include <string.h>
#include "bios_bda.h"
#include "dos_mcb.h"

static uint8_t mem[0x100000];          /* 1 MB flat guest memory */

static int total = 0, fails = 0;
#define CHECK(cond, msg) do {                                  \
        total++;                                               \
        if (cond) { printf("  PASS  %s\n", (msg)); }           \
        else      { printf("  FAIL  %s\n", (msg)); fails++; }  \
    } while (0)

static unsigned rd16(uint32_t lin) { return (unsigned)(mem[lin] | (mem[lin + 1] << 8)); }

int main(void)
{
    uint16_t first;
    unsigned k, dirty;

    printf("== BDA / EBDA battery (#253) ==\n");

    /* ── THE NUMBERS, worked from the map rather than restated. 0x9FC0 paragraphs is
         639 KB exactly; 0xA000 - 0x9FC0 = 0x40 paragraphs = 1 KB. Measured on 6.22
         (int12.memk = 027Fh, mcb.chain.ends.at = 9FC0h). */
    CHECK(BIOS_BASE_MEM_KB == 639u, "INT 12h / 0040:0013 constant = 639 KB");
    CHECK(BIOS_EBDA_SEG == 0x9FC0u, "EBDA segment = 9FC0h = DOS_MEM_TOP");
    CHECK(BIOS_EBDA_KB == 1u, "EBDA size = 1 KB");
    CHECK(BIOS_BASE_MEM_KB + BIOS_EBDA_KB == 640u, "base memory + EBDA = the 640 KB the CMOS reports");

    /* ── THE WRITE. Poison everything first so a field the init forgets is visible. */
    memset(mem, 0xA5, sizeof mem);
    first = dos_mcb_init(mem);
    bios_bda_init(mem, 0x4423);
    CHECK(rd16(0x40E) == 0x9FC0u, "0040:000E = 9FC0h (the EBDA, not 'LPT4: none')");
    CHECK(rd16(0x410) == 0x4423u, "0040:0010 = the equipment word passed in (INT 11h's)");
    CHECK(rd16(0x413) == 639u,    "0040:0013 = 639 (INT 12h's)");
    CHECK(mem[0x9FC00] == 1,      "EBDA:0000 = its own size in KB");
    dirty = 0;
    for (k = 1; k < 1024; ++k) if (mem[0x9FC00 + k]) ++dirty;
    CHECK(dirty == 0, "EBDA:0001..03FF zeroed");
    CHECK(mem[0x40C] == 0xA5 && mem[0x40D] == 0xA5 && mem[0x415] == 0xA5,
          "neighbours untouched (LPT3 slot below, 0040:0015 above)");
    CHECK(mem[0xA0000] == 0xA5 && mem[0x9FBFF] == 0xA5, "nothing written outside 9FC0:0000..03FF");

    /* ── THE LIVE HALF: a settings change rewrites 0010 and nothing else. */
    bios_bda_set_equipment(mem, 0x5423);
    CHECK(rd16(0x410) == 0x5423u && rd16(0x413) == 639u && rd16(0x40E) == 0x9FC0u,
          "set_equipment: 0010 follows, 000E/0013 unchanged");

    /* ── ★ NOTHING DOS OWNS OVERLAPS THE EBDA. The chain's last block must end exactly
         where the EBDA begins, and a top reservation (the CDS) must come from BELOW it --
         the reason the EBDA could be given its kilobyte at all. */
    CHECK(dos_mcb_check(mem, first, DOS_MEM_TOP) == 0, "MCB chain ends at the EBDA, consistent");
    {   uint16_t r = dos_mcb_reserve_top(mem, first, 0x40);
        CHECK(r != 0 && (uint32_t)r + 0x40u <= BIOS_EBDA_SEG,
              "reserve_top: carved below 9FC0h, never into the EBDA");
        CHECK(mem[0x9FC00] == 1, "reserve_top: EBDA size byte survives");
    }

    /* ── #136: CONVENTIONAL MEMORY AS A SETTING. 640 must be EXACTLY today's machine;
         anything less moves INT 12h, the EBDA and the MCB top together. */
    CHECK(bios_conv_top_para(640) == DOS_MEM_TOP, "conv 640 KB -> top 9FC0h = DOS_MEM_TOP (default unchanged)");
    CHECK(bios_base_kb_of_top(bios_conv_top_para(640)) == BIOS_BASE_MEM_KB, "conv 640 KB -> INT 12h 639 (unchanged)");
    CHECK(bios_conv_top_para(512) == 0x7FC0u, "conv 512 KB -> top 7FC0h");
    CHECK(bios_base_kb_of_top(0x7FC0u) == 511u, "conv 512 KB -> INT 12h 511");
    CHECK(bios_conv_top_para(256) == 0x3FC0u && bios_base_kb_of_top(0x3FC0u) == 255u, "conv 256 KB -> 3FC0h / 255");
    CHECK(bios_conv_top_para(10) == bios_conv_top_para(64), "conv below 64 clamps to 64");
    CHECK(bios_conv_top_para(4096) == DOS_MEM_TOP, "conv above 640 clamps to 640");
    memset(mem, 0xA5, sizeof mem);
    first = dos_mcb_init_top(mem, 0x7FC0u);
    bios_bda_init_top(mem, 0x4423, 0x7FC0u);
    CHECK(dos_mcb_check(mem, first, 0x7FC0u) == 0, "512 KB: MCB chain ends at 7FC0h, consistent");
    CHECK(rd16(0x40E) == 0x7FC0u && rd16(0x413) == 511u, "512 KB: 0040:000E = 7FC0h, 0040:0013 = 511");
    CHECK(mem[0x7FC00] == 1 && mem[0x9FC00] == 0xA5, "512 KB: EBDA at 7FC0:0000, nothing written at 9FC0h");
    {   uint16_t r = dos_mcb_reserve_top(mem, first, 0x40);
        CHECK(r != 0 && (uint32_t)r + 0x40u <= 0x7FC0u, "512 KB: reserve_top carved below the EBDA");
    }
    memset(mem, 0xA5, sizeof mem);
    first = dos_mcb_init(mem);
    {   static uint8_t mem2[0x100000];
        uint16_t f2;
        memset(mem2, 0xA5, sizeof mem2);
        f2 = dos_mcb_init_top(mem2, DOS_MEM_TOP);
        bios_bda_init(mem, 0x4423);
        bios_bda_init_top(mem2, 0x4423, DOS_MEM_TOP);
        CHECK(f2 == first && memcmp(mem, mem2, sizeof mem2) == 0,
              "init_top(DOS_MEM_TOP) is byte-identical to the old init (whole 1 MB)");
    }

    printf("\n%d checks, %d failed\n", total, fails);
    return fails ? 1 : 0;
}
