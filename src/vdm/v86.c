/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * See v86.h. Faithful port of the V86/NtVdmControl glue from the
 * first spike (offsets and sequences confirmed at run time against
 * XP's ntvdm).
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#include "v86.h"

#define VDM_NTDLL_NAME              "ntdll.dll"
#define VDM_NT_CREATE_SECTION       "NtCreateSection"
#define VDM_NT_FREE_VIRTUAL_MEMORY  "NtFreeVirtualMemory"
#define VDM_NT_MAP_VIEW_OF_SECTION  "NtMapViewOfSection"
#define VDM_NT_VDM_CONTROL          "NtVdmControl"

#define VDM_CURRENT_PROCESS         ((HANDLE)-1)    /* NtCurrentProcess() */

/* Static ICA-state backing store the kernel probes/stores (zero-init); generous
 * sizes so every probe lands in valid writable memory.
 */
#define VDM_ICA_BUFFER_SIZE         256
#define VDM_ICA_BOP_TABLE_SIZE      1024

/* Our own VDM_TIB (the kernel never sets TEB->Vdm; ntvdm self-allocates one).
 * 0x674 bytes per ntvdm 0xf044374; rounded up, 16-aligned.
 */
#define VDM_TIB_BUFFER_SIZE         0x700
#define VDM_TIB_ALIGNMENT           16

/* The section and the four views of it (0xf00ea75). */
#define VDM_SECTION_ACCESS          0xA             /* SECTION_MAP_WRITE | SECTION_MAP_EXECUTE */
#define VDM_OBJECT_ATTRIBUTES_SIZE  0x18
#define VDM_SECTION_SIZE            0x100000        /* Conv + video aperture + UMA (EMS frame, M4) */
#define VDM_VIEW_UNMAP              2               /* SECTION_INHERIT ViewUnmap */
#define VDM_LOW_BASE                1               /* Page 0 cannot be named: base 1 rounds down */
#define VDM_LOW_RELEASE_SIZE        0x9FFFF
#define VDM_FIRST_VIEW_SIZE         0xFFFF
#define VDM_HMA_BASE                0x100000
#define VDM_SEGMENT_SIZE            0x10000         /* 64KB */
#define VDM_VIDEO_BASE              0xA0000
#define VDM_VIDEO_SIZE              0x20000         /* A0000-BFFFF */
#define VDM_CONVENTIONAL_REST_BASE  0x10000
#define VDM_CONVENTIONAL_REST_SIZE  0x90000
#define VDM_SECTION_START           0

/* Conventional 64KB-aligned EMS page-frame segments, in preference order. E000 is
 * tried last: post-init it typically has only 32KB free.
 */
#define VDM_EMS_FRAME_D000          0xD0000
#define VDM_EMS_FRAME_C000          0xC0000
#define VDM_EMS_FRAME_E000          0xE0000
#define VDM_NO_EMS_FRAME            0

/* A real PC's PICs: the master based at 08h, the slave at 70h, cascaded on line 2. */
#define VDM_ICA_MASTER_VECTOR_BASE  0x08            /* IRQ0-7 -> INT 08h..0Fh */
#define VDM_ICA_SLAVE_VECTOR_BASE   0x70            /* IRQ8-15 -> INT 70h..77h */
#define VDM_ICA_VECTOR_BASE_HIGH    0x00            /* The high byte of the vector-base word */
#define VDM_ICA_CASCADE_BIT         (1 << 2)        /* IRQ2 */
#define VDM_ICA_LINES_PER_PIC       8
#define VDM_ICA_LINE_MASK           7
#define VDM_ICA_SINGLE_DISPATCH     1

#define VDM_ICA_STATE_ISR_SHIFT     8
#define VDM_ICA_STATE_IMR_SHIFT     16

#define VDM_NO_NT_VDM_CONTROL       1               /* The status for "ntdll has no NtVdmControl" */

/* The VDM_TIB fields ntvdm initialises when it allocates one (0xf044374). Only Size's
 * meaning is recovered; the rest are set to ntvdm's values without a claim about what
 * they are. (0x3B and 0x23 are the values of XP's TEB and flat user-data selectors.)
 */
#define VTIB_SIZE_FIELD             0x000
#define VTIB_FIELD_098              0x098
#define VTIB_FIELD_098_VALUE        0
#define VTIB_FIELD_09C              0x09c
#define VTIB_FIELD_09C_VALUE        0x3b
#define VTIB_FIELD_0A0              0x0a0
#define VTIB_FIELD_0A4              0x0a4
#define VTIB_FIELD_0A0_VALUE        0x23            /* Both 0x0A0 and 0x0A4 */
#define VTIB_FIELD_66C              0x66c
#define VTIB_FIELD_66C_VALUE        0xffffffff
#define VTIB_FLAG_5E4               0x5e4
#define VTIB_FLAG_5E5               0x5e5
#define VTIB_FLAG_5E6               0x5e6
#define VTIB_FLAG_5E_VALUE          1               /* 0x5E4, 0x5E5 and 0x5E6 */
#define VTIB_FLAG_670               0x670
#define VTIB_FLAG_670_VALUE         0

#define VDM_ENTRY_REGISTER_ZERO     0

/* The NtSetLdtEntries block: two {selector, low, high} triples. */
#define VDM_LDT_ENTRIES_DWORDS      6
#define VDM_LDT_FIRST_SELECTOR      0
#define VDM_LDT_FIRST_LOW           1
#define VDM_LDT_FIRST_HIGH          2
#define VDM_LDT_SECOND_SELECTOR     3
#define VDM_LDT_SECOND_LOW          4
#define VDM_LDT_SECOND_HIGH         5

/* PROCESS_LDT_INFORMATION { ULONG Start; ULONG Length; LDT_ENTRY Entries[] }. */
#define VDM_LDT_INFO_START          0
#define VDM_LDT_INFO_LENGTH         1
#define VDM_LDT_INFO_HEADER_DWORDS  2
#define VDM_LDT_INFO_HEADER_BYTES   8               /* FIELD_OFFSET(Entries) */
#define VDM_LDT_MAX_DESCRIPTORS     64
#define VDM_LDT_DWORDS_PER_ENTRY    2
#define VDM_LDT_BYTES_PER_ENTRY     8
#define VDM_LDT_TABLE_TOO_LARGE     1               /* The status for a table past the buffer */
#define VDM_LDT_SERVICE_BUFFER      0               /* ServiceData = { &buffer, size } */
#define VDM_LDT_SERVICE_SIZE        1
#define VDM_LDT_SERVICE_DWORDS      2

static BYTE g_VdmTib[VDM_TIB_BUFFER_SIZE] __attribute__((aligned(VDM_TIB_ALIGNMENT)));

/* VDM "trap continue" handler = VDM_INITIALIZE_DATA.TrapcHandler. The kernel calls it
 * (ebx = VDM_TIB) to (re)enter the guest via a far return to the guest CS:EIP. ntvdm
 * passes 0xf044820 here; ours is the faithful port DpmiTrapContinue (src/vdm/dpmi_enter.S).
 * The old empty stub left the kernel's PM trap path unable to complete (DPMI spike).
 */
extern VOID DpmiTrapContinue(VOID);

static BYTE g_IcaLock[VDM_ICA_BUFFER_SIZE];
static BYTE g_IcaMaster[VDM_ICA_BUFFER_SIZE];
static BYTE g_IcaSlave[VDM_ICA_BUFFER_SIZE];
static BYTE g_IcaBopTable[VDM_ICA_BOP_TABLE_SIZE];
static DWORD g_IcaDelayIrq;
static DWORD g_IcaUndelayIrq;
static DWORD g_IcaDelayIret;
static DWORD g_IcaIretHooked;
static DWORD g_IcaNinth;
static VDMICAUSERDATA     g_IcaUserData;
static VDM_INITIALIZE_DATA g_InitializeData;

/* Cached NtVdmControl entry point (set by VdmRegisterWithKernel, used by VdmRunGuest). */
static PFN_NtVdmControl g_NtVdmControl;

/* The V86 section (0..0xFFFFF); kept so the EMS page frame can be mapped from it
 * AFTER VdmInitialize (see VdmMapEmsFrame).
 */
static HANDLE g_V86Section;

PVOID VdmGetTeb(VOID)
{
    PVOID teb;

    __asm__ volatile ("movl %%fs:0x18,%0" : "=r"(teb));
    return teb;
}

LONG VdmSetupMemory(VOID)
{
    HANDLE ntdll = GetModuleHandleA(VDM_NTDLL_NAME);

    PFN_NtCreateSection     NtCreateSection =
        (PFN_NtCreateSection)GetProcAddress(ntdll, VDM_NT_CREATE_SECTION);
    PFN_NtFreeVirtualMemory NtFreeVirtualMemory =
        (PFN_NtFreeVirtualMemory)GetProcAddress(ntdll, VDM_NT_FREE_VIRTUAL_MEMORY);
    PFN_NtMapViewOfSection  NtMapViewOfSection =
        (PFN_NtMapViewOfSection)GetProcAddress(ntdll, VDM_NT_MAP_VIEW_OF_SECTION);
    OBJ_ATTR objectAttributes;
    LARGE_INTEGER maximumSize;
    LARGE_INTEGER sectionOffset;
    PVOID baseAddress;
    SIZE_T viewSize;
    LONG status;
    UINT byteIndex;
    CHAR *attributeBytes = (CHAR *)&objectAttributes;

    for (byteIndex = 0; byteIndex < sizeof(objectAttributes); ++byteIndex)
        attributeBytes[byteIndex] = 0;

    objectAttributes.Length = VDM_OBJECT_ATTRIBUTES_SIZE;
    maximumSize.QuadPart = VDM_SECTION_SIZE;
    NtCreateSection(&g_V86Section, VDM_SECTION_ACCESS, &objectAttributes, &maximumSize, PAGE_EXECUTE_READWRITE,
                    SEC_RESERVE_NT, NULL);

    /* Release the default low reservations the loader made, then map the section
     * X-RW across the full 640KB in the FOUR pieces ntvdm uses (0xf00ea75).
     */
    baseAddress = (PVOID)VDM_LOW_BASE;
    viewSize = VDM_LOW_RELEASE_SIZE;
    NtFreeVirtualMemory(VDM_CURRENT_PROCESS, &baseAddress, &viewSize, MEM_RELEASE_NT);
    baseAddress = (PVOID)VDM_HMA_BASE;
    viewSize = VDM_SEGMENT_SIZE;
    NtFreeVirtualMemory(VDM_CURRENT_PROCESS, &baseAddress, &viewSize, MEM_RELEASE_NT);
    baseAddress = (PVOID)VDM_VIDEO_BASE;
    viewSize = VDM_VIDEO_SIZE;
    NtFreeVirtualMemory(VDM_CURRENT_PROCESS, &baseAddress, &viewSize, MEM_RELEASE_NT);

    baseAddress = (PVOID)VDM_LOW_BASE;
    viewSize = VDM_FIRST_VIEW_SIZE;
    sectionOffset.QuadPart = VDM_SECTION_START;
    NtMapViewOfSection(g_V86Section, VDM_CURRENT_PROCESS, &baseAddress, 0, VDM_FIRST_VIEW_SIZE, &sectionOffset, &viewSize, VDM_VIEW_UNMAP,
                       VDM_MAP_FLAG, PAGE_EXECUTE_READWRITE);
    baseAddress = (PVOID)VDM_HMA_BASE;
    viewSize = VDM_SEGMENT_SIZE;
    sectionOffset.QuadPart = VDM_SECTION_START;
    NtMapViewOfSection(g_V86Section, VDM_CURRENT_PROCESS, &baseAddress, 0, VDM_SEGMENT_SIZE, &sectionOffset, &viewSize, VDM_VIEW_UNMAP,
                       VDM_MAP_FLAG, PAGE_EXECUTE_READWRITE);
    /* Map 3 (the rest of conventional memory): section[0x10000..] -> linear 0x10000,
     * size 0x90000. Without it the guest faults the moment it touches >64KB.
     */
    baseAddress = (PVOID)VDM_CONVENTIONAL_REST_BASE;
    viewSize = VDM_CONVENTIONAL_REST_SIZE;
    sectionOffset.QuadPart = VDM_CONVENTIONAL_REST_BASE;
    status = NtMapViewOfSection(g_V86Section, VDM_CURRENT_PROCESS, &baseAddress, 0, VDM_CONVENTIONAL_REST_SIZE, &sectionOffset, &viewSize, VDM_VIEW_UNMAP,
                                VDM_MAP_FLAG, PAGE_EXECUTE_READWRITE);
    /* Map 4 -- the VGA aperture A0000-BFFFF (128KB) as RAM, so direct-framebuffer
     * writes (mode 13h at A0000, text at B8000) just land in memory and the video
     * VDD renders it each frame.
     */
    baseAddress = (PVOID)VDM_VIDEO_BASE;
    viewSize = VDM_VIDEO_SIZE;
    sectionOffset.QuadPart = VDM_VIDEO_BASE;
    NtMapViewOfSection(g_V86Section, VDM_CURRENT_PROCESS, &baseAddress, 0, VDM_VIDEO_SIZE, &sectionOffset, &viewSize, VDM_VIEW_UNMAP,
                       VDM_MAP_FLAG, PAGE_EXECUTE_READWRITE);
    /* NOTE: the EMS page frame at 0xE0000 is deliberately NOT mapped here. The
     * kernel VDM claims the C0000-FFFFF UMA/ROM range during VdmInitialize, so a
     * section view pre-mapped at 0xE0000 makes VdmInitialize fail with
     * STATUS_UNABLE_TO_FREE_VM (0xC000001A) -- diagnosed via the selftest VM gate.
     * The frame is mapped AFTER init by VdmMapEmsFrame(). (The A0000 VGA
     * aperture above is fine pre-init: the kernel leaves it for the display.)
     */
    return status;
}

/* Map the EMS 64KB page frame as RAM, from the V86 section -- called AFTER
 * VdmRegisterWithKernel()/VdmInitialize (see the NOTE in VdmSetupMemory). EMS shadows
 * 16KB logical pages into this window via memcpy, so the guest's direct frame accesses
 * are plain RAM (no trap). Post-init only part of the UMA is free (e.g. E0000 has
 * just 32KB), so we scan the conventional page-frame segments for a free 64KB hole
 * and map there. Returns the linear base of the mapped frame (0 if none found).
 */
DWORD VdmMapEmsFrame(VOID)
{
    HANDLE ntdll = GetModuleHandleA(VDM_NTDLL_NAME);

    PFN_NtMapViewOfSection NtMapViewOfSection =
        (PFN_NtMapViewOfSection)GetProcAddress(ntdll, VDM_NT_MAP_VIEW_OF_SECTION);
    static const DWORD candidateFrames[] = { VDM_EMS_FRAME_D000, VDM_EMS_FRAME_C000, VDM_EMS_FRAME_E000 };
    UINT candidateIndex;

    if (!g_V86Section)
        return VDM_NO_EMS_FRAME;

    for (candidateIndex = 0; candidateIndex < sizeof(candidateFrames) / sizeof(candidateFrames[0]); ++candidateIndex)
    {
        MEMORY_BASIC_INFORMATION memoryInfo;
        LARGE_INTEGER sectionOffset;
        PVOID baseAddress;
        SIZE_T viewSize;
        LONG status;

        if (!VirtualQuery((LPCVOID)candidateFrames[candidateIndex], &memoryInfo, sizeof(memoryInfo)))
            continue;

        if (memoryInfo.State != MEM_FREE || memoryInfo.RegionSize < VDM_SEGMENT_SIZE)
            continue;                                                                            /* need 64KB free */

        baseAddress = (PVOID)candidateFrames[candidateIndex];
        viewSize = VDM_SEGMENT_SIZE;
        sectionOffset.QuadPart = candidateFrames[candidateIndex];
        status = NtMapViewOfSection(g_V86Section, VDM_CURRENT_PROCESS, &baseAddress, 0, VDM_SEGMENT_SIZE, &sectionOffset, &viewSize, VDM_VIEW_UNMAP,
                                    VDM_MAP_FLAG, PAGE_EXECUTE_READWRITE);

        if (status >= 0)
            return candidateFrames[candidateIndex];                /* mapped: linear base of the 64KB frame */
    }

    return VDM_NO_EMS_FRAME;
}

/* Program the virtual PIC the kernel emulates for us. Until now these buffers were
 * handed over zeroed -- "generous sizes so every probe lands in valid memory" -- which
 * means the kernel could never dispatch anything: vector base 0, no lines requested.
 * A real PC's master PIC is based at 0x08 (IRQ0 -> INT 08h) with the slave at 0x70 and
 * cascaded on line 2, so program exactly that. IMR = 0 (nothing masked) matches how DOS
 * leaves the PIC once a game has enabled its device's line; a guest that masks lines
 * does it through the PIC VDD, which mirrors into ICA_IMR.
 */
static VOID VdmIcaProgram(VOID)
{
    g_IcaMaster[ICA_BASE]       = VDM_ICA_MASTER_VECTOR_BASE;
    g_IcaMaster[ICA_BASE + 1]   = VDM_ICA_VECTOR_BASE_HIGH;
    g_IcaMaster[ICA_SLAVE_MASK] = VDM_ICA_CASCADE_BIT;
    g_IcaSlave[ICA_BASE]        = VDM_ICA_SLAVE_VECTOR_BASE;
    g_IcaSlave[ICA_BASE + 1]    = VDM_ICA_VECTOR_BASE_HIGH;
}

/* EXPERIMENT ONLY: retarget the master PIC's vector base. With the faithful base (0x08)
 * an IRQ 5 the KERNEL dispatches and one the HOST injects both arrive as INT 0Dh, so the
 * guest cannot tell which of us delivered it. Moving the kernel's base makes the two
 * distinguishable by vector, which is the whole point of the qirq2 probe.
 */
VOID VdmIcaSetBase(UINT vectorBase)
{
    g_IcaMaster[ICA_BASE]     = (BYTE)vectorBase;
    g_IcaMaster[ICA_BASE + 1] = VDM_ICA_VECTOR_BASE_HIGH;
}

VOID VdmIcaRaise(UINT irq)
{
    BYTE *ica = (irq < VDM_ICA_LINES_PER_PIC) ? g_IcaMaster : g_IcaSlave;
    UINT line = irq & VDM_ICA_LINE_MASK;

    /* One pending dispatch for this line. The kernel decrements the count and clears
     * the request bit when it drains, so this is a single edge, not a level.
     */
    *(volatile DWORD *)(ica + ICA_COUNT(line)) = VDM_ICA_SINGLE_DISPATCH;
    ica[ICA_IRR] |= (BYTE)(1u << line);

    if (irq >= VDM_ICA_LINES_PER_PIC)
        g_IcaMaster[ICA_IRR] |= VDM_ICA_CASCADE_BIT;                                 /* cascade through IRQ2 */
}

VOID VdmIcaEndOfInterrupt(UINT irq)
{
    BYTE *ica = (irq < VDM_ICA_LINES_PER_PIC) ? g_IcaMaster : g_IcaSlave;

    ica[ICA_ISR] &= (BYTE)~(1u << (irq & VDM_ICA_LINE_MASK));

    if (irq >= VDM_ICA_LINES_PER_PIC)
        g_IcaMaster[ICA_ISR] &= (BYTE)~VDM_ICA_CASCADE_BIT;
}

VOID VdmIcaSetMask(UINT irq, INT isMasked)
{
    BYTE *ica = (irq < VDM_ICA_LINES_PER_PIC) ? g_IcaMaster : g_IcaSlave;
    BYTE lineBit = (BYTE)(1u << (irq & VDM_ICA_LINE_MASK));

    if (isMasked)
        ica[ICA_IMR] |= lineBit;
    else
        ica[ICA_IMR] &= (BYTE)~lineBit;
}

DWORD VdmIcaGetState(UINT irq)
{
    const BYTE *ica = (irq < VDM_ICA_LINES_PER_PIC) ? g_IcaMaster : g_IcaSlave;

    return ((DWORD)ica[ICA_IRR]) | ((DWORD)ica[ICA_ISR] << VDM_ICA_STATE_ISR_SHIFT) |
           ((DWORD)ica[ICA_IMR] << VDM_ICA_STATE_IMR_SHIFT);
}

LONG VdmRegisterWithKernel(VOID)
{
    VdmIcaProgram();
    g_IcaUserData.pIcaLock = g_IcaLock;
    g_IcaUserData.pIcaMaster = g_IcaMaster;
    g_IcaUserData.pIcaSlave = g_IcaSlave;
    g_IcaUserData.pDelayIrq = &g_IcaDelayIrq;
    g_IcaUserData.pUndelayIrq = &g_IcaUndelayIrq;
    g_IcaUserData.pDelayIret = &g_IcaDelayIret;
    g_IcaUserData.pIretHooked = &g_IcaIretHooked;
    g_IcaUserData.pAddrIretBopTable = g_IcaBopTable;
    g_IcaUserData.p9 = &g_IcaNinth;
    g_InitializeData.TrapcHandler = (PVOID)&DpmiTrapContinue;
    g_InitializeData.IcaUserData  = &g_IcaUserData;

    g_NtVdmControl = (PFN_NtVdmControl)GetProcAddress(
                     GetModuleHandleA(VDM_NTDLL_NAME), VDM_NT_VDM_CONTROL);

    if (!g_NtVdmControl)
        return VDM_NO_NT_VDM_CONTROL;

    return g_NtVdmControl(VDM_SVC_VdmInitialize, &g_InitializeData);
}

volatile BYTE *VdmGetTib(VOID)
{
    BYTE *teb = (BYTE *)VdmGetTeb();
    BYTE *tib;

    if (!teb)
        return NULL;

    tib = *(BYTE **)(teb + TEB_VDM_TIB);

    if (!tib)
    {
        /* ntvdm allocates the VDM_TIB itself, registers TEB[0xF18], then inits it
         * (0xf044374). Replicate minimally into our static buffer.
         */
        tib = g_VdmTib;
        *(DWORD *)(tib + VTIB_SIZE_FIELD) = VTIB_SIZE_VALUE;   /* VDM_TIB.Size */
        *(DWORD *)(tib + VTIB_FIELD_098) = VTIB_FIELD_098_VALUE;
        *(DWORD *)(tib + VTIB_FIELD_09C) = VTIB_FIELD_09C_VALUE;
        *(DWORD *)(tib + VTIB_FIELD_0A0) = VTIB_FIELD_0A0_VALUE;
        *(DWORD *)(tib + VTIB_FIELD_0A4) = VTIB_FIELD_0A0_VALUE;
        *(DWORD *)(tib + VTIB_FIELD_66C) = VTIB_FIELD_66C_VALUE;
        tib[VTIB_FLAG_5E4] = VTIB_FLAG_5E_VALUE;
        tib[VTIB_FLAG_5E5] = VTIB_FLAG_5E_VALUE;
        tib[VTIB_FLAG_5E6] = VTIB_FLAG_5E_VALUE;
        tib[VTIB_FLAG_670] = VTIB_FLAG_670_VALUE;
        *(BYTE **)(teb + TEB_VDM_TIB) = tib;         /* register with our TEB */
    }

    return tib;
}

VOID VdmSetEntry(
    volatile BYTE *tib,
    WORD codeSegment,
    WORD instructionPointer,
    WORD stackSegment,
    WORD stackPointer,
    WORD pspSegment)
{
    VDM_REG(tib, VTIB_CONTEXT) = VTIB_CTXFLAGS_VAL;
    VDM_REG(tib, VTIB_GS) = pspSegment;
    VDM_REG(tib, VTIB_FS) = pspSegment;
    VDM_REG(tib, VTIB_ES) = pspSegment;
    VDM_REG(tib, VTIB_DS) = pspSegment;
    VDM_REG(tib, VTIB_EDI) = VDM_ENTRY_REGISTER_ZERO;
    VDM_REG(tib, VTIB_ESI) = VDM_ENTRY_REGISTER_ZERO;
    VDM_REG(tib, VTIB_EBX) = VDM_ENTRY_REGISTER_ZERO;
    VDM_REG(tib, VTIB_EDX) = VDM_ENTRY_REGISTER_ZERO;
    VDM_REG(tib, VTIB_ECX) = VDM_ENTRY_REGISTER_ZERO;
    VDM_REG(tib, VTIB_EAX) = VDM_ENTRY_REGISTER_ZERO;
    VDM_REG(tib, VTIB_EBP) = VDM_ENTRY_REGISTER_ZERO;
    VDM_REG(tib, VTIB_EIP) = instructionPointer;
    VDM_REG(tib, VTIB_CS)  = codeSegment;
    VDM_REG(tib, VTIB_EFLAGS) = VTIB_EFLAGS_V86;
    VDM_REG(tib, VTIB_ESP) = stackPointer;
    VDM_REG(tib, VTIB_SS)  = stackSegment;
}

DWORD VdmRunGuest(volatile BYTE *tib, LONG *status)
{
    LONG controlStatus = g_NtVdmControl(VDM_SVC_VdmStartExecution, NULL);

    if (status)
        *status = controlStatus;

    return (DWORD)VDM_REG(tib, VTIB_EVENT);
}

LONG VdmControl(ULONG service, PVOID serviceData)
{
    if (!g_NtVdmControl)
        return VDM_NO_NT_VDM_CONTROL;

    return g_NtVdmControl(service, serviceData);
}

LONG VdmInstallLdtEntries(
    WORD firstSelector,
    DWORD firstLow,
    DWORD firstHigh,
    WORD secondSelector,
    DWORD secondLow,
    DWORD secondHigh)
{
    /* NtSetLdtEntries 6-dword block; see v86.h + fcn.0f050100 in the research. */
    DWORD serviceData[VDM_LDT_ENTRIES_DWORDS];

    serviceData[VDM_LDT_FIRST_SELECTOR] = firstSelector;
    serviceData[VDM_LDT_FIRST_LOW] = firstLow;
    serviceData[VDM_LDT_FIRST_HIGH] = firstHigh;
    serviceData[VDM_LDT_SECOND_SELECTOR] = secondSelector;
    serviceData[VDM_LDT_SECOND_LOW] = secondLow;
    serviceData[VDM_LDT_SECOND_HIGH] = secondHigh;
    return VdmControl(VDM_SVC_VdmSetLdtEntries, serviceData);
}

LONG VdmRegisterLdtTable(WORD startSelector, const DWORD *entries, INT count)
{
    /* buffer = { StartSel, LengthBytes, entries[count] }; ServiceData = { &buffer, count }
     * (recovered from SetShadowDescriptorEntries' svc-11 branch, fcn.0f0500c9).
     */
    static DWORD ldtInformation[VDM_LDT_INFO_HEADER_DWORDS + VDM_LDT_DWORDS_PER_ENTRY * VDM_LDT_MAX_DESCRIPTORS];
    DWORD serviceData[VDM_LDT_SERVICE_DWORDS];
    INT dwordIndex;
    INT entryDwords = count * VDM_LDT_DWORDS_PER_ENTRY;

    if (entryDwords > (INT)(sizeof(ldtInformation)/sizeof(ldtInformation[0])) - VDM_LDT_INFO_HEADER_DWORDS)
        return VDM_LDT_TABLE_TOO_LARGE;

    /* PROCESS_LDT_INFORMATION { ULONG Start; ULONG Length; LDT_ENTRY Entries[] }.
     * Start = byte offset into the LDT; Length = byte count of the entries.
     */
    ldtInformation[VDM_LDT_INFO_START] = startSelector;
    ldtInformation[VDM_LDT_INFO_LENGTH] = (DWORD)(count * VDM_LDT_BYTES_PER_ENTRY);        /* Length: bytes of descriptor entries */

    for (dwordIndex = 0; dwordIndex < entryDwords; ++dwordIndex)
        ldtInformation[VDM_LDT_INFO_HEADER_DWORDS + dwordIndex] = entries[dwordIndex];

    serviceData[VDM_LDT_SERVICE_BUFFER] = (DWORD)(ULONG_PTR)ldtInformation;
    /* NtSetInformationProcess(ProcessLdtInformation) wants the TOTAL byte size:
     * FIELD_OFFSET(Entries)=8 + Length. Passing the count gave INFO_LENGTH_MISMATCH
     * (0xC0000004) in VM run 3.
     */
    serviceData[VDM_LDT_SERVICE_SIZE] = (DWORD)(VDM_LDT_INFO_HEADER_BYTES + count * VDM_LDT_BYTES_PER_ENTRY);
    return VdmControl(VDM_SVC_VdmSetProcessLdtInfo, serviceData);
}
