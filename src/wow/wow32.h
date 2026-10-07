/* wow32.h -- the 32-bit half of WOW: krnl386's calls out to Win32.  GH #128.
 *
 * krnl386.exe is a 16-bit DLL that cannot call Win32, so it reaches a 32-bit
 * companion (real Windows: wow32.dll inside ntvdm.exe) through a native BOP.
 * Every call arrives at the same BOP with a function ID on the stack, so the whole
 * interface is a small integer namespace -- 82 function IDs, enumerated in
 * docs/research/wow32-call-surface.md.
 *
 * ─────────────────────────────────────────────────────────────────────────────
 * THE FRAME, AS MEASURED AT THE BOP ON THE LIVE RIG (`@ss:sp` dumps).
 *
 * Relative to the BP the 16-bit side has set up when the BOP executes:
 *
 *     [bp+0]      saved BP
 *     [bp+2/+4]   a near return and a CS -- the per-function stub's
 *     [bp+6]      ★ THE FUNCTION ID
 *     [bp+8]      a zero word
 *     [bp+10]     ★ the ARGUMENT BYTE COUNT
 *     [bp+12/+14] the CALLER's return address -- offset then CS
 *     [bp+16...]  ★ THE ARGUMENTS
 *
 * ⚠ ARGUMENTS START AT bp+16, NOT bp+12. The first cut of the host's trace read
 *   them at bp+12 and so printed the caller's far return address as the first two
 *   argument words -- which is exactly why session 30 recorded VirtualAlloc's
 *   argument ORDER as "not pinned down, two readings possible". It was an
 *   instrument that lied, in this project's usual shape. The check that it is +16:
 *   the call returns to the address at bp+12/+14 with exactly the declared
 *   argument byte count removed from the stack, so the arguments are the N bytes
 *   above the far return address -- and with +16 every known API's arguments
 *   decode to sensible values (see ARGUMENT ORDER below).
 *
 * ★ AND THE RETURN VALUE IS NOT A REGISTER. Whatever we leave in AX/DX is
 *   overwritten when the guest resumes; the DWORD the caller receives is the one
 *   in a four-byte stack slot at [bp-16] (low word) and [bp-14] (high). Confirmed
 *   on hardware: in the rig's `@ss:sp` dump those two words held stale stack
 *   (0x0047, 0x0000) at the BOP -- an uninitialised return slot.
 *   Getting this wrong is silent: the guest reads garbage and blames itself.
 *
 * ─────────────────────────────────────────────────────────────────────────────
 * ARGUMENT ORDER IS PASCAL: pushed LEFT TO RIGHT, so the FIRST declared argument
 * is at the HIGHEST address and the LAST is at bp+16. Three independent calls,
 * as they arrive at run time, agree, which is what makes it a fact:
 *   VirtualAlloc   arrives with 0, size, 0x3000, 0x40 -> (lpAddress, dwSize,
 *                  flAllocationType, flProtect), and 0x3000/0x40 are exactly
 *                  MEM_COMMIT|MEM_RESERVE and PAGE_EXECUTE_READWRITE.
 *   VirtualFree    arrives with addr, size, 0x8000    -> (lpAddress, dwSize,
 *                  dwFreeType) with MEM_RELEASE.
 *   GlobalMemoryStatus arrives with a far pointer to a 32-byte stack buffer whose
 *                  first DWORD is 0x20 -- a MEMORYSTATUS with dwLength filled in.
 *
 * A far pointer argument is a normal 16:16: offset in the low word, SELECTOR in
 * the high word (the caller pushes the segment first, the offset second).
 *
 * ─────────────────────────────────────────────────────────────────────────────
 * NAMING. 28 of the 82 IDs are named by krnl386's OWN export table -- an entry
 * whose target IS a stub, so the export's name in the (non-)resident name table
 * is the function's name, with no inference at all. `tools/ne/wowmap.py` prints
 * the mapping. It was cross-checked before being trusted: id 0xcf (no arguments,
 * and the guest's behaviour changes with the LANGID returned) was guessed
 * independently, and the export table then said GETSYSTEMDEFAULTLANGID. Two
 * methods, one answer.
 */
#ifndef WOW32_H
#define WOW32_H

#include <windows.h>
/* For DOS_CURRENT_DRIVE -- the WOW32 select-drive thunk and INT 21h AH=19h
   must answer the same thing; see the note on the constant. */
#include "dos_layout.h"

/* Where each field sits relative to the thunk's BP. See the frame diagram above. */
#define WOW32_OFF_ID    6
#define WOW32_OFF_ARGB  10
#define WOW32_OFF_FROM  12           /* return address into krnl386 -- WHICH call site */
#define WOW32_OFF_ARGS  16
#define WOW32_OFF_RET   (-16)        /* the return slot: low word, then high */

/* ── ★★★ THE SECOND RETURN CHANNEL: THE EPILOGUE MODE. (GH #128, session 38) ──
     The return path the 16-bit side takes after the BOP is selected by a word on
     the guest stack, at [bp-24], which the 32-bit side is expected to write. The
     guest always arrives with it ZERO, and zero is the ordinary return. The
     frame below BP at the BOP, as dumped on the rig:

       bp-2 bx | -4 es | -6 cx | -8 fs | -10 gs | -12 ds | -16 the RETURN SLOT
       | -18 si | -20 di | -22 bp | -24 ★ THE MODE (0 on arrival)

   ★★ MODE 25 IS THE TASK SWITCH-BACK, and it is a matched pair with the task
     launch call `0x74`. That call is made on the NEW task's stack, with the
     creating task's SS:SP carried in DI and CX (both saved in the frame above).
     Returned through mode 25, the creator comes back on its own stack, with its
     BP and its current-task word restored -- observed on the rig as krnl386's
     creating task carrying on. So "this task's turn is over, put its creator
     back" is one word.
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
    WORD             from;           /* caller's return offset [bp+12] -- the CALL SITE */
    WORD             stubseg;        /* [bp+4]: the SEGMENT of the per-function stub    */
    wow32_sel2lin_fn sel2lin;        /* selector -> linear base (host's LDT view) */
    void            *ctx;
    /* ── ★★★ THE ID SPACE IS PER MODULE, AND THIS SAYS WHOSE. (session 38) ──────
         Every id in this file is one krnl386 sends. USER, GDI and the drivers send
         their OWN ids, with their own numbering, to the same BOP -- so an id is
         only meaningful together with the module it came from, and the module is
         named by `stubseg`.
       ⚠ MEASURED, AFTER GETTING IT WRONG. WOWEXEC's `RegisterClass(&WNDCLASS)`
         arrives as id `0x39` with `retstub=0x0c25` and **4** argument bytes, from
         USER's segment. krnl386's `0x39` is `GetProfileInt`, `retstub=0xb537`, **10**
         argument bytes. We serviced the first with `GetProfileIntA` and handed
         WOWEXEC the answer -- a function answered by an unrelated function, which is
         the "runs but lies" class this project treats as the most expensive kind.
       ⇒ 1 only when the stub's segment ([bp+4]) is the segment the BOP is executing
         in -- krnl386's first code segment, the id space this file describes.
         krnl386 also reaches the BOP from a SECOND code segment of its own, with a
         numbering that is also not this one, so "not ours" is about the SEGMENT,
         not about the module.
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
    int              cbact;          /* WOWCALL_ACT_* -- what to DO with it      */
    WORD             cbactarg;       /* what that action is about                */
    WORD             cbhwnd, cbmsg;  /* for the log; 0/0 when not a message     */
    /* ── ★★★ A STRUCTURE TO PUT WHERE THE GUEST CAN REACH IT. ────────────────
         Some messages carry a POINTER, not a value -- WM_CREATE's lParam is an
         LPCREATESTRUCT -- and a 16-bit program can only follow a pointer that
         lives behind a selector it already has. The host has no 16-bit heap of
         its own, so the structure is placed on the GUEST'S OWN STACK just below
         the arguments, which is where real USER puts it and which needs no
         allocator: the stack selector is already valid for the guest, and the
         bytes die with the call, which is exactly their lifetime.
       `cbblobarg` is the index in cbarg[] of the HIGH word of the far pointer
         that should be made to point at it -- filled in by WowCallEnter, which
         is the first code that knows what SS:SP will be. -1 = no blob. */
    BYTE             cbblob[64];
    int              cbblobn;        /* bytes of cbblob to place; 0 = none      */
    int              cbblobarg;      /* cbarg[] index to receive SEG:OFF, or -1  */
    /* ── ★★★ "DO NOT RETURN TO THE CALLER AT ALL." (session 57) ────────────────
         Every other field here describes a call to make BEFORE resuming the
         guest; this one says the guest must not be resumed past its BOP yet,
         because the service it asked for -- DialogBox -- is defined as not
         returning until a dialog is dismissed. The host runs the modal loop
         instead and completes this call much later, out of the same BOP handler.
       ⚠ THE SERVICE STILL WRITES A RETURN VALUE, and it is deliberately the one
         session 56 wrote: if the host declines to run the loop (callbacks off,
         stack full) the guest gets the old behaviour rather than a hole nobody
         filled. See src/wow/wowdlg.h. */
    int              modaldlg;       /* 1 = a modal dialog was parked by this call */
    /* 1 = an ENUMERATION was armed (wowenum.h) and its first call is owed. Same
       "do not simply resume the guest" meaning as `modaldlg`, different
       continuation; a service sets exactly one of them. */
    int              enumreq;
    /* ★ THE GUEST'S OWN DS AT THE BOP. A service that has to call back into
         application code with no class or window to take an instance from --
         LineDDA is the first -- needs the data segment the guest is actually
         running on. Filled by the handler because only it can see the TIB. */
    WORD             gds;
} wow32_frame_t;

/* ---- note building (shared by EVERY id space's dispatcher) --------------
   These live here rather than in wowuser.h because five module files build
   notes with them -- USER, SHELL, COMMDLG, KEYBOARD and GDI -- and their old
   home forced an include order in which wowgdi.h had to come LAST. That order
   is wrong now that USER's GetDC has to mint a GDI token, so the helpers moved
   to the header everything already includes instead of the dependency being
   worked around with forward declarations. */
static void wu_puts(char *b, int cap, int *k, const char *s)
{
    while (*s && *k < cap - 1) b[(*k)++] = *s++;
    b[*k] = 0;
}

/* Hex widths for wu_puthex: a byte, a word, a dword. */
#define WOW_HEX_BYTE_DIGITS  2
#define WOW_HEX_WORD_DIGITS  4
#define WOW_HEX_DWORD_DIGITS 8
static void wu_puthex(char *b, int cap, int *k, DWORD v, int digits)
{
    static const char hx[] = "0123456789abcdef";
    int i;
    for (i = digits - 1; i >= 0; --i) {
        if (*k >= cap - 1) break;
        b[(*k)++] = hx[(v >> (i * 4)) & 0xF];
    }
    b[*k] = 0;
}

/* A quoted string, with the quotes, truncated rather than dropped. */
static void wu_putq(char *b, int cap, int *k, const char *s)
{
    wu_puts(b, cap, k, "\"");
    wu_puts(b, cap, k, s);
    wu_puts(b, cap, k, "\"");
}

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
   A stepped-over call leaves the return slot holding whatever the stack last had
   there, and krnl386 receives it as the call's answer and acts on it -- so an
   unimplemented call is not inert, it answers at random. Two walls in this project
   were that value and not the guest: the null-`ES` fault after `WowLoadModule`
   (a stale value accepted as a module handle) and, upstream of it, a stale
   non-zero taken as failure inside LoadModule. Reading the slot back and PRINTING
   it is what lets a later reader tell "krnl386 decided this" from "our litter
   decided this". */
static DWORD wow32_peekret(const wow32_frame_t *f)
{
    return (DWORD)wow32_peekw(f->bp + WOW32_OFF_RET)
         | ((DWORD)wow32_peekw(f->bp + WOW32_OFF_RET + 2) << 16);
}

/* ── ★★ WHAT AN UNIMPLEMENTED CALL ANSWERS. (GH #128, session 36) ─────────────
     Session 35 measured that a stepped-over call is not inert: krnl386 takes the
     return slot as the answer and acts on it. Leaving the slot unwritten
     therefore does not mean "no answer", it means "an answer drawn from whatever
     the stack last held" -- which made two separate runs stop for reasons that
     were OURS, and which no amount of re-running can reproduce or rule out.
   ★ SO ANSWER THE SAME WAY EVERY TIME. This does not make the answer TRUE -- it is
     still a call we have not implemented, and the log says so on every line. It
     makes the run REPRODUCIBLE, which is the property every other conclusion in
     this investigation rests on. A deterministic wrong answer can be traced from
     the wall back to its cause; a random one cannot.
   ★ AND ZERO IS THE BETTER CONSTANT, at both calls measured so far -- chosen from
     what the guest visibly did with each answer, not from taste:
       0xc6  litter 0x01b7 sent the guest down a path we read as FAILURE. Zero
             does not.
             ⚠ s92: non-zero actually makes the guest FREE the block, not fail --
             but zero is still right: answering 1 broke every launch (see 0xc6's
             own case).
       0x2d  litter 0x2714 was accepted as a MODULE HANDLE (Win16 treats < 0x21
             as an error code) and ran on into the terminal #GP with a NULL
             parameter block. Zero is below 0x21, so LoadModule takes its error
             path and REPORTS, instead of faulting somewhere else.
     In both cases zero fails nearer the cause, which is the whole point.
   ⚠ NOT 0xFFFFFFFF: that is WOW32_DECLINE, and a decline is a different statement
     ("ask real DOS instead") that only holds at the sites in wow32_decline_sites.
     Reusing it here would make every unimplemented call claim to be one. */
#define WOW32_UNIMPL_RET 0u

/* ---- the function IDs we can name -------------------------------------- */
/* Names for the 28 that krnl386's export table names outright, plus the ones
   worked out from their call sites. An ID with no name here is not a gap in the
   evidence -- it is a function reached only from internal code, not yet pinned
   down by what it is called with and what the guest does with the answer. */
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
#define WOW32_SETCURRENTDRIVE           0xc8   /* named from its use, see below     */
#define WOW32_GETPROFILEINT             0x39   /* pinned from its strings, below    */
#define WOW32_GETPROFILESTRING          0x3a   /* ★ IT DECIDES PAINT'S COLOUR MODE */
#define WOW32_WRITEPROFILESTRING        0x3b   /* #293, the inventory's top gap    */
#define WOW32_WRITEPRIVATEPROFILESTRING 0x81
#define WOW32_WOWGETNEXTVDMCOMMAND      0x70
#define WOW32_OLDYIELD                  0x75
#define WOW32_REGISTERDOSDATA           0x78   /* named from its use, below       */
#define WOW32_GETSHORTPATHNAME          0x7b
#define WOW32_SETCURRENTDIR             0x82   /* ★ WHERE File > Save As PUT THE FILE */
#define WOW32_ACCEPTTASKSELECTOR        0x7d   /* pinned from its use, below        */
#define WOW32_GETPRIVATEPROFILESTRING   0x80   /* pinned from its strings, below    */
/* ── ★★★ 0x7f GetPrivateProfileInt -- NAMED BY MINESWEEPER'S FIRST RUN. ──────
     14 arg bytes = 4 + 4 + 2 + 4, and the fourth is a FILENAME, so it is the
     private twin of 0x39 exactly as 0x80 is of the string form. WINMINE.EXE
     asks it for "Height", "Width", "Mines", "Difficulty", "Xpos" and "Ypos" out
     of `winmine.ini`, 37 times in one startup.
   ★ AND UNIMPLEMENTED IT ANSWERED 0, WHICH THE CALL SITE READS AS A REAL
     ANSWER -- the fifth time this exact shape has cost this project a session.
     `nDefault` is right there in the arguments (8, for "Height") and we were
     throwing it away, so Minesweeper laid itself out with every stored value 0
     and created its window at (-2,-48): its caption and menu bar OFF THE TOP OF
     THE SCREEN. Nothing looked broken in the log. */
#define WOW32_GETPRIVATEPROFILEINT      0x7f
#define GPPI_ARG_FILE     0                    /* far */
#define GPPI_ARG_DEFAULT  4
#define GPPI_ARG_KEY      6                    /* far */
#define GPPI_ARG_APP     10                    /* far */
#define WOW32_WOWWAITFORMSGANDEVENT     0x83
#define WOW32_WOWMSGBOX                 0x84
#define WOW32_GETDATETIME               0x86   /* answer is used as a packed date  */
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
   ★ NAMED BY HOW IT ARRIVES, in pairs, and the second call is what makes it
     unambiguous:
       0xc5(dst, src)    -- resolve; a zero answer makes the guest fall back to
                            the name it started with
       0xc5(dst, NULL)   -- release, made only after a successful resolve, and
                            its result ignored
     so the pair is resolve/release and the host owns the storage between them.
   ★★ AND `dst` RECEIVES A FAR POINTER, NOT A COPIED STRING: on success the guest
     goes on to use the far pointer stored at `dst` exactly where, on failure, it
     uses the far pointer to the name it was given. One is the resolved path, the
     other is the original name; they must be the same kind of thing.
   ⇒ Answering 0 is what made krnl386 compose module names against the CURRENT
     DIRECTORY and fail to open `C:\Documents and Settings\<user>\SHELL.DLL`. */
#define WOW32_RESOLVEMODULEPATH         0xc5
/* ── ★★ 0xd0: GetWindowsDirectory(lpBuffer, uSize). (session 40) ──────────────
     Not named by krnl386's export table, so it comes from how it is called and
     from what the answer is USED for -- and the two agree.

   ★ krnl386's own call says what SHAPE it is: 6 argument bytes, a far buffer
     and 0x80 (lpBuffer, uSize=128); on a non-zero answer it takes the length of
     the NUL-terminated string now in the buffer and keeps both. So it fills the
     caller's buffer with a path and returns non-zero on success. That is a
     Get<something>Directory and nothing else.

   ★ SYSEDIT says WHICH directory, and it is a count rather than a guess:
     `sysedit` imports `KERNEL.134 GETWINDOWSDIRECTORY` and NOT
     `KERNEL.135 GETSYSTEMDIRECTORY` (`neimports.py`: exactly two fixups), and in
     a run SYSEDIT's task makes exactly TWO calls to this id. It then `lstrcat`s `\SYSTEM.INI` and
     `\WIN.INI` onto the answer, which is where those files live.
   ⚠ WHAT LEAVING IT UNIMPLEMENTED LOOKED LIKE: not an error, but a WRONG NAME.
     The buffer kept whatever was in it, so SYSEDIT opened -- and titled a
     window -- `"REGISTERPENAPP\SYSTEM.INI"`, a path built from another module's
     leftover string. The "runs but lies" class, and it took the callback work to
     get far enough to see it. */
#define WOW32_GETWINDOWSDIRECTORY       0xd0
#define WOW32_WOWSHUTDOWNTIMER          0xcd
/* Serviced in main.c, not here: it needs the DOS machine. Listed so the name table
   below can print it, and so nobody adds a decline for it -- krnl386 reports a
   DX=0xFFFF answer to the app as a hard error, not as "ask DOS instead". */
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
    case WOW32_SETCURRENTDRIVE:        return "SetCurrentDrive";
    case WOW32_GETPROFILEINT:          return "GetProfileInt";
    case WOW32_GETPROFILESTRING:       return "GetProfileString";
    case WOW32_SETCURRENTDIR:          return "SetCurrentDirectory";
    case WOW32_GETPRIVATEPROFILESTRING: return "GetPrivateProfileString";
    case WOW32_GETPRIVATEPROFILEINT:    return "GetPrivateProfileInt";
    case WOW32_WRITEPROFILESTRING:      return "WriteProfileString";
    case WOW32_WRITEPRIVATEPROFILESTRING: return "WritePrivateProfileString";
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
static char g_wow_cmd_dir[MAX_PATH] = { 0 }; /* #164: the launch directory, 8.3; "" = none */
static void wow32_curdir_set(const char *dir);  /* #164: main.c's per-task directory table */
static int  g_wow_cmd_taken     = 0;       /* delivered already -- deliver once */
/* ── ★ WIN16 SEES 8.3 NAMES, AND ONLY 8.3 NAMES. (s73) ──────────────────────────
     krnl386's loader opens the program through INT 21h, and a Win16 DOS world has no
     long file names: "C:\Documents and Settings\...\notepad\notepad.EXE" fails at
     the first space, WowFailedExec fires, and the user reads "Cannot find file ... (or
     one of its components)" for a file that is right there. Stock WOW hands krnl386
     the SHORT form (a double-click's AppName arrives that way). Every path we hand
     the Win16 side -- the launch command, ResolveModulePath's answer -- goes through
     here. A path with no short form (the API returns 0) is passed as given. */
static void wow_shorten(char *path, unsigned cap)
{
    char sh[MAX_PATH + 16]; DWORD n = 0;
    if (path[0]) n = GetShortPathNameA(path, sh, sizeof sh);
    if (n && n < sizeof sh && n < cap) { unsigned i; for (i = 0; i <= n; ++i) path[i] = sh[i]; }
}

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
     32-bit companion first. When the companion answers 0xFFFF(FFFF), the
     original INT 21h request then arrives at the PREVIOUS INT 21h handler --
     observed in the run log as the matching `INT21h AH=..` line right after the
     call. In this host that handler is our own DOS layer, the one COMMAND.COM and
     Doom already use. So a sentinel return hands file I/O to working code instead
     of to a parallel Win32 handle table that would then disagree with every call
     that chains anyway.

   ⚠ ONLY WHERE THE CALL SITE SAYS SO. There are sites (0x82, 0xc9, 0x71) where
     0xFFFF is a plain ERROR and krnl386 reports failure to the app rather than
     chaining. Declining there would turn
     "not implemented" into "the file does not exist" -- a wrong answer instead
     of a missing one, which is the more expensive kind. They are NOT in the list
     below, and the list is per call site, not a guess about the family.

   ⚠ Some sites test AX and some test DX, so the sentinel has to be 0xFFFFFFFF
     rather than either half.

   ▸ THE HONEST TRADE. Real WOW routes these to Win32, so a Win16 app gets NT
     file semantics (sharing modes, long names). Declining gives it our DOS
     semantics instead. For loading and running a program that is the same thing,
     and it is one line to change later -- but it IS a difference, so it is
     written down rather than discovered. */
#define WOW32_DECLINE 0xFFFFFFFFu

/* Verified declinable. All seven are the INT 21h file family; declining 0x97 and
   0x6f makes krnl386 re-issue a plain AH=3Fh / AH=40h to DOS, visible in the run
   log as the INT 21h line that follows the call. */
#define WOW32_FILE_OPEN        0xc1   /* AH=3Dh                    */
#define WOW32_FILE_READ        0x97   /* -> AH=3Fh on decline      */
#define WOW32_FILE_CLOSE       0xc2   /* AH=3Eh                    */
#define WOW32_FILE_GETATTR     0xc7   /* AH=43h AL=0               */
#define WOW32_FILE_7E          0x7e   /*                           */
#define WOW32_FILE_GETDATE     0x89   /* AH=57h AL=0               */
#define WOW32_FILE_WRITE       0x6f   /* -> AH=40h on decline      */
/* ── ★ THE SEEK, AND WHY IT WAS MISSED. (GH #128, session 34) ─────────────────
     The first list of declinable sites was incomplete: 0x98 was not on it, and
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
#define WOW32_FILE_SEEK        0x98   /* -> AH=42h on decline      */

/* ── ★★ DECLINING IS A PROPERTY OF THE CALL SITE, NOT OF THE ID. ───────────────
     This was keyed by ID, and that is measurably wrong: krnl386 calls 0x97 (read)
     from TWO places with OPPOSITE meanings --
       one where 0xFFFF means "ask real DOS": an `INT21h AH=3F` follows the
         decline in the log. A decline is a true statement.
       one where 0xFFFF is RETURNED TO THE CALLER as a failure: nothing follows.
         A decline here turns "we did not implement this" into "the read failed",
         which is a WRONG ANSWER rather than a missing one.

     Session 34's failing run declined at `from=0x8a51` -- the second kind -- and the
     log shows what that looks like: no `INT21h AH=3F` follows it, unlike every other
     read in the run. 0x6f (write) has the same split.

   ⚠ THE VALUES BELOW ARE krnl386 CALL-SITE RETURN OFFSETS -- what the frame carries
     at WOW32_OFF_FROM, as seen in the run log's `from=` -- each one checked to be
     followed by the chained INT 21h request when declined. They are specific to the
     XP krnl386.exe build. Anything not listed is not declined -- an unknown site
     gets the honest "unimplemented" rather than a guess. */
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
 * ⚠ EVERY SERVICE MUST CALL wow32_setret(), even a void one. The guest receives
 *   the return slot unconditionally, so "no return value" still means "write zero"
 *   -- otherwise the guest gets whatever was on the stack. GlobalMemoryStatus is
 *   the void case and it still writes 0.
 */
/* ── ★★ GENERIC THUNKS (#5, s90): 16-bit code calling 32-bit DLLs directly. ──────
     LoadLibraryEx32W / GetProcAddress32W / FreeLibrary32W / GetVDMPointer32W, and
     CallProc32W / _CallProcEx32W -- the documented Win16 route to Win32 (the WOW
     generic-thunk API), and the one XP's own 16-bit MMSYSTEM uses to reach WINMM.
   ★ THE 32-BIT SIDE IS THIS PROCESS. Under NT the DLL is loaded into the NTVDM
     process and called there; this host IS that process, so LoadLibraryExA and a
     direct call are the faithful answer, not a shortcut.

   ── THE CallProc FRAME, AS IT ARRIVES (KERNEL ords 517/518 -> thunk id 0x1c) ──
   Both arrive as id 0x1c with an argument byte count of 0 -- so the arguments lie
   past the declared block and are read RAW (offsets from the argument block, as
   measured with w_gthunk):
       +0 a saved bp   +2/+4 the app's far return   +6 cParams (DWORD)
       +10 fAddressConvert (DWORD)   +14 lpProcAddress (DWORD)   +18 the params
   CallProc32W is PASCAL: p1 was pushed first, so +18 holds pN and p1 is highest.
   _CallProcEx32W is CDECL: +18 holds p1. They arrive told apart for us -- 518
   with 0x4000 in cParams' high word, 517 without -- and krnl386 removes the
   arguments itself (517 per the PASCAL convention; 518 is cdecl, the caller pops).
   ⚠ The mask's bit order is taken from the probe run against stock
     (tests/probes/win16/w_gthunk, 16/16), not from the documentation. */
#define WOW_GT_MAXP 32
static WORD wow32_rawargw(const wow32_frame_t *f, int off)
{
    if (off < 0 || off > 0x200) return 0;
    return wow32_peekw(f->bp + WOW32_OFF_ARGS + off);
}
static DWORD wow32_rawargd(const wow32_frame_t *f, int off)
{
    return (DWORD)wow32_rawargw(f, off) | ((DWORD)wow32_rawargw(f, off + 2) << 16);
}
/* A protected-mode 16:16 far pointer -> the host address it names (0 for NULL). */
static DWORD wow32_flat(const wow32_frame_t *f, DWORD fp)
{
    WORD sel = (WORD)(fp >> 16);
    DWORD base;
    if (!sel || !f->sel2lin) return 0;
    base = f->sel2lin(sel, f->ctx);
    return base ? base + (fp & 0xFFFF) : 0;
}
/* Call a 32-bit function with n DWORDs, a[0] first. ESP is restored by hand, so a
   STDCALL target (which pops) and a CDECL one (which does not) both come back
   with the stack where it was. */
static DWORD wow_gt_invoke(DWORD fn, const DWORD *a, int n)
{
    DWORD r;
    __asm__ __volatile__ (
        "movl %%esp, %%edi\n\t"
        "movl %3, %%ecx\n\t"
        "1:\n\t"
        "testl %%ecx, %%ecx\n\t"
        "jz 2f\n\t"
        "pushl -4(%2,%%ecx,4)\n\t"
        "decl %%ecx\n\t"
        "jmp 1b\n\t"
        "2:\n\t"
        "call *%1\n\t"
        "movl %%edi, %%esp\n\t"
        : "=&a"(r)
        : "b"(fn), "S"(a), "d"(n)
        : "ecx", "edi", "memory", "cc");
    return r;
}

static int wow32_call(wow32_frame_t *f, wow32_dosdata_t *dd)
{
    /* ★ NOT OUR ID SPACE, NOT OUR ANSWER. See `krnl` in wow32_frame_t. */
    if (!f->krnl) return 0;
    switch (f->id) {

    /* ── 0xb8 VirtualAlloc(lpAddress, dwSize, flAllocationType, flProtect) ──
         krnl386 services DPMI 0501 ("allocate memory block") with this rather
         than passing it to the DPMI host: it arrives when a guest issues 0501,
         and its result comes back to that guest as 0501's address-and-handle
         pair (BX:CX, SI:DI).
       ★ A REAL VirtualAlloc IS THE RIGHT ANSWER, not a fake. The guest runs in
         our own address space, so an address we allocate is one the guest can
         reach once it puts a descriptor over it -- which is the next thing it
         does. Handing back a plausible-looking number would fail later, further
         from the cause. */
    /* ── 0x9a LoadLibraryEx32W(lpszLibFile, hFile, dwFlags) = 12, reversed. */
    case WOW32_LOADLIBRARYEX32W: {
        char  path[MAX_PATH];
        DWORD fl = wow32_argd(f, 0);
        HMODULE h = NULL;
        if (wow32_argstr(f, 8, path, (int)sizeof path) && path[0])
            h = LoadLibraryExA(path, NULL, fl);
        wow32_setret(f, (DWORD)(ULONG_PTR)h);
        return 1;
    }
    /* ── 0x8c FreeLibrary32W(hInst32) = 4. */
    case WOW32_FREELIBRARY32W: {
        DWORD h = wow32_argd(f, 0);
        wow32_setret(f, h ? (FreeLibrary((HMODULE)(ULONG_PTR)h) ? 1 : 0) : 0);
        return 1;
    }
    /* ── 0x8d GetProcAddress32W(hInst32, lpszProc) = 8: +0 the name (a null
         selector = an ordinal in the offset), +4 the module. */
    case WOW32_GETPROCADDRESS32W: {
        DWORD h  = wow32_argd(f, 4);
        DWORD np = wow32_argd(f, 0);
        char  nm[256];
        FARPROC p = NULL;
        if (h) {
            if (!(np >> 16)) p = GetProcAddress((HMODULE)(ULONG_PTR)h,
                                                (LPCSTR)(ULONG_PTR)(np & 0xFFFF));
            else if (wow32_argstr(f, 0, nm, (int)sizeof nm))
                p = GetProcAddress((HMODULE)(ULONG_PTR)h, nm);
        }
        wow32_setret(f, (DWORD)(ULONG_PTR)p);
        return 1;
    }
    /* ── 0x1a GetVDMPointer32W(lpAddress, fMode) = 6: +0 fMode (1 = protected
         mode, 0 = real mode), +2 the 16:16 address. Real-mode memory sits at
         linear 0 of this process, as in NTVDM. */
    case WOW32_GETVDMPOINTER32W: {
        WORD  mode = wow32_argw(f, 0);
        DWORD fp   = wow32_argd(f, 2);
        DWORD r;
        if (mode) r = wow32_flat(f, fp);
        else      r = ((fp >> 16) << 4) + (fp & 0xFFFF);
        wow32_setret(f, r);
        return 1;
    }
    /* ── 0x1c CallProc32W / _CallProcEx32W. See the frame note above wow32_call. */
    case WOW32_CALLPROCEX32W: {
        DWORD cp   = wow32_rawargd(f, 6);
        DWORD mask = wow32_rawargd(f, 10);
        DWORD fn   = wow32_rawargd(f, 14);
        int   ex   = (cp & 0x40000000u) != 0;      /* krnl386's own mark: 518 */
        int   n    = (int)(cp & 0xFFFF);
        DWORD a[WOW_GT_MAXP];
        int   i;
        if (!fn || n < 0 || n > WOW_GT_MAXP) { wow32_setret(f, 0); return 1; }
        for (i = 0; i < n; ++i) {
            /* a[i] = parameter i+1. Pascal: p(i+1) sits (n-1-i) DWORDs above +18. */
            int   slot = ex ? i : (n - 1 - i);
            DWORD v    = wow32_rawargd(f, 18 + 4 * slot);
            /* MASK, MEASURED (w_gthunk vs stock): bit 0 = the LAST parameter for
               CallProc32W and the FIRST for _CallProcEx32W -- i.e. bit 0 is always
               the parameter nearest the top of the 16-bit stack. */
            int   bit  = ex ? i : n - 1 - i;
            if (mask & (1u << bit)) v = wow32_flat(f, v);
            a[i] = v;
        }
        wow32_setret(f, wow_gt_invoke(fn, a, n));
        return 1;
    }

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
         reads the fields out of its own stack buffer. */
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
         Named by krnl386's export table. What it is for is "am I on a DBCS
         system" -- the Far-East LANGIDs 0x411/0x412/0x404/0x804/0x0c04. Answering
         with the host's real LANGID is both correct and the whole point: a
         Japanese XP should make krnl386 take the DBCS path. */
    case WOW32_GETSYSTEMDEFAULTLANGID:
        wow32_setret(f, (DWORD)GetSystemDefaultLangID());
        return 1;

    /* ── ★★ 0x7b GetShortPathName(lpszLong, lpszShort, cch). (s89, #270) ─────
         Named, never answered. At start-up krnl386 calls this with the value of
         `SYSTEMROOT=` from its environment and a 0x79-byte buffer (seen in the
         run log), and the system directory it later reports is the returned
         length's worth of that answer + "\SYSTEM". Stepped over, the length was
         0 and KERNEL.135 GetSystemDirectory answered "\SYSTEM" -- 7 characters,
         no drive (the Win16 test `kfile.sysdir.*`). Frame, reversed: +0 cch,
         +2 lpszShort far, +6 lpszLong far. Written only if it fits, as Win32
         does; otherwise 0, so krnl386 never copies a length it was not given. */
    case WOW32_GETSHORTPATHNAME: {
        volatile BYTE *dst = wow32_argptr(f, 2);
        WORD cap = wow32_argw(f, 0);
        char src[MAX_PATH], out[MAX_PATH];
        DWORD n;
        if (!dst || !cap || !wow32_argstr(f, 6, src, sizeof src) || !src[0]) {
            wow32_setret(f, 0); return 1;
        }
        n = GetShortPathNameA(src, out, sizeof out);
        if (!n || n >= sizeof out || n + 1 > (DWORD)cap) { wow32_setret(f, 0); return 1; }
        { DWORD k; for (k = 0; k <= n; ++k) dst[k] = (BYTE)out[k]; }
        wow32_setret(f, n);
        return 1;
    }

    /* ── ★★ 0xd0 GetWindowsDirectory(lpBuffer, uSize) -- see the note above.
         The real Win32 call against the real directory, for the same reason
         GetDriveType is a pass-through: our DOS layer opens real paths on the
         real filesystem, so the host's Windows directory IS the guest's.
       ⚠ uSize is the GUEST'S claim about its own buffer and the only bound
         there is -- krnl386 declares 0x80 bytes. Never write more
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
         were pinned from the strings their arguments point at, as they arrive --
         which turns out to name them outright. The first call at start-up:

           lpAppName        "BOOT"
           lpKeyName        "WOWSHELL"
           lpDefault        "WOWEXEC.EXE"
           lpReturnedString an empty 0x50-byte buffer
           nSize            0x50
           lpFileName       "SYSTEM.INI"      ; 22 arg bytes = 4+4+4+4+2+4  ✓

         and the very next thing krnl386 does is LoadModule the string we put in
         that buffer. So this call is krnl386 asking **what Win16 program to
         run**, and answering it is the launch itself.
       ★ A second call has the same signature over `[DEBUG] OUTPUTTO` with an
         empty default. Two independent calls agreeing on six arguments, with the
         documented Win16 GetPrivateProfileString shape, is what makes this a
         reading rather than a guess.
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

    /* Answered with the REAL Win32 call against the REAL file, like 0x80 above.
       ⚠ nDefault IS THE WHOLE POINT: it is what the guest gets when the key is
         absent, and it is what this call answered with 0 instead of. Passing it
         through means a missing winmine.ini gives Minesweeper its OWN defaults,
         which is what it would get on real Windows.
       ⚠ `hd ? app : NULL` -- NULL is documented for the two names; never a
         string literal, which is read-only and XP's profile code WRITES to these
         buffers (session 38, and it killed the host). */
    case WOW32_GETPRIVATEPROFILEINT: {
        char app[128], key[128], file[260];
        int  ha = wow32_argstr(f, GPPI_ARG_APP, app, sizeof app);
        int  hk = wow32_argstr(f, GPPI_ARG_KEY, key, sizeof key);
        WORD def = wow32_argw(f, GPPI_ARG_DEFAULT);
        UINT got;
        wow32_argstr(f, GPPI_ARG_FILE, file, sizeof file);
        got = GetPrivateProfileIntA(ha ? app : NULL, hk ? key : NULL,
                                    (INT)def, file);
        /* Win16 returns a UINT; the value is a WORD on the guest's side. */
        wow32_setret(f, (DWORD)(WORD)got);
        return 1;
    }

    /* ── 0x39 GetProfileInt(lpAppName, lpKeyName, nDefault) ───────────────
         10 arg bytes = 4 + 4 + 2, no filename -- so it is the SYSTEM profile
         twin of 0x80 rather than a private one. The calls seen at run time read
         as that, by their strings:
           ("KERNEL", "GPCONTINUE", <a default>)
           ("ModuleCompatibility", <the module's own name>, 0)
         -- the second one per module just loaded, which is exactly what that
         section is for.
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
           host died: `0xc0000005` in ntdll, a one-byte write to the literal's own
           address. It survived ten calls in every earlier run
           because every one of them had both names non-empty; the task scheduler
           simply let the guest get as far as asking with one missing.
         ⇒ The locals are writable and already the right size, so pass them always
           and make "absent" an empty *buffer* rather than an empty literal. */
    /* ── ★★★★ 0xc8 -- AND IT IS WHY krnl386 THINKS IT IS ON DRIVE A:. ────────
         Named from when it arrives, not from an export table: a Win16 guest's
         INT 21h AH=0Eh (SELECT DEFAULT DRIVE) for a drive other than the current
         one produces this call with the requested drive, and the guest then sees
         AL=26 drives, as DOS returns. And WHATEVER WE RETURN BECOMES krnl386's
         ANSWER TO "what drive am I on": krnl386 keeps it as its cached current
         drive.

       ⚠ UNIMPLEMENTED, THIS RETURNED THE HARNESS SENTINEL 0 -- and 0 is a
         perfectly good drive index, so krnl386 cached "the current drive is A:"
         after every select. Measured on TERMINAL, which walks drives 0x19 down
         to 0 and so calls this 25 times in one startup; WRITE does the same. It
         is [[stepped-over-call-answers-at-random]] again: not a crash, not a log
         line, just a number that means something.

       ⚠ AND WE DO NOT SUPPORT CHANGING DRIVES. INT 21h AH=0Eh accepts a select
         and ignores it, so returning the REQUESTED drive would claim a switch
         that did not happen -- and the guest's very next AH=19h would contradict
         it. Answer the one drive this machine is on, from the constant all three
         routes now share. When a real per-process current drive exists, this
         changes with it and not before. */
    /* ── ★★★★★ 0x84 WowMsgBox -- THE ONE THAT MAKES A DEAD GUEST SAY WHY.
         (session 56) ────────────────────────────────────────────────────────
         This is krnl386's own error reporter: when a launch fails it calls
         WowFailedExec (0x9d) and then this, with the text, and then
         ExitKernelThunk. Unimplemented, all three were stepped over -- so a
         guest that could not start simply VANISHED, with the reason sitting in
         a string nobody displayed.
       ★ MEASURED on WINFILE, which is exactly this shape:
             FUNC=0x9d WowFailedExec        -> UNIMPLEMENTED
             FUNC=0x84 WowMsgBox  arg = "Can't run 16-bit Windows program"
             FUNC=0x02 ExitKernelThunk      -> shutting the VDM down
         Its real cause is an ASSET gap, not a host defect: WINFILE.EXE's module
         table is [VER, KERNEL, GDI, USER, KEYBOARD, COMMDLG, SHELL, SCONFIG,
         COMMCTRL] -- it is the Windows for Workgroups build and imports
         SCONFIG.DLL, which is not on the box (`open "SCONFIG.DLL" -> CF=1
         gle=2`, and it is the ONE module the resolver could not give a path).
         That replaces the recorded blocker for this guest, which named
         ShellExecute and CreateWindowEx and had been stale since both were
         implemented later in s55.
       ⇒ THE GENERAL WIN IS NOT WINFILE. It is that this is the fifth time a
         Win16 guest has diagnosed itself in English and the sixth is now free:
         [[wow-real-hwnd-frontier]]'s standing advice is `implement MessageBox
         FIRST on any new guest`, and krnl386's own message box is the one that
         covers every guest that dies BEFORE it can put up its own.
       ⚠ THE OFFSETS ARE READ OFF A RUN, not from a signature. The harness's own
         `★ arg[2]` line resolves the text, and its `k` is a WORD index while
         wow32_argd takes a BYTE offset -- hence 4, not 2. `wType` at 0 was
         0x0030 = MB_ICONEXCLAMATION|MB_OK, which is what krnl386 would use for
         a launch failure and is the reading that makes the other fields line up.
         The caption slot is tried and falls back, because a wrong caption is
         cosmetic and a missing message is not.
       ⚠ IT IS MODAL, deliberately. The guest is being told its program cannot
         run and stock does exactly this; a headless run that used to end
         silently now ends with a box on screen saying why, which is strictly
         more information for the same outcome. */
    case WOW32_WOWMSGBOX: {
        char s4[512], s8[512], line[1200];
        const char *body, *capt;
        WORD type = wow32_argw(f, 0);
        int  n = 0;
        if (!wow32_argstr(f, 4, s4, sizeof s4)) s4[0] = 0;
        if (!wow32_argstr(f, 8, s8, sizeof s8)) s8[0] = 0;
        /* ★ THE SLOTS ARE PINNED, and by data rather than by a signature --
             the log line below was added first precisely so they could be. One
             WINFILE run prints BOTH, and they are unambiguous:
                 arg4 = "Can't run 16-bit Windows program"
                 arg8 = "Cannot find file C:\WIN16\WINFILE.EXE (or one of its
                         components). Check to ensure the path and filename are
                         correct and that all required libraries are available"
             The short one is the CAPTION and the long one is the TEXT.
           ⚠ MY FIRST CUT GUESSED THE OTHER WAY ROUND -- "take whichever slot has
             text as the body" -- and put the explanation in the title bar and
             the summary in the body. It looked plausible and was backwards; the
             instrument added one step earlier is what showed it. Keep both slots
             on the log line so this stays checkable rather than remembered. */
        capt = s4[0] ? s4 : "NTVDMEX -- 16-bit Windows";
        body = s8[0] ? s8 : (s4[0] ? s4 : "(krnl386 reported an error with no text)");
        /* ── ★★ RECORD BEFORE BLOCKING. ────────────────────────────────────
             MessageBoxA does not return until a human clicks, and this arm's
             log block is not flushed until it does -- so the FIRST cut put the
             box on screen and left NOTHING in the log, including the harness's
             own arg lines. An instrument that blocks before it records is not an
             instrument. Write the line here, then show the box. */
        {   const char *pre = "  WOWMSGBOX: krnl386 is reporting an error to the "
                              "user. type=0x";
            const char *h = "0123456789abcdef";
            const char *q; int i2;
            for (q = pre; *q && n < (int)sizeof line - 2; ++q) line[n++] = *q;
            line[n++] = h[(type >> 12) & 0xF]; line[n++] = h[(type >> 8) & 0xF];
            line[n++] = h[(type >> 4) & 0xF];  line[n++] = h[type & 0xF];
            for (q = " arg4=\""; *q; ++q) line[n++] = *q;
            for (i2 = 0; s4[i2] && n < (int)sizeof line - 40; ++i2)
                line[n++] = (s4[i2] == '\r' || s4[i2] == '\n') ? ' ' : s4[i2];
            for (q = "\" arg8=\""; *q; ++q) line[n++] = *q;
            for (i2 = 0; s8[i2] && n < (int)sizeof line - 8; ++i2)
                line[n++] = (s8[i2] == '\r' || s8[i2] == '\n') ? ' ' : s8[i2];
            for (q = "\"\r\n"; *q; ++q) line[n++] = *q;
            log_append(LOG_PATH, line, line + n);
        }
        MessageBoxA(NULL, body, capt,
                    (UINT)((type & 0xF0u) | MB_OK | MB_SETFOREGROUND));
        wow32_setret(f, 1);                       /* IDOK */
        return 1;
    }

    case WOW32_SETCURRENTDRIVE:
        wow32_setret(f, (DWORD)DOS_CURRENT_DRIVE);
        return 1;

    case WOW32_GETPROFILEINT: {
        char app[128], key[128];
        WORD def;
        if (!wow32_argstr(f, 6, app, sizeof app)) app[0] = 0;
        if (!wow32_argstr(f, 2, key, sizeof key)) key[0] = 0;
        def = wow32_argw(f, 0);
        wow32_setret(f, (DWORD)GetProfileIntA(app, key, (INT)def));
        return 1;
    }

    /* ── ★★★★★ 0x3a GetProfileString -- AND IT IS WHY MS PAINT WAS BLACK AND
         WHITE. ────────────────────────────────────────────────────────────────
         The string twin of 0x39, 18 argument bytes = 4 + 4 + 4 + 4 + 2 and no
         filename, so it is WIN.INI. Both the layout and the identity come from
         one logged call (PBRUSH.EXE's import of KERNEL.58) rather than from
         arithmetic on the id:

           args as logged   (0009 | 6f0a 09c7 | 012a 09c7 | 090e 09c7 |
                                                            08f4 09c7)
           the strings: "Paintbrush", "clear", default "COLOR", nSize = 9

         ⇒ nSize@0, lpReturnedString@2, lpDefault@6, lpKeyName@10, lpAppName@14.

       ★★★ WHY IT MATTERS. Paint's `[Paintbrush] clear=` setting chooses between a
         colour and a monochrome palette: "COLOR" (the default) means colour,
         anything else -- including an empty answer -- means 28 greys. Every grey
         brush a run has ever logged -- 0x00090909, 0x00121212, 0x00212121 -- is
         one of those greys (the colour palette's entries as luminances), so Paint
         was never losing colour in a blit. **It was told to be monochrome, by us,
         by answering 0.** Answering with the default turned the palette to
         colour on the next run.
       ⚠ THE DEFAULT IS "COLOR", so a rig with no `[Paintbrush] clear` key gets
         colour -- which is why searching WIN.INI for a colour key found nothing
         and the key still turned out to be the answer. The bug was never in the
         profile, it was that an unimplemented call cannot return a default.
       ⚠ Same read-only-literal trap as its two neighbours: XP's profile code
         writes into the name buffers it is given, so the locals are passed
         always and "absent" is an empty *buffer*.
       ⚠ Win32 truncates to nSize-1 and returns the character count without the
         NUL, which is Win16's own convention -- and "COLOR" is 5. */
    case WOW32_GETPROFILESTRING: {
        char app[128], key[128], def[260], buf[512];
        volatile BYTE *dst = wow32_argptr(f, 2);
        WORD  n = wow32_argw(f, 0);
        DWORD got;
        unsigned i;
        if (!wow32_argstr(f, 14, app, sizeof app)) app[0] = 0;
        if (!wow32_argstr(f, 10, key, sizeof key)) key[0] = 0;
        if (!wow32_argstr(f,  6, def, sizeof def)) def[0] = 0;
        if (n > sizeof buf) n = (WORD)sizeof buf;
        if (!n) { wow32_setret(f, 0); return 1; }
        got = GetProfileStringA(app, key, def, buf, n);
        if (dst) for (i = 0; i <= got && i < n; ++i) dst[i] = (BYTE)buf[i];
        wow32_setret(f, got);
        return 1;
    }

    /* ── #293: THE WRITE HALF. 0x3b WriteProfileString(lpAppName, lpKeyName,
         lpString) and 0x81 WritePrivateProfileString(..., lpFileName). ─────────
         Found by the s89 inventory (tools/ne/wowinventory.py), not by a run: 14
         shelf programs import one or the other -- Calc's Scientific mode, Clock's
         analog/digital, WinMine's best times, Solitaire's options -- and every
         one was stepped over, so no Win16 program ever saved a setting. Nothing
         looked broken, because the next launch simply read the defaults back.
         12 and 16 arg bytes = three and four far pointers, Pascal order like the
         Get* twins above: FIRST pushed is the HIGHEST offset, so the file (if
         any) is at 0, lpString next, lpAppName last.
       ★ NULL HAS MEANING, so `wow32_argstr`'s "no pointer" is passed as NULL:
         a NULL lpString deletes the key, a NULL lpKeyName deletes the section,
         and WritePrivateProfileString(NULL, NULL, NULL, file) flushes. Never a
         string literal in their place (the read-only trap documented at 0x39).
       ★ The REAL Win32 call against the REAL file, so XP's own IniFileMapping
         applies exactly as it does for stock WOW, and a bare filename resolves
         against the Windows directory -- the 16-bit convention.
         Win16 returns BOOL in AX. */
    case WOW32_WRITEPROFILESTRING: {
        char app[256], key[256], val[4096];
        int  ha = wow32_argstr(f, 8, app, sizeof app);
        int  hk = wow32_argstr(f, 4, key, sizeof key);
        int  hv = wow32_argstr(f, 0, val, sizeof val);
        BOOL ok = WriteProfileStringA(ha ? app : NULL, hk ? key : NULL,
                                      hv ? val : NULL);
        wow32_setret(f, ok ? 1 : 0);
        return 1;
    }
    case WOW32_WRITEPRIVATEPROFILESTRING: {
        char app[256], key[256], val[4096], file[MAX_PATH];
        int  ha = wow32_argstr(f, 12, app, sizeof app);
        int  hk = wow32_argstr(f, 8,  key, sizeof key);
        int  hv = wow32_argstr(f, 4,  val, sizeof val);
        BOOL ok;
        if (!wow32_argstr(f, 0, file, sizeof file) || !file[0]) {
            wow32_setret(f, 0);                  /* no file: nothing to write to */
            return 1;
        }
        ok = WritePrivateProfileStringA(ha ? app : NULL, hk ? key : NULL,
                                        hv ? val : NULL, file);
        wow32_setret(f, ok ? 1 : 0);
        return 1;
    }
    /* 0x03 WriteOutProfiles(): Win16's "flush the profile cache". Win32 flushes
       WIN.INI with an all-NULL write; there is no return value. */
    case WOW32_WRITEOUTPROFILES:
        WriteProfileStringA(NULL, NULL, NULL);
        wow32_setret(f, 0);
        return 1;

    /* ── ★★★★★ 0x82 SetCurrentDirectory -- AND IT IS WHERE `Save As` PUT THE
         FILE. (session 49) ──────────────────────────────────────────────────
         With the LDT collision and OLESVR's null pointer fixed, MS Paint's save
         RAN -- the log shows the whole .BMP being written: six `AH=40h` writes of
         0xF000 and 0xD7A0 bytes, a seek to end reporting **0x004AE7D6**
         (4,908,502 = 1680x974x3 + headers), a seek back to 0, then 14 bytes and
         40 bytes (the BITMAPFILEHEADER and BITMAPINFOHEADER) and a close.
         **It just wrote it in the wrong directory** -- `C:\Documents and
         Settings\Matthew\TEST.BMP`, one level above the Desktop the user chose.

         Three calls of this id explain it, and the third names itself:

           FUNC=0x82  arg = "C:\WINDOWS"
           FUNC=0x82  arg = "C:\DOCUME~1\Matthew\Desktop"
             -> UNIMPLEMENTED, STEPPED OVER ... ANSWERED 0

       ⚠⚠ AND THE SENTINEL IS "SUCCESS" HERE, WHICH IS WHY IT WAS SILENT. Only
         DX=0xFFFF is an error to krnl386; our 0 is success, so it tells the
         application the directory changed. It never did, so the subsequent
         create resolved against the old current directory. **A stepped-over call
         that answers "yes" is worse than one that answers "no".** Same family as
         [[stepped-over-call-answers-at-random]].
       ⚠ IT IS NOT DECLINABLE, and this file already said so before the bug:
         0x82 is among the three sites where 0xFFFF is a plain ERROR krnl386
         reports to the app rather than chaining to DOS.
         Declining was tried anyway in session 47 and moved the fault rather than
         fixing it. So it is PERFORMED here.
       ★ And the host's own current directory is the right place to perform it:
         our DOS layer resolves relative paths against it (`SetCurrentDirectoryA
         (g_cur)` in main.c), so one CWD serves the DOS side and this.
       ⚠ A genuine failure returns 0xFFFFFFFF -- which at THIS site means "error",
         and an error is the true answer when the directory does not exist. */
    case WOW32_SETCURRENTDIR: {
        char dir[MAX_PATH];
        if (!wow32_argstr(f, 0, dir, sizeof dir) || !dir[0]) {
            wow32_setret(f, 0xFFFFFFFFu);
            return 1;
        }
        if (SetCurrentDirectoryA(dir)) {
            wow32_curdir_set(dir);              /* #164: per task, see main.c */
            wow32_setret(f, 0);
        } else wow32_setret(f, 0xFFFFFFFFu);
        return 1;
    }

    /* ── 0x88 GetDriveType(nDrive) ────────────────────────────────────────
         Named by krnl386's export table, and the Win16 GetDriveType it backs
         uses the same codes as Win32 (documented: 2 = DRIVE_REMOVABLE).
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
    /* ── ★★★★★ 0x86 GetDateTime -- AND IT IS WHY THE CLOCK'S FACE IS BLANK.
         (session 54) ──────────────────────────────────────────────────────────
         CLOCK.EXE asks DOS for the date the ordinary way -- `INT 21h AH=2Ah` in
         protected mode. krnl386 owns that vector inside a WOW VDM (there is no DOS
         underneath to answer it), and its handler thunks straight out to here. We
         did not implement it, so it was STEPPED OVER and answered 0 -- and CLOCK
         got no date, drew nothing, invalidated its window and asked again:
         **10,317 times in a fourteen-second run.**

       ⚠⚠ AND IT CORRECTS A RECORDED CONCLUSION. The standing note said Clock's
         time "comes from krnl386's own 16-bit code, so there is no thunk to
         observe", and that both obvious hypotheses were dead (it never calls
         GetCurrentTime; the BDA tick is correct). The first two were right. The
         third was an inference, not a measurement -- there IS a thunk, this is it,
         and one grep of the run for UNIMPLEMENTED found it in a minute.

       ★ THE SIGNATURE BUG SHAPE, AGAIN: a stepped-over call whose sentinel answer
         is read as data. Fifth instance in this project. When something is blank,
         wrong or grey and the log shows no error, grep the run for the stepped-over
         lines and ask what each answer is used for.

       ⚠ THE PACKING IS THE FAT/MS-DOS ONE, and it is a JUDGEMENT, not a measurement:
         date in the HIGH word, time in the LOW -- the order a DOS directory entry
         stores them in, so a little-endian DWORD read gives exactly this.
             date: bits 15-9 year-1980, 8-5 month, 4-0 day
             time: bits 15-11 hour, 10-5 minute, 4-0 seconds/2
         If that is wrong the clock shows a WRONG time rather than no time, which is
         a different and much louder symptom than the one being fixed -- so the next
         run distinguishes them without any further instrumentation. */
    /* ── ⛔ s88: THAT JUDGEMENT WAS WRONG, AND THE CLOCK SAID SO: "?5/00/2074". ──
         The packing is pinned by how krnl386 hands it on in INT 21h AH=2Ah's own
         registers: CX = the high word (the YEAR, in full), DL = bits 15-8
         (the DAY), DH = bits 7-4 (the MONTH), AL = bits 3-0 (the DAY OF THE WEEK).
         So DX:AX = year : (day << 8 | month << 4 | weekday) -- AH=2Ah's own
         registers, folded into one DWORD. Not a time at all: Clock's TIME comes from
         INT 21h AH=2Ch, which krnl386 passes down to DOS. */
    case WOW32_GETDATETIME: {
        SYSTEMTIME st;
        GetLocalTime(&st);
        wow32_setret(f, ((DWORD)st.wYear << 16)
                      | ((DWORD)(st.wDay & 0xFF) << 8)
                      | ((DWORD)(st.wMonth & 0x0F) << 4)
                      | (DWORD)(st.wDayOfWeek & 0x0F));
        return 1;
    }

    case WOW32_GETDRIVETYPE: {
        WORD n = wow32_argw(f, 0);
        char root[4];
        if (n > 25) { wow32_setret(f, 1 /* DRIVE_NO_ROOT_DIR */); return 1; }
        root[0] = (char)('A' + n); root[1] = ':'; root[2] = '\\'; root[3] = 0;
        wow32_setret(f, (DWORD)GetDriveTypeA(root));
        return 1;
    }

    /* ── s92 (#298): THE INTERNAL IDS, NAMED FROM WHEN THEY ARRIVE. No export maps to
         these; krnl386 calls them from its own code and they were stepped over with
         the sentinel. Each is answered on purpose now -- the evidence is in the
         #298 issue thread; the two that CHANGE behaviour are marked ★. */

    /* ★ 0x87: INT 21h AX=4408h -- "is drive BL removable?" -- arrives when a Win16
         guest issues that call in protected mode, with (AX, BX). As the guest sees it:
         DX = FFFFh is an ERROR (CF set, AX = the code), anything else clears CF with
         AX = the answer. Stepped over it answered DX:AX = 0 -- "C: is REMOVABLE" -- to
         every caller. Same rule as our DOS layer's 44/08 (dos_int21.c): 0 removable (a
         CD too), 1 fixed, 0Fh invalid drive; BL 0 = the default drive. */
    case 0x87: {
        BYTE drv = (BYTE)(wow32_argw(f, 0) & 0xFF);
        UINT ty = 0;
        char root[4];
        if (!drv) {
            char cw[MAX_PATH];
            drv = (GetCurrentDirectoryA(sizeof cw, cw) && cw[0] >= 'A' && cw[1] == ':')
                ? (BYTE)((cw[0] | 0x20) - 'a' + 1) : 3;
        }
        if (drv >= 1 && drv <= 26 && (GetLogicalDrives() & (1u << (drv - 1)))) {
            root[0] = (char)('A' + drv - 1); root[1] = ':'; root[2] = '\\'; root[3] = 0;
            ty = GetDriveTypeA(root);
        }
        if (!ty || ty == DRIVE_NO_ROOT_DIR) wow32_setret(f, 0xFFFF000Fu);
        else wow32_setret(f, (ty == DRIVE_REMOVABLE || ty == DRIVE_CDROM) ? 0u : 1u);
        return 1;
    }
    /* 0xc6: arrives during GlobalFree of certain blocks (ones the 32-bit side is
         taken to own). Non-zero makes krnl386 go on and free the block; zero makes
         GlobalFree report success -- and the block is kept.
       ⛔ MEASURED s92: answering 1 (free it) killed EVERY Win16 launch -- krnl386 freed
         block 0x336 while loading KEYBOARD.DRV and then reported "Missing 16-bit
         system module: KEYBOARD.DRV" and shut the VDM down (runs/s92/gate1). So these
         blocks are the 32-bit side's to keep, and 0 -- "handled, do not free" -- is
         the answer, as the sentinel always (accidentally) gave. */
    case 0xc6:
        wow32_setret(f, 0);
        return 1;
    /* 0x8a: a new task's compatibility flags (argument: the new TDB; the answer is
         what GetAppCompatFlags then returns). No application is on a list here: 0. */
    case 0x8a:
    /* 0x2f: arrives from GetModuleHandle when krnl386 does not know the name
         (argument: the name). This host has no module krnl386 does not know: 0. */
    case 0x2f:
    /* 0xbe: WOWGetTableOffsets -- a table of 15 per-module id bases. ⚠ MUST STAY
         ZERO-FILLED: USER's and GDI's id spaces here are decoded with the bases at 0
         ("a DIFFERENT id space"); real offsets would shift every id. */
    case 0xbe:
    /* 0xc0: arrives once at boot, carrying pointers into krnl386's data; no use of
         the result has been observed. */
    case 0xc0:
    /* 0x9d WowFailedExec: WOWEXEC after every exec attempt; the result is ignored. */
    case WOW32_WOWFAILEDEXEC:
    /* 0x8b WowRegisterShellWindowHandle(hwnd, &wCmdShow, hmod): WOWEXEC reads > 0 as
         "a SHARED WOW" and 0 as "a separate one" -- and a separate WOW exits with its
         last application (it then calls WowSetExitOnLastApp(1)). One host per launch
         IS a separate WOW, so 0 is the faithful answer (#280 would change it). */
    case WOW32_WOWREGISTERSHELLWINDOW:
        wow32_setret(f, 0);
        return 1;

    /* ── ★ 0x7d: approve the selector about to become a TASK DATABASE ──────
         Not named by the export table, so it comes from when it arrives: during
         task creation, with one argument -- the selector of a freshly allocated
         0x320-byte block, which then becomes the new task's database (it is
         stamped "TD", the TDB signature). `0x320` is exactly limit+1 of every task
         database in the stock panel (session-37), including the two of a live
         stock WOW session.

       ★ THE RETRY IS WHAT PINS THE SEMANTICS. Answered `0`, the guest asks again
         with a DIFFERENT selector for the SAME bytes (same base and limit -- an
         alias), so the only thing that differs between one
         attempt and the next is the NUMERIC VALUE OF THE SELECTOR. The question
         can only be "may this selector value be a task handle?".

       ★ AND THE ANSWER IS THE SELECTOR, NOT A BOOLEAN: the value returned is the
         selector the new TDB is then written through. A
         32-bit companion cannot conjure an LDT selector, so the only value it
         can return is one it was offered: ECHO THE ARGUMENT. A bare `1` -- what
         the `wow32ret.txt` experiment answered to get WOWEXEC.EXE running -- is
         right only by luck; it would install `0x0001` as a task's selector.

       ★ ACCEPTING IS THE RIGHT ANSWER FOR THIS HOST, and that is a reading, not
         a shrug: whatever the real 32-bit side checks against, we keep no
         parallel handle table, the LDT krnl386 allocates from is the one we gave
         it, and each TDB gets a distinct selector by construction. Nothing here
         could make one selector unacceptable and the next one acceptable.
       ⚠ If a task ever does need rejecting, this is where it goes -- and the
         guest's retry is a trap: every rejection costs two bytes of krnl386's
         stack that are never given back; 1884 of them is the #SS that led here
         in the first place. */
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

       ★ AND `0` IS A HARD ERROR, NOT "NOTHING TO DO". WOWEXEC distinguishes:
             ret == 0                  -> an error box
             ret != 0, cbCmdLine == 0  -> nothing visible; it carries on
         So the sentinel was making WOWEXEC report a failure that had not
         happened. "No command" is `1` with a zero length, and it is silent.

       ── THE STRUCTURE, AS WOWEXEC PASSES IT AND USES OUR ANSWER ────────────────
         The one argument is a 16:16 pointer to 0x20 bytes on WOWEXEC's stack.
         Every field below was seen either filled in on arrival (a capacity, a
         buffer pointer) or carried by our answer into what WOWEXEC does next
         (LoadModule's arguments, SetCurrentDirectory's argument) -- there is no
         field here that was not observed being used.

           +0x00 DWORD lpCmdLine   -> a 0x10d-byte buffer
           +0x04 DWORD lpAppName   -> a 0x10d-byte buffer
           +0x08 DWORD lpEnv       -> a GlobalAlloc'd, locked block
           +0x0c WORD  (zero on arrival; no use seen)
           +0x0e WORD  (zero on arrival; no use seen)
           +0x10 WORD  cbCmdLine   in 0x10d -- ★ OUT MUST BE NON-ZERO OR NO LAUNCH
           +0x12 WORD  cbAppName   in 0x10d
           +0x14 WORD  cbEnv       in 0x1000 -- in/out, see the retry note below
           +0x16 WORD  CurDrive    0-based (0 = A:)
           +0x18 DWORD lpBufC      -> a third 0x10d-byte buffer
           +0x1c WORD  cbBufC      in 0x10d
           +0x1e WORD  nCmdShow    -> becomes lpCmdShow[1] of the LOADPARMS

       ★ +0x04 IS THE MODULE NAME, AND THAT IS MEASURED, NOT ASSUMED: it is the
         lpModuleName WOWEXEC passes to Win16 `LoadModule(lpModuleName,
         lpParameterBlock)` next. The parameter block is the standard LOADPARMS,
         and it is what identifies the other fields: `wEnvSeg` is the SEGMENT half
         of lpEnv (+0x0a), and `nCmdShow` comes from +0x1e.

       ⚠⚠ THE COMMAND LINE IS A PASCAL TAIL, AND THE `-2` IS THE WHOLE PUZZLE.
         The DOS command tail the launched program receives has a count byte of
         (length of our lpCmdLine string - 2), followed by our string. So the
         delivered string must be exactly two bytes longer than the tail text it
         represents, and the only shape that makes every case come out right is
                            <tail text> CR LF
         Check it: with no arguments the text is empty, we deliver "\r\n",
         its length is 2, the count byte is 0, and the tail reads <0><CR><LF> --
         a correct empty tail. With text " FOO" we deliver " FOO\r\n", the count
         is 4, and the tail is <4>' ''F''O''O'<CR><LF>. Both the count and the
         terminator land where DOS expects them.
       ⚠ Deliver an EMPTY string and `0 - 2` makes the count byte 0xFE, and the
         program reads 254 bytes of somebody's stack as its arguments. The two
         trailing bytes are load-bearing.

       ⚠ cbEnv (+0x14) IS DELIBERATELY NOT WRITTEN. It is an in/out "the buffer
         was too small" field -- if the callee returns a LARGER value, the caller
         reallocates and asks again (the documented shape of such fields). Leaving
         it alone is the only value that cannot start that loop. WOWEXEC supplies
         the environment itself (its own); we write a valid empty block so the
         buffer is never uninitialised, and no more.

       ★ DELIVER ONCE. In a shared WOW, WOWEXEC keeps asking for the next command
         (that is what the call is FOR), so a host that answered every time would
         relaunch the program forever. WOWEXEC only runs as a shared WOW when
         WowRegisterShellWindowHandle succeeds, which it does not yet (see 0x8b) --
         so this guard is not load-bearing
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
        /* ── #164: THE THIRD BUFFER IS THE CURRENT DIRECTORY. (s85) ───────────────
             It was "uninitialised stack, passed on unconditionally" and we sent "".
             Passed on to WHAT is in the log: WOWEXEC's next call is 0x82
             SetCurrentDirectory with this buffer, which failed on the empty string,
             so WOWEXEC stayed in C:\WINDOWS for LoadModule and the new task inherited
             it -- a relative CreateFile landed in C:\WINDOWS. Stock (w_cwd under
             stock.sh, the same rig) gives the task the folder it was launched from,
             which is CSRSS's cur= for the launch. So that is what goes here. */
        if (g_wow_cmd_dir[0]) {
            if (wow32_farput(f, ci, WOWCMD_LPBUFC, WOWCMD_CBBUFC, g_wow_cmd_dir) < 0)
                wow32_pokew(ci + WOWCMD_CBBUFC, 0);
        } else {
            volatile BYTE *c = wow32_farat(f, ci, WOWCMD_LPBUFC);
            if (c) c[0] = 0;
            wow32_pokew(ci + WOWCMD_CBBUFC, 0);
        }

        /* Drive letter of the program's own path, 0-based (0 = A:). Default to C: when the path is
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
         The argument is a 16:16 pointer to the structure whose address the host
         publishes at SysVars+0x6A (DOS_WOW_* in dos_layout.h) -- the same table
         krnl386 takes its far pointers into DOS's data from.
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
