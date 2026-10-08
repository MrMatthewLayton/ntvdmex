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
#define ASCII_DELETE        0x7F    /* one past the last printable character     */

/* Reading a number from text, one digit at a time. */
#define DECIMAL_RADIX       10
#define DECIMAL_RADIX_U     10u
#define HEX_RADIX           16
#define HEX_RADIX_U         16u
#define HEX_DIGIT_A_VALUE   10      /* 'A'/'a': the first hex digit that is a letter */

#endif /* NTVDMEX_ASCII_H */
