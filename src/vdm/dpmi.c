/* dpmi.c -- see dpmi.h. The V86->PM switch via kernel-monitor reuse. */
#include "dpmi.h"
#include "ntvdm.h"
#include "v86.h"

/* Diagnostic snapshot of the last switch (read by the host log): ret_cs, ret_ip,
   code descriptor lo, code descriptor hi. Localises base-0 faults (my descriptor
   vs the monitor not loading the LDT). */
DWORD g_dpmi_dbg[4] = {0,0,0,0};

/* LDT selector indices we hand the client. A ring-3 Win32 process has no LDT
   entries of its own, so starting at 1 is safe (index 0 would be selector 0x07). */
#define DPMI_IDX_CODE  1
#define DPMI_IDX_DATA  2

void dpmi_build_desc(DWORD base, DWORD limit, BYTE access, BYTE flags,
                     DWORD *lo, DWORD *hi)
{
    /* Standard x86 descriptor: limit in 20 bits, base in 32. access = P|DPL|S|type;
       flags nibble = G|D/B|0|AVL in bits 23..20 of the high dword. */
    *lo = (limit & 0xFFFF) | ((base & 0xFFFF) << 16);
    *hi = ((base >> 16) & 0xFF)
        | ((DWORD)access << 8)
        | (((limit >> 16) & 0xF) << 16)
        | (((DWORD)flags & 0xF) << 20)
        | (((base >> 24) & 0xFF) << 24);
}

int dpmi_switch_to_pm(volatile BYTE *tib, int client_is_32bit,
                      LONG *reg_st, LONG *set_st)
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

    /* EXPERIMENT (3c): BOTH code and data/stack selectors 32-bit (flags=0x4, D/B=1).
       ReactOS shows PM VDM faults aren't kernel-reflected -- they go to user-mode
       exception delivery. A 16-bit CS (D=0) apparently doesn't dispatch cleanly to
       KiUserExceptionDispatcher (runs 10/12/13 died with no VEH); a 32-bit CS should.
       The client's post-switch code is size-independent (INT 31h; JMP $) so it decodes
       correctly under D=1. */
    dpmi_build_desc((DWORD)ret_cs << 4, 0xFFFF, code_access, 0x4, &clo, &chi);
    dpmi_build_desc((DWORD)ds     << 4, 0xFFFF, data_access, 0x4, &dlo, &dhi);
    g_dpmi_dbg[0] = ret_cs; g_dpmi_dbg[1] = ret_ip; g_dpmi_dbg[2] = clo; g_dpmi_dbg[3] = chi;

    /* First REGISTER the LDT table (service 11) so the monitor loads LDTR -- without
       this, svc 10's descriptors resolve base 0 (VM run 2 diagnosis). Table covers
       indices 0..2: [0]=null, [1]=code (0x0F), [2]=data (0x17). */
    {
        DWORD entries[6];
        entries[0] = 0; entries[1] = 0;         /* index 0: null descriptor         */
        entries[2] = clo; entries[3] = chi;     /* index 1: code (selector 0x0F)     */
        entries[4] = dlo; entries[5] = dhi;     /* index 2: data (selector 0x17)     */
        st = v86_set_process_ldt_info(0, entries, 3);
        if (reg_st) *reg_st = st;
    }

    /* Then set the individual entries too (service 10). */
    st = v86_set_ldt_entries(code_sel, clo, chi, data_sel, dlo, dhi);
    if (set_st) *set_st = st;
    if (st < 0) return -1;

    /* Mark the client as in protected mode: set the virtual-MSW PE bit the monitor
       reads via getMSW (word[TIB+0x668]). Without this the monitor still treats the
       VDM as V86 and won't load our LDT (VM run 5: descriptor correct but base 0). */
    *(volatile WORD *)(tib + VTIB_MSW) |= MSW_PE_BIT;

    /* Rewrite the CONTEXT into protected mode: clear VM, load the LDT selectors,
       resume at the client's return address (same linear addr, now via a selector). */
    VDM_REG(tib, VTIB_EFLAGS) = VTIB_EFLAGS_PM;      /* VM clear -> PM              */
    VDM_SET16(tib, VTIB_CS,  code_sel);
    VDM_REG (tib, VTIB_EIP) = ret_ip;
    VDM_SET16(tib, VTIB_SS,  data_sel);
    VDM_REG (tib, VTIB_ESP) = new_sp;
    VDM_SET16(tib, VTIB_DS,  data_sel);
    VDM_SET16(tib, VTIB_ES,  data_sel);
    /* FS/GS still hold the real-mode segment left by v86_set_entry -- a real-mode
       segment value is an invalid PM selector, so loading it on the PM context faults
       (and without PM fault-reflection set up, that crashes the host). Point them at
       the data selector too. */
    VDM_SET16(tib, VTIB_FS,  data_sel);
    VDM_SET16(tib, VTIB_GS,  data_sel);
    return 0;
}

/* Run the guest in protected mode DIRECTLY in this host process, the way ntvdm does
   (0xf04483c iret's into the client -- PM is NOT run by the kernel monitor). We use
   NtContinue, the documented syscall that loads a full CONTEXT (incl. LDT selectors)
   and resumes at ring 3 -- the same effect as ntvdm's manual iretd. The guest runs
   in-process using the process LDT that svc 10/11 populated; its INT 31h / faults
   surface as Win32 exceptions the VEH catches. NtContinue does not return on success. */
typedef LONG (WINAPI *PFN_NtContinue)(CONTEXT *, BOOLEAN);

void dpmi_run_pm(volatile BYTE *tib)
{
    static CONTEXT c;                    /* static: keep it off the (small) stack     */
    HMODULE ntdll = GetModuleHandleA("ntdll.dll");
    PFN_NtContinue NtContinue = (PFN_NtContinue)GetProcAddress(ntdll, "NtContinue");
    BYTE *z = (BYTE *)&c; unsigned i;
    for (i = 0; i < sizeof c; ++i) z[i] = 0;
    c.ContextFlags = VTIB_CTXFLAGS_VAL;  /* CONTEXT_CONTROL|INTEGER|SEGMENTS (0x10007) */
    c.SegGs = VDM_REG(tib, VTIB_GS) & 0xFFFF;  c.SegFs = VDM_REG(tib, VTIB_FS) & 0xFFFF;
    c.SegEs = VDM_REG(tib, VTIB_ES) & 0xFFFF;  c.SegDs = VDM_REG(tib, VTIB_DS) & 0xFFFF;
    c.SegCs = VDM_REG(tib, VTIB_CS) & 0xFFFF;  c.SegSs = VDM_REG(tib, VTIB_SS) & 0xFFFF;
    c.Edi = VDM_REG(tib, VTIB_EDI); c.Esi = VDM_REG(tib, VTIB_ESI);
    c.Ebx = VDM_REG(tib, VTIB_EBX); c.Edx = VDM_REG(tib, VTIB_EDX);
    c.Ecx = VDM_REG(tib, VTIB_ECX); c.Eax = VDM_REG(tib, VTIB_EAX);
    c.Ebp = VDM_REG(tib, VTIB_EBP); c.Eip = VDM_REG(tib, VTIB_EIP);
    c.Esp = VDM_REG(tib, VTIB_ESP); c.EFlags = VDM_REG(tib, VTIB_EFLAGS);
    if (NtContinue) NtContinue(&c, FALSE);
}
