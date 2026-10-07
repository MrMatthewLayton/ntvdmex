/* ne_format.h -- the New Executable format's field layout, for src/wow/ne.h.
 *
 * Kept apart from ne.h because ne.h's line numbers are its error codes (`Error` is the
 * __LINE__ that rejected an image), so it cannot grow. Like ne.h, it uses only the
 * portable Windows types, so the loader still builds off-VM for tests/unit/ne_test.c.
 */
#ifndef NTVDMEX_NE_FORMAT_H
#define NTVDMEX_NE_FORMAT_H

#include "../ntvdmex_types.h"

/* Little-endian words and dwords. */
#define NE_BYTE_SHIFT      8
#define NE_WORD_SHIFT      16
#define NE_HIGH_BYTE_SHIFT 24
#define NE_BYTE_MASK       0xFF
#define NE_WORD_BYTES      2
#define NE_WORD_BYTES_U    2u
#define NE_FARADDR_BYTES   4u
#define NE_LOWER_TO_UPPER  32       /* 'a' - 'A' */

/* The MZ stub: its size, and e_lfanew -- where the NE header is. */
#define NE_MZ_HEADER_SIZE  0x40
#define NE_MZ_LFANEW       0x3C

/* The NE header (offsets from its 'NE'). */
#define NE_HEADER_SIZE              0x40
#define NE_HDR_ENTRY_TABLE          0x04
#define NE_HDR_ENTRY_LENGTH         0x06
#define NE_HDR_PROGRAM_FLAGS        0x0C
#define NE_HDR_AUTODATA             0x0E
#define NE_HDR_HEAP                 0x10
#define NE_HDR_STACK                0x12
#define NE_HDR_CSIP                 0x14
#define NE_HDR_SSSP                 0x18
#define NE_HDR_SEGMENT_COUNT        0x1C
#define NE_HDR_MODULE_COUNT         0x1E
#define NE_HDR_NONRESIDENT_LENGTH   0x20
#define NE_HDR_SEGMENT_TABLE        0x22
#define NE_HDR_RESOURCE_TABLE       0x24
#define NE_HDR_RESIDENT_TABLE       0x26
#define NE_HDR_MODULE_TABLE         0x28
#define NE_HDR_IMPORT_TABLE         0x2A
#define NE_HDR_NONRESIDENT_OFFSET   0x2C   /* ABSOLUTE, and a DWORD */
#define NE_HDR_MOVABLE_COUNT        0x30
#define NE_HDR_ALIGN_SHIFT          0x32
#define NE_HDR_TARGET_OS            0x36
#define NE_HDR_OTHER_FLAGS          0x37
#define NE_HDR_EXPECTED_VERSION     0x3E
#define NE_DEFAULT_ALIGN_SHIFT      9      /* an align shift of 0 means 512 */

/* A segment-table entry: sector, length, flags, minalloc. */
#define NE_SEGENT_SIZE      8
#define NE_SEGENT_LENGTH    2
#define NE_SEGENT_FLAGS     4
#define NE_SEGENT_MINALLOC  6
#define NE_SEGMENT_64K      0x10000    /* what a length of 0 means */

/* Entry-table bundles: count, indicator, then entries. */
#define NE_BUNDLE_HEADER_SIZE      2
#define NE_ENTRY_MOVEABLE_SIZE     6u   /* flags, INT 3Fh, segment, offset */
#define NE_ENTRY_FIXED_SIZE        3u   /* flags, offset */
#define NE_ENTRY_MOVEABLE_SEGMENT  3
#define NE_ENTRY_MOVEABLE_OFFSET   4

/* Name tables: a length byte, the name, an ordinal word. */
#define NE_NAME_MIN_RECORD  3
#define NE_ORDINAL_BYTES    2u

/* Relocation records: addr_type, rel_type, site, then two target words. */
#define NE_RELOC_SIZE       8
#define NE_RELOC_SITE       2
#define NE_RELOC_TARGET_A   4
#define NE_RELOC_TARGET_B   6
#define NE_REL_TYPE_MASK    3
#define NE_CHAIN_END        0xFFFF

#endif /* NTVDMEX_NE_FORMAT_H */
