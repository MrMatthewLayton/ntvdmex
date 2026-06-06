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
#define DOS_MEM_TOP 0xA000u     /* 640KB conventional top, in paragraphs */

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

/* --- chain bring-up -------------------------------------------------------- */

/* Lay down the initial MCB chain over conventional memory and return the chain
 * root (first MCB paragraph). Three physically-contiguous blocks up to 640KB:
 *   0x005F env block    -> data at ENV_SEG (0x60), owned by our PSP
 *   0x0070 DOS resident -> filler standing in for resident DOS (owner 8)
 *   0x00FF program block-> 'Z' (last), owns ALL remaining memory; .EXEs shrink
 *                          this via AH=4Ah at startup to free the tail.
 * (0x5F+1+0x10=0x70; 0x70+1+0x8E=0xFF; 0xFF+1+0x9F00=0xA000.) */
static inline uint16_t dos_mcb_init(volatile uint8_t *base) {
    mcb_lay(base, 0x005F, 'M', DOS_PSP_SEG, 0x0010);
    mcb_lay(base, 0x0070, 'M', 0x0008,      0x008E);
    mcb_lay(base, 0x00FF, 'Z', DOS_PSP_SEG, 0x9F00);
    return 0x005F;
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
