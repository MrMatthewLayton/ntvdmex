/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * Which form of RegisterConsoleVDM this Windows takes (s106). Separate from csrss.h,
 * which needs <windows.h>, so the off-VM battery can test the decision on the Mac.
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#ifndef NTVDMEX_VDM_CSRSS_VERSION_H
#define NTVDMEX_VDM_CSRSS_VERSION_H

#include "../ntvdmex_types.h"

/* The first Windows whose RegisterConsoleVDM takes nine arguments (see ntvdm.h):
 * NT 6.1, Windows 7. Vista (6.0) is untested and keeps XP's eleven.
 */
#define CSRSS_NINE_ARGUMENT_MAJOR   6
#define CSRSS_NINE_ARGUMENT_MINOR   1

/* TRUE when Windows majorVersion.minorVersion takes the nine-argument form. */
static inline BOOL CsrssUsesNineArgumentRegister(DWORD majorVersion, DWORD minorVersion)
{
    return majorVersion > CSRSS_NINE_ARGUMENT_MAJOR
        || (majorVersion == CSRSS_NINE_ARGUMENT_MAJOR && minorVersion >= CSRSS_NINE_ARGUMENT_MINOR);
}

#endif /* NTVDMEX_VDM_CSRSS_VERSION_H */
