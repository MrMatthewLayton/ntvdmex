/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * KEYBOARD.DRV's OWN ID SPACE.  GH #128, session 44.
 *
 * A SIXTH id space, and the smallest one yet: `tools/ne/neneeds.py` says
 * NOTEPAD.EXE imports exactly two things from KEYBOARD that reach the 32-bit
 * side, `5 ANSITOOEM` and `6 OEMTOANSI`, and both are character-set conversion
 * rather than anything to do with keys.
 *
 * WHY A TEXT EDITOR IMPORTS THEM, AND WHY IT MATTERS HERE:
 * A Windows 3.x program holds text in the ANSI (Windows) code page and a DOS
 * file holds it in the OEM one, so a Win16 editor converts on the way in and out.
 * That puts these two directly in the path this session could not finish: Notepad
 * opened, seeked and READ its file, and nothing appeared. An unimplemented call
 * here returns the harness sentinel and touches neither buffer -- so the
 * destination stays whatever it was, which for a freshly allocated block is
 * zeros, and a zero-length string is exactly what "nothing appeared" looks like.
 *
 * [CAUTION]: THAT IS A CANDIDATE, NOT A DIAGNOSIS. It is written down as the reason these
 * two went in first, so that if the file still does not appear the next reader
 * knows this was tried and can stop suspecting it.
 *
 * THE IDS, FROM THE FILE:
 * ordinal 5 ANSITOOEM -> entry table FIXED seg 1 offset 108 (0x6c), and the
 *   bytes there are `6a 08 68 00 00 68 05 00 9a` -- 8 argument bytes, id 0x05.
 * ordinal 6 OEMTOANSI -> offset 121 (0x79), `6a 08 68 00 00 68 06 00 9a`.
 * A stub is 13 bytes, so the return addresses are 0x0079 and 0x0086. As with
 * SHELL and COMMDLG the ids are the export ordinals; as always that is checked
 * per module and never assumed (krnl386's are nothing like its ordinals).
 *
 * THE ARGUMENTS:
 * 8 bytes is two far pointers, and the block is reversed as always, so the
 * DESTINATION -- the last parameter and therefore the last push -- is at +0:
 *   AnsiToOem(lpAnsiStr, lpOemStr)   ->  +0 lpOemStr   +4 lpAnsiStr
 *   OemToAnsi(lpOemStr, lpAnsiStr)   ->  +0 lpAnsiStr  +4 lpOemStr
 *
 * [CAUTION]: Getting that round the wrong way would convert in place over the source and
 * leave the destination untouched -- which looks exactly like doing nothing,
 * i.e. like the bug this is here to fix. The direction is taken from the
 * parameter order plus the reversal rule, both of which this host has
 * confirmed on every other block it reads.
 *
 * [CAUTION]: NUL-TERMINATED, NO COUNT. These are the counted versions' siblings
 * (`AnsiToOemBuff` takes a length and is a different ordinal), so the length
 * is the source string's and the destination must be at least as large. That
 * is the caller's contract in Win16 and it is unchanged here; Win32's
 * CharToOemA/OemToCharA have exactly the same one.
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#ifndef NTVDMEX_WOWKBD_H
#define NTVDMEX_WOWKBD_H

#include "wow32.h"

/* CHARMAP's two, and they live HERE and not in wowuser.h (Importance = 2):
 * VkKeyScan and MapVirtualKey are KEYBOARD.DRV exports in Win16, not USER's,
 * and every thunk module has an id space ALL ITS OWN -- 0x81 is VkKeyScan here
 * and something else entirely in USER. Implementing them in the wrong file
 * compiles, dispatches from the wrong table, and answers a question nobody
 * asked. (Gate on the BOP's own CS.)
 */
/* GetKeyNameText(lParam, lpszBuffer, nMaxCount) -- 4+4+2 = 10, reversed. */
#define WOWKBD_GETKEYNAMETEXT               0x0085
#define WOWKBD_GETKEYNAMETEXT_ARG_COUNT     0
#define WOWKBD_GETKEYNAMETEXT_ARG_BUFFER    2   /* Far */
#define WOWKBD_GETKEYNAMETEXT_ARG_LPARAM    6   /* DWORD */

#define WOWKBD_VKKEYSCAN                    0x0081
#define WOWKBD_MAPVIRTUALKEY                0x0083
#define WOWKBD_VKKEYSCAN_ARG_CHARACTER      0
#define WOWKBD_MAPVIRTUALKEY_ARG_TYPE       0
#define WOWKBD_MAPVIRTUALKEY_ARG_CODE       2

/* s90 (#297): GetKBCodePage() -- no arguments. Win32's GetKBCodePage is the OEM
 * code page (GetOEMCP), which is what Win16's answered too: 437 on a US machine.
 */
#define WOWKBD_GETKBCODEPAGE                0x0084
#define WOWKBD_ANSITOOEM                    0x0005
#define WOWKBD_OEMTOANSI                    0x0006

#define WOWKBD_CONVERT_ARG_DESTINATION      0
#define WOWKBD_CONVERT_ARG_SOURCE           4
#define WOWKBD_KEY_NAME_MAX                 64
#define WOWKBD_CHARACTER_MASK               0xFF

/* Defined in wowkbd.c (#335). */
INT WowKeyboardCall(WOW32_FRAME *frame, PSTR note, INT noteCapacity);

#endif /* NTVDMEX_WOWKBD_H */
