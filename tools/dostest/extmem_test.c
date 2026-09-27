/* extmem_test.c -- INT 15h AH=87h address resolution (src/dos/dos_extmem.h). GH #54.
 *
 * The resolver is the only thing between a DOS program's GDT and the host's own
 * memory, so what matters most here is what it REFUSES: a range that straddles two
 * regions, one past 16 MB, one that wraps. The acceptance round trip itself is
 * measured on the rig by p_int15.asm against three oracles.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dos_extmem.h"

static int total = 0, fails = 0;
#define CHECK(c,m) do{ total++; if(c){printf("  PASS  %s\n",(m));} \
    else{printf("  FAIL  %s\n",(m)); fails++;} }while(0)

int main(void)
{
    xms_state x;
    static uint8_t conv[0x10FFF0];
    uint8_t *raw = (uint8_t *)malloc(EXTMEM_RAW_LEN);
    memset(&x, 0, sizeof x);

    /* A fake EMB at a 32-bit address: classification only, never dereferenced. */
    x.h[3].used = 1; x.h[3].size_kb = 4; x.h[3].mem = (void *)(uintptr_t)0x02000000u;

    CHECK(extmem_classify(&x, 0x500, 256) == EXTMEM_DIRECT, "conventional -> direct");
    CHECK(extmem_classify(&x, 0x100000, 0xFFF0) == EXTMEM_DIRECT, "the whole HMA -> direct");
    CHECK(extmem_classify(&x, 0x10FFF0, 16) == EXTMEM_RAW, "first byte past the HMA -> raw");
    CHECK(extmem_classify(&x, 0x10FFE0, 32) == EXTMEM_NONE, "straddling HMA/raw -> refused");
    CHECK(extmem_classify(&x, 0xFFFF00, 256) == EXTMEM_RAW, "last 256 bytes below 16 MB -> raw");
    CHECK(extmem_classify(&x, 0xFFFF00, 257) == EXTMEM_NONE, "one byte past 16 MB -> refused");
    CHECK(extmem_classify(&x, 0x02000000, 4096) == EXTMEM_EMB, "whole EMB -> the block");
    CHECK(extmem_classify(&x, 0x02000F00, 256) == EXTMEM_EMB, "EMB tail -> the block");
    CHECK(extmem_classify(&x, 0x02000F00, 257) == EXTMEM_NONE, "past the EMB's end -> refused");
    CHECK(extmem_classify(&x, 0x01FFFFFF, 2) == EXTMEM_NONE, "just before the EMB -> refused");
    CHECK(extmem_classify(&x, 0x30000000, 16) == EXTMEM_NONE, "arbitrary high address -> refused");
    CHECK(extmem_classify(&x, 0xFFFFFFF0u, 32) == EXTMEM_NONE, "wraps 4 GB -> refused");
    CHECK(extmem_classify(&x, 0x500, 0) == EXTMEM_NONE, "zero length -> refused");
    x.h[3].used = 0;
    CHECK(extmem_classify(&x, 0x02000000, 16) == EXTMEM_NONE, "a FREED block is not memory");

    CHECK(extmem_resolve(&x, (uintptr_t)conv, raw, 0x1234, 4) == conv + 0x1234,
          "direct resolves into guest memory");
    CHECK(extmem_resolve(&x, (uintptr_t)conv, raw, 0x200000, 4) == raw + 0x100000,
          "raw 2 MB -> raw buffer offset 1 MB");
    CHECK(extmem_resolve(&x, (uintptr_t)conv, 0, 0x200000, 4) == 0,
          "raw with no buffer yet -> 0 (caller allocates)");

    {   uint8_t d[8] = { 0xFF, 0xFF, 0x34, 0x12, 0x10, 0x93, 0x00, 0x02 };
        CHECK(extmem_desc_base(d) == 0x02101234u, "GDT base: bytes 2-4 and 7"); }

    free(raw);
    printf("\n%d checks, %d failed\n", total, fails);
    return fails ? 1 : 0;
}
