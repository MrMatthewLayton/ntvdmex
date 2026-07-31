/* dpmi.c -- see dpmi.h. The V86->PM switch via kernel-monitor reuse. */
#include "dpmi.h"
#include "ntvdm.h"
#include "v86.h"

/* LDT selector indices we hand the client. A ring-3 Win32 process has no LDT
   entries of its own, so starting at 1 is safe (index 0 would be selector 0x07). */
#define DPMI_IDX_CODE  1
#define DPMI_IDX_DATA  2

void dpmi_build_desc(DWORD base, DWORD limit, BYTE access, DWORD *lo, DWORD *hi)
{
    /* Standard x86 descriptor, byte-granular (G=0), 16-bit (D/B=0): limit fits in
       20 bits, base in 32. access = P|DPL|S|type. */
    *lo = (limit & 0xFFFF) | ((base & 0xFFFF) << 16);
    *hi = ((base >> 16) & 0xFF)
        | ((DWORD)access << 8)
        | (((limit >> 16) & 0xF) << 16)
        | (((base >> 24) & 0xFF) << 24);
}

int dpmi_switch_to_pm(volatile BYTE *tib, int client_is_32bit)
{
    WORD ss = (WORD)(VDM_REG(tib, VTIB_SS)  & 0xFFFF);
    WORD sp = (WORD)(VDM_REG(tib, VTIB_ESP) & 0xFFFF);
    WORD ds = (WORD)(VDM_REG(tib, VTIB_DS)  & 0xFFFF);
    DWORD stk = ((DWORD)ss << 4) + sp;      /* linear addr of the far-call frame  */
    /* 16-bit FAR CALL pushed IP then CS: [SP]=retIP, [SP+2]=retCS. */
    WORD ret_ip = *(volatile WORD *)stk;
    WORD ret_cs = *(volatile WORD *)(stk + 2);
    WORD new_sp = (WORD)(sp + 4);           /* pop the far-call return frame       */

    WORD code_sel = DPMI_SEL(DPMI_IDX_CODE);
    WORD data_sel = DPMI_SEL(DPMI_IDX_DATA);
    DWORD clo, chi, dlo, dhi;
    LONG st;
    /* 16-bit selectors, byte granular, limit 64K-1: code exec/read, data r/w, DPL3.
       (32-bit clients would set the D/B + G flags and a 4GB limit -- a later step.) */
    BYTE code_access = 0xFA, data_access = 0xF2;
    (void)client_is_32bit;

    dpmi_build_desc((DWORD)ret_cs << 4, 0xFFFF, code_access, &clo, &chi);
    dpmi_build_desc((DWORD)ds     << 4, 0xFFFF, data_access, &dlo, &dhi);

    /* Install both selectors (service 10 sets two per call). */
    st = v86_set_ldt_entries(code_sel, clo, chi, data_sel, dlo, dhi);
    if (st < 0) return -1;

    /* Rewrite the CONTEXT into protected mode: clear VM, load the LDT selectors,
       resume at the client's return address (same linear addr, now via a selector). */
    VDM_REG(tib, VTIB_EFLAGS) = VTIB_EFLAGS_PM;      /* VM clear -> PM              */
    VDM_SET16(tib, VTIB_CS,  code_sel);
    VDM_REG (tib, VTIB_EIP) = ret_ip;
    VDM_SET16(tib, VTIB_SS,  data_sel);
    VDM_REG (tib, VTIB_ESP) = new_sp;
    VDM_SET16(tib, VTIB_DS,  data_sel);
    VDM_SET16(tib, VTIB_ES,  data_sel);
    return 0;
}
