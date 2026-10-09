/* host_core.h -- the host's foundations: where its folder is and the paths inside it, the
 * OS imports bound at run time, the host lock, guest-memory peeks and pokes, small utilities.
 *
 * Declarations only (#335); defined in host_core.c. */
#ifndef NTVDMEX_HOST_CORE_H
#define NTVDMEX_HOST_CORE_H
#include <windows.h>
#include "../ntvdmex_types.h"

/* ── ★ THE FOUR IMPORTS WINDOWS 2000 DOES NOT HAVE. (2026-09-22, user on the 2000 box) ──
     The host would not START on Windows 2000: "The procedure entry point
     AddVectoredExceptionHandler could not be located in KERNEL32.dll". The loader
     resolves every static import before WinMain runs, so ONE missing export is a
     refusal to load, not a degraded feature. The import table of the built exe was
     dumped and compared: of 417 imports exactly four are XP-only --
       kernel32  AddVectoredExceptionHandler   (the PM-fault VEH)
       kernel32  AttachConsole                 (stdout to the parent's console)
       user32    RegisterRawInputDevices       (raw mouse deltas while captured)
       user32    GetRawInputData
     Everything else is NT 5.0. So these four are bound at run time, and each has a
     fallback that already existed or is the same mechanism one layer down:
       no VEH        -> the unhandled-exception filter runs the SAME handler. With no
                        CRT there is no SEH frame in this host to claim a fault first,
                        so the filter is the next thing after the VEH would have been,
                        and a filter may return EXCEPTION_CONTINUE_EXECUTION with a
                        modified context exactly as the VEH does.
       no Attach     -> the other stdout routes (inherited handle, the parent's handle)
       no raw input  -> g_MouseRawOk stays 0 and the absolute-derived delta path runs,
                        which is what the code already did when registration failed.
   ⚠ THIS IS "LOADS ON 2000", NOT "RUNS ON 2000". The NtVdmControl contract (the
     VdmInitialize block, VDM_TIB offsets, TEB+0xF18, [0x714]) was taken from XP's
     ntvdm and kernel; what 2000's do differently is unmeasured, and the first run's
     log is the instrument. The Win16 half is pinned to XP's krnl386 and is a
     separate effort. On XP nothing changes: the same four functions are found and
     used as before. `STAGE0: os=` names the version and which of the four resolved. */
typedef PVOID (WINAPI *PFN_ADD_VECTORED_EXCEPTION_HANDLER)(ULONG, PVECTORED_EXCEPTION_HANDLER);
typedef BOOL  (WINAPI *PFN_ATTACH_CONSOLE)(DWORD);
typedef BOOL  (WINAPI *PFN_REGISTER_RAW_INPUT_DEVICES)(PCRAWINPUTDEVICE, UINT, UINT);
typedef UINT  (WINAPI *PFN_GET_RAW_INPUT_DATA)(HRAWINPUT, UINT, LPVOID, PUINT, UINT);

/* ── ★★ ONE FOLDER HOLDS THE WHOLE RIG. (s61, at the user's instruction) ──────────
     Everything this host reads or writes lives under ONE directory, and NOTHING is
     written to C:. Before this, the rig was spread over five places -- 52 knob and
     flag files loose in the share ROOT, results and screenshots beside them, and
     C:\ntvdmex holding the log, target.txt, autoexit, sb.raw, PRINTOUT.TXT,
     SERIAL*.TXT, the floppy image and three probe logs -- plus C:\test and C:\game
     recreated per run and rt.bat dropped into C:\WINDOWS. The user cleared the box
     and asked for that not to happen again; this is the half of it that is the
     HOST'S doing rather than the scripts'.

       <dir>\cfg\        everything we READ: knobs, flags, target.txt, the floppy image
       <dir>\debug\out\  everything we WRITE: the log, screenshots, traces, probe dumps
       <dir>\bin\        this binary                       (bm\ = the pre-s73 name)
       <dir>\debug\rig\  the harness                       (scripts' business, not ours)
       <dir>\demo\msdos\ the user's games and demos, run IN PLACE (never copied)

   ⚠ The directories are created at startup: a knob read may legitimately find
     nothing, but a WRITE to a missing debug\out\ would fail silently and take the
     log with it -- which is the one instrument that explains every other failure.
     debug\ is created before debug\out\ -- CreateDirectoryA makes ONE level. */
/* ── ★ THE FOLDER IS WHEREVER THE HOST WAS EXTRACTED. (s71, the 17th deliverable) ────
     This was a compile-time string naming the rig's share, so a copy of NTVDMEX on any
     other machine wrote its log nowhere, read no knobs and found no target -- a zip
     that "works on my machine" and nowhere else. The root is now derived once from
     the host's own path: the exe lives in <root>\bin\ (or the older <root>\bm\), so
     the root is that folder's parent; an exe anywhere else uses its own directory.
     The old share path is only the fallback for a GetModuleFileName that fails, which
     it does not. s73 renamed bm\ to bin\ for the release layout; bm\ stays accepted so
     an already-installed s72 zip keeps finding its cfg\.
   The macros keep their names and their call sites: each expands to a call that
   composes the path into one of a ring of buffers, so `CreateFileA(CFG_("x.txt"))`
   reads as before. A returned pointer is good for the next 15 calls from any thread,
   which covers every use here (all immediate). */
#define NTVDMEX_DIR_DEFAULT "C:\\Documents and Settings\\All Users\\Documents\\ntvdmex\\"
PCSTR NtvdmexRoot(VOID);                    /* "<root>\", trailing slash */
PCSTR NtvdmexPath(PCSTR subdirectory, PCSTR name);
#define NTVDMEX_DIR NtvdmexRoot()
#define NTVDMEX_CFG   NtvdmexPath("cfg\\", "")
#define NTVDMEX_DEBUG NtvdmexPath("debug\\", "")        /* parent of out\; created first */
#include "host_strings.h"   /* defines only: the strings that are not log text */
/* #211: the FIRST host writes to debug\out\ as always; a second one at the same time writes
   to debug\out\2\, and so on (the instance claim in WinMain) -- so no host clears another's log. */
extern CHAR g_OutSubdirectory[24];
extern INT  g_Instance, g_InstanceAbandoned;

#define NTVDMEX_OUT   NtvdmexPath(g_OutSubdirectory, "")
#define CFG_(n)       NtvdmexPath("cfg\\", n)
#define OUT_(n)       NtvdmexPath(g_OutSubdirectory, n)
/* The log is the one path log.h owns; define it before including so its #ifndef
   defers to us rather than putting the log back on C:. */
#define LOG_PATH    OUT_("ntvdmhost.log")
#define SBDUMP_PATH OUT_("sb.raw")    /* the Sound Blaster output dump (main.c records, the report writes) */

/* GetVersion() at start-up: 0x0500 = 2000, 0x0501 = XP. */
extern DWORD g_OsVersion;

/* The rest of host_core.c that other files use. */
extern CRITICAL_SECTION g_Lock;
extern DWORD g_PatchMapCount;
extern PFN_ATTACH_CONSOLE           g_PfnAttachConsole;
VOID HostLockEnter(INT site);
VOID HostLockLeave(VOID);
INT HostLockTry(INT site);
INT HostReadable(PCVOID pointer, SIZE_T length);
INT HostWritable(PVOID pointer, SIZE_T length);
INT MemoryReadable(ULONG_PTR address, SIZE_T length);
BOOL OsCompatAttachConsole(DWORD processId);
VOID OsCompatBind(VOID);
VOID PatchMapClear(DWORD linear);
BYTE PatchMapGet(DWORD linear);
VOID PatchMapSet(DWORD linear, BYTE vector);
DWORD PeekWidth(DWORD linear, INT width);
WORD PeekWord(DWORD linear);
VOID PokeDword(DWORD linear, DWORD value);
VOID PokeWidth(DWORD linear, DWORD value, INT width);
VOID PokeWord(DWORD linear, WORD value);
UINT32 QpcMicroseconds(LONGLONG ticks);
UINT64 QpcMicroseconds64(LONGLONG ticks);
LONGLONG QpcTicks(UINT32 microseconds);
INT StrStrNoCase(PCSTR block, PCSTR name);
INT StringsEqual(PCSTR first, PCSTR second);

#endif
