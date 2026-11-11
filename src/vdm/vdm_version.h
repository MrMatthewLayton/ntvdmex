/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * Which Windows' VDM interface the host is talking to (s106). Separate from v86.h,
 * which needs <windows.h>, so the off-VM battery can test the decision on the Mac.
 *
 * Windows 7 (NT 6.1) differs from XP in four places the host touches, each seen on the
 * Win7-32 SP1 rig:
 *   - RegisterConsoleVDM takes nine arguments, not eleven (csrss.c);
 *   - VdmInitialize needs C0000-FFFFF committed beforehand (v86.c, VdmCommitUpperMemory);
 *   - VDM_TIB.Size is 0x678, not 0x674 (v86.c, VdmGetTib);
 *   - so the EMS frame has to be handed back out of that range after init (VdmMapEmsFrame).
 * Vista (6.0) is untested and keeps XP's behaviour.
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#ifndef NTVDMEX_VDM_VDM_VERSION_H
#define NTVDMEX_VDM_VDM_VERSION_H

#include "../ntvdmex_types.h"

#define VDM_NT61_MAJOR  6   /* Windows 7 */
#define VDM_NT61_MINOR  1

/* TRUE when Windows majorVersion.minorVersion has Windows 7's VDM interface. */
static inline BOOL VdmIsNt61OrLater(DWORD majorVersion, DWORD minorVersion)
{
    return majorVersion > VDM_NT61_MAJOR
        || (majorVersion == VDM_NT61_MAJOR && minorVersion >= VDM_NT61_MINOR);
}

#endif /* NTVDMEX_VDM_VDM_VERSION_H */
