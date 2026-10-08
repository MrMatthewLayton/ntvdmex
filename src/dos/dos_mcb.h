/* dos_mcb.h -- DOS conventional-memory allocator (MCB chain), host-testable.
 *
 * The canonical DOS-memory allocator for the clean host (M2.6). The same logic
 * runs in V86 (absolute paragraph<<4 addressing) and off-VM against a plain byte
 * buffer: every routine takes a caller-supplied `base` pointer + paragraph offsets
 * (pass base=NULL for the host's absolute V86 addressing, a buffer for tests).
 * Verified off-VM by tests/unit/mcb_test.c.
 *
 * SYNC: the tools/vdmhost spike still carries an inline copy of AH=48/49/4A (kept
 * in step by hand) until it is retired in favour of this module; the clean host
 * (src/) uses this file directly. Originated as a port of the spike at 4aa6f44.
 *
 * MCB layout (16 bytes, immediately preceding the block it owns):
 *   [0]    signature: 'M' (member) or 'Z' (last block in the chain)
 *   [1..2] owner PSP segment   (0 = free, 8 = DOS, PSP_SEG = our process)
 *   [3..4] size of owned block in paragraphs (the block starts at mcbSegment+1)
 *
 * DOS error codes returned: 8 = insufficient memory, 9 = invalid memory block.
 */
#ifndef NTVDMEX_DOS_MCB_H
#define NTVDMEX_DOS_MCB_H

#include "../ntvdmex_types.h"

#define DOS_PSP_SEG 0x0100u     /* matches vdmhost.c enum PSP_SEG */
#define DOS_PSP_PARAGRAPHS 0x10 /* the PSP's own 256 bytes, in paragraphs */
/* ── WHERE CONVENTIONAL MEMORY ACTUALLY ENDS. (GH #47) ────────────────────────
   0xA000 is 640KB, and it is NOT where a real PC's MCB chain stops. MS-DOS 6.22,
   walked directly (tests/probes/dos/p_mcb.asm):
       CASE=mcb.chain.ends.at SIG=AX AX=9FC0
       CASE=psp.02.memtop     SIG=AX AX=9FC0
   The top 1KB (0x9FC0..0x9FFF) is the Extended BIOS Data Area, which the BIOS
   reserves and DOS never hands out -- which is exactly why MEM reports 639K and
   not 640K. Ours ran the last block all the way to 0xA000 and reported 640K.
   Both numbers are measured; this is the one a real machine gives. */
#define DOS_MEM_TOP 0x9FC0u     /* conventional top in paragraphs: 640K - 1K EBDA */

/* A paragraph is 16 bytes: segment << 4 is its linear address. */
#define DOS_PARAGRAPH_BYTES       16
#define DOS_PARAGRAPH_LAST_BYTE   15     /* added before dividing, to round up to a paragraph */

/* A little-endian WORD: the low byte first, then the high byte. */

/* The MCB's fields (see the layout above). */
#define DOS_MCB_SIGNATURE         0      /* 'M' or 'Z'                                    */
#define DOS_MCB_OWNER             1      /* WORD: the owner's PSP segment                 */
#define DOS_MCB_SIZE              3      /* WORD: the block's size in paragraphs          */
#define DOS_MCB_NAME              8      /* the owner name DOS 4+ writes (8 bytes)        */
#define DOS_MCB_NAME_LENGTH       8
#define DOS_MCB_MEMBER            'M'    /* member                                         */
#define DOS_MCB_LAST              'Z'    /* the last block in the chain                   */
#define DOS_MCB_OWNER_FREE        0
#define DOS_MCB_OWNER_DOS         0x0008
#define DOS_MCB_WALK_LIMIT        0x1000 /* more blocks than this = a runaway chain       */
#define DOS_MCB_NO_SEGMENT        0      /* DosMcbReserveTop: nothing was reserved        */
#define DOS_MCB_LOWER_TO_UPPER    0x20   /* 'a' - 'A'                                     */

/* What AH=48h/49h/4Ah return: 0, or the DOS error code. */
#define DOS_MCB_SUCCESS                   0
#define DOS_MCB_ERROR_INSUFFICIENT_MEMORY 8
#define DOS_MCB_ERROR_INVALID_BLOCK       9

/* DosMcbCheckChain's reason codes. */
#define DOS_MCB_CHAIN_OK                  0
#define DOS_MCB_CHAIN_RUNAWAY             1
#define DOS_MCB_CHAIN_CORRUPT_SIGNATURE   2
#define DOS_MCB_CHAIN_OVERRUNS_TOP        3
#define DOS_MCB_CHAIN_LAST_MISPLACED      4

/* --- raw MCB field access over base + paragraph addressing ----------------- */

static inline volatile BYTE *DosMcbSegmentAddress(_In_opt_ volatile BYTE *base,
                                                  _In_ WORD segment) {
    return (volatile BYTE *)((ULONG_PTR)base + ((DWORD)segment << PARAGRAPH_SHIFT));
}
static inline WORD DosMcbReadWord(_In_ volatile BYTE *field) {
    return (WORD)((WORD)field[0] | ((WORD)field[1] << BYTE_SHIFT));
}
static inline VOID DosMcbWriteWord(_Out_ volatile BYTE *field, _In_ WORD value) {
    field[0] = (BYTE)(value & BYTE_MASK);
    field[1] = (BYTE)(value >> BYTE_SHIFT);
}
static inline VOID DosMcbWriteHeader(_In_opt_ volatile BYTE *base, _In_ WORD mcbSegment,
                                     _In_ BYTE signature, _In_ WORD owner,
                                     _In_ WORD paragraphs) {
    volatile BYTE *mcb = DosMcbSegmentAddress(base, mcbSegment);
    mcb[DOS_MCB_SIGNATURE] = signature;
    DosMcbWriteWord(mcb + DOS_MCB_OWNER, owner);
    DosMcbWriteWord(mcb + DOS_MCB_SIZE, paragraphs);
    mcb[DOS_MCB_NAME] = 0;
}

/* --- the owner name DOS 4+ writes into a program's MCB ---------------------- *
 * Bytes 8-15 of the MCB IN FRONT OF A PSP hold the program's base name: the last
 * path component up to the '.', at most 8 characters, NUL-padded when shorter. MEM
 * /C and /D read it to say which program owns a block (6.22: "MEM  Program",
 * "COMMAND  Environment"); nothing wrote it here, so every block read as nameless
 * (s81, #47). Upper-cased because DOS's own EXEC path is. */
static inline VOID DosMcbSetOwnerName(_In_opt_ volatile BYTE *base, _In_ WORD pspSegment,
                                      _In_ PCSTR path) {
    volatile BYTE *mcb = DosMcbSegmentAddress(base, (WORD)(pspSegment - 1));
    PCSTR baseName = path, cursor;
    INT index;
    for (cursor = path; *cursor; ++cursor) if (*cursor == '\\' || *cursor == '/' || *cursor == ':') baseName = cursor + 1;
    for (index = 0; index < DOS_MCB_NAME_LENGTH; ++index) {
        CHAR character = baseName[index];
        if (!character || character == '.' || character == ' ') break;
        if (character >= 'a' && character <= 'z') character = (CHAR)(character - DOS_MCB_LOWER_TO_UPPER);
        mcb[DOS_MCB_NAME + index] = (BYTE)character;
    }
    for (; index < DOS_MCB_NAME_LENGTH; ++index) mcb[DOS_MCB_NAME + index] = 0;
}

/* --- chain bring-up -------------------------------------------------------- */

/* Lay down the initial MCB chain over conventional memory and return the chain
 * root (first MCB paragraph). Three physically-contiguous blocks up to 640KB:
 *   0x007E env block    -> data at ENV_SEG (0x7F), owned by our PSP
 *   0x008F DOS resident -> owner 8: the AH=65h tables, DPBs, stubs (DOS_CTAB_SEG 0x90)
 *   0x00FF program block-> 'Z' (last), owns ALL remaining memory; .EXEs shrink
 *                          this via AH=4Ah at startup to free the tail.
 * (0x7E+1+0x10=0x8F; 0x8F+1+0x6F=0xFF; 0xFF+1+0x9EC0=0x9FC0 -- the EBDA
 * boundary, not 640K; see DOS_MEM_TOP.)
 *
 * ── ★ #207: THE CHAIN STARTS ABOVE SysVars, AS IT DOES ON EVERY REAL DOS. ─────────
 *   It used to start at 0x5F (env 0x60, DOS filler 0x70), BELOW SysVars' segment 0x72.
 *   MEM /D does not walk the kernel's data: it prints fixed rows and derives two of them
 *   from AH=52h (measured, runs/s81_mem/oracle_memd.txt vs ntvdmex_memd3.log):
 *       00070 .. SysVars seg      "IO     System Data"   (6.22: 0070..0116 = 2,656)
 *       SysVars seg .. ES:BX-2    "MSDOS  System Data"   (6.22: 0116..0253 = 5,072)
 *   With the first MCB at 0x5F the MSDOS row was 0x5F-0x72 paragraphs -- NEGATIVE,
 *   printed "4,294,96" -- and the env and filler blocks were then listed AGAIN by the
 *   chain walk. Now everything below 0x7E is kernel data outside the chain (the IVT,
 *   BDA, our handler segment 0x50, the device headers at 0x60, [0x714] and SysVars/SDA
 *   at 0x72), which is the shape of IO.SYS + MSDOS.SYS on 6.22.
 * ⚠ WHAT DID NOT MOVE, ON PURPOSE: the program block (0xFF, PSP 0x100, same size, so
 *   the free block, PSP+2 and "Largest executable" are byte-identical -- s73 moved the
 *   environment to the TOP of memory, which moved PSP+2, and every DOS extender #GP'd;
 *   this move is BELOW the program and leaves PSP+2 alone); the same three blocks with
 *   the same owners in the same order; and DOS_CTAB_SEG (0x90), the first data paragraph
 *   of the DOS block exactly as before. Bytes below the first MCB grow by 0x1F
 *   paragraphs and the DOS block shrinks by exactly 0x1F, so MEM /C's MSDOS total
 *   (kernel area + owner-8 blocks) is unchanged by construction. */
#define DOS_FIRST_MCB    0x007Eu   /* the env block's MCB; ES:BX-2 of AH=52h          */
#define DOS_ENV_PARAS    0x0010u   /* 256 bytes -- dos_env.h's DOS_ENV_CAP            */
#define DOS_RESBLK_MCB   0x008Fu   /* DOS's own block; data at 0x90 = DOS_CTAB_SEG    */
#define DOS_RESBLK_PARAS ((WORD)(DOS_PSP_SEG - 1 - DOS_RESBLK_MCB - 1)) /* 0x6F    */
/* #136: the same chain over a SMALLER machine. `top` is the paragraph the 'Z' block ends
   at -- DOS_MEM_TOP for the 640 KB machine, less when Settings > Conventional Memory asks
   for one (BiosConventionalTopParagraph in bios_bda.h). Everything below the program block is
   untouched by it; only the program block's size, and so PSP+2, follow the top. */
static inline WORD DosMcbInitializeWithTop(_In_opt_ volatile BYTE *base, _In_ WORD top) {
    DosMcbWriteHeader(base, DOS_FIRST_MCB,  DOS_MCB_MEMBER, DOS_PSP_SEG, DOS_ENV_PARAS);
    DosMcbWriteHeader(base, DOS_RESBLK_MCB, DOS_MCB_MEMBER, DOS_MCB_OWNER_DOS,      DOS_RESBLK_PARAS);
    DosMcbWriteHeader(base, (WORD)(DOS_PSP_SEG - 1), DOS_MCB_LAST, DOS_PSP_SEG,
            (WORD)(top - DOS_PSP_SEG));
    return DOS_FIRST_MCB;
}
static inline WORD DosMcbInitialize(_In_opt_ volatile BYTE *base) {
    return DosMcbInitializeWithTop(base, DOS_MEM_TOP);
}

/* --- reserve `paragraphs` at the TOP of the chain for resident DOS data ---------- *
 * Splits the last ('Z') block: it keeps its owner and loses paragraphs+1 paragraphs,
 * and a new 'Z' block owned by DOS (8) takes the top. Returns the data segment
 * of the reserved block, or 0 if the last block is too small. Used for the CDS
 * array, which is LASTDRIVE (26) entries of 88 bytes -- more than the resident
 * filler holds -- and which real DOS keeps in its resident data just the same.
 * The top of the program's block moves down by exactly that much, and PSP+2
 * must be built from the value this returns minus one, not from DOS_MEM_TOP. */
/* ⚠ s81 (#169): SAFE TO CALL TWICE. It used to split whatever block was last -- and after
     one reservation the last block IS that reservation (owner 8), so a second call carved
     the new block out of the first one's data. Now a DOS-owned last block means "a
     reservation is already on top": the new one is carved from the TOP of the block
     before it, and slots in between, so every reservation keeps its bytes. */
static inline WORD DosMcbReserveTop(_In_opt_ volatile BYTE *base, _In_ WORD firstMcb,
                                    _In_ WORD paragraphs) {
    WORD mcbSegment = firstMcb, previousSegment = 0;
    INT walkCount = 0; BOOL hasPrevious = FALSE;
    for (;;) {
        volatile BYTE *mcb = DosMcbSegmentAddress(base, mcbSegment);
        WORD blockSize = DosMcbReadWord(mcb + DOS_MCB_SIZE);
        if (mcb[DOS_MCB_SIGNATURE] == DOS_MCB_LAST && DosMcbReadWord(mcb + DOS_MCB_OWNER) == DOS_MCB_OWNER_DOS && hasPrevious) {
            volatile BYTE *previousMcb = DosMcbSegmentAddress(base, previousSegment);
            WORD previousSize = DosMcbReadWord(previousMcb + DOS_MCB_SIZE), newSegment;
            if (previousSize < (WORD)(paragraphs + 2)) return DOS_MCB_NO_SEGMENT;
            DosMcbWriteWord(previousMcb + DOS_MCB_SIZE, (WORD)(previousSize - paragraphs - 1));
            newSegment = (WORD)(previousSegment + 1 + (previousSize - paragraphs - 1));          /* ends exactly at mcbSegment */
            DosMcbWriteHeader(base, newSegment, DOS_MCB_MEMBER, DOS_MCB_OWNER_DOS, paragraphs);
            return (WORD)(newSegment + 1);
        }
        if (mcb[DOS_MCB_SIGNATURE] == DOS_MCB_LAST) {
            WORD newTop;
            if (blockSize < (WORD)(paragraphs + 2)) return DOS_MCB_NO_SEGMENT;
            DosMcbWriteWord(mcb + DOS_MCB_SIZE, (WORD)(blockSize - paragraphs - 1));
            mcb[DOS_MCB_SIGNATURE] = DOS_MCB_MEMBER;
            newTop = (WORD)(mcbSegment + 1 + (blockSize - paragraphs - 1));     /* the new 'Z' MCB  */
            DosMcbWriteHeader(base, newTop, DOS_MCB_LAST, DOS_MCB_OWNER_DOS, paragraphs);
            return (WORD)(newTop + 1);
        }
        if (mcb[DOS_MCB_SIGNATURE] != DOS_MCB_MEMBER || ++walkCount > DOS_MCB_WALK_LIMIT) return DOS_MCB_NO_SEGMENT;
        previousSegment = mcbSegment; hasPrevious = TRUE;
        mcbSegment = (WORD)(mcbSegment + 1 + blockSize);
    }
}

/* --- AH=48: allocate `requested` paragraphs ------------------------------------- *
 * On success returns 0 and *allocatedSegment = segment of the allocated block (data, not
 * MCB). On failure returns 8 and *largestFree = largest free block found. */
static inline INT DosMcbAllocate(_In_opt_ volatile BYTE *base, _In_ WORD firstMcb,
                                 _In_ WORD requested, _Out_opt_ PWORD allocatedSegment,
                                 _Out_opt_ PWORD largestFree) {
    WORD mcbSegment = firstMcb, biggest = 0, result = 0;
    BOOL isDone = FALSE;
    for (;;) {
        volatile BYTE *mcb = DosMcbSegmentAddress(base, mcbSegment);
        BYTE signature = mcb[DOS_MCB_SIGNATURE];
        WORD owner = DosMcbReadWord(mcb + DOS_MCB_OWNER);
        WORD blockSize  = DosMcbReadWord(mcb + DOS_MCB_SIZE);
        if (owner == DOS_MCB_OWNER_FREE) {                                 /* free block */
            /* merge-on-alloc: coalesce following free blocks before sizing, so two
               adjacent free blocks jointly satisfy a request neither satisfies alone
               (this is what real MS-DOS does during the allocation walk). */
            while (signature == DOS_MCB_MEMBER) {
                volatile BYTE *nextMcb = DosMcbSegmentAddress(base, (WORD)(mcbSegment + 1 + blockSize));
                if (DosMcbReadWord(nextMcb + DOS_MCB_OWNER) != DOS_MCB_OWNER_FREE) break;       /* next block owned    */
                if (nextMcb[DOS_MCB_SIGNATURE] != DOS_MCB_MEMBER && nextMcb[DOS_MCB_SIGNATURE] != DOS_MCB_LAST) break;/* next is not an MCB  */
                blockSize = (WORD)(blockSize + 1 + DosMcbReadWord(nextMcb + DOS_MCB_SIZE));
                DosMcbWriteWord(mcb + DOS_MCB_SIZE, blockSize);
                signature = nextMcb[DOS_MCB_SIGNATURE];                            /* may become 'Z'      */
                mcb[DOS_MCB_SIGNATURE] = signature;
            }
            if (blockSize > biggest) biggest = blockSize;
            if (blockSize >= requested) {
                if (blockSize >= (WORD)(requested + 1)) {       /* split off a free tail */
                    volatile BYTE *nextMcb = DosMcbSegmentAddress(base, (WORD)(mcbSegment + 1 + requested));
                    nextMcb[DOS_MCB_SIGNATURE] = signature;                        /* tail inherits last-status */
                    DosMcbWriteWord(nextMcb + DOS_MCB_OWNER, DOS_MCB_OWNER_FREE);
                    DosMcbWriteWord(nextMcb + DOS_MCB_SIZE, (WORD)(blockSize - requested - 1));
                    nextMcb[DOS_MCB_NAME] = 0;
                    mcb[DOS_MCB_SIGNATURE] = DOS_MCB_MEMBER;
                    DosMcbWriteWord(mcb + DOS_MCB_SIZE, requested);
                }
                DosMcbWriteWord(mcb + DOS_MCB_OWNER, DOS_PSP_SEG);
                result = (WORD)(mcbSegment + 1);
                isDone = TRUE;
            }
        }
        if (isDone || signature == DOS_MCB_LAST) break;
        mcbSegment = (WORD)(mcbSegment + 1 + blockSize);
    }
    if (!isDone) { if (largestFree) *largestFree = biggest; return DOS_MCB_ERROR_INSUFFICIENT_MEMORY; }
    if (allocatedSegment) *allocatedSegment = result;
    return DOS_MCB_SUCCESS;
}

/* --- AH=49: free the block whose data segment is `blockSegment` ------------------- *
 * Marks the block free and coalesces forward into any following free blocks.
 * Returns 0 on success, 9 if blockSegment-1 is not a valid MCB. */
static inline INT DosMcbFree(_In_opt_ volatile BYTE *base, _In_ WORD blockSegment) {
    WORD mcbSegment = (WORD)(blockSegment - 1);
    volatile BYTE *mcb = DosMcbSegmentAddress(base, mcbSegment);
    if (mcb[DOS_MCB_SIGNATURE] != DOS_MCB_MEMBER && mcb[DOS_MCB_SIGNATURE] != DOS_MCB_LAST) return DOS_MCB_ERROR_INVALID_BLOCK;
    DosMcbWriteWord(mcb + DOS_MCB_OWNER, DOS_MCB_OWNER_FREE);                                /* mark free */
    while (mcb[DOS_MCB_SIGNATURE] == DOS_MCB_MEMBER) {                              /* coalesce forward */
        WORD blockSize = DosMcbReadWord(mcb + DOS_MCB_SIZE);
        volatile BYTE *nextMcb = DosMcbSegmentAddress(base, (WORD)(mcbSegment + 1 + blockSize));
        if (DosMcbReadWord(nextMcb + DOS_MCB_OWNER) != DOS_MCB_OWNER_FREE) break;              /* neighbour owned */
        if (nextMcb[DOS_MCB_SIGNATURE] != DOS_MCB_MEMBER && nextMcb[DOS_MCB_SIGNATURE] != DOS_MCB_LAST) break;       /* neighbour not an MCB */
        DosMcbWriteWord(mcb + DOS_MCB_SIZE, (WORD)(blockSize + 1 + DosMcbReadWord(nextMcb + DOS_MCB_SIZE)));
        mcb[DOS_MCB_SIGNATURE] = nextMcb[DOS_MCB_SIGNATURE];                                 /* absorb (may become 'Z') */
    }
    return DOS_MCB_SUCCESS;
}

/* --- AH=4A: resize the block whose data segment is `blockSegment` to `requested` ------- *
 * Shrink frees the tail; grow absorbs a following free neighbour (if any).
 * Returns 0 on success; 9 if not a valid block; 8 if it cannot grow, with
 * *largestAvailable = the largest size achievable. */
static inline INT DosMcbResize(_In_opt_ volatile BYTE *base, _In_ WORD blockSegment,
                               _In_ WORD requested, _Out_opt_ PWORD largestAvailable) {
    WORD mcbSegment = (WORD)(blockSegment - 1);
    volatile BYTE *mcb = DosMcbSegmentAddress(base, mcbSegment);
    if (mcb[DOS_MCB_SIGNATURE] != DOS_MCB_MEMBER && mcb[DOS_MCB_SIGNATURE] != DOS_MCB_LAST) return DOS_MCB_ERROR_INVALID_BLOCK;
    WORD currentSize = DosMcbReadWord(mcb + DOS_MCB_SIZE);
    BYTE signature = mcb[DOS_MCB_SIGNATURE];
    BOOL isResized = FALSE;
    if (requested <= currentSize) {                                 /* shrink (free the tail) */
        if ((WORD)(currentSize - requested) >= 1) {
            volatile BYTE *nextMcb = DosMcbSegmentAddress(base, (WORD)(mcbSegment + 1 + requested));
            nextMcb[DOS_MCB_SIGNATURE] = signature; DosMcbWriteWord(nextMcb + DOS_MCB_OWNER, DOS_MCB_OWNER_FREE);
            DosMcbWriteWord(nextMcb + DOS_MCB_SIZE, (WORD)(currentSize - requested - 1)); nextMcb[DOS_MCB_NAME] = 0;
            mcb[DOS_MCB_SIGNATURE] = DOS_MCB_MEMBER; DosMcbWriteWord(mcb + DOS_MCB_SIZE, requested);
        }
        isResized = TRUE;
    } else if (signature == DOS_MCB_MEMBER) {                            /* grow into a free neighbour */
        volatile BYTE *nextMcb = DosMcbSegmentAddress(base, (WORD)(mcbSegment + 1 + currentSize));
        if (DosMcbReadWord(nextMcb + DOS_MCB_OWNER) == DOS_MCB_OWNER_FREE) {
            WORD available = (WORD)(currentSize + 1 + DosMcbReadWord(nextMcb + DOS_MCB_SIZE));
            if (available >= requested) {
                if ((WORD)(available - requested) >= 1) {
                    volatile BYTE *tailMcb = DosMcbSegmentAddress(base, (WORD)(mcbSegment + 1 + requested));
                    tailMcb[DOS_MCB_SIGNATURE] = nextMcb[DOS_MCB_SIGNATURE]; DosMcbWriteWord(tailMcb + DOS_MCB_OWNER, DOS_MCB_OWNER_FREE);
                    DosMcbWriteWord(tailMcb + DOS_MCB_SIZE, (WORD)(available - requested - 1)); tailMcb[DOS_MCB_NAME] = 0;
                    DosMcbWriteWord(mcb + DOS_MCB_SIZE, requested); mcb[DOS_MCB_SIGNATURE] = DOS_MCB_MEMBER;
                } else {
                    DosMcbWriteWord(mcb + DOS_MCB_SIZE, available); mcb[DOS_MCB_SIGNATURE] = nextMcb[DOS_MCB_SIGNATURE];
                }
                isResized = TRUE;
            }
        }
    }
    if (!isResized) {                                          /* fail: report max available */
        WORD largest = currentSize;
        if (signature == DOS_MCB_MEMBER) {
            volatile BYTE *nextMcb = DosMcbSegmentAddress(base, (WORD)(mcbSegment + 1 + currentSize));
            if (DosMcbReadWord(nextMcb + DOS_MCB_OWNER) == DOS_MCB_OWNER_FREE) largest = (WORD)(currentSize + 1 + DosMcbReadWord(nextMcb + DOS_MCB_SIZE));
        }
        if (largestAvailable) *largestAvailable = largest;
        return DOS_MCB_ERROR_INSUFFICIENT_MEMORY;
    }
    return DOS_MCB_SUCCESS;
}

/* --- chain integrity validator (test oracle) ------------------------------- *
 * Walk from firstMcb; returns 0 if the chain is well-formed and ends exactly
 * at topParagraph with a single 'Z', else a nonzero reason code:
 *   1 runaway chain  2 corrupt signature  3 overruns top  4 'Z' misplaced. */
static inline INT DosMcbCheckChain(_In_opt_ volatile BYTE *base, _In_ WORD firstMcb, _In_ WORD topParagraph) {
    WORD mcbSegment = firstMcb;
    INT walkCount = 0;
    for (;;) {
        volatile BYTE *mcb = DosMcbSegmentAddress(base, mcbSegment);
        BYTE  signature  = mcb[DOS_MCB_SIGNATURE];
        WORD  blockSize   = DosMcbReadWord(mcb + DOS_MCB_SIZE);
        DWORD nextSegment = (DWORD)mcbSegment + 1 + blockSize;
        if (++walkCount > DOS_MCB_WALK_LIMIT) return DOS_MCB_CHAIN_RUNAWAY;
        if (signature != DOS_MCB_MEMBER && signature != DOS_MCB_LAST) return DOS_MCB_CHAIN_CORRUPT_SIGNATURE;
        if (nextSegment > topParagraph) return DOS_MCB_CHAIN_OVERRUNS_TOP;
        if (signature == DOS_MCB_LAST) return (nextSegment == topParagraph) ? DOS_MCB_CHAIN_OK : DOS_MCB_CHAIN_LAST_MISPLACED;
        mcbSegment = (WORD)nextSegment;
    }
}

#endif /* NTVDMEX_DOS_MCB_H */
