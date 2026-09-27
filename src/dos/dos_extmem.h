/* dos_extmem.h -- where an INT 15h AH=87h address points.  GH #54.
 *
 * AH=87h copies between two LINEAR addresses the caller puts in a GDT. On a real
 * AT that is physical memory, and above 1 MB it is the same memory HIMEM hands out
 * as XMS. This host has no such memory: an XMS block (EMB) is a buffer on the HOST
 * heap (dos_xms.h), so a guest linear address above the HMA is not memory we own --
 * it is whatever the host process happens to have there. Copying to it blindly
 * would let any DOS program write over the host.
 *
 * So every address is RESOLVED, and one that resolves to nothing is refused:
 *   1. below 0x10FFF0 (conventional memory + the HMA) -> the guest's own memory,
 *      which in the VDM is identity-mapped at host VA 0;
 *   2. inside an allocated EMB's range (the address XMS AH=0Ch lock handed out)
 *      -> that block. This is what makes "lock a block, then move with 87h" work,
 *      as it does on a real machine;
 *   3. otherwise, 0x110000..0x1000000 -> a private buffer standing for the 15 MB
 *      of raw extended memory INT 15h AH=88h already claims. The host allocates it
 *      on first use;
 *   4. anything else -> refused.
 * A range may not straddle two of these.
 *
 * Pure <stdint.h>, like dos_xms.h; tested off-VM by tools/dostest/extmem_test.c.
 */
#ifndef DOS_EXTMEM_H
#define DOS_EXTMEM_H

#include <stdint.h>
#include "dos_xms.h"

#define EXTMEM_DIRECT_END  0x0010FFF0u   /* FFFF:FFFF + 1: top of the HMA          */
#define EXTMEM_RAW_BASE    0x00100000u   /* raw buffer index 0 = linear 1 MB        */
#define EXTMEM_RAW_END     0x01000000u   /* 16 MB: 1 MB + AH=88h's 15 MB            */
#define EXTMEM_RAW_LEN     (EXTMEM_RAW_END - EXTMEM_RAW_BASE)

enum { EXTMEM_NONE = 0, EXTMEM_DIRECT, EXTMEM_EMB, EXTMEM_RAW };

/* Which region [lin, lin+n) lies in. n > 0. */
static inline int extmem_classify(const xms_state *x, uint32_t lin, uint32_t n)
{
    uint32_t end = lin + n;
    int i;
    if (n == 0 || end < lin) return EXTMEM_NONE;                 /* wraps 4 GB */
    if (end <= EXTMEM_DIRECT_END) return EXTMEM_DIRECT;
    if (x) {
        for (i = 0; i < XMS_MAX_HANDLES; ++i) {
            const xms_handle *h = &x->h[i];
            uint32_t b, e;
            if (!h->used || !h->mem || !h->size_kb) continue;
            b = (uint32_t)(uintptr_t)h->mem;
            e = b + h->size_kb * 1024u;
            if (lin >= b && end <= e && e > b) return EXTMEM_EMB;
        }
    }
    if (lin >= EXTMEM_DIRECT_END && end <= EXTMEM_RAW_END) return EXTMEM_RAW;
    return EXTMEM_NONE;
}

/* Host pointer for [lin, lin+n), or 0. `conv` is the host address of guest linear 0,
   `raw` the raw buffer (EXTMEM_RAW_LEN bytes; may be 0 until first needed -- then a
   RAW range resolves to 0 and the caller allocates and asks again). */
static inline uint8_t *extmem_resolve(const xms_state *x, uintptr_t conv, uint8_t *raw,
                                      uint32_t lin, uint32_t n)
{
    switch (extmem_classify(x, lin, n)) {
    case EXTMEM_DIRECT: return (uint8_t *)(conv + lin);
    case EXTMEM_EMB:    return (uint8_t *)(uintptr_t)lin;
    case EXTMEM_RAW:    return raw ? raw + (lin - EXTMEM_RAW_BASE) : 0;
    default:            return 0;
    }
}

/* The source and destination bases from an AH=87h GDT (48 bytes at `gdt`):
   descriptor 2 (+0x10) is the source, 3 (+0x18) the destination; base bits 0-23 at
   +2..+4 and 24-31 at +7 (386 BIOSes honour the high byte). */
static inline uint32_t extmem_desc_base(const volatile uint8_t *d)
{
    return (uint32_t)d[2] | ((uint32_t)d[3] << 8) | ((uint32_t)d[4] << 16)
         | ((uint32_t)d[7] << 24);
}

#endif /* DOS_EXTMEM_H */
