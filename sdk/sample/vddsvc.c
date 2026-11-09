/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * A Microsoft-ABI VDD that uses the SDK 2 calls (#11): it reserves an IRQ line, hooks a
 *   range of upper memory and makes it present when the guest first touches it, follows
 *   DOS programs starting and ending, and moves DMA channel 1 when its port is written.
 *
 * Build: sdk/build-sample.sh (an import library from sdk/lib/ntvdm.def, then one
 * compiler line). A DOS program loads it with the third-party BOP (RegisterModule) or it
 * is listed in HKLM\SYSTEM\CurrentControlSet\Control\VirtualDeviceDrivers "VDD". It
 * claims port 0x2E0: OUT 0x2E0,AL copies one byte from DMA channel 1 into a counter;
 * IN AL,0x2E0 reads how many bytes it has moved, IN 0x2E1 the IRQ line it was given.
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#include <windows.h>
#include "ntvdmex-vddsvc.h"

#define SAMPLE_PORT_FIRST   0x2E0
#define SAMPLE_PORT_LAST    0x2E1
#define SAMPLE_HOOK_BASE    0xD8000u    /* An upper-memory window nothing else uses */
#define SAMPLE_HOOK_BYTES   0x1000u
#define SAMPLE_DMA_CHANNEL  1

/* nt_vdd.h's I/O hook structures, by layout (STDCALL handlers). */
typedef struct
{
    WORD First;
    WORD Last;
} SAMPLE_PORT_RANGE;

typedef struct
{
    VOID (WINAPI *InByte)(WORD port, BYTE *data);
    VOID (WINAPI *InWord)(WORD port, WORD *data);
    VOID (WINAPI *InString)(WORD port, BYTE *data, WORD count);
    VOID (WINAPI *InWordString)(WORD port, WORD *data, WORD count);
    VOID (WINAPI *OutByte)(WORD port, BYTE data);
    VOID (WINAPI *OutWord)(WORD port, WORD data);
    VOID (WINAPI *OutString)(WORD port, BYTE *data, WORD count);
    VOID (WINAPI *OutWordString)(WORD port, WORD *data, WORD count);
} SAMPLE_IO_HANDLERS;

BOOL WINAPI VDDInstallIOHook(
    HANDLE vdd,
    WORD rangeCount,
    SAMPLE_PORT_RANGE *ranges,
    SAMPLE_IO_HANDLERS *handlers);

static HANDLE g_Vdd;
static WORD   g_Irq = NTVDMEX_VDD_ANY_IRQ;
static DWORD  g_Moved;
static DWORD  g_Programs;

static VOID WINAPI SampleMemoryTouched(PVOID address, ULONG readWriteMode)
{
    (VOID)address;
    (VOID)readWriteMode;
    VDDAllocMem(g_Vdd, (PVOID)(ULONG_PTR)SAMPLE_HOOK_BASE, SAMPLE_HOOK_BYTES);    /* make it present */
}

static VOID WINAPI SampleProgramStarted(USHORT pdb)
{
    (VOID)pdb;
    ++g_Programs;
}

static VOID WINAPI SampleProgramEnded(USHORT pdb)
{
    (VOID)pdb;
}

static VOID WINAPI SampleInByte(WORD port, BYTE *data)
{
    *data = (BYTE)(port == SAMPLE_PORT_FIRST ? g_Moved : g_Irq);
}

static VOID WINAPI SampleOutByte(WORD port, BYTE data)
{
    BYTE byte;
    NTVDMEX_VDD_DMA_INFO info;

    (VOID)port;
    (VOID)data;

    if (VDDQueryDMA(g_Vdd, SAMPLE_DMA_CHANNEL, &info) && !info.Mask)
        g_Moved += VDDRequestDMA(g_Vdd, SAMPLE_DMA_CHANNEL, &byte, 1);
}

BOOL WINAPI DllEntryPoint(HINSTANCE instance, DWORD reason, LPVOID reserved)
{
    static SAMPLE_PORT_RANGE range = { SAMPLE_PORT_FIRST, SAMPLE_PORT_LAST };
    static SAMPLE_IO_HANDLERS handlers;

    (VOID)reserved;

    if (reason == DLL_PROCESS_ATTACH)
    {
        g_Vdd = (HANDLE)instance;
        handlers.InByte = SampleInByte;
        handlers.OutByte = SampleOutByte;

        if (!VDDInstallIOHook(g_Vdd, 1, &range, &handlers))
            return FALSE;

        g_Irq = VDDReserveIrqLine(g_Vdd, NTVDMEX_VDD_ANY_IRQ);
        VDDInstallMemoryHook(g_Vdd, (PVOID)(ULONG_PTR)SAMPLE_HOOK_BASE, SAMPLE_HOOK_BYTES, SampleMemoryTouched);
        VDDInstallUserHook(g_Vdd, SampleProgramStarted, SampleProgramEnded, NULL, NULL);
    }
    else if (reason == DLL_PROCESS_DETACH)
    {
        VDDDeInstallUserHook(g_Vdd);
        VDDDeInstallMemoryHook(g_Vdd, (PVOID)(ULONG_PTR)SAMPLE_HOOK_BASE, SAMPLE_HOOK_BYTES);

        if (g_Irq != NTVDMEX_VDD_ANY_IRQ)
            VDDReleaseIrqLine(g_Vdd, g_Irq);
    }

    return TRUE;
}
