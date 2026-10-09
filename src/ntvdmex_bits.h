/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * The masks and shifts that select a byte, word or dword of a value (#333).
 *
 * One name per meaning, defined once (docs/STYLE.md, section 3). Each mask exists in the
 * C type the code already used it in: the plain form is an `int` literal, the `_U` form is
 * `unsigned`. They are not interchangeable -- an `int` mask and an `unsigned` mask give the
 * expression around them a different type, and that has changed the generated code before.
 *
 * Included by ntvdmex_types.h, so every file that has the base types has these too.
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#ifndef NTVDMEX_BITS_H
#define NTVDMEX_BITS_H

#define BYTE_MASK                   0xFF            /* The low 8 bits */
#define BYTE_MASK_U                 0xFFu
#define HIGH_BYTE_MASK              0xFF00          /* Bits 8-15 */
#define HIGH_BYTE_MASK_U            0xFF00u
#define WORD_MASK                   0xFFFF          /* The low 16 bits */
#define WORD_MASK_U                 0xFFFFu
#define HIGH_WORD_MASK_U            0xFFFF0000u     /* Bits 16-31 */
#define DWORD_MASK_U                0xFFFFFFFFu     /* All 32 bits */

#define NIBBLE_SHIFT                4               /* One hex digit */
#define NIBBLE_MASK                 0xF             /* The low hex digit */
#define NIBBLE_MASK_U               0xFu
#define BYTE_SHIFT                  8               /* One byte up: bits 8-15 */
#define WORD_SHIFT                  16              /* The high word: bits 16-31 */
#define TOP_BYTE_SHIFT              24              /* The top byte of a dword: bits 24-31 */
#define DWORD_SHIFT                 32              /* The high dword of a 64-bit value */
#define BITS_PER_BYTE               8
#define BITS_PER_DWORD              32
#define BYTE_VALUES                 256             /* The values a byte can hold */
#define DWORD_HEX_DIGITS            8
#define INT16_MAX_VALUE             32767           /* A signed WORD's range */
#define INT16_MIN_VALUE             (-32768)
#define INT8_MAX_VALUE              127             /* A signed BYTE's range */
#define INT8_MIN_VALUE              (-128)

/* Bit n of a bitmap of bytes: byte n >> BITMAP_BYTE_SHIFT, bit n & BITMAP_BIT_MASK. */
#define BITMAP_BYTE_SHIFT           3
#define BITMAP_BIT_MASK             7

/* Knuth's multiplicative hash: 2^32 divided by the golden ratio. */
#define KNUTH_HASH_MULTIPLIER_U     2654435761u

#endif /* NTVDMEX_BITS_H */
