/* bios_kbdact.h -- the BIOS INT 09h's side-calls (INT 1Bh, INT 05h, INT 15h AH=85h,
 * the pause loop) as guest code the host plants. GH #254. Source and rationale:
 * bios_kbdact.asm. Lives at DOS_CTAB_SEG:DOS_KBDACT_OFF (dos_layout.h); the V86
 * INT 09h arm resumes the guest at an entry when vdd_input_bios_consume() asks.
 * tools/dostest/kbdact_test.c runs these bytes.
 */
#ifndef BIOS_KBDACT_H
#define BIOS_KBDACT_H

#define KBDACT_BRK    0x00     /* INT 1Bh                    */
#define KBDACT_PRT    0x04     /* INT 05h                    */
#define KBDACT_SYSD   0x08     /* INT 15h AX=8500h           */
#define KBDACT_SYSU   0x10     /* INT 15h AX=8501h           */
#define KBDACT_PAUSE  0x18     /* spin while 0040:0018 bit 3 */

static const unsigned char bios_kbdact_code[41] = {
  0xfb, 0xcd, 0x1b, 0xcf, 0xfb, 0xcd, 0x05, 0xcf, 0x50, 0xb8, 0x00, 0x85,
  0xcd, 0x15, 0x58, 0xcf, 0x50, 0xb8, 0x01, 0x85, 0xcd, 0x15, 0x58, 0xcf,
  0xfb, 0x1e, 0x50, 0x31, 0xc0, 0x8e, 0xd8, 0xf6, 0x06, 0x18, 0x04, 0x08,
  0x75, 0xf9, 0x58, 0x1f, 0xcf
};

#endif /* BIOS_KBDACT_H */
