/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * THE GUEST'S OWN RESOURCES, AS REAL Win32 OBJECTS. GH #128, s.43.
 *
 * WHY THE HOST READS THE FILE ITSELF:
 * A Win16 program's menu, icons and accelerators live in its NE module, and the
 * 32-bit side cannot ask the 16-bit loader for them -- krnl386 hands out handles
 * into ITS address space, not bytes we can pass to Win32. But the file is right
 * there on disk and this host already knows its path (it is what it launched), so
 * the resources can simply be read.
 *
 * [CAUTION]: THE APPLICATION'S OWN FILE, and only that. A class registered by a DLL would
 * name a resource in the DLL, and answering it out of the EXE would be a wrong
 * menu rather than a missing one. Every lookup says which file it searched.
 *
 * BOTH LAYOUTS WERE CONFIRMED AGAINST THE DATA BEFORE THIS EXISTED:
 * `tools/ne/neres.py` decodes the same two structures offline, and its output for
 * NOTEPAD.EXE's MENU #1 is:
 *
 *   &File -> &New / &Open... / &Save / Save &As... / &Print / Page Se&tup... /
 *            P&rint Setup... / --- / E&xit
 *   &Edit -> &Undo Ctrl+Z / --- / Cu&t Ctrl+X / &Copy Ctrl+C / ...
 *   &Search -> &Find... / Find &Next F3
 *   &Help -> &Contents / ... / &About Notepad...
 *
 * A wrong offset does not accidentally spell "&About Notepad...". That is the
 * whole reason the decoder was written as a tool first: the reading checks itself.
 *
 * THE TWO FORMATS:
 * RESOURCE TABLE (at the NE header + 0x24): a WORD alignment shift, then TYPEINFO
 * records -- {WORD type, WORD count, DWORD reserved} followed by `count`
 * NAMEINFOs of {WORD offset, WORD length, WORD flags, WORD id, WORD, WORD} --
 * terminated by a zero type. Offsets and lengths are in alignment units. An id or
 * type with the high bit set is an integer; otherwise it is an offset to a
 * length-prefixed name.
 *
 * MENU TEMPLATE: WORD version, WORD headerSize, then items. An item is a WORD of
 * flags, then (unless it is a popup) a WORD id, then an ASCIIZ label. `MF_POPUP`
 * opens a submenu; `MF_END` closes the current level.
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#ifndef NTVDMEX_WOWRES_H
#define NTVDMEX_WOWRES_H

#define WOWRES_MAX_FILE             (2u * 1024u * 1024u)
#define WOWRES_RT_MENU              4
#define WOWRES_RT_ACCEL             9
#define WOWRES_MF_POPUP             0x0010
#define WOWRES_MF_END               0x0080
#define WOWRES_PATH_MAX             512

/* The file's layout: the MZ stub's pointer to the NE header, the NE header's to the
 * resource table, and the table's records (see the note above).
 */
#define WOWRES_MZ_HEADER_SIZE       0x40
#define WOWRES_MZ_NE_OFFSET         0x3C
#define WOWRES_NE_HEADER_SIZE       0x40
#define WOWRES_NE_RESOURCE_TABLE    0x24
#define WOWRES_MAX_ALIGN_SHIFT      16
#define WOWRES_TYPEINFO_SIZE        8
#define WOWRES_TYPEINFO_COUNT       2
#define WOWRES_NAMEINFO_SIZE        12
#define WOWRES_NAMEINFO_LENGTH      2
#define WOWRES_NAMEINFO_ID          6
#define WOWRES_INTEGER_ID           0x8000  /* The high bit: an integer, not a name offset */
#define WOWRES_ID_MASK              0x7FFF
#define WOWRES_LOWER_TO_UPPER       32      /* 'a' - 'A' */
#define WOWRES_MENU_HEADER_SIZE     4       /* Version, headerSize */
#define WOWRES_MENU_HEADER_EXTRA    2       /* headerSize: the extra header bytes */
#define WOWRES_MENU_TEXT_MAX        128
#define WOWRES_MENU_MAX_DEPTH       8

/* Read the application's file once. Returns 1 if there is an image to search.
 * s92 (#306): ONCE PER FILE, not once per process. With a run queue there is more
 * than one Win16 program, and WinHelp's menu is in WINHELP.EXE, not in the program
 * on the command line; a few images are kept and the current one is selected.
 */
#define WOWRES_CACHE                6

/* THE ACCELERATOR TABLE, AND WHY IT IS READ FROM THE FILE (Importance = 3):
 * `LoadAccelerators` is USER's own 16-bit code: it loads the resource itself
 * and all this host is asked for is PERMISSION (NotifyWow, wKind == 3), so the
 * hAccel a guest later hands to TranslateAccelerator is a Win16 global handle
 * to data we never saw. Rather than chase that handle, the table is read out
 * of the program's own file -- the same source, and the same route the menu
 * already takes.
 *
 * [CAUTION]: THE ASSUMPTION IS STATED: this takes the FIRST accelerator resource in the
 * module, because the hAccel cannot be mapped back to a resource id. Every
 * program measured has exactly one (WINMINE has ACCELERATOR 501 and nothing
 * else), and the log says which id was used so a program with two is visible
 * rather than silently half-working.
 * A Win16 ACCELTABLE entry is FIVE bytes, and the last has 0x80 set:
 *   BYTE fFlags   0x01 VIRTKEY  0x02 NOINVERT  0x04 SHIFT
 *                 0x08 CONTROL  0x10 ALT       0x80 LAST
 *   WORD wEvent   the key (a virtual key when VIRTKEY, else a character)
 *   WORD wId      the command id posted as WM_COMMAND's wParam
 */
#define WOWRES_ACCEL_VIRTKEY        0x01
#define WOWRES_ACCEL_SHIFT          0x04
#define WOWRES_ACCEL_CONTROL        0x08
#define WOWRES_ACCEL_ALT            0x10
#define WOWRES_ACCEL_LAST           0x80
#define WOWRES_MAX_ACCEL            64
#define WOWRES_ACCEL_ENTRY_SIZE     5
#define WOWRES_ACCEL_FIELD_KEY      1
#define WOWRES_ACCEL_FIELD_ID       3

/* THE APPLICATION'S OWN ICON (Importance = 2):
 * A GROUP_ICON resource is a directory: {WORD reserved, WORD type, WORD count}
 * then `count` 14-byte entries {BYTE w, h, colours, reserved; WORD planes, bits;
 * DWORD bytes; WORD id}, each naming an ICON resource by id.
 *
 * [INFO]: CONFIRMED AGAINST NOTEPAD BEFORE THIS WAS WRITTEN. Its GROUP_ICON #1 decodes
 * to type=1, count=2, and the two entries are 32x32 1bpp/304 bytes id=1 and
 * 32x32 4bpp/744 bytes id=2 -- and the resource table independently lists ICON 1
 * at 304 bytes and ICON 2 at 752 (744 rounded up to the alignment unit). A wrong
 * layout does not produce byte counts that match a different table.
 *
 * The ICON resource itself is a DIB -- header, palette, XOR bits, AND mask -- and
 * `CreateIconFromResourceEx` takes exactly that, so the OS does the decoding. The
 * `0x00030000` is the icon-resource version that API is documented to want; it is
 * a Win32 contract, not a claim about Win16.
 */
#define WOWRES_RT_ICON              3
#define WOWRES_RT_GROUP_ICON        14

/* A GROUP_ICON / GROUP_CURSOR directory, and the version CreateIconFromResourceEx wants. */
#define WOWRES_GROUP_HEADER_SIZE    6
#define WOWRES_GROUP_TYPE           2
#define WOWRES_GROUP_COUNT          4
#define WOWRES_GROUP_TYPE_ICON      1
#define WOWRES_GROUP_TYPE_CURSOR    2
#define WOWRES_GROUP_ENTRY_SIZE     14
#define WOWRES_GROUP_ENTRY_BITS     6
#define WOWRES_GROUP_ENTRY_ID       12
#define WOWRES_ICON_VERSION         0x00030000

/* AND THE SAME DIRECTORY SHAPE FOR CURSORS (Importance = 1):
 * `RT_GROUP_CURSOR` (12) indexes `RT_CURSOR` (1) exactly as GROUP_ICON indexes
 * ICON, and `CreateIconFromResourceEx` takes a cursor resource with `fIcon =
 * FALSE` -- the 4-byte hotspot at the front of the resource is part of what it
 * expects, so nothing has to be parsed here either.
 *
 * [INFO]: MS Paint ships SEVEN of them and every one is NAMED: "FLOOD", "CROSSH",
 * "PICK", "TEXT", "SIDEAROW", "DUMMY", "XDUMMY" -- read out of its resource
 * table. A paint program whose pointer never changes shape is using none of
 * them. [CAUTION] A cursor group's entries record the hotspot where an icon group
 * records planes/bits, so the "richest depth wins" rule is NOT reused: a
 * cursor group in practice holds one entry, and the first is taken.
 */
#define WOWRES_RT_CURSOR            1
#define WOWRES_RT_GROUP_CURSOR      12

typedef struct _WOWRES_CACHE_ENTRY
{
    CHAR Path[WOWRES_PATH_MAX];
    PBYTE Image;
    DWORD Length;
} WOWRES_CACHE_ENTRY;
typedef struct _WOWRES_ACCEL
{
    BYTE Flags;
    WORD Key;
    WORD Id;
} WOWRES_ACCEL, *PWOWRES_ACCEL;

/* Defined in wowres.c (#335). */
INT WowResOpen(PCSTR path);
INT WowResAccelFirst(PWOWRES_ACCEL output, INT capacity, PWORD resourceId);
HMENU WowResMenu(WORD id, PINT items);
HMENU WowResMenuByName(PCSTR name, PINT items);
HICON WowResIconNamed(PCSTR name, PINT picked, INT width, INT height);
HICON WowResIcon(WORD groupId, PINT picked, INT width, INT height);
HCURSOR WowResCursorNamed(PCSTR name);
HCURSOR WowResCursor(WORD groupId);

#endif /* NTVDMEX_WOWRES_H */
