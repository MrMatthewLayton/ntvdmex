/* dpmi.c -- see dpmi.h. The V86->PM switch via kernel-monitor reuse. */
#include "dpmi.h"
#include "ntvdm.h"
#include "v86.h"
#include "../ntvdmex_bits.h"
#include "../ntvdmex_x86.h"

/* Diagnostic snapshot of the last switch (read by the host log): ret_cs, ret_ip,
   code descriptor lo, code descriptor hi. Localises base-0 faults (my descriptor
   vs the monitor not loading the LDT). */
DWORD g_DpmiDebug[DPMI_DEBUG_ENTRIES] = {0,0,0,0};

/* Bases of the three initial selectors (code/data/stack); see dpmi.h. */
DWORD g_DpmiSegmentBase[DPMI_INITIAL_SELECTORS] = {0,0,0};

/* The client's declared width from the mode-switch AX bit0 (1 = 32-bit, e.g. DOS/4GW).
   Recorded rather than acted on for the INITIAL selectors -- see the long note in
   DpmiSwitchToProtectedMode. It is the right input for DPMI API register widths, not for D/B. */
INT g_DpmiIsClient32 = FALSE;

/* LDT selector indices we hand the client. A ring-3 Win32 process has no LDT
   entries of its own, so starting at 1 is safe (index 0 would be selector 0x07). */
#define DPMI_IDX_CODE  1
#define DPMI_IDX_DATA  2
#define DPMI_IDX_STACK 3

/* The x86 segment descriptor's fields (Intel SDM vol. 3, "Segment Descriptors"). */
#define DPMI_DESCRIPTOR_NIBBLE_MASK      0xF
#define DPMI_DESCRIPTOR_BASE_LOW_SHIFT   16    /* low dword: base 15-0 in bits 31-16     */
#define DPMI_DESCRIPTOR_BASE_MID_SHIFT   16    /* base 23-16 -> high dword bits 7-0      */
#define DPMI_DESCRIPTOR_ACCESS_SHIFT     8     /* access byte -> high dword bits 15-8    */
#define DPMI_DESCRIPTOR_LIMIT_HIGH_SHIFT 16    /* limit 19-16, in and out                */
#define DPMI_DESCRIPTOR_FLAGS_SHIFT      20    /* G|D/B|0|AVL -> high dword bits 23-20   */
#define DPMI_DESCRIPTOR_BASE_HIGH_SHIFT  24    /* base 31-24, in and out                 */

VOID DpmiBuildDescriptor(DWORD base, DWORD limit, BYTE access, BYTE flags,
                         DWORD *descriptorLow, DWORD *descriptorHigh)
{
    /* Standard x86 descriptor: limit in 20 bits, base in 32. access = P|DPL|S|type;
       flags nibble = G|D/B|0|AVL in bits 23..20 of the high dword. */
    *descriptorLow = (limit & WORD_MASK) | ((base & WORD_MASK) << DPMI_DESCRIPTOR_BASE_LOW_SHIFT);
    *descriptorHigh = ((base >> DPMI_DESCRIPTOR_BASE_MID_SHIFT) & BYTE_MASK)
        | ((DWORD)access << DPMI_DESCRIPTOR_ACCESS_SHIFT)
        | (((limit >> DPMI_DESCRIPTOR_LIMIT_HIGH_SHIFT) & DPMI_DESCRIPTOR_NIBBLE_MASK) << DPMI_DESCRIPTOR_LIMIT_HIGH_SHIFT)
        | (((DWORD)flags & DPMI_DESCRIPTOR_NIBBLE_MASK) << DPMI_DESCRIPTOR_FLAGS_SHIFT)
        | (((base >> DPMI_DESCRIPTOR_BASE_HIGH_SHIFT) & BYTE_MASK) << DPMI_DESCRIPTOR_BASE_HIGH_SHIFT);
}

#define DPMI_FAR_RETURN_CS_OFFSET    2         /* 16-bit FAR CALL frame: [SP]=IP, [SP+2]=CS */
#define DPMI_FAR_RETURN_FRAME_SIZE   4
#define DPMI_CODE_ACCESS             0xFA      /* present, DPL 3, code exec/read            */
#define DPMI_DATA_ACCESS             0xF2      /* present, DPL 3, data read/write           */
#define DPMI_FLAGS_16BIT_BYTE        0x0       /* G=0, D/B=0: 16-bit, byte-granular         */
#define DPMI_SEGMENT_LIMIT           0xFFFF    /* 64KB                                      */
#define DPMI_LDT_TABLE_ENTRIES       4         /* null, code, data, stack                    */
#define DPMI_DWORDS_PER_DESCRIPTOR   2
#define DPMI_DESCRIPTOR_HIGH         1         /* a descriptor's second dword               */
#define DPMI_IDX_NULL                0
#define DPMI_LDT_TABLE_DWORDS        (DPMI_DWORDS_PER_DESCRIPTOR * DPMI_LDT_TABLE_ENTRIES)
#define DPMI_LDT_FIRST_SELECTOR      0
#define DPMI_NULL_DESCRIPTOR         0
#define DPMI_SWITCH_OK               0
#define DPMI_SWITCH_FAILED           (-1)

INT DpmiSwitchToProtectedMode(volatile BYTE *tib, INT isClient32,
                              LONG *registerStatus, LONG *setStatus)
{
    WORD stackSegment = (WORD)(VDM_REG(tib, VTIB_SS)  & WORD_MASK);
    WORD stackPointer = (WORD)(VDM_REG(tib, VTIB_ESP) & WORD_MASK);
    WORD dataSegment = (WORD)(VDM_REG(tib, VTIB_DS)  & WORD_MASK);
    DWORD frameLinear = ((DWORD)stackSegment << PARAGRAPH_SHIFT) + stackPointer;      /* linear addr of the far-call frame  */
    /* 16-bit FAR CALL pushed IP then CS: [SP]=retIP, [SP+2]=retCS. */
    WORD returnOffset = *(volatile WORD *)frameLinear;
    WORD returnSegment = *(volatile WORD *)(frameLinear + DPMI_FAR_RETURN_CS_OFFSET);
    WORD newStackPointer = (WORD)(stackPointer + DPMI_FAR_RETURN_FRAME_SIZE);           /* pop the far-call return frame       */

    WORD codeSelector  = DPMI_SELECTOR(DPMI_IDX_CODE);
    WORD dataSelector  = DPMI_SELECTOR(DPMI_IDX_DATA);
    WORD stackSelector = DPMI_SELECTOR(DPMI_IDX_STACK);
    DWORD codeLow, codeHigh, dataLow, dataHigh, stackLow, stackHigh;
    DWORD codeBase  = (DWORD)returnSegment << PARAGRAPH_SHIFT;           /* linear base of the guest CS  */
    DWORD dataBase  = (DWORD)dataSegment     << PARAGRAPH_SHIFT;           /* linear base of the guest DS  */
    DWORD stackBase = (DWORD)stackSegment     << PARAGRAPH_SHIFT;           /* linear base of the guest SS  */
    DWORD linearEip = codeBase  + returnOffset;             /* linear code addr             */
    DWORD linearEsp = stackBase + newStackPointer;             /* linear stack addr            */
    LONG status;
    BYTE codeAccess = DPMI_CODE_ACCESS, dataAccess = DPMI_DATA_ACCESS;
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
       inside the FIRST DpmiEnterProtectedMode, with no output and no reflected exception.

       WHY OUR OWN TESTS DID NOT CATCH IT: pm32flat/pm32io/... put `bits 32` directly
       after `call far [entry]`, so they were written to match this implementation
       rather than to test it -- and, unlike a real client, they have no `jc` failure
       path, which is the very thing that forces real post-switch code to be 16-bit.
       Same lesson as the OPL work: our own instrument agreed with us.

       A 32-bit client reaches 32-bit code the way DOS/4GW does -- it allocates its own
       descriptors via INT 31h (0000/0008/0009) and far-jmps to them; dpmi_sel_is32()
       then reports 32-bit for THOSE selectors, which is correct. The client's declared
       width is still recorded (g_DpmiIsClient32) for DPMI API widths; it just must not
       decide the D/B of these based, 64K, real-mode-derived selectors. */
    BYTE descriptorFlags = DPMI_FLAGS_16BIT_BYTE;
    g_DpmiIsClient32 = isClient32 ? TRUE : FALSE;

    /* BASED, 64K selectors (G=0) -- the config that PROVED PM execution (run 28). XP's
       NtSetLdtEntries REJECTS a flat 4GB LDT descriptor (PspIsDescriptorValid: base +
       (G?(limit<<12)|0xFFF:limit) <= MmHighestUserAddress), so run-17's flat could never
       install. NB: a base-0 ~2GB descriptor (limit 0x7FFEF, G=1) ALSO installs (run 30) --
       that is the FLAT selector a real DOS/4GW extender allocates for itself via INT 31h
       (0000/0007/0008/0009), NOT the initial mode-switch selectors, which stay based/64K.
       A based selector maps the guest's real-mode segment (linear seg<<4, <1MB); resume
       EIP/ESP are the real-mode OFFSETS. */
    (VOID)linearEip; (VOID)linearEsp;
    DpmiBuildDescriptor(codeBase,  DPMI_SEGMENT_LIMIT, codeAccess, descriptorFlags, &codeLow, &codeHigh);
    DpmiBuildDescriptor(dataBase,  DPMI_SEGMENT_LIMIT, dataAccess, descriptorFlags, &dataLow, &dataHigh);
    DpmiBuildDescriptor(stackBase, DPMI_SEGMENT_LIMIT, dataAccess, descriptorFlags, &stackLow, &stackHigh);
    g_DpmiDebug[DPMI_DEBUG_RETURN_CS] = returnSegment; g_DpmiDebug[DPMI_DEBUG_RETURN_LINEAR] = codeBase + returnOffset; g_DpmiDebug[DPMI_DEBUG_CODE_LOW] = codeLow; g_DpmiDebug[DPMI_DEBUG_CODE_HIGH] = codeHigh;
    /* Publish the per-selector bases so the host can drive dpmi_sel_base() uniformly. */
    g_DpmiSegmentBase[DPMI_INITIAL_CODE] = codeBase; g_DpmiSegmentBase[DPMI_INITIAL_DATA] = dataBase; g_DpmiSegmentBase[DPMI_INITIAL_STACK] = stackBase;

    /* First REGISTER the LDT table (service 11) so the monitor loads LDTR -- without
       this, svc 10's descriptors resolve base 0 (VM run 2 diagnosis). Table covers
       indices 0..3: [0]=null, [1]=code (0x0F), [2]=data (0x17), [3]=stack (0x1F). A real
       .EXE client has CS!=DS!=SS, so the stack gets its OWN selector rather than reusing
       the data one (which for a .COM is identical -- CS=DS=SS=PSP). */
    {
        DWORD entries[DPMI_LDT_TABLE_DWORDS];
        entries[DPMI_DWORDS_PER_DESCRIPTOR * DPMI_IDX_NULL] = DPMI_NULL_DESCRIPTOR;   entries[DPMI_DWORDS_PER_DESCRIPTOR * DPMI_IDX_NULL + DPMI_DESCRIPTOR_HIGH] = DPMI_NULL_DESCRIPTOR;       /* index 0: null descriptor          */
        entries[DPMI_DWORDS_PER_DESCRIPTOR * DPMI_IDX_CODE] = codeLow; entries[DPMI_DWORDS_PER_DESCRIPTOR * DPMI_IDX_CODE + DPMI_DESCRIPTOR_HIGH] = codeHigh;     /* index 1: code  (selector 0x0F)     */
        entries[DPMI_DWORDS_PER_DESCRIPTOR * DPMI_IDX_DATA] = dataLow; entries[DPMI_DWORDS_PER_DESCRIPTOR * DPMI_IDX_DATA + DPMI_DESCRIPTOR_HIGH] = dataHigh;     /* index 2: data  (selector 0x17)     */
        entries[DPMI_DWORDS_PER_DESCRIPTOR * DPMI_IDX_STACK] = stackLow; entries[DPMI_DWORDS_PER_DESCRIPTOR * DPMI_IDX_STACK + DPMI_DESCRIPTOR_HIGH] = stackHigh;     /* index 3: stack (selector 0x1F)     */
        status = VdmRegisterLdtTable(DPMI_LDT_FIRST_SELECTOR, entries, DPMI_LDT_TABLE_ENTRIES);
        if (registerStatus) *registerStatus = status;
    }

    /* Then set the individual entries too (service 10; two selectors per call). */
    status = VdmInstallLdtEntries(codeSelector, codeLow, codeHigh, dataSelector, dataLow, dataHigh);
    if (setStatus) *setStatus = status;
    if (status < 0) return DPMI_SWITCH_FAILED;
    status = VdmInstallLdtEntries(stackSelector, stackLow, stackHigh, stackSelector, stackLow, stackHigh);
    if (status < 0) return DPMI_SWITCH_FAILED;

    /* Mark the client as in protected mode: set the virtual-MSW PE bit the monitor
       reads via getMSW (word[TIB+0x668]). Without this the monitor still treats the
       VDM as V86 and won't load our LDT (VM run 5: descriptor correct but base 0). */
    *(volatile WORD *)(tib + VTIB_MSW) |= MSW_PE_BIT;

    /* Rewrite the CONTEXT into protected mode: clear VM, load the BASED selectors,
       resume at the real-mode OFFSETS (the selectors carry the seg<<4 base). */
    /* ── VIF, NOT JUST IF, OR THE KERNEL WILL NEVER DELIVER AN INTERRUPT HERE. ──────
         The kernel's "can I deliver a hardware interrupt to this VDM right now?" test
         reads the VIRTUAL interrupt flag, not IF -- see the EFLAGS_VIF note in
         ntvdm.h, where exactly this cost the real-mode timer: "a guest started with
         IF=1 but VIF=0 therefore looks to the kernel like interrupts are disabled
         forever, so its interrupt-assist never delivers and it just sets VIP and
         defers". The V86 path learned that and has a knob for it (g_qi_vif); the
         protected-mode path was never given one and has always entered with VIF clear.
         That fits what the rig shows: with our own asynchronous injection disabled a PM
         guest receives ZERO timer ticks and spins for ever, while stock ntvdm runs the
         same client to Doom's title screen. If the kernel is willing to deliver to a PM
         VDM at all, VIF is the flag it asks about. */
    VDM_REG(tib, VTIB_EFLAGS) = VTIB_EFLAGS_PM | EFLAGS_VIF;  /* VM clear -> PM */
    VDM_SET16(tib, VTIB_CS,  codeSelector);
    VDM_REG (tib, VTIB_EIP) = returnOffset;                /* offset within the based CS  */
    VDM_SET16(tib, VTIB_SS,  stackSelector);
    VDM_REG (tib, VTIB_ESP) = newStackPointer;                /* offset within the based SS  */
    VDM_SET16(tib, VTIB_DS,  dataSelector);
    VDM_SET16(tib, VTIB_ES,  dataSelector);
    VDM_SET16(tib, VTIB_FS,  dataSelector);
    VDM_SET16(tib, VTIB_GS,  dataSelector);
    return DPMI_SWITCH_OK;
}

/* Run the guest in protected mode DIRECTLY in this host process, the way ntvdm does
   (0xf04483c iret's into the client -- PM is NOT run by the kernel monitor). We use
   NtContinue, the documented syscall that loads a full CONTEXT (incl. LDT selectors)
   and resumes at ring 3 -- the same effect as ntvdm's manual iretd. The guest runs
   in-process using the process LDT that svc 10/11 populated; its INT 31h / faults
   surface as Win32 exceptions the VEH catches. NtContinue does not return on success. */
typedef LONG (WINAPI *PFN_NtContinue)(CONTEXT *, BOOLEAN);

#define DPMI_NTDLL_NAME       "ntdll.dll"
#define DPMI_NT_CONTINUE      "NtContinue"

VOID DpmiRunProtectedMode(volatile BYTE *tib)
{
    static CONTEXT context;                    /* static: keep it off the (small) stack     */
    HMODULE ntdll = GetModuleHandleA(DPMI_NTDLL_NAME);
    PFN_NtContinue NtContinue = (PFN_NtContinue)GetProcAddress(ntdll, DPMI_NT_CONTINUE);
    BYTE *contextBytes = (BYTE *)&context; UINT byteIndex;
    for (byteIndex = 0; byteIndex < sizeof context; ++byteIndex) contextBytes[byteIndex] = 0;
    context.ContextFlags = VTIB_CTXFLAGS_VAL;  /* CONTEXT_CONTROL|INTEGER|SEGMENTS (0x10007) */
    context.SegGs = VDM_REG(tib, VTIB_GS) & WORD_MASK;  context.SegFs = VDM_REG(tib, VTIB_FS) & WORD_MASK;
    context.SegEs = VDM_REG(tib, VTIB_ES) & WORD_MASK;  context.SegDs = VDM_REG(tib, VTIB_DS) & WORD_MASK;
    context.SegCs = VDM_REG(tib, VTIB_CS) & WORD_MASK;  context.SegSs = VDM_REG(tib, VTIB_SS) & WORD_MASK;
    context.Edi = VDM_REG(tib, VTIB_EDI); context.Esi = VDM_REG(tib, VTIB_ESI);
    context.Ebx = VDM_REG(tib, VTIB_EBX); context.Edx = VDM_REG(tib, VTIB_EDX);
    context.Ecx = VDM_REG(tib, VTIB_ECX); context.Eax = VDM_REG(tib, VTIB_EAX);
    context.Ebp = VDM_REG(tib, VTIB_EBP); context.Eip = VDM_REG(tib, VTIB_EIP);
    context.Esp = VDM_REG(tib, VTIB_ESP); context.EFlags = VDM_REG(tib, VTIB_EFLAGS);
    if (NtContinue) NtContinue(&context, FALSE);
}
