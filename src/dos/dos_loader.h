/* dos_loader.h -- load a DOS program image (MZ .EXE or flat .COM) into
 * conventional memory. Pure logic over a `base` pointer (same convention as
 * dos_mcb.h): base=NULL for the host's absolute V86 addressing, a byte buffer
 * for off-VM tests. Ported from the M2.3 loader in tools/vdmhost/vdmhost.c.
 * Verified off-VM by tools/dostest/mcb_test.c.
 */
#ifndef DOS_LOADER_H
#define DOS_LOADER_H

#include <stdint.h>
#include "dos_mcb.h"        /* mcb_at / mcb_rd16 / mcb_wr16 paragraph addressing */

typedef struct {
    uint16_t cs, ip;        /* entry CS:IP                                   */
    uint16_t ss, sp;        /* entry SS:SP                                   */
    int      is_exe;        /* 1 = MZ .EXE, 0 = flat .COM                    */
    uint32_t img_size;      /* bytes placed in conventional memory           */
} dos_image_t;

static inline uint16_t dos_ld_rd16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

/* Place `file` (nread bytes) into conventional memory at `base`:
 *  - MZ .EXE: load module copied to (psp_seg+0x10):0, relocations applied,
 *    CS:IP/SS:SP from the header (segment fields biased by the load segment).
 *  - flat .COM: copied to psp_seg:0x100; CS=SS=psp_seg, IP=0x100, SP=0xFFFE.
 * Returns the entry context. Mirrors vdmhost.c's M2.3 loader exactly. */
static inline dos_image_t dos_load(volatile uint8_t *base, const uint8_t *file,
                                   uint32_t nread, uint16_t psp_seg) {
    dos_image_t e;
    uint32_t i;
    if (nread >= 0x1C && file[0] == 'M' && file[1] == 'Z') {
        uint32_t hdrsize  = (uint32_t)dos_ld_rd16(file + 8) * 16;  /* e_cparhdr */
        uint32_t e_crlc   = dos_ld_rd16(file + 6);                 /* reloc count   */
        uint32_t e_lfarlc = dos_ld_rd16(file + 24);               /* reloc tbl off */
        uint16_t load_seg = (uint16_t)(psp_seg + 0x10);
        volatile uint8_t *img = mcb_at(base, load_seg);
        uint16_t e_cblp = dos_ld_rd16(file + 2);                  /* bytes, last pg */
        uint16_t e_cp   = dos_ld_rd16(file + 4);                  /* page count    */
        uint32_t totalused = e_cp ? ((uint32_t)(e_cp - 1) * 512 + (e_cblp ? e_cblp : 512))
                                  : nread;
        uint32_t imgsize;
        if (totalused > nread) totalused = nread;
        imgsize = totalused > hdrsize ? totalused - hdrsize : 0;
        for (i = 0; i < imgsize; ++i) img[i] = file[hdrsize + i];
        for (i = 0; i < e_crlc; ++i) {                            /* apply relocations */
            uint32_t ro = dos_ld_rd16(file + e_lfarlc + i * 4);
            uint32_t rs = dos_ld_rd16(file + e_lfarlc + i * 4 + 2);
            volatile uint8_t *loc = (volatile uint8_t *)((uintptr_t)base
                                    + (((uint32_t)(load_seg + rs)) << 4) + ro);
            mcb_wr16(loc, (uint16_t)(mcb_rd16(loc) + load_seg));
        }
        e.cs = (uint16_t)(load_seg + dos_ld_rd16(file + 22));     /* e_cs */
        e.ip = dos_ld_rd16(file + 20);                            /* e_ip */
        e.ss = (uint16_t)(load_seg + dos_ld_rd16(file + 14));     /* e_ss */
        e.sp = dos_ld_rd16(file + 16);                            /* e_sp */
        e.is_exe = 1;
        e.img_size = imgsize;
    } else {                                                      /* flat .COM */
        uint32_t n = nread > 0xFE00 ? 0xFE00 : nread;
        volatile uint8_t *code = mcb_at(base, psp_seg) + 0x100;   /* psp_seg:0x100 */
        for (i = 0; i < n; ++i) code[i] = file[i];
        e.cs = psp_seg; e.ip = 0x100; e.ss = psp_seg; e.sp = 0xFFFE;
        e.is_exe = 0;
        e.img_size = n;
    }
    return e;
}

#endif /* DOS_LOADER_H */
