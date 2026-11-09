/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * Which drive and directory an FCB search (INT 21h AH=11h) lists: the PURE half. (#240)
 *
 * The FCB's drive byte picks the drive: 1 = A:, 2 = B:, and 0 = the CURRENT drive. The
 * directory is that drive's current directory, so the pattern handed to the host is
 * "X:*" (Win32 resolves "X:" through its per-drive current directory) or, for the
 * current drive, "*" -- but only when the current drive is one Win32 stands on. A drive
 * Win32 cannot stand on (an empty floppy bay, the image A: of #240) is held in
 * DOS_MACHINE::VirtualDrive, and "*" alone would list the PROCESS directory instead.
 *
 * [WARNING]: THAT IS HOW DIR LISTED bin\ FROM A:. XP's COMMAND.COM lists a directory with
 * the FCB search, drive byte 0 (s101 rig report, item 8). The handle search (AH=4Eh)
 * went through DosGuestPath and its VirtualDrive prefix; this one built "*" itself.
 * The rule here is DosGuestPath's: a relative name on a virtual current drive gets
 * that drive's letter in front.
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#ifndef NTVDMEX_DOS_FCBFIND_H
#define NTVDMEX_DOS_FCBFIND_H

#include "../ntvdmex_types.h"

#define DOS_FCB_SEARCH_PATTERN_MINIMUM  4   /* "X:*" and its NUL */

/* The 0-based drive the search lists: the FCB's drive byte, or the current drive. */
static inline INT DosFcbSearchDrive(_In_ BYTE fcbDrive, _In_ INT currentDrive)
{
    return fcbDrive ? (INT)fcbDrive - 1 : currentDrive;
}

/* The pattern for the search: "X:*", or "*" on a current drive Win32 stands on.
 * `virtualDrive` is DOS_MACHINE::VirtualDrive (-1 = none). 0 if `capacity` is too small.
 */
static inline INT DosFcbSearchPattern(
    _In_ BYTE fcbDrive,
    _In_ INT virtualDrive,
    _Out_writes_(capacity) PSTR pattern,
    _In_ INT capacity)
{
    INT length = 0;
    INT drive = fcbDrive ? (INT)fcbDrive - 1 : virtualDrive;

    if (capacity < DOS_FCB_SEARCH_PATTERN_MINIMUM)
        return 0;

    if (drive >= 0)
    {
        pattern[length++] = (CHAR)('A' + drive);
        pattern[length++] = ':';
    }

    pattern[length++] = '*';
    pattern[length] = 0;
    return 1;
}

#endif
