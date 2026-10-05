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

/* A fake EMB at a 32-bit address: classification only, never dereferenced. */
#define EXTMEM_TEST_EMB_SLOT             3
#define EXTMEM_TEST_EMB_BASE             0x02000000u
#define EXTMEM_TEST_EMB_KB               4
#define EXTMEM_TEST_EMB_BYTES            (EXTMEM_TEST_EMB_KB * DOS_XMS_BYTES_PER_KB)
#define EXTMEM_TEST_EMB_TAIL             (EXTMEM_TEST_EMB_BASE + EXTMEM_TEST_EMB_BYTES \
                                          - EXTMEM_TEST_TAIL_LENGTH)   /* its last 256 bytes */

/* The addresses and lengths the checks classify. */
#define EXTMEM_TEST_CONVENTIONAL_ADDRESS 0x500
#define EXTMEM_TEST_HMA_LENGTH           (DOS_EXTMEM_DIRECT_END - DOS_EXTMEM_RAW_BASE)
#define EXTMEM_TEST_TAIL_LENGTH          256
#define EXTMEM_TEST_SHORT_LENGTH         16
#define EXTMEM_TEST_STRADDLE_LENGTH      32
#define EXTMEM_TEST_WORD_LENGTH          4
#define EXTMEM_TEST_BOUNDARY_LENGTH      2             /* the byte before a region and its first */
#define EXTMEM_TEST_UNOWNED_ADDRESS      0x30000000u   /* an arbitrary high address      */
#define EXTMEM_TEST_WRAPPING_ADDRESS     0xFFFFFFF0u   /* 32 bytes from here wraps 4 GB  */
#define EXTMEM_TEST_DIRECT_ADDRESS       0x1234
#define EXTMEM_TEST_RAW_ADDRESS          0x200000u     /* 2 MB                           */

/* An AH=87h descriptor whose base is 02101234h: bytes 2-4 and 7. */
#define EXTMEM_TEST_DESCRIPTOR_BASE      0x02101234u
#define EXTMEM_TEST_DESCRIPTOR           { 0xFF, 0xFF, 0x34, 0x12, 0x10, 0x93, 0x00, 0x02 }

static INT g_Checks = 0, g_Failures = 0;

static VOID ExtMemTestCheck(BOOL passed, PCSTR description)
{
    g_Checks++;
    if (passed) { printf("  PASS  %s\n", description); }
    else { printf("  FAIL  %s\n", description); g_Failures++; }
}

int main(void)
{
    DOS_XMS_STATE xmsState;
    static BYTE conventional[DOS_EXTMEM_DIRECT_END];
    PBYTE rawBuffer = (PBYTE)malloc(DOS_EXTMEM_RAW_LENGTH);
    memset(&xmsState, 0, sizeof xmsState);

    /* A fake EMB at a 32-bit address: classification only, never dereferenced. */
    xmsState.Handles[EXTMEM_TEST_EMB_SLOT].InUse = 1;
    xmsState.Handles[EXTMEM_TEST_EMB_SLOT].SizeKb = EXTMEM_TEST_EMB_KB;
    xmsState.Handles[EXTMEM_TEST_EMB_SLOT].Memory = (PVOID)(UINT_PTR)EXTMEM_TEST_EMB_BASE;

    ExtMemTestCheck(DosExtMemClassify(&xmsState, EXTMEM_TEST_CONVENTIONAL_ADDRESS,
                                      EXTMEM_TEST_TAIL_LENGTH) == DOS_EXTMEM_REGION_DIRECT,
                    "conventional -> direct");
    ExtMemTestCheck(DosExtMemClassify(&xmsState, DOS_EXTMEM_RAW_BASE, EXTMEM_TEST_HMA_LENGTH)
                        == DOS_EXTMEM_REGION_DIRECT,
                    "the whole HMA -> direct");
    ExtMemTestCheck(DosExtMemClassify(&xmsState, DOS_EXTMEM_DIRECT_END, EXTMEM_TEST_SHORT_LENGTH)
                        == DOS_EXTMEM_REGION_RAW,
                    "first byte past the HMA -> raw");
    ExtMemTestCheck(DosExtMemClassify(&xmsState, DOS_EXTMEM_DIRECT_END - EXTMEM_TEST_SHORT_LENGTH,
                                      EXTMEM_TEST_STRADDLE_LENGTH) == DOS_EXTMEM_REGION_NONE,
                    "straddling HMA/raw -> refused");
    ExtMemTestCheck(DosExtMemClassify(&xmsState, DOS_EXTMEM_RAW_END - EXTMEM_TEST_TAIL_LENGTH,
                                      EXTMEM_TEST_TAIL_LENGTH) == DOS_EXTMEM_REGION_RAW,
                    "last 256 bytes below 16 MB -> raw");
    ExtMemTestCheck(DosExtMemClassify(&xmsState, DOS_EXTMEM_RAW_END - EXTMEM_TEST_TAIL_LENGTH,
                                      EXTMEM_TEST_TAIL_LENGTH + 1) == DOS_EXTMEM_REGION_NONE,
                    "one byte past 16 MB -> refused");
    ExtMemTestCheck(DosExtMemClassify(&xmsState, EXTMEM_TEST_EMB_BASE, EXTMEM_TEST_EMB_BYTES)
                        == DOS_EXTMEM_REGION_EMB,
                    "whole EMB -> the block");
    ExtMemTestCheck(DosExtMemClassify(&xmsState, EXTMEM_TEST_EMB_TAIL, EXTMEM_TEST_TAIL_LENGTH)
                        == DOS_EXTMEM_REGION_EMB,
                    "EMB tail -> the block");
    ExtMemTestCheck(DosExtMemClassify(&xmsState, EXTMEM_TEST_EMB_TAIL, EXTMEM_TEST_TAIL_LENGTH + 1)
                        == DOS_EXTMEM_REGION_NONE,
                    "past the EMB's end -> refused");
    ExtMemTestCheck(DosExtMemClassify(&xmsState, EXTMEM_TEST_EMB_BASE - 1,
                                      EXTMEM_TEST_BOUNDARY_LENGTH) == DOS_EXTMEM_REGION_NONE,
                    "just before the EMB -> refused");
    ExtMemTestCheck(DosExtMemClassify(&xmsState, EXTMEM_TEST_UNOWNED_ADDRESS,
                                      EXTMEM_TEST_SHORT_LENGTH) == DOS_EXTMEM_REGION_NONE,
                    "arbitrary high address -> refused");
    ExtMemTestCheck(DosExtMemClassify(&xmsState, EXTMEM_TEST_WRAPPING_ADDRESS,
                                      EXTMEM_TEST_STRADDLE_LENGTH) == DOS_EXTMEM_REGION_NONE,
                    "wraps 4 GB -> refused");
    ExtMemTestCheck(DosExtMemClassify(&xmsState, EXTMEM_TEST_CONVENTIONAL_ADDRESS, 0)
                        == DOS_EXTMEM_REGION_NONE,
                    "zero length -> refused");
    xmsState.Handles[EXTMEM_TEST_EMB_SLOT].InUse = 0;
    ExtMemTestCheck(DosExtMemClassify(&xmsState, EXTMEM_TEST_EMB_BASE, EXTMEM_TEST_SHORT_LENGTH)
                        == DOS_EXTMEM_REGION_NONE,
                    "a FREED block is not memory");

    ExtMemTestCheck(DosExtMemResolve(&xmsState, (UINT_PTR)conventional, rawBuffer,
                                     EXTMEM_TEST_DIRECT_ADDRESS, EXTMEM_TEST_WORD_LENGTH)
                        == conventional + EXTMEM_TEST_DIRECT_ADDRESS,
                    "direct resolves into guest memory");
    ExtMemTestCheck(DosExtMemResolve(&xmsState, (UINT_PTR)conventional, rawBuffer,
                                     EXTMEM_TEST_RAW_ADDRESS, EXTMEM_TEST_WORD_LENGTH)
                        == rawBuffer + (EXTMEM_TEST_RAW_ADDRESS - DOS_EXTMEM_RAW_BASE),
                    "raw 2 MB -> raw buffer offset 1 MB");
    ExtMemTestCheck(DosExtMemResolve(&xmsState, (UINT_PTR)conventional, 0,
                                     EXTMEM_TEST_RAW_ADDRESS, EXTMEM_TEST_WORD_LENGTH) == 0,
                    "raw with no buffer yet -> 0 (caller allocates)");

    {   BYTE descriptor[DOS_EXTMEM_DESCRIPTOR_SIZE] = EXTMEM_TEST_DESCRIPTOR;
        ExtMemTestCheck(DosExtMemDescriptorBase(descriptor) == EXTMEM_TEST_DESCRIPTOR_BASE,
                        "GDT base: bytes 2-4 and 7"); }

    free(rawBuffer);
    printf("\n%d checks, %d failed\n", g_Checks, g_Failures);
    return g_Failures ? 1 : 0;
}
