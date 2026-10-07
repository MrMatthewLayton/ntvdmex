/* vga_font.h -- the three character tables (8x8, 8x14, 8x16; 256 glyphs, bit 7 =
   leftmost pixel, code page 437 order).
 *
 * #322: NTVDMEX SHIPS NO FONT DATA. These were a dump of IBM's VGA ROM; they are now
 * filled at start-up from the machine's own fonts (src/host/sysfont.h): Fixedsys for
 * letters, digits and punctuation, Terminal for the DOS graphics characters, and
 * Terminal's own 8x8 size for the 8x8 table. The same arrays feed the text renderer
 * and the copies in guest memory behind INT 10h AX=1130h (vdd_video_install_fonts).
 * Zero until filled; the off-VM tests fill them with a synthetic pattern. */
#ifndef VGA_FONT_H
#define VGA_FONT_H
#include "../ntvdmex_types.h"

/* The tables' shape. */
#define VGA_FONT_CHARACTERS 256
#define VGA_FONT8_HEIGHT    8
#define VGA_FONT14_HEIGHT   14
#define VGA_FONT16_HEIGHT   16

extern BYTE g_VgaFont8x8[VGA_FONT_CHARACTERS][VGA_FONT8_HEIGHT];
extern BYTE g_VgaFont8x14[VGA_FONT_CHARACTERS][VGA_FONT14_HEIGHT];
extern BYTE g_VgaFont8x16[VGA_FONT_CHARACTERS][VGA_FONT16_HEIGHT];

#endif
