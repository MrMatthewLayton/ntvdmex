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
#ifndef NTVDMEX_VDM_NTVDM_H
#define NTVDMEX_VDM_NTVDM_H

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
/* VDMState flags, as the code's own call shapes name them (rig, s73). */
#define VDM_FLAG_FIRST_TASK    0x01
#define VDM_FLAG_WOW           0x02
#define VDM_FLAG_DOS           0x04    /* also: a DOS VDM reporting its exit */
#define VDM_FLAG_RETRY         0x08
#define VDM_FLAG_DONT_WAIT     0x20

typedef BOOL (WINAPI *PFN_GetNextVDMCommand)(VDM_COMMAND_INFO *);

/* ===========================================================================
 * NtVdmControl(VdmInitialize) -- register this process as a VDM with the kernel
 * (sets TEB.Vdm + trap/ICA handlers) so GetNextVDMCommand stops returning 0x57,
 * and so VdmStartExecution can run the guest. Contract as stock ntvdm uses it.
 * ======================================================================== */
#define VDM_SVC_VdmInitialize     3
#define VDM_SVC_VdmStartExecution 0   /* NtVdmControl(0, NULL) runs the V86 CONTEXT */
/* VdmQueueInterrupt -- the ASYNC preemption lever (RE'd from XP ntoskrnl this session;
   see docs/research/dpmi-under-ntvdmcontrol.md). ServiceData is NOT a pointer: it is a
   THREAD HANDLE. The target thread must be in the calling process and the process must be a VDM
   (EPROCESS.VdmObjects set by VdmInitialize). The kernel then queues an APC to that
   thread, which is what breaks a guest out of V86 execution without waiting for it to
   trap -- the one thing our in-process exec loop could not do (session 10's blocker).
   What it delivers is decided by the FIXED_NTVDMSTATE pending bits at [0x714]:
   bit 0 = VDM_INT_HARDWARE (kernel dispatches via its virtual ICA), bit 1 =
   VDM_INT_TIMER. Returns STATUS_INVALID_PARAMETER_1 (0xC00000EF) if the thread is not
   ours or the process was never VdmInitialize'd. */
#define VDM_SVC_VdmQueueInterrupt 1
/* DPMI plumbing services (recovered from the ntvdm call-site scan, 2026-07-31 --
   see research/dpmi-under-ntvdmcontrol.md). Service 10's ServiceData is the
   NtSetLdtEntries 6-dword block; service 13 virtualises the PM client interrupt flag. */
#define VDM_SVC_VdmSetLdtEntries     10
#define VDM_SVC_VdmSetProcessLdtInfo 11
#define VDM_SVC_VdmPMCliControl      13

/* ===========================================================================
 * The kernel's VIRTUAL 8259 (ICA). VdmInitialize hands the kernel pointers to
 * these user-mode structures and the kernel emulates the PIC in them: it is what
 * VdmQueueInterrupt's APC consults to decide WHICH vector a pending hardware
 * interrupt becomes, and it is the only path by which the kernel will inject an
 * interrupt into a guest that is not trapping. The layout, and how the kernel
 * treats it (Kernel RE sessions; confirmed by delivery on the rig):
 *
 *   deliverable = IRR & ~(IMR | delayed);  blocked by any bit set in ISR
 *   vector      = ICA_BASE + line;  lines are scanned from ICA_HIPRI (rotation)
 *   the per-line COUNT is decremented on dispatch and clears IRR when it hits 0
 *
 * So a raise is: count[line] = 1; IRR |= 1 << line -- then set VDM_INT_HARDWARE in
 * FIXED_NTVDMSTATE and call VdmQueueInterrupt. The ISR bit the kernel sets stays
 * set until an EOI clears it, exactly like real silicon, which is why the guest's
 * OUT 0x20,0x20 has to reach us (see the PIC VDD).
 * ======================================================================== */
#define ICA_COUNT(line)  ((line) * 4)  /* dword per line: dispatches remaining   */
#define ICA_BASE         0x28          /* word: vector base (master 0x08)        */
#define ICA_HIPRI        0x2A          /* word: priority rotation start          */
#define ICA_MODE         0x2C          /* byte: mode bits (0x20 tested by kernel) */
#define ICA_MODE2        0x2D          /* byte: &3 -> ignore ISR priority block  */
#define ICA_IRR          0x2F          /* byte: interrupt REQUEST mask           */
#define ICA_ISR          0x30          /* byte: IN-SERVICE mask (cleared by EOI) */
#define ICA_IMR          0x31          /* byte: interrupt MASK register          */
#define ICA_SLAVE_MASK   0x32          /* byte: lines with a slave attached (IRQ2)*/
#define ICA_STRUCT_SIZE  0x40          /* generous: kernel touches up to 0x32    */

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
   args, as stock ntvdm calls it; DOS passes flag 1 and 0 for the
   video-state buffer/size (args 8,9). */
typedef BOOL (WINAPI *PFN_RegisterConsoleVDM)(DWORD, HANDLE, HANDLE, HANDLE,
            DWORD, PVOID, PVOID, PVOID, DWORD, PVOID, PVOID);

/* ===========================================================================
 * V86 low-memory address space (set up right before VdmInitialize -- without it
 * VdmInitialize access-violates). Create a section,
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
/* EFlags: VM + IF + reserved bit, IOPL=0. IF MATTERS: a DOS program is entered by
   DOS with interrupts already enabled and, having no reason to think otherwise,
   never issues STI. Starting the guest with IF=0 therefore left it disabled for the
   whole run, so the host's IRQ0 gate never opened, no INT 08h was ever injected and
   the BIOS tick at 0040:006C never advanced -- measured, not inferred: iobench saw
   34M port-I/O events with irq0_inj=0 and the tick frozen, and [0x714] sat with
   VDM_INT_TIMER pending throughout. Anything paced on the tick (Skyroads' sound/PIT
   init, FM music, PCM block timing) hung. Note VTIB_EFLAGS_PM below always had IF
   set, which is why the protected-mode timer path worked while real mode did not. */
#define VTIB_EFLAGS_V86    0x20202
/* ── PROTECTED-MODE EFLAGS: IOPL MUST BE 3, AND IT IS NOT A DETAIL. ──────────────
   The guest runs at CPL 3. With IOPL 0, `STI`, `CLI`, `IN`, `OUT` and `INT n` to a
   gate are all IOPL-sensitive and raise #GP -- and a raw protected-mode #GP is the one
   fault XP will not reflect to us, so the kernel silently terminates the whole VDM. No
   VEH exception, no trampoline catch, the log just stops.
   Session 17 measured it exactly: Doom's DOS/4GW executes an STI a few instructions
   after returning from an INT 31h 000C wrapper, and a breakpoint planted on that STI
   FIRED -- proving the client got there -- after which letting the single displaced
   byte execute killed the run every time.
   ► THE OBVIOUS FIX DOES NOT WORK, AND THAT IS MEASURED, NOT ASSUMED. Setting IOPL=3
     here (0x03202) changes nothing: the kernel SANITISES IOPL out of the context it
     loads. Across a whole Doom run the live EFLAGS the guest reported were 0x...0296,
     0x...0292, 0x...0206, 0x...0202, 0x...0246 -- bits 12-13 never once set. So the
     value below stays 0, because pretending otherwise would read as "IOPL is 3" to the
     next person.
   ► AND THE PROCESS-WIDE ROUTE IS WORSE, NOT BETTER. NtSetInformationProcess's
     ProcessUserModeIOPL would give the whole process IOPL 3 -- but at CPL <= IOPL the
     I/O permission bitmap is BYPASSED, so every guest IN/OUT would reach real hardware
     instead of our VDDs. That trades a fault for silent loss of the entire device
     emulation. Do not "fix" STI that way.
   ► WHAT IS ACTUALLY REQUIRED is the GH #18 protected-mode #GP reflect, so a privileged
     instruction surfaces to us and can be emulated (STI/CLI -> g_dpmi_vi), which is what
     a VDM monitor does. Until then a PM client that uses CLI/STI cannot run to
     completion. See return-ntvdm.md. */
#define VTIB_EFLAGS_PM     0x00202    /* EFlags: IF + reserved bit, VM clear (PM); IOPL
                                         is 0 because the kernel strips anything else */
/* Virtual MSW (low 16 of the client's CR0) the monitor keeps in the VDM_TIB.
   Stock keeps it at TIB+0x668 and decides PM-vs-V86 on its PE bit. Setting PE
   marks the client as in protected mode. */
#define VTIB_MSW           0x668
#define MSW_PE_BIT         0x0001     /* CR0.PE                                    */


/* V86 stop/event reporting in the VDM_TIB, read after VdmStartExecution returns. */
#define VTIB_EVENT       0x5A8       /* event code (VDM_EVENT_BOP = serviceable BOP) */
#define VTIB_EVENT_INFO  0x5B0       /* extra info for fault events                 */
/* Event taxonomy (kernel -> host, by VTIB_EVENT value):
   0 = I/O port access (IOPL-0 IN/OUT trap); 1,3 internal; 2 = GP fault; 4 = BOP/
   software dispatch; 5 = terminate; 6 = hardware IRQ; >=7 exits the loop.
   [VM-confirmed 2026-06-07] Under QEMU+HVF an IOPL-0 `OUT 0x43,al` stops with
   VTIB_EVENT(0x5A8)=0 and VTIB_EVENT_INFO(0x5B0) low word = the port (0x0043).
   [BARE-METAL-confirmed 2026-08-18] On a real XP box (Core 2 / Quadro) the SAME
   IOPL-0 I/O trap surfaces as VTIB_EVENT=3 with INFO=0 (Skyroads' `IN AL,DX` at
   0x0110:0x5878). So the I/O reflect code differs by platform: HVF=0, real HW=3.
   host_try_io() self-validates (decodes the IN/OUT at CS:IP), so we route BOTH. */
#define VDM_EVENT_IO      0
/* String I/O (REP INS/OUTS). The kernel decodes it for us and leaves a descriptor
   in the words after VTIB_EVENT -- observed for `rep outsb` to 0x3C9:
   {1, 2, port|size<<16 (0x000103C9), 1, count (0x40), seg:off (0x01000355)}.
   We service it from the guest's own SI/DI/CX/DF instead, so we depend only on
   register fields whose meaning is already established. */
#define VDM_EVENT_IO_STRING 1
#define VDM_EVENT_GPFAULT 2
#define VDM_EVENT_IO_HW   3       /* I/O reflect code on real hardware (HVF uses 0) */
#define VDM_EVENT_BOP     4
#define VDM_EVENT_HWIRQ   6

/* --- PM-fault reflect block (GH #18, real-CPU protected mode) --------------------------
   When a raw (non-BOP) protected-mode #GP faults, the NT kernel reflects the fault
   through this VDM_TIB block (Kernel RE sessions 4-7, confirmed by the behaviour
   below on the rig): when the nest counter is 0 it saves the interrupted CS/EIP and
   resumes the guest at the handler selector with EIP=0x1000:
     +0x634 word  nesting counter -- MUST be 0 for the "first level, save CS:EIP" path;
                  the kernel inc's it, so the host re-arms it to 0 before each PM entry.
     +0x636 word  16/32-bit client flag (stock ntvdm arms it per client).
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
/* FIXED_NTVDMSTATE, the kernel's VDM state word at linear 0x714, and its pending-interrupt bits. */
#define FIXED_NTVDMSTATE_LINEAR   0x714
#define VDM_INT_HARDWARE          0x1u    /* bit 0: dispatch through the virtual ICA */
#define VDM_INT_TIMER             0x2u    /* bit 1                                   */
#define VDM_INT_PENDING           (VDM_INT_HARDWARE | VDM_INT_TIMER)
/* An access violation's ExceptionInformation: [0] the operation, [1] the address. */
#define EXCEPTION_AV_OPERATION    0
#define EXCEPTION_AV_ADDRESS      1
#define EXCEPTION_AV_PARAMETERS   2
#define NT_USER_SPACE_END_U       0x7FFF0000u   /* one past the highest user-mode page */
/* NT's own facts the host meets in exceptions: an NTSTATUS's top nibble C is error severity,
   and user mode runs on GDT selector 1Bh. */
#define NT_STATUS_SEVERITY_MASK   0xF0000000u
#define NT_STATUS_SEVERITY_ERROR  0xC0000000u
#define NT_STATUS_NOT_SUCCESS_BIT_U 0x80000000u   /* set for both warning (8...) and error (C...) */
#define NT_USER_CODE_SELECTOR     0x1B
#define VDM_REG(tib, off)      (*(volatile DWORD *)((volatile BYTE *)(tib) + (off)))
/* The low 16 bits of such a field: a 16-bit register (CS, IP, DS...) or AX/BX/CX/DX. */
#define VDM_REG16(tib, off)    (VDM_REG(tib, off) & WORD_MASK)
/* Set the low 16 bits of such a field, preserving the high half. */
#define VDM_SET16(tib, off, v) (VDM_REG((tib), (off)) = \
        (VDM_REG((tib), (off)) & HIGH_WORD_MASK_U) | ((DWORD)(v) & WORD_MASK_U))

/* ── XP's COMMAND.COM AND ITS PRIVATE BOP 54h (docs/inventory/bop.md). `C4 C4 54 sub`;
     the names are the inventory's, from what each call was observed to do. */
#define NTVDM_BOP_ISV                  0x58  /* a guest's third-party BOP, C4 C4 58 sub    */
#define NTVDM_CMD_TERMINATE            0x00  /* ends the VDM, as VDDTerminateVDM          */
#define NTVDM_CMD_NEXT_COMMAND         0x01  /* GetNextVDMCommand: what to run next       */
#define NTVDM_CMD_STARTUP_BATCH        0x0D  /* a path to open: the startup batch file    */
#define NTVDM_CMD_KEYBOARD_CONFIG      0x0E  /* the console's keyboard layout, code page  */
#define NTVDM_CMD_PROMPT               0x0F  /* the host's PROMPT / environment           */
#define NTVDM_CMD_QUERY_BIT            0x10  /* a one-bit query, answered in AL           */
/* sub 01's block, at DS:DX. */
#define NTVDM_CMD_BLOCK_TAIL_SEGMENT   0x08  /* the command-tail buffer, seg:off          */
#define NTVDM_CMD_BLOCK_TAIL_OFFSET    0x0A
#define NTVDM_CMD_BLOCK_EXIT_CODE      0x0E  /* copied into the VDM_COMMAND_INFO          */
#define NTVDM_CMD_BLOCK_REDIRECTION    0x10  /* bit 0 stdin, 1 stdout, 2 stderr           */
#define NTVDM_CMD_BLOCK_INTERACTIVE    0x12  /* DWORD: 0 = nothing drives the shell       */
#define NTVDM_CMD_BLOCK_KEYBOARD_GATE  0x1A  /* low byte must be 0 for the prompt         */
#define NTVDM_CMD_BLOCK_NAME_SEGMENT   0x1C  /* the program-name buffer, seg:off          */
#define NTVDM_CMD_BLOCK_NAME_OFFSET    0x1E
#define NTVDM_CMD_BLOCK_NAME_CAPACITY  0x20
#define NTVDM_CMD_BLOCK_PROGRAM_TYPE   0x22
/* +0x22: the program's type, by stock's rule on the name's last four characters. */
#define NTVDM_CMD_TYPE_BAT             2
#define NTVDM_CMD_TYPE_EXE             4
#define NTVDM_CMD_TYPE_COM             8
#define NTVDM_CMD_TYPE_OTHER           9     /* anything else, or a name this short:   */
#define NTVDM_CMD_SHORT_NAME_MAX       6
#define NTVDM_CMD_STARTUP_PATH_MAX     0x3F  /* sub 0D: XP's own cap                    */

#endif /* NTVDMEX_VDM_NTVDM_H */
