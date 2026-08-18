/* ntvdm.h -- the undocumented NT/CSRSS/V86 contract NTVDMEX drives.
 *
 * The clean home for the declarations the DOS VDM host needs: the structures and
 * functions ntvdm.exe uses but Microsoft never published, recovered by reverse-
 * engineering XP's ntvdm/basesrv/ntoskrnl (see docs/research/ntvdmcontrol-and-v86.md
 * and ntvdmcontrol's ground-truth offsets). Promoted out of the tools/vdmhost spike
 * (M2.6) so there is one documented source of truth instead of scattered globals.
 *
 * Everything here is a *declaration* (types, constants, function pointer types,
 * field offsets) -- no logic -- so it stays a stable contract the src/vdm modules
 * build on.
 */
#ifndef NTVDM_H
#define NTVDM_H

#include <windows.h>

/* ===========================================================================
 * GetNextVDMCommand -- pull the program-to-run from the CSRSS VDM queue.
 * VDM_COMMAND_INFO matches the ~0xA0-byte struct ntvdm passes (layout from
 * ReactOS sdk/include/reactos/subsys/win/vdm.h, cross-checked against ntvdm).
 * ======================================================================== */
typedef struct {
    ULONG  TaskId;
    ULONG  CreationFlags;
    ULONG  ExitCode;
    ULONG  CodePage;
    HANDLE StdIn, StdOut, StdErr;
    LPSTR  CmdLine, AppName, PifFile, CurDirectory, Env;
    ULONG  EnvLen;
    STARTUPINFOA StartupInfo;
    LPSTR  Desktop;  ULONG DesktopLen;
    LPSTR  Title;    ULONG TitleLen;
    LPVOID Reserved; ULONG ReservedLen;
    USHORT CmdLen, AppLen, PifLen, CurDirectoryLen, VDMState, CurrentDrive;
    BOOLEAN ComingFromBat;
} VDM_COMMAND_INFO;

#define VDM_GET_FIRST_COMMAND  0x100
#define VDM_GET_ENVIRONMENT    0x400

typedef BOOL (WINAPI *PFN_GetNextVDMCommand)(VDM_COMMAND_INFO *);

/* ===========================================================================
 * NtVdmControl(VdmInitialize) -- register this process as a VDM with the kernel
 * (sets TEB.Vdm + trap/ICA handlers) so GetNextVDMCommand stops returning 0x57,
 * and so VdmStartExecution can run the guest. Contract from ntvdm 0xf00e668.
 * ======================================================================== */
#define VDM_SVC_VdmInitialize     3
#define VDM_SVC_VdmStartExecution 0   /* NtVdmControl(0, NULL) runs the V86 CONTEXT */
/* DPMI plumbing services (recovered from the ntvdm call-site scan, 2026-07-31 --
   see research/dpmi-under-ntvdmcontrol.md). Service 10's ServiceData is the
   NtSetLdtEntries 6-dword block; service 13 virtualises the PM client interrupt flag. */
#define VDM_SVC_VdmSetLdtEntries     10
#define VDM_SVC_VdmSetProcessLdtInfo 11
#define VDM_SVC_VdmPMCliControl      13

typedef struct {            /* VDMICAUSERDATA -- 9 pointers (XP ntvdm fills 9) */
    PVOID pIcaLock, pIcaMaster, pIcaSlave, pDelayIrq, pUndelayIrq,
          pDelayIret, pIretHooked, pAddrIretBopTable, p9;
} VDMICAUSERDATA;

typedef struct {            /* VDM_INITIALIZE_DATA */
    PVOID TrapcHandler;
    VDMICAUSERDATA *IcaUserData;
} VDM_INITIALIZE_DATA;

typedef LONG (WINAPI *PFN_NtVdmControl)(ULONG Service, PVOID ServiceData);

/* RegisterConsoleVDM (kernel32) -- register as the console VDM with CSRSS. 11
   args, matching ntvdm's call at 0xf014078; DOS passes flag 1 and 0 for the
   video-state buffer/size (args 8,9). */
typedef BOOL (WINAPI *PFN_RegisterConsoleVDM)(DWORD, HANDLE, HANDLE, HANDLE,
            DWORD, PVOID, PVOID, PVOID, DWORD, PVOID, PVOID);

/* ===========================================================================
 * V86 low-memory address space (ntvdm fn 0xf00ea75, runs right before
 * VdmInitialize -- without it VdmInitialize access-violates). Create a section,
 * release the default low reservations, then map the section X-RW into low memory.
 * ======================================================================== */
typedef struct {                /* OBJECT_ATTRIBUTES (24 bytes) */
    ULONG Length; PVOID RootDirectory, ObjectName; ULONG Attributes;
    PVOID SecurityDescriptor, SecurityQOS;
} OBJ_ATTR;

typedef LONG (WINAPI *PFN_NtCreateSection)(PHANDLE, ULONG, OBJ_ATTR *,
            LARGE_INTEGER *, ULONG, ULONG, HANDLE);
typedef LONG (WINAPI *PFN_NtFreeVirtualMemory)(HANDLE, PVOID *, SIZE_T *, ULONG);
typedef LONG (WINAPI *PFN_NtMapViewOfSection)(HANDLE, HANDLE, PVOID *, ULONG,
            SIZE_T, LARGE_INTEGER *, SIZE_T *, ULONG, ULONG, ULONG);
typedef LONG (WINAPI *PFN_NtUnmapViewOfSection)(HANDLE, PVOID);

#define MEM_RELEASE_NT  0x8000
#define SEC_RESERVE_NT  0x04000000
#define VDM_MAP_FLAG    0x40000000   /* ntvdm's AllocationType for the V86 map */

/* ===========================================================================
 * VDM_TIB + embedded V86 CONTEXT. ntvdm allocates the VDM_TIB itself and stores
 * the pointer at TEB+0xF18 (the kernel does NOT); the kernel runs the CONTEXT
 * embedded in it. Field offsets recovered from ntvdm's getXX accessors + the
 * standard x86 CONTEXT. The host reads/writes guest registers at these absolute
 * VDM_TIB offsets (e.g. AX = *(DWORD*)(tib + VTIB_EAX)).
 * ======================================================================== */
#define TEB_VDM_TIB        0xF18      /* TEB offset of the VDM_TIB pointer        */
#define VTIB_SIZE_VALUE    0x674      /* value stored in VDM_TIB.Size (+0x000)    */
#define VTIB_CONTEXT       0x2D8      /* CONTEXT.ContextFlags                     */
#define VTIB_CTXFLAGS_VAL  0x10007    /* ContextFlags the host sets (full V86 ctx)*/
#define VTIB_GS            0x364
#define VTIB_FS            0x368
#define VTIB_ES            0x36C
#define VTIB_DS            0x370
#define VTIB_EDI           0x374
#define VTIB_ESI           0x378
#define VTIB_EBX           0x37C
#define VTIB_EDX           0x380
#define VTIB_ECX           0x384
#define VTIB_EAX           0x388
#define VTIB_EBP           0x38C
#define VTIB_EIP           0x390
#define VTIB_CS            0x394
#define VTIB_EFLAGS        0x398
#define VTIB_ESP           0x39C
#define VTIB_SS            0x3A0
#define VTIB_EFLAGS_V86    0x20002    /* EFlags: VM + reserved bit, IOPL=0        */
#define VTIB_EFLAGS_PM     0x00202    /* EFlags: IF + reserved bit, VM clear (PM) */
#define EFLAGS_VM_BIT      0x20000    /* EFLAGS.VM (bit 17): set=V86, clear=PM     */
/* Virtual MSW (low 16 of the client's CR0) the monitor keeps in the VDM_TIB.
   getMSW (ntvdm 0xf0041b5) reads word[TIB+0x668]; fcn.0f00532e gates PM-vs-V86 on
   its PE bit. Setting PE marks the client as in protected mode. */
#define VTIB_MSW           0x668
#define MSW_PE_BIT         0x0001     /* CR0.PE                                    */

/* BOP (BIOS Operation): the 3-byte sequence C4 C4 nn is an invalid opcode the
   kernel reflects back to the host as a VDM event, carrying the byte nn. The host
   advances EIP past the 3 bytes and re-enters; a trailing IRET (CF) resumes the
   guest. Real-mode INT 21h is vectored through the IVT to a handler that runs a
   BOP, so every INT 21h surfaces to the host. */
#define VDM_BOP0 0xC4
#define VDM_BOP1 0xC4

/* V86 stop/event reporting in the VDM_TIB, read after VdmStartExecution returns. */
#define VTIB_EVENT       0x5A8       /* event code (VDM_EVENT_BOP = serviceable BOP) */
#define VTIB_EVENT_INFO  0x5B0       /* extra info for fault events                 */
/* Event taxonomy (kernel -> host, ntvdm dispatch table 0xf064a60, index=VTIB_EVENT):
   0 = I/O port access (IOPL-0 IN/OUT trap); 1,3 internal; 2 = GP fault; 4 = BOP/
   software dispatch; 5 = terminate; 6 = hardware IRQ; >=7 exits the loop.
   [VM-confirmed 2026-06-07] Under QEMU+HVF an IOPL-0 `OUT 0x43,al` stops with
   VTIB_EVENT(0x5A8)=0 and VTIB_EVENT_INFO(0x5B0) low word = the port (0x0043).
   [BARE-METAL-confirmed 2026-08-18] On a real XP box (Core 2 / Quadro) the SAME
   IOPL-0 I/O trap surfaces as VTIB_EVENT=3 with INFO=0 (Skyroads' `IN AL,DX` at
   0x0110:0x5878). So the I/O reflect code differs by platform: HVF=0, real HW=3.
   host_try_io() self-validates (decodes the IN/OUT at CS:IP), so we route BOTH. */
#define VDM_EVENT_IO      0
#define VDM_EVENT_GPFAULT 2
#define VDM_EVENT_IO_HW   3       /* I/O reflect code on real hardware (HVF uses 0) */
#define VDM_EVENT_BOP     4
#define VDM_EVENT_HWIRQ   6

/* --- PM-fault reflect block (GH #18, real-CPU protected mode) --------------------------
   When a raw (non-BOP) protected-mode #GP faults, the kernel's reflect path
   (KiTrap0D -> 0x565041[BOP-only] -> 0x4f67f8 -> 0x4f6f67 -> PM branch 0x4f6fed ->
   0x4f6e6f) reflects the fault through this VDM_TIB block, recovered by disassembling
   ntoskrnl.exe 0x4f6e6f (Kernel RE sessions 4-7). The writer receives edi = VDM_TIB+0x634
   and, when the nest counter is 0, saves the interrupted CS/EIP and installs the handler
   selector as the new CS with EIP=0x1000:
     +0x634 word  nesting counter -- MUST be 0 for the "first level, save CS:EIP" path;
                  the kernel inc's it, so the host re-arms it to 0 before each PM entry.
     +0x636 word  16/32-bit client flag (ntvdm arm routine 0xf050ad7 -> [0xf09c178]).
     +0x638 word  handler selector -- the kernel loads this as the new CS.
     +0x63a word  saved faulting CS   (kernel writes).
     +0x63c dword saved faulting EIP  (kernel writes).
     +0x640 dword saved third slot ([trap+0x10]; SS:ESP-class, kernel writes).
   The plan (Kernel RE session 6): make +0x638 a code selector whose base+0x1000 holds a
   BOP (C4 C4 nn); the kernel jumps the guest to selector:0x1000 -> the BOP reflects as
   VTIB_EVENT=4 -> the host reads the saved fault CS:EIP from +0x63a/0x63c and services it. */
#define VTIB_FLT_NEST   0x634    /* nesting counter (arm to 0 each entry)        */
#define VTIB_FLT_FLAG   0x636    /* 16/32-bit client flag                        */
#define VTIB_FLT_HSEL   0x638    /* PM-fault handler selector (our trampoline H) */
#define VTIB_FLT_SAVCS  0x63A    /* kernel: saved faulting CS                     */
#define VTIB_FLT_SAVEIP 0x63C    /* kernel: saved faulting EIP                    */
#define VTIB_FLT_SAV3   0x640    /* kernel: saved third slot (SS:ESP-class)      */

/* Access a 32-bit guest register/field at VDM_TIB offset `off` (e.g. VTIB_EAX). */
#define VDM_REG(tib, off)      (*(volatile DWORD *)((volatile BYTE *)(tib) + (off)))
/* Set the low 16 bits of such a field, preserving the high half. */
#define VDM_SET16(tib, off, v) (VDM_REG((tib), (off)) = \
        (VDM_REG((tib), (off)) & 0xFFFF0000u) | ((DWORD)(v) & 0xFFFFu))

#endif /* NTVDM_H */
