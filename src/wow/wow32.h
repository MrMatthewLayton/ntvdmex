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

/* ── ★★★ THE SECOND RETURN CHANNEL: THE EPILOGUE MODE. (GH #128, session 38) ──
     The thunk does not have one return path, it has THIRTY-EIGHT, and which one
     it takes is a word on the guest stack that the 32-bit side is expected to
     write. Reading up from SP, the prologue at seg1:0x2bb6 lays down

       bp-2 bx | -4 es | -6 cx | -8 fs | -10 gs | -12 ds | -16 the RETURN HOLE
       | -18 si | -20 di | -22 bp | -24 ★ THE MODE (`push 0`) | -26 [0x228]

     and afterwards:

       2c04  pop ax / cmp ax,[0x228] / jne   ; the re-entrancy guard (NOT a lever)
       2c0b  pop bx                          ; ★ THE MODE
       2c0d  cmp bx,0 / jne 0x2c32           ; 0 = the ordinary epilogue
       2c3f  add bx,bx
       2c41  jmp word ptr cs:[bx+0x2a36]     ; ★ a 38-entry table of epilogues

   ★ krnl386 ITSELF NEVER SETS IT. `seg1:0x2bc7 push 0` is the only writer of the
     slot other than `seg1:0x2c5c`, which CLEARS it -- checked by scanning every
     segment for a store to `[bp-0x18]`. Thirty-seven epilogues that the guest
     can never select are not dead code; they are a menu for the other side.
   ★★ MODE 25 IS THE TASK SWITCH-BACK, and it is a matched pair with the task
     launcher. `seg1:0x97be` starts a task with

       97be  push [0x228] / push bp      ; on the CREATOR's stack
       97c3  mov di,ss / mov cx,sp       ; its stack, kept in registers
       97e9  mov ss,[bp+8] / mov sp,si   ; switch to the new task
       9822  jmp 0xb1d0                  ; WOW32 0x74, through the thunk
                                         ;   -- which pushes DI and CX into its
                                         ;      frame, on the NEW task's stack

     and mode 25 lands at `seg1:0x2c4e`, which pops that frame (so DI and CX come
     back) and jumps to `seg1:0x9827`:

       9827  mov ss,di / mov sp,cx / pop bp / pop [0x228]

     -- the creator back on its own stack, with its BP and its current-task word
     restored. So "this task's turn is over, put its creator back" is one word.
   ⚠ IT IS ONLY VALID AT THE FRAME THAT STARTED THE TASK. DI and CX are a stack
     only in the `0x74` call; at any other call site they are just the caller's
     registers, and mode 25 would load SS:SP from whatever they happened to hold.
   ★ AND THE HOST NOW USES IT. src/wow/wowsched.h returns the `0x74` launch call
     through mode 25, which is what lets krnl386's creating task carry on and
     `LoadModule` finish. Opt-in; see that file for the two moments and the one
     ordering that works. */
#define WOW32_OFF_MODE  (-24)
#define WOW32_MODE_ORDINARY   0
#define WOW32_MODE_SWITCHBACK 25

/* The BOP is `C4 C4 51`. Resuming the guest anywhere but past all three bytes
   restarts it mid-instruction. */
#define WOW32_BOP_LEN   3

typedef DWORD (*wow32_sel2lin_fn)(WORD sel, void *ctx);

typedef struct {
    volatile BYTE   *bp;             /* linear address of SS:BP inside the thunk */
    WORD             id;
    WORD             argb;
    WORD             from;           /* return address in krnl386 seg 1 -- the CALL SITE */
    WORD             stubseg;        /* [bp+4]: the SEGMENT of the per-function stub    */
    wow32_sel2lin_fn sel2lin;        /* selector -> linear base (host's LDT view) */
    void            *ctx;
    /* ── ★★★ THE ID SPACE IS PER MODULE, AND THIS SAYS WHOSE. (session 38) ──────
         Every id in this file was read out of krnl386's thunk table. USER, GDI and
         the drivers have their OWN tables, with their own numbering, reaching the
         same BOP -- so an id is only meaningful together with the table it came
         through, and the table is named by `stubseg`.
       ⚠ MEASURED, AFTER GETTING IT WRONG. WOWEXEC's `RegisterClass(&WNDCLASS)`
         arrives as id `0x39` with `retstub=0x0c25` and **4** argument bytes, from
         USER's segment. krnl386's `0x39` is `GetProfileInt`, `retstub=0xb537`, **10**
         argument bytes. We serviced the first with `GetProfileIntA` and handed
         WOWEXEC the answer -- a function answered by an unrelated function, which is
         the "runs but lies" class this project treats as the most expensive kind.
       ⇒ 1 only when the stub lives in the segment the BOP is executing in, which is
         krnl386's seg1 -- the table this file describes. krnl386 has a SECOND table
         of its own in seg2 (121 stubs, far-calling the same thunk) whose numbering is
         also not this one, so "not ours" is about the TABLE, not about the module.
         Everything here is gated on it; anything else gets the honest
         "unimplemented", which is a missing answer instead of a wrong one. */
    int              krnl;
    /* Filled in by the host so a service can talk back about what it did. */
    DWORD            ret;
    int              serviced;
    /* ── ★★★ A 16-BIT CALL THE SERVICE WANTS MADE. (GH #128, session 40) ────────
         A service cannot make one itself: entering guest code means replacing the
         whole guest context, which only the BOP handler is in a position to do
         and undo. So a service ASKS, by filling these in, and the handler acts on
         the request after the service has returned and its answer is already in
         the return hole. See src/wow/wowcall.h.
       ⚠ `cbok` is the host's permission, not the service's opinion: the machinery
         is opt-in (wowcall.txt), and a service that requested a callback the host
         will not make must not then describe one in its log note. */
    int              cbok;           /* 1 = the host can call 16-bit code now   */
    DWORD            cbproc;         /* 16:16 procedure to call; 0 = none asked */
    WORD             cbds;           /* the DS it must be entered with          */
    WORD             cbarg[6];       /* words to push, in DECLARED order        */
    int              cbnarg;
    int              cbret;          /* WOWCALL_RET_KEEP / _RESULT -- whose
                                        answer the caller's return value is    */
    WORD            *cbsink;         /* optional: where the host keeps the answer */
    WORD             cbhwnd, cbmsg;  /* for the log; 0/0 when not a message     */
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

/* ---- writing back through a far pointer the guest gave us --------------- */

/* Resolve a 16:16 far pointer stored INSIDE a guest structure (rather than in
   the argument block) to a host address. Same null-selector rule as
   wow32_argptr: 0 rather than the LDT base, so a missing check cannot scribble
   at the bottom of the address space. */
static volatile BYTE *wow32_farat(const wow32_frame_t *f, volatile BYTE *base, int off)
{
    DWORD fp  = (DWORD)wow32_peekw(base + off)
              | ((DWORD)wow32_peekw(base + off + 2) << 16);
    WORD  sel = (WORD)(fp >> 16);
    DWORD lin;
    if (!sel || !f->sel2lin) return NULL;
    lin = f->sel2lin(sel, f->ctx);
    if (!lin) return NULL;
    return (volatile BYTE *)(ULONG_PTR)(lin + (fp & 0xFFFF));
}

/* Copy a host string into a guest buffer described by a POINTER/CAPACITY PAIR,
   and write the length actually stored back over the capacity.
   ⚠ THE CAPACITY IS THE GUEST'S CLAIM ABOUT ITS OWN STACK, and it is the only
     bound there is -- these buffers sit inside the caller's frame, a few bytes
     below its return address, so overrunning one does not corrupt data, it
     corrupts control flow. Never write more than the guest declared.
   Returns the length written, or -1 if there was nowhere to write. */
static int wow32_farput(const wow32_frame_t *f, volatile BYTE *base,
                        int lpoff, int cboff, const char *s)
{
    volatile BYTE *d = wow32_farat(f, base, lpoff);
    WORD cap = wow32_peekw(base + cboff);
    int k = 0;
    if (!d || !cap) return -1;
    while (s[k] && k < (int)cap - 1) { d[k] = (BYTE)s[k]; ++k; }
    d[k] = 0;
    wow32_pokew(base + cboff, (WORD)k);
    return k;
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
/* ── ★★★ 0xc5: RESOLVE A MODULE NAME TO A FULL PATH. (session 39) ─────────────
     Serviced in main.c, not here: the answer is a 16:16 far pointer, so it needs
     guest-visible memory and a selector, and both live over there.
   ★ NAMED BY ITS TWO CALL SITES, and the second is what makes it unambiguous:
       seg2:0x0856   0xc5(dst, src)    -- resolve; `cmp ax,0 / je` picks the tail
       seg2:0x08ba   0xc5(dst, NULL)   -- release, gated on the first having
                                          succeeded, and its result ignored
     so the pair is resolve/release and the host owns the storage between them.
   ★★ AND `dst` RECEIVES A FAR POINTER, NOT A COPIED STRING -- proved by symmetry
     rather than by reading the pushes. The success tail (seg2:0x0864) and the
     fallback tail (0x0875) are THE SAME five-word call, with `[bp-0x1c]:[bp-0x1e]`
     substituted for the caller's own `[bp+0x0c]:[bp+0x0a]`. One is the resolved
     path, the other is the name we were given; they must be the same kind of
     thing.
   ⇒ Answering 0 is what made krnl386 compose module names against the CURRENT
     DIRECTORY and fail to open `C:\Documents and Settings\<user>\SHELL.DLL`. */
#define WOW32_RESOLVEMODULEPATH         0xc5
/* ── ★★ 0xd0: GetWindowsDirectory(lpBuffer, uSize). (session 40) ──────────────
     Not named by krnl386's export table, so it comes from its two call sites and
     from what the answer is USED for -- and the two agree.

   ★ krnl386's own (seg1:0xc917) says what SHAPE it is, to the byte:
       c90d  mov di,0x624
       c910  push ds / push di / push 0x80     ; (lpBuffer = ds:0x624, uSize=128)
       c917  call 0xb3e5                       ; this id, 6 argument bytes
       c91a  or ax,ax / je                     ; 0 is failure
       c920  repne scasb ...                   ; measure the string it wrote
       c92a  mov [0x506],ds / [0x504],0x624 / [0x50c],cx   ; cache ptr + LENGTH
     So it fills the caller's buffer with a NUL-terminated path and returns
     non-zero on success. That is a Get<something>Directory and nothing else.

   ★ SYSEDIT says WHICH directory, and it is a count rather than a guess:
     `sysedit` imports `KERNEL.134 GETWINDOWSDIRECTORY` and NOT
     `KERNEL.135 GETSYSTEMDIRECTORY` (`neimports.py`: seg2:0x017c and
     seg2:0x01c6 -- exactly two sites), and in a run SYSEDIT's task makes
     exactly TWO calls to this id. It then `lstrcat`s `\SYSTEM.INI` and
     `\WIN.INI` onto the answer, which is where those files live.
   ⚠ WHAT LEAVING IT UNIMPLEMENTED LOOKED LIKE: not an error, but a WRONG NAME.
     The buffer kept whatever was in it, so SYSEDIT opened -- and titled a
     window -- `"REGISTERPENAPP\SYSTEM.INI"`, a path built from another module's
     leftover string. The "runs but lies" class, and it took the callback work to
     get far enough to see it. */
#define WOW32_GETWINDOWSDIRECTORY       0xd0
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
    case WOW32_RESOLVEMODULEPATH:      return "ResolveModulePath";
    case WOW32_WOWSHUTDOWNTIMER:       return "WowShutdownTimer";
    case WOW32_GETCURDIR:              return "GetCurrentDirectory";
    case WOW32_GETSYSTEMDEFAULTLANGID: return "GetSystemDefaultLangID";
    case WOW32_GETWINDOWSDIRECTORY:    return "GetWindowsDirectory";
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

/* ── THE WIN16 PROGRAM THIS VDM EXISTS TO RUN ─────────────────────────────────
     Filled in by the host once it knows what it was launched for, and handed to
     the guest by WOW32 0x70 (WowGetNextVDMCommand) -- see that case for the
     structure and for why this is the call that launches an application.
   ⚠ A WOW LAUNCH DOES NOT CARRY THE PROGRAM ON ITS COMMAND LINE. Windows starts
     the VDM as `ntvdm -f -i1 -w -a <krnl386>` and the application is delivered
     out of band; real ntvdm gets it from the Win32 GetNextVDMCommand, which
     returns FALSE/0x57 for us (measured -- see docs/research/). On the rig it
     comes from target.txt, which is the harness's channel for the same fact.
   ★ EMPTY IS A LEGITIMATE STATE and it has a correct answer: "no command", which
     is NOT the same as an error. See the 0x70 case. */
static char g_wow_cmd_prog[512] = { 0 };   /* full path of the Win16 program   */
static char g_wow_cmd_args[192] = { 0 };   /* its arguments, without a leading space */
static int  g_wow_cmd_taken     = 0;       /* delivered already -- deliver once */

/* Field offsets of the command structure -- derived in the 0x70 case, which is
   the only place they are used and the only place the derivation makes sense. */
#define WOWCMD_LPCMDLINE   0x00
#define WOWCMD_LPAPPNAME   0x04
#define WOWCMD_LPENV       0x08
#define WOWCMD_CBCMDLINE   0x10
#define WOWCMD_CBAPPNAME   0x12
#define WOWCMD_CBENV       0x14
#define WOWCMD_CURDRIVE    0x16
#define WOWCMD_LPBUFC      0x18
#define WOWCMD_CBBUFC      0x1c
#define WOWCMD_NCMDSHOW    0x1e

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
    /* ★ NOT OUR ID SPACE, NOT OUR ANSWER. See `krnl` in wow32_frame_t. */
    if (!f->krnl) return 0;
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

    /* ── ★★ 0xd0 GetWindowsDirectory(lpBuffer, uSize) -- see the note above.
         The real Win32 call against the real directory, for the same reason
         GetDriveType is a pass-through: our DOS layer opens real paths on the
         real filesystem, so the host's Windows directory IS the guest's.
       ⚠ uSize is the GUEST'S claim about its own buffer and the only bound
         there is -- krnl386's is 0x80 bytes inside its DGROUP. Never write more
         than it declared. */
    case WOW32_GETWINDOWSDIRECTORY: {
        volatile BYTE *dst = wow32_argptr(f, 2);
        WORD cap = wow32_argw(f, 0);
        char dir[MAX_PATH];
        UINT n;
        if (!dst || !cap) { wow32_setret(f, 0); return 1; }
        n = GetWindowsDirectoryA(dir, sizeof dir);
        if (!n || n >= sizeof dir || n + 1 > (UINT)cap) { wow32_setret(f, 0); return 1; }
        { UINT k; for (k = 0; k <= n; ++k) dst[k] = (BYTE)dir[k]; }
        wow32_setret(f, n);
        return 1;
    }

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
        DWORD  got;
        unsigned k;
        /* ⚠ Same trap as 0x39 next door: `hd ? def : ""` would hand a READ-ONLY
             literal to a call that writes to its arguments. NULL is documented and
             safe for the two names; the default has to be a writable buffer. */
        if (!wow32_argstr(f, 10, def, sizeof def)) def[0] = 0;
        wow32_argstr(f, 0, file, sizeof file);
        if (n > sizeof buf) n = sizeof buf;
        got = GetPrivateProfileStringA(ha ? app : NULL, hk ? key : NULL,
                                       def, buf, n, file);
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
    /* ⚠⚠ NEVER HAND A STRING LITERAL TO THESE. (session 38) This read
           `GetProfileIntA(ha ? app : "", hk ? key : "", def)`, and the `""` is in
           .rdata -- a READ-ONLY page. XP's profile code writes to the name buffers
           it is given, so the moment krnl386 asked with an empty section or key the
           host died: `0xc0000005` in ntdll at `mov [ecx+eax],bl`, with EAX holding
           the literal's own address. It survived ten calls in every earlier run
           because every one of them had both names non-empty; the task scheduler
           simply let the guest get as far as asking with one missing.
         ⇒ The locals are writable and already the right size, so pass them always
           and make "absent" an empty *buffer* rather than an empty literal. */
    case WOW32_GETPROFILEINT: {
        char app[128], key[128];
        WORD def;
        if (!wow32_argstr(f, 6, app, sizeof app)) app[0] = 0;
        if (!wow32_argstr(f, 2, key, sizeof key)) key[0] = 0;
        def = wow32_argw(f, 0);
        wow32_setret(f, (DWORD)GetProfileIntA(app, key, (INT)def));
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

    /* ── ★★★★ 0x70 WowGetNextVDMCommand -- "WHICH 16-BIT PROGRAM DO I RUN?" ────
         This is the call that launches a Win16 application, and answering it is
         the difference between a WOW bootstrap and a running program.

       ★ HOW IT WAS FOUND. Not by working down a list: the run said so. WOWEXEC
         asks this once, we answered the harness sentinel `0`, and the VERY NEXT
         call was `WowMsgBox("Can't run 16-bit Windows program", "Insufficient
         memory to run this application...")`. The program it wanted is the one
         the host was launched for -- on the rig, `SYSEDIT.EXE`.

       ★ AND `0` IS A HARD ERROR, NOT "NOTHING TO DO". The caller distinguishes:
             ret == 0                  -> error box, `wowexec seg1:0x0bf8`
             ret != 0, cbCmdLine == 0  -> quiet cleanup, `seg1:0x0c1a / je 0x0c05`
         So the sentinel was making WOWEXEC report a failure that had not
         happened. "No command" is `1` with a zero length, and it is silent.

       ── THE STRUCTURE, READ OFF WOWEXEC'S OWN FRAME (seg1:0x0b20 onward) ──────
         The one argument is a 16:16 pointer to 0x20 bytes at `ss:bp-0x34a`, and
         every field below is either written by the caller before the call or read
         by it after -- there is no field here that was not observed being used.

           +0x00 DWORD lpCmdLine   -> a 0x10d-byte buffer at bp-0x10e
           +0x04 DWORD lpAppName   -> a 0x10d-byte buffer at bp-0x21c
           +0x08 DWORD lpEnv       -> GlobalAlloc(2, cbEnv*2), then GlobalLock
           +0x0c WORD  (caller zeroes; never read back)
           +0x0e WORD  (caller zeroes; never read back)
           +0x10 WORD  cbCmdLine   in 0x10d -- ★ OUT MUST BE NON-ZERO OR NO LAUNCH
           +0x12 WORD  cbAppName   in 0x10d
           +0x14 WORD  cbEnv       in 0x1000 -- in/out, see the retry note below
           +0x16 WORD  CurDrive    0-based: the caller does `add al,0x41`
           +0x18 DWORD lpBufC      -> a third 0x10d-byte buffer at bp-0x32a
           +0x1c WORD  cbBufC      in 0x10d
           +0x1e WORD  nCmdShow    -> becomes lpCmdShow[1] of the LOADPARMS

       ★ +0x04 IS THE MODULE NAME, AND THAT IS MEASURED, NOT ASSUMED. The success
         path reaches `seg1:0x01c0`:
             push [bx+6] / push [bx+4]      ; the far pointer at +0x04
             lea ax,[bp-0x10] / push ss / push ax
             lcall  KERNEL.LoadModule       ; named from the relocation chain
         -- Win16 `LoadModule(lpModuleName, lpParameterBlock)`, 8 argument bytes.
         The parameter block it builds beside it is the standard LOADPARMS, and it
         is what identifies the other fields: `wEnvSeg` comes from +0x0a (the
         SEGMENT half of lpEnv), and `nCmdShow` from +0x1e.

       ⚠⚠ THE COMMAND LINE IS A PASCAL TAIL, AND THE `-2` IS THE WHOLE PUZZLE.
         `seg1:0x0173` does `lstrlen(lpCmdLine)`, then `sub al,2`, and stores THAT
         as the count byte of the DOS command tail it hands to LoadModule -- then
         `lstrcpy`s our buffer to the byte AFTER it. So the delivered string must
         be exactly two bytes longer than the tail text it represents, and the
         only shape that makes every case come out right is
                            <tail text> CR LF
         Check it: with no arguments the text is empty, we deliver "\r\n",
         `lstrlen` is 2, the count byte is 0, and the tail reads <0><CR><LF> --
         a correct empty tail. With text " FOO" we deliver " FOO\r\n", the count
         is 4, and the tail is <4>' ''F''O''O'<CR><LF>. Both the count and the
         terminator land where DOS expects them.
       ⚠ Deliver an EMPTY string and `0 - 2` makes the count byte 0xFE, and the
         program reads 254 bytes of somebody's stack as its arguments. The two
         trailing bytes are load-bearing.

       ⚠ cbEnv (+0x14) IS DELIBERATELY NOT WRITTEN. It is an in/out "the buffer
         was too small" field -- the caller saves it, and if the callee returns a
         LARGER value it frees the block, reallocates and asks again
         (`seg1:0x0be4`, `jae`). Leaving it alone is the only value that cannot
         start that loop. The environment itself is built by the caller from its
         own PSP (`seg1:0x02da`, GetCurrentTask -> TDB -> PSP+0x2c); we write a
         valid empty block so the buffer is never uninitialised, and no more.

       ★ DELIVER ONCE. The caller loops on this while `[0x18]` is set
         (`seg1:0x0791`), so a host that answered every time would relaunch the
         program forever. `[0x18]` is only set when WowRegisterShellWindowHandle
         succeeds, which it does not yet -- so this guard is not load-bearing
         today, and it is here because the day it becomes load-bearing the symptom
         is a fork bomb inside the VDM rather than a wrong answer in a log. */
    case WOW32_WOWGETNEXTVDMCOMMAND: {
        volatile BYTE *ci = wow32_argptr(f, 0);
        const char *prog = g_wow_cmd_prog;
        char tail[192];
        int n;

        /* An unreachable structure is the one case that really is a hard error:
           there is nowhere to put the answer, so `0` is the truth. */
        if (!ci) { wow32_setret(f, 0); return 1; }

        if (!prog[0] || g_wow_cmd_taken) {         /* nothing (more) to run */
            wow32_pokew(ci + WOWCMD_CBCMDLINE, 0);
            wow32_setret(f, 1);
            return 1;
        }

        /* The tail: <text> CR LF, per the note above. DOS tails conventionally
           begin with the separating space, so the text is " " + args. */
        n = 0;
        if (g_wow_cmd_args[0]) {
            int j;
            tail[n++] = ' ';
            for (j = 0; g_wow_cmd_args[j] && n < (int)sizeof tail - 3; ++j)
                tail[n++] = g_wow_cmd_args[j];
        }
        tail[n++] = '\r'; tail[n++] = '\n'; tail[n] = 0;

        n = wow32_farput(f, ci, WOWCMD_LPAPPNAME, WOWCMD_CBAPPNAME, prog);
        if (n <= 0) { wow32_setret(f, 0); return 1; }   /* no name, no launch */
        if (wow32_farput(f, ci, WOWCMD_LPCMDLINE, WOWCMD_CBCMDLINE, tail) <= 0) {
            wow32_setret(f, 0); return 1;
        }
        /* A valid empty environment, so the caller never walks uninitialised
           stack looking for its double NUL. */
        { volatile BYTE *e = wow32_farat(f, ci, WOWCMD_LPENV);
          if (e) { e[0] = 0; e[1] = 0; } }
        /* The third buffer is uninitialised stack in the caller's frame and it is
           passed on unconditionally -- terminate it rather than let it travel. */
        { volatile BYTE *c = wow32_farat(f, ci, WOWCMD_LPBUFC);
          if (c) c[0] = 0; }
        wow32_pokew(ci + WOWCMD_CBBUFC, 0);

        /* Drive letter of the program's own path, 0-based -- the caller turns it
           back into a letter with `add al,0x41`. Default to C: when the path is
           not drive-qualified, because there is no "unknown" in a byte. */
        { char d = (prog[0] && prog[1] == ':') ? prog[0] : 'C';
          if (d >= 'a' && d <= 'z') d = (char)(d - 32);
          wow32_pokew(ci + WOWCMD_CURDRIVE, (WORD)(d - 'A')); }
        wow32_pokew(ci + WOWCMD_NCMDSHOW, 1);          /* SW_SHOWNORMAL */

        g_wow_cmd_taken = 1;
        wow32_setret(f, 1);
        return 1;
    }

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
