/* ntvdmex_bits.h -- the masks and shifts that select a byte, word or dword of a value (#333).
 *
 * One name per meaning, defined once (docs/STYLE.md, section 3). Each mask exists in the
 * C type the code already used it in: the plain form is an `int` literal, the `_U` form is
 * `unsigned`. They are not interchangeable -- an `int` mask and an `unsigned` mask give the
 * expression around them a different type, and that has changed the generated code before.
 *
 * Included by ntvdmex_types.h, so every file that has the base types has these too.
 */
#ifndef NTVDMEX_BITS_H
#define NTVDMEX_BITS_H

#define BYTE_MASK           0xFF            /* the low 8 bits                       */
#define BYTE_MASK_U         0xFFu
#define HIGH_BYTE_MASK      0xFF00          /* bits 8-15                            */
#define HIGH_BYTE_MASK_U    0xFF00u
#define WORD_MASK           0xFFFF          /* the low 16 bits                      */
#define WORD_MASK_U         0xFFFFu
#define HIGH_WORD_MASK_U    0xFFFF0000u     /* bits 16-31                           */
#define DWORD_MASK_U        0xFFFFFFFFu     /* all 32 bits                          */

#define NIBBLE_SHIFT        4               /* one hex digit                        */
#define NIBBLE_MASK         0xF             /* the low hex digit                    */
#define BYTE_SHIFT          8               /* one byte up: bits 8-15               */
#define WORD_SHIFT          16              /* the high word: bits 16-31            */
#define TOP_BYTE_SHIFT      24              /* the top byte of a dword: bits 24-31  */
#define DWORD_SHIFT         32              /* the high dword of a 64-bit value     */
#define BITS_PER_BYTE       8
#define BITS_PER_DWORD      32
#define BYTE_VALUES         256             /* the values a byte can hold           */
#define DWORD_HEX_DIGITS    8

/* Bit n of a bitmap of bytes: byte n >> BITMAP_BYTE_SHIFT, bit n & BITMAP_BIT_MASK. */
#define BITMAP_BYTE_SHIFT   3
#define BITMAP_BIT_MASK     7

#endif /* NTVDMEX_BITS_H */
