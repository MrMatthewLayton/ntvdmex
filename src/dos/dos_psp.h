/* dos_psp.h -- build a DOS Program Segment Prefix (+ a minimal environment) in
 * conventional memory. Pure logic over a `base` pointer (same convention as
 * dos_mcb.h). Ported from the M2.1 PSP setup in tools/vdmhost/vdmhost.c.
 * Verified off-VM by tools/dostest/mcb_test.c.
 */
#ifndef DOS_PSP_H
#define DOS_PSP_H

#include <stdint.h>
#include "dos_mcb.h"        /* mcb_at / mcb_wr16 paragraph addressing */

/* Build a PSP at psp_seg:0 and an empty environment at env_seg:0. top_seg is the
 * top-of-conventional-memory segment (0xA000 = 640KB). The command tail is empty
 * (length 0) until M2.5 wires real args; psp[0x81] holds the 0x0D the command-tail
 * parser scans for. Mirrors vdmhost.c's PSP setup. */
static inline void dos_psp_build(volatile uint8_t *base, uint16_t psp_seg,
                                 uint16_t env_seg, uint16_t top_seg) {
    volatile uint8_t *psp = mcb_at(base, psp_seg);
    volatile uint8_t *env = mcb_at(base, env_seg);
    uint32_t i;

    env[0] = 0; env[1] = 0; env[2] = 0;                /* empty environment block   */

    for (i = 0; i < 0x100; ++i) psp[i] = 0;
    psp[0x00] = 0xCD; psp[0x01] = 0x20;                /* INT 20h (legacy exit)      */
    mcb_wr16(psp + 0x02, top_seg);                     /* segment of top-of-memory   */
    psp[0x18] = 1; psp[0x19] = 1; psp[0x1A] = 1;       /* JFT: std handles open      */
    psp[0x1B] = 0; psp[0x1C] = 2;
    for (i = 0x1D; i < 0x2C; ++i) psp[i] = 0xFF;       /* remaining JFT = closed     */
    mcb_wr16(psp + 0x2C, env_seg);                     /* environment segment        */
    mcb_wr16(psp + 0x32, 0x14);                        /* JFT size (20 handles)      */
    mcb_wr16(psp + 0x34, 0x18);                        /* JFT pointer: offset        */
    mcb_wr16(psp + 0x36, psp_seg);                     /* JFT pointer: segment       */
    psp[0x38] = 0xFF; psp[0x39] = 0xFF;                /* previous PSP = 0xFFFFFFFF  */
    psp[0x3A] = 0xFF; psp[0x3B] = 0xFF;
    psp[0x50] = 0xCD; psp[0x51] = 0x21; psp[0x52] = 0xCB; /* INT 21h ; RETF          */
    psp[0x80] = 0; psp[0x81] = 0x0D;                   /* empty command tail + 0x0D  */
}

#endif /* DOS_PSP_H */
