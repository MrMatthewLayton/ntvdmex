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

/* BOP (BIOS Operation): the 3-byte sequence C4 C4 nn is an invalid opcode the
   kernel reflects back to the host as a VDM event, carrying the byte nn. The host
   advances EIP past the 3 bytes and re-enters; a trailing IRET (CF) resumes the
   guest. Real-mode INT 21h is vectored through the IVT to a handler that runs a
   BOP, so every INT 21h surfaces to the host. */
#define VDM_BOP0 0xC4
#define VDM_BOP1 0xC4

#endif /* NTVDM_H */
