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
#include <string.h>
#include "dos_mcb.h"
#include "dos_layout.h"     /* #207: DOS_ENV_SEG / SysVars / DOS_CTAB_SEG vs the chain */
#include "dos_loader.h"
#include "dos_psp.h"
#include "dos_env.h"

/* The chain DosMcbInitialize lays down (#207), by MCB paragraph. */
#define MCB_TEST_MEMORY_SIZE      0x100000  /* 1MB flat "conventional memory" buffer      */
#define MCB_TEST_FIRST_MCB        0x7E      /* the env block                              */
#define MCB_TEST_DOS_BLOCK        0x8F      /* DOS's own block                            */
#define MCB_TEST_DOS_PARAS        0x6F
#define MCB_TEST_PROGRAM_MCB      0xFF      /* the program block, in front of the PSP     */
#define MCB_TEST_PSP              0x0100
#define MCB_TEST_OLD_FIRST_MCB    0x5F      /* where the chain started before #207        */
#define MCB_TEST_OLD_DOS_PARAS    0x8E
#define MCB_TEST_WALK_LIMIT       32        /* dump at most this many blocks              */
#define MCB_TEST_PRINTABLE_FIRST  32
#define MCB_TEST_PRINTABLE_END    127
#define MCB_TEST_POISON           0xDEAD    /* so a value the allocator forgets is visible */

/* The sizes each step asks for, in paragraphs. */
#define MCB_TEST_CDS_PARAS        0x8F
#define MCB_TEST_SECOND_RESERVE   0x20
#define MCB_TEST_SHRUNK           0x1000
#define MCB_TEST_ALLOC            0x80
#define MCB_TEST_GROWN            0x2000
#define MCB_TEST_TOO_BIG          0x3000    /* past the owned neighbour                  */
#define MCB_TEST_SMALL_ALLOC      0x10
#define MCB_TEST_NEIGHBOUR_ALLOC  0x100
#define MCB_TEST_HUGE             0xFFFF
#define MCB_TEST_BAD_SEGMENT      0x0001
#define MCB_TEST_TAIL_MCB         0x1100    /* the freed tail after the 0x1000 shrink     */
#define MCB_TEST_TAIL_DATA        0x1101
#define MCB_TEST_SPLIT_MCB        0x1181
#define MCB_TEST_GROWN_TAIL_MCB   0x2100
#define MCB_TEST_NEIGHBOUR_DATA   0x2101
#define MCB_TEST_MARK_FIRST       0xA5      /* marks in the first reservation's data      */
#define MCB_TEST_MARK_LAST        0x5A

/* T9's mini-chain. */
#define MCB_TEST_MINI_SIZE        0x10000
#define MCB_TEST_MINI_FIRST       0x0300
#define MCB_TEST_MINI_SECOND      0x0321
#define MCB_TEST_MINI_LAST        0x0342
#define MCB_TEST_MINI_FREE_PARAS  0x20
#define MCB_TEST_MINI_LAST_PARAS  0x10
#define MCB_TEST_MINI_ALLOC       0x21
#define MCB_TEST_MINI_DATA        0x0301
#define MCB_TEST_MINI_TAIL        0x0322
#define MCB_TEST_MINI_TAIL_PARAS  0x1F
#define MCB_TEST_MINI_TOP         0x0353

/* The PSP, loader and environment checks. */
#define MCB_TEST_IMAGE_SIZE       0x20000
#define MCB_TEST_ENV_SEGMENT      0x0060
#define MCB_TEST_TOP_640K         0xA000
#define MCB_TEST_COM_LAST_INDEX   4
#define MCB_TEST_EXE_IP           0x0005
#define MCB_TEST_EXE_SP           0x0100
#define MCB_TEST_EXE_IMAGE_BYTES  2
#define MCB_TEST_ENV_SIZE         0x1000
#define MCB_TEST_SMALL_ENV_SIZE   0x400
#define MCB_TEST_ENV_SEGMENT_ZERO 0x0000
#define MCB_TEST_PROGRAM_PATH     "C:\\T.COM"
#define MCB_TEST_PATH_LENGTH      8         /* strlen("C:\T.COM")                         */
#define MCB_TEST_BLASTER          "BLASTER="
#define MCB_TEST_NAME_LENGTH      8         /* strlen("BLASTER=") and strlen("DOS4GVM=")  */
#define MCB_TEST_DOS4GVM_LENGTH   16        /* strlen("DOS4GVM=@ZAR.VMC")                 */
#define MCB_TEST_SHORT_PREFIX     4         /* strlen("A=1") + its NUL; strlen("PAD=")    */
#define MCB_TEST_TAIL_MEMORY_SIZE 0x2000
#define MCB_TEST_HELLO_LENGTH     6         /* " HELLO"                                   */
#define MCB_TEST_HELLO_LAST       (DOS_PSP_COMMAND_TAIL + 5)

static BYTE g_Memory[MCB_TEST_MEMORY_SIZE];          /* 1MB flat "conventional memory" buffer */

static INT g_Total = 0, g_Failures = 0;

static VOID McbTestCheck(BOOL passed, PCSTR description)
{
    g_Total++;
    if (passed) { printf("  PASS  %s\n", description); }
    else        { printf("  FAIL  %s\n", description); g_Failures++; }
}

static BYTE McbTestSignature(WORD mcbSegment) { return g_Memory[(DWORD)mcbSegment << DOS_PARAGRAPH_SHIFT]; }
static WORD McbTestOwner(WORD mcbSegment) { return DosMcbReadWord(g_Memory + (((DWORD)mcbSegment << DOS_PARAGRAPH_SHIFT) + DOS_MCB_OWNER)); }
static WORD McbTestSize(WORD mcbSegment) { return DosMcbReadWord(g_Memory + (((DWORD)mcbSegment << DOS_PARAGRAPH_SHIFT) + DOS_MCB_SIZE)); }

static VOID McbTestDumpChain(WORD firstMcb) {
    WORD mcbSegment = firstMcb; INT walkCount = 0;
    printf("  chain:");
    for (;;) {
        BYTE signature = McbTestSignature(mcbSegment); WORD owner = McbTestOwner(mcbSegment), blockSize = McbTestSize(mcbSegment);
        printf(" [%04X %c o=%04X sz=%04X]", mcbSegment,
               (signature >= MCB_TEST_PRINTABLE_FIRST && signature < MCB_TEST_PRINTABLE_END) ? signature : '?', owner, blockSize);
        if (signature == DOS_MCB_LAST || ++walkCount > MCB_TEST_WALK_LIMIT) break;
        mcbSegment = (WORD)(mcbSegment + 1 + blockSize);
    }
    printf("\n");
}

/* Find `name` in a built environment block; 0 if absent. */
static INT McbTestFind(PCBYTE block, INT blockSize, PCSTR name, INT nameLength) {
    INT index;
    for (index = 0; index < blockSize - nameLength; ++index)
        if (memcmp(block + index, name, nameLength) == 0) return index;
    return 0;
}

INT main(VOID) {
    WORD firstMcb = DosMcbInitialize(g_Memory);
    WORD segment = 0, largest = 0;
    INT status;

    printf("== M2.4 MCB allocator battery ==\n");

    /* ── SIZES ARE DERIVED FROM DOS_MEM_TOP, NOT TYPED IN. ────────────────────
         They were literals (0x9F00, 0x8EFF, 0x8E7E, 0x7EFF), every one of them a
         restatement of "conventional memory ends at 0xA000" -- so moving the top
         to the real EBDA boundary broke six checks that were not testing the
         allocator at all. Derived, they follow the map instead of contradicting
         it, and a future move of DOS_MEM_TOP changes one line.
       The top itself is MEASURED: MS-DOS 6.22's own chain ends at 0x9FC0 and its
       PSP+0x02 reads 0x9FC0 (tests/probes/dos/p_mcb.asm, p_psp.asm) -- 640K less the
       1KB Extended BIOS Data Area, which is why real MEM reports 639K. */
    {
    const UINT programParas  = DOS_MEM_TOP - DOS_PSP_SEG;                       /* the program block      */
    const UINT tailParas     = programParas - MCB_TEST_SHRUNK - 1;              /* after a 0x1000 shrink  */
    const UINT splitParas    = tailParas - MCB_TEST_ALLOC - 1;                  /* after a 0x80 alloc     */
    const UINT grownParas    = programParas - MCB_TEST_GROWN - 1;               /* after growing to 0x2000 */

    /* T0: initial chain ---------------------------------------------------- */
    /* #207: the chain starts at 0x7E, ABOVE SysVars' segment (0x72), as 6.22's does
       (SysVars 0116, first MCB 0253). It was 0x5F, which made MEM /D's "MSDOS System
       Data" row (SysVars seg .. first MCB) negative. */
    McbTestCheck(firstMcb == MCB_TEST_FIRST_MCB, "init: chain root at 0x7E (#207)");
    McbTestCheck(DosMcbCheckChain(g_Memory, firstMcb, DOS_MEM_TOP) == DOS_MCB_CHAIN_OK, "init: chain consistent");
    McbTestCheck(McbTestSignature(MCB_TEST_PROGRAM_MCB) == DOS_MCB_LAST && McbTestOwner(MCB_TEST_PROGRAM_MCB) == DOS_PSP_SEG && McbTestSize(MCB_TEST_PROGRAM_MCB) == programParas,
                 "init: program block = Z / PSP / DOS_MEM_TOP-PSP (unchanged by #207)");
    McbTestCheck(McbTestSignature(MCB_TEST_FIRST_MCB) == DOS_MCB_MEMBER && McbTestOwner(MCB_TEST_FIRST_MCB) == DOS_PSP_SEG && McbTestSize(MCB_TEST_FIRST_MCB) == DOS_ENV_PARAS,
                 "init: env block = M / PSP / 0x10, data at DOS_ENV_SEG 0x7F");
    McbTestCheck(DOS_ENV_SEG == firstMcb + 1, "init: DOS_ENV_SEG is the first block's data");
    McbTestCheck(McbTestSignature(MCB_TEST_DOS_BLOCK) == DOS_MCB_MEMBER && McbTestOwner(MCB_TEST_DOS_BLOCK) == DOS_MCB_OWNER_DOS && McbTestSize(MCB_TEST_DOS_BLOCK) == MCB_TEST_DOS_PARAS,
                 "init: DOS block = M / 8 / 0x6F");
    McbTestCheck(DOS_CTAB_SEG == MCB_TEST_DOS_BLOCK + 1, "init: the DOS block's data starts at DOS_CTAB_SEG");
    /* MEM /C's MSDOS total = the kernel area below the first MCB + the owner-8 block.
       #207 moved 0x1F paragraphs from the block to the area; the sum must not move:
       old 0x5F + (0x8E+1) == new 0x7E + (0x6F+1). */
    McbTestCheck(MCB_TEST_OLD_FIRST_MCB + MCB_TEST_OLD_DOS_PARAS + 1 == firstMcb + McbTestSize(MCB_TEST_DOS_BLOCK) + 1,
                 "init: kernel area + DOS block = the pre-#207 total (MEM /C's MSDOS)");
    /* SysVars, its -2 word and the SDA are kernel data below the chain now. */
    McbTestCheck((DWORD)DOS_SYSVARS_SEG < firstMcb
                 && (DWORD)DOS_SYSVARS_SEG * DOS_PARAGRAPH_BYTES + DOS_SYSVARS_OFF + DOS_SYSVARS_LEN <= (DWORD)firstMcb * DOS_PARAGRAPH_BYTES
                 && (DWORD)DOS_SDA_SEG * DOS_PARAGRAPH_BYTES + DOS_SDA_OFF + DOS_SDA_LEN <= (DWORD)firstMcb * DOS_PARAGRAPH_BYTES,
                 "init: SysVars + SDA end below the first MCB header (#207)");

    /* T0b: a block reserved at the TOP for resident DOS data (the CDS array) --
       the program block shrinks by paragraphs+1, the chain still ends at DOS_MEM_TOP,
       and the new last block is DOS's. Then put the chain back for the rest. */
    {   WORD cdsSegment = DosMcbReserveTop(g_Memory, firstMcb, MCB_TEST_CDS_PARAS);
        McbTestCheck(cdsSegment == DOS_MEM_TOP - MCB_TEST_CDS_PARAS, "reserve_top: data segment = TOP - paras");
        McbTestCheck(DosMcbCheckChain(g_Memory, firstMcb, DOS_MEM_TOP) == DOS_MCB_CHAIN_OK, "reserve_top: chain still consistent");
        McbTestCheck(McbTestSignature(MCB_TEST_PROGRAM_MCB) == DOS_MCB_MEMBER && McbTestSize(MCB_TEST_PROGRAM_MCB) == programParas - MCB_TEST_CDS_PARAS - 1,
                     "reserve_top: program block is M and paras+1 smaller");
        McbTestCheck(McbTestSignature(cdsSegment - 1) == DOS_MCB_LAST && McbTestOwner(cdsSegment - 1) == DOS_MCB_OWNER_DOS && McbTestSize(cdsSegment - 1) == MCB_TEST_CDS_PARAS,
                     "reserve_top: the reserved block is Z / DOS / paras");
        /* #169: a SECOND reservation must not eat the first. Mark the first block's
           data, reserve again, and the mark must survive with both blocks intact. */
        {   volatile BYTE *firstData = DosMcbSegmentAddress(g_Memory, cdsSegment);
            WORD secondSegment;
            firstData[0] = MCB_TEST_MARK_FIRST; firstData[MCB_TEST_CDS_PARAS * DOS_PARAGRAPH_BYTES - 1] = MCB_TEST_MARK_LAST;
            secondSegment = DosMcbReserveTop(g_Memory, firstMcb, MCB_TEST_SECOND_RESERVE);
            McbTestCheck(secondSegment == cdsSegment - 1 - MCB_TEST_SECOND_RESERVE, "reserve_top x2: second block sits just below the first");
            McbTestCheck(DosMcbCheckChain(g_Memory, firstMcb, DOS_MEM_TOP) == DOS_MCB_CHAIN_OK, "reserve_top x2: chain consistent");
            McbTestCheck(McbTestSignature(secondSegment - 1) == DOS_MCB_MEMBER && McbTestOwner(secondSegment - 1) == DOS_MCB_OWNER_DOS && McbTestSize(secondSegment - 1) == MCB_TEST_SECOND_RESERVE,
                         "reserve_top x2: new block is M / DOS / paras");
            McbTestCheck(McbTestSignature(cdsSegment - 1) == DOS_MCB_LAST && McbTestSize(cdsSegment - 1) == MCB_TEST_CDS_PARAS && firstData[0] == MCB_TEST_MARK_FIRST
                         && firstData[MCB_TEST_CDS_PARAS * DOS_PARAGRAPH_BYTES - 1] == MCB_TEST_MARK_LAST, "reserve_top x2: the first block is untouched");
            McbTestCheck(McbTestSize(MCB_TEST_PROGRAM_MCB) == programParas - MCB_TEST_CDS_PARAS - 1 - MCB_TEST_SECOND_RESERVE - 1,
                         "reserve_top x2: program block shrank by the second reservation");
        }
        firstMcb = DosMcbInitialize(g_Memory);
    }

    /* T1: alloc on a fresh chain must fail (everything is owned) ------------ */
    largest = MCB_TEST_POISON;
    status = DosMcbAllocate(g_Memory, firstMcb, MCB_TEST_SMALL_ALLOC, &segment, &largest);
    McbTestCheck(status == DOS_MCB_ERROR_INSUFFICIENT_MEMORY && largest == 0, "alloc on fresh chain fails, max=0 (all owned)");

    /* T2: program shrinks its own block -- the .EXE-startup pattern --------- */
    status = DosMcbResize(g_Memory, MCB_TEST_PSP, MCB_TEST_SHRUNK, &largest);
    McbTestCheck(status == DOS_MCB_SUCCESS, "resize: program shrinks to 0x1000");
    McbTestCheck(McbTestSignature(MCB_TEST_PROGRAM_MCB) == DOS_MCB_MEMBER && McbTestSize(MCB_TEST_PROGRAM_MCB) == MCB_TEST_SHRUNK, "shrink: block now M / 0x1000");
    McbTestCheck(McbTestSignature(MCB_TEST_TAIL_MCB) == DOS_MCB_LAST && McbTestOwner(MCB_TEST_TAIL_MCB) == DOS_MCB_OWNER_FREE && McbTestSize(MCB_TEST_TAIL_MCB) == tailParas,
                 "shrink: freed tail = Z / free / derived");
    McbTestCheck(DosMcbCheckChain(g_Memory, firstMcb, DOS_MEM_TOP) == DOS_MCB_CHAIN_OK, "after shrink: chain consistent");

    /* T3: allocate from the freed tail (split) ----------------------------- */
    status = DosMcbAllocate(g_Memory, firstMcb, MCB_TEST_ALLOC, &segment, &largest);
    McbTestCheck(status == DOS_MCB_SUCCESS && segment == MCB_TEST_TAIL_DATA, "alloc 0x80 -> seg 0x1101");
    McbTestCheck(McbTestSignature(MCB_TEST_TAIL_MCB) == DOS_MCB_MEMBER && McbTestOwner(MCB_TEST_TAIL_MCB) == DOS_PSP_SEG && McbTestSize(MCB_TEST_TAIL_MCB) == MCB_TEST_ALLOC,
                 "alloc: block = M / PSP / 0x80");
    McbTestCheck(McbTestSignature(MCB_TEST_SPLIT_MCB) == DOS_MCB_LAST && McbTestOwner(MCB_TEST_SPLIT_MCB) == DOS_MCB_OWNER_FREE && McbTestSize(MCB_TEST_SPLIT_MCB) == splitParas,
                 "alloc: split tail = Z / free / derived");
    McbTestCheck(DosMcbCheckChain(g_Memory, firstMcb, DOS_MEM_TOP) == DOS_MCB_CHAIN_OK, "after alloc: chain consistent");

    /* T3b: a too-large alloc fails and reports the largest free block ------ */
    largest = 0;
    status = DosMcbAllocate(g_Memory, firstMcb, MCB_TEST_HUGE, &segment, &largest);
    McbTestCheck(status == DOS_MCB_ERROR_INSUFFICIENT_MEMORY && largest == splitParas, "alloc 0xFFFF fails, max = largest free block");

    /* T4: free + forward coalesce back to one free block ------------------- */
    status = DosMcbFree(g_Memory, MCB_TEST_TAIL_DATA);
    McbTestCheck(status == DOS_MCB_SUCCESS, "free seg 0x1101 ok");
    McbTestCheck(McbTestSignature(MCB_TEST_TAIL_MCB) == DOS_MCB_LAST && McbTestOwner(MCB_TEST_TAIL_MCB) == DOS_MCB_OWNER_FREE && McbTestSize(MCB_TEST_TAIL_MCB) == tailParas,
                 "free: forward-coalesced to Z / free / derived");
    McbTestCheck(DosMcbCheckChain(g_Memory, firstMcb, DOS_MEM_TOP) == DOS_MCB_CHAIN_OK, "after free: chain consistent");

    /* T5: resize grow into the free neighbour ------------------------------ */
    status = DosMcbResize(g_Memory, MCB_TEST_PSP, MCB_TEST_GROWN, &largest);
    McbTestCheck(status == DOS_MCB_SUCCESS, "resize grow 0x1000 -> 0x2000 into free neighbour");
    McbTestCheck(McbTestSignature(MCB_TEST_PROGRAM_MCB) == DOS_MCB_MEMBER && McbTestSize(MCB_TEST_PROGRAM_MCB) == MCB_TEST_GROWN, "grow: block now 0x2000");
    McbTestCheck(McbTestSignature(MCB_TEST_GROWN_TAIL_MCB) == DOS_MCB_LAST && McbTestOwner(MCB_TEST_GROWN_TAIL_MCB) == DOS_MCB_OWNER_FREE && McbTestSize(MCB_TEST_GROWN_TAIL_MCB) == grownParas,
                 "grow: remaining free = Z / derived");
    McbTestCheck(DosMcbCheckChain(g_Memory, firstMcb, DOS_MEM_TOP) == DOS_MCB_CHAIN_OK, "after grow: chain consistent");

    /* T6: grow blocked by an owned neighbour ------------------------------- */
    status = DosMcbAllocate(g_Memory, firstMcb, MCB_TEST_NEIGHBOUR_ALLOC, &segment, &largest);
    McbTestCheck(status == DOS_MCB_SUCCESS && segment == MCB_TEST_NEIGHBOUR_DATA, "alloc 0x100 -> seg 0x2101 (neighbour now owned)");
    largest = 0;
    status = DosMcbResize(g_Memory, MCB_TEST_PSP, MCB_TEST_TOO_BIG, &largest);
    McbTestCheck(status == DOS_MCB_ERROR_INSUFFICIENT_MEMORY && largest == MCB_TEST_GROWN, "grow blocked by owned neighbour, max = cur 0x2000");
    McbTestCheck(McbTestSignature(MCB_TEST_PROGRAM_MCB) == DOS_MCB_MEMBER && McbTestSize(MCB_TEST_PROGRAM_MCB) == MCB_TEST_GROWN, "blocked grow leaves block unchanged");

    /* T7: invalid block segments ------------------------------------------- */
    McbTestCheck(DosMcbFree(g_Memory, MCB_TEST_BAD_SEGMENT) == DOS_MCB_ERROR_INVALID_BLOCK, "free bad block -> err 9");
    McbTestCheck(DosMcbResize(g_Memory, MCB_TEST_BAD_SEGMENT, MCB_TEST_SMALL_ALLOC, &largest) == DOS_MCB_ERROR_INVALID_BLOCK, "resize bad block -> err 9");

    /* T8: resize to the current size is a no-op success -------------------- */
    status = DosMcbResize(g_Memory, MCB_TEST_PSP, MCB_TEST_GROWN, &largest);
    McbTestCheck(status == DOS_MCB_SUCCESS && McbTestSize(MCB_TEST_PROGRAM_MCB) == MCB_TEST_GROWN, "resize to same size is a no-op success");
    McbTestCheck(DosMcbCheckChain(g_Memory, firstMcb, DOS_MEM_TOP) == DOS_MCB_CHAIN_OK, "final: chain consistent");

    /* T9: merge-on-alloc. DosMcbAllocate() coalesces adjacent free blocks during the walk
     * (as real MS-DOS does), so two adjacent free blocks jointly satisfy a request
     * that neither satisfies alone. (Previously a pinned gap; now closed.) */
    {
        static BYTE miniMemory[MCB_TEST_MINI_SIZE];
        DosMcbWriteHeader(miniMemory, MCB_TEST_MINI_FIRST, DOS_MCB_MEMBER, DOS_MCB_OWNER_FREE, MCB_TEST_MINI_FREE_PARAS);   /* free block #1            */
        DosMcbWriteHeader(miniMemory, MCB_TEST_MINI_SECOND, DOS_MCB_MEMBER, DOS_MCB_OWNER_FREE, MCB_TEST_MINI_FREE_PARAS);  /* free block #2 (adjacent) */
        DosMcbWriteHeader(miniMemory, MCB_TEST_MINI_LAST, DOS_MCB_LAST, DOS_PSP_SEG, MCB_TEST_MINI_LAST_PARAS);            /* owned terminator         */
        segment = largest = 0;
        status = DosMcbAllocate(miniMemory, MCB_TEST_MINI_FIRST, MCB_TEST_MINI_ALLOC, &segment, &largest);
        /* merged = 0x20 + 1 + 0x20 = 0x41 paras; alloc 0x21 splits it, leaving a
         * 0x41 - 0x21 - 1 = 0x1F free tail at paragraph 0x322. */
        McbTestCheck(status == DOS_MCB_SUCCESS && segment == MCB_TEST_MINI_DATA,
                     "merge-on-alloc: adjacent free blocks merged to satisfy 0x21");
        McbTestCheck(miniMemory[(DWORD)MCB_TEST_MINI_FIRST << DOS_PARAGRAPH_SHIFT] == DOS_MCB_MEMBER
                     && DosMcbReadWord(miniMemory + (((DWORD)MCB_TEST_MINI_FIRST << DOS_PARAGRAPH_SHIFT) + DOS_MCB_OWNER)) == DOS_PSP_SEG
                     && DosMcbReadWord(miniMemory + (((DWORD)MCB_TEST_MINI_FIRST << DOS_PARAGRAPH_SHIFT) + DOS_MCB_SIZE)) == MCB_TEST_MINI_ALLOC,
                     "merge-on-alloc: allocated block = M / PSP / 0x21");
        McbTestCheck(miniMemory[(DWORD)MCB_TEST_MINI_TAIL << DOS_PARAGRAPH_SHIFT] == DOS_MCB_MEMBER
                     && DosMcbReadWord(miniMemory + (((DWORD)MCB_TEST_MINI_TAIL << DOS_PARAGRAPH_SHIFT) + DOS_MCB_OWNER)) == DOS_MCB_OWNER_FREE
                     && DosMcbReadWord(miniMemory + (((DWORD)MCB_TEST_MINI_TAIL << DOS_PARAGRAPH_SHIFT) + DOS_MCB_SIZE)) == MCB_TEST_MINI_TAIL_PARAS,
                     "merge-on-alloc: free tail = M / free / 0x1F");
        McbTestCheck(DosMcbCheckChain(miniMemory, MCB_TEST_MINI_FIRST, MCB_TEST_MINI_TOP) == DOS_MCB_CHAIN_OK,
                     "merge-on-alloc: mini-chain consistent");
    }

    /* T10: PSP builder (src/dos/dos_psp.h) --------------------------------- */
    {
        static BYTE pspMemory[MCB_TEST_IMAGE_SIZE];
        volatile BYTE *psp = pspMemory + ((DWORD)MCB_TEST_PSP << DOS_PARAGRAPH_SHIFT);
        DosPspBuild(pspMemory, MCB_TEST_PSP, MCB_TEST_ENV_SEGMENT, MCB_TEST_TOP_640K);
        McbTestCheck(psp[DOS_PSP_INT20] == DOS_PSP_OPCODE_INT && psp[DOS_PSP_INT20 + 1] == DOS_PSP_INT20_VECTOR, "psp: INT 20h at offset 0");
        McbTestCheck(DosMcbReadWord(psp + DOS_PSP_MEMORY_TOP) == MCB_TEST_TOP_640K, "psp: top-of-mem segment = 0xA000");
        McbTestCheck(DosMcbReadWord(psp + DOS_PSP_ENVIRONMENT) == MCB_TEST_ENV_SEGMENT, "psp: environment segment = 0x60");
        McbTestCheck(psp[DOS_PSP_DISPATCH] == DOS_PSP_OPCODE_INT && psp[DOS_PSP_DISPATCH + 1] == DOS_PSP_INT21_VECTOR && psp[DOS_PSP_DISPATCH + 2] == DOS_PSP_OPCODE_RETF,
                     "psp: INT 21h;RETF dispatch stub at 0x50");
        McbTestCheck(psp[DOS_PSP_COMMAND_TAIL_LENGTH] == 0 && psp[DOS_PSP_COMMAND_TAIL] == DOS_PSP_COMMAND_TAIL_END, "psp: empty command tail + 0x0D");
    }

    /* T11: flat .COM loader (src/dos/dos_loader.h) ------------------------- */
    {
        static BYTE comMemory[MCB_TEST_IMAGE_SIZE];
        static const BYTE comFile[] = { 0xB4, 0x09, 0xCD, 0x21, 0xC3 };   /* mov ah,09 ; int 21h ; ret */
        volatile BYTE *code = comMemory + ((DWORD)MCB_TEST_PSP << DOS_PARAGRAPH_SHIFT) + DOS_COM_ENTRY;
        DOS_IMAGE image = DosLoadImage(comMemory, comFile, (DWORD)sizeof(comFile), MCB_TEST_PSP);
        McbTestCheck(!image.IsExe && image.CodeSegment == MCB_TEST_PSP && image.InstructionPointer == DOS_COM_ENTRY
                     && image.StackSegment == MCB_TEST_PSP && image.StackPointer == DOS_COM_INITIAL_SP,
                     ".COM: entry CS=SS=0x100, IP=0x100, SP=0xFFFE");
        McbTestCheck(code[0] == comFile[0] && code[1] == comFile[1] && code[MCB_TEST_COM_LAST_INDEX] == comFile[MCB_TEST_COM_LAST_INDEX]
                     && image.ImageSize == sizeof(comFile),
                     ".COM: image placed at PSP:0x100");
    }

    /* T12: MZ .EXE loader + one relocation --------------------------------- */
    {
        static BYTE exeMemory[MCB_TEST_IMAGE_SIZE];
        /* 34-byte MZ: 32-byte header (e_cparhdr=2, e_crlc=1, reloc tbl @0x1C,
         * e_ip=5, e_sp=0x100), one reloc -> image word at offset 0, 2-byte image
         * = 0x0000 (to be fixed up to the load segment). */
        static const BYTE exeFile[] = {
            'M','Z',   0x22,0x00, 0x01,0x00, 0x01,0x00, 0x02,0x00, 0x00,0x00, 0xFF,0xFF,
            0x00,0x00, 0x00,0x01, 0x00,0x00, 0x05,0x00, 0x00,0x00, 0x1C,0x00, 0x00,0x00,
            0x00,0x00, 0x00,0x00,          /* reloc[0] = offset 0, segment 0 */
            0x00,0x00                      /* image[0..1] = 0x0000           */
        };
        WORD loadSegment = (WORD)(MCB_TEST_PSP + DOS_PSP_PARAGRAPHS);          /* 0x110 */
        volatile BYTE *imageBytes = exeMemory + ((DWORD)loadSegment << DOS_PARAGRAPH_SHIFT);
        DOS_IMAGE image = DosLoadImage(exeMemory, exeFile, (DWORD)sizeof(exeFile), MCB_TEST_PSP);
        McbTestCheck(image.IsExe && image.CodeSegment == loadSegment && image.InstructionPointer == MCB_TEST_EXE_IP
                     && image.StackSegment == loadSegment && image.StackPointer == MCB_TEST_EXE_SP,
                     ".EXE: CS:IP/SS:SP from header, biased by the load segment");
        McbTestCheck(image.ImageSize == MCB_TEST_EXE_IMAGE_BYTES && DosMcbReadWord(imageBytes) == loadSegment,
                     ".EXE: relocation fixed the image word to the load segment");
    }

    /* T13: environment block builder (src/dos/dos_env.h) -------------------- */
    {
        static BYTE envMemory[MCB_TEST_ENV_SIZE];
        PCSTR path = MCB_TEST_PROGRAM_PATH;
        INT pathLength = MCB_TEST_PATH_LENGTH, characterIndex;    /* strlen("C:\T.COM") = 8 */
        BOOL isPathIntact = TRUE;
        DWORD blockLength = DosEnvBuild(envMemory, MCB_TEST_ENV_SEGMENT_ZERO, path);
        McbTestCheck(blockLength > 0 && envMemory[0] == 'C' && envMemory[1] == 'O' && envMemory[2] == 'M' && envMemory[3] == 'S',
                     "env: starts with COMSPEC=");
        for (characterIndex = 0; characterIndex < pathLength; ++characterIndex) if (envMemory[blockLength - 1 - pathLength + characterIndex] != (BYTE)path[characterIndex]) isPathIntact = FALSE;
        McbTestCheck(isPathIntact && envMemory[blockLength - 1] == 0, "env: program path is the final ASCIIZ string");
        McbTestCheck(envMemory[blockLength - 1 - pathLength - 2] == DOS_ENV_STRING_COUNT_LOW && envMemory[blockLength - 1 - pathLength - 1] == DOS_ENV_STRING_COUNT_HIGH,
                     "env: WORD count 0x0001 precedes the program path");
        McbTestCheck(envMemory[blockLength - 1 - pathLength - 3] == 0x00, "env: trailing NUL ends the variable list");
    }

    /* T13b: BLASTER tracks the card, because the Audio page can move it -------
       ⚠ Telling a guest the wrong port/IRQ/DMA is worse than telling it nothing:
         a driver that believes the string masks the line it was told about and
         waits for an interrupt that arrives somewhere else. So the string is
         built from the same numbers vdd_sb is configured with, and the DEFAULT
         must still come out byte-for-byte as the literal it replaced. */
    {
        static BYTE envMemory[MCB_TEST_ENV_SIZE];
        DOS_SB_CONFIG card;
        INT found;
        memset(&card, 0, sizeof card);                 /* #231: .MpuBase is new */
        /* Find "BLASTER=" in the built block and compare the rest of that string. */
        DosEnvBuild(envMemory, MCB_TEST_ENV_SEGMENT_ZERO, MCB_TEST_PROGRAM_PATH);
        found = McbTestFind(envMemory, (INT)sizeof envMemory, MCB_TEST_BLASTER, MCB_TEST_NAME_LENGTH);
        McbTestCheck(found > 0, "BLASTER: the variable is in the block");
        McbTestCheck(found > 0 && strcmp((PCSTR)envMemory + found, "BLASTER=A220 I5 D1 T3") == 0,
                     "BLASTER: no card supplied -> the literal this host always claimed");

        card.IoBase = 0x240; card.Irq = 7; card.Dma8Channel = 3; card.Dma16Channel = 0; card.Type = 3;
        memset(envMemory, 0, sizeof envMemory);
        DosEnvBuildWithCard(envMemory, MCB_TEST_ENV_SEGMENT_ZERO, MCB_TEST_PROGRAM_PATH, DOS_ENV_DEFAULT_PATH, &card, NULL);
        found = McbTestFind(envMemory, (INT)sizeof envMemory, MCB_TEST_BLASTER, MCB_TEST_NAME_LENGTH);
        McbTestCheck(found > 0 && strcmp((PCSTR)envMemory + found, "BLASTER=A240 I7 D3 T3") == 0,
                     "BLASTER: a moved card is reported at its real port, IRQ and channel");

        /* A 16-bit channel is advertised as H, and ONLY when one is set -- the
           default card has one and has never mentioned it, and Doom's audio is
           user-confirmed against the string without it. */
        card.IoBase = 0x220; card.Irq = 5; card.Dma8Channel = 1; card.Dma16Channel = 5; card.Type = 3;
        memset(envMemory, 0, sizeof envMemory);
        DosEnvBuildWithCard(envMemory, MCB_TEST_ENV_SEGMENT_ZERO, MCB_TEST_PROGRAM_PATH, DOS_ENV_DEFAULT_PATH, &card, NULL);
        found = McbTestFind(envMemory, (INT)sizeof envMemory, MCB_TEST_BLASTER, MCB_TEST_NAME_LENGTH);
        McbTestCheck(found > 0 && strcmp((PCSTR)envMemory + found, "BLASTER=A220 I5 D1 H5 T3") == 0,
                     "BLASTER: a 16-bit channel appears as H, and only when it is set");

        /* Two digits must not be truncated to one, and a base is three hex
           digits with no 0x -- both are how a driver parses it. */
        card.IoBase = 0x280; card.Irq = 10; card.Dma8Channel = 1; card.Dma16Channel = 0; card.Type = 6;
        memset(envMemory, 0, sizeof envMemory);
        DosEnvBuildWithCard(envMemory, MCB_TEST_ENV_SEGMENT_ZERO, MCB_TEST_PROGRAM_PATH, DOS_ENV_DEFAULT_PATH, &card, NULL);
        found = McbTestFind(envMemory, (INT)sizeof envMemory, MCB_TEST_BLASTER, MCB_TEST_NAME_LENGTH);
        McbTestCheck(found > 0 && strcmp((PCSTR)envMemory + found, "BLASTER=A280 I10 D1 T6") == 0,
                     "BLASTER: a two-digit IRQ survives, and the base is three hex digits");

        /* ── EXTRA VARIABLES (dosenv.txt). The guest that needed this is ZAR, whose
             own RUNZAR.BAT sets DOS4GVM before launching -- i.e. the game's supported
             way to start it configures the extender through the environment, and we
             had no way to pass one. */
        {
            static BYTE emptyExtraMemory[MCB_TEST_SMALL_ENV_SIZE];
            INT baseLength, withLength, byteIndex;

            card.IoBase = DOS_SB_DEFAULT_IO_BASE; card.Irq = DOS_SB_DEFAULT_IRQ; card.Dma8Channel = DOS_SB_DEFAULT_DMA8;
            card.Dma16Channel = DOS_SB_NOT_ADVERTISED; card.Type = DOS_SB_DEFAULT_TYPE;

            /* ⚠ THE DEFAULT MUST BE BYTE-IDENTICAL. A knob nobody sets must not
                 change the environment every existing guest already runs against. */
            memset(envMemory, 0, sizeof envMemory);
            baseLength = (INT)DosEnvBuildWithCard(envMemory, MCB_TEST_ENV_SEGMENT_ZERO, MCB_TEST_PROGRAM_PATH, DOS_ENV_DEFAULT_PATH, &card, NULL);
            memset(emptyExtraMemory, 0, sizeof emptyExtraMemory);
            (VOID)DosEnvBuildWithCard(emptyExtraMemory, MCB_TEST_ENV_SEGMENT_ZERO, MCB_TEST_PROGRAM_PATH, DOS_ENV_DEFAULT_PATH, &card, "");
            McbTestCheck(memcmp(envMemory, emptyExtraMemory, sizeof emptyExtraMemory) == 0,
                         "dosenv: absent and empty both leave the block byte-identical");

            memset(envMemory, 0, sizeof envMemory);
            withLength = (INT)DosEnvBuildWithCard(envMemory, MCB_TEST_ENV_SEGMENT_ZERO, MCB_TEST_PROGRAM_PATH, DOS_ENV_DEFAULT_PATH, &card,
                                                  "DOS4GVM=@ZAR.VMC");
            found = McbTestFind(envMemory, (INT)sizeof envMemory, "DOS4GVM=", MCB_TEST_NAME_LENGTH);
            McbTestCheck(found > 0 && strcmp((PCSTR)envMemory + found, "DOS4GVM=@ZAR.VMC") == 0,
                         "dosenv: a variable is emitted as its own NUL-terminated string");
            McbTestCheck(withLength == baseLength + MCB_TEST_DOS4GVM_LENGTH + 1,
                         "dosenv: it costs exactly its own length plus the NUL");
            /* The list terminator must still be there, or a guest walking to the
               double NUL runs off into whatever follows -- how krnl386 finds its
               own path, and a defect this project has already paid for once. */
            McbTestCheck(envMemory[found + MCB_TEST_DOS4GVM_LENGTH] == 0 && envMemory[found + MCB_TEST_DOS4GVM_LENGTH + 1] == 0,
                         "dosenv: the double NUL still ends the list after the last extra");

            memset(envMemory, 0, sizeof envMemory);
            (VOID)DosEnvBuildWithCard(envMemory, MCB_TEST_ENV_SEGMENT_ZERO, MCB_TEST_PROGRAM_PATH, DOS_ENV_DEFAULT_PATH, &card,
                                      "# a comment\r\nA=1\r\n\r\nB=2\r\n");
            found = McbTestFind(envMemory, (INT)sizeof envMemory, "A=1", MCB_TEST_SHORT_PREFIX);
            McbTestCheck(found > 0, "dosenv: CRLF lines are split, blank lines skipped");
            McbTestCheck(found > 0 && strcmp((PCSTR)envMemory + found + MCB_TEST_SHORT_PREFIX, "B=2") == 0,
                         "dosenv: the next variable follows immediately after the NUL");
            for (byteIndex = 0, found = 0; byteIndex < (INT)sizeof envMemory - 2; ++byteIndex)
                if (envMemory[byteIndex] == '#') { found = 1; break; }
            McbTestCheck(!found, "dosenv: a '#' line is a comment and never reaches the guest");

            /* An entry that does not fit is dropped WHOLE. Half an environment
               variable is a value, and a wrong one -- worse than an absent one. */
            memset(envMemory, 0, sizeof envMemory);
            (VOID)DosEnvBuildWithCard(envMemory, MCB_TEST_ENV_SEGMENT_ZERO, MCB_TEST_PROGRAM_PATH, DOS_ENV_DEFAULT_PATH, &card,
                                      "PAD=012345678901234567890123456789012345678901234567890"
                                      "12345678901234567890123456789012345678901234567890"
                                      "12345678901234567890123456789012345678901234567890"
                                      "12345678901234567890123456789012345678901234567890;Z=1");
            found = McbTestFind(envMemory, (INT)sizeof envMemory, "PAD=", MCB_TEST_SHORT_PREFIX);
            McbTestCheck(!found, "dosenv: an entry that cannot fit is dropped whole, not clipped");
        }
    }

    /* T14: PSP command-tail builder (src/dos/dos_psp.h) --------------------- */
    {
        static BYTE tailMemory[MCB_TEST_TAIL_MEMORY_SIZE];
        volatile BYTE *psp = tailMemory + ((DWORD)MCB_TEST_PSP << DOS_PARAGRAPH_SHIFT);
        DosPspBuildCommandTail(tailMemory, MCB_TEST_PSP, "HELLO");
        McbTestCheck(psp[DOS_PSP_COMMAND_TAIL_LENGTH] == MCB_TEST_HELLO_LENGTH && psp[DOS_PSP_COMMAND_TAIL] == ' ' && psp[DOS_PSP_COMMAND_TAIL + 1] == 'H'
                     && psp[MCB_TEST_HELLO_LAST] == 'O' && psp[MCB_TEST_HELLO_LAST + 1] == DOS_PSP_COMMAND_TAIL_END,
                     "cmdtail: \"HELLO\" -> len 6, \" HELLO\", 0x0D");
        DosPspBuildCommandTail(tailMemory, MCB_TEST_PSP, "");
        McbTestCheck(psp[DOS_PSP_COMMAND_TAIL_LENGTH] == 0 && psp[DOS_PSP_COMMAND_TAIL] == DOS_PSP_COMMAND_TAIL_END, "cmdtail: empty -> len 0, 0x0D");
        DosPspBuildCommandTail(tailMemory, MCB_TEST_PSP, (PCSTR)0);
        McbTestCheck(psp[DOS_PSP_COMMAND_TAIL_LENGTH] == 0 && psp[DOS_PSP_COMMAND_TAIL] == DOS_PSP_COMMAND_TAIL_END, "cmdtail: NULL -> len 0, 0x0D");
    }

    }

    McbTestDumpChain(firstMcb);
    printf("== %d/%d passed, %d failed ==\n", g_Total - g_Failures, g_Total, g_Failures);
    return g_Failures ? 1 : 0;
}
