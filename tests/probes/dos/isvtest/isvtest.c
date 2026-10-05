/*
 * isvtest.c -- ISVTEST.DLL, a VDD written to MICROSOFT's ABI (s91, #11).
 *
 * Built against nothing of ours: the declarations below are nt_vdd.h / vddsvc.h's,
 * and it links NTVDM.EXE's exports through an import library (ntvdm.def), as a DDK
 * VDD links ntvdm.lib. The same DLL is loaded by stock NTVDM and by NTVDMEX through
 * tests/probes/dos/p_isv.com's RegisterModule, so its answers compare the two VDD hosts.
 *
 *   init      hooks ports 2F0h-2F1h: IN returns 5Ah XOR the port's low byte, OUT latches
 *   dispatch  DX=1: CX = BX XOR FFFFh      DX=2: CX = the last byte OUT to 2F0h
 *             DX=3: CX = the sum of the CX bytes at DS:SI (VdmMapFlat, V86)
 *             else: CF set
 */
#include <windows.h>

/* ⚠ STDCALL. nt_vdd.h declares these with no convention, but NT and the DDK build
     environment compile with __stdcall as the default (/Gz), so that is what NTVDM
     calls. s91's first cut used cdecl: stock NTVDM died at the first IN to the hooked
     port (its call popped 8 bytes the handler had not), while ours -- calling cdecl
     too -- happened to agree with the mistake. */
typedef VOID (WINAPI *PFNVDD_INB)(WORD, BYTE *);   typedef VOID (WINAPI *PFNVDD_INW)(WORD, WORD *);
typedef VOID (WINAPI *PFNVDD_OUTB)(WORD, BYTE);    typedef VOID (WINAPI *PFNVDD_OUTW)(WORD, WORD);
typedef struct { PFNVDD_INB inb; PFNVDD_INW inw; PVOID insb, insw;
                 PFNVDD_OUTB outb; PFNVDD_OUTW outw; PVOID outsb, outsw; } VDD_IO_HANDLERS;
typedef struct { WORD First, Last; } VDD_IO_PORTRANGE;

USHORT WINAPI getAX(VOID); VOID WINAPI setAX(USHORT);
USHORT WINAPI getBX(VOID); USHORT WINAPI getCX(VOID); VOID WINAPI setCX(USHORT);
USHORT WINAPI getDX(VOID); USHORT WINAPI getSI(VOID); USHORT WINAPI getDS(VOID);
VOID WINAPI setCF(ULONG);
BOOL WINAPI VDDInstallIOHook(HANDLE, WORD, VDD_IO_PORTRANGE *, VDD_IO_HANDLERS *);
VOID WINAPI VDDDeInstallIOHook(HANDLE, WORD, VDD_IO_PORTRANGE *);
PVOID WINAPI VdmMapFlat(USHORT, ULONG, ULONG);

static HANDLE g_self;
static BYTE   g_latch;

static VOID WINAPI io_inb(WORD port, BYTE *d) { *d = (BYTE)(0x5A ^ (port & 0xFF)); }
static VOID WINAPI io_outb(WORD port, BYTE v) { if (port == 0x2F0) g_latch = v; }

__declspec(dllexport) VOID IsvInit(VOID)
{
    VDD_IO_PORTRANGE r = { 0x2F0, 0x2F1 };
    VDD_IO_HANDLERS  h = { io_inb, NULL, NULL, NULL, io_outb, NULL, NULL, NULL };
    setCF(VDDInstallIOHook(g_self, 1, &r, &h) ? 0 : 1);
}

__declspec(dllexport) VOID IsvDispatch(VOID)
{
    switch (getDX()) {
    case 1: setCX((USHORT)(getBX() ^ 0xFFFF)); setCF(0); break;
    case 2: setCX(g_latch); setCF(0); break;
    case 3: {
        BYTE *p = (BYTE *)VdmMapFlat(getDS(), getSI(), 0);
        USHORT n = getCX(), sum = 0, i;
        for (i = 0; p && i < n; ++i) sum = (USHORT)(sum + p[i]);
        setCX(sum); setCF(p ? 0 : 1); break; }
    default: setCF(1); break;
    }
}

BOOL WINAPI DllMainCRTStartup(HINSTANCE h, DWORD why, LPVOID r)
{
    (void)r;
    if (why == DLL_PROCESS_ATTACH) g_self = h;
    return TRUE;
}
