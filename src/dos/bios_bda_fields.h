/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * The BIOS data area's fields, as offsets from 0040:0000 (#333).
 *
 * One home for every BDA field the host, the DOS layer and the device models touch.
 * Defines only, so the device models can include it without the DOS layer.
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#ifndef NTVDMEX_DOS_BIOS_BDA_FIELDS_H
#define NTVDMEX_DOS_BIOS_BDA_FIELDS_H

#define BIOS_BDA_BASE                       0x400u      /* The BDA's linear address */
#define BIOS_BDA_KEY_ENTRY_SIZE             2           /* One ring entry: AL then AH */
#define BIOS_BDA_CURSOR_ENTRY_SIZE          2
#define BIOS_BDA_LPT_BASES                  0x08        /* WORD x3: LPT1-3 base ports */
#define BIOS_BDA_COM_BASES                  0x00        /* WORD x4: COM1-4 base ports */
#define BIOS_BDA_COM_PORTS                  4
#define BIOS_BDA_EBDA_SEGMENT               0x0E        /* WORD: EBDA segment (AT and later) */
#define BIOS_BDA_EQUIPMENT                  0x10        /* WORD: the equipment word, = INT 11h */

/* The equipment word's bits (INT 11h). */
#define BIOS_EQUIPMENT_FLOPPY               0x0001
#define BIOS_EQUIPMENT_FPU                  0x0002
#define BIOS_EQUIPMENT_VIDEO_80X25_COLOUR   0x0020
#define BIOS_EQUIPMENT_SERIAL_MASK_U        0x0E00u     /* Bits 9-11: how many serial ports */
#define BIOS_EQUIPMENT_SERIAL_SHIFT         9
#define BIOS_EQUIPMENT_SERIAL_COUNT_MASK    7
#define BIOS_EQUIPMENT_GAMEPORT             0x1000
#define BIOS_EQUIPMENT_ONE_PARALLEL         0x4000      /* Bits 14-15 = 1 */
#define BIOS_BDA_MEMORY_KB                  0x13        /* WORD: base memory in KB, = INT 12h */
#define BIOS_BDA_SHIFT_FLAGS                0x17        /* Shift/ctrl/alt + lock state (INT 16h AH=02) */
#define BIOS_BDA_SHIFT_FLAGS2               0x18        /* Extended shift flags (INT 16h AH=12) */
#define BIOS_BDA_ALT_KEYPAD                 0x19        /* The Alt+keypad accumulator (#274) */
#define BIOS_BDA_KEYBOARD_HEAD              0x1A        /* Offsets from 0040:0000 */
#define BIOS_BDA_KEYBOARD_TAIL              0x1C
#define BIOS_BDA_KEYBOARD_BUFFER            0x1E        /* 16 entries, 2 bytes each -- POST's bounds */
#define BIOS_BDA_KEYBOARD_BUFFER_END        0x3E        /* One past the last entry -- POST's bounds */
#define BIOS_BDA_VIDEO_MODE                 0x49
#define BIOS_BDA_VIDEO_COLUMNS              0x4A
#define BIOS_BDA_VIDEO_COLUMNS_HIGH         0x4B
#define BIOS_BDA_VIDEO_PAGE_SIZE            0x4C
#define BIOS_BDA_VIDEO_PAGE_SIZE_HIGH       0x4D
#define BIOS_BDA_VIDEO_PAGE_OFFSET          0x4E
#define BIOS_BDA_VIDEO_PAGE_OFFSET_HIGH     0x4F
#define BIOS_BDA_CURSOR_COLUMN              0x50
#define BIOS_BDA_CURSOR_ROW                 0x51
#define BIOS_BDA_CURSOR_SHAPE               0x60
#define BIOS_BDA_CURSOR_SHAPE_HIGH          0x61
#define BIOS_BDA_ACTIVE_PAGE                0x62
#define BIOS_BDA_CRTC_PORT                  0x63
#define BIOS_BDA_CRTC_PORT_HIGH             0x64
#define BIOS_BDA_CGA_MODE_SELECT            0x65
#define BIOS_BDA_CGA_PALETTE                0x66
#define BIOS_BDA_TICK_COUNT                 0x6C        /* 0040:006C, DWORD */
#define BIOS_BDA_TICK_COUNT_HIGH            0x6E        /* The tick count's high word */
#define BIOS_BDA_MIDNIGHT_FLAG              0x70        /* 0040:0070, BYTE */
#define BIOS_BDA_BREAK_FLAG                 0x71
#define BIOS_BDA_FIXED_DISK_COUNT           0x75        /* Number of fixed disks (INT 13h DL=80h) */
#define BIOS_BDA_KEYBOARD_START_POINTER     0x80
#define BIOS_BDA_KEYBOARD_END_POINTER       0x82
#define BIOS_BDA_VIDEO_ROWS                 0x84
#define BIOS_BDA_CHARACTER_HEIGHT           0x85
#define BIOS_BDA_CHARACTER_HEIGHT_HIGH      0x86
#define BIOS_BDA_EGA_INFO                   0x87
#define BIOS_BDA_EGA_SWITCHES               0x88
#define BIOS_BDA_VGA_FLAGS                  0x89
#define BIOS_BDA_KEYBOARD_FLAGS3            0x96
#define BIOS_BDA_WAIT_FLAG_POINTER          0x98        /* INT 15h AH=83h: the flag's far pointer */
#define BIOS_BDA_WAIT_FLAG_SEGMENT          0x9A
#define BIOS_BDA_WAIT_COUNT                 0x9C        /* DWORD: microseconds left */
#define BIOS_BDA_WAIT_ACTIVE                0xA0
#define BIOS_BDA_WAIT_IN_PROGRESS           0x01        /* 40:A0 bit 0: an AH=83h wait counts */
#define BIOS_BDA_WAIT_NONE                  0x00
#define BIOS_BDA_LPT_PORTS                  3
#define BIOS_BDA_VIDEO_SAVE_POINTER         0xA8

#endif /* NTVDMEX_DOS_BIOS_BDA_FIELDS_H */
