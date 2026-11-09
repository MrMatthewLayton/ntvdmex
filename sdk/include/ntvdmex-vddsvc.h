/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * The Microsoft-ABI VDD service calls NTVDMEX's NTVDM.EXE provides beyond the I/O hooks
 *   and register accessors: memory hooks, memory, DMA, IRQ lines, user hooks (#11).
 *
 * WHY THIS EXISTS. A VDD written for NTVDM is built against the NT DDK's nt_vdd.h and
 * vddsvc.h and links NTVDM.EXE's exports by name. Those headers are Microsoft's and are
 * not shipped here; this one declares the same calls -- names, argument lists, STDCALL --
 * so a VDD can be built without the DDK, against sdk/lib/ntvdm.def. A VDD built with the
 * DDK needs none of it: the exports are the same.
 *
 * What NTVDMEX does with each call, and where it differs from NTVDM, is in
 * docs/sdk/vdd-sdk.md ("The Microsoft API").
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#ifndef NTVDMEX_VDDSVC_H
#define NTVDMEX_VDDSVC_H

#include <windows.h>

#define NTVDMEX_VDDSVC_SDK_VERSION  2u      /* Docs/sdk/vdd-sdk.md "SDK versions" */

/* VDDReserveIrqLine: as an argument "any free line", as the answer "none". */
#define NTVDMEX_VDD_ANY_IRQ         0xFFFF

#define NTVDMEX_VDD_DMA_ADDRESS     0x01    /* VDDSetDMA's flags: which fields to write */
#define NTVDMEX_VDD_DMA_COUNT       0x02
#define NTVDMEX_VDD_DMA_PAGE        0x04
#define NTVDMEX_VDD_DMA_STATUS      0x08

/* A memory hook's handler: the address the guest touched, and 0 = read / 1 = write. */
typedef VOID (WINAPI *PNTVDMEX_VDD_MEMORY_HANDLER)(PVOID faultAddress, ULONG readWriteMode);

/* A channel as VDDQueryDMA reports it and VDDSetDMA writes it. */
typedef struct _NTVDMEX_VDD_DMA_INFO
{
    WORD Address;           /* Current address (words on channels 5-7) */
    WORD Count;             /* Current count, transfers - 1 */
    WORD Page;
    BYTE Status;            /* Bit (line) = terminal count, bit (line + 4) = request */
    BYTE Mode;              /* The 8237 mode byte */
    BYTE Mask;              /* 1 = masked */
} NTVDMEX_VDD_DMA_INFO, *PNTVDMEX_VDD_DMA_INFO;

typedef VOID (WINAPI *PNTVDMEX_VDD_USER_PDB_HOOK)(USHORT dosPdb);
typedef VOID (WINAPI *PNTVDMEX_VDD_USER_HOOK)(VOID);

BOOL  WINAPI VDDInstallMemoryHook(
    HANDLE vdd,
    PVOID start,
    DWORD count,
    PNTVDMEX_VDD_MEMORY_HANDLER handler);
BOOL  WINAPI VDDDeInstallMemoryHook(HANDLE vdd, PVOID start, DWORD count);
BOOL  WINAPI VDDAllocMem(HANDLE vdd, PVOID address, DWORD size);
BOOL  WINAPI VDDFreeMem(HANDLE vdd, PVOID address, DWORD size);
BOOL  WINAPI VDDIncludeMem(HANDLE vdd, PVOID address, DWORD size);
BOOL  WINAPI VDDExcludeMem(HANDLE vdd, PVOID address, DWORD size);
DWORD WINAPI VDDRequestDMA(HANDLE vdd, WORD channel, PVOID buffer, DWORD length);
BOOL  WINAPI VDDQueryDMA(HANDLE vdd, WORD channel, PNTVDMEX_VDD_DMA_INFO info);
BOOL  WINAPI VDDSetDMA(HANDLE vdd, WORD channel, WORD flags, PNTVDMEX_VDD_DMA_INFO info);
WORD  WINAPI VDDReserveIrqLine(HANDLE vdd, WORD line);
BOOL  WINAPI VDDReleaseIrqLine(HANDLE vdd, WORD line);
BOOL  WINAPI VDDInstallUserHook(
    HANDLE vdd,
    PNTVDMEX_VDD_USER_PDB_HOOK create,
    PNTVDMEX_VDD_USER_PDB_HOOK terminate,
    PNTVDMEX_VDD_USER_HOOK block,
    PNTVDMEX_VDD_USER_HOOK resume);
BOOL  WINAPI VDDDeInstallUserHook(HANDLE vdd);

#endif /* NTVDMEX_VDDSVC_H */
