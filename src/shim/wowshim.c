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
    DWORD  version;                                         /* 2 */
    void  *(*getvdmptr)(DWORD vp, DWORD cb, BOOL pm);
    HANDLE (*handle32)(WORD h16, DWORD type);
    WORD   (*handle16)(HANDLE h32, DWORD type);
    BOOL   (*callback16ex)(DWORD vpfn, DWORD flags, DWORD cb, void *args, DWORD *ret);
    void   (*ica_interrupt)(int ms, BYTE line, int count);
    void   (*yield16)(void);
    void   (*log)(const char *what);
    DWORD  (*global16)(int op, DWORD a, DWORD b);           /* 2: krnl386's global heap */
} ntvdmex_shim_api_t;

static ntvdmex_shim_api_t g_api;
static int g_have;

__declspec(dllexport) BOOL WINAPI NtvdmexShimInit(const ntvdmex_shim_api_t *api)
{
    if (!api || api->version != 2) return FALSE;
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
     tools/wintest/w_wcb: both return 0001 where GlobalFree said 0. */
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

BOOL WINAPI DllMainCRTStartup(HINSTANCE h, DWORD why, LPVOID r)
{ (void)h; (void)why; (void)r; return TRUE; }
