/* bios_kbdact.h -- the BIOS INT 09h's side-calls (INT 1Bh, INT 05h, INT 15h AH=85h,
 * the pause loop) as guest code the host plants. GH #254. Source and rationale:
 * bios_kbdact.asm. Lives at DOS_CTAB_SEG:DOS_KBDACT_OFF (dos_layout.h); the V86
 * INT 09h arm resumes the guest at an entry when VddInputBiosConsume() asks.
 * #244 added k4f (the INT 15h AH=4Fh intercept call) and #274 p5 (the default INT 05h,
 * print screen). All three BOP sites are BOP 09h, told apart by ADDRESS.
 * tests/unit/kbdact_test.c runs these bytes.
 */
#ifndef NTVDMEX_DOS_BIOS_KBDACT_H
#define NTVDMEX_DOS_BIOS_KBDACT_H

#include "../ntvdmex_types.h"

/* The entries, as offsets into g_BiosKeyboardActionCode (labels in bios_kbdact.asm). */
#define BIOS_KEYBOARD_ACTION_BREAK         0x00   /* brk:   INT 1Bh                    */
#define BIOS_KEYBOARD_ACTION_PRINT_SCREEN  0x04   /* prt:   INT 05h                    */
#define BIOS_KEYBOARD_ACTION_SYSREQ_DOWN   0x08   /* sysd:  INT 15h AX=8500h           */
#define BIOS_KEYBOARD_ACTION_SYSREQ_UP     0x10   /* sysu:  INT 15h AX=8501h           */
#define BIOS_KEYBOARD_ACTION_PAUSE         0x18   /* pause: spin while 0040:0018 bit 3 */
#define BIOS_KEYBOARD_ACTION_IRET          0x03   /* a bare IRET (brk's last byte) */
/* #244 k4f: stc / int 15h / jnc / BOP 09h (translate AL), and the BOP's address. */
#define BIOS_KEYBOARD_ACTION_INTERCEPT     0x29
#define BIOS_KEYBOARD_ACTION_INTERCEPT_BOP 0x2E
/* #274 p5: the default INT 05h (IVT[05h] points here), and its two BOP 09h sites. */
#define BIOS_KEYBOARD_ACTION_DEFAULT_INT05       0x37
#define BIOS_KEYBOARD_ACTION_DEFAULT_INT05_BEGIN 0x38   /* BOP 09h: begin     */
#define BIOS_KEYBOARD_ACTION_DEFAULT_INT05_NEXT  0x3F   /* BOP 09h: next byte */

/* The assembled bios_kbdact.asm, byte for byte. */
#define BIOS_KEYBOARD_ACTION_CODE_SIZE     69

static const BYTE g_BiosKeyboardActionCode[BIOS_KEYBOARD_ACTION_CODE_SIZE] = {
  0xfb, 0xcd, 0x1b, 0xcf, 0xfb, 0xcd, 0x05, 0xcf, 0x50, 0xb8, 0x00, 0x85,
  0xcd, 0x15, 0x58, 0xcf, 0x50, 0xb8, 0x01, 0x85, 0xcd, 0x15, 0x58, 0xcf,
  0xfb, 0x1e, 0x50, 0x31, 0xc0, 0x8e, 0xd8, 0xf6, 0x06, 0x18, 0x04, 0x08,
  0x75, 0xf9, 0x58, 0x1f, 0xcf, 0xf9, 0xcd, 0x15, 0x73, 0x03, 0xc4, 0xc4,
  0x09, 0xb0, 0x20, 0xe6, 0x20, 0x58, 0xcf, 0xfb, 0xc4, 0xc4, 0x09, 0x72,
  0x07, 0xcd, 0x17, 0xc4, 0xc4, 0x09, 0xeb, 0xf7, 0xcf
};

#endif /* NTVDMEX_DOS_BIOS_KBDACT_H */
