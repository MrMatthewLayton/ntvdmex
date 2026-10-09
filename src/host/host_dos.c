/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * The DOS side of the host: EXEC and termination, INT 24h, disks, stdio and the
 *   console, and the XMS/EMS host calls.
 *
 * Its own translation unit (#335): declared in host_dos.h.
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#include "host_dos.h"
#include <tlhelp32.h>
#include "host_state.h"
#include "log.h"
#include "main.h"
#include "host_bios.h"
#include "host_mouse.h"
#include "host_timing.h"
#include "host_video.h"
#define HMA_ERROR_PROTECTED         0xE1                /* g_HmaError: committed but not accessible */

/* ---- INT 21h AH=4Bh EXEC.  GH #30. ------------------------------------------
 *
 * A parent calls EXEC, a child runs to completion, and the parent carries on at
 * the instruction after its INT 21h with the child's exit code retrievable via
 * AH=4Dh.  That is what turns COMMAND.COM from a prompt into a shell.
 *
 * The work is split: DosInt21 only RECORDS the request, because the loader, the
 * file I/O and the guest's register frame all live out here.
 *
 * HOW THE RETURN WORKS, which is the part worth understanding.  The parent
 * entered through `INT 21h`, so the CPU pushed FLAGS/CS/IP on the parent's stack
 * and we are executing inside our BOP stub.  We snapshot the parent's ENTIRE
 * register frame -- including CS:IP pointing AT the BOP and SS:SP pointing at
 * that IRET frame -- then overwrite the frame with the child's entry state.
 * When the child terminates we put the parent's frame back and step EIP past the
 * BOP, so the stub's IRET pops the parent's own frame and lands exactly where it
 * would have if EXEC had simply returned.  No stack is unwound by hand.
 */
#define EXEC_MAX_DEPTH              8

#define EXEC_SHELL_BOP_SITES_MIN    8                   /* This many C4 C4 54 sites mark XP's COMMAND.COM (it has 15) */

/* DISK IMAGES BEHIND INT 13h / INT 25h. (GH #44):
 * A drive is an image FILE or it is absent -- see src/dos/dos_disk.h for why
 * nothing is synthesised. Drive 0 (A:) comes from FLOPPY_IMG_PATH.
 *
 * [CAUTION]: OPENED LAZILY, ON THE FIRST DISK CALL, AND NEVER AT STARTUP. Opening media
 * at startup is how the LPT1 spool wedged the rig this morning: a blocking
 * Win32 call before the host has a window presents as a hang, not an error.
 * SetErrorMode for the same reason. A guest that never touches INT 13h never
 * pays for this and never risks it.
 */
#define FLOPPY_IMG_PATH             CFG_("FLOPPY.IMG")  /* Read, not written -> cfg */

/* THE UNDOCUMENTED PAIR OF OFFSETS THIS ROUTE RESTS ON (x86):
 * PEB                          +0x10  ProcessParameters
 * RTL_USER_PROCESS_PARAMETERS  +0x18  StandardInput
 *                              +0x1c  StandardOutput
 *                              +0x20  StandardError
 *
 * [CAUTION]: NOT TRUSTED, CHECKED. StdioPebStdout() below reads them out of THIS
 * process and compares with GetStdHandle before they are used on another.
 */
#define PEB_OFF_PROCESSPARAMS       0x10
#define RUPP_OFF_STDIN              0x18
#define RUPP_OFF_STDOUT             0x1c

/* THE PARENT'S OWN STANDARD OUTPUT, DUPLICATED. (GH #131) (Importance = 5):
 * Called only after the five earlier routes have failed. See the long note at
 * the call site for why the handle is in the parent and not in us.
 *
 * [CAUTION]: THE OFFSETS ARE VALIDATED BY THE DATA THEY READ, which is the only form of
 * proof available for an undocumented structure at run time -- and it is a
 * STRONGER one than checking our own process, because it validates them on the
 * EXACT process we are about to duplicate a handle out of. The parameters
 * block carries the process's own IMAGE PATH; if reading it at the offset this
 * code believes in produces a string whose file name is the one the process
 * list reports for that pid, then this really is that structure. If it does
 * not, we refuse and say so, so a wrong constant costs a log line rather than
 * a handle to something else entirely.
 *
 * [CAUTION]: AND THE DUPLICATED HANDLE IS TYPE-CHECKED BEFORE IT IS ADOPTED. GetFileType
 * is the test the rest of this file already uses: a handle whose type we can
 * name is one we can write to.
 *
 * [CAUTION]: A CONSOLE HANDLE IS TAKEN TOO, and that is not a lesser answer -- a DOS
 * program run at a prompt with no redirect should write to that prompt, which
 * is the same defect wearing different clothes.
 */
#define RUPP_OFF_IMAGEPATH          0x38                /* UNICODE_STRING; Buffer at +4 */

/* #153: TEXT THE HOST TYPES AT THE SHELL'S PROMPT (File > Open Executable):
 * Characters queued here are read by the console input BEFORE the keyboard, so the
 * shell's AH=0Ah line reader sees them exactly as if they had been typed -- echoed,
 * with no length limit and no scancode translation. Only File > Open fills it, and
 * only while the top-level shell is sitting in that line read (OpenAtPrompt).
 * Head/tail under g_Lock.
 */
#define TYPEIN_CAP                  512

enum
{
    EXEC_WINDOWS_POLL_MS = 100
};   /* ExecWindows: waiting for a console child */

enum
{
    DOS_TERMINATE_MCB_GUARD = 1024
};   /* DosTerminate: blocks freed before giving up */

typedef LONG (WINAPI *PFN_NT_QUERY_INFORMATION_PROCESS)(HANDLE, ULONG, PVOID,
                                                     ULONG, PULONG);

/* #208: A PROGRAM STARTED FROM WINDOWS RUNS UNDER XP's COMMAND.COM, AS STOCK DOES:
 * Stock ntvdm never loads the program itself: it starts COMMAND.COM /P and answers the
 * shell's first BOP 54 sub 01 ("what next?") with the program. That is what puts a
 * shell UNDER every program -- and File > Close Program needs one to return to. The
 * shell is told 5.00 (it demands it); the program gets the Settings version (the
 * user's choice, SETVER-style -- see g_shell_psp).
 */
INT   g_Routed;                  /* this run's program is handed over via sub 01 */
INT   g_BackToPrompt;          /* Close Program ended it: next sub 01 = prompt */

/* #233: the AWE32's EMU8000 at SB base + 400h/800h/C00h, fitted when the model is AWE32. */
EMU8K_STATE  g_Emu8K;     NTVDD_DEVICE g_Emu8KDevice;
INT          g_AweOn = 0;
MPU_STATE    g_Mpu;       NTVDD_DEVICE g_MpuDevice;
PVOID g_Hma;       /* the HMA at linear 0x100000, 0 = unavailable */
DWORD        g_HmaError;   /* why not, when g_Hma == 0 */

/* Make the HMA real: one committed 64KB range at linear 0x100000, which a guest
 * reaches as FFFF:0010 because in this design a guest linear IS a host VA.
 *
 * [CAUTION]: Called ONCE and EARLY so the answer can ride the STAGE0 preamble, which is a
 * single buffered flush. Logged as its own later append it simply VANISHED --
 * LogAppend caches one handle per path with no lock, so a concurrent writer can
 * lose or misplace a line. Chasing that cost a cycle here, and the same defect
 * had already put a stray SysVars line at byte 0 of the log.
 */
/* what was already at 0x100000 */
DWORD g_HmaState;
DWORD g_HmaProtection;

DOS_EMS_STATE    g_Ems;       /* M4: EMS expanded-memory manager */

/* #167: Settings > General "behave like" = MS-DOS 6.22 (0 = Windows XP NTVDM, the default).
 * Mirrored from g_Settings by SettingsApply, for the device code that runs before it.
 */
INT            g_BehaveDos622 = 0;

/* AND THE DRIVES PAGE CAN POINT IT SOMEWHERE ELSE:
 * SettingsApply() aims this at the FloppyAImage setting when one is stored.
 *
 * [CAUTION]: IT IS A POINTER, NOT A COPY, AND IT POINTS INTO g_Settings -- which lives for
 * the process. A copy here would be a second place for the path to be, and
 * the first thing a second place does is go stale.
 *
 * [CAUTION]: THE HARNESS PATH STAYS THE FALLBACK. The rig drops FLOPPY.IMG at the
 * literal above and re-launches; if an empty setting overrode that, every
 * headless disk measurement would start reporting "drive not ready" on a
 * machine where nothing had changed.
 */
/* NULL = the harness fallback (FLOPPY_IMG_PATH), composed at use because the root is
 * runtime-derived now; a settings value points this INTO g_Settings as before.
 */
PCSTR g_FloppyImage = NULL;

/* THE REAL STANDARD OUTPUT, IF WE WERE LAUNCHED FROM ONE. (GH #131):
 * Everything a DOS guest printed went to the video VDD and, at exit, to
 * CONOUT$ -- so `myprog.exe > out.txt` from cmd.exe captured NOTHING, and
 * nothing appeared until the program ended. That is the blocker on leaving
 * NTVDMEX installed as the machine's VDM: anything script-driven changes
 * behaviour.
 * - THE IFEO HOOK IS WHY THIS CAN WORK AT ALL. Windows launches us IN PLACE of
 *   ntvdm.exe, so our parent is whatever ran ntvdm -- cmd.exe -- and we inherit
 *   ITS standard handles. If the user redirected, the inherited handle is
 *   already the file: no console involved, and nothing to attach to.
 * - TWO CASES, IN THIS ORDER:
 *   1. an inherited handle that is a FILE or a PIPE -- redirection. Use it.
 *   2. otherwise AttachConsole(ATTACH_PARENT_PROCESS) and open CONOUT$, so an
 *      unredirected run prints inline in the console it was started from.
 *   A GUI-subsystem process has no console of its own, which is exactly why (2)
 *   is needed and why it must not be attempted before (1) -- attaching would
 *   hand us a console handle and hide the redirect.
 *
 * [CAUTION]: BOTH OF THOSE ARE MEASURED DEAD (session 53), along with two more: CSRSS
 * leaves StartupInfo.dwFlags at 0, and attaching to the REAL parent pid found
 * by hand through Toolhelp fails the same way ATTACH_PARENT_PROCESS does.
 * The premise above is simply wrong: an IFEO-substituted VDM is not created
 * the way a child is, so there is nothing to inherit and nothing to attach to.
 *
 * [CAUTION]: AND THE FIFTH ROUTE IS DEAD TOO, MEASURED THE SAME DAY. VDM_COMMAND_INFO
 * carries StdIn/StdOut/StdErr at +0x10, filled by GetNextVDMCommand and already
 * duplicated into this process by CSRSS -- which is how stock ntvdm gets a
 * console it never inherited. It is tried in StdioInitializeVdm() below and the
 * three fields come back as NON-HANDLES: 0x02341fc0 / 0x7c867f23 / 0x65470000,
 * the same values every run, all three FILE_TYPE_UNKNOWN.
 *
 * [INFO]: THE INSTRUMENT THAT SAID SO NAMED THE REAL BUG, AND IT IS NOT A STDIO BUG.
 * Those fields are junk because the WHOLE STRUCT is: CreationFlags reads
 * 0x4d445674 and CodePage 0x78654e74 -- ASCII, identical on every run --
 * AppName is garbage, CmdLine is "\", and TaskId is 0. GetNextVDMCommand
 * returns TRUE and populates nothing. We have never noticed because the launch
 * does not depend on it: the program comes from target.txt or from our own
 * command line.
 *
 * And the command line says why: `ntvdmhost.exe "...\ntvdm.exe" -f`. There is
 * NO `-i<taskid>`, so CsrssParseTaskId() yields 0, so CSRSS cannot tell
 * which queued task we are asking about, so it answers with nothing. The
 * shape that DOES carry `-i` (anything launched through `start`, which is
 * every run rt.bat has ever done) reports TaskId 0x18 -- and that is exactly
 * the shape whose console we do inherit, which is how this stayed invisible.
 *
 * SO #131 IS THE SAME DEFECT AS THE M2.5 OPEN ITEM `recover the real command
 * line from CSRSS's undocumented multi-call GetNextVDMCommand protocol`. Not a
 * Win32 handle problem, not a subsystem problem: we are not participating in
 * the VDM handshake, and the handles and the command line are both on the far
 * side of it. Fixing one fixes the other.
 *
 * [CAUTION]: NOT a subsystem problem -- that hypothesis is REFUTED, not doubted. This
 * binary is already CUI/console, pinned deliberately in CMakeLists.txt, exactly
 * like ntvdm.exe.
 * - And the target is known to be reachable, because stock ntvdm was asked on the
 *   same box with the same program and the same command: `hello.com > out.txt`
 *   under stock writes 136 bytes into the file and under NTVDMEX writes 0.
 *   The code below stays: it costs nothing, it upgrades if a handle ever does
 *   arrive, and the raw-handle line it prints at STAGE1 is the instrument that
 *   turned four sessions of hypotheses into one located bug.
 */
HANDLE g_Stdio = INVALID_HANDLE_VALUE;
static struct
{
    DWORD Eax;
    DWORD Ebx;
    DWORD Ecx;
    DWORD Edx;
    DWORD Esi;
    DWORD Edi;
    DWORD Ebp;
    DWORD Esp;
    DWORD Eip;
    DWORD Eflags;
    WORD Cs;
    WORD Ss;
    WORD Ds;
    WORD Es;
    WORD Psp;
    WORD DtaSegment;
    WORD DtaOffset;
    WORD  ChildSegment;                  /* freed when the child terminates */
    WORD  EnvironmentSegment;                    /* the env COPY we made for it, if any; freed with it */
} g_Exec[EXEC_MAX_DEPTH];
/* FILE > CLOSE PROGRAM ENDS A CHILD THAT NEVER ASKED TO END. (GH #152):
 * A program that exits unhooks what it hooked; one we END does not. Its INT 08h/09h
 * would go on pointing into the block DosTerminate frees, and the shell would die
 * on the next tick or keypress -- so EXEC photographs what the child can break (the
 * IVT, the PIC masks, the video mode, the PIT period) and a forced close puts it
 * back before the ordinary child terminate runs. Only a FORCED close restores: a
 * program's own exit is DOS's business, and DOS does not do this.
 */
static struct
{
    WORD  Ivt[IVT_SIZE / X86_WORD_SIZE];                   /* 0000:0000-03FF as the parent left it */
    BYTE ImrMaster;
    BYTE ImrSlave;
    BYTE VideoMode;
    DWORD Pit0;                       /* channel 0's effective reload */
    CHAR  ProgramName[64];               /* the PARENT's status-strip name (user, s84) */
} g_ExecMachine[EXEC_MAX_DEPTH];
static BYTE g_ExecFileBuffer[0x80000];    /* child image; separate from the parent's */

/* GH #34: INT 24h, THE CRITICAL-ERROR HANDLER (Importance = 1):
 * A disk call that failed for a HARDWARE reason (extended error 13h-1Fh: write-
 * protected, not ready, CRC, ... general failure) is not simply returned on DOS: DOS
 * calls the program's INT 24h with AH = what it was doing and which answers are
 * allowed, AL = the drive, DI = the error, BP:SI = the device header, and does what
 * the handler says in AL -- 0 IGNORE, 1 RETRY, 2 ABORT, 3 FAIL. That is the "Not
 * ready reading drive A / Abort, Retry, Fail?" prompt (COMMAND.COM's handler), and
 * a program that installs its own (every editor that saves to floppy) decides for
 * itself. Nothing of it existed: the error went straight back to the caller.
 * - HOW: INT 21h is serviced here, host-side, so the call into the guest is made by
 *   redirecting the guest. CriticalSnapshot keeps the INT 21h call's INPUT registers;
 *   DosInt21 sets m->crit_pending instead of finishing; CriticalRaise points CS:IP at
 *   DOS_CRIT_RAISE (`int 24h / bop 20h`) with the handler's registers loaded; the
 *   handler IRETs onto the BOP and CriticalReturn puts the inputs back and acts:
 *   RETRY  -> CS:IP back ON the INT 21h BOP, so the whole call is made again;
 *   FAIL   -> the call returns CF=1 with the measured AX, 59h then says 53h;
 *   IGNORE -> not allowed for a path call (AH bit 5 clear, measured), so FAIL;
 *             allowed for 3Fh/40h on an open file (#275): the call "succeeds";
 *   ABORT  -> the program ends (AH=4Dh AH=02h), through DosTerminate.
 *   An answer the handler was not allowed to give is converted as DOS converts it:
 *   ignore/retry -> fail, fail -> abort.
 *
 * [CAUTION]: MAIN V86 LOOP ONLY (m.crit_raise_ok, #275). A DPMI client's INT 21h is serviced
 * with the client in protected mode, and the nested real-mode loops cannot redirect
 * the guest the way this does; there a 3Fh/40h hardware error is answered as FAIL
 * and a path call keeps the raw code (see the tail of DosInt21). And never while
 * a handler is already running: DOS does not nest INT 24h.
 */
static struct
{
    DWORD Eax;
    DWORD Ebx;
    DWORD Ecx;
    DWORD Edx;
    DWORD Esi;
    DWORD Edi;
    DWORD Ebp;
    DWORD Eip;
    WORD Cs;
    WORD Ds;
    WORD Es;
    BYTE  Ah;                                  /* what the handler was told it may answer */
    BYTE  Function;                                  /* the INT 21h function that failed */
} g_Critical;

static HANDLE        g_DiskHandle[1] = { INVALID_HANDLE_VALUE };
static DOS_DISK_GEOMETRY g_DiskGeometry[1];
static INT           g_DiskTried[1];
static CHAR   g_StdioBuffer[512];
static UINT g_StdioLength = 0;

/* THE INPUT SIDE, AND IT IS THE SAME DEFECT. (GH #131, session 57) (Importance = 2):
 * `prog < file` is the mirror of `prog > file`: the handle is in the parent
 * and not in us, for exactly the same reason. Adopted only when it is a FILE
 * or a PIPE -- a character handle is a console, and a console's input is the
 * keyboard, which this host already has a whole VDD for. Taking that would
 * replace a working path with a worse one.
 *
 * [CAUTION]: AND AT END OF FILE, DOS SAYS Ctrl-Z. A redirected read that runs out does
 * not block and does not fail: it returns 0x1A, which is what every DOS
 * program written since 1981 tests for.
 */
static HANDLE g_StdinHandle = NULL;
static INT    g_StdinEof = 0;
static DWORD  g_StdinBytes = 0;

/* DOS console input (INT 21h AH=01/07/08/0A) -> the keyboard VDD ring, blocking
 * on the V86 thread until the UI thread pushes a key (or the window closes).
 */
/* HOW DOS HANDS OVER AN ARROW. A BIOS keycode is a PAIR (AH=scancode, AL=ascii), but every
 * INT 21h console read returns ONE byte. For the keys with no ascii -- arrows, F-keys, the
 * nav cluster, i.e. AL=0 -- DOS returns 0x00 first and the SCANCODE on the NEXT call, and a
 * program reading arrows is written to expect exactly that. We returned `k & 0xFF` and threw
 * the scancode away, so an arrow arrived as a lone NUL that never had a second half: every
 * extended key was unreadable through DOS. That is the Skyroads menu, which sits in INT 21h
 * (measured: the guest parks at DOS_HDLR_SEG:0000, the INT 21h BOP, for the whole run).
 * g_ConsoleInPending holds that second byte between the two calls.
 */
static INT g_ConsoleInPending = -1;                /* scancode owed to the next read, or -1 */
static CHAR g_TypeIn[TYPEIN_CAP];
static INT g_TypeInHead;
static INT g_TypeInTail;

/* Used before their definitions below. */
static VOID ExecMachineSave(INT depth);

/* THE COMPILER VARIABLES REACH THE GUEST, WITHOUT MOVING THE MEMORY MAP. (s73) (Importance = 1):
 * A DOS build tool is configured through its environment -- LIB and INCLUDE for the
 * Microsoft tool chain -- and we built a fixed block (COMSPEC PATH PROMPT BLASTER),
 * so `set LIB=...` then `LINK` in a cmd window could not find BCOM45.LIB while the
 * same two lines under stock could: "QuickBASIC cannot build EXEs" from a batch file.
 *
 * [WARNING]: THE FIRST FIX RELOCATED THE WHOLE ENVIRONMENT TO THE TOP OF MEMORY (a big
 * PSP-owned MCB, PSP:2C repointed) and BROKE EVERY DOS EXTENDER: Doom, Zar and
 * Wolf3d took a PM #GP at startup (Error [35], ~400 ms, before any mode set) --
 * user-confirmed, and confirmed fixed by rolling that change out. DOS/4GW reads the
 * memory map we moved. So this NEVER moves the memory map: it feeds the two compiler
 * variables into the EXISTING 256-byte block at 0x60 (DosEnvBuildWithCard, bounded to
 * the cap that stops short of the 0x714 landmine), exactly where the four defaults
 * and dosenv.txt already live. A game -- which sets no LIB -- gets a byte-identical
 * block and an unchanged memory map; only a caller that SET LIB/INCLUDE sees them.
 * - Values are shortened to 8.3 (a 1988 linker cannot read a long path), and returned
 *   in DosEnvBuildWithCard's `extra` format (NAME=VALUE, newline-separated). A value
 *   containing ';' is not split here -- LIB/INCLUDE for the tool chain are single
 *   directories; a multi-dir LIB is not the case this serves.
 */
static INT DosEnvironmentNameIs(PCSTR name, UINT length, PCSTR literal)
{
    UINT index;

    for (index = 0; index < length; ++index)
    {
        CHAR character = name[index];
        if (character >= 'a' && character <= 'z')
            character = (CHAR)(character - ASCII_CASE_BIT);
        if (character != literal[index])
            return 0;
    }
    return literal[length] == 0;
}

/* Scan a Win32 environment block for LIB= and INCLUDE=, 8.3-shorten each value, and
 * write them as newline-separated NAME=VALUE lines into out (for DosEnvBuildWithCard's
 * `extra`). Returns the number written. Nothing is written for a var that is absent.
 */
UINT LauncherCompilerVariables(
    PCSTR environment,
    DWORD environmentCapacity,
    PSTR out,
    DWORD outCapacity)
{
    static PCSTR want[] = { "LIB", "INCLUDE", NULL };
    PCSTR entry = environment;
    UINT count = 0;
    DWORD outLength = 0;
    if (out && outCapacity)
        out[0] = 0;
    if (!environment || environmentCapacity < 2 || !environment[0] || !out)
        return 0;
    while (entry < environment + environmentCapacity && *entry)
    {
        PCSTR equals = entry;
        UINT nameLength;
        UINT wanted;
        while (*equals && *equals != '=')
            ++equals;
        if (*equals != '=' || equals == entry)
        {
            entry += lstrlenA(entry) + 1;
            continue;
        }
        nameLength = (UINT)(equals - entry);
        for (wanted = 0; want[wanted]; ++wanted) if (DosEnvironmentNameIs(entry, nameLength, want[wanted]))
        {
            CHAR shortValue[512];
            PCSTR value = equals + 1;
            DWORD shortLength = GetShortPathNameA(value, shortValue, sizeof shortValue);
            UINT valueLength;
            UINT need;
            if (shortLength && shortLength < sizeof shortValue)
                value = shortValue;
            valueLength = (UINT)lstrlenA(value);
            need = (UINT)lstrlenA(want[wanted]) + 1 + valueLength + 1;   /* NAME=VALUE\n */
            if (outLength + need + 1 < outCapacity)
            {
                outLength += (DWORD)wsprintfA(out + outLength, HOST_ENV_LINE_FORMAT, want[wanted], value);
                ++count;
            }
            break;
        }
        entry += lstrlenA(entry) + 1;
    }
    return count;
}

VOID HmaTry(VOID)
{
    MEMORY_BASIC_INFORMATION memoryInfo;
    PVOID want = (VOID *)(ULONG_PTR)DOS_HMA_BASE_U;

    /* ASK WHAT IS THERE BEFORE ASKING FOR IT:
     * The first cut went straight to VirtualAlloc(MEM_RESERVE|MEM_COMMIT) and
     * got ERROR_INVALID_ADDRESS (0x1E7) -- which says "something already owns
     * this", not "you may not have it", and those need opposite responses. If
     * NT has already mapped the VDM's HMA then the memory is real and all we
     * were ever missing was the nerve to report it; if it is merely reserved,
     * it needs committing; only if it is free do we allocate.
     */
    if (VirtualQuery(want, &memoryInfo, sizeof memoryInfo) != sizeof memoryInfo)
    {
        g_HmaError = GetLastError();
        return;
    }
    g_HmaState = memoryInfo.State;
    g_HmaProtection = memoryInfo.Protect;
    if (memoryInfo.State == MEM_COMMIT)
    {
        if (memoryInfo.Protect & (PAGE_NOACCESS | PAGE_GUARD))
        {
            g_HmaError = HMA_ERROR_PROTECTED;
            return;
        }
        g_Hma = want;                       /* already ours -- nothing to do */
        return;
    }
    g_Hma = VirtualAlloc(want, DOS_HMA_ALLOCATION_U,
                         (memoryInfo.State == MEM_RESERVE) ? MEM_COMMIT
                                                    : (MEM_COMMIT | MEM_RESERVE),
                         PAGE_READWRITE);
    if (g_Hma != want)
    {
        g_HmaError = GetLastError();
        g_Hma = 0;
        return;
    }
    { UINT index;
    volatile BYTE *hma = (volatile BYTE *)(ULONG_PTR)DOS_HMA_BASE_U;
      for (index = 0; index < DOS_HMA_ALLOCATION_U; ++index)
          hma[index] = 0; }
}

/* GH #255: EXEC OF A WINDOWS PROGRAM GOES TO WINDOWS, AS ON STOCK NTVDM (Importance = 1):
 * `kind` from DosExeKind. CreateProcess does what stock's EXEC does with a
 * non-DOS binary: Windows itself routes it -- a PE to Win32, an NE to WOW (which,
 * with NTVDMEX installed, is another NTVDMEX). Command line = the quoted path plus
 * the DOS command tail; the working directory is the process's, which is the DOS
 * current directory.
 * - WAIT FOR A CONSOLE PROGRAM, NOT FOR A WINDOW. EXEC is synchronous by contract
 *   and AH=4Dh then has the child's exit code -- so a console PE is waited for (in
 *   its own console window: our DOS screen is not a console it can write to). A GUI
 *   PE or an NE is started and EXEC returns at once, as cmd.exe does: a DOS shell
 *   frozen until Notepad closes would be the surprising answer.
 *
 * [CAUTION]: UNMEASURED AGAINST STOCK -- that needs a supervised stock run (stock.sh drops
 * the IFEO key). The wait/no-wait split is a decision, flagged on #255.
 * Returns 1 if Windows took it (EXEC succeeds, child rc in m->child_rc); 0 if it
 * would not -- e.g. a "PE" that is a DOS extender's 32-bit image Windows refuses --
 * and the caller then runs the MZ stub, which is what DOS would have done.
 */
static INT ExecWindows(DOS_MACHINE *machine, INT kind, UINT subsystem, PSTR *logCursor)
{
    CHAR command[MAX_PATH + 160];
    STARTUPINFOA startupInfo;
    PROCESS_INFORMATION processInfo;
    const volatile BYTE *tail = (const volatile BYTE *)
        (((DWORD)machine->ExecTailSegment << PARAGRAPH_SHIFT) + machine->ExecTailOffset);
    INT tailLength = tail[0] > DOS_PSP_COMMAND_TAIL_MAX ? DOS_PSP_COMMAND_TAIL_MAX : tail[0];
    INT index;
    INT wait = (kind == DOS_EXE_PE && subsystem == IMAGE_SUBSYSTEM_WINDOWS_CUI);
    PSTR cursor = command;
    DWORD exitCode = 0;

    *cursor++ = '"';
    cursor = LogPut(cursor, machine->ExecPath);
    *cursor++ = '"';
    for (index = 0; index < tailLength && tail[1 + index] != DOS_PSP_COMMAND_TAIL_END; ++index)
        *cursor++ = (CHAR)tail[1 + index];
    *cursor = 0;
    for (index = 0; index < (INT)sizeof startupInfo; ++index)
        ((PSTR)&startupInfo)[index] = 0;
    startupInfo.cb = sizeof startupInfo;
    if (!CreateProcessA(NULL, command, NULL, NULL, FALSE,
                        wait ? CREATE_NEW_CONSOLE : 0, NULL, NULL, &startupInfo, &processInfo))
    {
        *logCursor = LogPut(*logCursor, "  EXEC: Windows would not start it (error 0x");
        *logCursor = LogHex(*logCursor, GetLastError());
        *logCursor = LogPut(*logCursor, ") -- running its MZ stub, as DOS would\r\n");
        return 0;
    }
    *logCursor = LogPut(*logCursor, kind == DOS_EXE_NE ? "  EXEC: a Windows (NE) program -> handed to Windows/WOW"
                                       : "  EXEC: a Win32 (PE) program -> handed to Windows");
    *logCursor = LogPut(*logCursor, wait ? ", console: waiting for it\r\n" : ", started, not waited for\r\n");
    if (wait)
    {
        while (WaitForSingleObject(processInfo.hProcess, EXEC_WINDOWS_POLL_MS) == WAIT_TIMEOUT)
            if (!g_Running || g_WoundDown)
                break;                                  /* the host is closing */
        if (!GetExitCodeProcess(processInfo.hProcess, &exitCode) || exitCode == STILL_ACTIVE)
            exitCode = 0;
        *logCursor = LogPut(*logCursor, "  EXEC: it exited, rc=0x"); *logCursor = LogHex(*logCursor, exitCode); *logCursor = LogPut(*logCursor, "\r\n");
    }
    CloseHandle(processInfo.hThread);
    CloseHandle(processInfo.hProcess);
    machine->ChildReturnCode = (WORD)(exitCode & BYTE_MASK);                 /* AH=4Dh: AH=0 normal end */
    return 1;
}

static VOID ExecMachineSave(INT depth)
{
    UINT index;

    for (index = 0; index < IVT_SIZE / X86_WORD_SIZE; ++index)
        g_ExecMachine[depth].Ivt[index] = PeekWord(index * X86_WORD_SIZE);
    g_ExecMachine[depth].ImrMaster = g_Pic.Master.Imr;
    g_ExecMachine[depth].ImrSlave = g_Pic.Slave.Imr;
    g_ExecMachine[depth].VideoMode = *(volatile BYTE *)(ULONG_PTR)(BIOS_BDA_BASE + BIOS_BDA_VIDEO_MODE);   /* BDA current mode */
    g_ExecMachine[depth].Pit0  = VddPitEffectiveReload(&g_Pit);
}

/* Perform a recorded EXEC: load the child, snapshot the parent, hand over. */
PSTR ExecBegin(DOS_MACHINE *machine, volatile BYTE *tib, PSTR cursor)
{
    HANDLE fileHandle;
    DWORD bytesRead = 0;
    WORD child = 0;
    WORD maximumParagraphs = 0;
    WORD want;
    WORD environmentSegment = 0;
    WORD environmentBlock = 0;
    DOS_IMAGE image;
    volatile WORD *flagsPointer;
    INT depth = g_ExecDepth;
    INT loadHigh = 0;

    flagsPointer = (volatile WORD *)((VDM_REG16(tib, VTIB_SS) << PARAGRAPH_SHIFT)
           + ((VDM_REG16(tib, VTIB_ESP) + X86_FRAME16_FLAGS) & WORD_MASK));

    cursor = LogPut(cursor, "  EXEC: \""); cursor = LogPut(cursor, machine->ExecPath); cursor = LogPut(cursor, "\"\r\n");

    if (depth >= EXEC_MAX_DEPTH)
    {
        cursor = LogPut(cursor, "  EXEC: nesting limit reached\r\n");
        VDM_REG(tib, VTIB_EAX) = (VDM_REG(tib, VTIB_EAX) & HIGH_WORD_MASK_U) | DOS_ERR_INSUFFICIENT_MEMORY;
        *flagsPointer |= EFLAGS_CF;
        VDM_REG(tib, VTIB_EIP) += VDM_BOP_LENGTH;
        return cursor;
    }
    fileHandle = CreateFileA(machine->ExecPath, GENERIC_READ, FILE_SHARE_READ, NULL,
                     OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (fileHandle == INVALID_HANDLE_VALUE)
    {
        cursor = LogPut(cursor, "  EXEC: file not found\r\n");
        VDM_REG(tib, VTIB_EAX) = (VDM_REG(tib, VTIB_EAX) & HIGH_WORD_MASK_U) | DOS_ERR_FILE_NOT_FOUND;
        *flagsPointer |= EFLAGS_CF;
        VDM_REG(tib, VTIB_EIP) += VDM_BOP_LENGTH;
        return cursor;
    }
    ReadFile(fileHandle, g_ExecFileBuffer, sizeof(g_ExecFileBuffer), &bytesRead, NULL);
    CloseHandle(fileHandle);

    /* GH #255: a Windows program is Windows's to run (see ExecWindows). Load-and-go
     * only: AL=01 asks for an IMAGE in memory and AL=03 for an overlay, and for those
     * the MZ part is the only thing DOS could give.
     */
    if (machine->ExecMode == DOS_INT21_EXEC_LOAD_AND_GO)
    {
        UINT subsystem = 0;
        INT kind = DosExeKind(g_ExecFileBuffer, bytesRead, &subsystem);
        if (kind != DOS_EXE_DOS && ExecWindows(machine, kind, subsystem, &cursor))
        {
            VDM_REG(tib, VTIB_EAX) &= HIGH_WORD_MASK_U;
            *flagsPointer &= (WORD)~EFLAGS_CF;
            VDM_REG(tib, VTIB_EIP) += VDM_BOP_LENGTH;
            return cursor;
        }
    }

    /* AL=03, THE OVERLAY: BRANCH BEFORE ANY OF THE PROCESS MACHINERY:
     * It is not a process. No memory is allocated, no PSP is built, no parent
     * frame is snapshotted and control does not transfer -- the caller already
     * owns the buffer and only wants the image put in it and relocated by the
     * factor IT supplies. Running it through the code below would allocate a
     * block and hand the child the CPU, which is a different function. (#50)
     */
    if (machine->ExecMode == DOS_INT21_EXEC_OVERLAY)
    {
        UINT32 count = DosLoadOverlay(NULL, g_ExecFileBuffer, bytesRead,
                                      machine->ExecOverlaySegment, machine->ExecOverlayRelocation);
        cursor = LogPut(cursor, "  EXEC: AL=03 overlay -> seg=0x"); cursor = LogHex(cursor, machine->ExecOverlaySegment);
        cursor = LogPut(cursor, " reloc=0x"); cursor = LogHex(cursor, machine->ExecOverlayRelocation);
        cursor = LogPut(cursor, " bytes=0x"); cursor = LogHex(cursor, count); cursor = LogPut(cursor, "\r\n");
        VDM_REG(tib, VTIB_EAX) &= HIGH_WORD_MASK_U;
        *flagsPointer &= (WORD)~EFLAGS_CF;
        VDM_REG(tib, VTIB_EIP) += VDM_BOP_LENGTH;
        return cursor;
    }

    /* THE CHILD'S ENVIRONMENT. (s74):
     * Parameter block word 0 = 0 means "inherit", and on MS-DOS 6.22 inherit
     * means A COPY: a fresh block, allocated BELOW the program block, holding
     * the parent's strings, the double NUL, the word 0001 and the program name
     * exactly as the caller spelled it to EXEC (p_child measures all four as
     * relations: child.env.copy=0101, count=0100, namekind=0000). We used to
     * hand the child the parent's block itself -- and then DosPspBuild wiped
     * its first three bytes. DOS/4GW, launched as a separate DOS4GW.EXE by the
     * stub in Heaven7's h7.EXE, reads that name slot to find what to load, and
     * read "PEC=C:\COMMAND.COM". Doom, Heretic, Hexen and ZAR never showed it
     * because their extenders are BOUND into the game EXE: no EXEC, no copy.
     *
     * [CAUTION]: AND A NON-ZERO WORD IS COPIED TOO (s74c). It used to be handed to the child
     * as it stood, on the theory that "the caller's own block" is the caller's
     * business. DOS does not think so: EXEC always builds the child a FRESH block
     * -- strings from whichever segment was named, then 0001 + the program name --
     * because the name slot is DOS's, not the caller's. Duke3D's SETUP.EXE spawns
     * SETMAIN.EXE (bound DOS/4G) with a Watcom-built environment that has no name
     * slot; DOS/4G read its own path out of the tail, got "", and died with
     * "DOS/16M error: [8] cannot open file ''".
     */
    environmentSegment = machine->ExecEnvironment;
    {
        const volatile BYTE *parentPsp = (const volatile BYTE *)((DWORD)machine->PspSegment << PARAGRAPH_SHIFT);
        WORD parentEnvironmentSegment = environmentSegment ? environmentSegment : (WORD)(parentPsp[DOS_PSP_ENVIRONMENT] | (parentPsp[DOS_PSP_ENVIRONMENT + 1] << BYTE_SHIFT));
        const volatile BYTE *parentEnvironment = (const volatile BYTE *)((DWORD)parentEnvironmentSegment << PARAGRAPH_SHIFT);
        DWORD environmentLength;
        DWORD nameLength = 0;
        DWORD total;
        DWORD index;
        volatile BYTE *childEnvironment;
        if (parentEnvironment[0] == 0)
            environmentLength = 1;                                              /* empty: one NUL ends the list */
        else
        {
            for (environmentLength = 0; environmentLength < DOS_ENV_SCAN_MAX && !(parentEnvironment[environmentLength] == 0 && parentEnvironment[environmentLength + 1] == 0); ++environmentLength) ;
            environmentLength += 2;
        }
        PCSTR executableName = machine->ExecName;
        /* THE 64-BYTE argv[0] RULE, AT EXEC TOO. (s81, #208) (Importance = 1):
         * DOS/4GW 1.97 copies its own path into a 64-byte buffer (see the start-up
         * copy of this rule, "argv[0] MUST BE 8.3"). That rule only guarded a program
         * WE loaded; anything EXEC'd -- typed at the prompt, and since #208 every
         * program started from Windows -- got the full path, and Duke3D's
         * C:\DOCUME~1\...\DUKE3D\DUKE3D.EXE (66 chars) died with "can't find file
         * ...DUKE3D.E>". The same answer here: when the name reaches 64 and the program
         * is in the current directory, the bare file name -- which the guest resolves
         * against that directory exactly as DOS would.
         */
        while (executableName[nameLength] && nameLength < sizeof(machine->ExecName) - 1)
            ++nameLength;
        if (nameLength >= DOS_EXTENDER_ARGV0_MAX)
        {
            CHAR currentDirectory[MAX_PATH];
            CHAR shortCurrentDirectory[MAX_PATH];
            DWORD currentDirectoryLength;
            DWORD shortCurrentDirectoryLength;
            PCSTR executableBaseName = executableName;
            PCSTR scan;
            for (scan = executableName; *scan; ++scan)
                if (*scan == '\\')
                    executableBaseName = scan + 1;
            currentDirectoryLength = GetCurrentDirectoryA(sizeof currentDirectory, currentDirectory);
            shortCurrentDirectoryLength = (currentDirectoryLength && currentDirectoryLength < sizeof currentDirectory) ? GetShortPathNameA(currentDirectory, shortCurrentDirectory, sizeof shortCurrentDirectory) : 0;
            if (shortCurrentDirectoryLength && shortCurrentDirectoryLength < sizeof shortCurrentDirectory && executableBaseName > executableName
                && (DWORD)(executableBaseName - executableName - 1) == shortCurrentDirectoryLength && CompareStringA(LOCALE_SYSTEM_DEFAULT,
                       NORM_IGNORECASE, executableName, (INT)shortCurrentDirectoryLength, shortCurrentDirectory, (INT)shortCurrentDirectoryLength) == CSTR_EQUAL)
            {
                cursor = LogPut(cursor, "  EXEC: argv[0] is 64+ chars -- past DOS/4GW's buffer; the bare name [");
                cursor = LogPut(cursor, executableBaseName); cursor = LogPut(cursor, "] (it is in the current directory)\r\n");
                executableName = executableBaseName;
                for (nameLength = 0; executableName[nameLength]; ++nameLength) ;
            }
        }
        total = environmentLength + X86_WORD_SIZE + nameLength + 1;
        if (DosMcbAllocate(NULL, machine->FirstMcb, (WORD)((total + PARAGRAPH_LAST_BYTE) >> PARAGRAPH_SHIFT), &environmentBlock, &maximumParagraphs) != 0)
        {
            cursor = LogPut(cursor, "  EXEC: no memory for the environment copy\r\n");
            VDM_REG(tib, VTIB_EAX) = (VDM_REG(tib, VTIB_EAX) & HIGH_WORD_MASK_U) | DOS_ERR_INSUFFICIENT_MEMORY;
            *flagsPointer |= EFLAGS_CF;
            VDM_REG(tib, VTIB_EIP) += VDM_BOP_LENGTH;
            return cursor;
        }
        childEnvironment = (volatile BYTE *)((DWORD)environmentBlock << PARAGRAPH_SHIFT);
        for (index = 0; index < environmentLength; ++index)
            childEnvironment[index] = parentEnvironment[index];
        childEnvironment[environmentLength] = 1;
        childEnvironment[environmentLength + 1] = 0;             /* count word 0001 */
        for (index = 0; index <= nameLength; ++index)
            childEnvironment[environmentLength + X86_WORD_SIZE + index] = (BYTE)executableName[index];
        environmentSegment = environmentBlock;
        cursor = LogPut(cursor, "  EXEC: env copied from 0x"); cursor = LogHex(cursor, parentEnvironmentSegment);
        cursor = LogPut(cursor, " to 0x"); cursor = LogHex(cursor, environmentBlock);
        cursor = LogPut(cursor, " ("); cursor = LogHex(cursor, environmentLength); cursor = LogPut(cursor, " bytes of strings) + name [");
        cursor = LogPut(cursor, executableName); cursor = LogPut(cursor, "]\r\n");
    }

    /* HOW MUCH: THE HEADER DECIDES, NOT "EVERYTHING". (GH #255):
     * Probe the largest block by asking for too much, then size from it: a .COM
     * takes it all; an MZ gets 10h + image + e_maxalloc (capped at the block), is
     * REFUSED with 8 when 10h + image + e_minalloc does not fit -- before anything
     * is loaded -- and with min = max = 0 takes the whole block and is loaded at
     * its TOP. DosExecSize has the measurements. Every child used to get the
     * whole block, so a program linked to leave memory for its own children found
     * none, and one that could not fit was loaded anyway.
     */
    if (DosMcbAllocate(NULL, machine->FirstMcb, DOS_MCB_LARGEST_REQUEST, &child, &maximumParagraphs) == 0)
        maximumParagraphs = 0;
    want = 0;
    if (maximumParagraphs && DosExecSize(g_ExecFileBuffer, bytesRead, maximumParagraphs, &want, &loadHigh) != 0)
    {
        cursor = LogPut(cursor, "  EXEC: e_minalloc does not fit -- largest block 0x"); cursor = LogHex(cursor, maximumParagraphs);
        cursor = LogPut(cursor, " paras -> error 8, not loaded\r\n");
        want = 0;
    }
    if (!want || DosMcbAllocate(NULL, machine->FirstMcb, want, &child, &maximumParagraphs) != 0)
    {
        cursor = LogPut(cursor, "  EXEC: no memory\r\n");
        if (environmentBlock)
            DosMcbFree(NULL, environmentBlock);
        VDM_REG(tib, VTIB_EAX) = (VDM_REG(tib, VTIB_EAX) & HIGH_WORD_MASK_U) | DOS_ERR_INSUFFICIENT_MEMORY;
        *flagsPointer |= EFLAGS_CF;
        VDM_REG(tib, VTIB_EIP) += VDM_BOP_LENGTH;
        return cursor;
    }
    /* The env block belongs to the child, as DOS records it (MCB owner = its PSP). */
    if (environmentBlock)
        DosMcbWriteWord((volatile BYTE *)((DWORD)(environmentBlock - 1) << PARAGRAPH_SHIFT) + 1, child);
    /* #243: AND SO DOES THE PROGRAM'S OWN BLOCK. (s86):
     * DosMcbAllocate stamps DOS_PSP_SEG (0100h) on what it hands out, and only the env
     * block was put right -- so a child's own memory read as the SHELL's (measured,
     * QB.EXE /L: `026C M owner=0100`). DOS makes the program block's owner the
     * child's PSP, and a program that walks the chain for "my blocks" depends on it.
     */
    DosMcbWriteWord((volatile BYTE *)((DWORD)(child - 1) << PARAGRAPH_SHIFT) + 1, child);

    /* Snapshot the parent BEFORE anything is overwritten. */
    g_Exec[depth].Eax = VDM_REG(tib, VTIB_EAX);
    g_Exec[depth].Ebx = VDM_REG(tib, VTIB_EBX);
    g_Exec[depth].Ecx = VDM_REG(tib, VTIB_ECX);
    g_Exec[depth].Edx = VDM_REG(tib, VTIB_EDX);
    g_Exec[depth].Esi = VDM_REG(tib, VTIB_ESI);
    g_Exec[depth].Edi = VDM_REG(tib, VTIB_EDI);
    g_Exec[depth].Ebp = VDM_REG(tib, VTIB_EBP);
    g_Exec[depth].Esp = VDM_REG(tib, VTIB_ESP);
    g_Exec[depth].Eip = VDM_REG(tib, VTIB_EIP);
    g_Exec[depth].Eflags = VDM_REG(tib, VTIB_EFLAGS);
    g_Exec[depth].Cs  = (WORD)VDM_REG(tib, VTIB_CS);
    g_Exec[depth].Ss = (WORD)VDM_REG(tib, VTIB_SS);
    g_Exec[depth].Ds  = (WORD)VDM_REG(tib, VTIB_DS);
    g_Exec[depth].Es = (WORD)VDM_REG(tib, VTIB_ES);
    g_Exec[depth].Psp = machine->PspSegment;
    g_Exec[depth].DtaSegment = machine->DtaSegment;
    g_Exec[depth].DtaOffset = machine->DtaOffset;
    g_Exec[depth].ChildSegment = child;
    g_Exec[depth].EnvironmentSegment   = environmentBlock;

    /* Build the child's PSP and copy in its command tail, then load the image. */
    DosPspBuild(NULL, child, environmentSegment, (WORD)(child + want));
    DosMcbSetOwnerName(NULL, child, machine->ExecPath);        /* DOS 4+: MEM /C and /D read it */
    /* #208: a SECOND XP shell (the user typed `command`) is told 5.00 as the first one
     * is -- the same image test as at start-up: >= 8 NTVDM `C4 C4 54` sites.
     */
    {   DWORD index, bopCount = 0;
        for (index = 0; index + VDM_BOP_LENGTH < bytesRead; ++index)
            if (g_ExecFileBuffer[index] == VDM_BOP0 && g_ExecFileBuffer[index+1] == VDM_BOP1 && g_ExecFileBuffer[index+2] == NTVDM_BOP_CMD)
                ++bopCount;
        if (bopCount >= EXEC_SHELL_BOP_SITES_MIN)
            DosInt21SetShellPsp(machine, child, TRUE); }
    /* ...and so is every program in Windows' own SYSTEM directory. (s81, user: `mem` ->
     * "Incorrect DOS version".) Those are XP's DOS tools -- MEM, EDIT, DEBUG, EDLIN,
     * EXE2BIN -- built for the DOS 5.00 that stock reports to everything; MEM checks for
     * exactly that. This is SETVER's own job: a per-PROGRAM version, not a per-session
     * one. Compared on the directory, long or 8.3 form.
     */
    {   CHAR systemDirectory[MAX_PATH], shortSystemDirectory[MAX_PATH];
    DWORD systemLength;
    DWORD shortSystemLength = 0;
    DWORD directoryLength = 0;
    PCSTR scan;
        for (scan = machine->ExecPath; *scan; ++scan)
            if (*scan == '\\')
                directoryLength = (DWORD)(scan - machine->ExecPath);
        systemLength = GetSystemDirectoryA(systemDirectory, sizeof systemDirectory);
        if (systemLength && systemLength < sizeof systemDirectory)
            shortSystemLength = GetShortPathNameA(systemDirectory, shortSystemDirectory, sizeof shortSystemDirectory);
        if (directoryLength && ((systemLength && systemLength < sizeof systemDirectory && directoryLength == systemLength
                    && CompareStringA(LOCALE_SYSTEM_DEFAULT, NORM_IGNORECASE,
                                      machine->ExecPath, (INT)directoryLength, systemDirectory, (INT)systemLength) == CSTR_EQUAL)
                   || (shortSystemLength && shortSystemLength < sizeof shortSystemDirectory && directoryLength == shortSystemLength
                    && CompareStringA(LOCALE_SYSTEM_DEFAULT, NORM_IGNORECASE,
                                      machine->ExecPath, (INT)directoryLength, shortSystemDirectory, (INT)shortSystemLength) == CSTR_EQUAL)))
        {
            DosInt21SetShellPsp(machine, child, TRUE);
            cursor = LogPut(cursor, "  EXEC: an XP DOS tool (Windows' system directory) -- told DOS 5.00, as SETVER would\r\n");
        } }
    /* The child gets the vectors as they stand NOW, so whatever it installs is
     * unwound to the parent's when it exits -- that is the whole contract, and it
     * matters most for INT 24h. (GH #34)
     */
    DosPspSaveVectors(NULL, child, machine->PspSegment);
    { volatile BYTE *childPsp = (volatile BYTE *)(child << PARAGRAPH_SHIFT);
      const volatile BYTE *tail = (const volatile BYTE *)
          ((machine->ExecTailSegment << PARAGRAPH_SHIFT) + machine->ExecTailOffset);
      INT index;
      INT count = tail[0] > DOS_PSP_COMMAND_TAIL_MAX ? DOS_PSP_COMMAND_TAIL_MAX : tail[0];
      for (index = 0; index <= count; ++index)
          childPsp[DOS_PSP_COMMAND_TAIL_LENGTH + index] = tail[index];
      childPsp[DOS_PSP_COMMAND_TAIL + count] = DOS_PSP_COMMAND_TAIL_END;
      childPsp[DOS_PSP_PARENT] = (BYTE)(machine->PspSegment & BYTE_MASK);       /* parent PSP */
      childPsp[DOS_PSP_PARENT + 1] = (BYTE)(machine->PspSegment >> BYTE_SHIFT); }

    /* Load high puts the image at the top of the block; the PSP stays at the bottom. */
    image = DosLoadImageAt(NULL, g_ExecFileBuffer, bytesRead, child,
                      loadHigh ? (WORD)(child + want - DosImageParagraphs(g_ExecFileBuffer, bytesRead)) : 0);
    if (loadHigh) { cursor = LogPut(cursor, "  EXEC: e_minalloc = e_maxalloc = 0 -> loaded HIGH at 0x");
                     cursor = LogHex(cursor, image.CodeSegment);
                     cursor = LogPut(cursor, "\r\n"); }

    if (machine->ExecMode == DOS_INT21_EXEC_LOAD_ONLY)
    {
        /* -- LOAD WITHOUT EXECUTING. DOS builds the PSP and loads the image, then
         * ANSWERS THROUGH THE PARAMETER BLOCK instead of transferring control:
         * +0x0E gets the initial SS:SP and +0x12 the entry CS:IP. The memory
         * STAYS ALLOCATED -- the caller is going to jump into it, and freeing
         * the child here (as this used to) hands back a pointer to a block that
         * is on the free list. (GH #50)
         *
         * [CAUTION]: SP IS e_sp MINUS TWO, and that is measured, not derived.
         * tests/probes/dos/p_ovl.asm builds an image whose header declares
         * e_sp = 0x0100 and MS-DOS 6.22 returns:
         *   CASE=ovl.4B01.entry  CS=14F4 IP=0000 SS=14F4 SP=00FE
         * One image, one data point: a second image with a different e_sp
         * would confirm the rule rather than the instance. Recorded as a
         * single-point measurement rather than dressed up as a law.
         */
        volatile BYTE *parameterBlock = (volatile BYTE *)
            (((DWORD)machine->ExecBlockSegment << PARAGRAPH_SHIFT) + machine->ExecBlockOffset);
        WORD sp01 = (WORD)(image.StackPointer - X86_WORD_SIZE);
        parameterBlock[DOS_EXEC_BLOCK_SP] = (BYTE)(sp01 & BYTE_MASK);
        parameterBlock[DOS_EXEC_BLOCK_SP + 1] = (BYTE)(sp01 >> BYTE_SHIFT);
        parameterBlock[DOS_EXEC_BLOCK_SS] = (BYTE)(image.StackSegment & BYTE_MASK);
        parameterBlock[DOS_EXEC_BLOCK_SS + 1] = (BYTE)(image.StackSegment >> BYTE_SHIFT);
        parameterBlock[DOS_EXEC_BLOCK_IP] = (BYTE)(image.InstructionPointer & BYTE_MASK);
        parameterBlock[DOS_EXEC_BLOCK_IP + 1] = (BYTE)(image.InstructionPointer >> BYTE_SHIFT);
        parameterBlock[DOS_EXEC_BLOCK_CS] = (BYTE)(image.CodeSegment & BYTE_MASK);
        parameterBlock[DOS_EXEC_BLOCK_CS + 1] = (BYTE)(image.CodeSegment >> BYTE_SHIFT);
        cursor = LogPut(cursor, "  EXEC: AL=01 loaded, not run -- entry=");
        cursor = LogHex(cursor, image.CodeSegment); cursor = LogPut(cursor, ":"); cursor = LogHex(cursor, image.InstructionPointer);
        cursor = LogPut(cursor, " stack="); cursor = LogHex(cursor, image.StackSegment); cursor = LogPut(cursor, ":");
        cursor = LogHex(cursor, sp01); cursor = LogPut(cursor, " (block KEPT)\r\n");
        /* #165: AND THE CHILD IS NOW THE CURRENT PROCESS. Measured (p_4b05): after
         * 4B01h, AH=62h returns the child's PSP on 6.22, DOSBox-X and PCem alike --
         * which is why a loader puts its own back with AH=50h, and why 4B05h then
         * leaves it alone. We kept the loader's.
         */
        machine->PspSegment = child;
        VDM_REG(tib, VTIB_EAX) &= HIGH_WORD_MASK_U;
        *flagsPointer &= (WORD)~EFLAGS_CF;
        VDM_REG(tib, VTIB_EIP) += VDM_BOP_LENGTH;
        return cursor;
    }

    ExecMachineSave(depth);                              /* #152: what a forced close restores */
    /* -- THE STRIP NAMES THE PROGRAM RUNNING NOW, NOT THE SHELL. (user, s84: "the exe/com
     * name is always COMMAND.COM") It was set once, from the program NTVDMEX loaded
     * first -- which is the shell whenever a game is started from a prompt or by
     * double-click through it. Save the parent's name with this level; DosTerminate
     * puts it back (a Close Program ends through there too).
     */
    {   PCSTR programBaseName = machine->ExecPath, scan;
    INT index = 0;
        for (index = 0; index < ARRAYSIZE(g_ProgramName) - 1 && g_ProgramName[index]; ++index)
            g_ExecMachine[depth].ProgramName[index] = g_ProgramName[index];
        g_ExecMachine[depth].ProgramName[index] = 0;
        for (scan = machine->ExecPath; *scan; ++scan)
            if (*scan == '\\' || *scan == '/' || *scan == ':')
                programBaseName = scan + 1;
        if (*programBaseName)
        {
            for (index = 0; programBaseName[index] && index < ARRAYSIZE(g_ProgramName) - 1; ++index)
                g_ProgramName[index] = programBaseName[index];
            g_ProgramName[index] = 0;
        }
        }
    DosHandlesPush(machine);                            /* s81: the child works on a copy */
    DosJftExec(machine, child);    /* s91: JFT edits the parent made directly (COMMAND.COM's `>`) */
    ++g_ExecDepth;
    machine->PspSegment = child;
    machine->DtaSegment = child;
    machine->DtaOffset = DOS_PSP_DEFAULT_DTA;        /* DOS resets the DTA to PSP:80 */
    VDM_REG(tib, VTIB_CS)  = image.CodeSegment;
    VDM_REG(tib, VTIB_EIP) = image.InstructionPointer;
    VDM_REG(tib, VTIB_SS)  = image.StackSegment;
    VDM_REG(tib, VTIB_ESP) = image.StackPointer;
    VDM_REG(tib, VTIB_DS)  = child;
    VDM_REG(tib, VTIB_ES)  = child;
    VDM_REG(tib, VTIB_EAX) = 0;
    /* #212: THE CHILD STARTS WITH INTERRUPTS ON, as DOS's EXEC enters it. These flags
     * are the parent's AT THE BOP -- inside our INT 21h stub, where the INT has already
     * cleared IF -- so without this every program ran from its first instruction with
     * the virtual IF clear (p_ifst: 0000 vs 0200 on 6.22, DOSBox-X and PCem), and one
     * that never executes STI had every timer tick refused (mybench: 0040:006C frozen).
     * IF in the VTIB after an event exit IS the virtual flag (s81, VME), so setting it
     * here is what the child sees. TF off too: a trace flag must not follow into it.
     */
    VDM_REG(tib, VTIB_EFLAGS) = (VDM_REG(tib, VTIB_EFLAGS) & ~EFLAGS_TF_U) | EFLAGS_IF_U;
    cursor = LogPut(cursor, "  EXEC: child at seg=0x"); cursor = LogHex(cursor, child);
    cursor = LogPut(cursor, " entry="); cursor = LogHex(cursor, image.CodeSegment); cursor = LogPut(cursor, ":"); cursor = LogHex(cursor, image.InstructionPointer);
    cursor = LogPut(cursor, image.IsExe ? " (EXE)" : " (COM)");
    cursor = LogPut(cursor, " depth="); cursor = LogHexByte(cursor, (UINT)g_ExecDepth);
    cursor = LogPut(cursor, "\r\n");
    return cursor;
}

VOID CriticalSnapshot(volatile BYTE *tib)
{
    g_Critical.Eax = VDM_REG(tib, VTIB_EAX);
    g_Critical.Ebx = VDM_REG(tib, VTIB_EBX);
    g_Critical.Ecx = VDM_REG(tib, VTIB_ECX);
    g_Critical.Edx = VDM_REG(tib, VTIB_EDX);
    g_Critical.Esi = VDM_REG(tib, VTIB_ESI);
    g_Critical.Edi = VDM_REG(tib, VTIB_EDI);
    g_Critical.Ebp = VDM_REG(tib, VTIB_EBP);
    g_Critical.Eip = VDM_REG(tib, VTIB_EIP);
    g_Critical.Cs  = (WORD)VDM_REG(tib, VTIB_CS);
    g_Critical.Ds  = (WORD)VDM_REG(tib, VTIB_DS);
    g_Critical.Es = (WORD)VDM_REG(tib, VTIB_ES);
}

VOID CriticalRaise(DOS_MACHINE *machine, volatile BYTE *tib, PSTR *logCursor)
{
    volatile BYTE *swappableDataArea = (volatile BYTE *)(((DWORD)DOS_SDA_SEG << PARAGRAPH_SHIFT) + DOS_SDA_OFF);

    g_Critical.Ah = machine->CritAh;
    g_Critical.Function = (BYTE)(g_Critical.Eax >> BYTE_SHIFT);
    *logCursor = LogPut(*logCursor, "  INT24 raised: fn=0x"); *logCursor = LogHexByte(*logCursor, g_Critical.Function);
    *logCursor = LogPut(*logCursor, " AH=0x"); *logCursor = LogHexByte(*logCursor, machine->CritAh);
    *logCursor = LogPut(*logCursor, " drive=");
    **logCursor = (CHAR)('A' + machine->CritAl);
    ++*logCursor;
    *logCursor = LogPut(*logCursor, ": DI=0x"); *logCursor = LogHexByte(*logCursor, machine->CritCode);
    *logCursor = LogPut(*logCursor, " -> the guest's handler at 0x"); *logCursor = LogHex(*logCursor, *(volatile WORD *)(IVT_SEGMENT_ADDRESS(VECTOR_CRITICAL_ERROR)));
    *logCursor = LogPut(*logCursor, ":0x"); *logCursor = LogHex(*logCursor, *(volatile WORD *)(IVT_OFFSET_ADDRESS(VECTOR_CRITICAL_ERROR))); *logCursor = LogPut(*logCursor, "\r\n");
    VDM_SET16(tib, VTIB_EAX, ((WORD)machine->CritAh << BYTE_SHIFT) | machine->CritAl);
    VDM_SET16(tib, VTIB_EDI, machine->CritCode);
    /* BP:SI = the device header -- the one our DPBs name, which since #48 is the BLOCK
     * driver (DOS_DEV_SEG, attribute bit 15 clear), as 6.22's floppy DPB names its own at
     * 0070:006B. It was NUL, a CHARACTER device, which tells a handler that tests bit 15
     * of [BP:SI+4] the opposite of AH bit 7's "disk error".
     */
    VDM_SET16(tib, VTIB_EBP, DOS_DEV_SEG);
    VDM_SET16(tib, VTIB_ESI, DOS_DEVICE_OFFSET(DOS_DEVICE_BLOCK));
    VDM_SET16(tib, VTIB_CS, DOS_CTAB_SEG);
    VDM_SET16(tib, VTIB_EIP, DOS_CRIT_RAISE);
    swappableDataArea[0] = 1;                                          /* SDA+0: in a critical error */
    machine->IsCritPending = 0;
    machine->IsCritActive = 1;
}

/* Returns 1 for ABORT (the caller terminates the program), 0 to resume the guest. */
INT CriticalReturn(DOS_MACHINE *machine, volatile BYTE *tib, PSTR *logCursor)
{
    volatile BYTE *swappableDataArea = (volatile BYTE *)(((DWORD)DOS_SDA_SEG << PARAGRAPH_SHIFT) + DOS_SDA_OFF);
    BYTE criticalAction = (BYTE)(VDM_REG(tib, VTIB_EAX) & BYTE_MASK);
    BYTE said = criticalAction;
    BYTE allowed = g_Critical.Ah;
    volatile WORD *flagsPointer;

    swappableDataArea[0] = 0;
    machine->IsCritActive = 0;
    if (criticalAction > DOS_CRIT_ACTION_FAIL)
        criticalAction = DOS_CRIT_ACTION_FAIL;
    if (criticalAction == DOS_CRIT_ACTION_IGNORE && !(allowed & DOS_CRIT_ALLOW_IGNORE))
        criticalAction = DOS_CRIT_ACTION_FAIL;                                                                                               /* ignore not allowed -> fail */
    if (criticalAction == DOS_CRIT_ACTION_RETRY && !(allowed & DOS_CRIT_ALLOW_RETRY))
        criticalAction = DOS_CRIT_ACTION_FAIL;                                                                                             /* retry not allowed  -> fail */
    if (criticalAction == DOS_CRIT_ACTION_FAIL && !(allowed & DOS_CRIT_ALLOW_FAIL))
        criticalAction = DOS_CRIT_ACTION_ABORT;                                                                                           /* fail not allowed   -> abort */
    *logCursor = LogPut(*logCursor, "  INT24 answered AL=0x"); *logCursor = LogHexByte(*logCursor, said);
    *logCursor = LogPut(*logCursor, criticalAction == DOS_CRIT_ACTION_IGNORE ? " -> IGNORE" : criticalAction == DOS_CRIT_ACTION_RETRY ? " -> RETRY"
                  : criticalAction == DOS_CRIT_ACTION_ABORT ? " -> ABORT" : " -> FAIL");
    *logCursor = LogPut(*logCursor, "\r\n");
    /* the INT 21h call's own registers, and CS:IP back ON its BOP */
    VDM_REG(tib, VTIB_EAX) = g_Critical.Eax;
    VDM_REG(tib, VTIB_EBX) = g_Critical.Ebx;
    VDM_REG(tib, VTIB_ECX) = g_Critical.Ecx;
    VDM_REG(tib, VTIB_EDX) = g_Critical.Edx;
    VDM_REG(tib, VTIB_ESI) = g_Critical.Esi;
    VDM_REG(tib, VTIB_EDI) = g_Critical.Edi;
    VDM_REG(tib, VTIB_EBP) = g_Critical.Ebp;
    VDM_REG(tib, VTIB_EIP) = g_Critical.Eip;
    VDM_SET16(tib, VTIB_CS, g_Critical.Cs);
    VDM_SET16(tib, VTIB_DS, g_Critical.Ds);
    VDM_SET16(tib, VTIB_ES, g_Critical.Es);
    if (criticalAction == DOS_CRIT_ACTION_RETRY)
        return 0;                                                                       /* RETRY: the BOP runs again */
    if (criticalAction == DOS_CRIT_ACTION_ABORT)                                        /* ABORT */
    {
        machine->ExitCode = DOS_CRIT_ABORT_RETURN_CODE;
        machine->TermType = DOS_TERM_CRITICAL_ABORT;
        return 1;
    }
    /* CF goes on the FLAGS its INT pushed, as every INT 21h answer does. */
    flagsPointer = (volatile WORD *)((VDM_REG16(tib, VTIB_SS) << PARAGRAPH_SHIFT)
                            + ((VDM_REG16(tib, VTIB_ESP) + X86_FRAME16_FLAGS) & WORD_MASK));
    /* -- #275: IGNORE, which only a 3Fh/40h is allowed (AH bit 5, DosCritInt24Ah). DOS
     * carries on as if the sectors had moved (MS-DOS 4.0 DREAD: IGNORE returns carry
     * clear): the call reports the bytes asked for -- a read still stops at end of
     * file -- and the position advances by them. Nothing is copied: what an ignored
     * read leaves in the buffer is what was there. See DosCritIgnoreCount.
     *
     * [CAUTION]: Spec-derived, unmeasured on 6.22: p_crit2 crit2.h3f.ignore / crit2.h40.ignore.
     */
    if (criticalAction == DOS_CRIT_ACTION_IGNORE && (g_Critical.Function == DOS_CRIT_FUNCTION_READ || g_Critical.Function == DOS_CRIT_FUNCTION_WRITE)
        && (g_Critical.Ebx & WORD_MASK) < DOS_MAX_FILES && machine->FileHandles[g_Critical.Ebx & WORD_MASK])
    {
        HANDLE fileHandle = machine->FileHandles[g_Critical.Ebx & WORD_MASK];
        DWORD position = SetFilePointer(fileHandle, 0, NULL, FILE_CURRENT);
        DWORD size = GetFileSize(fileHandle, NULL);
        WORD ignoreCount = DosCritIgnoreCount(g_Critical.Function, (WORD)(g_Critical.Ecx & WORD_MASK), position, size,
                                       position != INVALID_SET_FILE_POINTER && size != INVALID_FILE_SIZE);
        if (position != INVALID_SET_FILE_POINTER)
            SetFilePointer(fileHandle, (LONG)ignoreCount, NULL, FILE_CURRENT);
        VDM_SET16(tib, VTIB_EAX, ignoreCount);
        *flagsPointer &= (WORD)~EFLAGS_CF_U;
        *logCursor = LogPut(*logCursor, "  INT24 IGNORE: the call reports 0x"); *logCursor = LogHex(*logCursor, ignoreCount);
        *logCursor = LogPut(*logCursor, " bytes, CF=0\r\n");
        VDM_REG(tib, VTIB_EIP) += VDM_BOP_LENGTH;                     /* past the BOP -> the IRET */
        return 0;
    }
    /* FAIL (and an IGNORE with no handle left to ignore on): the call returns an error. */
    VDM_SET16(tib, VTIB_EAX, DosCritFailAx(g_Critical.Function, machine->CritCode));
    *flagsPointer |= EFLAGS_CF;
    machine->LastError = DOS_ERR_FAIL_I24;
    VDM_REG(tib, VTIB_EIP) += VDM_BOP_LENGTH;                         /* past the BOP -> the IRET */
    return 0;
}

/* -- #275: THE DPMI TRANSLATOR's OWN 3Fh/40h (dpmi INT 21h, direct-translated through
 * the client's selectors) never looked at ReadFile/WriteFile either. A hardware
 * failure (19-31) there is answered the way DosInt21 answers it for a protected-
 * mode caller -- as if INT 24h had said FAIL: CF=1, AX=0005, 59h=53h -- because
 * INT 24h is not reflected to DPMI clients (see the tail of DosInt21 for why, and
 * what doing it properly needs). Returns 1 if it answered; 0 = not a hardware
 * error, the caller keeps its old answer. `we` = GetLastError(), 0 = it did not fail.
 */
INT PmRwHardwareFail(
    DOS_MACHINE *machine,
    volatile BYTE *tib,
    BYTE function,
    DWORD win32Error,
    PSTR *logCursor)
{
    WORD dosError = 0;

    if (!win32Error || !DosErrFromWin32((DWORD)win32Error, &dosError) || !DosCritIsHardwareError(dosError))
        return 0;
    VDM_REG(tib, VTIB_EFLAGS) |= EFLAGS_CF_U;
    VDM_SET16(tib, VTIB_EAX, DosCritFailAx(function, (BYTE)(dosError - DOS_ERR_WRITE_PROTECT)));
    machine->LastError = DOS_ERR_FAIL_I24;
    *logCursor = LogPut(*logCursor, "  INT24 not raised (DPMI client): AH=0x"); *logCursor = LogHexByte(*logCursor, function);
    *logCursor = LogPut(*logCursor, " error 0x"); *logCursor = LogHexByte(*logCursor, dosError);
    *logCursor = LogPut(*logCursor, " answered as FAIL -> AX=0x"); *logCursor = LogHex(*logCursor, VDM_REG16(tib, VTIB_EAX));
    *logCursor = LogPut(*logCursor, ", 59h=53h\r\n");
    return 1;
}

/* #251: DOS's PRN/AUX output when the guest cannot be resumed in the driver code
 * (dos_auxprn.h) -- a DPMI client's INT 21h. Same devices, without the IVT hop.
 */
INT DosPrnOut(PVOID context, BYTE character)
{
    (VOID)context;
    return LptSpoolPut(character);
}

VOID DosAuxOut(PVOID context, BYTE character)
{
    NTVDD_REGISTERS registers;

    (VOID)context;
    registers.Eax = (BIOS_SERIAL_SEND << BYTE_SHIFT) | character;                         /* INT 14h AH=01h, COM1 (DX=0) */
    registers.Ebx = registers.Ecx = registers.Edx = registers.Esi = registers.Edi = registers.Ebp = 0;
    registers.Ds = registers.Es = 0;
    registers.CarryFlag = registers.ZeroFlag = 0;
    HOST_LOCK();
    VddBusDeliverInterrupt(&g_Bus, VECTOR_SERIAL, &registers);
    HOST_UNLOCK();
}

/* A GUEST ASKED TO TERMINATE. FOUR DOORS, ONE ANSWER. (GH #134):
 * AH=4Ch, AH=00h, INT 20h and INT 27h all end a program, and this decides
 * whether that ends the RUN or merely returns a child to its parent.
 *
 * [INFO]: IT IS A FUNCTION BECAUSE IT USED TO BE INLINE IN THE INT 21h BRANCH ONLY.
 * The BIOS stubs for INT 20h and INT 27h called DosInt21, threw away its
 * "stop" answer and took their own `break` -- so a .COM child exiting through
 * INT 20h, which is THE classic .COM exit, tore down the whole VDM instead of
 * returning to whatever EXEC'd it. Measured: tests/probes/dos/p_curdir.asm's
 * two-byte child (CD 20) killed the probe mid-run, and every case after the
 * EXEC simply never reported.
 * Returns 1 if a child exited and the parent has been restored (the exec loop
 * carries on), 0 if this was the top-level program and the run is over.
 */
INT DosTerminate(DOS_MACHINE *machine, PVOID tib, PSTR *logCursor, PSTR base)
{
    PSTR cursor = *logCursor;

    (VOID)cursor;
            if (g_ExecDepth > 0)
            {
        /* A CHILD terminated, not the program we were asked to run.
         * Put the parent's frame back and let it continue.
         */
        INT depth = --g_ExecDepth;
        volatile WORD *flagsPointer;
        /* AH = how it ended (#34): 0 normally, 2 when its INT 24h answered ABORT. */
        machine->ChildReturnCode = (WORD)(((WORD)machine->TermType << BYTE_SHIFT) | (machine->ExitCode & BYTE_MASK));
        machine->TermType = DOS_TERM_NORMAL;
        if (g_ExecMachine[depth].ProgramName[0])             /* the strip names the parent again */
            LogPut(g_ProgramName, g_ExecMachine[depth].ProgramName);
        /* s81: the parent's handle table back, the child's leftover files closed (not
         * a TSR's). Without this Doom's SETUP closed the shell's stdout for good.
         */
        DosHandlesPop(machine, machine->IsTsrPending);
        if (g_Exec[depth].ChildSegment && !machine->IsTsrPending)
            DosInt21SetShellPsp(machine, g_Exec[depth].ChildSegment, FALSE);   /* #208: its PSP is gone */
        MouseChildExited();          /* s81: the shell does not own the mouse */
        /* #214: A PROGRAM THAT HAS ENDED MUST NOT KEEP SOUNDING. Close Program on Doom
         * left its MIDI notes hanging: the emulated MPU-401 was reset, but the notes
         * were already in XP's synth. Every child exit comes through here (its own
         * AH=4Ch, a DPMI client's teardown, Close Program), so: every MIDI note off on
         * the host synth, and every OPL voice keyed off -- pitch kept, so each one
         * releases through its own envelope instead of clicking off. A TSR is still
         * running, so it is left alone.
         */
        if (!machine->IsTsrPending)
        {
            UINT unusedValue;
            AudioWaveMidiSilence(&g_Wave);
            HOST_LOCK();
            (VOID)unusedValue;
            VddOplAllNotesOff(&g_Opl);              /* both banks, and the rhythm drums (#232) */
            HOST_UNLOCK();
        }
        if (machine->IsTsrPending)
        {
            /* TERMINATE AND STAY RESIDENT. (GH #49):
             * The block is RESIZED, not freed, and the vectors are
             * deliberately NOT unwound -- the whole point of a TSR is
             * that the handlers it installed outlive it. So this skips
             * both DosPspRestoreVectors and DosMcbFree, which is
             * precisely the difference between exiting and staying.
             * DX counted paragraphs from the PSP; a request for less
             * than a PSP is nonsense, so floor it at 16 rather than
             * hand back a block that does not contain its own header.
             */
            WORD keep = machine->TsrKeep < DOS_PSP_PARAGRAPHS ? DOS_PSP_PARAGRAPHS : machine->TsrKeep;
            WORD maximumParagraphs = 0;
            INT resizeResult = DosMcbResize(NULL, g_Exec[depth].ChildSegment, keep, &maximumParagraphs);
            *logCursor = LogPut(*logCursor, "  TSR: seg=0x"); *logCursor = LogHex(*logCursor, g_Exec[depth].ChildSegment);
            *logCursor = LogPut(*logCursor, " stays resident, 0x"); *logCursor = LogHex(*logCursor, keep);
            *logCursor = LogPut(*logCursor, " paras");
            if (resizeResult)
                *logCursor = LogPut(*logCursor, " -- RESIZE FAILED, block kept whole");
            *logCursor = LogPut(*logCursor, ", vectors left installed\r\n");
            machine->IsTsrPending = 0;
            machine->TsrKeep = 0;
        }
        else
        {
        /* UNWIND THE CHILD'S INTERRUPT VECTORS. (GH #34):
         * The other half of the PSP contract: INT 22h/23h/24h go back
         * to what they were when the child started, from the copies
         * DosPspSaveVectors put in its PSP. Without this a child
         * that installed its own critical-error handler leaves it
         * servicing the PARENT's failures, pointing into memory that
         * is freed on the very next line.
         */
        if (g_Exec[depth].ChildSegment)
            DosPspRestoreVectors(NULL, g_Exec[depth].ChildSegment);
        if (g_Exec[depth].ChildSegment)
            DosMcbFree(NULL, g_Exec[depth].ChildSegment);
        /* ...and every OTHER block it still owns, as DOS does on terminate (s80).
         * AH=48h now stamps the child's PSP as owner; a program that exits without
         * freeing its allocations would otherwise shrink the parent's memory for
         * good. Rescan after each free: DosMcbFree coalesces, so the chain moves.
         */
        if (g_Exec[depth].ChildSegment)
        {
            INT pass;
            INT freedCount = 0;
            /* [CAUTION]: Was 64, and Skyroads owns MORE than that: closing it (#152) freed exactly
             * 0x40 and stopped, leaving the rest owned by a PSP that no longer exists.
             */
            for (pass = 0; pass < DOS_MCB_WALK_LIMIT; ++pass)
            {
                WORD mcbSegment = machine->FirstMcb;
                WORD hit = 0;
                INT guard = 0;
                for (;;)
                {
                    volatile BYTE *mcb = DosMcbSegmentAddress(NULL, mcbSegment);
                    if ((mcb[DOS_MCB_SIGNATURE] != DOS_MCB_MEMBER && mcb[DOS_MCB_SIGNATURE] != DOS_MCB_LAST) || ++guard > DOS_TERMINATE_MCB_GUARD)
                        break;
                    if (DosMcbReadWord(mcb + 1) == g_Exec[depth].ChildSegment)
                    {
                        hit = (WORD)(mcbSegment + 1);
                        break;
                    }
                    if (mcb[DOS_MCB_SIGNATURE] == DOS_MCB_LAST)
                        break;
                    mcbSegment = (WORD)(mcbSegment + 1 + DosMcbReadWord(mcb + DOS_MCB_SIZE));
                }
                if (!hit || DosMcbFree(NULL, hit))
                    break;
                ++freedCount;
            }
            if (freedCount)
            {
                *logCursor = LogPut(*logCursor, "  EXEC: freed 0x"); *logCursor = LogHex(*logCursor, (DWORD)freedCount);
                *logCursor = LogPut(*logCursor, " more block(s) the child still owned\r\n");
            }
        }
        /* The chain as the parent will find it: what a child LEFT is exactly what the
         * next program cannot have, and a leak is invisible in any one line above.
         */
        {   WORD mcbSegment = machine->FirstMcb;
        INT guard = 0;
            *logCursor = LogPut(*logCursor, "  EXEC: chain after exit:");
            for (;;)
            {
                volatile BYTE *mcb = DosMcbSegmentAddress(NULL, mcbSegment);
                WORD owner;
                WORD size;
                if ((mcb[DOS_MCB_SIGNATURE] != DOS_MCB_MEMBER && mcb[DOS_MCB_SIGNATURE] != DOS_MCB_LAST) || ++guard > DOS_MCB_DUMP_GUARD)
                {
                    *logCursor = LogPut(*logCursor, " <broken>");
                    break;
                }
                owner = DosMcbReadWord(mcb + DOS_MCB_OWNER);
                size = DosMcbReadWord(mcb + DOS_MCB_SIZE);
                *logCursor = LogPut(*logCursor, " "); *logCursor = LogHex(*logCursor, mcbSegment);
                *logCursor = LogPut(*logCursor, owner ? "/own=" : "/FREE");
                if (owner)
                    *logCursor = LogHex(*logCursor, owner);
                *logCursor = LogPut(*logCursor, "/sz="); *logCursor = LogHex(*logCursor, size);
                if (mcb[0] == 'Z')
                    break;
                mcbSegment = (WORD)(mcbSegment + 1 + size);
            }
            *logCursor = LogPut(*logCursor, "\r\n");
            LogAppend(LOG_PATH, base, *logCursor); *logCursor = base;
        }
        /* And the environment copy EXEC made for it (s74). A TSR keeps its env
         * block along with its PSP, which is why this is on the exit arm only.
         */
        if (g_Exec[depth].EnvironmentSegment)
        {
            DosMcbFree(NULL, g_Exec[depth].EnvironmentSegment);
            g_Exec[depth].EnvironmentSegment = 0;
        }
        }
        VDM_REG(tib, VTIB_EAX) = g_Exec[depth].Eax;
        VDM_REG(tib, VTIB_EBX) = g_Exec[depth].Ebx;
        VDM_REG(tib, VTIB_ECX) = g_Exec[depth].Ecx;
        VDM_REG(tib, VTIB_EDX) = g_Exec[depth].Edx;
        VDM_REG(tib, VTIB_ESI) = g_Exec[depth].Esi;
        VDM_REG(tib, VTIB_EDI) = g_Exec[depth].Edi;
        VDM_REG(tib, VTIB_EBP) = g_Exec[depth].Ebp;
        VDM_REG(tib, VTIB_ESP) = g_Exec[depth].Esp;
        VDM_REG(tib, VTIB_EIP) = g_Exec[depth].Eip;
        VDM_REG(tib, VTIB_EFLAGS) = g_Exec[depth].Eflags;
        VDM_REG(tib, VTIB_CS)  = g_Exec[depth].Cs;
        VDM_REG(tib, VTIB_SS) = g_Exec[depth].Ss;
        VDM_REG(tib, VTIB_DS)  = g_Exec[depth].Ds;
        VDM_REG(tib, VTIB_ES) = g_Exec[depth].Es;
        machine->PspSegment = g_Exec[depth].Psp;
        machine->DtaSegment = g_Exec[depth].DtaSegment;
        machine->DtaOffset = g_Exec[depth].DtaOffset;
        /* EXEC succeeded, so clear the carry the parent's IRET will
         * restore, and set AX=0 as DOS does.
         */
        flagsPointer = (volatile WORD *)((VDM_REG16(tib, VTIB_SS) << PARAGRAPH_SHIFT)
                + ((VDM_REG16(tib, VTIB_ESP) + X86_FRAME16_FLAGS) & WORD_MASK));
        *flagsPointer &= (WORD)~EFLAGS_CF;
        VDM_REG(tib, VTIB_EAX) &= HIGH_WORD_MASK_U;
        VDM_REG(tib, VTIB_EIP) += VDM_BOP_LENGTH;        /* past the BOP -> the IRET */
        machine->ExitCode = 0;
        *logCursor = LogPut(*logCursor, "  EXEC: child exited rc=0x"); *logCursor = LogHexByte(*logCursor, machine->ChildReturnCode);
        *logCursor = LogPut(*logCursor, ", parent resumed (depth="); *logCursor = LogHexByte(*logCursor, (UINT)g_ExecDepth);
        *logCursor = LogPut(*logCursor, ")\r\n");
        LogAppend(LOG_PATH, base, *logCursor); *logCursor = base;
        return 1;                    /* the parent is back: carry on */
    }
    VDM_REG(tib, VTIB_EIP) += VDM_BOP_LENGTH;
    LogAppend(LOG_PATH, base, *logCursor); *logCursor = base;
    return 0;                        /* top-level program: the run is over */
}

static PCSTR FloppyImagePath(VOID)
{
    return g_FloppyImage ? g_FloppyImage : FLOPPY_IMG_PATH;
}

/* s84 (user): DOES THIS PC HAVE THE PHYSICAL DRIVE AT ALL?:
 * GetDriveType reads the drive's TYPE, never its media, so it cannot raise an
 * "insert a disk" prompt or spin a drive up. With no physical drive the Drives tab
 * greys the choice and the host treats the setting as "mounted image".
 */
INT HostHasFloppy(VOID)
{
    return GetDriveTypeA(HOST_FLOPPY_A_ROOT) == DRIVE_REMOVABLE || GetDriveTypeA(HOST_FLOPPY_B_ROOT) == DRIVE_REMOVABLE;
}

INT HostHasCdrom(VOID)
{
    DWORD drives = GetLogicalDrives();
    CHAR root[4] = HOST_DEFAULT_DRIVE_ROOT;
    INT drive;

    for (drive = DOS_DRIVE_C; drive < DOS_DRIVE_LETTERS; ++drive)
    {
        if (!(drives & (1u << drive)))
            continue;
        root[0] = (CHAR)('A' + drive);
        if (GetDriveTypeA(root) == DRIVE_CDROM)
            return 1;
    }
    return 0;
}

PDOS_DISK_GEOMETRY DiskFor(UINT drive)
{
    BYTE boot[DOS_SECTOR_SIZE];
    DWORD got = 0;
    DWORD size;
    UINT oldErrorMode;

    if (drive != 0)
        return NULL;                             /* only A: is backed today */
    if (g_DiskGeometry[0].IsValid)
        return &g_DiskGeometry[0];
    if (g_DiskTried[0])
        return NULL;
    g_DiskTried[0] = 1;
    oldErrorMode = SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX);
    g_DiskHandle[0] = CreateFileA(FloppyImagePath(), GENERIC_READ | GENERIC_WRITE,
                              FILE_SHARE_READ, NULL, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, NULL);
    SetErrorMode(oldErrorMode);
    if (g_DiskHandle[0] == INVALID_HANDLE_VALUE)
        return NULL;
    size = GetFileSize(g_DiskHandle[0], NULL);
    if (!ReadFile(g_DiskHandle[0], boot, DOS_SECTOR_SIZE, &got, NULL) || got != DOS_SECTOR_SIZE
        || !DosDiskGeometryFromBpb(boot, size, &g_DiskGeometry[0]))
    {
        /* An image we cannot read the geometry of is treated as ABSENT. A
         * guessed cylinder count returns the WRONG SECTOR and reports success,
         * which is strictly worse than no drive.
         */
        CHAR lineBuffer[160];
        CHAR *lineCursor = lineBuffer;
        lineCursor = LogPut(lineCursor, "  INT13 image present but its BPB is not usable -- "
                      "treating drive 0 as ABSENT (GH #44)\r\n");
        LogAppend(LOG_PATH, lineBuffer, lineCursor); SerialOut(lineBuffer, lineCursor);
        CloseHandle(g_DiskHandle[0]);
        g_DiskHandle[0] = INVALID_HANDLE_VALUE;
        return NULL;
    }
    { CHAR lineBuffer[200], *lineCursor = lineBuffer;
      lineCursor = LogPut(lineCursor, "  INT13 drive 0 = "); lineCursor = LogPut(lineCursor, FloppyImagePath()); lineCursor = LogPut(lineCursor, ", ");
      lineCursor = LogHex(lineCursor, g_DiskGeometry[0].Cylinders); lineCursor = LogPut(lineCursor, " cyl x ");
      lineCursor = LogHex(lineCursor, g_DiskGeometry[0].Heads);     lineCursor = LogPut(lineCursor, " head x ");
      lineCursor = LogHex(lineCursor, g_DiskGeometry[0].SectorsPerTrack);   lineCursor = LogPut(lineCursor, " sec, type 0x");
      lineCursor = LogHexByte(lineCursor, g_DiskGeometry[0].DriveType); lineCursor = LogPut(lineCursor, "\r\n");
      LogAppend(LOG_PATH, lineBuffer, lineCursor);
      SerialOut(lineBuffer, lineCursor); }
    return &g_DiskGeometry[0];
}

/* Move `count` sectors between the image and guest memory. Returns 1 on a FULL
 * transfer only: a short read is a failure, not a partial success, because the
 * caller reports sectors-transferred in AL and a guest trusts it.
 */
INT DiskIo(UINT drive, UINT32 lba, UINT count, BYTE *guest, INT write)
{
    DWORD moved = 0;
    DWORD want = count * DOS_SECTOR_SIZE;

    if (drive != 0 || g_DiskHandle[0] == INVALID_HANDLE_VALUE || !count)
        return 0;
    if (SetFilePointer(g_DiskHandle[0], (LONG)(lba * DOS_SECTOR_SIZE), NULL, FILE_BEGIN)
        == INVALID_SET_FILE_POINTER)
        return 0;
    if (write)
    {
        if (!WriteFile(g_DiskHandle[0], guest, want, &moved, NULL))
            return 0;
        FlushFileBuffers(g_DiskHandle[0]);
    }
    else
    {
        if (!ReadFile(g_DiskHandle[0], guest, want, &moved, NULL))
            return 0;
    }
    return moved == want;
}

VOID StdioFlush(VOID)
{
    DWORD bytesWritten = 0;

    if (g_Stdio != INVALID_HANDLE_VALUE && g_StdioLength)
        WriteFile(g_Stdio, g_StdioBuffer, g_StdioLength, &bytesWritten, NULL);
    g_StdioLength = 0;
}

/* One byte from the redirected input, or -1 if there is none to be had. */
static INT StdinReadByte(VOID)
{
    BYTE byteValue;
    DWORD got = 0;

    if (!g_StdinHandle)
        return -1;
    if (g_StdinEof)
        return ASCII_END_OF_FILE;
    if (!ReadFile(g_StdinHandle, &byteValue, 1, &got, NULL) || got != 1)
    {
        g_StdinEof = 1;
        return ASCII_END_OF_FILE;
    }
    ++g_StdinBytes;
    return byteValue;
}

/* A handle VALUE out of `proc`'s process parameters at `off`, or 0. The value is
 * meaningful only in that process's handle table.
 */
static HANDLE StdioPebStdout(HANDLE proc)
{
    static PFN_NT_QUERY_INFORMATION_PROCESS queryInformationProcess;
    struct
    {
        LONG State;
        PVOID PebBase;
        ULONG_PTR ProcessId;
        ULONG_PTR AffinityMask;
        ULONG_PTR BasePriority;
        ULONG_PTR ParentProcessId;
    } basicInfo;
    ULONG got = 0;
    ULONG_PTR params = 0;
    HANDLE out = NULL;
    SIZE_T bytesRead = 0;
    if (!queryInformationProcess) queryInformationProcess = (PFN_NT_QUERY_INFORMATION_PROCESS)(ULONG_PTR)GetProcAddress(
                        GetModuleHandleA(HOST_MODULE_NTDLL), HOST_EXPORT_NT_QUERY_INFORMATION_PROCESS);
    if (!queryInformationProcess || !proc)
        return NULL;
    if (queryInformationProcess(proc, 0 /* ProcessBasicInformation */, &basicInfo, sizeof basicInfo, &got) < 0)
        return NULL;
    if (!basicInfo.PebBase)
        return NULL;
    if (!ReadProcessMemory(proc, (BYTE *)basicInfo.PebBase + PEB_OFF_PROCESSPARAMS,
                           &params, sizeof params, &bytesRead) || bytesRead != sizeof params)
        return NULL;
    if (!params)
        return NULL;
    if (!ReadProcessMemory(proc, (BYTE *)params + RUPP_OFF_STDOUT,
                           &out, sizeof out, &bytesRead) || bytesRead != sizeof out)
        return NULL;
    return out;
}

/* The same read at any offset in the parameters block -- used for StandardInput.
 * Kept as a separate entry point rather than a parameter on the one above so the
 * OUTPUT path, which is the one under test against the stock oracle, cannot be
 * changed by an edit meant for the input path.
 */
static HANDLE StdioPebHandle(HANDLE proc, UINT offset)
{
    static PFN_NT_QUERY_INFORMATION_PROCESS queryInformationProcess;
    struct
    {
        LONG State;
        PVOID PebBase;
        ULONG_PTR ProcessId;
        ULONG_PTR AffinityMask;
        ULONG_PTR BasePriority;
        ULONG_PTR ParentProcessId;
    } basicInfo;
    ULONG got = 0;
    ULONG_PTR params = 0;
    HANDLE out = NULL;
    SIZE_T bytesRead = 0;
    if (!queryInformationProcess) queryInformationProcess = (PFN_NT_QUERY_INFORMATION_PROCESS)(ULONG_PTR)GetProcAddress(
                        GetModuleHandleA(HOST_MODULE_NTDLL), HOST_EXPORT_NT_QUERY_INFORMATION_PROCESS);
    if (!queryInformationProcess || !proc)
        return NULL;
    if (queryInformationProcess(proc, 0, &basicInfo, sizeof basicInfo, &got) < 0 || !basicInfo.PebBase)
        return NULL;
    if (!ReadProcessMemory(proc, (BYTE *)basicInfo.PebBase + PEB_OFF_PROCESSPARAMS,
                           &params, sizeof params, &bytesRead) || !params)
        return NULL;
    if (!ReadProcessMemory(proc, (BYTE *)params + offset, &out, sizeof out, &bytesRead)
        || bytesRead != sizeof out)
        return NULL;
    return out;
}

/* [INFO]: PROVE THE OFFSETS ON THIS PROCESS. Returns 1 if our own PEB reports the
 * same StandardOutput that GetStdHandle does -- which is the only evidence
 * available at run time that the two constants above are right on THIS
 * Windows. [CAUTION] A process with no stdout at all (which is exactly our case) has
 * 0 in both places, and 0 == 0 would "prove" nothing -- so that is reported as
 * UNPROVEN rather than as agreement.
 */
static INT StdioPebLayoutOk(INT *sawNull)
{
    HANDLE mine = GetStdHandle(STD_OUTPUT_HANDLE);
    HANDLE pebStdout  = StdioPebStdout(GetCurrentProcess());

    if (sawNull)
        *sawNull = (!mine && !pebStdout);
    if (!mine && !pebStdout)
        return 0;                                   /* nothing to compare -- unproven */
    return mine == pebStdout;
}

static INT StdioParentIs(HANDLE proc, DWORD parentProcessId)
{
    struct
    {
        USHORT Length;
        USHORT Maximum;
        PVOID Buffer;
    } imagePath;
    ULONG_PTR params = 0;
    struct
    {
        LONG State;
        PVOID PebBase;
        ULONG_PTR ProcessId;
        ULONG_PTR AffinityMask;
        ULONG_PTR BasePriority;
        ULONG_PTR ParentProcessId;
    } basicInfo;
    static PFN_NT_QUERY_INFORMATION_PROCESS queryInformationProcess;
    ULONG got = 0;
    SIZE_T bytesRead = 0;
    WCHAR path[MAX_PATH];
    CHAR narrow[MAX_PATH];
    CHAR want[MAX_PATH];
    INT index;
    INT length;
    HANDLE snap;
    want[0] = 0;
    if (!queryInformationProcess) queryInformationProcess = (PFN_NT_QUERY_INFORMATION_PROCESS)(ULONG_PTR)GetProcAddress(
                        GetModuleHandleA(HOST_MODULE_NTDLL), HOST_EXPORT_NT_QUERY_INFORMATION_PROCESS);
    if (!queryInformationProcess)
        return 0;
    /* What the process list says this pid is. */
    snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap != INVALID_HANDLE_VALUE)
    {
        PROCESSENTRY32 entry;
        entry.dwSize = sizeof entry;
        if (Process32First(snap, &entry))
        {
            do { if (entry.th32ProcessID == parentProcessId)
            {
                     for (index = 0; index < MAX_PATH - 1 && entry.szExeFile[index]; ++index)
                         want[index] = entry.szExeFile[index];
                     want[index] = 0;
                     break; }
            } while (Process32Next(snap, &entry));
        }
        CloseHandle(snap);
    }
    if (!want[0])
        return 0;
    if (queryInformationProcess(proc, 0, &basicInfo, sizeof basicInfo, &got) < 0 || !basicInfo.PebBase)
        return 0;
    if (!ReadProcessMemory(proc, (BYTE *)basicInfo.PebBase + PEB_OFF_PROCESSPARAMS,
                           &params, sizeof params, &bytesRead) || !params)
        return 0;
    if (!ReadProcessMemory(proc, (BYTE *)params + RUPP_OFF_IMAGEPATH,
                           &imagePath, sizeof imagePath, &bytesRead) || bytesRead != sizeof imagePath)
        return 0;
    if (!imagePath.Buffer || !imagePath.Length || imagePath.Length >= sizeof path)
        return 0;
    if (!ReadProcessMemory(proc, imagePath.Buffer, path, imagePath.Length, &bytesRead) || bytesRead != imagePath.Length)
        return 0;
    length = (INT)(imagePath.Length / sizeof(WCHAR));
    if (length >= MAX_PATH)
        length = MAX_PATH - 1;
    for (index = 0; index < length; ++index)
        narrow[index] = (path[index] < ASCII_HIGH_FIRST) ? (CHAR)path[index] : '?';
    narrow[length] = 0;
    /* Compare the FILE NAME, case-insensitively: the list gives a name, the PEB
     * gives a full path, and it is the tail that has to agree.
     */
    /* [CAUTION]: NO CRT IN THIS LINK (see docs: the host links -nostdlib), so the length
     * is counted here rather than borrowed from a library that is not there.
     */
    {   INT wantLength = 0, nameLength = length;
        while (want[wantLength])
            ++wantLength;
        {
        PCSTR tail = narrow + (nameLength > wantLength ? nameLength - wantLength : 0);
        if (nameLength < wantLength)
            return 0;
        for (index = 0; index < wantLength; ++index)
        {
            CHAR tailCharacter = tail[index];
            CHAR wantCharacter = want[index];
            if (tailCharacter >= 'A' && tailCharacter <= 'Z')
                tailCharacter = (CHAR)(tailCharacter + ASCII_CASE_BIT);
            if (wantCharacter >= 'A' && wantCharacter <= 'Z')
                wantCharacter = (CHAR)(wantCharacter + ASCII_CASE_BIT);
            if (tailCharacter != wantCharacter)
                return 0;
        }
        }
    }
    return 1;
}

static PCSTR StdioFromParent(DWORD parentProcessId)
{
    HANDLE proc = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ
                              | PROCESS_DUP_HANDLE, FALSE, parentProcessId);
    HANDLE remote;
    HANDLE duplicate = NULL;
    DWORD valueType;

    if (!proc)
        return "none (no console, no redirect)";
    if (!StdioParentIs(proc, parentProcessId))           /* the offsets did not check out */
    {
        CloseHandle(proc);
        return "none (no console, no redirect)";
    }
    /* -- THE INPUT HANDLE, TAKEN IN THE SAME BREATH. Only a FILE or a PIPE:
     * see the note on g_StdinHandle. Failure here is silent on purpose -- it must
     * never cost the output handle, which is the one being measured.
     */
    {   HANDLE parentStdin = StdioPebHandle(proc, RUPP_OFF_STDIN), duplicateStdin = NULL;
        if (parentStdin && DuplicateHandle(proc, parentStdin, GetCurrentProcess(), &duplicateStdin, 0, FALSE,
                                   DUPLICATE_SAME_ACCESS))
        {
            DWORD inputType = GetFileType(duplicateStdin);
            if (inputType == FILE_TYPE_DISK || inputType == FILE_TYPE_PIPE)
                g_StdinHandle = duplicateStdin;
            else
                CloseHandle(duplicateStdin);
        }
    }
    remote = StdioPebStdout(proc);
    if (!remote)
    {
        CloseHandle(proc);
        return "none (no console, no redirect)";
    }
    if (!DuplicateHandle(proc, remote, GetCurrentProcess(), &duplicate, 0, FALSE,
                         DUPLICATE_SAME_ACCESS))
    {
        CloseHandle(proc);
        return "none (no console, no redirect)";
    }
    CloseHandle(proc);
    valueType = GetFileType(duplicate);
    if (valueType == FILE_TYPE_DISK || valueType == FILE_TYPE_PIPE)
    {
        g_Stdio = duplicate;
        return "duplicated from the parent (REDIRECTED)";
    }
    if (valueType == FILE_TYPE_CHAR)
    {
        g_Stdio = duplicate;
        return "duplicated from the parent (its console)";
    }
    CloseHandle(duplicate);
    return "none (no console, no redirect)";
}

PCSTR StdioInitialize(VOID)
{
    HANDLE handle = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD valueType = (handle && handle != INVALID_HANDLE_VALUE) ? GetFileType(handle) : FILE_TYPE_UNKNOWN;

    if (valueType == FILE_TYPE_DISK || valueType == FILE_TYPE_PIPE)
    {
        g_Stdio = handle;                       /* redirected: write straight to it */
        return "inherited (redirected)";
    }
    if (valueType == FILE_TYPE_CHAR)
    {
        g_Stdio = handle;
        return "inherited console";
    }
    if (OsCompatAttachConsole(ATTACH_PARENT_PROCESS))
    {
        g_Stdio = CreateFileA(HOST_DEVICE_CONSOLE_OUTPUT, GENERIC_WRITE, FILE_SHARE_WRITE, NULL,
                              OPEN_EXISTING, 0, NULL);
        if (g_Stdio != INVALID_HANDLE_VALUE)
            return "attached parent console";
    }
    /* ATTACH_PARENT_PROCESS FAILED. FIND THE PARENT OURSELVES. (GH #131):
     * That constant asks the kernel for "the process that created me", and an
     * IFEO-substituted image is not created the way a normal child is -- which
     * is one explanation for why it fails here while stock ntvdm has a console.
     * So walk the process list, find whose child we are, and attach to THAT
     * pid explicitly. If the answer is the same, this fails the same way and
     * we have eliminated a second route rather than assumed one.
     */
    {   HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        DWORD threadId = GetCurrentProcessId();
        DWORD parentProcessId = 0;
        if (snap != INVALID_HANDLE_VALUE)
        {
            PROCESSENTRY32 entry;
            entry.dwSize = sizeof(entry);
            if (Process32First(snap, &entry))
            {
                do
                {
                    if (entry.th32ProcessID == threadId)
                    {
                        parentProcessId = entry.th32ParentProcessID;
                        break;
                    }
                }
                while (Process32Next(snap, &entry));
            }
            CloseHandle(snap);
        }
        if (parentProcessId && OsCompatAttachConsole(parentProcessId))
        {
            g_Stdio = CreateFileA(HOST_DEVICE_CONSOLE_OUTPUT, GENERIC_WRITE, FILE_SHARE_WRITE, NULL,
                                  OPEN_EXISTING, 0, NULL);
            if (g_Stdio != INVALID_HANDLE_VALUE)
                return "attached console by parent pid";
        }
        g_StdioParentProcessId = parentProcessId;              /* reported at exit either way */

        /* -- ROUTE SIX: TAKE IT OUT OF THE PARENT. (GH #131, session 57)
         * Five routes have failed and the oracle says the handle EXISTS: run
         * `hello.com > out.txt` at a cmd prompt and STOCK ntvdm writes 136
         * bytes to the file while we write 0. The difference is not the
         * plumbing -- it is that an IFEO-substituted image is not handed the
         * creator's standard handles. Measured, in the same run:
         *
         *     si.hStdOut=0x00000000/tffffffff  sf=0x00000000
         *
         * -- no STARTF_USESTDHANDLES, no handles, nothing to inherit. And the
         * STOCK half of that comparison is not in the debugger shape at all
         * (the harness drops the IFEO key for it), which is exactly why it
         * works there and not here. So the asymmetry IS the substitution.
         *
         * BUT THE HANDLE IS STILL OPEN IN THE PARENT. cmd.exe implements
         * `> file` by putting the file on ITS OWN StandardOutput across the
         * child's lifetime, and it waits for the child -- so while we run, the
         * thing we want is sitting in cmd's process parameters. Read it, and
         * duplicate it into this process.
         *
         * [CAUTION]: AND THE STRUCTURE OFFSETS ARE PROVEN ON OURSELVES FIRST, which is
         * the only reason this is allowed to exist. PEB.ProcessParameters and
         * RTL_USER_PROCESS_PARAMETERS.StandardOutput are undocumented offsets,
         * and this project's rule is that an expectation is never written from
         * memory. So the code reads its OWN PEB by the same path and checks
         * the answer against GetStdHandle(): if our own StandardOutput does
         * not come back where the offsets say it should, the layout is wrong
         * on this machine and the parent is NOT touched. A wrong offset then
         * costs a log line instead of a duplicated handle to something else
         * entirely.
         */
        if (parentProcessId)
        {
            PCSTR why = StdioFromParent(parentProcessId);
            if (g_Stdio != INVALID_HANDLE_VALUE && g_Stdio)
                return why;
        }
    }
    return "none (no console, no redirect)";
}

/* THE FIFTH ROUTE: THE HANDLES CSRSS HANDS THE VDM. (GH #131):
 * Runs after GetNextVDMCommand, because that is what fills the fields. Only
 * ever UPGRADES: if StdioInitialize() already found something at startup we keep
 * it, because an inherited redirect is the user's own and outranks anything
 * we go looking for.
 * - TWO SOURCES, IN THE ORDER STOCK ntvdm READS THEM:
 *   1. VDM_COMMAND_INFO.StdOut  -- the dedicated field, +0x14 in the struct.
 *   2. StartupInfo.hStdOutput   -- the same handles by another road.
 *
 * [CAUTION]: (2) IS NO LONGER GATED ON STARTF_USESTDHANDLES. That flag is what the
 * previous cut tested, CSRSS leaves dwFlags at 0, and so a handle that was
 * sitting right there was refused on the strength of a flag about how a
 * Win32 CreateProcess would have been called. GetFileType is the real test:
 * a handle we can name the type of is a handle we can write to, and one we
 * cannot is rejected either way.
 *
 * [CAUTION]: NOT CLOSED HERE, and deliberately: stdin. A guest reading INT 21h AH=01
 * from a pipe needs HostConsoleIn to read StdIn instead of the keyboard VDD,
 * which is a different mechanism (blocking, on the exec thread). The value is
 * logged so the next session starts from a measurement rather than a guess.
 */
/* Adopt h as the real stdout if it is writable at all, and say what it turned out
 * to be. NULL = not usable, so the caller falls through to the next source.
 */
static PCSTR StdioAdopt(HANDLE handle)
{
    DWORD valueType;

    if (!handle || handle == INVALID_HANDLE_VALUE)
        return NULL;
    valueType = GetFileType(handle) & ~FILE_TYPE_REMOTE;
    if (valueType == FILE_TYPE_UNKNOWN)
        return NULL;
    g_Stdio = handle;
    return (valueType == FILE_TYPE_DISK) ? "redirected to a file"
         : (valueType == FILE_TYPE_PIPE) ? "a pipe"
                                  : "the console";
}

PCSTR StdioInitializeVdm(VOID)
{
    PCSTR what;

    if (g_Stdio != INVALID_HANDLE_VALUE)
        return g_StdioHow;                                    /* never downgrade */
    if ((what = StdioAdopt(g_CommandInfo.StdOut)) != NULL)
    {
        g_StdioSource = "CSRSS VDM StdOut";
        return what;
    }
    if ((what = StdioAdopt(g_CommandInfo.StartupInfo.hStdOutput)) != NULL)
    {
        g_StdioSource = "CSRSS StartupInfo";
        return what;
    }
    return g_StdioHow;
}

/* DOS console output (INT 21h AH=02/09/40) -> the video VDD teletype, AND the
 * real standard output when there is one.
 *
 * [CAUTION]: BUFFERED, AND FLUSHED ON EVERY NEWLINE. One WriteFile per character is the
 * same shape as the per-line CreateFile that cost Skyroads 24% of its timer
 * ticks; flushing per line keeps `| more` and an interactive prompt responsive
 * without paying a syscall per byte.
 */
VOID HostConsoleOut(PVOID context, BYTE ch)
{
    (VOID)context;
    HOST_LOCK();
    VddVideoPutChar(&g_Video, ch);
    if (g_Stdio != INVALID_HANDLE_VALUE)
    {
        if (g_StdioLength < sizeof(g_StdioBuffer))
            g_StdioBuffer[g_StdioLength++] = (CHAR)ch;
        if (ch == '\n' || g_StdioLength >= sizeof(g_StdioBuffer))
            StdioFlush();
    }
    HOST_UNLOCK();
}

INT HostConsoleIn(PVOID context)
{
    WORD key;
    INT got;

    (VOID)context;
    if (g_ConsoleInPending >= 0)
    {
        INT pending = g_ConsoleInPending;
        g_ConsoleInPending = -1;
        return pending;
    }
    /* [INFO]: A REDIRECTED STDIN OUTRANKS THE KEYBOARD, and must: a program run as
     * `prog < file` is not waiting for a human, and blocking on the key event
     * would hang a batch that has no console at all. See g_StdinHandle.
     */
    if (g_StdinHandle)
        return StdinReadByte();
    for (;;)
    {
        HOST_LOCK();
        got = VddInputPop(&g_Input, &key);
        HOST_UNLOCK();
        if (got)
        {
            key = VddInputDosKey(key);           /* #254: grey-key E0 forms -> 83-key */
            if ((key & BYTE_MASK) == 0)
            {
                g_ConsoleInPending = (key >> BYTE_SHIFT) & BYTE_MASK;
                return ASCII_NUL;
            }
            return key & BYTE_MASK;
        }
        if (!g_Running)
            return ASCII_ESCAPE;                        /* window gone -> unblock as ESC */
        WaitForSingleObject(g_KeyEvent, INPUT_KEY_WAIT_MS);
    }
}

static INT TypeInPop(VOID)
{
    INT character = -1;

    if (g_TypeInHead != g_TypeInTail)
    {
        character = (BYTE)g_TypeIn[g_TypeInHead];
        g_TypeInHead = (g_TypeInHead + 1) % TYPEIN_CAP;
    }
    return character;
}

/* Queue a string. All or nothing: a half-typed command is worse than none. */
INT TypeInPush(PCSTR text)
{
    INT length = lstrlenA(text);
    INT used;
    INT index;

    HOST_LOCK();
    used = (g_TypeInTail - g_TypeInHead + TYPEIN_CAP) % TYPEIN_CAP;
    if (used + length >= TYPEIN_CAP - 1)
    {
        HOST_UNLOCK();
        return 0;
    }
    for (index = 0; index < length; ++index)
    {
        g_TypeIn[g_TypeInTail] = text[index];
        g_TypeInTail = (g_TypeInTail + 1) % TYPEIN_CAP;
    }
    HOST_UNLOCK();
    return 1;
}

/* Non-blocking console read (INT 21h AH=06 DL=FF): a key char, or -1 if none. */
INT HostConsoleInNoBlock(PVOID context)
{
    WORD key;
    INT got;

    (VOID)context;
    if (g_ConsoleInPending >= 0)
    {
        INT pending = g_ConsoleInPending;
        g_ConsoleInPending = -1;
        return pending;
    }
    if (g_StdinHandle)
        return StdinReadByte();                  /* a file is always ready */
    HOST_LOCK();
    {
        INT pending = TypeInPop();
        if (pending >= 0)
        {
            HOST_UNLOCK();
            return pending;
        }
    }
    got = VddInputPop(&g_Input, &key);
    HOST_UNLOCK();
    if (!got)
        return -1;
    key = VddInputDosKey(key);                   /* #254: grey-key E0 forms -> 83-key */
    if ((key & BYTE_MASK) == 0)
    {
        g_ConsoleInPending = (key >> BYTE_SHIFT) & BYTE_MASK;
        return ASCII_NUL;
    }
    return key & BYTE_MASK;
}

/* Non-blocking console status (INT 21h AH=0B / AH=06 DL=FF peek): 1 if a key is ready.
 * An owed scancode counts as ready, or a program that polls status before reading would
 * stall halfway through an arrow.
 */
INT HostConsolePeek(PVOID context)
{
    WORD key;
    INT got;

    (VOID)context;
    if (g_ConsoleInPending >= 0)
        return 1;
    /* [CAUTION]: A REDIRECTED INPUT IS ALWAYS "READY", INCLUDING AT END OF FILE -- the
     * read that follows returns Ctrl-Z immediately. Answering "not ready"
     * there would park a polling program forever on a file that has nothing
     * left to give, which is the hang this whole route exists to remove.
     */
    if (g_StdinHandle)
        return 1;
    HOST_LOCK();
    if (g_TypeInHead != g_TypeInTail)
    {
        HOST_UNLOCK();
        return 1;
    }
    got = VddInputPeek(&g_Input, &key);
    HOST_UNLOCK();
    return got;
}

/* Reflect CF/ZF a bus interrupt returned onto the FLAGS the INT pushed on the
 * V86 stack (SS:SP+4) -- the handler's IRET restores them.
 */
VOID HostSetFlags(volatile BYTE *tib, BYTE carryFlag, BYTE zeroFlag)
{
    volatile WORD *flagsPointer = (volatile WORD *)((VDM_REG16(tib, VTIB_SS) << PARAGRAPH_SHIFT)
                         + ((VDM_REG16(tib, VTIB_ESP) + X86_FRAME16_FLAGS) & WORD_MASK));

    if (carryFlag)
        *flagsPointer |= EFLAGS_CF;
    else
        *flagsPointer &= (WORD)~EFLAGS_CF;
    if (zeroFlag)
        *flagsPointer |= EFLAGS_ZF;
    else
        *flagsPointer &= (WORD)~EFLAGS_ZF;
}

/* --- XMS (M4) -------------------------------------------------------------- *
 * Extended memory lives on the host heap (above the 1MB the V86 map covers), so
 * each EMB is a VirtualAlloc block; DosXmsMove() memcpys between it and the guest's
 * conventional window. The XMS entry point is a BOP stub reached by FAR CALL (so
 * it ends in RETF, not IRET); INT 2Fh AX=4300/4310 advertise it.
 */
PVOID XmsHostAllocate(PVOID context, DWORD kilobytes)
{
    (VOID)context;
    return VirtualAlloc(NULL, (SIZE_T)kilobytes * BYTES_PER_KILOBYTE, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
}

VOID XmsHostFree(PVOID context, PVOID memory, DWORD kilobytes)
{
    (VOID)context;
    (VOID)kilobytes;
    if (memory)
        VirtualFree(memory, 0, MEM_RELEASE);
}

/* Service one XMS far-call (function in AH). XMS returns AX=1 success / AX=0 fail
 * with BL=error code -- it does NOT use the carry flag, so no pushed-FLAGS edit.
 */
VOID HostXms(volatile BYTE *tib)
{
    DWORD ah = (VDM_REG(tib, VTIB_EAX) >> BYTE_SHIFT) & BYTE_MASK;
    BYTE error = DOS_XMS_ERROR_NOT_IMPLEMENTED;
    WORD handle;
    WORD newHandle;
    DWORD largest;
    DWORD totalFree;
    DWORD linear;
    BYTE lock;
    BYTE freeHandles;
    DWORD sizeKb;

    #define X_SETAX(v) VDM_SET16(tib, VTIB_EAX, (v))
    #define X_SETBX(v) VDM_SET16(tib, VTIB_EBX, (v))
    #define X_SETDX(v) VDM_SET16(tib, VTIB_EDX, (v))
    #define X_SETBL(v) VDM_REG(tib, VTIB_EBX) = (VDM_REG(tib, VTIB_EBX) & ~BYTE_MASK_U) | ((v) & BYTE_MASK)
    #define X_SETBH(v) VDM_REG(tib, VTIB_EBX) = (VDM_REG(tib, VTIB_EBX) & ~HIGH_BYTE_MASK_U) | (((DWORD)(v) & BYTE_MASK) << BYTE_SHIFT)
    #define X_FAIL(e)  do { X_SETAX(0); X_SETBL(e); } while (0)

    switch (ah)
    {
    case DOS_XMS_FN_GET_VERSION:                                  /* get version */
        /* DX=0: no HMA. [CAUTION] REPORTING 1 HERE WAS TRIED AS A DIAGNOSTIC AND
         * REFUTED (GH #47): MEM.EXE still asked the version twice and stopped,
         * still reported Extended (XMS) 0K. So the HMA flag is not what gates
         * it, and claiming an HMA we do not provide would have been a lie for
         * nothing. Fourth refuted hypothesis on that issue.
         */
        /* - DX NOW ANSWERS FROM THE MEMORY, NOT FROM A CONSTANT: 1 only when the
         * HMA is really committed (see the VirtualAlloc at startup). The note
         * above stands -- claiming an HMA we do not provide fixed nothing and
         * was a lie; providing one and then saying so is a different act.
         */
        X_SETAX(DOS_XMS_VERSION);
        X_SETBX(DOS_XMS_REVISION);
        X_SETDX(g_Hma ? 1 : 0);
        break;

    case DOS_XMS_FN_REQUEST_HMA:                                  /* request HMA: DX = bytes needed */
        /* Oracle (6.22 + HIMEM, DOS=HIGH): BL=0x91 "already in use". Ours is free
         * at boot, so a first caller gets it. DX=0xFFFF is the documented "I am a
         * TSR/driver, give me all of it"; anything larger than the HMA is refused
         * with the same code HIMEM uses for "your request does not fit".
         */
        if (!g_Hma)
            X_FAIL(DOS_XMS_ERROR_NO_HMA);
        else if (g_Xms.IsHmaAllocated)
            X_FAIL(DOS_XMS_ERROR_HMA_IN_USE);
        else
        {
            g_Xms.IsHmaAllocated = 1;
            X_SETAX(1);
            X_SETBL(0);
        }
        break;

    case DOS_XMS_FN_RELEASE_HMA:                                  /* release HMA */
        if (!g_Hma)
            X_FAIL(DOS_XMS_ERROR_NO_HMA);
        else if (!g_Xms.IsHmaAllocated)
            X_FAIL(DOS_XMS_ERROR_HMA_NOT_ALLOCATED);
        else
        {
            g_Xms.IsHmaAllocated = 0;
            X_SETAX(1);
            X_SETBL(0);
        }
        break;

    /* A20 IS ONE WIRE AND XMS IS ONLY ONE OF ITS THREE DOORS (Importance = 3):
     * `g_Xms.IsA20Enabled` used to be the ONLY A20 state in the host: the 8042's output
     * port was not implemented and port 92h was claimed by nothing, so a guest
     * that opened the gate the hardware way -- which is what HIMEM, every DOS
     * extender and most loaders actually do -- and then asked XMS AH=07h was
     * told it was SHUT. The honest reading of that answer is "this machine
     * cannot do XMS".
     * - The controller owns the bit now (VddInputGetA20/set, and the same bit
     *   backs port 92h), and these three cases are a view onto it. `g_Xms.IsA20Enabled` is
     *   kept in step so nothing else that reads it goes stale.
     *
     * [CAUTION]: STILL NO ADDRESS WRAP. That decision is separate, recorded at the top of
     * this file and in dos_xms.h, and it still stands -- what was wrong was that
     * the three ways of ASKING disagreed with each other.
     */
    case DOS_XMS_FN_GLOBAL_ENABLE_A20:
    case DOS_XMS_FN_LOCAL_ENABLE_A20:                                   /* enable A20 (global/local) */
        VddInputSetA20(&g_Input, TRUE);
        g_Xms.IsA20Enabled = 1;
        X_SETAX(1);
        break;

    case DOS_XMS_FN_GLOBAL_DISABLE_A20:
    case DOS_XMS_FN_LOCAL_DISABLE_A20:                                   /* disable A20 */
        VddInputSetA20(&g_Input, FALSE);
        g_Xms.IsA20Enabled = 0;
        X_SETAX(1);
        break;

    case DOS_XMS_FN_QUERY_A20:                                              /* query A20 */
        g_Xms.IsA20Enabled = VddInputGetA20(&g_Input);
        X_SETAX(g_Xms.IsA20Enabled ? 1 : 0);
        X_SETBL(0);
        break;

    case DOS_XMS_FN_QUERY_FREE:                                  /* query free extended memory */
        DosXmsQueryFreeMemory(&g_Xms, &largest, &totalFree);
        X_SETAX(largest > MAXWORD ? MAXWORD : largest);
        X_SETDX(totalFree > MAXWORD ? MAXWORD : totalFree);
        X_SETBL(largest ? 0 : DOS_XMS_ERROR_OUT_OF_MEMORY);
        /* #167: BH is undefined for 08h and the references disagree: stock NTVDM leaves
         * it alone (p_xms: BX=B100 over the poison), 6.22's HIMEM writes AAh (BX=AA00).
         * Settings > General > "behave like" picks which.
         */
        if (g_BehaveDos622)
            X_SETBH(DOS_XMS_HIMEM622_QUERY_BH);
        break;

    case DOS_XMS_FN_ALLOCATE:                                  /* allocate EMB: DX=KB */
        if (DosXmsAllocate(&g_Xms, VDM_REG16(tib, VTIB_EDX), &newHandle, &error))
        {
            X_SETAX(1);
            X_SETDX(newHandle);
        }
        else
            X_FAIL(error);
        break;

    case DOS_XMS_FN_FREE:                                  /* free EMB: DX=handle */
        if (DosXmsFree(&g_Xms, (WORD)VDM_REG16(tib, VTIB_EDX), &error))
            X_SETAX(1);
        else
            X_FAIL(error);
        break;

    case DOS_XMS_FN_MOVE:                                  /* move EMB: DS:SI -> move struct */
    {
        DWORD ds = VDM_REG16(tib, VTIB_DS);
        DWORD si = VDM_REG16(tib, VTIB_ESI);
        const volatile BYTE *source = (const volatile BYTE *)((ds << PARAGRAPH_SHIFT) + si);
        DOS_XMS_MOVE move;
        move.Length     = (DWORD)source[DOS_XMS_MOVE_LENGTH] | ((DWORD)source[DOS_XMS_MOVE_LENGTH + 1] << BYTE_SHIFT) | ((DWORD)source[DOS_XMS_MOVE_LENGTH + 2] << WORD_SHIFT) | ((DWORD)source[DOS_XMS_MOVE_LENGTH + 3] << TOP_BYTE_SHIFT);
        move.SourceHandle = (WORD)(source[DOS_XMS_MOVE_SOURCE_HANDLE] | (source[DOS_XMS_MOVE_SOURCE_HANDLE + 1] << BYTE_SHIFT));
        move.SourceOffset = (DWORD)source[DOS_XMS_MOVE_SOURCE_OFFSET] | ((DWORD)source[DOS_XMS_MOVE_SOURCE_OFFSET + 1] << BYTE_SHIFT) | ((DWORD)source[DOS_XMS_MOVE_SOURCE_OFFSET + 2] << WORD_SHIFT) | ((DWORD)source[DOS_XMS_MOVE_SOURCE_OFFSET + 3] << TOP_BYTE_SHIFT);
        move.DestinationHandle = (WORD)(source[DOS_XMS_MOVE_DESTINATION_HANDLE] | (source[DOS_XMS_MOVE_DESTINATION_HANDLE + 1] << BYTE_SHIFT));
        move.DestinationOffset = (DWORD)source[DOS_XMS_MOVE_DESTINATION_OFFSET] | ((DWORD)source[DOS_XMS_MOVE_DESTINATION_OFFSET + 1] << BYTE_SHIFT) | ((DWORD)source[DOS_XMS_MOVE_DESTINATION_OFFSET + 2] << WORD_SHIFT) | ((DWORD)source[DOS_XMS_MOVE_DESTINATION_OFFSET + 3] << TOP_BYTE_SHIFT);
        if (DosXmsMove(&g_Xms, NULL, &move, &error))
            X_SETAX(1);
        else
            X_FAIL(error);
        break; }

    case DOS_XMS_FN_LOCK:                                  /* lock EMB: DX=handle -> DX:BX linear */
        handle = (WORD)VDM_REG16(tib, VTIB_EDX);
        if (DosXmsLock(&g_Xms, handle, &linear, &error))
        {
            X_SETAX(1);
            X_SETDX(linear >> WORD_SHIFT);
            X_SETBX(linear & WORD_MASK);
        }
        else
            X_FAIL(error);
        break;

    case DOS_XMS_FN_UNLOCK:                                  /* unlock EMB: DX=handle */
        if (DosXmsUnlock(&g_Xms, (WORD)VDM_REG16(tib, VTIB_EDX), &error))
            X_SETAX(1);
        else
            X_FAIL(error);
        break;

    case DOS_XMS_FN_GET_HANDLE_INFO:                                  /* get handle info: DX=handle */
        handle = (WORD)VDM_REG16(tib, VTIB_EDX);
        if (DosXmsGetHandleInformation(&g_Xms, handle, &lock, &freeHandles, &sizeKb, &error))
        {
            X_SETAX(1);
            X_SETBH(lock);
            X_SETBL(freeHandles);
            X_SETDX(sizeKb > MAXWORD ? MAXWORD : sizeKb);
        }
        else
            X_FAIL(error);
        break;

    case DOS_XMS_FN_REALLOCATE:                                  /* reallocate EMB: BX=new KB, DX=handle */
        handle = (WORD)VDM_REG16(tib, VTIB_EDX);
        if (DosXmsReallocate(&g_Xms, handle, VDM_REG16(tib, VTIB_EBX), &error))
            X_SETAX(1);
        else
            X_FAIL(error);
        break;

    case DOS_XMS_FN_REQUEST_UMB:
        X_SETAX(0);
    X_SETBL(DOS_XMS_ERROR_NO_UMB);
    X_SETDX(0);
    break;  /* request UMB: none */

    case DOS_XMS_FN_RELEASE_UMB:
        X_FAIL(DOS_XMS_ERROR_INVALID_UMB);
    break;                          /* release UMB */

    default:
        X_FAIL(DOS_XMS_ERROR_NOT_IMPLEMENTED);
    break;
    }
    #undef X_SETAX
    #undef X_SETBX
    #undef X_SETDX
    #undef X_SETBL
    #undef X_SETBH
    #undef X_FAIL
}

/* --- EMS (M4) -------------------------------------------------------------- *
 * Expanded memory lives on the host heap (pages * 16KB per handle); the 64KB
 * page frame at E000:0 is real V86 RAM (v86 Map 5). DosEmsMapPage memcpys logical
 * pages in/out of the frame windows (page-frame shadowing). INT 67h carries the
 * function in AH and returns status in AH (0 = ok).
 */
PVOID EmsHostAllocate(PVOID context, DWORD pages)
{
    (VOID)context;
    return VirtualAlloc(NULL, (SIZE_T)pages * DOS_EMS_PAGE_SIZE, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
}

VOID EmsHostFree(PVOID context, PVOID memory, DWORD pages)
{
    (VOID)context;
    (VOID)pages;
    if (memory)
        VirtualFree(memory, 0, MEM_RELEASE);
}

/* Service one INT 67h (EMM) call (function in AH; status back in AH). */
VOID HostEms(volatile BYTE *tib)
{
    DWORD ah = (VDM_REG(tib, VTIB_EAX) >> BYTE_SHIFT) & BYTE_MASK;
    BYTE error = DOS_EMS_ERROR_UNDEFINED_FUNCTION;
    WORD handle = 0;
    WORD unallocatedPages = 0;
    WORD totalPages = 0;

    #define E_SETAH(v) VDM_REG(tib, VTIB_EAX) = (VDM_REG(tib, VTIB_EAX) & ~HIGH_BYTE_MASK_U) | (((DWORD)(v) & BYTE_MASK) << BYTE_SHIFT)
    #define E_SETAL(v) VDM_REG(tib, VTIB_EAX) = (VDM_REG(tib, VTIB_EAX) & ~BYTE_MASK_U) | ((v) & BYTE_MASK)
    #define E_SETBX(v) VDM_SET16(tib, VTIB_EBX, (v))
    #define E_SETDX(v) VDM_SET16(tib, VTIB_EDX, (v))

    switch (ah)
    {
    case DOS_EMS_FN_GET_STATUS:
        E_SETAH(DOS_EMS_STATUS_OK);
    break;                  /* get manager status */

    case DOS_EMS_FN_GET_PAGE_FRAME:
        E_SETBX(g_Ems.FrameSegment);
    E_SETAH(DOS_EMS_STATUS_OK);
    break;  /* page frame seg */

    case DOS_EMS_FN_GET_PAGE_COUNTS:                                          /* unallocated/total pages */
        DosEmsGetPageCounts(&g_Ems, &unallocatedPages, &totalPages);
        E_SETBX(unallocatedPages);
        E_SETDX(totalPages);
        E_SETAH(DOS_EMS_STATUS_OK);
        break;

    case DOS_EMS_FN_ALLOCATE:                                          /* allocate BX pages -> DX handle */
        if (DosEmsAllocatePages(&g_Ems, VDM_REG16(tib, VTIB_EBX), &handle, &error))
        {
            E_SETDX(handle);
            E_SETAH(DOS_EMS_STATUS_OK);
        }
        else
            E_SETAH(error);
        break;

    case DOS_EMS_FN_MAP:                                          /* map: AL=phys BX=logical DX=handle */
        if (DosEmsMapPage(&g_Ems, (BYTE)(VDM_REG(tib, VTIB_EAX) & BYTE_MASK),
                    (WORD)VDM_REG16(tib, VTIB_EBX),
                    (WORD)VDM_REG16(tib, VTIB_EDX), &error))
            E_SETAH(DOS_EMS_STATUS_OK);
        else
            E_SETAH(error);
        break;

    case DOS_EMS_FN_DEALLOCATE:                                          /* deallocate DX handle */
        if (DosEmsDeallocatePages(&g_Ems, (WORD)VDM_REG16(tib, VTIB_EDX), &error))
            E_SETAH(DOS_EMS_STATUS_OK);
        else
            E_SETAH(error);
        break;

    case DOS_EMS_FN_GET_VERSION:
        E_SETAL(DOS_EMS_VERSION);
    E_SETAH(DOS_EMS_STATUS_OK);
    break;  /* EMM version 4.0 */

    case DOS_EMS_FN_SAVE_PAGE_MAP:                                          /* save page map: DX handle */
        if (DosEmsSavePageMap(&g_Ems, (WORD)VDM_REG16(tib, VTIB_EDX), &error))
            E_SETAH(DOS_EMS_STATUS_OK);
        else
            E_SETAH(error);
        break;

    case DOS_EMS_FN_RESTORE_PAGE_MAP:                                          /* restore page map: DX handle */
        if (DosEmsRestorePageMap(&g_Ems, (WORD)VDM_REG16(tib, VTIB_EDX), &error))
            E_SETAH(DOS_EMS_STATUS_OK);
        else
            E_SETAH(error);
        break;

    case DOS_EMS_FN_GET_HANDLE_COUNT:
        E_SETBX(DosEmsGetHandleCount(&g_Ems));
    E_SETAH(DOS_EMS_STATUS_OK);
    break;  /* # handles */

    case DOS_EMS_FN_GET_HANDLE_PAGES:                                          /* pages owned by DX handle */
        if (DosEmsGetHandlePages(&g_Ems, (WORD)VDM_REG16(tib, VTIB_EDX), &unallocatedPages, &error))
        {
            E_SETBX(unallocatedPages);
            E_SETAH(DOS_EMS_STATUS_OK);
        }
        else
            E_SETAH(error);
        break;

    case DOS_EMS_FN_GET_ALL_HANDLE_PAGES:                                          /* all handle pages -> ES:DI, BX */
    {
        BYTE pairs[DOS_EMS_MAX_HANDLES * DOS_EMS_HANDLE_PAGES_ENTRY];
        volatile BYTE *destination = (volatile BYTE *)(ULONG_PTR)
            ((VDM_REG16(tib, VTIB_ES) << PARAGRAPH_SHIFT) + VDM_REG16(tib, VTIB_EDI));
        INT count = DosEmsGetAllHandlePages(&g_Ems, pairs);
        INT index;
        for (index = 0; index < count * DOS_EMS_HANDLE_PAGES_ENTRY; ++index)
            destination[index] = pairs[index];
        E_SETBX((WORD)count);
        E_SETAH(DOS_EMS_STATUS_OK);
        break; }

    case DOS_EMS_FN_HANDLE_NAME:                                          /* handle name: AL=0 get ES:DI, 1 set DS:SI */
    {
        DWORD int53Al = VDM_REG(tib, VTIB_EAX) & BYTE_MASK;
        volatile BYTE *nameBuffer = (int53Al == 0)
            ? (volatile BYTE *)(ULONG_PTR)((VDM_REG16(tib, VTIB_ES) << PARAGRAPH_SHIFT) + VDM_REG16(tib, VTIB_EDI))
            : (volatile BYTE *)(ULONG_PTR)((VDM_REG16(tib, VTIB_DS) << PARAGRAPH_SHIFT) + VDM_REG16(tib, VTIB_ESI));
        if (int53Al > DOS_EMS_HANDLE_NAME_SET)
            E_SETAH(DOS_EMS_ERROR_INVALID_SUBFUNCTION);                                                       /* LIM: invalid subfunction */
        else if (DosEmsGetSetHandleName(&g_Ems, (WORD)VDM_REG16(tib, VTIB_EDX),
                                 (INT)int53Al, nameBuffer, &error))
            E_SETAH(DOS_EMS_STATUS_OK);
        else
            E_SETAH(error);
        break; }

    case DOS_EMS_FN_REALLOCATE:                                          /* reallocate: BX pages, DX handle */
        if (DosEmsReallocatePages(&g_Ems, (WORD)VDM_REG16(tib, VTIB_EDX),
                        (WORD)VDM_REG16(tib, VTIB_EBX), &error))
        {
            DosEmsGetHandlePages(&g_Ems, (WORD)VDM_REG16(tib, VTIB_EDX), &unallocatedPages, &error);
            E_SETBX(unallocatedPages);
            E_SETAH(DOS_EMS_STATUS_OK);
        }
        else
            E_SETAH(error);
        break;

    default:
        E_SETAH(DOS_EMS_ERROR_UNDEFINED_FUNCTION);
    break;
    }
    #undef E_SETAH
    #undef E_SETAL
    #undef E_SETBX
    #undef E_SETDX
}

/* Put back what the ended child may have left broken. Called with the child still at
 * depth d+1, before DosTerminate frees its memory.
 */
VOID ExecMachineRestore(INT depth, PSTR *logCursor)
{
    UINT index;
    INT needsModeSet;

    HOST_LOCK();
    for (index = 0; index < IVT_SIZE / X86_WORD_SIZE; ++index)
        PokeWord(index * X86_WORD_SIZE, g_ExecMachine[depth].Ivt[index]);
    g_Pic.Master.Imr = g_ExecMachine[depth].ImrMaster;
    g_Pic.Slave.Imr = g_ExecMachine[depth].ImrSlave;
    g_Pic.Master.Isr = 0;
    g_Pic.Slave.Isr = 0;           /* a handler it never finished */
    g_Irq0IsrSince = 0;
    if (VddPitEffectiveReload(&g_Pit) != g_ExecMachine[depth].Pit0)     /* a game's fast timer */
    {
        UINT32 value = PIT_CONTROL_CHANNEL0_SQUARE;
        UINT32 reload = g_ExecMachine[depth].Pit0 & WORD_MASK;
        VddBusIo(&g_Bus, PIT_PORT_CONTROL, 1, VDD_IO_OUT, &value);
        value = reload & BYTE_MASK;
        VddBusIo(&g_Bus, PIT_PORT_COUNTER0, 1, VDD_IO_OUT, &value);
        value = (reload >> BYTE_SHIFT) & BYTE_MASK;
        VddBusIo(&g_Bus, PIT_PORT_COUNTER0, 1, VDD_IO_OUT, &value);
    }
    /* Silence: an auto-init DMA block or a held OPL note would otherwise play on
     * into the shell.
     */
    VddSbReset(&g_Sb);
    VddOplReset(&g_Opl);
    VddGusReset(&g_Gus);
    if (g_AweOn)
        VddEmu8kReset(&g_Emu8K);             /* #233 */
    VddMpuReset(&g_Mpu);
    VddSpeakerReset(&g_Speaker);
    needsModeSet = (*(volatile BYTE *)(ULONG_PTR)(BIOS_BDA_BASE + BIOS_BDA_VIDEO_MODE) != g_ExecMachine[depth].VideoMode
              || g_Video.ModeKind != VIDEO_KIND_TEXT);
    if (needsModeSet)
    {
        NTVDD_REGISTERS registers;
        ZeroMemory(&registers, sizeof registers);
        registers.Eax = g_ExecMachine[depth].VideoMode;           /* AH=00h set mode */
        VddBusDeliverInterrupt(&g_Bus, VECTOR_VIDEO, &registers);
    }
    HOST_UNLOCK();
    if (needsModeSet)
        VideoTrapSync();
    /* Its mouse event handler lives in the block about to be freed. */
    g_MouseCallbackActive = 0;
    I33ResetState();
    InterlockedExchange(&g_MouseWantCapture, 0);
    *logCursor = LogPut(*logCursor, "CLOSEPROG: machine restored to the parent's (IVT, PIC, PIT");
    *logCursor = LogPut(*logCursor, needsModeSet ? ", video mode 0x" : ")\r\n");
    if (needsModeSet)
    {
        *logCursor = LogHexByte(*logCursor, g_ExecMachine[depth].VideoMode);
        *logCursor = LogPut(*logCursor, ")\r\n");
    }
}

/* V86 loop: 1 = a child was ended and its parent resumed, 0 = the program we were
 * started with was ended, so the run is over.
 */
INT CloseProgramNow(DOS_MACHINE *machine, PVOID tib, PSTR *logCursor, PSTR base)
{
    if (g_ExecDepth > 0)
    {
        *logCursor = LogPut(*logCursor, "CLOSEPROG: ending the program at depth ");
        *logCursor = LogHexByte(*logCursor, (UINT)g_ExecDepth); *logCursor = LogPut(*logCursor, " (File > Close Program)\r\n");
        ExecMachineRestore(g_ExecDepth - 1, logCursor);
        if (g_Routed && g_ExecDepth == 1)
            g_BackToPrompt = 1;                                 /* #208 */
        machine->IsTsrPending = 0;
        machine->ExitCode = 0;
        return DosTerminate(machine, tib, logCursor, base);
    }
    *logCursor = LogPut(*logCursor, "CLOSEPROG: ending the top-level program -- the run is over\r\n");
    LogAppend(LOG_PATH, base, *logCursor); *logCursor = base;
    machine->ExitCode = 0;
    return 0;
}

/* PUBLISH THE TABLE krnl386 READS AT SysVars+0x6A.  GH #128:
 * Called with SysVars already planted. See the DOS_WOW_* block in
 * dos_layout.h for the evidence -- this is the code half of it.
 *
 * The six entries krnl386 consults become far pointers whose SEGMENT half is
 * the SysVars segment. It ignores that half and pairs each OFFSET with a
 * selector it makes itself (DPMI 0002 on the SysVars segment), so what has to
 * be right is the offset -- and every target must therefore be reachable as
 * DOS_HDLR_SEG:<16-bit offset>. Both blocks live in the MCB-reserved resident
 * filler at DOS_CTAB_SEG, which is inside that window.
 *
 * [CAUTION]: Two of the six are seeded with real values and four are private scratch.
 * That distinction is the honest state of the evidence, not a shortcut:
 * LASTDRIVE and the current-drive byte are pinned (krnl386 answers a PM
 * INT 21h AH=19h with the latter -- observed), and the other four are only
 * known to be read and written by krnl386 itself. Scratch keeps it
 * self-consistent; when one of them turns out to matter, it gets pointed at
 * the real variable and this comment shrinks by a line.
 */
VOID DosWowPublish(volatile BYTE *handlerArea, volatile BYTE *controlTable, UINT currentDrive)
{
    /* Offsets of the two blocks as seen from the SysVars SEGMENT, which is the one
     * krnl386 builds its selector on (AH=52h's ES). [CAUTION] s81: that is DOS_SYSVARS_SEG
     * now, not DOS_HDLR_SEG -- see dos_layout.h. Both blocks are ABOVE it, so the
     * offsets stay positive.
     */
    const WORD table  = (WORD)(((DOS_CTAB_SEG << PARAGRAPH_SHIFT) + DOS_WOW_TBL_OFF)
                             - (DOS_SYSVARS_SEG << PARAGRAPH_SHIFT));
    const WORD variablesOffset = (WORD)(((DOS_CTAB_SEG << PARAGRAPH_SHIFT) + DOS_WOW_VARS_OFF)
                             - (DOS_SYSVARS_SEG << PARAGRAPH_SHIFT));
    volatile BYTE *sysVars = (volatile BYTE *)(ULONG_PTR)((DWORD)DOS_SYSVARS_SEG << PARAGRAPH_SHIFT);
    UINT index;
    volatile BYTE *tableBytes = controlTable + DOS_WOW_TBL_OFF;
    volatile BYTE *variables = controlTable + DOS_WOW_VARS_OFF;

    for (index = 0; index < DOS_WOW_VARS_LEN; ++index)
        variables[index] = 0;
    variables[0] = (BYTE)currentDrive;                        /* 0 = A:, as INT 21h AH=19h */

    /* Every entry points somewhere valid, not just the six that are read. An
     * unread entry left at 0:0 is a landmine for the next thing that reads it.
     */
    for (index = 0; index < DOS_WOW_TBL_N; ++index)
    {
        *(volatile WORD *)(tableBytes + index * X86_FAR_POINTER_SIZE) = variablesOffset;
        *(volatile WORD *)(tableBytes + index * X86_FAR_POINTER_SIZE + X86_FAR_POINTER_SEGMENT) = DOS_SYSVARS_SEG;
    }
    *(volatile WORD *)(tableBytes + DOS_WOW_E_LASTDRV) = (WORD)(DOS_SYSVARS_OFF + DOS_SYSVARS_LASTDRIVE);
    *(volatile WORD *)(tableBytes + DOS_WOW_E_CURDRV)  = (WORD)(variablesOffset + DOS_WOW_VAR_CURDRV);
    *(volatile WORD *)(tableBytes + DOS_WOW_E_C)       = (WORD)(variablesOffset + DOS_WOW_VAR_C);
    *(volatile WORD *)(tableBytes + DOS_WOW_E_E)       = (WORD)(variablesOffset + DOS_WOW_VAR_E);
    *(volatile WORD *)(tableBytes + DOS_WOW_E_D)       = (WORD)(variablesOffset + DOS_WOW_VAR_D);
    *(volatile WORD *)(tableBytes + DOS_WOW_E_F)       = (WORD)(variablesOffset + DOS_WOW_VAR_F);

    /* [WARNING]: THIS COMMENT USED TO SAY SysVars+0x6A "LANDS AT DOS_HDLR_SEG:0x00FA ... FREE".
     * It was the NAME FIELD OF THE FIRST MCB (0x5F:000A), and SysVars+0x60 onward was
     * that MCB's whole header -- the defect behind MEM /C's 1 MB "MSDOS" (#47). In
     * SysVars' own segment it is simply SysVars+0x6A.
     */
    (VOID)handlerArea;
    *(volatile WORD *)(sysVars + DOS_SYSVARS_OFF + DOS_SYSVARS_WOW_TABLE) = table;
}
