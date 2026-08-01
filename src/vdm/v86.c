/* v86.c -- see v86.h. Faithful port of the V86/NtVdmControl glue from the
 * tools/vdmhost spike (offsets/sequences recovered from XP ntvdm; see
 * docs/research/ntvdmcontrol-and-v86.md). */
#include "v86.h"

/* Static ICA-state backing store the kernel probes/stores (zero-init); generous
   sizes so every probe lands in valid writable memory. */
static BYTE  g_ica_lock[256], g_ica_master[256], g_ica_slave[256], g_ica_bop[1024];
static DWORD g_delayirq, g_undelayirq, g_delayiret, g_irethooked, g_p9;
static VDMICAUSERDATA     g_ica;
static VDM_INITIALIZE_DATA g_initdata;

/* Our own VDM_TIB (the kernel never sets TEB->Vdm; ntvdm self-allocates one).
   0x674 bytes per ntvdm 0xf044374; rounded up, 16-aligned. */
static BYTE g_vdmtib[0x700] __attribute__((aligned(16)));

/* Cached NtVdmControl entry point (set by v86_init, used by v86_run). */
static PFN_NtVdmControl s_ntvdmctl;

/* The V86 section (0..0xFFFFF); kept so the EMS page frame can be mapped from it
   AFTER VdmInitialize (see v86_map_ems_frame). */
static HANDLE s_hSec;

/* VDM "trap continue" handler = VDM_INITIALIZE_DATA.TrapcHandler. The kernel calls it
   (ebx = VDM_TIB) to (re)enter the guest via a far return to the guest CS:EIP. ntvdm
   passes 0xf044820 here; ours is the faithful port dpmi_trapc (src/vdm/dpmi_enter.S).
   The old empty stub left the kernel's PM trap path unable to complete (DPMI spike). */
extern void dpmi_trapc(void);

void *v86_teb(void)
{
    void *t;
    __asm__ volatile ("movl %%fs:0x18,%0" : "=r"(t));
    return t;
}

LONG v86_setup_memory(void)
{
    HANDLE hntdll = GetModuleHandleA("ntdll.dll");
    PFN_NtCreateSection     NtCreateSection =
        (PFN_NtCreateSection)GetProcAddress(hntdll, "NtCreateSection");
    PFN_NtFreeVirtualMemory NtFreeVirtualMemory =
        (PFN_NtFreeVirtualMemory)GetProcAddress(hntdll, "NtFreeVirtualMemory");
    PFN_NtMapViewOfSection  NtMapViewOfSection =
        (PFN_NtMapViewOfSection)GetProcAddress(hntdll, "NtMapViewOfSection");
    OBJ_ATTR oa;
    LARGE_INTEGER maxsize, off;
    PVOID base; SIZE_T sz; LONG st;
    unsigned i; char *q = (char *)&oa;

    for (i = 0; i < sizeof(oa); ++i) q[i] = 0;
    oa.Length = 0x18;
    maxsize.QuadPart = 0x100000;       /* conv + video aperture + UMA (EMS frame, M4) */
    NtCreateSection(&s_hSec, 0xA, &oa, &maxsize, PAGE_EXECUTE_READWRITE,
                    SEC_RESERVE_NT, NULL);

    /* Release the default low reservations the loader made, then map the section
       X-RW across the full 640KB in the FOUR pieces ntvdm uses (0xf00ea75). */
    base = (PVOID)1;        sz = 0x9FFFF; NtFreeVirtualMemory((HANDLE)-1, &base, &sz, MEM_RELEASE_NT);
    base = (PVOID)0x100000; sz = 0x10000; NtFreeVirtualMemory((HANDLE)-1, &base, &sz, MEM_RELEASE_NT);
    base = (PVOID)0xA0000;  sz = 0x20000; NtFreeVirtualMemory((HANDLE)-1, &base, &sz, MEM_RELEASE_NT);

    base = (PVOID)1;        sz = 0xFFFF;  off.QuadPart = 0;
    NtMapViewOfSection(s_hSec, (HANDLE)-1, &base, 0, 0xFFFF, &off, &sz, 2,
                       VDM_MAP_FLAG, PAGE_EXECUTE_READWRITE);
    base = (PVOID)0x100000; sz = 0x10000; off.QuadPart = 0;
    NtMapViewOfSection(s_hSec, (HANDLE)-1, &base, 0, 0x10000, &off, &sz, 2,
                       VDM_MAP_FLAG, PAGE_EXECUTE_READWRITE);
    /* Map 3 (the rest of conventional memory): section[0x10000..] -> linear 0x10000,
       size 0x90000. Without it the guest faults the moment it touches >64KB. */
    base = (PVOID)0x10000;  sz = 0x90000; off.QuadPart = 0x10000;
    st = NtMapViewOfSection(s_hSec, (HANDLE)-1, &base, 0, 0x90000, &off, &sz, 2,
                            VDM_MAP_FLAG, PAGE_EXECUTE_READWRITE);
    /* Map 4 -- the VGA aperture A0000-BFFFF (128KB) as RAM, so direct-framebuffer
       writes (mode 13h at A0000, text at B8000) just land in memory and the video
       VDD renders it each frame. */
    base = (PVOID)0xA0000;  sz = 0x20000; off.QuadPart = 0xA0000;
    NtMapViewOfSection(s_hSec, (HANDLE)-1, &base, 0, 0x20000, &off, &sz, 2,
                       VDM_MAP_FLAG, PAGE_EXECUTE_READWRITE);
    /* NOTE: the EMS page frame at 0xE0000 is deliberately NOT mapped here. The
       kernel VDM claims the C0000-FFFFF UMA/ROM range during VdmInitialize, so a
       section view pre-mapped at 0xE0000 makes VdmInitialize fail with
       STATUS_UNABLE_TO_FREE_VM (0xC000001A) -- diagnosed via the selftest VM gate.
       The frame is mapped AFTER init by v86_map_ems_frame(). (The A0000 VGA
       aperture above is fine pre-init: the kernel leaves it for the display.) */
    return st;
}

/* Map the EMS 64KB page frame as RAM, from the V86 section -- called AFTER
   v86_init()/VdmInitialize (see the NOTE in v86_setup_memory). EMS shadows 16KB
   logical pages into this window via memcpy, so the guest's direct frame accesses
   are plain RAM (no trap). Post-init only part of the UMA is free (e.g. E0000 has
   just 32KB), so we scan the conventional page-frame segments for a free 64KB hole
   and map there. Returns the linear base of the mapped frame (0 if none found). */
DWORD v86_map_ems_frame(void)
{
    HANDLE hntdll = GetModuleHandleA("ntdll.dll");
    PFN_NtMapViewOfSection NtMapViewOfSection =
        (PFN_NtMapViewOfSection)GetProcAddress(hntdll, "NtMapViewOfSection");
    /* Conventional 64KB-aligned EMS page-frame segments, in preference order. E000
       is tried last: post-init it typically has only 32KB free. */
    static const DWORD cand[] = { 0xD0000, 0xC0000, 0xE0000 };
    unsigned i;

    if (!s_hSec) return 0;
    for (i = 0; i < sizeof(cand) / sizeof(cand[0]); ++i) {
        MEMORY_BASIC_INFORMATION mbi;
        LARGE_INTEGER off; PVOID base; SIZE_T sz; LONG st;
        if (!VirtualQuery((LPCVOID)cand[i], &mbi, sizeof(mbi))) continue;
        if (mbi.State != MEM_FREE || mbi.RegionSize < 0x10000) continue;  /* need 64KB free */
        base = (PVOID)cand[i]; sz = 0x10000; off.QuadPart = cand[i];
        st = NtMapViewOfSection(s_hSec, (HANDLE)-1, &base, 0, 0x10000, &off, &sz, 2,
                                VDM_MAP_FLAG, PAGE_EXECUTE_READWRITE);
        if (st >= 0) return cand[i];   /* mapped: linear base of the 64KB frame */
    }
    return 0;
}

LONG v86_init(void)
{
    g_ica.pIcaLock = g_ica_lock;   g_ica.pIcaMaster = g_ica_master;
    g_ica.pIcaSlave = g_ica_slave; g_ica.pDelayIrq = &g_delayirq;
    g_ica.pUndelayIrq = &g_undelayirq; g_ica.pDelayIret = &g_delayiret;
    g_ica.pIretHooked = &g_irethooked; g_ica.pAddrIretBopTable = g_ica_bop;
    g_ica.p9 = &g_p9;
    g_initdata.TrapcHandler = (PVOID)&dpmi_trapc;
    g_initdata.IcaUserData  = &g_ica;

    s_ntvdmctl = (PFN_NtVdmControl)GetProcAddress(
                     GetModuleHandleA("ntdll.dll"), "NtVdmControl");
    if (!s_ntvdmctl) return 1;
    return s_ntvdmctl(VDM_SVC_VdmInitialize, &g_initdata);
}

volatile BYTE *v86_get_tib(void)
{
    BYTE *teb = (BYTE *)v86_teb();
    BYTE *tib;
    if (!teb) return NULL;
    tib = *(BYTE **)(teb + TEB_VDM_TIB);
    if (!tib) {
        /* ntvdm allocates the VDM_TIB itself, registers TEB[0xF18], then inits it
           (0xf044374). Replicate minimally into our static buffer. */
        tib = g_vdmtib;
        *(DWORD *)(tib + 0x000) = VTIB_SIZE_VALUE;   /* VDM_TIB.Size */
        *(DWORD *)(tib + 0x098) = 0;
        *(DWORD *)(tib + 0x09c) = 0x3b;
        *(DWORD *)(tib + 0x0a0) = 0x23;
        *(DWORD *)(tib + 0x0a4) = 0x23;
        *(DWORD *)(tib + 0x66c) = 0xffffffff;
        tib[0x5e4] = 1; tib[0x5e5] = 1; tib[0x5e6] = 1; tib[0x670] = 0;
        *(BYTE **)(teb + TEB_VDM_TIB) = tib;         /* register with our TEB */
    }
    return tib;
}

void v86_set_entry(volatile BYTE *tib, WORD cs, WORD ip, WORD ss, WORD sp,
                   WORD psp_seg)
{
    VDM_REG(tib, VTIB_CONTEXT) = VTIB_CTXFLAGS_VAL;
    VDM_REG(tib, VTIB_GS) = psp_seg; VDM_REG(tib, VTIB_FS) = psp_seg;
    VDM_REG(tib, VTIB_ES) = psp_seg; VDM_REG(tib, VTIB_DS) = psp_seg;
    VDM_REG(tib, VTIB_EDI) = 0; VDM_REG(tib, VTIB_ESI) = 0;
    VDM_REG(tib, VTIB_EBX) = 0; VDM_REG(tib, VTIB_EDX) = 0;
    VDM_REG(tib, VTIB_ECX) = 0; VDM_REG(tib, VTIB_EAX) = 0;
    VDM_REG(tib, VTIB_EBP) = 0;
    VDM_REG(tib, VTIB_EIP) = ip;
    VDM_REG(tib, VTIB_CS)  = cs;
    VDM_REG(tib, VTIB_EFLAGS) = VTIB_EFLAGS_V86;
    VDM_REG(tib, VTIB_ESP) = sp;
    VDM_REG(tib, VTIB_SS)  = ss;
}

DWORD v86_run(volatile BYTE *tib, LONG *out_status)
{
    LONG st = s_ntvdmctl(VDM_SVC_VdmStartExecution, NULL);
    if (out_status) *out_status = st;
    return (DWORD)VDM_REG(tib, VTIB_EVENT);
}

LONG v86_vdmcontrol(ULONG service, PVOID data)
{
    if (!s_ntvdmctl) return 1;
    return s_ntvdmctl(service, data);
}

LONG v86_set_ldt_entries(WORD sel0, DWORD e0lo, DWORD e0hi,
                         WORD sel1, DWORD e1lo, DWORD e1hi)
{
    /* NtSetLdtEntries 6-dword block; see v86.h + fcn.0f050100 in the research. */
    DWORD data[6];
    data[0] = sel0; data[1] = e0lo; data[2] = e0hi;
    data[3] = sel1; data[4] = e1lo; data[5] = e1hi;
    return v86_vdmcontrol(VDM_SVC_VdmSetLdtEntries, data);
}

LONG v86_set_process_ldt_info(WORD start_sel, const DWORD *entries, int count)
{
    /* buf = { StartSel, LengthBytes, entries[count] }; ServiceData = { &buf, count }
       (recovered from SetShadowDescriptorEntries' svc-11 branch, fcn.0f0500c9). */
    static DWORD buf[2 + 2 * 64];       /* header + up to 64 descriptors             */
    DWORD sd[2];
    int i, nd = count * 2;              /* two dwords per descriptor                 */
    if (nd > (int)(sizeof(buf)/sizeof(buf[0])) - 2) return 1;
    /* PROCESS_LDT_INFORMATION { ULONG Start; ULONG Length; LDT_ENTRY Entries[] }.
       Start = byte offset into the LDT; Length = byte count of the entries. */
    buf[0] = start_sel;
    buf[1] = (DWORD)(count * 8);        /* Length: bytes of descriptor entries       */
    for (i = 0; i < nd; ++i) buf[2 + i] = entries[i];
    sd[0] = (DWORD)(ULONG_PTR)buf;
    /* NtSetInformationProcess(ProcessLdtInformation) wants the TOTAL byte size:
       FIELD_OFFSET(Entries)=8 + Length. Passing the count gave INFO_LENGTH_MISMATCH
       (0xC0000004) in VM run 3. */
    sd[1] = (DWORD)(8 + count * 8);
    return v86_vdmcontrol(VDM_SVC_VdmSetProcessLdtInfo, sd);
}
