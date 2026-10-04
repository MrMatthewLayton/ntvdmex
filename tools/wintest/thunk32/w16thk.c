/*
 * w16thk.c -- W16THK.DLL, the 32-bit half of tools/wintest/w_wcb (s91, #309).
 *
 * A 32-bit thunk DLL of the kind wownt32.h is for: 16-bit code reaches it through
 * the generic thunks (LoadLibraryEx32W / CallProc32W), and it calls BACK into the
 * VDM through WOW32.DLL. It finds WOW32.DLL the way XP's own winmm.dll does --
 * GetModuleHandle + GetProcAddress, never an import -- so the same file runs
 * under stock (the real WOW32.DLL) and under NTVDMEX (bin\wowshim\WOW32.DLL)
 * without either loader having to resolve a static import against the other.
 *
 *   T_Cb(vpfn, p)    WOWCallback16(vpfn, p)                      -> its DX:AX
 *   T_CbEx(vpfn)     WOWCallback16Ex, PASCAL, three WORDs 1,2,3  -> its DX:AX
 *                    (the callee answers a*100+b*10+c: 123 = the order is right)
 *   T_Glob()         Alloc/Lock/write/LockSize/Unlock/Free, then AllocLock +
 *                    UnlockFree -- a bit per step that answered as documented
 *
 * Build: tools/wintest/thunk32/build.sh  (i686-w64-mingw32, no CRT)
 */
#include <windows.h>

typedef BOOL  (WINAPI *cbex_t)(DWORD, DWORD, DWORD, PVOID, PDWORD);
typedef DWORD (WINAPI *cb_t)(DWORD, DWORD);
typedef WORD  (WINAPI *ga_t)(WORD, DWORD);
typedef WORD  (WINAPI *gf_t)(WORD);
typedef DWORD (WINAPI *gl_t)(WORD);
typedef BOOL  (WINAPI *gu_t)(WORD);
typedef DWORD (WINAPI *gal_t)(WORD, DWORD, WORD *);
typedef WORD  (WINAPI *guf_t)(DWORD);
typedef DWORD (WINAPI *gls_t)(WORD, PDWORD);
typedef LPVOID (WINAPI *vp_t)(DWORD, DWORD, BOOL);

static FARPROC fn(const char *name)
{
    HMODULE h = GetModuleHandleA("WOW32.DLL");
    return h ? GetProcAddress(h, name) : NULL;
}

__declspec(dllexport) DWORD WINAPI T_Cb(DWORD vpfn, DWORD p)
{
    cb_t f = (cb_t)fn("WOWCallback16");
    return f ? f(vpfn, p) : 0xDEAD0001u;
}

__declspec(dllexport) DWORD WINAPI T_CbEx(DWORD vpfn)
{
    cbex_t f = (cbex_t)fn("WOWCallback16Ex");
    WORD a[3];
    DWORD r = 0;
    /* PASCAL: the stack image, lowest address = the LAST argument */
    a[0] = 3; a[1] = 2; a[2] = 1;
    if (!f) return 0xDEAD0001u;
    if (!f(vpfn, 0 /* WCB16_PASCAL */, sizeof a, a, &r)) return 0xDEAD0002u;
    return r;
}

__declspec(dllexport) DWORD WINAPI T_Glob(void)
{
    ga_t  ga  = (ga_t)fn("WOWGlobalAlloc16");
    gf_t  gf  = (gf_t)fn("WOWGlobalFree16");
    gl_t  gl  = (gl_t)fn("WOWGlobalLock16");
    gu_t  gu  = (gu_t)fn("WOWGlobalUnlock16");
    gal_t gal = (gal_t)fn("WOWGlobalAllocLock16");
    guf_t guf = (guf_t)fn("WOWGlobalUnlockFree16");
    gls_t gls = (gls_t)fn("WOWGlobalLockSize16");
    vp_t  vp  = (vp_t)fn("WOWGetVDMPointer");
    DWORD bits = 0, p16, cb = 0;
    WORD h, h2 = 0;
    BYTE *p;
    if (!ga || !gf || !gl || !gu || !gal || !guf || !gls || !vp) return 0xDEAD0001u;
    h = ga(GMEM_MOVEABLE | GMEM_ZEROINIT, 100);
    if (h) bits |= 1;
    p16 = h ? gl(h) : 0;
    if (p16) bits |= 2;
    p = p16 ? (BYTE *)vp(p16, 100, TRUE) : NULL;
    if (p && p[0] == 0 && p[99] == 0) bits |= 4;            /* ZEROINIT honoured */
    if (p) { p[0] = 'O'; p[1] = 'K'; }
    if (h && gls(h, &cb) == p16 && cb >= 100) bits |= 8;    /* same block, its size */
    if (p && ((BYTE *)vp(gl(h), 2, TRUE))[1] == 'K') bits |= 0x10;
    if (h) { gu(h); gu(h); gu(h); }                         /* three locks taken */
    {   WORD r = h ? gf(h) : 0xFFFF;
        if (r == 0) bits |= 0x20;                           /* GlobalFree: 0 = freed */
        bits |= (DWORD)(r == 0 ? 0 : r == h ? 1 : 2) << 8;  /* what it said instead  */
        if (r != 0 && r != h) bits |= (DWORD)(r & 0xFF) << 16;  /* ...and its low byte */
    }
    p16 = gal(GMEM_FIXED, 32, &h2);
    if (p16 && h2) bits |= 0x40;
    if (p16) {
        WORD r = guf(p16);
        if (r == 0) bits |= 0x80;
        bits |= (DWORD)(r == 0 ? 0 : r == h2 ? 1 : r == (WORD)(p16 >> 16) ? 2 : 3) << 10;
        if (r != 0 && r != h2 && r != (WORD)(p16 >> 16)) bits |= (DWORD)(r & 0xFF) << 24;
    }
    return bits;     /* low byte: a bit per documented answer; bits 8-11: the free codes */
}

BOOL WINAPI DllMainCRTStartup(HINSTANCE h, DWORD why, LPVOID r)
{ (void)h; (void)why; (void)r; return TRUE; }
