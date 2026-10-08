/*
 * wowshim.c -- WOW32.DLL and NTVDM.EXE, as this host's stand-ins.  GH #5 / #278, s90.
 *
 * ── WHY TWO MODULES WITH MICROSOFT'S NAMES EXIST IN OUR PROCESS ────────────────
 * A 32-bit DLL that serves 16-bit code (a generic-thunk target) talks back to the
 * VDM through two documented surfaces, and FINDS them by module name:
 *   - WOW32.DLL: WOWGetVDMPointer, WOWHandle16/32, WOWCallback16(Ex), WOWGlobal*16,
 *     WOWYield16 -- the "WOW32 interface" of wownt32.h;
 *   - NTVDM.EXE: call_ica_hw_interrupt, how a VDD-style component raises an IRQ.
 * XP's own winmm.dll does exactly this (reverse/xp/winmm.dll, s90): NotifyCallbackData
 * calls GetModuleHandleW(L"WOW32.DLL") and GetProcAddress for WOWGetVDMPointer /
 * WOWHandle32 / WOWHandle16, and GetModuleHandleW(L"NTVDM.EXE") for
 * call_ica_hw_interrupt -- which it calls with (1, 2|4, 1): slave PIC line 2, IRQ 10,
 * the vector 72h that 16-bit MMSYSTEM hooks. With neither module present winmm
 * concludes it is not under WOW, NotifyCallbackData returns 0, and MMSYSTEM reports
 * "no device drivers".
 *
 * Under real NTVDM those modules ARE the VDM. Here the VDM is ntvdmhost.exe, so this
 * one source is built twice under those two names (bin\wowshim\) and loaded by full
 * path at WOW start-up; every export forwards to a table the host hands over in
 * NtvdmexShimInit. Nothing here knows anything about the VDM itself.
 *
 * ⚠ A CALL WITH NO HOST ENTRY FAILS, LOUDLY: 0/FALSE, said through the host's log
 *   hook. WOWCallback16(Ex) and WOWGlobal*16 (s91, #309) run 16-bit code through the
 *   host's nested run, so they work on the guest thread only.
 */
#include <windows.h>
#include "../ntvdmex_bits.h"
#include "../ntvdmex_x86.h"

#include "shim_api.h"            /* SHIM_API_VERSION: host + bin\wowshim\ must agree */

/* The table the host hands over in NtvdmexShimInit. Its LAYOUT is the contract (the
   host's copy in main.c must match it field for field); the names are ours. */
typedef struct _NTVDMEX_SHIM_API {
    DWORD  Version;                                         /* 3 */
    PVOID  (*GetVdmPointer)(DWORD segmentedAddress, DWORD byteCount, BOOL isProtectedMode);
    HANDLE (*Handle32)(WORD handle16, DWORD handleType);
    WORD   (*Handle16)(HANDLE handle32, DWORD handleType);
    BOOL   (*Callback16Ex)(DWORD segmentedFunction, DWORD flags, DWORD argumentBytes, PVOID arguments, DWORD *returnValue);
    VOID   (*IcaInterrupt)(INT picAdapter, BYTE line, INT count);
    VOID   (*Yield16)(VOID);
    VOID   (*Log)(PCSTR message);
    DWORD  (*Global16)(INT operation, DWORD firstArgument, DWORD secondArgument);   /* 2: krnl386's global heap */
    /* 3 (s91): the VDD service API of nt_vdd.h / vddsvc.h, for a Microsoft-ABI VDD
       that imports it from NTVDM.EXE by name. Registers by index (SHIM_R_*). */
    DWORD  (*GetRegister)(INT registerIndex);
    VOID   (*SetRegister)(INT registerIndex, DWORD value);
    PVOID  (*MapFlat)(WORD segment, DWORD offset, INT isProtectedMode);
    BOOL   (*InstallIoHook)(HANDLE vddHandle, WORD rangeCount, const VOID *ranges, const VOID *handlers);
    VOID   (*RemoveIoHook)(HANDLE vddHandle, WORD rangeCount, const VOID *ranges);
} NTVDMEX_SHIM_API, *PNTVDMEX_SHIM_API;

typedef const NTVDMEX_SHIM_API *PCNTVDMEX_SHIM_API;

static NTVDMEX_SHIM_API g_ShimApi;
static INT g_HasShimApi;

__declspec(dllexport) BOOL WINAPI NtvdmexShimInit(PCNTVDMEX_SHIM_API api)
{
    if (!api || api->Version != SHIM_API_VERSION) return FALSE;
    g_ShimApi = *api;
    g_HasShimApi = TRUE;
    return TRUE;
}

static VOID ShimReportMissing(PCSTR message) { if (g_HasShimApi && g_ShimApi.Log) g_ShimApi.Log(message); }

#define SHIM_MISSING_CALLBACK16EX   "WOWCallback16Ex: no host entry"
#define SHIM_MISSING_GLOBAL16       "WOWGlobal*16: no host entry"
#define SHIM_MISSING_IO_HOOK        "VDDInstallIOHook: no host entry"
#define SHIM_TERMINATE_VDM          "VDDTerminateVDM: the VDD asked to end the VDM"
#define SHIM_NO_HANDLE16            0
#define SHIM_NO_RESULT              0

/* ── WOW32.DLL ────────────────────────────────────────────────────────────── */
__declspec(dllexport) LPVOID WINAPI WOWGetVDMPointer(DWORD segmentedAddress, DWORD byteCount, BOOL isProtectedMode)
{ return (g_HasShimApi && g_ShimApi.GetVdmPointer) ? g_ShimApi.GetVdmPointer(segmentedAddress, byteCount, isProtectedMode) : NULL; }
__declspec(dllexport) LPVOID WINAPI WOWGetVDMPointerFix(DWORD segmentedAddress, DWORD byteCount, BOOL isProtectedMode)
{ return WOWGetVDMPointer(segmentedAddress, byteCount, isProtectedMode); }   /* our selectors never move: Fix is Get */
__declspec(dllexport) VOID WINAPI WOWGetVDMPointerUnfix(DWORD segmentedAddress) { (VOID)segmentedAddress; }
__declspec(dllexport) HANDLE WINAPI WOWHandle32(WORD handle16, DWORD handleType)
{ return (g_HasShimApi && g_ShimApi.Handle32) ? g_ShimApi.Handle32(handle16, handleType) : NULL; }
__declspec(dllexport) WORD WINAPI WOWHandle16(HANDLE handle32, DWORD handleType)
{ return (g_HasShimApi && g_ShimApi.Handle16) ? g_ShimApi.Handle16(handle32, handleType) : SHIM_NO_HANDLE16; }
__declspec(dllexport) BOOL WINAPI WOWCallback16Ex(DWORD segmentedFunction, DWORD flags, DWORD argumentBytes, PVOID arguments, PDWORD returnValue)
{
    if (g_HasShimApi && g_ShimApi.Callback16Ex) return g_ShimApi.Callback16Ex(segmentedFunction, flags, argumentBytes, arguments, returnValue);
    ShimReportMissing(SHIM_MISSING_CALLBACK16EX); return FALSE;
}

#define SHIM_WCB16_PASCAL           0       /* wownt32.h: WCB16_PASCAL                    */
#define SHIM_ONE_DWORD_ARGUMENT     4       /* bytes                                     */

__declspec(dllexport) DWORD WINAPI WOWCallback16(DWORD segmentedFunction, DWORD argument)
{
    DWORD returnValue = SHIM_NO_RESULT;
    /* WCB16_PASCAL (0), one DWORD argument */
    if (!WOWCallback16Ex(segmentedFunction, SHIM_WCB16_PASCAL, SHIM_ONE_DWORD_ARGUMENT, &argument, &returnValue)) return SHIM_NO_RESULT;
    return returnValue;
}
__declspec(dllexport) VOID WINAPI WOWYield16(VOID)
{ if (g_HasShimApi && g_ShimApi.Yield16) g_ShimApi.Yield16(); }
__declspec(dllexport) VOID WINAPI WOWDirectedYield16(WORD task16) { (VOID)task16; WOWYield16(); }

/* s91 (#309): krnl386's own global heap, through the host (shim_global16). Ops:
   0 GlobalAlloc(flags, cb) 1 GlobalFree(h) 2 GlobalLock(h) 3 GlobalUnlock(h)
   4 GlobalSize(h) 5 GlobalHandle(sel) -- answers exactly as the 16-bit calls do.
   The operation codes are the host's too: SHIM_GLOBAL_* in shim_api.h. */
#define SHIM_NO_ARGUMENT            0
#define SHIM_GLOBAL_FREED           0       /* Win16 GlobalFree: 0 = freed                */
#define SHIM_SELECTOR_SHIFT         16      /* a 16:16 pointer's selector                */

static DWORD ShimGlobal16(INT operation, DWORD firstArgument, DWORD secondArgument)
{
    if (g_HasShimApi && g_ShimApi.Global16) return g_ShimApi.Global16(operation, firstArgument, secondArgument);
    ShimReportMissing(SHIM_MISSING_GLOBAL16);
    return SHIM_NO_RESULT;
}
__declspec(dllexport) WORD WINAPI WOWGlobalAlloc16(WORD flags, DWORD byteCount)
{ return (WORD)ShimGlobal16(SHIM_GLOBAL_ALLOC, flags, byteCount); }
/* ⚠ Free16 and UnlockFree16 answer TRUE (1) when the block is freed -- NOT Win16's
     GlobalFree convention (0 = freed, else the handle). Measured against stock with
     tests/probes/win16/w_wcb: both return 0001 where GlobalFree said 0. */
__declspec(dllexport) WORD WINAPI WOWGlobalFree16(WORD handle16)
{ return (WORD)(ShimGlobal16(SHIM_GLOBAL_FREE, handle16, SHIM_NO_ARGUMENT) == SHIM_GLOBAL_FREED); }
__declspec(dllexport) DWORD WINAPI WOWGlobalLock16(WORD handle16)
{ return ShimGlobal16(SHIM_GLOBAL_LOCK, handle16, SHIM_NO_ARGUMENT); }
__declspec(dllexport) BOOL WINAPI WOWGlobalUnlock16(WORD handle16)
{ return (BOOL)(WORD)ShimGlobal16(SHIM_GLOBAL_UNLOCK, handle16, SHIM_NO_ARGUMENT); }
__declspec(dllexport) DWORD WINAPI WOWGlobalAllocLock16(WORD flags, DWORD byteCount, WORD *handle16Out)
{
    WORD handle16 = (WORD)ShimGlobal16(SHIM_GLOBAL_ALLOC, flags, byteCount);
    if (handle16Out) *handle16Out = handle16;
    return handle16 ? ShimGlobal16(SHIM_GLOBAL_LOCK, handle16, SHIM_NO_ARGUMENT) : SHIM_NO_RESULT;
}
__declspec(dllexport) WORD WINAPI WOWGlobalUnlockFree16(DWORD segmentedAddress)
{
    WORD handle16 = (WORD)ShimGlobal16(SHIM_GLOBAL_HANDLE, (WORD)(segmentedAddress >> SHIM_SELECTOR_SHIFT), SHIM_NO_ARGUMENT);   /* GlobalHandle(selector) -> AX */
    if (!handle16) return SHIM_NO_HANDLE16;
    ShimGlobal16(SHIM_GLOBAL_UNLOCK, handle16, SHIM_NO_ARGUMENT);
    return (WORD)(ShimGlobal16(SHIM_GLOBAL_FREE, handle16, SHIM_NO_ARGUMENT) == SHIM_GLOBAL_FREED);
}
__declspec(dllexport) DWORD WINAPI WOWGlobalLockSize16(WORD handle16, PDWORD byteCount)
{
    if (byteCount) *byteCount = ShimGlobal16(SHIM_GLOBAL_SIZE, handle16, SHIM_NO_ARGUMENT);
    return ShimGlobal16(SHIM_GLOBAL_LOCK, handle16, SHIM_NO_ARGUMENT);
}

/* ── NTVDM.EXE ────────────────────────────────────────────────────────────── */
/* VOID call_ica_hw_interrupt(int ms, half_word line, int count) -- `ms` is the PIC adapter,
   0 = master, 1 = slave -- -- and it is STDCALL,
   whatever vddsvc.h's bare prototype suggests: winmm pushes three arguments and the
   very next instruction is `pop edi` (winmm 0x76b50d95/9b), so the callee must have
   popped them. A cdecl export here would hand winmm a stack 12 bytes off. */
__declspec(dllexport) VOID WINAPI call_ica_hw_interrupt(INT picAdapter, BYTE line, INT count)
{ if (g_HasShimApi && g_ShimApi.IcaInterrupt) g_ShimApi.IcaInterrupt(picAdapter, line, count); }

/* ── THE VDD SERVICE API (s91, the SDK's binary-compatibility veneer, #11). ─────────
     What nt_vdd.h / vddsvc.h declare and NTVDM.EXE exports: a Microsoft-ABI VDD
     imports these BY NAME from NTVDM.EXE, and this module IS the NTVDM.EXE in our
     process, so its imports resolve here. Register accessors read and write the
     guest's context at the moment the VDD runs (a RegisterModule init, a
     DispatchCall, an I/O hook) -- WINAPI, as declared. Index order = SHIM_R_* in
     main.c. */
enum { SHIM_EAX, SHIM_EBX, SHIM_ECX, SHIM_EDX, SHIM_ESI, SHIM_EDI, SHIM_EBP, SHIM_ESP, SHIM_EIP,
       SHIM_CS, SHIM_DS, SHIM_ES, SHIM_SS, SHIM_FS, SHIM_GS, SHIM_EFLAGS, SHIM_MSW };
#define SHIM_KEEP_HIGH_WORD         0xFFFF0000u
#define SHIM_KEEP_ALL_BUT_LOW_BYTE  0xFFFFFF00u
#define SHIM_KEEP_ALL_BUT_HIGH_BYTE 0xFFFF00FFu
#define SHIM_FLAG_MASK              1
#define SHIM_FLAG_BIT               1u
#define SHIM_CARRY_FLAG             0       /* EFLAGS bit numbers                         */
#define SHIM_PARITY_FLAG            2
#define SHIM_AUXILIARY_FLAG         4
#define SHIM_ZERO_FLAG              6
#define SHIM_SIGN_FLAG              7
#define SHIM_INTERRUPT_FLAG         9
#define SHIM_DIRECTION_FLAG         10
#define SHIM_OVERFLOW_FLAG          11

static DWORD ShimGetRegister(INT registerIndex) { return (g_HasShimApi && g_ShimApi.GetRegister) ? g_ShimApi.GetRegister(registerIndex) : SHIM_NO_RESULT; }
static VOID  ShimSetRegister(INT registerIndex, DWORD value) { if (g_HasShimApi && g_ShimApi.SetRegister) g_ShimApi.SetRegister(registerIndex, value); }

/* The accessor pairs the VDD API exports, generated per register: getAX/setAX, ... */
#define SHIM_WORD_REGISTER(name, registerIndex) \
    __declspec(dllexport) USHORT WINAPI get##name(VOID) { return (USHORT)ShimGetRegister(registerIndex); } \
    __declspec(dllexport) VOID WINAPI set##name(USHORT value) { ShimSetRegister(registerIndex, (ShimGetRegister(registerIndex) & SHIM_KEEP_HIGH_WORD) | value); }
#define SHIM_DWORD_REGISTER(name, registerIndex) \
    __declspec(dllexport) ULONG WINAPI get##name(VOID) { return ShimGetRegister(registerIndex); } \
    __declspec(dllexport) VOID WINAPI set##name(ULONG value) { ShimSetRegister(registerIndex, value); }
#define SHIM_LOW_BYTE_REGISTER(name, registerIndex) \
    __declspec(dllexport) UCHAR WINAPI get##name(VOID) { return (UCHAR)ShimGetRegister(registerIndex); } \
    __declspec(dllexport) VOID WINAPI set##name(UCHAR value) { ShimSetRegister(registerIndex, (ShimGetRegister(registerIndex) & SHIM_KEEP_ALL_BUT_LOW_BYTE) | value); }
#define SHIM_HIGH_BYTE_REGISTER(name, registerIndex) \
    __declspec(dllexport) UCHAR WINAPI get##name(VOID) { return (UCHAR)(ShimGetRegister(registerIndex) >> BYTE_SHIFT); } \
    __declspec(dllexport) VOID WINAPI set##name(UCHAR value) { ShimSetRegister(registerIndex, (ShimGetRegister(registerIndex) & SHIM_KEEP_ALL_BUT_HIGH_BYTE) | ((DWORD)value << BYTE_SHIFT)); }
#define SHIM_FLAG(name, bit) \
    __declspec(dllexport) ULONG WINAPI get##name(VOID) { return (ShimGetRegister(SHIM_EFLAGS) >> bit) & SHIM_FLAG_MASK; } \
    __declspec(dllexport) VOID WINAPI set##name(ULONG value) { ShimSetRegister(SHIM_EFLAGS, (ShimGetRegister(SHIM_EFLAGS) & ~(SHIM_FLAG_BIT << bit)) | ((value & SHIM_FLAG_MASK) << bit)); }
SHIM_WORD_REGISTER(AX, SHIM_EAX) SHIM_WORD_REGISTER(BX, SHIM_EBX) SHIM_WORD_REGISTER(CX, SHIM_ECX) SHIM_WORD_REGISTER(DX, SHIM_EDX)
SHIM_WORD_REGISTER(SI, SHIM_ESI) SHIM_WORD_REGISTER(DI, SHIM_EDI) SHIM_WORD_REGISTER(BP, SHIM_EBP) SHIM_WORD_REGISTER(SP, SHIM_ESP) SHIM_WORD_REGISTER(IP, SHIM_EIP)
SHIM_WORD_REGISTER(CS, SHIM_CS) SHIM_WORD_REGISTER(DS, SHIM_DS) SHIM_WORD_REGISTER(ES, SHIM_ES) SHIM_WORD_REGISTER(SS, SHIM_SS) SHIM_WORD_REGISTER(FS, SHIM_FS) SHIM_WORD_REGISTER(GS, SHIM_GS)
SHIM_DWORD_REGISTER(EAX, SHIM_EAX) SHIM_DWORD_REGISTER(EBX, SHIM_EBX) SHIM_DWORD_REGISTER(ECX, SHIM_ECX) SHIM_DWORD_REGISTER(EDX, SHIM_EDX)
SHIM_DWORD_REGISTER(ESI, SHIM_ESI) SHIM_DWORD_REGISTER(EDI, SHIM_EDI) SHIM_DWORD_REGISTER(EBP, SHIM_EBP) SHIM_DWORD_REGISTER(ESP, SHIM_ESP) SHIM_DWORD_REGISTER(EIP, SHIM_EIP)
SHIM_LOW_BYTE_REGISTER(AL, SHIM_EAX) SHIM_LOW_BYTE_REGISTER(BL, SHIM_EBX) SHIM_LOW_BYTE_REGISTER(CL, SHIM_ECX) SHIM_LOW_BYTE_REGISTER(DL, SHIM_EDX)
SHIM_HIGH_BYTE_REGISTER(AH, SHIM_EAX) SHIM_HIGH_BYTE_REGISTER(BH, SHIM_EBX) SHIM_HIGH_BYTE_REGISTER(CH, SHIM_ECX) SHIM_HIGH_BYTE_REGISTER(DH, SHIM_EDX)
SHIM_FLAG(CF, SHIM_CARRY_FLAG) SHIM_FLAG(PF, SHIM_PARITY_FLAG) SHIM_FLAG(AF, SHIM_AUXILIARY_FLAG) SHIM_FLAG(ZF, SHIM_ZERO_FLAG)
SHIM_FLAG(SF, SHIM_SIGN_FLAG) SHIM_FLAG(IF, SHIM_INTERRUPT_FLAG) SHIM_FLAG(DF, SHIM_DIRECTION_FLAG) SHIM_FLAG(OF, SHIM_OVERFLOW_FLAG)
__declspec(dllexport) USHORT WINAPI getMSW(VOID) { return (USHORT)ShimGetRegister(SHIM_MSW); }
__declspec(dllexport) VOID WINAPI setMSW(USHORT value) { (VOID)value; }   /* not settable here */

/* PVOID VdmMapFlat(USHORT seg, ULONG off, VDM_MODE mode): VDM_V86 = 0, VDM_PM = 1 */
#define SHIM_VDM_PM                 1

__declspec(dllexport) PVOID WINAPI VdmMapFlat(USHORT segment, ULONG offset, ULONG mode)
{ return (g_HasShimApi && g_ShimApi.MapFlat) ? g_ShimApi.MapFlat(segment, offset, mode == SHIM_VDM_PM) : NULL; }
__declspec(dllexport) BOOL WINAPI VdmUnmapFlat(USHORT segment, ULONG offset, PVOID buffer, ULONG mode)
{ (VOID)segment; (VOID)offset; (VOID)buffer; (VOID)mode; return TRUE; }   /* nothing was copied */
__declspec(dllexport) BOOL WINAPI VdmFlushCache(USHORT segment, ULONG offset, ULONG byteCount, ULONG mode)
{ (VOID)segment; (VOID)offset; (VOID)byteCount; (VOID)mode; return TRUE; }  /* real CPU: no cache */

/* BOOL VDDInstallIOHook(HANDLE hVdd, WORD cPortRange, PVDD_IO_PORTRANGE,
                         PVDD_IO_HANDLERS) and its undo */
__declspec(dllexport) BOOL WINAPI VDDInstallIOHook(HANDLE vddHandle, WORD rangeCount, PVOID ranges, PVOID handlers)
{
    if (g_HasShimApi && g_ShimApi.InstallIoHook) return g_ShimApi.InstallIoHook(vddHandle, rangeCount, ranges, handlers);
    ShimReportMissing(SHIM_MISSING_IO_HOOK); return FALSE;
}
__declspec(dllexport) VOID WINAPI VDDDeInstallIOHook(HANDLE vddHandle, WORD rangeCount, PVOID ranges)
{ if (g_HasShimApi && g_ShimApi.RemoveIoHook) g_ShimApi.RemoveIoHook(vddHandle, rangeCount, ranges); }
/* VDDSimulateInterrupt(ms, line, count) is call_ica_hw_interrupt by another name */
__declspec(dllexport) VOID WINAPI VDDSimulateInterrupt(INT picAdapter, BYTE line, INT count)
{ if (g_HasShimApi && g_ShimApi.IcaInterrupt) g_ShimApi.IcaInterrupt(picAdapter, line, count); }

#define SHIM_TERMINATE_EXIT_CODE    0

__declspec(dllexport) VOID WINAPI VDDTerminateVDM(VOID)
{ ShimReportMissing(SHIM_TERMINATE_VDM); ExitProcess(SHIM_TERMINATE_EXIT_CODE); }

BOOL WINAPI DllMainCRTStartup(HINSTANCE instance, DWORD reason, LPVOID reserved)
{ (VOID)instance; (VOID)reason; (VOID)reserved; return TRUE; }
