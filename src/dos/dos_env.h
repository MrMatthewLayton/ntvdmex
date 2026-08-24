/* dos_env.h -- build a DOS environment block in conventional memory. Pure logic
 * over a `base` pointer (same convention as dos_mcb.h). M2.5.
 *
 * Layout DOS programs expect at the environment segment (PSP:0x2C):
 *   NAME=VALUE\0 NAME=VALUE\0 ... \0      (a trailing \0 ends the variable list)
 *   <WORD count = 1>                       (DOS 3.0+: number of strings following)
 *   <program full path>\0                  (argv[0] for the guest)
 * An empty/short env is a common cause of real tools mis-starting; a well-formed
 * block (COMSPEC etc. + the program path) is what they read.
 */
#ifndef DOS_ENV_H
#define DOS_ENV_H

#include <stdint.h>
#include "dos_mcb.h"

static inline volatile uint8_t *dos_env_puts(volatile uint8_t *p, const char *s) {
    while (*s) *p++ = (uint8_t)*s++;
    return p;
}

/* Build a standard environment at env_seg and append the program path. Returns the
   number of bytes written (fits in the 0x10-paragraph env block laid by dos_mcb_init). */
static inline uint32_t dos_env_build(volatile uint8_t *base, uint16_t env_seg,
                                     const char *progpath) {
    volatile uint8_t *e = mcb_at(base, env_seg);
    volatile uint8_t *p = e;
    p = dos_env_puts(p, "COMSPEC=C:\\COMMAND.COM"); *p++ = 0;
    p = dos_env_puts(p, "PATH=C:\\");               *p++ = 0;
    p = dos_env_puts(p, "PROMPT=$p$g");             *p++ = 0;
    /* ── BLASTER IS HOW A DOS GAME FINDS THE SOUND CARD. ─────────────────────────
         Every Sound Blaster install sets it, so every DOS sound driver reads it and
         only falls back to probing when it is absent -- and a fallback probe is a
         WORSE test of our card than being told where it is, because it also has to
         guess the IRQ and DMA channel. The values are the ones vdd_sb is actually
         configured with (SB_DEFAULT_BASE / SB_DEFAULT_IRQ / channel 1); T3 = an
         SB 2.0-class card, which matches the DSP version the VDD reports.
         ⚠ THESE MUST TRACK vdd_sb.h. Telling the guest I7 while the VDD raises IRQ5
           is worse than saying nothing: a driver that believes the string masks the
           line it was told about and waits on an interrupt that arrives elsewhere. */
    p = dos_env_puts(p, "BLASTER=A220 I5 D1 T3");   *p++ = 0;
    *p++ = 0;                                       /* trailing \0 ends the var list */
    *p++ = 0x01; *p++ = 0x00;                       /* WORD: one string follows */
    p = dos_env_puts(p, (progpath && progpath[0]) ? progpath : "C:\\PROGRAM.COM");
    *p++ = 0;
    return (uint32_t)(p - e);
}

#endif /* DOS_ENV_H */
