/* bios_bda.h -- the BIOS Data Area fields that describe the MACHINE, and the EBDA.
 * (GH #253, docs/inventory/bda.md 1 and 6)
 *
 * Host-testable in the same shape as dos_mcb.h: every routine takes a caller-supplied
 * `base` (NULL for the host's absolute V86 addressing, a 1 MB buffer off-VM).
 * Verified off-VM by tools/dostest/bda_test.c.
 *
 * ── ★★★ TWO DOORS ONTO ONE VALUE, AGAIN. ──────────────────────────────────────────
 *   On a real BIOS, INT 11h IS `mov ax,[0040:0010] / iret` and INT 12h IS
 *   `mov ax,[0040:0013] / iret`: POST computes the machine's description ONCE, parks it
 *   in the BDA, and the interrupts are just readers. Ours computed both answers on every
 *   call (bios_equipment_word(), DOS_MEM_TOP) and never wrote the BDA at all, so a guest
 *   that reads 0040:0010 directly -- which many do, it is cheaper than an INT -- got a
 *   word of zeroes: no floppy, no video, no serial, no printer. Same shape as the
 *   equipment word vs the 0040:0000 port table that cost COMM.DRV a session.
 *   ⇒ The interrupts stay the computers, and the BDA is written FROM THE SAME
 *     FUNCTIONS, at start-up and whenever a setting that feeds them changes. The
 *     values cannot drift because there is only one place each is derived.
 *
 * ── ★★ THE EBDA STORY, SETTLED: THERE IS ONE, 1 KB AT 9FC0h. ──────────────────────
 *   Three answers said "no EBDA" (0040:000E = 0, INT 15h AH=C1h CF=1, the AH=C0h table's
 *   feature-1 bit 2 clear) while two said there was one: INT 12h = 639 KB and the MCB
 *   chain ending at 9FC0h, both MEASURED on MS-DOS 6.22 (p_bios int12.memk = 027Fh,
 *   p_mcb mcb.chain.ends.at = 9FC0h) and both already agreeing with that oracle.
 *   The machine behind that oracle is SeaBIOS, which DOES allocate a 1 KB EBDA at 9FC0h
 *   (its C1h answers ES=9FC0 CF=0, its C0h feature byte is 74h -- bit 2 set). So 639 is
 *   not a free-floating number, it is half of a machine that has an EBDA, and the honest
 *   way to make the five answers agree is to give the guest the EBDA the 639 implies,
 *   not to move the two values the oracle already confirms.
 *   ⚠ The period AMI 486 under PCem has NO EBDA (C1h CF=1, C0h feature 70h). It is not
 *     the model for this one field: its INT 12h was never measured, and moving to 640 KB
 *     would change int12.memk, mcb.chain.ends.at and every PSP+02h, all of which agree
 *     with 6.22 now. Recorded in oracle-rules.json (int15.c0.table, int15.c1.status).
 *   ⚠ NOTHING OF OURS LIVES AT 9FC0:0000. dos_mcb_reserve_top() carves its reservations
 *     out of the 'Z' block BELOW DOS_MEM_TOP, the WOW launch allocates through the same
 *     chain, and the CMOS still reports 640 KB of base memory (vdd_cmos.c) -- which is
 *     right: CMOS 15h/16h is the memory FITTED, INT 12h is what is left after the BIOS
 *     takes its EBDA, and on a real board the two differ by exactly this kilobyte.
 */
#ifndef BIOS_BDA_H
#define BIOS_BDA_H

#include <stdint.h>
#include "dos_mcb.h"            /* DOS_MEM_TOP -- where conventional memory ends */

/* The EBDA is the kilobyte between the end of DOS's memory and the 640 KB line. */
#define BIOS_EBDA_SEG     DOS_MEM_TOP                              /* 0x9FC0          */
#define BIOS_EBDA_KB      ((0xA000u - BIOS_EBDA_SEG) * 16u / 1024u)   /* 1           */
/* INT 12h's answer and 0040:0013: conventional memory BELOW the EBDA, in KB.  */
#define BIOS_BASE_MEM_KB  ((DOS_MEM_TOP * 16u) / 1024u)            /* 639             */

/* ── #136: CONVENTIONAL MEMORY IS A SETTING, AND IT MEANS MEMORY FITTED. ─────────────
     Settings > Machine > Memory > Conventional Memory (KB), 64..640, default 640. The number is
     what the board CARRIES -- what CMOS 15h/16h says -- and everything else follows from
     it exactly as it does at 640: the BIOS takes its 1 KB EBDA off the top, INT 12h and
     0040:0013 report what is left, and DOS's arena ends where the EBDA starts. So a
     512 KB machine reads 511 KB from INT 12h, EBDA at 7FC0h, MCB chain ending at 7FC0h --
     the same shape as 640 / 639 / 9FC0h, which is what both oracles measure (p_bios
     int12.memk 027Fh, p_mcb mcb.chain.ends.at 9FC0h).
   ★ 640 MAPS TO EXACTLY DOS_MEM_TOP, so the default machine is byte-identical: these
     are the same expressions BIOS_EBDA_SEG and BIOS_BASE_MEM_KB were, with the 640 a
     parameter instead of a constant. Checked off-VM by bda_test.c.
   ⚠ WHAT IS NOT MODELLED: the memory between the new top and A0000h is still RAM. A
     real 512 KB board has nothing there (reads float to FFh); ours keeps the host's
     committed pages. Nothing of ours hands it out, so DOS, INT 12h, the BDA and CMOS all
     agree the machine is smaller -- but a program that sizes memory by WRITING to it
     (some diagnostics do) would find more than it was told. Unmeasured: no probe here
     does that. */
#define BIOS_CONV_KB_MIN   64u
#define BIOS_CONV_KB_MAX   640u
#define BIOS_EBDA_PARAS    0x40u                                   /* 1 KB            */
static inline unsigned bios_conv_kb_clamp(unsigned kb) {
    return kb < BIOS_CONV_KB_MIN ? BIOS_CONV_KB_MIN
         : kb > BIOS_CONV_KB_MAX ? BIOS_CONV_KB_MAX : kb;
}
/* The paragraph DOS's arena ends at, and the EBDA starts at, for `kb` fitted. */
static inline uint16_t bios_conv_top_para(unsigned kb) {
    return (uint16_t)(bios_conv_kb_clamp(kb) * 64u - BIOS_EBDA_PARAS);
}
/* INT 12h's answer for a given top -- the memory below the EBDA, in KB. */
static inline uint16_t bios_base_kb_of_top(uint16_t top) {
    return (uint16_t)(((uint32_t)top * 16u) / 1024u);
}

/* BDA offsets (from linear 0x400) this header owns. */
#define BDA_EBDA_SEG      0x0E      /* WORD: EBDA segment (AT and later)          */
#define BDA_EQUIPMENT     0x10      /* WORD: the equipment word, = INT 11h       */
#define BDA_MEM_KB        0x13      /* WORD: base memory in KB,  = INT 12h       */

static inline volatile uint8_t *bios_lin(volatile uint8_t *base, uint32_t lin) {
    return (volatile uint8_t *)((uintptr_t)base + lin);
}
static inline void bios_wr16(volatile uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)(v >> 8);
}

/* 0040:0010 only -- the live half, for a setting (the joystick type) that changes the
   equipment word while the guest runs. Same function INT 11h calls; see bios_bda_init. */
static inline void bios_bda_set_equipment(volatile uint8_t *base, uint16_t equip) {
    bios_wr16(bios_lin(base, 0x400u + BDA_EQUIPMENT), equip);
}

/* What POST would have left behind for these fields. `equip` must be the value INT 11h
   answers (the host passes bios_equipment_word()); the memory size and the EBDA come
   from DOS_MEM_TOP, the same constant INT 12h and the MCB chain are built from.
   The EBDA's first byte is its own size in KB (IBM PS/2 BIOS TR; RBIL MEMORY.LST
   "EBDA:0000"), which is what a program that walks it -- or relocates it, as some
   memory managers do -- reads first. The rest is zeroed: everything else in an EBDA is
   PS/2-mouse and vendor state for devices this machine does not model. */
/* #136: `top` is the EBDA's segment = the end of DOS's arena (bios_conv_top_para). */
static inline void bios_bda_init_top(volatile uint8_t *base, uint16_t equip, uint16_t top) {
    volatile uint8_t *ebda = bios_lin(base, (uint32_t)top << 4);
    unsigned k;
    bios_wr16(bios_lin(base, 0x400u + BDA_EBDA_SEG), top);
    bios_bda_set_equipment(base, equip);
    bios_wr16(bios_lin(base, 0x400u + BDA_MEM_KB), bios_base_kb_of_top(top));
    for (k = 0; k < BIOS_EBDA_KB * 1024u; ++k) ebda[k] = 0;
    ebda[0] = (uint8_t)BIOS_EBDA_KB;
}
static inline void bios_bda_init(volatile uint8_t *base, uint16_t equip) {
    bios_bda_init_top(base, equip, (uint16_t)BIOS_EBDA_SEG);
}

#endif /* BIOS_BDA_H */
