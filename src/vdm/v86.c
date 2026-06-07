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

/* Trap handler stub -- stored by the kernel, only invoked on a V86 fault. */
static void __stdcall trap_stub(void) { }

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
    HANDLE hSec = NULL;
    OBJ_ATTR oa;
    LARGE_INTEGER maxsize, off;
    PVOID base; SIZE_T sz; LONG st;
    unsigned i; char *q = (char *)&oa;

    for (i = 0; i < sizeof(oa); ++i) q[i] = 0;
    oa.Length = 0x18;
    maxsize.QuadPart = 0xC0000;        /* +video aperture A0000-BFFFF (M3 mode 13h) */
    NtCreateSection(&hSec, 0xA, &oa, &maxsize, PAGE_EXECUTE_READWRITE,
                    SEC_RESERVE_NT, NULL);

    /* Release the default low reservations the loader made, then map the section
       X-RW across the full 640KB in the FOUR pieces ntvdm uses (0xf00ea75). */
    base = (PVOID)1;        sz = 0x9FFFF; NtFreeVirtualMemory((HANDLE)-1, &base, &sz, MEM_RELEASE_NT);
    base = (PVOID)0x100000; sz = 0x10000; NtFreeVirtualMemory((HANDLE)-1, &base, &sz, MEM_RELEASE_NT);
    base = (PVOID)0xA0000;  sz = 0x20000; NtFreeVirtualMemory((HANDLE)-1, &base, &sz, MEM_RELEASE_NT);

    base = (PVOID)1;        sz = 0xFFFF;  off.QuadPart = 0;
    NtMapViewOfSection(hSec, (HANDLE)-1, &base, 0, 0xFFFF, &off, &sz, 2,
                       VDM_MAP_FLAG, PAGE_EXECUTE_READWRITE);
    base = (PVOID)0x100000; sz = 0x10000; off.QuadPart = 0;
    NtMapViewOfSection(hSec, (HANDLE)-1, &base, 0, 0x10000, &off, &sz, 2,
                       VDM_MAP_FLAG, PAGE_EXECUTE_READWRITE);
    /* Map 3 (the rest of conventional memory): section[0x10000..] -> linear 0x10000,
       size 0x90000. Without it the guest faults the moment it touches >64KB. */
    base = (PVOID)0x10000;  sz = 0x90000; off.QuadPart = 0x10000;
    st = NtMapViewOfSection(hSec, (HANDLE)-1, &base, 0, 0x90000, &off, &sz, 2,
                            VDM_MAP_FLAG, PAGE_EXECUTE_READWRITE);
    /* Map 4 -- the VGA aperture A0000-BFFFF (128KB) as RAM, so direct-framebuffer
       writes (mode 13h at A0000, text at B8000) just land in memory and the video
       VDD renders it each frame. */
    base = (PVOID)0xA0000;  sz = 0x20000; off.QuadPart = 0xA0000;
    NtMapViewOfSection(hSec, (HANDLE)-1, &base, 0, 0x20000, &off, &sz, 2,
                       VDM_MAP_FLAG, PAGE_EXECUTE_READWRITE);
    return st;
}

LONG v86_init(void)
{
    g_ica.pIcaLock = g_ica_lock;   g_ica.pIcaMaster = g_ica_master;
    g_ica.pIcaSlave = g_ica_slave; g_ica.pDelayIrq = &g_delayirq;
    g_ica.pUndelayIrq = &g_undelayirq; g_ica.pDelayIret = &g_delayiret;
    g_ica.pIretHooked = &g_irethooked; g_ica.pAddrIretBopTable = g_ica_bop;
    g_ica.p9 = &g_p9;
    g_initdata.TrapcHandler = (PVOID)&trap_stub;
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
