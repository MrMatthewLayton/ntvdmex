/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * The host half of the rest of the VDD service API (#11): memory hooks and memory,
 *   DMA, IRQ reservations, user hooks, and the registry's VDD list.
 *
 * Declarations only: defined in host_vddsvc.c.
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#ifndef NTVDMEX_HOST_VDDSVC_H
#define NTVDMEX_HOST_VDDSVC_H

#include "host_state.h"

/* The shim's entries (NTVDMEX_SHIM_API version 4); see src/shim/wowshim.c. */
BOOL  HostVddInstallMemoryHook(HANDLE vddHandle, PVOID start, DWORD count, PVOID handler);
BOOL  HostVddRemoveMemoryHook(HANDLE vddHandle, PVOID start, DWORD count);
DWORD HostVddRequestDma(HANDLE vddHandle, WORD channel, PVOID buffer, DWORD length);
BOOL  HostVddQueryDma(HANDLE vddHandle, WORD channel, PVOID info);
BOOL  HostVddSetDma(HANDLE vddHandle, WORD channel, WORD flags, PVOID info);
WORD  HostVddReserveIrq(HANDLE vddHandle, WORD line);
BOOL  HostVddReleaseIrq(HANDLE vddHandle, WORD line);
BOOL  HostVddMemory(INT operation, HANDLE vddHandle, PVOID address, DWORD size);
BOOL  HostVddInstallUserHook(
    HANDLE vddHandle,
    PVOID create,
    PVOID terminate,
    PVOID block,
    PVOID resume);
BOOL  HostVddRemoveUserHook(HANDLE vddHandle);

/* The interpreter's slow path, for a page HostVddPageHooked() says is hooked: a claimed
 * range (our ABI's claim_mem) or a Microsoft-ABI memory hook answers the access. Returns
 * 1 if it was answered (a read's byte in *value), 0 to fall through to the plain page.
 */
INT HostVddMemoryRead(UINT32 linear, BYTE *value);
INT HostVddMemoryWrite(UINT32 linear, BYTE value);

/* Our own ABI's claim_mem, as plugins get it: the bus's claim, and its pages hooked. */
INT HostVddClaimMemory(
    PVDD_BUS bus,
    UINT32 base,
    UINT32 size,
    PVDD_MEMORY_READ_ROUTINE readRoutine,
    PVDD_MEMORY_WRITE_ROUTINE writeRoutine,
    PVOID context);

/* User hooks, called where NTVDM calls them: a program created and terminated (its PSP),
 * the VDM blocked and resumed (the background pause, #219).
 */
VOID HostVddUserCreate(WORD psp);
VOID HostVddUserTerminate(WORD psp);
VOID HostVddUserBlock(VOID);
VOID HostVddUserResume(VOID);

/* A Microsoft-ABI VDD's module is going away: drop its memory hooks and user hooks. */
VOID HostVddModuleGone(HANDLE vddHandle);

/* Load HKLM ...\Control\VirtualDeviceDrivers "VDD" (REG_MULTI_SZ), as NTVDM does at
 * start-up. Skipped when cfg\noregvdd.flag exists. Never fatal: each failure is logged.
 */
PSTR HostVddRegistryLoad(PSTR cursor);

#endif /* NTVDMEX_HOST_VDDSVC_H */
