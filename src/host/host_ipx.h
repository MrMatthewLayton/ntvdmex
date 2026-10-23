/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * The host's IPX: Winsock AF_IPX behind vdd_ipx.c's backend, and the INT 2Fh/entry glue (#315).
 *
 * WHY THIS EXISTS. XP has IPX only when the NWLink IPX/SPX protocol is installed, and
 * stock NTVDM's vwipxspx.dll rides on it; without NWLink its INT 2Fh AX=7A00h answers
 * "not installed". Ours does the same thing the same way round: ws2_32.dll is loaded the
 * first time a program asks 7A00h, and if an AF_IPX socket cannot be made -- no NWLink --
 * the answer is AL=00h, not installed, and nothing else is touched.
 *
 * [CAUTION]: ws2_32 IS LOADED, NEVER IMPORTED. The host's import table is what XP must be
 * able to resolve at start-up; a static import of ws2_32 would put a networking DLL on
 * the start-up path of every DOS program for the sake of the few that ask for IPX.
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#ifndef NTVDMEX_HOST_IPX_H
#define NTVDMEX_HOST_IPX_H

#include "host_state.h"
#include "vdd_ipx.h"

extern IPX_STATE g_Ipx;

/* INT 2Fh AX=7A00h: 1 (and the entry) if this host has IPX, else 0 -- tried once. */
INT HostIpxInstallCheck(VOID);

/* The far-call entry's BOP: one IPX call on the guest's registers. */
VOID HostIpxEntry(volatile BYTE *tib);

/* Each BIOS tick: deliver what has arrived to the ECBs listening for it. */
VOID HostIpxTick(VOID);

#endif /* NTVDMEX_HOST_IPX_H */
