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
/* ── ★ PATH IS NOT DECORATION -- krnl386 SEARCHES IT. (GH #128, session 37) ──────
     `PATH=C:\` was fine while every guest was a DOS program launched by full path.
     It is not fine for WOW: krnl386's file search (`seg1:0x1dad`) walks the
     environment block for `PATH=`, splits it on `;`, and tries each directory in
     turn -- and the module it looks for that way is **WOWEXEC.EXE**, the Win16
     program itself, whose name comes from `[boot] WOWSHELL` in SYSTEM.INI as a bare
     filename with no directory. With `C:\` the only entry, the search could never
     succeed, and krnl386 reported it the way it reports any module it cannot find:
     "Please re-install the following module to your system32 directory: WOWEXEC.EXE".
   ⚠ Passed in rather than hardcoded here, and defaulted to the old value, so a DOS
     guest takes a byte-identical path to before. The WOW caller passes the host's
     REAL Windows and system directories -- which is also what a real WOW launch
     gets, since ntvdm inherits the NT process environment. */
/* ⚠⚠ THE BLOCK IS 256 BYTES AND WHAT FOLLOWS IT IS A LANDMINE. DOS_ENV_SEG is
     0x0060 and the DOS-resident filler block starts at 0x0070, so this has exactly
     0x10 paragraphs -- and linear 0x714, a few bytes into that block, is the NT
     kernel's VDM interrupt-state dword, which dos_layout.h records as breaking
     EVERY guest when written from user mode. Nothing here was bounded before,
     which was survivable while every string was a literal and stopped being so the
     moment PATH and the program path became host-supplied. Both are truncated. */
#define DOS_ENV_CAP   0x100

static inline volatile uint8_t *dos_env_putv(volatile uint8_t *p,
                                             volatile uint8_t *end, const char *s) {
    while (*s && p < end) *p++ = (uint8_t)*s++;
    return p;
}

static inline uint32_t dos_env_build_path(volatile uint8_t *base, uint16_t env_seg,
                                          const char *progpath, const char *pathvar) {
    volatile uint8_t *e = mcb_at(base, env_seg);
    volatile uint8_t *p = e;
    /* Leave room for the trailing NUL, the count WORD, the program path and its
       NUL, so the tail krnl386 actually reads can never be the part that is lost. */
    volatile uint8_t *vend = e + DOS_ENV_CAP - 8;
    p = dos_env_putv(p, vend, "COMSPEC=C:\\COMMAND.COM"); *p++ = 0;
    p = dos_env_putv(p, vend, "PATH=");
    p = dos_env_putv(p, vend, (pathvar && pathvar[0]) ? pathvar : "C:\\");  *p++ = 0;
    p = dos_env_putv(p, vend, "PROMPT=$p$g");             *p++ = 0;
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
    p = dos_env_putv(p, vend, "BLASTER=A220 I5 D1 T3"); *p++ = 0;
    *p++ = 0;                                       /* trailing \0 ends the var list */
    *p++ = 0x01; *p++ = 0x00;                       /* WORD: one string follows */
    p = dos_env_putv(p, e + DOS_ENV_CAP - 1,
                     (progpath && progpath[0]) ? progpath : "C:\\PROGRAM.COM");
    *p++ = 0;
    return (uint32_t)(p - e);
}

static inline uint32_t dos_env_build(volatile uint8_t *base, uint16_t env_seg,
                                     const char *progpath) {
    return dos_env_build_path(base, env_seg, progpath, "C:\\");
}

#endif /* DOS_ENV_H */
