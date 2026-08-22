/* dpmi.c -- see dpmi.h. The V86->PM switch via kernel-monitor reuse. */
#include "dpmi.h"
#include "ntvdm.h"
#include "v86.h"

/* Diagnostic snapshot of the last switch (read by the host log): ret_cs, ret_ip,
   code descriptor lo, code descriptor hi. Localises base-0 faults (my descriptor
   vs the monitor not loading the LDT). */
DWORD g_dpmi_dbg[4] = {0,0,0,0};

/* Bases of the three initial selectors (code/data/stack); see dpmi.h. */
DWORD g_dpmi_seg_base[3] = {0,0,0};

/* The client's declared width from the mode-switch AX bit0 (1 = 32-bit, e.g. DOS/4GW).
   Recorded rather than acted on for the INITIAL selectors -- see the long note in
   dpmi_switch_to_pm. It is the right input for DPMI API register widths, not for D/B. */
int g_dpmi_client32 = 0;

/* LDT selector indices we hand the client. A ring-3 Win32 process has no LDT
   entries of its own, so starting at 1 is safe (index 0 would be selector 0x07). */
#define DPMI_IDX_CODE  1
#define DPMI_IDX_DATA  2
#define DPMI_IDX_STACK 3

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

    WORD code_sel  = DPMI_SEL(DPMI_IDX_CODE);
    WORD data_sel  = DPMI_SEL(DPMI_IDX_DATA);
    WORD stack_sel = DPMI_SEL(DPMI_IDX_STACK);
    DWORD clo, chi, dlo, dhi, slo, shi;
    DWORD code_base  = (DWORD)ret_cs << 4;           /* linear base of the guest CS  */
    DWORD data_base  = (DWORD)ds     << 4;           /* linear base of the guest DS  */
    DWORD stack_base = (DWORD)ss     << 4;           /* linear base of the guest SS  */
    DWORD lin_eip = code_base  + ret_ip;             /* linear code addr             */
    DWORD lin_esp = stack_base + new_sp;             /* linear stack addr            */
    LONG st;
    BYTE code_access = 0xFA, data_access = 0xF2;
    /* ── THE INITIAL SELECTORS ARE 16-BIT, EVEN FOR A 32-BIT CLIENT ──────────────
       Run 81 set D/B=1 here when the client passed AX bit0=1, reasoning that "its
       initial CS/DS/SS must be 32-bit so the code AFTER the far-call runs as 32-bit".
       That is WRONG, and it is what killed Doom (session 16).

       THE ARGUMENT, which needs no spec lookup: our mode-switch entry stub is
       `BOP 0x50 ; RETF`, and the RETF is taken when the switch FAILS -- returning to
       the client IN REAL MODE with CF=1. So the bytes immediately after the client's
       `call far` are executed as 16-bit real-mode code on the failure path. The same
       bytes cannot also be 32-bit code on the success path. Therefore the client's
       post-switch code is 16-bit, and its CS must be D/B=0.

       THE EVIDENCE. Doom (DOS/4GW) resumes at CS base 0x5ca0, offset 0x6e6a, bytes
       `72 81 fc 36 c7 06 c2 0a dc 71 36 8c 1e 30 0c`:
         as 16-bit: jb <err> / cld / mov word ss:[0xac2],0x71dc / mov word ss:[0xc30],ds
         as 32-bit: jb <err> / cld / mov DWORD ss:[esi],0x71dc0ac2   <-- wild write
                                     mov WORD  ss:[esi],ds          <-- wild write
                                     xor BYTE  [esp+ecx*4],cl       <-- wild write
       The 16-bit decode is a textbook post-switch stub: test CF, clear direction, save
       DS. The 32-bit decode is three wild writes through an uninitialised ESI within
       four instructions -- which is precisely the observed failure: the host dies
       inside the FIRST dpmi_enter_pm, with no output and no reflected exception.

       WHY OUR OWN TESTS DID NOT CATCH IT: pm32flat/pm32io/... put `bits 32` directly
       after `call far [entry]`, so they were written to match this implementation
       rather than to test it -- and, unlike a real client, they have no `jc` failure
       path, which is the very thing that forces real post-switch code to be 16-bit.
       Same lesson as the OPL work: our own instrument agreed with us.

       A 32-bit client reaches 32-bit code the way DOS/4GW does -- it allocates its own
       descriptors via INT 31h (0000/0008/0009) and far-jmps to them; dpmi_sel_is32()
       then reports 32-bit for THOSE selectors, which is correct. The client's declared
       width is still recorded (g_dpmi_client32) for DPMI API widths; it just must not
       decide the D/B of these based, 64K, real-mode-derived selectors. */
    BYTE dbflag = 0x0;
    g_dpmi_client32 = client_is_32bit ? 1 : 0;

    /* BASED, 64K selectors (G=0) -- the config that PROVED PM execution (run 28). XP's
       NtSetLdtEntries REJECTS a flat 4GB LDT descriptor (PspIsDescriptorValid: base +
       (G?(limit<<12)|0xFFF:limit) <= MmHighestUserAddress), so run-17's flat could never
       install. NB: a base-0 ~2GB descriptor (limit 0x7FFEF, G=1) ALSO installs (run 30) --
       that is the FLAT selector a real DOS/4GW extender allocates for itself via INT 31h
       (0000/0007/0008/0009), NOT the initial mode-switch selectors, which stay based/64K.
       A based selector maps the guest's real-mode segment (linear seg<<4, <1MB); resume
       EIP/ESP are the real-mode OFFSETS. */
    (void)lin_eip; (void)lin_esp;
    dpmi_build_desc(code_base,  0xFFFF, code_access, dbflag, &clo, &chi);
    dpmi_build_desc(data_base,  0xFFFF, data_access, dbflag, &dlo, &dhi);
    dpmi_build_desc(stack_base, 0xFFFF, data_access, dbflag, &slo, &shi);
    g_dpmi_dbg[0] = ret_cs; g_dpmi_dbg[1] = code_base + ret_ip; g_dpmi_dbg[2] = clo; g_dpmi_dbg[3] = chi;
    /* Publish the per-selector bases so the host can drive dpmi_sel_base() uniformly. */
    g_dpmi_seg_base[0] = code_base; g_dpmi_seg_base[1] = data_base; g_dpmi_seg_base[2] = stack_base;

    /* First REGISTER the LDT table (service 11) so the monitor loads LDTR -- without
       this, svc 10's descriptors resolve base 0 (VM run 2 diagnosis). Table covers
       indices 0..3: [0]=null, [1]=code (0x0F), [2]=data (0x17), [3]=stack (0x1F). A real
       .EXE client has CS!=DS!=SS, so the stack gets its OWN selector rather than reusing
       the data one (which for a .COM is identical -- CS=DS=SS=PSP). */
    {
        DWORD entries[8];
        entries[0] = 0;   entries[1] = 0;       /* index 0: null descriptor          */
        entries[2] = clo; entries[3] = chi;     /* index 1: code  (selector 0x0F)     */
        entries[4] = dlo; entries[5] = dhi;     /* index 2: data  (selector 0x17)     */
        entries[6] = slo; entries[7] = shi;     /* index 3: stack (selector 0x1F)     */
        st = v86_set_process_ldt_info(0, entries, 4);
        if (reg_st) *reg_st = st;
    }

    /* Then set the individual entries too (service 10; two selectors per call). */
    st = v86_set_ldt_entries(code_sel, clo, chi, data_sel, dlo, dhi);
    if (set_st) *set_st = st;
    if (st < 0) return -1;
    st = v86_set_ldt_entries(stack_sel, slo, shi, stack_sel, slo, shi);
    if (st < 0) return -1;

    /* Mark the client as in protected mode: set the virtual-MSW PE bit the monitor
       reads via getMSW (word[TIB+0x668]). Without this the monitor still treats the
       VDM as V86 and won't load our LDT (VM run 5: descriptor correct but base 0). */
    *(volatile WORD *)(tib + VTIB_MSW) |= MSW_PE_BIT;

    /* Rewrite the CONTEXT into protected mode: clear VM, load the BASED selectors,
       resume at the real-mode OFFSETS (the selectors carry the seg<<4 base). */
    /* ── VIF, NOT JUST IF, OR THE KERNEL WILL NEVER DELIVER AN INTERRUPT HERE. ──────
         The kernel's "can I deliver a hardware interrupt to this VDM right now?" test
         reads the VIRTUAL interrupt flag, not IF -- see the EFLAGS_VIF_BIT note in
         ntvdm.h, where exactly this cost the real-mode timer: "a guest started with
         IF=1 but VIF=0 therefore looks to the kernel like interrupts are disabled
         forever, so its interrupt-assist never delivers and it just sets VIP and
         defers". The V86 path learned that and has a knob for it (g_qi_vif); the
         protected-mode path was never given one and has always entered with VIF clear.
         That fits what the rig shows: with our own asynchronous injection disabled a PM
         guest receives ZERO timer ticks and spins for ever, while stock ntvdm runs the
         same client to Doom's title screen. If the kernel is willing to deliver to a PM
         VDM at all, VIF is the flag it asks about. */
    VDM_REG(tib, VTIB_EFLAGS) = VTIB_EFLAGS_PM | EFLAGS_VIF_BIT;  /* VM clear -> PM */
    VDM_SET16(tib, VTIB_CS,  code_sel);
    VDM_REG (tib, VTIB_EIP) = ret_ip;                /* offset within the based CS  */
    VDM_SET16(tib, VTIB_SS,  stack_sel);
    VDM_REG (tib, VTIB_ESP) = new_sp;                /* offset within the based SS  */
    VDM_SET16(tib, VTIB_DS,  data_sel);
    VDM_SET16(tib, VTIB_ES,  data_sel);
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
