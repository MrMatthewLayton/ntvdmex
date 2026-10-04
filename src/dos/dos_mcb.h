/* dos_mcb.h -- DOS conventional-memory allocator (MCB chain), host-testable.
 *
 * The canonical DOS-memory allocator for the clean host (M2.6). The same logic
 * runs in V86 (absolute paragraph<<4 addressing) and off-VM against a plain byte
 * buffer: every routine takes a caller-supplied `base` pointer + paragraph offsets
 * (pass base=NULL for the host's absolute V86 addressing, a buffer for tests).
 * Verified off-VM by tools/dostest/mcb_test.c.
 *
 * SYNC: the tools/vdmhost spike still carries an inline copy of AH=48/49/4A (kept
 * in step by hand) until it is retired in favour of this module; the clean host
 * (src/) uses this file directly. Originated as a port of the spike at 4aa6f44.
 *
 * MCB layout (16 bytes, immediately preceding the block it owns):
 *   [0]    signature: 'M' (member) or 'Z' (last block in the chain)
 *   [1..2] owner PSP segment   (0 = free, 8 = DOS, PSP_SEG = our process)
 *   [3..4] size of owned block in paragraphs (the block starts at mcb_seg+1)
 *
 * DOS error codes returned: 8 = insufficient memory, 9 = invalid memory block.
 */
#ifndef DOS_MCB_H
#define DOS_MCB_H

#include <stdint.h>

#define DOS_PSP_SEG 0x0100u     /* matches vdmhost.c enum PSP_SEG */
/* ── WHERE CONVENTIONAL MEMORY ACTUALLY ENDS. (GH #47) ────────────────────────
   0xA000 is 640KB, and it is NOT where a real PC's MCB chain stops. MS-DOS 6.22,
   walked directly (tools/dostest/p_mcb.asm):
       CASE=mcb.chain.ends.at SIG=AX AX=9FC0
       CASE=psp.02.memtop     SIG=AX AX=9FC0
   The top 1KB (0x9FC0..0x9FFF) is the Extended BIOS Data Area, which the BIOS
   reserves and DOS never hands out -- which is exactly why MEM reports 639K and
   not 640K. Ours ran the last block all the way to 0xA000 and reported 640K.
   Both numbers are measured; this is the one a real machine gives. */
#define DOS_MEM_TOP 0x9FC0u     /* conventional top in paragraphs: 640K - 1K EBDA */

/* --- raw MCB field access over base + paragraph addressing ----------------- */

static inline volatile uint8_t *mcb_at(volatile uint8_t *base, uint16_t seg) {
    return (volatile uint8_t *)((uintptr_t)base + ((uint32_t)seg << 4));
}
static inline uint16_t mcb_rd16(volatile uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}
static inline void mcb_wr16(volatile uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)(v >> 8);
}
static inline void mcb_lay(volatile uint8_t *base, uint16_t seg,
                           uint8_t sig, uint16_t owner, uint16_t paras) {
    volatile uint8_t *mc = mcb_at(base, seg);
    mc[0] = sig;
    mcb_wr16(mc + 1, owner);
    mcb_wr16(mc + 3, paras);
    mc[8] = 0;
}

/* --- the owner name DOS 4+ writes into a program's MCB ---------------------- *
 * Bytes 8-15 of the MCB IN FRONT OF A PSP hold the program's base name: the last
 * path component up to the '.', at most 8 characters, NUL-padded when shorter. MEM
 * /C and /D read it to say which program owns a block (6.22: "MEM  Program",
 * "COMMAND  Environment"); nothing wrote it here, so every block read as nameless
 * (s81, #47). Upper-cased because DOS's own EXEC path is. */
static inline void mcb_set_name(volatile uint8_t *base, uint16_t psp_seg, const char *path) {
    volatile uint8_t *mc = mcb_at(base, (uint16_t)(psp_seg - 1));
    const char *s = path, *p;
    int k;
    for (p = path; *p; ++p) if (*p == '\\' || *p == '/' || *p == ':') s = p + 1;
    for (k = 0; k < 8; ++k) {
        char c = s[k];
        if (!c || c == '.' || c == ' ') break;
        if (c >= 'a' && c <= 'z') c = (char)(c - 0x20);
        mc[8 + k] = (uint8_t)c;
    }
    for (; k < 8; ++k) mc[8 + k] = 0;
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
#define DOS_RESBLK_PARAS ((uint16_t)(DOS_PSP_SEG - 1 - DOS_RESBLK_MCB - 1)) /* 0x6F    */
/* #136: the same chain over a SMALLER machine. `top` is the paragraph the 'Z' block ends
   at -- DOS_MEM_TOP for the 640 KB machine, less when Settings > Conventional Memory asks
   for one (bios_conv_top_para in bios_bda.h). Everything below the program block is
   untouched by it; only the program block's size, and so PSP+2, follow the top. */
static inline uint16_t dos_mcb_init_top(volatile uint8_t *base, uint16_t top) {
    mcb_lay(base, DOS_FIRST_MCB,  'M', DOS_PSP_SEG, DOS_ENV_PARAS);
    mcb_lay(base, DOS_RESBLK_MCB, 'M', 0x0008,      DOS_RESBLK_PARAS);
    mcb_lay(base, (uint16_t)(DOS_PSP_SEG - 1), 'Z', DOS_PSP_SEG,
            (uint16_t)(top - DOS_PSP_SEG));
    return DOS_FIRST_MCB;
}
static inline uint16_t dos_mcb_init(volatile uint8_t *base) {
    return dos_mcb_init_top(base, DOS_MEM_TOP);
}

/* --- reserve `paras` at the TOP of the chain for resident DOS data ---------- *
 * Splits the last ('Z') block: it keeps its owner and loses paras+1 paragraphs,
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
static inline uint16_t dos_mcb_reserve_top(volatile uint8_t *base, uint16_t first_mcb,
                                           uint16_t paras) {
    uint16_t m = first_mcb, prev = 0;
    int guard = 0, have_prev = 0;
    for (;;) {
        volatile uint8_t *mc = mcb_at(base, m);
        uint16_t sz = mcb_rd16(mc + 3);
        if (mc[0] == 'Z' && mcb_rd16(mc + 1) == 0x0008 && have_prev) {
            volatile uint8_t *pc = mcb_at(base, prev);
            uint16_t psz = mcb_rd16(pc + 3), nb;
            if (psz < (uint16_t)(paras + 2)) return 0;
            mcb_wr16(pc + 3, (uint16_t)(psz - paras - 1));
            nb = (uint16_t)(prev + 1 + (psz - paras - 1));          /* ends exactly at m */
            mcb_lay(base, nb, 'M', 0x0008, paras);
            return (uint16_t)(nb + 1);
        }
        if (mc[0] == 'Z') {
            uint16_t newtop;
            if (sz < (uint16_t)(paras + 2)) return 0;
            mcb_wr16(mc + 3, (uint16_t)(sz - paras - 1));
            mc[0] = 'M';
            newtop = (uint16_t)(m + 1 + (sz - paras - 1));     /* the new 'Z' MCB  */
            mcb_lay(base, newtop, 'Z', 0x0008, paras);
            return (uint16_t)(newtop + 1);
        }
        if (mc[0] != 'M' || ++guard > 0x1000) return 0;
        prev = m; have_prev = 1;
        m = (uint16_t)(m + 1 + sz);
    }
}

/* --- AH=48: allocate `want` paragraphs ------------------------------------- *
 * On success returns 0 and *out_seg = segment of the allocated block (data, not
 * MCB). On failure returns 8 and *out_max = largest free block found. */
static inline int dos_alloc(volatile uint8_t *base, uint16_t first_mcb, uint16_t want,
                            uint16_t *out_seg, uint16_t *out_max) {
    uint16_t m = first_mcb, biggest = 0, result = 0;
    int done = 0;
    for (;;) {
        volatile uint8_t *mc = mcb_at(base, m);
        uint8_t  sig = mc[0];
        uint16_t own = mcb_rd16(mc + 1);
        uint16_t sz  = mcb_rd16(mc + 3);
        if (own == 0) {                                 /* free block */
            /* merge-on-alloc: coalesce following free blocks before sizing, so two
               adjacent free blocks jointly satisfy a request neither satisfies alone
               (this is what real MS-DOS does during the allocation walk). */
            while (sig == 'M') {
                volatile uint8_t *nm = mcb_at(base, (uint16_t)(m + 1 + sz));
                if (mcb_rd16(nm + 1) != 0) break;       /* next block owned    */
                if (nm[0] != 'M' && nm[0] != 'Z') break;/* next is not an MCB  */
                sz = (uint16_t)(sz + 1 + mcb_rd16(nm + 3));
                mcb_wr16(mc + 3, sz);
                sig = nm[0];                            /* may become 'Z'      */
                mc[0] = sig;
            }
            if (sz > biggest) biggest = sz;
            if (sz >= want) {
                if (sz >= (uint16_t)(want + 1)) {       /* split off a free tail */
                    volatile uint8_t *nm = mcb_at(base, (uint16_t)(m + 1 + want));
                    nm[0] = sig;                        /* tail inherits last-status */
                    mcb_wr16(nm + 1, 0);
                    mcb_wr16(nm + 3, (uint16_t)(sz - want - 1));
                    nm[8] = 0;
                    mc[0] = 'M';
                    mcb_wr16(mc + 3, want);
                }
                mcb_wr16(mc + 1, DOS_PSP_SEG);
                result = (uint16_t)(m + 1);
                done = 1;
            }
        }
        if (done || sig == 'Z') break;
        m = (uint16_t)(m + 1 + sz);
    }
    if (!done) { if (out_max) *out_max = biggest; return 8; }
    if (out_seg) *out_seg = result;
    return 0;
}

/* --- AH=49: free the block whose data segment is `es_seg` ------------------- *
 * Marks the block free and coalesces forward into any following free blocks.
 * Returns 0 on success, 9 if es_seg-1 is not a valid MCB. */
static inline int dos_free(volatile uint8_t *base, uint16_t es_seg) {
    uint16_t m = (uint16_t)(es_seg - 1);
    volatile uint8_t *mc = mcb_at(base, m);
    if (mc[0] != 'M' && mc[0] != 'Z') return 9;
    mcb_wr16(mc + 1, 0);                                /* mark free */
    while (mc[0] == 'M') {                              /* coalesce forward */
        uint16_t sz = mcb_rd16(mc + 3);
        volatile uint8_t *nm = mcb_at(base, (uint16_t)(m + 1 + sz));
        if (mcb_rd16(nm + 1) != 0) break;              /* neighbour owned */
        if (nm[0] != 'M' && nm[0] != 'Z') break;       /* neighbour not an MCB */
        mcb_wr16(mc + 3, (uint16_t)(sz + 1 + mcb_rd16(nm + 3)));
        mc[0] = nm[0];                                 /* absorb (may become 'Z') */
    }
    return 0;
}

/* --- AH=4A: resize the block whose data segment is `es_seg` to `want` ------- *
 * Shrink frees the tail; grow absorbs a following free neighbour (if any).
 * Returns 0 on success; 9 if not a valid block; 8 if it cannot grow, with
 * *out_max = the largest size achievable. */
static inline int dos_resize(volatile uint8_t *base, uint16_t es_seg, uint16_t want,
                             uint16_t *out_max) {
    uint16_t m = (uint16_t)(es_seg - 1);
    volatile uint8_t *mc = mcb_at(base, m);
    if (mc[0] != 'M' && mc[0] != 'Z') return 9;
    uint16_t cur = mcb_rd16(mc + 3);
    uint8_t  sig = mc[0];
    int ok = 0;
    if (want <= cur) {                                 /* shrink (free the tail) */
        if ((uint16_t)(cur - want) >= 1) {
            volatile uint8_t *nm = mcb_at(base, (uint16_t)(m + 1 + want));
            nm[0] = sig; mcb_wr16(nm + 1, 0);
            mcb_wr16(nm + 3, (uint16_t)(cur - want - 1)); nm[8] = 0;
            mc[0] = 'M'; mcb_wr16(mc + 3, want);
        }
        ok = 1;
    } else if (sig == 'M') {                            /* grow into a free neighbour */
        volatile uint8_t *nm = mcb_at(base, (uint16_t)(m + 1 + cur));
        if (mcb_rd16(nm + 1) == 0) {
            uint16_t avail = (uint16_t)(cur + 1 + mcb_rd16(nm + 3));
            if (avail >= want) {
                if ((uint16_t)(avail - want) >= 1) {
                    volatile uint8_t *n2 = mcb_at(base, (uint16_t)(m + 1 + want));
                    n2[0] = nm[0]; mcb_wr16(n2 + 1, 0);
                    mcb_wr16(n2 + 3, (uint16_t)(avail - want - 1)); n2[8] = 0;
                    mcb_wr16(mc + 3, want); mc[0] = 'M';
                } else {
                    mcb_wr16(mc + 3, avail); mc[0] = nm[0];
                }
                ok = 1;
            }
        }
    }
    if (!ok) {                                          /* fail: report max available */
        uint16_t max = cur;
        if (sig == 'M') {
            volatile uint8_t *nm = mcb_at(base, (uint16_t)(m + 1 + cur));
            if (mcb_rd16(nm + 1) == 0) max = (uint16_t)(cur + 1 + mcb_rd16(nm + 3));
        }
        if (out_max) *out_max = max;
        return 8;
    }
    return 0;
}

/* --- chain integrity validator (test oracle) ------------------------------- *
 * Walk from first_mcb; returns 0 if the chain is well-formed and ends exactly
 * at top_para with a single 'Z', else a nonzero reason code:
 *   1 runaway chain  2 corrupt signature  3 overruns top  4 'Z' misplaced. */
static inline int dos_mcb_check(volatile uint8_t *base, uint16_t first_mcb, uint16_t top_para) {
    uint16_t m = first_mcb;
    int guard = 0;
    for (;;) {
        volatile uint8_t *mc = mcb_at(base, m);
        uint8_t  sig  = mc[0];
        uint16_t sz   = mcb_rd16(mc + 3);
        uint32_t next = (uint32_t)m + 1 + sz;
        if (++guard > 0x1000) return 1;
        if (sig != 'M' && sig != 'Z') return 2;
        if (next > top_para) return 3;
        if (sig == 'Z') return (next == top_para) ? 0 : 4;
        m = (uint16_t)next;
    }
}

#endif /* DOS_MCB_H */
