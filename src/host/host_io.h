/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * Port I/O: the trap dispatcher, the fast paths, and the VDD plug-in surface (third-party VDDs and the ISV I/O hooks).
 *
 * Declarations only (#335): defined in host_io.c.
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#ifndef NTVDMEX_HOST_IO_H
#define NTVDMEX_HOST_IO_H
#include "host_state.h"

BYTE NetSubmit(PVOID context, NETBIOS_REQUEST *request);
extern WORD g_IoLastPort;
VOID VddLoadThirdParty(VOID);
extern UINT32 g_DmaPollInAsync;
extern UINT32 g_DmaPollMainline;
VOID IoHotNote(WORD port, DWORD cs, DWORD ip);
INT HostTryIo(volatile BYTE *tib, VDD_BUS *bus);
INT HostTryIoRetro(volatile BYTE *tib, VDD_BUS *bus);
INT HostTryIoString(volatile BYTE *tib, VDD_BUS *bus);
extern DWORD g_DmaPollEip[DMAPOLL_MAX];
extern DWORD g_DmaPollHits[DMAPOLL_MAX];
extern UINT g_DmaPollCount;
extern UINT g_DmaPollOverflow;
extern DWORD g_PollStack[POLLSTK_MAX];
extern DWORD g_PollStackHits[POLLSTK_MAX];
extern DWORD g_PollGap[10];
extern DWORD g_PollGapMaximumMicroseconds;
extern UINT g_PollStackCount;
extern UINT g_PollStackOverflow;
INT HostTryIoPm(volatile BYTE *tib, VDD_BUS *bus);
VOID IsvIoIn(PVOID self, WORD port, BYTE width, UINT32 *value);
VOID IsvIoOut(PVOID self, WORD port, BYTE width, UINT32 value);
VOID IsvBop(volatile BYTE *tib, DWORD subfunction, PSTR *logCursor);

#endif
