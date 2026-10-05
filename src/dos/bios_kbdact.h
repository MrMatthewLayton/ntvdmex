/* bios_kbdact.h -- the BIOS INT 09h's side-calls (INT 1Bh, INT 05h, INT 15h AH=85h,
 * the pause loop) as guest code the host plants. GH #254. Source and rationale:
 * bios_kbdact.asm. Lives at DOS_CTAB_SEG:DOS_KBDACT_OFF (dos_layout.h); the V86
 * INT 09h arm resumes the guest at an entry when vdd_input_bios_consume() asks.
 * #244 added k4f (the INT 15h AH=4Fh intercept call) and #274 p5 (the default INT 05h,
 * print screen). All three BOP sites are BOP 09h, told apart by ADDRESS.
 * tests/unit/kbdact_test.c runs these bytes.
 */
#ifndef BIOS_KBDACT_H
#define BIOS_KBDACT_H

#define KBDACT_BRK    0x00     /* INT 1Bh                    */
#define KBDACT_PRT    0x04     /* INT 05h                    */
#define KBDACT_SYSD   0x08     /* INT 15h AX=8500h           */
#define KBDACT_SYSU   0x10     /* INT 15h AX=8501h           */
#define KBDACT_PAUSE  0x18     /* spin while 0040:0018 bit 3 */
#define KBDACT_IRET   0x03     /* a bare IRET (brk's last byte) */
#define KBDACT_K4F    0x29     /* #244: stc / int 15h / jnc / BOP 09h (translate AL) */
#define KBDACT_K4F_BOP 0x2E    /*        ...the BOP's address                         */
#define KBDACT_P5     0x37     /* #274: the default INT 05h (IVT[05h] points here)   */
#define KBDACT_P5_BEGIN 0x38   /*        BOP 09h: begin                              */
#define KBDACT_P5_NEXT  0x3F   /*        BOP 09h: next byte                          */

static const unsigned char bios_kbdact_code[69] = {
  0xfb, 0xcd, 0x1b, 0xcf, 0xfb, 0xcd, 0x05, 0xcf, 0x50, 0xb8, 0x00, 0x85,
  0xcd, 0x15, 0x58, 0xcf, 0x50, 0xb8, 0x01, 0x85, 0xcd, 0x15, 0x58, 0xcf,
  0xfb, 0x1e, 0x50, 0x31, 0xc0, 0x8e, 0xd8, 0xf6, 0x06, 0x18, 0x04, 0x08,
  0x75, 0xf9, 0x58, 0x1f, 0xcf, 0xf9, 0xcd, 0x15, 0x73, 0x03, 0xc4, 0xc4,
  0x09, 0xb0, 0x20, 0xe6, 0x20, 0x58, 0xcf, 0xfb, 0xc4, 0xc4, 0x09, 0x72,
  0x07, 0xcd, 0x17, 0xc4, 0xc4, 0x09, 0xeb, 0xf7, 0xcf
};

#endif /* BIOS_KBDACT_H */
