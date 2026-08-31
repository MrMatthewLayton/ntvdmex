/* wow32.h -- the 32-bit half of WOW: krnl386's calls out to Win32.  GH #128.
 *
 * krnl386.exe is a 16-bit DLL that cannot call Win32, so it reaches a 32-bit
 * companion (real Windows: wow32.dll inside ntvdm.exe) through a native BOP.
 * Every call goes through ONE thunk at seg1:0x2bb6, reached from a per-function
 * stub, so the whole interface is a small integer namespace -- 82 function IDs,
 * enumerated in docs/research/wow32-call-surface.md.
 *
 * ─────────────────────────────────────────────────────────────────────────────
 * THE FRAME, READ OFF krnl386's OWN CODE AND CONFIRMED AGAINST THE LIVE RIG.
 *
 * A per-function stub (e.g. VirtualAlloc's, at seg1:0xb48e) is:
 *
 *     push <arg byte count> / push 0 / push <ID> / nop / push cs / call 0x2bb6
 *
 * and its CALLER reached it the same way -- `push cs` then a NEAR call, so the
 * stub can be returned from with `retf`. The thunk then does `push bp; mov bp,sp`
 * and a fixed run of pushes. That fixes every offset:
 *
 *     [bp+0]      saved BP
 *     [bp+2/+4]   near-return into the stub, and the stub's pushed CS
 *     [bp+6]      ★ THE FUNCTION ID
 *     [bp+8]      the pushed 0
 *     [bp+10]     ★ the ARGUMENT BYTE COUNT
 *     [bp+12/+14] the CALLER's return address -- offset then CS
 *     [bp+16...]  ★ THE ARGUMENTS
 *
 * ⚠ ARGUMENTS START AT bp+16, NOT bp+12. The first cut of the host's trace read
 *   them at bp+12 and so printed the caller's far return address as the first two
 *   argument words -- which is exactly why session 30 recorded VirtualAlloc's
 *   argument ORDER as "not pinned down, two readings possible". It was an
 *   instrument that lied, in this project's usual shape. The proof it is +16 is
 *   the thunk's own return path (seg1:0x2c1d):
 *       mov bx,[bp+10] / shl bx,2 / add bx,0x2ab6 / jmp bx
 *   which lands in a table of `pop bx / pop bp / add sp,0xA / retf N` stubs, one
 *   per argument size. `add sp,0xA` skips exactly the five words bp+2..bp+10, the
 *   `retf` consumes bp+12/+14, and `retf N` then discards N bytes of arguments.
 *   So the arguments are the N bytes above the far return address: bp+16.
 *
 * ★ AND THE RETURN VALUE IS NOT A REGISTER. The thunk reserves four bytes with
 *   `sub sp,4` BEFORE the BOP and unconditionally does `pop ax / pop dx` after it.
 *   Whatever we leave in AX/DX is therefore overwritten. The 32-bit side must
 *   write the DWORD into that hole, at [bp-16] (low word) and [bp-14] (high).
 *   Confirmed on hardware: in the rig's `@ss:sp` dump those two words held stale
 *   stack (0x0047, 0x0000) at the BOP -- an uninitialised return slot.
 *   Getting this wrong is silent: the guest reads garbage and blames itself.
 *
 * ─────────────────────────────────────────────────────────────────────────────
 * ARGUMENT ORDER IS PASCAL: pushed LEFT TO RIGHT, so the FIRST declared argument
 * is at the HIGHEST address and the LAST is at bp+16. Three independent call
 * sites agree, which is what makes it a fact rather than a reading:
 *   VirtualAlloc   pushes 0, size, 0x3000, 0x40   -> (lpAddress, dwSize,
 *                  flAllocationType, flProtect), and 0x3000/0x40 are exactly
 *                  MEM_COMMIT|MEM_RESERVE and PAGE_EXECUTE_READWRITE.
 *   VirtualFree    pushes addr, size, 0x8000      -> (lpAddress, dwSize,
 *                  dwFreeType) with MEM_RELEASE.
 *   GlobalMemoryStatus pushes ss, bp of a 32-byte buffer whose first DWORD it
 *                  set to 0x20 -- a MEMORYSTATUS with dwLength filled in -- and
 *                  afterwards reads [+0x0c]+[+0x14] and compares [+0x1c], i.e.
 *                  dwAvailPhys + dwAvailPageFile vs dwAvailVirtual.
 *
 * A far pointer argument is a normal 16:16: offset in the low word, SELECTOR in
 * the high word (the caller pushes the segment first, the offset second).
 *
 * ─────────────────────────────────────────────────────────────────────────────
 * NAMING. 28 of the 82 IDs are named by krnl386's OWN export table -- an entry
 * whose target IS a stub, so the export's name in the (non-)resident name table
 * is the function's name, with no inference at all. `tools/ne/wowmap.py` prints
 * the mapping. It was cross-checked before being trusted: id 0xcf was worked out
 * from its call site alone (the caller compares the result against 0x411, 0x412,
 * 0x404, 0x804, 0x0c04 -- LANGIDs) and the export table then said
 * GETSYSTEMDEFAULTLANGID. Two methods, one answer.
 */
#ifndef WOW32_H
#define WOW32_H

#include <windows.h>

/* Where each field sits relative to the thunk's BP. See the frame diagram above. */
#define WOW32_OFF_ID    6
#define WOW32_OFF_ARGB  10
#define WOW32_OFF_FROM  12           /* return address into krnl386 -- WHICH call site */
#define WOW32_OFF_ARGS  16
#define WOW32_OFF_RET   (-16)        /* the `sub sp,4` hole: low word, then high */

/* The BOP is `C4 C4 51`. Resuming the guest anywhere but past all three bytes
   restarts it mid-instruction. */
#define WOW32_BOP_LEN   3

typedef DWORD (*wow32_sel2lin_fn)(WORD sel, void *ctx);

typedef struct {
    volatile BYTE   *bp;             /* linear address of SS:BP inside the thunk */
    WORD             id;
    WORD             argb;
    WORD             from;           /* return address in krnl386 seg 1 -- the CALL SITE */
    wow32_sel2lin_fn sel2lin;        /* selector -> linear base (host's LDT view) */
    void            *ctx;
    /* Filled in by the host so a service can talk back about what it did. */
    DWORD            ret;
    int              serviced;
} wow32_frame_t;

/* ---- frame accessors ---------------------------------------------------- */

static WORD wow32_peekw(volatile BYTE *p)
{
    return (WORD)(p[0] | (p[1] << 8));
}

static void wow32_pokew(volatile BYTE *p, WORD v)
{
    p[0] = (BYTE)(v & 0xFF);
    p[1] = (BYTE)(v >> 8);
}

/* Argument WORD at byte offset `off` into the argument block. */
static WORD wow32_argw(const wow32_frame_t *f, int off)
{
    if (off < 0 || off + 2 > (int)f->argb) return 0;
    return wow32_peekw(f->bp + WOW32_OFF_ARGS + off);
}

/* Argument DWORD at byte offset `off`. */
static DWORD wow32_argd(const wow32_frame_t *f, int off)
{
    return (DWORD)wow32_argw(f, off) | ((DWORD)wow32_argw(f, off + 2) << 16);
}

/* A 16:16 far pointer argument, resolved to a host linear address.
   ⚠ Returns 0 for a null selector rather than the LDT base, so a caller that
     forgets to check cannot scribble at the bottom of the address space. */
static volatile BYTE *wow32_argptr(const wow32_frame_t *f, int off)
{
    DWORD fp = wow32_argd(f, off);
    WORD  sel = (WORD)(fp >> 16);
    DWORD base;
    if (!sel || !f->sel2lin) return NULL;
    base = f->sel2lin(sel, f->ctx);
    if (!base) return NULL;
    return (volatile BYTE *)(ULONG_PTR)(base + (fp & 0xFFFF));
}

/* Copy a NUL-terminated guest string at a far-pointer argument into a host buffer.
   Returns 1 if there was a string to copy, 0 for a null/unreadable pointer -- and the
   difference matters: the profile API gives `lpAppName == NULL` its own meaning
   ("enumerate"), so "no pointer" must not arrive at Win32 as an empty string. */
static int wow32_argstr(const wow32_frame_t *f, int off, char *out, int cap)
{
    volatile BYTE *s = wow32_argptr(f, off);
    int k = 0;
    if (!s) { if (cap) out[0] = 0; return 0; }
    while (k < cap - 1 && s[k]) { out[k] = (char)s[k]; ++k; }
    out[k] = 0;
    return 1;
}

/* ★ The return value goes in the stack hole, NOT in AX/DX -- see the header note. */
static void wow32_setret(wow32_frame_t *f, DWORD v)
{
    wow32_pokew(f->bp + WOW32_OFF_RET,     (WORD)(v & 0xFFFF));
    wow32_pokew(f->bp + WOW32_OFF_RET + 2, (WORD)(v >> 16));
    f->ret = v;
}

/* ★ What the guest WILL READ out of the return hole if nobody writes it.
   A stepped-over call leaves the `sub sp,4` hole holding whatever the stack last
   had there, and krnl386 pops it into AX:DX and branches on it -- so an
   unimplemented call is not inert, it answers at random. Two walls in this project
   were that value and not the guest: the null-`ES` fault after `WowLoadModule`
   (a stale slot passing a `cmp ax,0x21` it should have failed) and, upstream of
   it, a stale non-zero failing `or ax,ax / jne` inside LoadModule. Reading the
   hole back and PRINTING it is what lets a later reader tell "krnl386 decided
   this" from "our litter decided this". */
static DWORD wow32_peekret(const wow32_frame_t *f)
{
    return (DWORD)wow32_peekw(f->bp + WOW32_OFF_RET)
         | ((DWORD)wow32_peekw(f->bp + WOW32_OFF_RET + 2) << 16);
}

/* ── ★★ WHAT AN UNIMPLEMENTED CALL ANSWERS. (GH #128, session 36) ─────────────
     Session 35 measured that a stepped-over call is not inert: krnl386 pops the
     `sub sp,4` hole into AX:DX and branches on it. Leaving the hole unwritten
     therefore does not mean "no answer", it means "an answer drawn from whatever
     the stack last held" -- which made two separate runs stop for reasons that
     were OURS, and which no amount of re-running can reproduce or rule out.
   ★ SO ANSWER THE SAME WAY EVERY TIME. This does not make the answer TRUE -- it is
     still a call we have not implemented, and the log says so on every line. It
     makes the run REPRODUCIBLE, which is the property every other conclusion in
     this investigation rests on. A deterministic wrong answer can be traced from
     the wall back to its cause; a random one cannot.
   ★ AND ZERO IS THE BETTER CONSTANT, at both sites measured so far -- chosen from
     the two call sites' own tests, not from taste:
       0xc6  seg1:0x4795  `or ax,ax / jne <failure>`  -> litter 0x01b7 took the
             FAILURE path. Zero does not. That failure was ours.
       0x2d  seg2:0x0f16  `cmp ax,0x21 / jb`          -> litter 0x2714 passed as a
             MODULE HANDLE and ran on into the terminal #GP with a NULL parameter
             block. Zero is below 0x21, so LoadModule takes its error path and
             REPORTS, instead of faulting somewhere else.
     In both cases zero fails nearer the cause, which is the whole point.
   ⚠ NOT 0xFFFFFFFF: that is WOW32_DECLINE, and a decline is a different statement
     ("ask real DOS instead") that only holds at the sites in wow32_decline_sites.
     Reusing it here would make every unimplemented call claim to be one. */
#define WOW32_UNIMPL_RET 0u

/* ---- the function IDs we can name -------------------------------------- */
/* Names for the 28 that krnl386's export table names outright, plus the ones
   worked out from their call sites. An ID with no name here is not a gap in the
   evidence -- it is a function reached only from internal code, whose call site
   has not been read yet. `tools/ne/nedis.py --wowfunc <id>` is how. */
#define WOW32_FATALEXIT                 0x01
#define WOW32_EXITKERNELTHUNK           0x02
#define WOW32_WRITEOUTPROFILES          0x03
#define WOW32_GETVDMPOINTER32W          0x1a
#define WOW32_CALLPROCEX32W             0x1c
#define WOW32_YIELD                     0x1d
#define WOW32_WAITEVENT                 0x1e
#define WOW32_POSTEVENT                 0x1f
#define WOW32_SETPRIORITY               0x20
#define WOW32_LOCKCURRENTTASK           0x21
#define WOW32_WOWLOADMODULE             0x2d
#define WOW32_GETPROFILEINT             0x39   /* pinned from DGROUP, see below    */
#define WOW32_WOWGETNEXTVDMCOMMAND      0x70
#define WOW32_OLDYIELD                  0x75
#define WOW32_REGISTERDOSDATA           0x78   /* named from its call site, below */
#define WOW32_GETSHORTPATHNAME          0x7b
#define WOW32_ACCEPTTASKSELECTOR        0x7d   /* pinned from its call sites, below */
#define WOW32_GETPRIVATEPROFILESTRING   0x80   /* pinned from DGROUP, see below    */
#define WOW32_WOWWAITFORMSGANDEVENT     0x83
#define WOW32_WOWMSGBOX                 0x84
#define WOW32_GETDATETIME               0x86   /* call site unpacks a packed date  */
#define WOW32_GETDRIVETYPE              0x88
#define WOW32_WOWREGISTERSHELLWINDOW    0x8b
#define WOW32_FREELIBRARY32W            0x8c
#define WOW32_GETPROCADDRESS32W         0x8d
#define WOW32_DIRECTEDYIELD             0x96
#define WOW32_LOADLIBRARYEX32W          0x9a
#define WOW32_WOWQUERYPERFCOUNTER       0x9b
#define WOW32_WOWCURSORICONOP           0x9c
#define WOW32_WOWFAILEDEXEC             0x9d
#define WOW32_WOWCLOSECOMPORT           0x9f
#define WOW32_VIRTUALALLOC              0xb8
#define WOW32_VIRTUALFREE               0xb9
#define WOW32_GLOBALMEMORYSTATUS        0xbc
#define WOW32_WOWKILLREMOTETASK         0xbf
#define WOW32_MESSAGEBOX                0xc4   /* the fatal-error box; see wowmap  */
#define WOW32_WOWSHUTDOWNTIMER          0xcd
/* Serviced in main.c, not here: it needs the DOS machine. Listed so the name table
   below can print it, and so nobody adds a decline for it -- its call site
   (seg1:0x53a2) treats DX=0xFFFF as a hard error, not as "ask DOS instead". */
#define WOW32_GETCURDIR                 0xc9
#define WOW32_GETSYSTEMDEFAULTLANGID    0xcf

static const char *wow32_name(WORD id)
{
    switch (id) {
    case WOW32_FATALEXIT:              return "FatalExit";
    case WOW32_EXITKERNELTHUNK:        return "ExitKernelThunk";
    case WOW32_WRITEOUTPROFILES:       return "WriteOutProfiles";
    case WOW32_GETVDMPOINTER32W:       return "GetVDMPointer32W";
    case WOW32_CALLPROCEX32W:          return "CallProcEx32W";
    case WOW32_YIELD:                  return "Yield";
    case WOW32_WAITEVENT:              return "WaitEvent";
    case WOW32_POSTEVENT:              return "PostEvent";
    case WOW32_SETPRIORITY:            return "SetPriority";
    case WOW32_LOCKCURRENTTASK:        return "LockCurrentTask";
    case WOW32_WOWLOADMODULE:          return "WowLoadModule";
    case WOW32_WOWGETNEXTVDMCOMMAND:   return "WowGetNextVDMCommand";
    case WOW32_OLDYIELD:               return "OldYield";
    case WOW32_REGISTERDOSDATA:        return "RegisterDosData?";
    case WOW32_GETSHORTPATHNAME:       return "GetShortPathName";
    case WOW32_ACCEPTTASKSELECTOR:     return "AcceptTaskSelector?";
    case WOW32_WOWWAITFORMSGANDEVENT:  return "WowWaitForMsgAndEvent";
    case WOW32_WOWMSGBOX:              return "WowMsgBox";
    case WOW32_GETDATETIME:            return "GetDateTime?";
    case WOW32_GETDRIVETYPE:           return "GetDriveType";
    case WOW32_WOWREGISTERSHELLWINDOW: return "WowRegisterShellWindowHandle";
    case WOW32_FREELIBRARY32W:         return "FreeLibrary32W";
    case WOW32_GETPROCADDRESS32W:      return "GetProcAddress32W";
    case WOW32_DIRECTEDYIELD:          return "DirectedYield";
    case WOW32_LOADLIBRARYEX32W:       return "LoadLibraryEx32W";
    case WOW32_WOWQUERYPERFCOUNTER:    return "WowQueryPerformanceCounter";
    case WOW32_WOWCURSORICONOP:        return "WowCursorIconOp";
    case WOW32_WOWFAILEDEXEC:          return "WowFailedExec";
    case WOW32_WOWCLOSECOMPORT:        return "WowCloseComPort";
    case WOW32_VIRTUALALLOC:           return "VirtualAlloc";
    case WOW32_VIRTUALFREE:            return "VirtualFree";
    case WOW32_GLOBALMEMORYSTATUS:     return "GlobalMemoryStatus";
    case WOW32_WOWKILLREMOTETASK:      return "WowKillRemoteTask";
    case WOW32_MESSAGEBOX:             return "MessageBox?";
    case WOW32_WOWSHUTDOWNTIMER:       return "WowShutdownTimer";
    case WOW32_GETCURDIR:              return "GetCurrentDirectory";
    case WOW32_GETSYSTEMDEFAULTLANGID: return "GetSystemDefaultLangID";
    case WOW32_GETPROFILEINT:          return "GetProfileInt";
    case WOW32_GETPRIVATEPROFILESTRING: return "GetPrivateProfileString";
    default:                           return NULL;
    }
}

/* What the host learned from a REGISTERDOSDATA call, for the log and for anyone
   who later wants to reconcile krnl386's view of DOS with ours. */
typedef struct {
    int   seen;
    DWORD farptr;                     /* the 16:16 the guest passed */
} wow32_dosdata_t;

/* ── ★ DECLINING IS A REAL ANSWER, AND krnl386 ALREADY HANDLES IT ───────────
     krnl386 hooks INT 21h in protected mode and offers some functions to its
     32-bit companion first. When the companion says no, it chains to the vector
     it saved before hooking:

         5507  cmp ax, 0xffff
         550a  je  0x55a1     -> pop ax/bx/dx -> jmp 0x56c8 -> lcall cs:[0x3c]

     `cs:[0x3c]` is the PREVIOUS INT 21h handler -- which in this host is our own
     DOS layer, the one COMMAND.COM and Doom already use. So a sentinel return
     hands file I/O to working code instead of to a parallel Win32 handle table
     that would then disagree with every call that chains anyway.

   ⚠ ONLY WHERE THE CALL SITE SAYS SO. `tools/ne/wowdecline.py` checks each site
     and finds three (0x82, 0xc9, 0x71) where 0xFFFF is a plain ERROR and krnl386
     reports failure to the app rather than chaining. Declining there would turn
     "not implemented" into "the file does not exist" -- a wrong answer instead
     of a missing one, which is the more expensive kind. They are NOT in the list
     below, and the list is the tool's output, not a guess about the family.

   ⚠ Some sites test AX and some test DX, so the sentinel has to be 0xFFFFFFFF
     rather than either half.

   ▸ THE HONEST TRADE. Real WOW routes these to Win32, so a Win16 app gets NT
     file semantics (sharing modes, long names). Declining gives it our DOS
     semantics instead. For loading and running a program that is the same thing,
     and it is one line to change later -- but it IS a difference, so it is
     written down rather than discovered. */
#define WOW32_DECLINE 0xFFFFFFFFu

/* Verified declinable, with the call site that proves it. All seven are the
   INT 21h file family; declining 0x97 and 0x6f makes krnl386 re-issue a plain
   AH=3Fh / AH=40h to DOS, which is visible in its own code at 0x55a7 / 0x56c6. */
#define WOW32_FILE_OPEN        0xc1   /* seg1:0x5504  AH=3Dh                    */
#define WOW32_FILE_READ        0x97   /* seg1:0x5570  -> AH=3Fh on decline      */
#define WOW32_FILE_CLOSE       0xc2   /* seg1:0x558f  AH=3Eh                    */
#define WOW32_FILE_GETATTR     0xc7   /* seg1:0x55c4  AH=43h AL=0               */
#define WOW32_FILE_7E          0x7e   /* seg1:0x55df                            */
#define WOW32_FILE_GETDATE     0x89   /* seg1:0x5609  AH=57h AL=0               */
#define WOW32_FILE_WRITE       0x6f   /* seg1:0x56bc  -> AH=40h on decline      */
/* ── ★ THE SEEK, AND WHY IT WAS MISSED. (GH #128, session 34) ─────────────────
     `tools/ne/wowdecline.py` finds decline sites by the shape of the test after
     the call -- `test dx` or `cmp ax,0xffff`. THIS site tests neither:

         549b  call 0xb211        ; WOW32 0x98
         549e  inc  dx            ; 0xffff + 1 == 0 -> ZF
         549f  jne  0x54a4        ; serviced
         54a1  jmp  0x55a1        ; -> pop ax/bx/dx -> 0x56c8 -> lcall cs:[0x3c]

     so the tool reported ten declinable sites and never mentioned 0x98, and 0x98
     sat in the "unimplemented, stepped over" list looking like work rather than
     like a one-line answer.
   ★ IT IS THE FILE SEEK, and leaving it unanswered is what produced "NTVDM
     KERNEL: Missing 16-bit system module". krnl386 reads SYSTEM.DRV's first 0x40
     bytes, calls 0x98 with offset 0x0400 -- which is that file's e_lfanew, checked
     against the bytes on disk, not assumed -- and reads the NE header. Stepping the
     seek over left the file position where the MZ read had left it, so every
     subsequent read returned the wrong part of the file and the NE header it
     parsed was whatever followed the MZ stub.
     Declining restores the original AX (AH=42h) and chains to real DOS, which our
     PM thunk already serves. */
#define WOW32_FILE_SEEK        0x98   /* seg1:0x549b  -> AH=42h on decline      */

/* ── ★★ DECLINING IS A PROPERTY OF THE CALL SITE, NOT OF THE ID. ───────────────
     This was keyed by ID, and that is measurably wrong: krnl386 calls 0x97 (read)
     from TWO places with OPPOSITE meanings --

       seg1:0x5570  inc dx / je 0x55a7  -> mov ah,0x3f / jmp 0x56c8 -> lcall cs:[0x3c]
                    ⇒ 0xFFFF means "ask real DOS". A decline is a true statement.
       seg1:0x8a4e  inc dx / jne 0x8a59 -> xor dx,dx / or ax,0xffff / dec dx / ret
                    ⇒ 0xFFFF is RETURNED TO THE CALLER as a failure. A decline here
                      turns "we did not implement this" into "the read failed",
                      which is a WRONG ANSWER rather than a missing one.

     Session 34's failing run declined at `from=0x8a51` -- the second site -- and the
     log shows what that looks like: no `INT21h AH=3F` follows it, unlike every other
     read in the run. 0x6f (write) has the same split, at 0x56bc and 0x8aa1.

   The list is the output of `tools/ne/wowdecline.py`, which reads each site's test
     and asks whether its sentinel path reaches `lcall cs:[0x3c]`. Values are the
     RETURN address (call site + 3), because that is what the thunk frame carries at
     WOW32_OFF_FROM. Anything not listed is not declined -- an unknown site gets the
     honest "unimplemented" rather than a guess. */
typedef struct { WORD id; WORD from; } wow32_decline_site_t;

static const wow32_decline_site_t wow32_decline_sites[] = {
    { 0xb7,                0x52e5 },
    { WOW32_FILE_SEEK,     0x549e },   /* 0x98 -> AH=42h */
    { 0x77,                0x54d3 },
    { WOW32_FILE_OPEN,     0x5507 },   /* 0xc1 -> AH=3Dh */
    { WOW32_FILE_READ,     0x5573 },   /* 0x97 -> AH=3Fh */
    { WOW32_FILE_CLOSE,    0x5592 },   /* 0xc2 -> AH=3Eh */
    { WOW32_FILE_GETATTR,  0x55c7 },   /* 0xc7 -> AH=43h */
    { WOW32_FILE_7E,       0x55e2 },   /* 0x7e             */
    { WOW32_FILE_GETDATE,  0x560c },   /* 0x89 -> AH=57h */
    { 0x76,                0x5634 },
    { 0x71,                0x565e },
    { WOW32_FILE_WRITE,    0x56bf },   /* 0x6f -> AH=40h */
};

static int wow32_may_decline(WORD id, WORD from)
{
    unsigned i;
    for (i = 0; i < sizeof wow32_decline_sites / sizeof wow32_decline_sites[0]; ++i)
        if (wow32_decline_sites[i].id == id && wow32_decline_sites[i].from == from)
            return 1;
    return 0;
}

/* ---- the services ------------------------------------------------------- */
/*
 * Returns 1 if this ID was serviced (the caller then advances EIP past the BOP),
 * 0 if it is still unimplemented (the caller logs and steps over).
 *
 * ⚠ EVERY SERVICE MUST CALL wow32_setret(), even a void one. The thunk pops the
 *   return slot unconditionally, so "no return value" still means "write zero"
 *   -- otherwise the guest gets whatever was on the stack. GlobalMemoryStatus is
 *   the void case and it still writes 0.
 */
static int wow32_call(wow32_frame_t *f, wow32_dosdata_t *dd)
{
    switch (f->id) {

    /* ── 0xb8 VirtualAlloc(lpAddress, dwSize, flAllocationType, flProtect) ──
         krnl386 services DPMI 0501 ("allocate memory block") with this rather
         than passing it to the DPMI host: its call site is guarded by
         `cmp ax,0x501`, and afterwards it loads BX:CX and SI:DI from the result
         -- exactly 0501's address-and-handle return convention.
       ★ A REAL VirtualAlloc IS THE RIGHT ANSWER, not a fake. The guest runs in
         our own address space, so an address we allocate is one the guest can
         reach once it puts a descriptor over it -- which is the next thing it
         does. Handing back a plausible-looking number would fail later, further
         from the cause. */
    case WOW32_VIRTUALALLOC: {
        DWORD addr  = wow32_argd(f, 12);
        DWORD size  = wow32_argd(f, 8);
        DWORD type  = wow32_argd(f, 4);
        DWORD prot  = wow32_argd(f, 0);
        void *p = VirtualAlloc((LPVOID)(ULONG_PTR)addr, size, type, prot);
        wow32_setret(f, (DWORD)(ULONG_PTR)p);
        return 1;
    }

    /* ── 0xb9 VirtualFree(lpAddress, dwSize, dwFreeType) ─────────────────── */
    case WOW32_VIRTUALFREE: {
        DWORD addr = wow32_argd(f, 8);
        DWORD size = wow32_argd(f, 4);
        DWORD type = wow32_argd(f, 0);
        BOOL ok = addr ? VirtualFree((LPVOID)(ULONG_PTR)addr, size, type) : FALSE;
        wow32_setret(f, (DWORD)ok);
        return 1;
    }

    /* ── 0xbc GlobalMemoryStatus(LPMEMORYSTATUS) ──────────────────────────
         The one argument is a 16:16 pointer to a 32-byte buffer whose dwLength
         the guest has already set to 0x20. Fill it through the guest's own
         selector -- we must not hand back a host pointer, because the guest
         reads the fields with `mov edx,[bp+0x0c]` off its own stack copy. */
    case WOW32_GLOBALMEMORYSTATUS: {
        volatile BYTE *dst = wow32_argptr(f, 0);
        if (dst) {
            MEMORYSTATUS ms;
            unsigned k;
            const BYTE *s = (const BYTE *)&ms;
            ms.dwLength = sizeof(ms);
            GlobalMemoryStatus(&ms);
            for (k = 0; k < sizeof(ms); ++k) dst[k] = s[k];
        }
        wow32_setret(f, 0);            /* void -- but the slot is popped anyway */
        return 1;
    }

    /* ── 0xcf GetSystemDefaultLangID() ────────────────────────────────────
         Named by krnl386's export table AND confirmed by its call site, which
         compares the answer against 0x411/0x412/0x404/0x804/0x0c04 -- the
         Far-East LANGIDs, i.e. it is asking "am I on a DBCS system". Answering
         with the host's real LANGID is both correct and the whole point: a
         Japanese XP should make krnl386 take the DBCS path. */
    case WOW32_GETSYSTEMDEFAULTLANGID:
        wow32_setret(f, (DWORD)GetSystemDefaultLangID());
        return 1;

    /* ── ★★★ 0x80 GetPrivateProfileString, AND IT IS THE PROGRAM LAUNCH ───
         Neither this ID nor 0x39 is self-named by krnl386's export table, so both
         were pinned from the call sites and from the DGROUP the arguments point
         into -- which turns out to name them outright. `seg1:0xcc08`:

           push ds / push 0x158e     ; ds:0x158e = "BOOT"          lpAppName
           push ds / push 0x16b2     ; ds:0x16b2 = "WOWSHELL"      lpKeyName
           push ds / push [0x1492]   ; measured ds:0x16a6 =
                                     ;   "WOWEXEC.EXE"             lpDefault
           push ds / push 0x15f1     ; an empty 0x50-byte buffer   lpReturnedString
           push 0x50                 ;                             nSize
           push ds / push 0x1593     ; ds:0x1593 = "SYSTEM.INI"    lpFileName
           call 0xb544               ; 22 arg bytes = 4+4+4+4+2+4  ✓

         and thirty bytes later krnl386 does `mov di,0x15f1` and lcalls the
         LoadModule thunk at `seg2:0x190a`, then `cmp ax,0x20 / jbe` -- Win16's
         "failed to launch". So this call is krnl386 asking **what Win16 program
         to run**, and answering it is the launch itself.
       ★ The second site, `seg1:0xca6d`, is the same signature over
         `[DEBUG] OUTPUTTO` with an empty default, and it DOES test the result:
         `or ax,ax / je`, then `cmp ax,0x4e` -- and `0x4e` is `nSize - 2`, which is
         GetPrivateProfileString's own "the answer did not fit" convention. Two
         independent sites agreeing on six arguments and a return convention is
         what makes this a reading rather than a guess.
       ⚠ Arguments are Pascal order: FIRST pushed is the HIGHEST offset, so
         lpFileName (pushed last) is at 0, lpAppName at 18.
       ★ Answer it with the REAL Win32 call against the REAL file. On this rig
         `C:\WINDOWS\SYSTEM.INI` has no `[boot]` section at all (measured), so the
         default is what comes back -- `WOWEXEC.EXE`, which is present. That is
         the right answer for the right reason, and a box that DOES set WOWSHELL
         gets its own shell rather than ours. A bare filename resolves against the
         Windows directory, which is exactly the 16-bit convention. */
    case WOW32_GETPRIVATEPROFILESTRING: {
        char app[128], key[128], def[260], file[260], buf[512];
        volatile BYTE *dst = wow32_argptr(f, 6);
        WORD   n   = wow32_argw(f, 4);
        int    ha  = wow32_argstr(f, 18, app,  sizeof app);
        int    hk  = wow32_argstr(f, 14, key,  sizeof key);
        int    hd  = wow32_argstr(f, 10, def,  sizeof def);
        DWORD  got;
        unsigned k;
        wow32_argstr(f, 0, file, sizeof file);
        if (n > sizeof buf) n = sizeof buf;
        got = GetPrivateProfileStringA(ha ? app : NULL, hk ? key : NULL,
                                       hd ? def : "", buf, n, file);
        if (dst) for (k = 0; k <= got && k < n; ++k) dst[k] = (BYTE)buf[k];
        wow32_setret(f, got);
        return 1;
    }

    /* ── 0x39 GetProfileInt(lpAppName, lpKeyName, nDefault) ───────────────
         10 arg bytes = 4 + 4 + 2, no filename -- so it is the SYSTEM profile
         twin of 0x80 rather than a private one. Both its call sites read as
         that, and both name themselves out of DGROUP:
           seg1:0xca4f  ("KERNEL", "GPCONTINUE", [0x53c])
           seg2:0x09bc  ("ModuleCompatibility", <the module's own name>, 0)
         -- one per module just loaded, which is exactly what that section is for.
       ⚠ WHICH FILE IS UNPROVEN. Win16 `GetProfileInt` means WIN.INI, and
         `[ModuleCompatibility]` is conventionally a SYSTEM.INI section; the two
         readings are not distinguishable from krnl386's side because the call
         carries no filename. It is recorded rather than hidden, and it costs
         nothing today: this rig's SYSTEM.INI and WIN.INI have NEITHER section
         (measured), so both readings return the caller's default. Revisit if a
         module ever needs a compatibility flag. */
    case WOW32_GETPROFILEINT: {
        char app[128], key[128];
        int ha = wow32_argstr(f, 6, app, sizeof app);
        int hk = wow32_argstr(f, 2, key, sizeof key);
        WORD def = wow32_argw(f, 0);
        wow32_setret(f, (DWORD)GetProfileIntA(ha ? app : "", hk ? key : "", (INT)def));
        return 1;
    }

    /* ── 0x88 GetDriveType(nDrive) ────────────────────────────────────────
         Named by krnl386's export table, and its ONE caller pins the semantics
         to the byte: `push di / call 0xb4b5 / pop dx / cmp al,2` at seg1:0x1ea7
         -- i.e. "is drive nDrive REMOVABLE", and 2 is Win32's DRIVE_REMOVABLE.
         So this is a straight pass-through of the Win32 call, not a WOW-private
         encoding, and the host's answer is the guest's answer: our DOS layer
         opens real paths on the real filesystem, so its drives ARE these drives.
       ★ MEASURED, not assumed: krnl386 calls it 26 times in a row with nDrive
         0x00..0x19 -- A: through Z: -- which is what fixes 0 = A: rather than
         0 = "the default drive". Every one of those was previously stepped over
         and answered with the harness sentinel, and a drive table built from 26
         identical answers is what fed 0xf0 to the GetCurrentDirectory that
         followed and #GP'd the run.
       ⚠ GetDriveTypeA wants a ROOT PATH ("A:\"), not a letter. */
    case WOW32_GETDRIVETYPE: {
        WORD n = wow32_argw(f, 0);
        char root[4];
        if (n > 25) { wow32_setret(f, 1 /* DRIVE_NO_ROOT_DIR */); return 1; }
        root[0] = (char)('A' + n); root[1] = ':'; root[2] = '\\'; root[3] = 0;
        wow32_setret(f, (DWORD)GetDriveTypeA(root));
        return 1;
    }

    /* ── ★ 0x7d: approve the selector about to become a TASK DATABASE ──────
         Not named by the export table, so it comes from its call sites -- both
         of which are inside ONE function, `seg2:0x2984`, and that function is
         the TDB creator:

           2984  enter 4,0                      ; the whole of it
           29b5  mov si,0x100                   ; room for a PSP
           29d5  add si,0x223 / and si,0xfff0   ; -> 0x320
           29e6  lcall seg1:0x4e81              ; GlobalAlloc(that many bytes)
           29f6  push ax / lcall 0xb397         ; 0x7d: "is THIS one acceptable?"
           29fc  or ax,ax / je 0x2a04           ; no -> the retry loop
           2c02  mov word ptr [0xfa],0x4454     ; "TD", the TDB signature

         and `0x320` is exactly limit+1 of every task database in the stock
         panel (session-37), including the two of a live stock WOW session.

       ★ THE RETRY LOOP IS WHAT PINS THE SEMANTICS. On a `0` the caller does not
         go and allocate different memory -- it calls `seg1:0x574d`, which is
         AllocSelector: `lsl ecx,<sel>` for the limit, take LDT entries, copy the
         source DESCRIPTOR onto them, `or si,7`. Every retry therefore offers an
         ALIAS OF THE SAME BYTES, and the only thing that differs between one
         attempt and the next is the NUMERIC VALUE OF THE SELECTOR. The question
         can only be "may this selector value be a task handle?", and the rejects
         are freed again (`seg1:0x5a53`) as soon as one is accepted.

       ★ AND THE ANSWER IS THE SELECTOR, NOT A BOOLEAN. `seg2:0x29fe` merely
         tests it, but `seg2:0x2a22` does `mov si,ax` and si goes on to BE the
         TDB selector (`mov es,ax`, then the block is zeroed through it). A
         32-bit companion cannot conjure an LDT selector, so the only value it
         can return is one it was offered: ECHO THE ARGUMENT. A bare `1` -- what
         the `wow32ret.txt` experiment answered to get WOWEXEC.EXE running -- is
         right only because the first call is accepted and the second path is
         never taken; it would install `0x0001` as a task's selector if it were.

       ★ ACCEPTING IS THE RIGHT ANSWER FOR THIS HOST, and that is a reading, not
         a shrug: whatever the real 32-bit side checks against, we keep no
         parallel handle table, the LDT krnl386 allocates from is the one we gave
         it, and each TDB gets a distinct selector by construction. Nothing here
         could make one selector unacceptable and the next one acceptable.
       ⚠ If a task ever does need rejecting, this is where it goes -- and the
         guest's loop is a trap. `seg2:0x2a14` pushes and does not pop on the
         loop-back edge, so every rejection leaks two bytes of krnl386's stack;
         1884 of them is the #SS that led here in the first place. */
    case WOW32_ACCEPTTASKSELECTOR:
        wow32_setret(f, (DWORD)wow32_argw(f, 0));
        return 1;

    /* ── 0x78: krnl386 hands us its view of the DOS data area ─────────────
         The argument is a 16:16 pointer to the structure whose address it read
         from SysVars+0x6A moments earlier (seg1:0xc05b), which is also where it
         got the six far pointers into DOS's data that it caches at [0x271]
         through [0x285].
       ★ RECORD IT AND SUCCEED -- and that is not a stub dressed up. On real
         Windows the 32-bit side wants this because ntvdm owns that memory from
         another module; here the HOST already owns it, because the host is what
         planted SysVars and that table in the first place (see DOS_WOW_* in
         dos_layout.h). There is nothing to look up. What matters is that the
         table krnl386 already read was valid, and that is the host's job, not
         this call's. Logged so the two can be compared. */
    case WOW32_REGISTERDOSDATA:
        if (dd) { dd->seen = 1; dd->farptr = wow32_argd(f, 0); }
        wow32_setret(f, 0);
        return 1;

    /* ── The INT 21h file family: decline, and let our own DOS layer serve it.
         See the WOW32_DECLINE block above for why this is an answer rather than
         a stub, and for the one behavioural difference it buys.
       ⚠ ONLY AT A SITE THAT CHAINS. The same IDs are called from places where
         0xFFFF is returned to the app as a failure -- see wow32_may_decline. At
         one of those, fall through to "unimplemented", which is honest, and which
         the log distinguishes. */
    default:
        if (wow32_may_decline(f->id, f->from)) {
            wow32_setret(f, WOW32_DECLINE);
            return 1;
        }
        return 0;
    }
}

#endif /* WOW32_H */
