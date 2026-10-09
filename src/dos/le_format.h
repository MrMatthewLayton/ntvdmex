/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * The parts of the LE (linear executable) format the host reads: the header
 * fields DpmiLeLearn uses and the object table's entries (#333). Defines only.
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#ifndef NTVDMEX_DOS_LE_FORMAT_H
#define NTVDMEX_DOS_LE_FORMAT_H

#define LE_FORMAT_LEVEL         0x04    /* DWORD, 0 */
#define LE_CPU_TYPE             0x08    /* WORD, 1-4 (286 .. 586) */
#define LE_TARGET_OS            0x0A    /* WORD, 1-4 */
#define LE_OBJECT_TABLE         0x40    /* DWORD, from the LE header */
#define LE_OBJECT_COUNT         0x44
#define LE_HEADER_MIN           0x50    /* The object table starts after this */
#define LE_FIELD_TYPE_FIRST     1       /* The range a CPU or OS field holds */
#define LE_FIELD_TYPE_LAST      4
#define LE_OBJECTS_MAX          64      /* More than this is not a header */
#define LE_OBJECT_ENTRY_SIZE    24
#define LE_OBJECT_VIRTUAL_SIZE  0x00
#define LE_OBJECT_FLAGS         0x08
#define LE_OBJECT_EXECUTABLE    0x0004u

#endif /* NTVDMEX_DOS_LE_FORMAT_H */
