/* mcb_test.c -- off-VM unit battery for the DOS MCB allocator (dos_mcb.h).
 *
 * Layer 1 of the M2.4 test plan: exercise alloc/free/resize and the chain
 * invariants natively on the build host, with no XP VM in the loop. Catches the
 * allocator-logic bugs (split math, forward-coalesce, grow-into-neighbour, the
 * fail-returns-largest contract) in milliseconds. Run via run.sh; exits nonzero
 * if any case fails.
 *
 * The sequence mirrors how a real DOS .EXE drives the allocator: at startup the
 * program block ('Z') owns ALL of conventional memory, so it must AH=4Ah-shrink
 * itself before anything can be allocated -- exactly what mem.exe did on the VM.
 */
#include <stdio.h>
#include "dos_mcb.h"
#include "dos_loader.h"
#include "dos_psp.h"
#include "dos_env.h"

static uint8_t mem[0x100000];          /* 1MB flat "conventional memory" buffer */

static int total = 0, fails = 0;
#define CHECK(cond, msg) do {                                  \
        total++;                                               \
        if (cond) { printf("  PASS  %s\n", (msg)); }           \
        else      { printf("  FAIL  %s\n", (msg)); fails++; }  \
    } while (0)

static uint8_t  sig_of(uint16_t seg) { return mem[(uint32_t)seg << 4]; }
static uint16_t own_of(uint16_t seg) { return mcb_rd16(mem + (((uint32_t)seg << 4) + 1)); }
static uint16_t sz_of (uint16_t seg) { return mcb_rd16(mem + (((uint32_t)seg << 4) + 3)); }

static void dump_chain(uint16_t first) {
    uint16_t m = first; int g = 0;
    printf("  chain:");
    for (;;) {
        uint8_t sig = sig_of(m); uint16_t own = own_of(m), sz = sz_of(m);
        printf(" [%04X %c o=%04X sz=%04X]", m,
               (sig >= 32 && sig < 127) ? sig : '?', own, sz);
        if (sig == 'Z' || ++g > 32) break;
        m = (uint16_t)(m + 1 + sz);
    }
    printf("\n");
}

int main(void) {
    uint16_t first = dos_mcb_init(mem);
    uint16_t seg = 0, max = 0;
    int rc;

    printf("== M2.4 MCB allocator battery ==\n");

    /* T0: initial chain ---------------------------------------------------- */
    CHECK(first == 0x5F, "init: chain root at 0x5F");
    CHECK(dos_mcb_check(mem, first, DOS_MEM_TOP) == 0, "init: chain consistent");
    CHECK(sig_of(0xFF) == 'Z' && own_of(0xFF) == DOS_PSP_SEG && sz_of(0xFF) == 0x9F00,
          "init: program block = Z / PSP / 0x9F00");
    CHECK(sig_of(0x5F) == 'M' && own_of(0x5F) == DOS_PSP_SEG && sz_of(0x5F) == 0x10,
          "init: env block = M / PSP / 0x10");
    CHECK(sig_of(0x70) == 'M' && own_of(0x70) == 0x0008 && sz_of(0x70) == 0x8E,
          "init: DOS block = M / 8 / 0x8E");

    /* T1: alloc on a fresh chain must fail (everything is owned) ------------ */
    max = 0xDEAD;
    rc = dos_alloc(mem, first, 0x10, &seg, &max);
    CHECK(rc == 8 && max == 0, "alloc on fresh chain fails, max=0 (all owned)");

    /* T2: program shrinks its own block -- the .EXE-startup pattern --------- */
    rc = dos_resize(mem, 0x100, 0x1000, &max);
    CHECK(rc == 0, "resize: program shrinks 0x9F00 -> 0x1000");
    CHECK(sig_of(0xFF) == 'M' && sz_of(0xFF) == 0x1000, "shrink: block now M / 0x1000");
    CHECK(sig_of(0x1100) == 'Z' && own_of(0x1100) == 0 && sz_of(0x1100) == 0x8EFF,
          "shrink: freed tail = Z / free / 0x8EFF");
    CHECK(dos_mcb_check(mem, first, DOS_MEM_TOP) == 0, "after shrink: chain consistent");

    /* T3: allocate from the freed tail (split) ----------------------------- */
    rc = dos_alloc(mem, first, 0x80, &seg, &max);
    CHECK(rc == 0 && seg == 0x1101, "alloc 0x80 -> seg 0x1101");
    CHECK(sig_of(0x1100) == 'M' && own_of(0x1100) == DOS_PSP_SEG && sz_of(0x1100) == 0x80,
          "alloc: block = M / PSP / 0x80");
    CHECK(sig_of(0x1181) == 'Z' && own_of(0x1181) == 0 && sz_of(0x1181) == 0x8E7E,
          "alloc: split tail = Z / free / 0x8E7E");
    CHECK(dos_mcb_check(mem, first, DOS_MEM_TOP) == 0, "after alloc: chain consistent");

    /* T3b: a too-large alloc fails and reports the largest free block ------ */
    max = 0;
    rc = dos_alloc(mem, first, 0xFFFF, &seg, &max);
    CHECK(rc == 8 && max == 0x8E7E, "alloc 0xFFFF fails, max = largest free 0x8E7E");

    /* T4: free + forward coalesce back to one free block ------------------- */
    rc = dos_free(mem, 0x1101);
    CHECK(rc == 0, "free seg 0x1101 ok");
    CHECK(sig_of(0x1100) == 'Z' && own_of(0x1100) == 0 && sz_of(0x1100) == 0x8EFF,
          "free: forward-coalesced to Z / free / 0x8EFF");
    CHECK(dos_mcb_check(mem, first, DOS_MEM_TOP) == 0, "after free: chain consistent");

    /* T5: resize grow into the free neighbour ------------------------------ */
    rc = dos_resize(mem, 0x100, 0x2000, &max);
    CHECK(rc == 0, "resize grow 0x1000 -> 0x2000 into free neighbour");
    CHECK(sig_of(0xFF) == 'M' && sz_of(0xFF) == 0x2000, "grow: block now 0x2000");
    CHECK(sig_of(0x2100) == 'Z' && own_of(0x2100) == 0 && sz_of(0x2100) == 0x7EFF,
          "grow: remaining free = Z / 0x7EFF");
    CHECK(dos_mcb_check(mem, first, DOS_MEM_TOP) == 0, "after grow: chain consistent");

    /* T6: grow blocked by an owned neighbour ------------------------------- */
    rc = dos_alloc(mem, first, 0x100, &seg, &max);
    CHECK(rc == 0 && seg == 0x2101, "alloc 0x100 -> seg 0x2101 (neighbour now owned)");
    max = 0;
    rc = dos_resize(mem, 0x100, 0x3000, &max);
    CHECK(rc == 8 && max == 0x2000, "grow blocked by owned neighbour, max = cur 0x2000");
    CHECK(sig_of(0xFF) == 'M' && sz_of(0xFF) == 0x2000, "blocked grow leaves block unchanged");

    /* T7: invalid block segments ------------------------------------------- */
    CHECK(dos_free(mem, 0x0001) == 9, "free bad block -> err 9");
    CHECK(dos_resize(mem, 0x0001, 0x10, &max) == 9, "resize bad block -> err 9");

    /* T8: resize to the current size is a no-op success -------------------- */
    rc = dos_resize(mem, 0x100, 0x2000, &max);
    CHECK(rc == 0 && sz_of(0xFF) == 0x2000, "resize to same size is a no-op success");
    CHECK(dos_mcb_check(mem, first, DOS_MEM_TOP) == 0, "final: chain consistent");

    /* T9: merge-on-alloc. alloc() coalesces adjacent free blocks during the walk
     * (as real MS-DOS does), so two adjacent free blocks jointly satisfy a request
     * that neither satisfies alone. (Previously a pinned gap; now closed.) */
    {
        static uint8_t g[0x10000];
        mcb_lay(g, 0x0300, 'M', 0,           0x20);   /* free block #1            */
        mcb_lay(g, 0x0321, 'M', 0,           0x20);   /* free block #2 (adjacent) */
        mcb_lay(g, 0x0342, 'Z', DOS_PSP_SEG, 0x10);   /* owned terminator         */
        seg = max = 0;
        rc = dos_alloc(g, 0x0300, 0x21, &seg, &max);
        /* merged = 0x20 + 1 + 0x20 = 0x41 paras; alloc 0x21 splits it, leaving a
         * 0x41 - 0x21 - 1 = 0x1F free tail at paragraph 0x322. */
        CHECK(rc == 0 && seg == 0x0301,
              "merge-on-alloc: adjacent free blocks merged to satisfy 0x21");
        CHECK(g[(uint32_t)0x0300 << 4] == 'M'
              && mcb_rd16(g + (((uint32_t)0x0300 << 4) + 1)) == DOS_PSP_SEG
              && mcb_rd16(g + (((uint32_t)0x0300 << 4) + 3)) == 0x21,
              "merge-on-alloc: allocated block = M / PSP / 0x21");
        CHECK(g[(uint32_t)0x0322 << 4] == 'M'
              && mcb_rd16(g + (((uint32_t)0x0322 << 4) + 1)) == 0
              && mcb_rd16(g + (((uint32_t)0x0322 << 4) + 3)) == 0x1F,
              "merge-on-alloc: free tail = M / free / 0x1F");
        CHECK(dos_mcb_check(g, 0x0300, 0x0353) == 0,
              "merge-on-alloc: mini-chain consistent");
    }

    /* T10: PSP builder (src/dos/dos_psp.h) --------------------------------- */
    {
        static uint8_t pm[0x20000];
        volatile uint8_t *psp = pm + ((uint32_t)0x0100 << 4);
        dos_psp_build(pm, 0x0100, 0x0060, 0xA000);
        CHECK(psp[0] == 0xCD && psp[1] == 0x20, "psp: INT 20h at offset 0");
        CHECK(mcb_rd16(psp + 0x02) == 0xA000, "psp: top-of-mem segment = 0xA000");
        CHECK(mcb_rd16(psp + 0x2C) == 0x0060, "psp: environment segment = 0x60");
        CHECK(psp[0x50] == 0xCD && psp[0x51] == 0x21 && psp[0x52] == 0xCB,
              "psp: INT 21h;RETF dispatch stub at 0x50");
        CHECK(psp[0x80] == 0 && psp[0x81] == 0x0D, "psp: empty command tail + 0x0D");
    }

    /* T11: flat .COM loader (src/dos/dos_loader.h) ------------------------- */
    {
        static uint8_t cm[0x20000];
        static const uint8_t com[] = { 0xB4, 0x09, 0xCD, 0x21, 0xC3 };
        volatile uint8_t *code = cm + ((uint32_t)0x0100 << 4) + 0x100;
        dos_image_t e = dos_load(cm, com, (uint32_t)sizeof(com), 0x0100);
        CHECK(!e.is_exe && e.cs == 0x0100 && e.ip == 0x0100
              && e.ss == 0x0100 && e.sp == 0xFFFE,
              ".COM: entry CS=SS=0x100, IP=0x100, SP=0xFFFE");
        CHECK(code[0] == 0xB4 && code[1] == 0x09 && code[4] == 0xC3
              && e.img_size == sizeof(com),
              ".COM: image placed at PSP:0x100");
    }

    /* T12: MZ .EXE loader + one relocation --------------------------------- */
    {
        static uint8_t xm[0x20000];
        /* 34-byte MZ: 32-byte header (e_cparhdr=2, e_crlc=1, reloc tbl @0x1C,
         * e_ip=5, e_sp=0x100), one reloc -> image word at offset 0, 2-byte image
         * = 0x0000 (to be fixed up to the load segment). */
        static const uint8_t mz[] = {
            'M','Z',   0x22,0x00, 0x01,0x00, 0x01,0x00, 0x02,0x00, 0x00,0x00, 0xFF,0xFF,
            0x00,0x00, 0x00,0x01, 0x00,0x00, 0x05,0x00, 0x00,0x00, 0x1C,0x00, 0x00,0x00,
            0x00,0x00, 0x00,0x00,          /* reloc[0] = offset 0, segment 0 */
            0x00,0x00                      /* image[0..1] = 0x0000           */
        };
        uint16_t load_seg = (uint16_t)(0x0100 + 0x10);          /* 0x110 */
        volatile uint8_t *img = xm + ((uint32_t)load_seg << 4);
        dos_image_t e = dos_load(xm, mz, (uint32_t)sizeof(mz), 0x0100);
        CHECK(e.is_exe && e.cs == load_seg && e.ip == 0x0005
              && e.ss == load_seg && e.sp == 0x0100,
              ".EXE: CS:IP/SS:SP from header, biased by the load segment");
        CHECK(e.img_size == 2 && mcb_rd16(img) == load_seg,
              ".EXE: relocation fixed the image word to the load segment");
    }

    /* T13: environment block builder (src/dos/dos_env.h) -------------------- */
    {
        static uint8_t g[0x1000];
        const char *path = "C:\\T.COM";
        int plen = 8, k, okp = 1;                       /* strlen("C:\T.COM") = 8 */
        uint32_t L = dos_env_build(g, 0x0000, path);
        CHECK(L > 0 && g[0] == 'C' && g[1] == 'O' && g[2] == 'M' && g[3] == 'S',
              "env: starts with COMSPEC=");
        for (k = 0; k < plen; ++k) if (g[L - 1 - plen + k] != (uint8_t)path[k]) okp = 0;
        CHECK(okp && g[L - 1] == 0, "env: program path is the final ASCIIZ string");
        CHECK(g[L - 1 - plen - 2] == 0x01 && g[L - 1 - plen - 1] == 0x00,
              "env: WORD count 0x0001 precedes the program path");
        CHECK(g[L - 1 - plen - 3] == 0x00, "env: trailing NUL ends the variable list");
    }

    /* T14: PSP command-tail builder (src/dos/dos_psp.h) --------------------- */
    {
        static uint8_t g[0x2000];
        volatile uint8_t *psp = g + ((uint32_t)0x0100 << 4);
        dos_cmdtail_build(g, 0x0100, "HELLO");
        CHECK(psp[0x80] == 6 && psp[0x81] == ' ' && psp[0x82] == 'H'
              && psp[0x86] == 'O' && psp[0x87] == 0x0D,
              "cmdtail: \"HELLO\" -> len 6, \" HELLO\", 0x0D");
        dos_cmdtail_build(g, 0x0100, "");
        CHECK(psp[0x80] == 0 && psp[0x81] == 0x0D, "cmdtail: empty -> len 0, 0x0D");
        dos_cmdtail_build(g, 0x0100, (const char *)0);
        CHECK(psp[0x80] == 0 && psp[0x81] == 0x0D, "cmdtail: NULL -> len 0, 0x0D");
    }

    dump_chain(first);
    printf("== %d/%d passed, %d failed ==\n", total - fails, total, fails);
    return fails ? 1 : 0;
}
