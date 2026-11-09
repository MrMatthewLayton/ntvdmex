/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * The host half of the rest of the VDD service API: memory, DMA, IRQs, user hooks, registry.
 *
 * The function definitions of host_vddsvc.h, which keeps their declarations and doc comments (#11).
 *
 * [CAUTION]: MEMORY HOOKS SEE THE INTERPRETER'S ACCESSES, NOT THE CPU'S. NTVDM traps a hooked
 * range by leaving its pages not present and taking the fault. Here that does not work:
 * marking a page of the V86 window PAGE_NOACCESS FREEZES the guest on real hardware
 * (measured twice for A0000, see A000Protect in host_video.c) -- the exec thread never
 * comes back out of VdmStartExecution. So a hooked page is marked in the interpreter's
 * page map (HostPageMapHook), and every access the HOST INTERPRETER makes there comes here.
 * A guest running natively on the CPU reads and writes the page as plain memory. The SDK
 * documentation says so.
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#include "host_vddsvc.h"
#include "host_video.h"
#include "host_wow.h"
#include "log.h"
#include "main.h"
#include "../vdd/vdd_svc.h"

#define VDDSVC_VDM_TOP          0x110000u               /* The VDM's linear range: 1 MB + the HMA */
#define VDDSVC_RW_READ          0                       /* Vddsvc.h: the memory handler's RWMode */
#define VDDSVC_RW_WRITE         1
#define VDDSVC_MEMORY_ALLOC     SHIM_VDD_MEMORY_ALLOC   /* HostVddMemory's operation: the shim's */
#define VDDSVC_MEMORY_FREE      SHIM_VDD_MEMORY_FREE
#define VDDSVC_MEMORY_INCLUDE   SHIM_VDD_MEMORY_INCLUDE
#define VDDSVC_MEMORY_EXCLUDE   SHIM_VDD_MEMORY_EXCLUDE
#define VDDSVC_PAGES            (VDDSVC_VDM_TOP >> PAGE_SHIFT)
#define VDDSVC_HOST_IRQ_LINES       ((1u << 0) | (1u << 1) | (1u << 2) | (1u << 3) | (1u << 4) | (1u << 5) | (1u << 6) | (1u << 7) \
                                     | (1u << 8) | (1u << 10) | (1u << 11) | (1u << 12) | (1u << 13) | (1u << 14) | (1u << 15))
#define VDDSVC_REGISTRY_KEY     "SYSTEM\\CurrentControlSet\\Control\\VirtualDeviceDrivers"
#define VDDSVC_REGISTRY_VALUE   "VDD"
#define VDDSVC_REGISTRY_BYTES   4096
#define VDDSVC_OPT_OUT_FLAG     CFG_("noregvdd.flag")
#define VDDSVC_PAGE_BYTES       (1u << PAGE_SHIFT)
#define VDDSVC_BUS_OK           0   /* vdd_bus.c: a claim taken */
#define VDDSVC_BUS_CLAIMED      1   /* vdd_bus.c: a VDD answered the access */

typedef VOID (WINAPI *PVDDSVC_MEMORY_HANDLER)(PVOID faultAddress, ULONG readWriteMode);

typedef VOID (WINAPI *PVDDSVC_USER_PSP_HOOK)(USHORT psp);

typedef VOID (WINAPI *PVDDSVC_USER_HOOK)(VOID);

static VDD_SVC_MEMORY_HOOKS g_VddMemoryHooks;
static VDD_SVC_USER_HOOKS   g_VddUserHooks;
/* The lines the host's own devices raise (PIT, keyboard, cascade, COM, SB, FDC, LPT, RTC,
 * MMSYSTEM's 10, GUS, PS/2, FPU, IDE). Line 9 is the one a VDD can have for the asking.
 */
static WORD g_VddIrqReserved = VDDSVC_HOST_IRQ_LINES;
static BYTE g_VddPagesCommitted[VDDSVC_PAGES];                    /* pages VDDAllocMem committed */
static DWORD g_VddMemoryHookCalls;

static VOID VddSvcLog(PCSTR what, DWORD first, DWORD second)
{
    CHAR buffer[160];
    CHAR *cursor = buffer;

    cursor = LogPut(cursor, "VDDSVC: "); cursor = LogPut(cursor, what);
    cursor = LogPut(cursor, " 0x"); cursor = LogHex(cursor, first);
    cursor = LogPut(cursor, " 0x"); cursor = LogHex(cursor, second);
    cursor = LogPut(cursor, "\r\n");
    LogAppend(LOG_PATH, buffer, cursor);
}

static INT VddSvcInVdm(DWORD start, DWORD size)
{
    return size && start < VDDSVC_VDM_TOP && size <= VDDSVC_VDM_TOP - start;
}

BOOL HostVddInstallMemoryHook(HANDLE vddHandle, PVOID start, DWORD count, PVOID handler)
{
    DWORD linear = (DWORD)(ULONG_PTR)start;

    if (!VddSvcInVdm(linear, count) || !VddSvcMemoryHookAdd(&g_VddMemoryHooks, vddHandle, linear, count, handler))
    {
        VddSvcLog("VDDInstallMemoryHook REFUSED", linear, count);
        return FALSE;
    }

    HostPageMapHook(linear, count);
    VddSvcLog("VDDInstallMemoryHook", linear, count);
    return TRUE;
}

BOOL HostVddRemoveMemoryHook(HANDLE vddHandle, PVOID start, DWORD count)
{
    DWORD linear = (DWORD)(ULONG_PTR)start;

    if (!VddSvcMemoryHookRemove(&g_VddMemoryHooks, vddHandle, linear, count))
        return FALSE;

    HostPageMapReset(linear, count);
    VddSvcLog("VDDDeInstallMemoryHook", linear, count);
    return TRUE;
}

/* A hooked page's access: our ABI's claimed range first (the bus answers it), then a
 * Microsoft-ABI hook, whose handler is told the address and the direction -- and is
 * expected, as under NTVDM, to make the memory present (VDDAllocMem) if it wants the
 * access to land. If it does, the access goes to the page; if not, a read floats high
 * and a write is dropped, as on an empty bus.
 */
static INT HostVddMemoryAccess(UINT32 linear, INT isWrite, BYTE *value)
{
    PVDD_SVC_MEMORY_HOOK hook;

    if (isWrite ? VddBusMemoryWrite(&g_Bus, linear, *value) == VDDSVC_BUS_CLAIMED
                : VddBusMemoryRead(&g_Bus, linear, value) == VDDSVC_BUS_CLAIMED)
        return 1;

    hook = VddSvcMemoryHookFind(&g_VddMemoryHooks, linear);

    if (!hook)
        return 0;

    ++g_VddMemoryHookCalls;
    ((PVDDSVC_MEMORY_HANDLER)hook->Handler)((PVOID)(ULONG_PTR)linear, isWrite ? VDDSVC_RW_WRITE : VDDSVC_RW_READ);

    if (g_VddPagesCommitted[linear >> PAGE_SHIFT] && HostWritable((PVOID)(ULONG_PTR)linear, 1))
    {
        if (isWrite)
            *(volatile BYTE *)(ULONG_PTR)linear = *value;
        else
            *value = *(volatile BYTE *)(ULONG_PTR)linear;
    }
    else if (!isWrite)
        *value = VDD_UNCLAIMED_READ_BYTE;

    return 1;
}

INT HostVddMemoryRead(UINT32 linear, BYTE *value)
{
    return HostVddMemoryAccess(linear, 0, value);
}

INT HostVddMemoryWrite(UINT32 linear, BYTE value)
{
    return HostVddMemoryAccess(linear, 1, &value);
}

INT HostVddClaimMemory(
    PVDD_BUS bus,
    UINT32 base,
    UINT32 size,
    PVDD_MEMORY_READ_ROUTINE readRoutine,
    PVDD_MEMORY_WRITE_ROUTINE writeRoutine,
    PVOID context)
{
    INT status = VddClaimMemory(bus, base, size, readRoutine, writeRoutine, context);

    if (status == VDDSVC_BUS_OK && VddSvcInVdm(base, size))
        HostPageMapHook(base, size);

    return status;
}

DWORD HostVddRequestDma(HANDLE vddHandle, WORD channel, PVOID buffer, DWORD length)
{
    DWORD moved;

    (VOID)vddHandle;

    if (channel >= DMA_CHANNELS)
        return 0;

    HOST_LOCK();
    moved = VddDmaRequest(&g_Dma, (BYTE)channel, (BYTE *)buffer, length);
    HOST_UNLOCK();
    return moved;
}

BOOL HostVddQueryDma(HANDLE vddHandle, WORD channel, PVOID info)
{
    INT isOk;

    (VOID)vddHandle;

    if (!info || channel >= DMA_CHANNELS)
        return FALSE;

    HOST_LOCK();
    isOk = VddDmaQueryInfo(&g_Dma, (BYTE)channel, (PDMA_VDD_INFO)info);
    HOST_UNLOCK();
    return isOk ? TRUE : FALSE;
}

BOOL HostVddSetDma(HANDLE vddHandle, WORD channel, WORD flags, PVOID info)
{
    INT isOk;

    (VOID)vddHandle;

    if (!info || channel >= DMA_CHANNELS)
        return FALSE;

    HOST_LOCK();
    isOk = VddDmaSetInfo(&g_Dma, (BYTE)channel, flags, (PCDMA_VDD_INFO)info);
    HOST_UNLOCK();
    return isOk ? TRUE : FALSE;
}

WORD HostVddReserveIrq(HANDLE vddHandle, WORD line)
{
    WORD granted = VddSvcIrqReserve(&g_VddIrqReserved, line);

    (VOID)vddHandle;
    VddSvcLog("VDDReserveIrqLine asked/granted", line, granted);
    return granted;
}

BOOL HostVddReleaseIrq(HANDLE vddHandle, WORD line)
{
    (VOID)vddHandle;

    if (line < VDD_SVC_IRQ_LINES && (VDDSVC_HOST_IRQ_LINES & (1u << line)))
        return FALSE;                                                  /* never the host's own */

    return VddSvcIrqRelease(&g_VddIrqReserved, line) ? TRUE : FALSE;
}

/* VDDAllocMem / VDDFreeMem / VDDIncludeMem / VDDExcludeMem, by operation.
 * - Alloc commits every page of the range that is not committed yet, inside the VDM's
 *   linear range only, and remembers which it committed; Free decommits only those.
 * - Include / Exclude tell NTVDM's memory manager a range is (not) usable as upper
 *   memory. This host provides no UMBs (the Umb setting is stored, not honoured), so
 *   there is nothing for them to change: the range is checked and the call logged.
 */
BOOL HostVddMemory(INT operation, HANDLE vddHandle, PVOID address, DWORD size)
{
    DWORD linear = (DWORD)(ULONG_PTR)address;
    DWORD page;
    DWORD firstPage;
    DWORD lastPage;

    (VOID)vddHandle;

    if (!VddSvcInVdm(linear, size))
        return FALSE;

    firstPage = linear >> PAGE_SHIFT;
    lastPage = (linear + size - 1) >> PAGE_SHIFT;

    if (operation == VDDSVC_MEMORY_INCLUDE || operation == VDDSVC_MEMORY_EXCLUDE)
    {
        VddSvcLog(operation == VDDSVC_MEMORY_INCLUDE ? "VDDIncludeMem (no UMB provider: recorded)" : "VDDExcludeMem (no UMB provider: recorded)",
                  linear, size);
        return TRUE;
    }

    if (operation == VDDSVC_MEMORY_FREE)
    {
        for (page = firstPage; page <= lastPage; ++page)
            if (!g_VddPagesCommitted[page])
                return FALSE;                                          /* not ours to free */

        for (page = firstPage; page <= lastPage; ++page)
        {
            VirtualFree((PVOID)(ULONG_PTR)(page << PAGE_SHIFT), VDDSVC_PAGE_BYTES, MEM_DECOMMIT);
            g_VddPagesCommitted[page] = 0;
        }

        HostPageMapReset(firstPage << PAGE_SHIFT, (lastPage - firstPage + 1) << PAGE_SHIFT);

        for (page = firstPage; page <= lastPage; ++page)              /* a hooked page is hooked again */
            if (VddSvcMemoryHookFind(&g_VddMemoryHooks, page << PAGE_SHIFT))
                HostPageMapHook(page << PAGE_SHIFT, VDDSVC_PAGE_BYTES);

        VddSvcLog("VDDFreeMem", linear, size);
        return TRUE;
    }

    if (operation != VDDSVC_MEMORY_ALLOC)
        return FALSE;

    for (page = firstPage; page <= lastPage; ++page)
    {
        MEMORY_BASIC_INFORMATION memoryInfo;
        PVOID pageAddress = (PVOID)(ULONG_PTR)(page << PAGE_SHIFT);

        if (VirtualQuery(pageAddress, &memoryInfo, sizeof memoryInfo) != sizeof memoryInfo)
            return FALSE;

        if (memoryInfo.State == MEM_COMMIT)
            continue;                                                  /* already there */

        if (!VirtualAlloc(pageAddress, VDDSVC_PAGE_BYTES, memoryInfo.State == MEM_FREE ? (MEM_RESERVE | MEM_COMMIT) : MEM_COMMIT,
                          PAGE_EXECUTE_READWRITE))
        {
            VddSvcLog("VDDAllocMem FAILED at", page << PAGE_SHIFT, GetLastError());
            return FALSE;
        }

        g_VddPagesCommitted[page] = 1;
    }

    HostPageMapReset(firstPage << PAGE_SHIFT, (lastPage - firstPage + 1) << PAGE_SHIFT);
    VddSvcLog("VDDAllocMem", linear, size);
    return TRUE;
}

BOOL HostVddInstallUserHook(
    HANDLE vddHandle,
    PVOID create,
    PVOID terminate,
    PVOID block,
    PVOID resume)
{
    return VddSvcUserHookAdd(&g_VddUserHooks, vddHandle, create, terminate, block, resume) ? TRUE : FALSE;
}

BOOL HostVddRemoveUserHook(HANDLE vddHandle)
{
    return VddSvcUserHookRemove(&g_VddUserHooks, vddHandle) ? TRUE : FALSE;
}

VOID HostVddUserCreate(WORD psp)
{
    INT index;

    for (index = 0; index < VDD_SVC_USER_HOOKS_MAX; ++index)
        if (g_VddUserHooks.Entries[index].Owner && g_VddUserHooks.Entries[index].Create)
            ((PVDDSVC_USER_PSP_HOOK)g_VddUserHooks.Entries[index].Create)(psp);
}

VOID HostVddUserTerminate(WORD psp)
{
    INT index;

    for (index = 0; index < VDD_SVC_USER_HOOKS_MAX; ++index)
        if (g_VddUserHooks.Entries[index].Owner && g_VddUserHooks.Entries[index].Terminate)
            ((PVDDSVC_USER_PSP_HOOK)g_VddUserHooks.Entries[index].Terminate)(psp);
}

VOID HostVddUserBlock(VOID)
{
    INT index;

    for (index = 0; index < VDD_SVC_USER_HOOKS_MAX; ++index)
        if (g_VddUserHooks.Entries[index].Owner && g_VddUserHooks.Entries[index].Block)
            ((PVDDSVC_USER_HOOK)g_VddUserHooks.Entries[index].Block)();
}

VOID HostVddUserResume(VOID)
{
    INT index;

    for (index = 0; index < VDD_SVC_USER_HOOKS_MAX; ++index)
        if (g_VddUserHooks.Entries[index].Owner && g_VddUserHooks.Entries[index].Resume)
            ((PVDDSVC_USER_HOOK)g_VddUserHooks.Entries[index].Resume)();
}

VOID HostVddModuleGone(HANDLE vddHandle)
{
    INT index;

    for (index = 0; index < VDD_SVC_MEMORY_HOOKS_MAX; ++index)
        if (g_VddMemoryHooks.Entries[index].Owner == vddHandle)
            HostPageMapReset(g_VddMemoryHooks.Entries[index].Start, g_VddMemoryHooks.Entries[index].Count);

    VddSvcMemoryHookRemoveOwner(&g_VddMemoryHooks, vddHandle);
    VddSvcUserHookRemove(&g_VddUserHooks, vddHandle);
}

/* THE REGISTRY'S VDDs. NTVDM loads every DLL named in
 * HKLM\SYSTEM\CurrentControlSet\Control\VirtualDeviceDrivers, value "VDD" (REG_MULTI_SZ),
 * when a VDM starts; a VDD sets itself up in its DllEntryPoint (DLL_PROCESS_ATTACH) --
 * installing its I/O hooks and the rest -- so loading it is the whole protocol. Our
 * NTVDM.EXE (bin\wowshim\) is loaded first so the VDD's imports resolve to it.
 *
 * [CAUTION]: a VDD that imports something this host does not export fails to LOAD, which is
 * logged and skipped. A VDD whose own initialisation crashes takes the host with it, as it
 * would take NTVDM -- cfg\noregvdd.flag turns the whole list off.
 */
PSTR HostVddRegistryLoad(PSTR cursor)
{
    static CHAR list[VDDSVC_REGISTRY_BYTES];
    HKEY key;
    DWORD type = 0;
    DWORD bytes = sizeof list - 2;
    PCSTR walk;
    CHAR name[MAX_PATH];
    INT loaded = 0;

    if (GetFileAttributesA(VDDSVC_OPT_OUT_FLAG) != INVALID_FILE_ATTRIBUTES)
    {
        cursor = LogPut(cursor, "VDDSVC: registry VDDs skipped (cfg\\noregvdd.flag)\r\n");
        return cursor;
    }

    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, VDDSVC_REGISTRY_KEY, 0, KEY_READ, &key) != ERROR_SUCCESS)
        return cursor;

    if (RegQueryValueExA(key, VDDSVC_REGISTRY_VALUE, NULL, &type, (BYTE *)list, &bytes) != ERROR_SUCCESS
        || (type != REG_MULTI_SZ && type != REG_SZ))
        bytes = 0;

    RegCloseKey(key);
    walk = list;

    while (VddSvcMultiStringNext(&walk, list + bytes, name, (INT)sizeof name))
    {
        HMODULE module;

        if (!loaded++)
            WowShimsLoad();                                            /* NTVDM.EXE before the VDD */

        module = LoadLibraryA(name);
        cursor = LogPut(cursor, "VDDSVC: registry VDD ["); cursor = LogPut(cursor, name);

        if (module)
        {
            cursor = LogPut(cursor, "] loaded at 0x"); cursor = LogHex(cursor, (DWORD)(ULONG_PTR)module);
        }
        else
        {
            cursor = LogPut(cursor, "] NOT LOADED, err=0x"); cursor = LogHex(cursor, GetLastError());
        }

        cursor = LogPut(cursor, "\r\n");
    }

    return cursor;
}
