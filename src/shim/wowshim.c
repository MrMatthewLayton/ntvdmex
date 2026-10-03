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
 * ⚠ A CALL WITH NO HOST ENTRY FAILS, LOUDLY. The table may leave slots NULL (the
 *   WOWGlobal*16 family needs krnl386's own allocator, which is not reachable from a
 *   foreign thread); those answer 0/FALSE and say so through the host's log hook.
 */
#include <windows.h>

typedef struct {
    DWORD  version;                                         /* 1 */
    void  *(*getvdmptr)(DWORD vp, DWORD cb, BOOL pm);
    HANDLE (*handle32)(WORD h16, DWORD type);
    WORD   (*handle16)(HANDLE h32, DWORD type);
    BOOL   (*callback16ex)(DWORD vpfn, DWORD flags, DWORD cb, void *args, DWORD *ret);
    void   (*ica_interrupt)(int ms, BYTE line, int count);
    void   (*yield16)(void);
    void   (*log)(const char *what);
} ntvdmex_shim_api_t;

static ntvdmex_shim_api_t g_api;
static int g_have;

__declspec(dllexport) BOOL WINAPI NtvdmexShimInit(const ntvdmex_shim_api_t *api)
{
    if (!api || api->version != 1) return FALSE;
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
__declspec(dllexport) WORD WINAPI WOWGlobalAlloc16(WORD f, DWORD cb)
{ (void)f; (void)cb; miss("WOWGlobalAlloc16: not provided"); return 0; }
__declspec(dllexport) WORD WINAPI WOWGlobalFree16(WORD h)
{ (void)h; miss("WOWGlobalFree16: not provided"); return h; }
__declspec(dllexport) DWORD WINAPI WOWGlobalLock16(WORD h)
{ (void)h; miss("WOWGlobalLock16: not provided"); return 0; }
__declspec(dllexport) BOOL WINAPI WOWGlobalUnlock16(WORD h)
{ (void)h; miss("WOWGlobalUnlock16: not provided"); return FALSE; }
__declspec(dllexport) DWORD WINAPI WOWGlobalAllocLock16(WORD f, DWORD cb, WORD *ph)
{ (void)f; (void)cb; if (ph) *ph = 0; miss("WOWGlobalAllocLock16: not provided"); return 0; }
__declspec(dllexport) WORD WINAPI WOWGlobalUnlockFree16(DWORD vp)
{ (void)vp; miss("WOWGlobalUnlockFree16: not provided"); return 0; }
__declspec(dllexport) DWORD WINAPI WOWGlobalLockSize16(WORD h, PDWORD pcb)
{ (void)h; if (pcb) *pcb = 0; miss("WOWGlobalLockSize16: not provided"); return 0; }

/* ── NTVDM.EXE ────────────────────────────────────────────────────────────── */
/* VOID call_ica_hw_interrupt(int ms, half_word line, int count) -- and it is STDCALL,
   whatever vddsvc.h's bare prototype suggests: winmm pushes three arguments and the
   very next instruction is `pop edi` (winmm 0x76b50d95/9b), so the callee must have
   popped them. A cdecl export here would hand winmm a stack 12 bytes off. */
__declspec(dllexport) void WINAPI call_ica_hw_interrupt(int ms, BYTE line, int count)
{ if (g_have && g_api.ica_interrupt) g_api.ica_interrupt(ms, line, count); }

BOOL WINAPI DllMainCRTStartup(HINSTANCE h, DWORD why, LPVOID r)
{ (void)h; (void)why; (void)r; return TRUE; }
