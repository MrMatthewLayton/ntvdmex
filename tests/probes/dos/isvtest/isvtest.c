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

#define VDM_V86 0                  /* vddsvc.h's VDM_MODE: a real-mode (V86) address */

#define ISV_PORT_FIRST       0x2F0
#define ISV_PORT_LAST        0x2F1
#define ISV_PORT_RANGE_COUNT 1
#define ISV_IN_PATTERN       0x5A   /* IN answers this XOR the port's low byte */
#define ISV_LOW_BYTE         0xFF
#define ISV_WORD_MASK        0xFFFF
#define ISV_FUNCTION_COMPLEMENT 1   /* DX=1: CX = BX XOR FFFFh             */
#define ISV_FUNCTION_LATCH      2   /* DX=2: CX = the last byte OUT        */
#define ISV_FUNCTION_SUM        3   /* DX=3: CX = the sum of CX bytes at DS:SI */

static HANDLE g_IsvModule;
static BYTE   g_IsvLatch;

static VOID WINAPI IsvPortInByte(WORD port, BYTE *data) { *data = (BYTE)(ISV_IN_PATTERN ^ (port & ISV_LOW_BYTE)); }
static VOID WINAPI IsvPortOutByte(WORD port, BYTE value) { if (port == ISV_PORT_FIRST) g_IsvLatch = value; }

__declspec(dllexport) VOID IsvInit(VOID)
{
    VDD_IO_PORTRANGE range = { ISV_PORT_FIRST, ISV_PORT_LAST };
    VDD_IO_HANDLERS  handlers = { IsvPortInByte, NULL, NULL, NULL, IsvPortOutByte, NULL, NULL, NULL };
    setCF(VDDInstallIOHook(g_IsvModule, ISV_PORT_RANGE_COUNT, &range, &handlers) ? 0 : 1);
}

__declspec(dllexport) VOID IsvDispatch(VOID)
{
    switch (getDX()) {
    case ISV_FUNCTION_COMPLEMENT: setCX((USHORT)(getBX() ^ ISV_WORD_MASK)); setCF(0); break;
    case ISV_FUNCTION_LATCH: setCX(g_IsvLatch); setCF(0); break;
    case ISV_FUNCTION_SUM: {
        BYTE *bytes = (BYTE *)VdmMapFlat(getDS(), getSI(), VDM_V86);
        USHORT count = getCX(), sum = 0, index;
        for (index = 0; bytes && index < count; ++index) sum = (USHORT)(sum + bytes[index]);
        setCX(sum); setCF(bytes ? 0 : 1); break; }
    default: setCF(1); break;
    }
}

BOOL WINAPI DllMainCRTStartup(HINSTANCE module, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) g_IsvModule = module;
    return TRUE;
}
