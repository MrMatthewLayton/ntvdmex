/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * The bookkeeping behind the rest of the VDD service API (#11): memory hooks, IRQ line
 * reservations, user hooks and the registry's VDD list.
 *
 * WHY THIS EXISTS. A Microsoft-ABI VDD imports these from NTVDM.EXE by name; the DDK's
 * vddsvc.h is the contract (VDDInstallMemoryHook / VDDDeInstallMemoryHook,
 * VDDReserveIrqLine / VDDReleaseIrqLine, VDDInstallUserHook / VDDDeInstallUserHook, and
 * the HKLM ...\Control\VirtualDeviceDrivers "VDD" REG_MULTI_SZ list NTVDM loads at
 * start-up). What each call has to REMEMBER -- which VDD owns which range, line or set of
 * callbacks -- lives here, as plain tables with no Windows calls in them, so every rule
 * is exercised off-VM by tests/unit/vddsvc_test.c. The host half (src/host/host_vddsvc.c)
 * does what the tables decide.
 *
 * Pure C, no <windows.h>.
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#ifndef NTVDMEX_VDD_SVC_H
#define NTVDMEX_VDD_SVC_H

#include "../ntvdmex_types.h"

#define VDD_SVC_MEMORY_HOOKS_MAX    16      /* Ranges hooked at once, all VDDs together */
#define VDD_SVC_USER_HOOKS_MAX      8       /* VDDs with user hooks at once */
#define VDD_SVC_IRQ_LINES           16      /* Both PICs */
#define VDD_SVC_NO_IRQ              0xFFFF  /* VDDReserveIrqLine's failure; as an argument, "any free line" */
#define VDD_SVC_OK                  1
#define VDD_SVC_FAILED              0

/* A memory hook: a VDD's handler for [Start, Start + Count). The handler is opaque here;
 * the host calls it (vddsvc.h: PVDD_MEMORY_HANDLER, VOID (FaultAddress, RWMode)).
 */
typedef struct _VDD_SVC_MEMORY_HOOK
{
    PVOID  Owner;           /* The VDD's handle; NULL = a free slot */
    DWORD  Start;           /* Guest linear */
    DWORD  Count;           /* Bytes */
    PVOID  Handler;
} VDD_SVC_MEMORY_HOOK, *PVDD_SVC_MEMORY_HOOK;

typedef struct _VDD_SVC_MEMORY_HOOKS
{
    VDD_SVC_MEMORY_HOOK Entries[VDD_SVC_MEMORY_HOOKS_MAX];
} VDD_SVC_MEMORY_HOOKS, *PVDD_SVC_MEMORY_HOOKS;

typedef const VDD_SVC_MEMORY_HOOKS *PCVDD_SVC_MEMORY_HOOKS;

/* USER HOOKS: per VDD, the four callbacks of VDDInstallUserHook (create, terminate,
 * block, resume). Any may be NULL. One set per VDD: installing again replaces it.
 */
typedef struct _VDD_SVC_USER_HOOK
{
    PVOID Owner;            /* NULL = a free slot */
    PVOID Create;           /* VOID (USHORT DosPDB) */
    PVOID Terminate;        /* VOID (USHORT DosPDB) */
    PVOID Block;            /* VOID (VOID) */
    PVOID Resume;           /* VOID (VOID) */
} VDD_SVC_USER_HOOK, *PVDD_SVC_USER_HOOK;

typedef struct _VDD_SVC_USER_HOOKS
{
    VDD_SVC_USER_HOOK Entries[VDD_SVC_USER_HOOKS_MAX];
} VDD_SVC_USER_HOOKS, *PVDD_SVC_USER_HOOKS;

/* Add a hook. Fails on an empty range, a range that wraps, a full table, or any overlap
 * with a range already hooked (by this VDD or another: one address, one handler).
 */
INT VddSvcMemoryHookAdd(
    _Inout_ PVDD_SVC_MEMORY_HOOKS hooks,
    _In_ PVOID owner,
    _In_ DWORD start,
    _In_ DWORD count,
    _In_ PVOID handler);

/* Remove the hook `owner` installed with exactly this start and count. */
INT VddSvcMemoryHookRemove(
    _Inout_ PVDD_SVC_MEMORY_HOOKS hooks,
    _In_ PVOID owner,
    _In_ DWORD start,
    _In_ DWORD count);

/* Remove every hook `owner` holds (its module is going away). Returns how many. */
INT VddSvcMemoryHookRemoveOwner(_Inout_ PVDD_SVC_MEMORY_HOOKS hooks, _In_ PVOID owner);

/* The hook covering `address`, or NULL. */
PVDD_SVC_MEMORY_HOOK VddSvcMemoryHookFind(_In_ PVDD_SVC_MEMORY_HOOKS hooks, _In_ DWORD address);

/* IRQ LINES. `reserved` is a bit per line (bit n = IRQ n); the host seeds it with the
 * lines its own devices raise. VddSvcIrqReserve takes `line` if it is free, or with
 * VDD_SVC_NO_IRQ the first free line in the order a PC hands lines out to add-in cards
 * (vddsvc.h: "if IrqLine is -1, the first available line is reserved"); it answers the
 * line, or VDD_SVC_NO_IRQ. Line 2 (the cascade) is never handed out.
 */
WORD VddSvcIrqReserve(_Inout_ WORD *reserved, _In_ WORD line);
INT  VddSvcIrqRelease(_Inout_ WORD *reserved, _In_ WORD line);

INT VddSvcUserHookAdd(
    _Inout_ PVDD_SVC_USER_HOOKS hooks,
    _In_ PVOID owner,
    _In_opt_ PVOID create,
    _In_opt_ PVOID terminate,
    _In_opt_ PVOID block,
    _In_opt_ PVOID resume);
INT VddSvcUserHookRemove(_Inout_ PVDD_SVC_USER_HOOKS hooks, _In_ PVOID owner);

/* THE REGISTRY'S VDD LIST: a REG_MULTI_SZ, NUL-separated names ending in an empty one.
 * `*cursor` walks it; each call copies the next name (at most `capacity - 1` characters,
 * a longer one is skipped whole) and answers 1, or 0 at the end. `end` is one past the
 * last byte the registry returned, so a list missing its final NULs still stops.
 */
INT VddSvcMultiStringNext(
    _Inout_ PCSTR *cursor,
    _In_ PCSTR end,
    _Out_writes_(capacity) PSTR name,
    _In_ INT capacity);

#endif /* NTVDMEX_VDD_SVC_H */
