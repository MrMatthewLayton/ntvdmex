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

/* Set the PSP command tail at psp_seg:0x80 from an args string (no leading space):
   [0x80] = length, [0x81..] = " <args>", terminated by 0x0D (the parser scans for it).
   A leading space is the DOS convention. Empty/NULL args -> length 0, 0x0D at 0x81. */
static inline void dos_cmdtail_build(volatile uint8_t *base, uint16_t psp_seg,
                                     const char *args) {
    volatile uint8_t *psp = mcb_at(base, psp_seg);
    int n = 0, i;
    if (args && args[0]) {
        psp[0x81 + n++] = ' ';                         /* conventional leading space */
        for (i = 0; args[i] && n < 126; ++i) psp[0x81 + n++] = (uint8_t)args[i];
    }
    psp[0x80] = (uint8_t)n;
    psp[0x81 + n] = 0x0D;
}

/* ── SAVE THE LIVE INT 22h/23h/24h VECTORS INTO THE PSP. (GH #34) ─────────────
   DOS does this as part of building a PSP, and restores them on termination.
   Reads the IVT directly (segment 0), so it must run AFTER the host has planted
   its handlers -- saving a vector that is still 0000:0000 stores a null that the
   program will happily restore later.
   Measured on MS-DOS 6.22 (tools/dostest/p_psp.asm): the PSP's copy of all three
   EQUALS the live vector at program entry, which is the host-independent
   invariant the probe asserts. */
static inline void dos_psp_save_vectors(volatile uint8_t *base, uint16_t psp_seg,
                                        uint16_t parent_psp) {
    static const uint8_t vec[3] = { 0x22, 0x23, 0x24 };
    static const uint8_t at[3]  = { 0x0A, 0x0E, 0x12 };
    volatile uint8_t *psp = mcb_at(base, psp_seg);
    volatile uint8_t *ivt = mcb_at(base, 0);
    unsigned k, b;
    for (k = 0; k < 3; ++k)
        for (b = 0; b < 4; ++b)
            psp[at[k] + b] = ivt[vec[k] * 4 + b];
    mcb_wr16(psp + 0x16, parent_psp);          /* the parent's PSP segment */
}

/* The other half of the contract: put them back. A program that installed its
   own INT 24h must not leave it installed after it exits -- that is how one
   guest's critical-error handler ends up servicing the next one's failure. */
static inline void dos_psp_restore_vectors(volatile uint8_t *base, uint16_t psp_seg) {
    static const uint8_t vec[3] = { 0x22, 0x23, 0x24 };
    static const uint8_t at[3]  = { 0x0A, 0x0E, 0x12 };
    volatile uint8_t *psp = mcb_at(base, psp_seg);
    volatile uint8_t *ivt = mcb_at(base, 0);
    unsigned k, b;
    for (k = 0; k < 3; ++k)
        for (b = 0; b < 4; ++b)
            ivt[vec[k] * 4 + b] = psp[at[k] + b];
}

#endif /* DOS_PSP_H */
