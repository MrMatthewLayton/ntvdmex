/* dos_int21.h -- the DOS INT 21h service surface for the host.
 *
 * One BOP per INT 21h surfaces here; DosInt21() reads the guest registers from
 * the VDM_TIB, services the call (console, Win32-backed file I/O, memory via
 * dos_mcb.h, misc), and returns CF on the FLAGS the INT pushed on the V86 stack.
 * Ported from tools/vdmhost/vdmhost.c; the memory calls now delegate to the shared
 * dos_mcb.h allocator instead of an inline copy.
 */
#ifndef DOS_INT21_H
#define DOS_INT21_H

#include <windows.h>
#include <stdint.h>
#include "ntvdm.h"
#include "dos_layout.h"   /* DOS_MAX_FILES -- the capacity fh[] must match */
#include "dos_clock.h"    /* the VDM's own clock -- GH #250 */
#include "dos_psp.h"      /* DOS_PSP_JFT_HANDLES -- a JFT's size */

/* INT 21h function numbers (AH), by their documented names. */
#define DOS_FN_TERMINATE                0x00
#define DOS_FN_CHAR_INPUT_ECHO          0x01
#define DOS_FN_CHAR_OUTPUT              0x02
#define DOS_FN_AUX_INPUT                0x03
#define DOS_FN_AUX_OUTPUT               0x04
#define DOS_FN_PRINTER_OUTPUT           0x05
#define DOS_FN_DIRECT_CONSOLE_IO        0x06
#define DOS_FN_DIRECT_INPUT             0x07
#define DOS_FN_CHAR_INPUT               0x08
#define DOS_FN_PRINT_STRING             0x09
#define DOS_FN_BUFFERED_INPUT           0x0A
#define DOS_FN_INPUT_STATUS             0x0B
#define DOS_FN_FLUSH_AND_INPUT          0x0C
#define DOS_FN_DISK_RESET               0x0D
#define DOS_FN_SELECT_DRIVE             0x0E
#define DOS_FN_FCB_OPEN                 0x0F
#define DOS_FN_FCB_CLOSE                0x10
#define DOS_FN_FCB_FIND_FIRST           0x11
#define DOS_FN_FCB_FIND_NEXT            0x12
#define DOS_FN_FCB_DELETE               0x13
#define DOS_FN_FCB_READ_SEQUENTIAL      0x14
#define DOS_FN_FCB_WRITE_SEQUENTIAL     0x15
#define DOS_FN_FCB_CREATE               0x16
#define DOS_FN_FCB_RENAME               0x17
#define DOS_FN_NULL_18                  0x18
#define DOS_FN_GET_DRIVE                0x19
#define DOS_FN_SET_DTA                  0x1A
#define DOS_FN_GET_DEFAULT_DRIVE_INFO   0x1B
#define DOS_FN_GET_DRIVE_INFO           0x1C
#define DOS_FN_NULL_1D                  0x1D
#define DOS_FN_NULL_1E                  0x1E
#define DOS_FN_GET_DEFAULT_DPB          0x1F
#define DOS_FN_NULL_20                  0x20
#define DOS_FN_FCB_READ_RANDOM          0x21
#define DOS_FN_FCB_WRITE_RANDOM         0x22
#define DOS_FN_FCB_FILE_SIZE            0x23
#define DOS_FN_FCB_SET_RANDOM_RECORD    0x24
#define DOS_FN_SET_VECTOR               0x25
#define DOS_FN_CREATE_PSP               0x26
#define DOS_FN_FCB_READ_BLOCK           0x27
#define DOS_FN_FCB_WRITE_BLOCK          0x28
#define DOS_FN_PARSE_FILENAME           0x29
#define DOS_FN_GET_DATE                 0x2A
#define DOS_FN_SET_DATE                 0x2B
#define DOS_FN_GET_TIME                 0x2C
#define DOS_FN_SET_TIME                 0x2D
#define DOS_FN_SET_VERIFY               0x2E
#define DOS_FN_GET_DTA                  0x2F
#define DOS_FN_GET_VERSION              0x30
#define DOS_FN_KEEP_PROCESS             0x31
#define DOS_FN_GET_DPB                  0x32
#define DOS_FN_BREAK_STATE              0x33
#define DOS_FN_GET_INDOS_FLAG           0x34
#define DOS_FN_GET_VECTOR               0x35
#define DOS_FN_GET_FREE_SPACE           0x36
#define DOS_FN_SWITCH_CHAR              0x37
#define DOS_FN_COUNTRY_INFO             0x38
#define DOS_FN_MKDIR                    0x39
#define DOS_FN_RMDIR                    0x3A
#define DOS_FN_CHDIR                    0x3B
#define DOS_FN_CREATE                   0x3C
#define DOS_FN_OPEN                     0x3D
#define DOS_FN_CLOSE                    0x3E
#define DOS_FN_READ                     0x3F
#define DOS_FN_WRITE                    0x40
#define DOS_FN_DELETE                   0x41
#define DOS_FN_SEEK                     0x42
#define DOS_FN_FILE_ATTRIBUTES          0x43
#define DOS_FN_IOCTL                    0x44
#define DOS_FN_DUP                      0x45
#define DOS_FN_DUP2                     0x46
#define DOS_FN_GET_CURRENT_DIRECTORY    0x47
#define DOS_FN_ALLOCATE                 0x48
#define DOS_FN_FREE                     0x49
#define DOS_FN_RESIZE                   0x4A
#define DOS_FN_EXEC                     0x4B
#define DOS_FN_EXIT                     0x4C
#define DOS_FN_GET_RETURN_CODE          0x4D
#define DOS_FN_FIND_FIRST               0x4E
#define DOS_FN_FIND_NEXT                0x4F
#define DOS_FN_SET_PSP                  0x50
#define DOS_FN_GET_PSP_UNDOCUMENTED     0x51
#define DOS_FN_GET_LIST_OF_LISTS        0x52
#define DOS_FN_BPB_TO_DPB               0x53
#define DOS_FN_GET_VERIFY               0x54
#define DOS_FN_CREATE_CHILD_PSP         0x55
#define DOS_FN_RENAME                   0x56
#define DOS_FN_FILE_DATE_TIME           0x57
#define DOS_FN_ALLOCATION_STRATEGY      0x58
#define DOS_FN_EXTENDED_ERROR           0x59
#define DOS_FN_CREATE_TEMP              0x5A
#define DOS_FN_CREATE_NEW               0x5B
#define DOS_FN_LOCK                     0x5C
#define DOS_FN_SERVER                   0x5D
#define DOS_FN_NETWORK_MACHINE          0x5E
#define DOS_FN_REDIRECTION              0x5F
#define DOS_FN_TRUENAME                 0x60
#define DOS_FN_UNUSED_61                0x61
#define DOS_FN_GET_PSP                  0x62
#define DOS_FN_DBCS_TABLE               0x63
#define DOS_FN_SET_LOOKAHEAD            0x64
#define DOS_FN_EXTENDED_COUNTRY         0x65
#define DOS_FN_CODE_PAGE                0x66
#define DOS_FN_SET_HANDLE_COUNT         0x67
#define DOS_FN_COMMIT                   0x68
#define DOS_FN_DISK_SERIAL              0x69
#define DOS_FN_COMMIT_6A                0x6A
#define DOS_FN_NULL_6B                  0x6B
#define DOS_FN_EXTENDED_OPEN            0x6C
#define DOS_FN_LFN                      0x71
#define DOS_FN_GET_LOGICAL_DRIVE_MAP    0xDC
#define DOS_FN_LAST_622                 0x6C    /* the last function 6.22 defines */


/* A trace is opt-in via cfg\dostrace.flag, which says who PAYS for it and nothing
   about how big it gets. XP's COMMAND.COM in a command loop wrote 2,166,824 lines
   and 268 MB before this existed. Opt-in is not the same as bounded. */
#define DOS_TRACE_MAX 4000
#define DOS_FIND_SLOTS        8      /* AH=4Eh/4Fh searches live at once            */
#define DOS_EXEC_NAME_SIZE    128
#define DOS_SERVICE_BITS      32     /* one bit per AH: 256 services                */
#define DOS_CONSOLE_LINE_SIZE 130    /* 127 characters + CR LF + room               */
#define DOS_SFT_INDEXES       256    /* a JFT entry is a byte                       */
#define DOS_FCB_NAME_SIZE     11     /* 8.3, blank-padded, no dot                   */

/* One saved handle table -- see DOS_MACHINE::HandleStack. */
typedef struct _DOS_HANDLE_FRAME { HANDLE FileHandles[DOS_MAX_FILES]; UINT32 StdOpen; BYTE JftKnown[DOS_PSP_JFT_HANDLES]; } DOS_HANDLE_FRAME;

/* DOS-machine state the INT 21h surface owns. */
typedef struct _DOS_MACHINE {
    volatile BYTE *Tib;        /* guest CONTEXT (registers via VDM_REG)            */
    HANDLE   FileHandles[DOS_MAX_FILES]; /* DOS handle -> Win32 (0..4 console; files in 5+) */
    WORD FirstMcb;        /* MCB chain root (AH=48/49/4A)                     */
    WORD DtaSegment, DtaOffset; /* Disk Transfer Area (AH=1A/2F)                    */
    BYTE  VersionMajor, VersionMinor;  /* reported DOS version -- GH #28, default 6.22 */
    /* ── #208, THE USER'S CHOICE: SETVER, NOT A SESSION-WIDE 5.00. ─────────────────
         XP's COMMAND.COM refuses anything but 5.00, and every program started from
         Windows now runs UNDER it -- so a session-wide 5.00 would have changed the
         version every program sees. Instead, like DOS's own SETVER table, the version
         is answered PER PROCESS: a PSP listed here (an NTVDM-aware shell) is told
         shell_ver; everything else gets VersionMajor/VersionMinor, the Settings version. */
#define DOS_SHELL_PSP_SLOTS 4
    WORD ShellPsps[DOS_SHELL_PSP_SLOTS];   /* 0 = unused slot                                */
    BYTE  ShellVersionMajor, ShellVersionMinor;
    BYTE  AllocationStrategy;      /* AH=58h allocation strategy (0 = first fit)       */
    BYTE  UmbLink;         /* AH=58h UMB link state (0 = not linked)           */
    BYTE  IsBreakOn;         /* AH=33h extended Ctrl-Break checking (BREAK=)     */
    WORD SysvarsSegment, SysvarsOffset;  /* AH=52h list of lists, planted by the host */
    HANDLE   FindHandles[DOS_FIND_SLOTS];        /* AH=4Eh/4Fh live searches; slot stashed in the DTA */
    WORD LastError;         /* AH=59h extended error -- last failing call's AX   */
    BYTE  IsVerifyOn;           /* AH=2Eh/54h verify-after-write flag                */
    WORD ChildReturnCode;         /* AH=4Dh return code of the last child              */
    HANDLE   FcbFind;         /* AH=11h/12h FCB search in progress                  */
    BYTE  SwitchChar;      /* AH=37h -- oracle says '/' on 6.22                  */
    WORD PspSegment;          /* AH=50h/51h/62h -- the CURRENT process's PSP        */
    /* AH=4Bh EXEC.  DosInt21 only RECORDS the request; the host performs the
       load and the control transfer, because the loader, the file I/O and the
       guest's register frame all live there.  See exec_begin() in main.c. */
    INT      IsExecPending;
    /* ── GH #49: TSR RESIDENCY. AH=31h and INT 27h terminate the program but
         must NOT free its memory or unwind its interrupt vectors -- that is the
         whole of "stay resident". `TsrKeep` is the paragraph count the program
         asked to keep; `IsTsrPending` tells the host to take the resident exit
         path instead of the ordinary one. */
    INT      IsTsrPending;
    WORD TsrKeep;
    BYTE  ExecMode;        /* AL: 00 load+go, 01 load only, 03 overlay          */
    /* AL=01 and AL=03 both ANSWER through the caller's parameter block, so the
       host needs to find it again after the load. AL=01 writes the entry SS:SP
       and CS:IP back into it; AL=03 reads the load segment and relocation
       factor out of it. (GH #50) */
    WORD ExecBlockSegment, ExecBlockOffset;
    WORD ExecOverlaySegment, ExecOverlayRelocation;
    char     ExecPath[DOS_EXEC_NAME_SIZE];
    /* The name EXACTLY as the caller passed it in DS:DX. DOS 6.22 appends this,
       verbatim -- not qualified, not upcased -- to the child's environment copy
       after the 00 01 00; measured by p_exec/p_child (child.env.namekind). */
    char     ExecName[DOS_EXEC_NAME_SIZE];
    WORD ExecEnvironment;         /* 0 = inherit the parent's environment (a COPY)      */
    WORD ExecTailSegment, ExecTailOffset;
    WORD ExecFcb1Segment, ExecFcb1Offset;
    WORD ExecFcb2Segment, ExecFcb2Offset;
    char    *Output; INT OutputCapacity; INT OutputLength;  /* captured console output (02/09/40) */
    INT      IsOutputTruncated;        /* set when output was dropped -- see OUTC()        */
    BYTE  Unimplemented[DOS_SERVICE_BITS];     /* GH #27: DOS-defined services we have not written  */
    BYTE  Undefined[DOS_SERVICE_BITS];       /* GH #27: services 6.22 does not define either       */
    char    *TraceCursor;               /* current trace cursor (caller resets + flushes)   */
    INT      ExitCode;        /* AH=4Ch AL -- DOS errorlevel (read after the loop) */
    VOID   (*ConsoleOut)(PVOID context, BYTE character);  /* optional sink for console output  */
    PVOID ConsoleOutContext;           /* passed to conout (e.g. the video VDD)            */
    INT    (*ConsoleIn)(PVOID context); /* optional source for console input (blocking)     */
    PVOID ConsoleInContext;           /* passed to conin (e.g. the keyboard VDD)          */
    INT    (*ConsoleInNoWait)(PVOID context); /* non-blocking console read: char, or -1 if none */
    /* RETRY: set by a blocking service that has nothing to return yet. The host must then
       leave the guest's EIP ON the BOP so it re-executes the INT -- turning a host-side
       block into a guest-side poll. This matters enormously: blocking the exec thread in C
       stops the GUEST dead, so its timer stops, its music stops and its screen freezes until
       a key arrives. A real BIOS spins in the guest with interrupts enabled and the machine
       stays alive; now so do we. */
    INT    IsRetry;
    /* ── #251: AUX AND PRN ARE GUEST CODE THAT CALLS THE BIOS (dos_auxprn.h). ──
         `CanTrampoline` (in): the caller can resume the guest somewhere other than
         the stub's IRET -- only the V86 exec loops can. `Trampoline` (out): nonzero =
         resume at DOS_CTAB_SEG:Trampoline with the INT 21h frame still on the stack.
         In protected mode (or a caller that cannot), output falls back to `prnout` /
         `auxout`, the same devices without the IVT hop. */
    INT      CanTrampoline;
    WORD Trampoline;
    INT    (*PrinterOut)(PVOID context, BYTE character);   /* PRN byte -> LPT1; 0 = went nowhere */
    VOID   (*AuxOut)(PVOID context, BYTE character);   /* AUX byte -> COM1                   */
    PVOID DeviceContext;
    INT    (*ConsolePeek)(PVOID context); /* non-blocking status: 1 if a key is ready       */
    /* AH=0Ah (buffered input) IS A BLOCKING SERVICE THAT SPANS MANY RETRIES, so the
       line it is collecting has to survive them. The characters live in the GUEST's
       buffer, which is untouched between retries; what we need to remember is how far
       in we are, and WHICH buffer it was -- a different DS:DX means a different call,
       not a continuation. See the handler. */
    WORD LineSegment, LineOffset;
    INT      LineLength, IsLineActive;
    /* #251: AH=3Fh FROM THE CONSOLE IS DOS's OWN LINE EDITOR, and unlike AH=0Ah its
       line lives on DOS's side: it is read whole (127 characters + CR LF) and handed
       out across as many reads as the caller makes. ConsoleTyped counts what is typed while
       collecting; ConsoleLength/ConsolePosition are what is left to hand out. */
    BYTE     ConsoleLine[DOS_CONSOLE_LINE_SIZE];
    INT      ConsoleTyped, ConsoleLength, ConsolePosition, IsConsoleCollecting;
    INT      IsTraceAll;        /* log EVERY INT 21h call -- see the trace at entry */
    DWORD    TraceCount;          /* how many have been printed; capped at DOS_TRACE_MAX */
    /* THE CURRENT DRIVE WHEN WIN32 CANNOT STAND ON IT. -1 = the current drive is the
       process current directory's, as it always was. DOS selects a drive (AH=0Eh)
       from the CDS without touching the media -- oracle: `0Eh B:` on a one-floppy
       machine selects the phantom B: and 19h reads it back -- but Win32's
       SetCurrentDirectory("A:") on an empty floppy or CD-ROM drive says NOT READY.
       So a drive that exists (GetLogicalDrives) but cannot be entered is held here,
       19h/47h/36h answer for it, and every relative path is prefixed with it so the
       access fails on THAT drive the way DOS's would, instead of quietly landing on
       C:. QB.EXE's File dialog sizes its drive list by select-then-read-back, and
       listed one drive on a machine with four. (s72) */
    INT      VirtualDrive;
    /* WHICH OF THE FIVE STANDARD HANDLES ARE STILL OPEN (bits 0-4, set at startup).
       fh[0..4] are NULL because they are devices, not files, so "NULL" cannot also
       mean "free" for them -- and the difference is load-bearing. DOS hands out the
       LOWEST FREE handle, which is how `> file` works: the shell closes handle 1 and
       opens the target, and the target BECOMES handle 1. See AH=3Ch. */
    /* 32 bits, not 8: a device handle is not confined to slots 0-4. AH=45h can
       duplicate the console into any free slot, which is how a shell saves stdout
       before redirecting -- see DOS_DEV_SLOTS in dos_fh.h. */
    UINT32 StdOpen;
    /* ── ★★ EACH PROGRAM HAS ITS OWN HANDLE TABLE ON DOS. OURS IS ONE TABLE. (s81) ──
         fh[]/StdOpen are the machine's ONLY handle table, so a child that closed its
         handle 1 closed the SHELL's stdout: Doom's SETUP does exactly that, and XP's
         COMMAND.COM came back unable to print its prompt or the "Invalid handle" error
         about it -- a silent spin the user saw as a flashing cursor. On DOS the child
         works on a COPY (the PSP's JFT) and its closes are its own.
       ⇒ EXEC pushes the parent's table here; while a child runs, closing or dup2-ing
         over a Win32 handle the parent still holds only UNBINDS it; terminate closes
         whatever the child still has open (as DOS does) and pops the parent's table
         back. See DosHandlesPush/pop. */
#define DOS_HANDLE_STACK_DEPTH 8
    DOS_HANDLE_FRAME HandleStack[DOS_HANDLE_STACK_DEPTH];
    /* ── s91: THE PSP's JOB FILE TABLE, KEPT TRUTHFUL. fh[] above is what we use; the
         JFT (PSP:34h -> 20 bytes) is what a DOS program can SEE -- and XP's COMMAND.COM
         does `>` by editing it directly (JFT[1] = JFT[5], JFT[5] = FFh) rather than
         with AH=46h. So every handle we hand out is written into the JFT as a pseudo
         SFT index (0 AUX, 1 CON, 2 PRN, 3..FEh a host handle in SftHost[]), JftKnown
         remembers what we wrote, and at EXEC an entry that differs was edited by the
         program and is applied to the child's table (DosJftExec). */
    BYTE  JftKnown[DOS_PSP_JFT_HANDLES];
    HANDLE   SftHost[DOS_SFT_INDEXES];
    INT      HandleDepth;
    /* AH=11h/12h: the 11-byte template the live FCB search matches against (s81) --
       see DosFindMatches in dos_int21.c. */
    BYTE  FcbTemplate[DOS_FCB_NAME_SIZE];
    /* GH #250: AH=2Dh reloads the BIOS tick count (0040:006C) the way DOS's CLOCK$
       does. A hook rather than a store, because the pacer thread increments that
       dword under the PIT's own lock and a bare write could be lost between its read
       and its write. NULL (off-VM) = the ticks are left alone. */
    VOID   (*SetTicks)(PVOID context, UINT32 ticks);
    PVOID TicksContext;
    /* ── GH #34: INT 24h, THE CRITICAL-ERROR HANDLER. DosInt21 only DETECTS one
         (a disk call failing for a hardware reason: extended error 13h-1Fh) and
         records what the handler is to be told; the host makes the call into the
         guest and acts on the answer, because only the exec loop can redirect the
         guest's CS:IP and re-run, fail or end the call. See crit_raise in main.c. */
    INT      IsCritPending;     /* this call hit one: the host must raise INT 24h     */
    BYTE  CritAh;          /* INT 24h AH: bit 0 write, 1-2 area, 3-5 allowed     */
    BYTE  CritAl;          /* INT 24h AL: the drive, 0 = A:                      */
    BYTE  CritCode;        /* INT 24h DI: 0 write-protect .. 0Ch general failure */
    INT      IsCritActive;      /* the guest's handler is running: never re-raised    */
    BYTE  TermType;        /* AH of the next AH=4Dh: 0 normal, 2 critical abort  */
    /* #275 (in): the caller can raise INT 24h -- only the MAIN V86 exec loop can (it
       is the one that handles IsCritPending). The nested real-mode loops (a 0301h/
       0302h procedure, a reflected IRQ's handler) and every protected-mode caller
       leave it 0, and a 3Fh/40h hardware error there is answered as if the handler
       had said FAIL -- see the tail of DosInt21. */
    INT      CanRaiseCrit;
} DOS_MACHINE, *PDOS_MACHINE; typedef const DOS_MACHINE *PCDOS_MACHINE;

/* GH #250: the host's local time as fields, and the VDM's reading of a clock that is
   `off` centiseconds from it (g_DosClock.DosOffset / .RtcOffset). Win32 lives here, the
   arithmetic in dos_clock.h. */
VOID DosClockHostNow(_Out_ PDOS_CLOCK_TIME time);
VOID DosClockRead(_In_ INT64 offset, _Out_ PDOS_CLOCK_TIME out);
/* GH #262: the PIT's "was 0040:006C set by anything but the BIOS?" (vdd_pit_tick_take,
   wired by the host; NULL off-VM), DOS's clock re-derived from a count (DosClockFollowTicks
   on g_DosClock.DosOffset), and the two together -- called before any read or set of
   DOS's clock. */
extern INT (*g_DosTickTake)(_Out_ UINT32 *ticks, _Out_ UINT32 *wraps, _Out_ UINT32 *since);
VOID DosClockFollow(_In_ UINT32 ticks, _In_ UINT32 wraps, _In_ UINT32 since);
VOID DosClockSync(VOID);
/* GH #263: stamp a just-created or just-written file with DOS's clock (a no-op while no
   guest has moved it). Exported for main.c's protected-mode INT 21h twins. */
VOID DosStampVdmNow(_In_opt_ HANDLE file);

/* Zero the handle table, set the MCB root, default DTA = PSP:0x80. */
VOID DosInt21Initialize(_Out_ PDOS_MACHINE machine, _In_ WORD firstMcb);

/* Per-process handle tables, DOS-style (see DOS_MACHINE::HandleStack). push at EXEC,
   pop at the child's terminate; `tsr` = the child stays resident, so the files it
   still holds stay open (DOS does not close a TSR's handles). */
VOID DosHandlesPush(_Inout_ PDOS_MACHINE machine);
VOID DosJftExec(_Inout_ PDOS_MACHINE machine, _In_ WORD childPsp);   /* s91, see JftKnown */
VOID DosJftReset(_Inout_ PDOS_MACHINE machine);                       /* the first process's JFT */
VOID DosHandlesPop(_Inout_ PDOS_MACHINE machine, _In_ INT isTsr);
/* Close DOS handle `slot` in the current table -- for real only if no parent still
   holds the same Win32 handle. Use instead of CloseHandle(m->fh[slot]). */
VOID DosHandleRelease(_Inout_ PDOS_MACHINE machine, _In_ UINT slot);

/* Service one INT 21h BOP (function in AH). Returns 1 to continue the guest, 0 to
   terminate (AH=4Ch). Appends a trace via m->tp; writes console output to m->out. */
extern INT g_DosInt21IsProtectedMode;      /* 1 = client is in protected mode (DPMI) */
VOID DosInt21SetProtectedMode(_In_ INT isOn);  /* CF/ZF -> live VTIB_EFLAGS, not a V86 FLAGS frame */
/* The reported DOS version, which is a LIE THE GUEST GETS TO CHOOSE -- real DOS has
   SETVER for exactly this. Default is 6.22 (the oracle), and that default is what
   XP's own COMMAND.COM refuses: it prints "Incorrect DOS version" and terminates,
   because NT's DOS has always reported 5.00 and its shell is built to match.
   Call before DosInt21Initialize's defaults are wanted, or any time after. */
VOID DosInt21SetVersion(_Inout_opt_ PDOS_MACHINE machine, _In_ BYTE major, _In_ BYTE minor);
/* #208: the process at `psp` is an NTVDM-aware shell and is answered 5.00 by AH=30h
   and AX=3306h (on=1), or no longer is (on=0, e.g. at its terminate). */
VOID DosInt21SetShellPsp(_Inout_ PDOS_MACHINE machine, _In_ WORD psp, _In_ INT isOn);
/* The current drive, 0 = A:. Published because the NTVDM `BOP 0x54 sub 01` answer has
   to carry it (COMMAND.COM reads it out of the reply block and immediately issues
   AH=0Eh with it) -- and the host must not re-derive the same policy separately, which
   is how `vdrive` would have been silently dropped. */
BYTE DosInt21CurrentDrive(_In_ PCDOS_MACHINE machine);

/* ── INT 21h AH=53h, THE PRIVATE SUB-FUNCTIONS, AS A TABLE RATHER THAN A SWITCH. ──
     Documented AH=53h is BPB->DPB and has no AL selector; NT's NTDOS.SYS overloads it
     as a private query and XP's COMMAND.COM reads the answer out of AL. The defaults
     here are the values MEASURED against stock ntvdm (see the handler), but that
     measurement was taken by a probe whose output was REDIRECTED TO A FILE, and at
     least one of these sub-functions is suspected of depending on exactly that -- so
     the table is a knob (`cfg\int53.txt`) and not a constant. Index = AL, 0..7; AL>7
     keeps DOS's "invalid function" (AX=1, CF=1). */
typedef struct _DOS_INT53_ANSWER { WORD Ax; BYTE IsCarry; } DOS_INT53_ANSWER;
#define DOS_INT53_COUNT 8
extern DOS_INT53_ANSWER g_DosInt53Answers[DOS_INT53_COUNT];

INT DosInt21(_Inout_ PDOS_MACHINE machine);

#endif /* DOS_INT21_H */
