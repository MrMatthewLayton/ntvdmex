/* pif.h -- read a Program Information File: which program, which arguments, which
 * directory. Header-only and free of Win32 so the off-VM battery can hold it to a
 * real PIF's bytes (tests/unit/pif_test.c).
 *
 * WHY. Explorer launches a .PIF by queuing the PIF ITSELF as the VDM's program
 * (measured on the rig, s85: `command fetch ... app=[C:\QB45\QB.PIF] args=[]`). Stock
 * NTVDM reads it and runs the program it names. We handed the PIF's bytes to
 * COMMAND.COM as a program, which is the "sidescrolling cursor" the user saw, and
 * QuickBASIC's `/L` (needed for CALL ABSOLUTE) never arrived.
 *
 * THE FORMAT (Windows 3.x PIF, 0x171-byte basic section, then extension sections):
 *   0x02  title[30]           0x24  program[63]        0x65  start directory[64]
 *   0xA5  parameters[64]
 *   0x171 the first extension header, "MICROSOFT PIFEX"; each header is
 *         name[16], next-header offset (word, 0xFFFF = last), data offset, data length.
 *   "WINDOWS 386 3.0" data +0x28: parameters[64] -- what Windows 3.x and NT use when
 *   present, so it wins over the basic section's copy.
 * Strings are space- or NUL-padded; both are trimmed.
 */
#ifndef NTVDMEX_PIF_H
#define NTVDMEX_PIF_H
#include "../ntvdmex_types.h"
#define PIF_SECTION_WINDOWS_386 "WINDOWS 386 3.0"   /* the 386-enhanced extension block */

#define PIF_BASIC_LEN   0x171
#define PIF_PROG_OFF    0x24
#define PIF_PROG_LEN    63
#define PIF_DIR_OFF     0x65
#define PIF_DIR_LEN     64
#define PIF_PARAMS_OFF  0xA5
#define PIF_PARAMS_LEN  64
#define PIF_W386_PARAMS 0x28
/* An extension header: name[16], then three words. */
#define PIF_EXT_NAME_LENGTH   16
#define PIF_EXT_NEXT          16    /* the next header's offset; PIF_EXT_LAST = none */
#define PIF_EXT_DATA_OFFSET   18
#define PIF_EXT_DATA_LENGTH   20
#define PIF_EXT_HEADER_SIZE   22
#define PIF_EXT_LAST          0xFFFF
#define PIF_EXT_MAX_SECTIONS  16    /* a malformed chain cannot loop past this */

typedef struct _PIF_INFO {
    char Program[PIF_PROG_LEN + 1];       /* char, not CHAR: the spelling moves code (#333) */
    char Directory[PIF_DIR_LEN + 1];
    char Parameters[PIF_PARAMS_LEN + 1];
    INT  IsParametersFrom386;          /* 1 = the WINDOWS 386 3.0 section supplied them */
} PIF_INFO, *PPIF_INFO;

/* `unsigned`, not UINT, in this file: the spelling moved code in main.c (#333). */



/* 1 if `bytes` looks like a PIF and `out` was filled; 0 otherwise. An MZ image, or
   anything shorter than the basic section, is not a PIF. */
INT PifParse(PCBYTE bytes, unsigned long length, PPIF_INFO out);

#endif
