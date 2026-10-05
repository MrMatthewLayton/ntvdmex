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

typedef struct {
    DWORD  version;                                         /* 3 */
    void  *(*getvdmptr)(DWORD vp, DWORD cb, BOOL pm);
    HANDLE (*handle32)(WORD h16, DWORD type);
    WORD   (*handle16)(HANDLE h32, DWORD type);
    BOOL   (*callback16ex)(DWORD vpfn, DWORD flags, DWORD cb, void *args, DWORD *ret);
    void   (*ica_interrupt)(int ms, BYTE line, int count);
    void   (*yield16)(void);
    void   (*log)(const char *what);
    DWORD  (*global16)(int op, DWORD a, DWORD b);           /* 2: krnl386's global heap */
    /* 3 (s91): the VDD service API of nt_vdd.h / vddsvc.h, for a Microsoft-ABI VDD
       that imports it from NTVDM.EXE by name. Registers by index (SHIM_R_*). */
    DWORD  (*getreg)(int r);
    void   (*setreg)(int r, DWORD v);
    void  *(*mapflat)(WORD seg, DWORD off, int pm);
    BOOL   (*io_hook)(HANDLE hvdd, WORD n, const void *ranges, const void *handlers);
    void   (*io_unhook)(HANDLE hvdd, WORD n, const void *ranges);
} ntvdmex_shim_api_t;

static ntvdmex_shim_api_t g_api;
static int g_have;

__declspec(dllexport) BOOL WINAPI NtvdmexShimInit(const ntvdmex_shim_api_t *api)
{
    if (!api || api->version != 3) return FALSE;
    g_api = *api;
    g_have = 1;
    return TRUE;
}

static void miss(const char *what) { if (g_have && g_api.log) g_api.log(what); }

/* ── WOW32.DLL ────────────────────────────────────────────────────────────── */
__declspec(dllexport) LPVOID WINAPI WOWGetVDMPointer(DWORD vp, DWORD cb, BOOL pm)
{ return (g_have && g_api.getvdmptr) ? g_api.getvdmptr(vp, cb, pm) : NULL; }
__declspec(dllexport) LPVOID WINAPI WOWGetVDMPointerFix(DWORD vp, DWORD cb, BOOL pm)
{ return WOWGetVDMPointer(vp, cb, pm); }   /* our selectors never move: Fix is Get */
__declspec(dllexport) VOID WINAPI WOWGetVDMPointerUnfix(DWORD vp) { (void)vp; }
__declspec(dllexport) HANDLE WINAPI WOWHandle32(WORD h, DWORD type)
{ return (g_have && g_api.handle32) ? g_api.handle32(h, type) : NULL; }
__declspec(dllexport) WORD WINAPI WOWHandle16(HANDLE h, DWORD type)
{ return (g_have && g_api.handle16) ? g_api.handle16(h, type) : 0; }
__declspec(dllexport) BOOL WINAPI WOWCallback16Ex(DWORD vpfn, DWORD fl, DWORD cb, PVOID a, PDWORD r)
{
    if (g_have && g_api.callback16ex) return g_api.callback16ex(vpfn, fl, cb, a, r);
    miss("WOWCallback16Ex: no host entry"); return FALSE;
}
__declspec(dllexport) DWORD WINAPI WOWCallback16(DWORD vpfn, DWORD p)
{
    DWORD r = 0;
    /* WCB16_PASCAL (0), one DWORD argument */
    if (!WOWCallback16Ex(vpfn, 0, 4, &p, &r)) return 0;
    return r;
}
__declspec(dllexport) VOID WINAPI WOWYield16(VOID)
{ if (g_have && g_api.yield16) g_api.yield16(); }
__declspec(dllexport) VOID WINAPI WOWDirectedYield16(WORD t) { (void)t; WOWYield16(); }
/* s91 (#309): krnl386's own global heap, through the host (shim_global16). Ops:
   0 GlobalAlloc(flags, cb) 1 GlobalFree(h) 2 GlobalLock(h) 3 GlobalUnlock(h)
   4 GlobalSize(h) 5 GlobalHandle(sel) -- answers exactly as the 16-bit calls do. */
static DWORD g16(int op, DWORD a, DWORD b)
{
    if (g_have && g_api.global16) return g_api.global16(op, a, b);
    miss("WOWGlobal*16: no host entry");
    return 0;
}
__declspec(dllexport) WORD WINAPI WOWGlobalAlloc16(WORD f, DWORD cb)
{ return (WORD)g16(0, f, cb); }
/* ⚠ Free16 and UnlockFree16 answer TRUE (1) when the block is freed -- NOT Win16's
     GlobalFree convention (0 = freed, else the handle). Measured against stock with
     tests/probes/win16/w_wcb: both return 0001 where GlobalFree said 0. */
__declspec(dllexport) WORD WINAPI WOWGlobalFree16(WORD h)
{ return (WORD)(g16(1, h, 0) == 0); }
__declspec(dllexport) DWORD WINAPI WOWGlobalLock16(WORD h)
{ return g16(2, h, 0); }
__declspec(dllexport) BOOL WINAPI WOWGlobalUnlock16(WORD h)
{ return (BOOL)(WORD)g16(3, h, 0); }
__declspec(dllexport) DWORD WINAPI WOWGlobalAllocLock16(WORD f, DWORD cb, WORD *ph)
{
    WORD h = (WORD)g16(0, f, cb);
    if (ph) *ph = h;
    return h ? g16(2, h, 0) : 0;
}
__declspec(dllexport) WORD WINAPI WOWGlobalUnlockFree16(DWORD vp)
{
    WORD h = (WORD)g16(5, (WORD)(vp >> 16), 0);   /* GlobalHandle(selector) -> AX */
    if (!h) return 0;
    g16(3, h, 0);
    return (WORD)(g16(1, h, 0) == 0);
}
__declspec(dllexport) DWORD WINAPI WOWGlobalLockSize16(WORD h, PDWORD pcb)
{
    if (pcb) *pcb = g16(4, h, 0);
    return g16(2, h, 0);
}

/* ── NTVDM.EXE ────────────────────────────────────────────────────────────── */
/* VOID call_ica_hw_interrupt(int ms, half_word line, int count) -- and it is STDCALL,
   whatever vddsvc.h's bare prototype suggests: winmm pushes three arguments and the
   very next instruction is `pop edi` (winmm 0x76b50d95/9b), so the callee must have
   popped them. A cdecl export here would hand winmm a stack 12 bytes off. */
__declspec(dllexport) void WINAPI call_ica_hw_interrupt(int ms, BYTE line, int count)
{ if (g_have && g_api.ica_interrupt) g_api.ica_interrupt(ms, line, count); }

/* ── THE VDD SERVICE API (s91, the SDK's binary-compatibility veneer, #11). ─────────
     What nt_vdd.h / vddsvc.h declare and NTVDM.EXE exports: a Microsoft-ABI VDD
     imports these BY NAME from NTVDM.EXE, and this module IS the NTVDM.EXE in our
     process, so its imports resolve here. Register accessors read and write the
     guest's context at the moment the VDD runs (a RegisterModule init, a
     DispatchCall, an I/O hook) -- WINAPI, as declared. Index order = SHIM_R_* in
     main.c. */
enum { R_EAX, R_EBX, R_ECX, R_EDX, R_ESI, R_EDI, R_EBP, R_ESP, R_EIP,
       R_CS, R_DS, R_ES, R_SS, R_FS, R_GS, R_EFL, R_MSW };
static DWORD gr(int r) { return (g_have && g_api.getreg) ? g_api.getreg(r) : 0; }
static void  sr(int r, DWORD v) { if (g_have && g_api.setreg) g_api.setreg(r, v); }
#define W16(n, r) \
    __declspec(dllexport) USHORT WINAPI get##n(VOID) { return (USHORT)gr(r); } \
    __declspec(dllexport) VOID WINAPI set##n(USHORT v) { sr(r, (gr(r) & 0xFFFF0000u) | v); }
#define W32(n, r) \
    __declspec(dllexport) ULONG WINAPI get##n(VOID) { return gr(r); } \
    __declspec(dllexport) VOID WINAPI set##n(ULONG v) { sr(r, v); }
#define LO8(n, r) \
    __declspec(dllexport) UCHAR WINAPI get##n(VOID) { return (UCHAR)gr(r); } \
    __declspec(dllexport) VOID WINAPI set##n(UCHAR v) { sr(r, (gr(r) & 0xFFFFFF00u) | v); }
#define HI8(n, r) \
    __declspec(dllexport) UCHAR WINAPI get##n(VOID) { return (UCHAR)(gr(r) >> 8); } \
    __declspec(dllexport) VOID WINAPI set##n(UCHAR v) { sr(r, (gr(r) & 0xFFFF00FFu) | ((DWORD)v << 8)); }
#define FLG(n, bit) \
    __declspec(dllexport) ULONG WINAPI get##n(VOID) { return (gr(R_EFL) >> bit) & 1; } \
    __declspec(dllexport) VOID WINAPI set##n(ULONG v) { sr(R_EFL, (gr(R_EFL) & ~(1u << bit)) | ((v & 1) << bit)); }
W16(AX, R_EAX) W16(BX, R_EBX) W16(CX, R_ECX) W16(DX, R_EDX)
W16(SI, R_ESI) W16(DI, R_EDI) W16(BP, R_EBP) W16(SP, R_ESP) W16(IP, R_EIP)
W16(CS, R_CS) W16(DS, R_DS) W16(ES, R_ES) W16(SS, R_SS) W16(FS, R_FS) W16(GS, R_GS)
W32(EAX, R_EAX) W32(EBX, R_EBX) W32(ECX, R_ECX) W32(EDX, R_EDX)
W32(ESI, R_ESI) W32(EDI, R_EDI) W32(EBP, R_EBP) W32(ESP, R_ESP) W32(EIP, R_EIP)
LO8(AL, R_EAX) LO8(BL, R_EBX) LO8(CL, R_ECX) LO8(DL, R_EDX)
HI8(AH, R_EAX) HI8(BH, R_EBX) HI8(CH, R_ECX) HI8(DH, R_EDX)
FLG(CF, 0) FLG(PF, 2) FLG(AF, 4) FLG(ZF, 6) FLG(SF, 7) FLG(IF, 9) FLG(DF, 10) FLG(OF, 11)
__declspec(dllexport) USHORT WINAPI getMSW(VOID) { return (USHORT)gr(R_MSW); }
__declspec(dllexport) VOID WINAPI setMSW(USHORT v) { (void)v; }   /* not settable here */

/* PVOID VdmMapFlat(USHORT seg, ULONG off, VDM_MODE mode): VDM_V86 = 0, VDM_PM = 1 */
__declspec(dllexport) PVOID WINAPI VdmMapFlat(USHORT seg, ULONG off, ULONG mode)
{ return (g_have && g_api.mapflat) ? g_api.mapflat(seg, off, mode == 1) : NULL; }
__declspec(dllexport) BOOL WINAPI VdmUnmapFlat(USHORT seg, ULONG off, PVOID p, ULONG mode)
{ (void)seg; (void)off; (void)p; (void)mode; return TRUE; }   /* nothing was copied */
__declspec(dllexport) BOOL WINAPI VdmFlushCache(USHORT seg, ULONG off, ULONG n, ULONG mode)
{ (void)seg; (void)off; (void)n; (void)mode; return TRUE; }  /* real CPU: no cache */

/* BOOL VDDInstallIOHook(HANDLE hVdd, WORD cPortRange, PVDD_IO_PORTRANGE,
                         PVDD_IO_HANDLERS) and its undo */
__declspec(dllexport) BOOL WINAPI VDDInstallIOHook(HANDLE h, WORD n, PVOID ranges, PVOID handlers)
{
    if (g_have && g_api.io_hook) return g_api.io_hook(h, n, ranges, handlers);
    miss("VDDInstallIOHook: no host entry"); return FALSE;
}
__declspec(dllexport) VOID WINAPI VDDDeInstallIOHook(HANDLE h, WORD n, PVOID ranges)
{ if (g_have && g_api.io_unhook) g_api.io_unhook(h, n, ranges); }
/* VDDSimulateInterrupt(ms, line, count) is call_ica_hw_interrupt by another name */
__declspec(dllexport) VOID WINAPI VDDSimulateInterrupt(int ms, BYTE line, int count)
{ if (g_have && g_api.ica_interrupt) g_api.ica_interrupt(ms, line, count); }
__declspec(dllexport) VOID WINAPI VDDTerminateVDM(VOID)
{ miss("VDDTerminateVDM: the VDD asked to end the VDM"); ExitProcess(0); }

BOOL WINAPI DllMainCRTStartup(HINSTANCE h, DWORD why, LPVOID r)
{ (void)h; (void)why; (void)r; return TRUE; }
