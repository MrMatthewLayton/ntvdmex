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
#include <stdint.h>

extern uint8_t vga_font_8x8[256][8];
extern uint8_t vga_font_8x14[256][14];
extern uint8_t vga_font_8x16[256][16];

#endif
