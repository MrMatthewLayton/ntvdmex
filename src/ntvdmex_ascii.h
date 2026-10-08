/* ntvdmex_ascii.h -- the ASCII control characters and text bytes the host uses (#333).
 * Defines only; included by ntvdmex_types.h.
 */
#ifndef NTVDMEX_ASCII_H
#define NTVDMEX_ASCII_H

#define ASCII_NUL           0x00
#define ASCII_BACKSPACE     0x08
#define ASCII_TAB           9
#define ASCII_LF            0x0A
#define ASCII_CR            0x0D
#define ASCII_END_OF_FILE   0x1A    /* ^Z                                        */
#define ASCII_SPACE         0x20    /* and the first printable character         */
#define ASCII_CASE_BIT      0x20    /* 'a' - 'A'                                 */

#endif /* NTVDMEX_ASCII_H */
