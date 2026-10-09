/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * The 32-bit half of WOW: krnl386's calls out to Win32.  GH #128.
 *
 * krnl386.exe is a 16-bit DLL that cannot call Win32, so it reaches a 32-bit
 * companion (real Windows: wow32.dll inside ntvdm.exe) through a native BOP.
 * Every call arrives at the same BOP with a function ID on the stack, so the whole
 * interface is a small integer namespace -- 82 function IDs, enumerated in
 * docs/research/wow32-call-surface.md.
 *
 * THE FRAME, AS MEASURED AT THE BOP ON THE LIVE RIG (`@ss:sp` dumps).
 *
 * Relative to the BP the 16-bit side has set up when the BOP executes:
 *
 *   [bp+0]      saved BP
 *   [bp+2/+4]   a near return and a CS -- the per-function stub's
 *   [bp+6]      THE FUNCTION ID
 *   [bp+8]      a zero word
 *   [bp+10]     the ARGUMENT BYTE COUNT
 *   [bp+12/+14] the CALLER's return address -- offset then CS
 *   [bp+16...]  THE ARGUMENTS
 *
 * [CAUTION]: ARGUMENTS START AT bp+16, NOT bp+12. The first cut of the host's trace read
 * them at bp+12 and so printed the caller's far return address as the first two
 * argument words -- which is exactly why session 30 recorded VirtualAlloc's
 * argument ORDER as "not pinned down, two readings possible". It was an
 * instrument that lied, in this project's usual shape. The check that it is +16:
 * the call returns to the address at bp+12/+14 with exactly the declared
 * argument byte count removed from the stack, so the arguments are the N bytes
 * above the far return address -- and with +16 every known API's arguments
 * decode to sensible values (see ARGUMENT ORDER below).
 *
 * [INFO]: AND THE RETURN VALUE IS NOT A REGISTER. Whatever we leave in AX/DX is
 * overwritten when the guest resumes; the DWORD the caller receives is the one
 * in a four-byte stack slot at [bp-16] (low word) and [bp-14] (high). Confirmed
 * on hardware: in the rig's `@ss:sp` dump those two words held stale stack
 * (0x0047, 0x0000) at the BOP -- an uninitialised return slot.
 * Getting this wrong is silent: the guest reads garbage and blames itself.
 *
 * ARGUMENT ORDER IS PASCAL: pushed LEFT TO RIGHT, so the FIRST declared argument
 * is at the HIGHEST address and the LAST is at bp+16. Three independent calls,
 * as they arrive at run time, agree, which is what makes it a fact:
 * VirtualAlloc   arrives with 0, size, 0x3000, 0x40 -> (lpAddress, dwSize,
 *                flAllocationType, flProtect), and 0x3000/0x40 are exactly
 *                MEM_COMMIT|MEM_RESERVE and PAGE_EXECUTE_READWRITE.
 * VirtualFree    arrives with addr, size, 0x8000    -> (lpAddress, dwSize,
 *                dwFreeType) with MEM_RELEASE.
 * GlobalMemoryStatus arrives with a far pointer to a 32-byte stack buffer whose
 *                first DWORD is 0x20 -- a MEMORYSTATUS with dwLength filled in.
 *
 * A far pointer argument is a normal 16:16: offset in the low word, SELECTOR in
 * the high word (the caller pushes the segment first, the offset second).
 *
 * NAMING. 28 of the 82 IDs are named by krnl386's OWN export table -- an entry
 * whose target IS a stub, so the export's name in the (non-)resident name table
 * is the function's name, with no inference at all. `tools/ne/wowmap.py` prints
 * the mapping. It was cross-checked before being trusted: id 0xcf (no arguments,
 * and the guest's behaviour changes with the LANGID returned) was guessed
 * independently, and the export table then said GETSYSTEMDEFAULTLANGID. Two
 * methods, one answer.
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#ifndef NTVDMEX_WOW32_H
#define NTVDMEX_WOW32_H

#include <windows.h>
/* For DOS_CURRENT_DRIVE -- the WOW32 select-drive thunk and INT 21h AH=19h
 * must answer the same thing; see the note on the constant.
 */
#include "dos_layout.h"

/* Where each field sits relative to the thunk's BP. See the frame diagram above. */
#define WOW32_OFF_ID            6
#define WOW32_OFF_ARGB          10
#define WOW32_OFF_FROM          12      /* Return address into krnl386 -- WHICH call site */
#define WOW32_OFF_ARGS          16
#define WOW32_OFF_RETURN_STUB   2       /* [bp+2]: the return offset into the per-function stub */
#define WOW32_OFF_STUB_SEGMENT  4       /* [bp+4]: that stub's segment -- whose id space this is */
#define WOW32_OFF_RET           (-16)   /* The return slot: low word, then high */

/* THE SECOND RETURN CHANNEL: THE EPILOGUE MODE. (GH #128, session 38) (Importance = 3):
 * The return path the 16-bit side takes after the BOP is selected by a word on
 * the guest stack, at [bp-24], which the 32-bit side is expected to write. The
 * guest always arrives with it ZERO, and zero is the ordinary return. The
 * frame below BP at the BOP, as dumped on the rig:
 *
 *   bp-2 bx | -4 es | -6 cx | -8 fs | -10 gs | -12 ds | -16 the RETURN SLOT
 *   | -18 si | -20 di | -22 bp | -24 THE MODE (0 on arrival)
 *
 * [INFO]: MODE 25 IS THE TASK SWITCH-BACK, and it is a matched pair with the task
 * launch call `0x74`. That call is made on the NEW task's stack, with the
 * creating task's SS:SP carried in DI and CX (both saved in the frame above).
 * Returned through mode 25, the creator comes back on its own stack, with its
 * BP and its current-task word restored -- observed on the rig as krnl386's
 * creating task carrying on. So "this task's turn is over, put its creator
 * back" is one word.
 *
 * [CAUTION]: IT IS ONLY VALID AT THE FRAME THAT STARTED THE TASK. DI and CX are a stack
 * only in the `0x74` call; at any other call site they are just the caller's
 * registers, and mode 25 would load SS:SP from whatever they happened to hold.
 *
 * [INFO]: AND THE HOST NOW USES IT. src/wow/wowsched.h returns the `0x74` launch call
 * through mode 25, which is what lets krnl386's creating task carry on and
 * `LoadModule` finish. Opt-in; see that file for the two moments and the one
 * ordering that works.
 */
#define WOW32_OFF_MODE          (-24)
#define WOW32_MODE_ORDINARY     0
#define WOW32_MODE_SWITCHBACK   25

/* The BOP is `C4 C4 51`. Resuming the guest anywhere but past all three bytes
 * restarts it mid-instruction.
 */
#define WOW32_BOP               0x51    /* C4 C4 51: a krnl386 call out to WOW32 (3 bytes) */
#define WOW32_BOP_DISPATCH      0x53    /* C4 C4 53 sub (4 bytes) */
#define WOW32_DISPATCH_POINTER  0x03    /* 53/03: a far pointer to a 32-bit dispatch routine */
#define WOW32_TASK_LAUNCH       0x74    /* Named from its use: made on the new task's stack */

typedef DWORD (*PWOW32_SELECTOR_TO_LINEAR)(WORD selector, PVOID context);

/* What a service may ask the host to push for a 16-bit callback (see below). */
#define WOW32_CALLBACK_MAX_ARGUMENTS    6
#define WOW32_CALLBACK_BLOB_MAX         64

typedef struct _WOW32_FRAME
{
    volatile BYTE *FrameBase;                       /* linear address of SS:BP inside the thunk */
    WORD             Id;
    WORD             ArgumentBytes;
    WORD             CallSite;                        /* caller's return offset [bp+12] -- the CALL SITE */
    WORD             StubSegment;                     /* [bp+4]: the SEGMENT of the per-function stub */
    PWOW32_SELECTOR_TO_LINEAR SelectorToLinear;       /* selector -> linear base (host's LDT view) */
    PVOID Context;
    /* THE ID SPACE IS PER MODULE, AND THIS SAYS WHOSE. (session 38) (Importance = 3):
     * Every id in this file is one krnl386 sends. USER, GDI and the drivers send
     * their OWN ids, with their own numbering, to the same BOP -- so an id is
     * only meaningful together with the module it came from, and the module is
     * named by `StubSegment`.
     *
     * [CAUTION]: MEASURED, AFTER GETTING IT WRONG. WOWEXEC's `RegisterClass(&WNDCLASS)`
     * arrives as id `0x39` with `retstub=0x0c25` and **4** argument bytes, from
     * USER's segment. krnl386's `0x39` is `GetProfileInt`, `retstub=0xb537`, **10**
     * argument bytes. We serviced the first with `GetProfileIntA` and handed
     * WOWEXEC the answer -- a function answered by an unrelated function, which is
     * the "runs but lies" class this project treats as the most expensive kind.
     *
     * 1 only when the stub's segment ([bp+4]) is the segment the BOP is executing
     * in -- krnl386's first code segment, the id space this file describes.
     * krnl386 also reaches the BOP from a SECOND code segment of its own, with a
     * numbering that is also not this one, so "not ours" is about the SEGMENT,
     * not about the module.
     * Everything here is gated on it; anything else gets the honest
     * "unimplemented", which is a missing answer instead of a wrong one.
     */
    INT              IsKernel;
    /* Filled in by the host so a service can talk back about what it did. */
    DWORD            Result;
    INT              IsServiced;
    /* A 16-BIT CALL THE SERVICE WANTS MADE. (GH #128, session 40) (Importance = 3):
     * A service cannot make one itself: entering guest code means replacing the
     * whole guest context, which only the BOP handler is in a position to do
     * and undo. So a service ASKS, by filling these in, and the handler acts on
     * the request after the service has returned and its answer is already in
     * the return hole. See src/wow/wowcall.h.
     *
     * [CAUTION]: `IsCallbackAllowed` is the host's permission, not the service's opinion: the
     * machinery is opt-in (wowcall.txt), and a service that requested a callback the host will not
     * make must not then describe one in its log note.
     */
    INT              IsCallbackAllowed;               /* 1 = the host can call 16-bit code now */
    DWORD            CallbackProcedure;               /* 16:16 procedure to call; 0 = none asked */
    WORD             CallbackDataSelector;            /* the DS it must be entered with */
    WORD             CallbackArguments[WOW32_CALLBACK_MAX_ARGUMENTS];            /* words to push, in DECLARED order */
    INT              CallbackArgumentCount;
    INT              CallbackReturnMode;              /* WOWCALL_RET_KEEP / _RESULT -- whose
                                        answer the caller's return value is    */
    PWORD CallbackSink;                    /* optional: where the host keeps the answer */
    INT              CallbackAction;                  /* WOWCALL_ACT_* -- what to DO with it */
    WORD             CallbackActionArgument;          /* what that action is about */
    WORD             CallbackWindow, CallbackMessage; /* for the log; 0/0 when not a message */
    /* A STRUCTURE TO PUT WHERE THE GUEST CAN REACH IT (Importance = 3):
     * Some messages carry a POINTER, not a value -- WM_CREATE's lParam is an
     * LPCREATESTRUCT -- and a 16-bit program can only follow a pointer that
     * lives behind a selector it already has. The host has no 16-bit heap of
     * its own, so the structure is placed on the GUEST'S OWN STACK just below
     * the arguments, which is where real USER puts it and which needs no
     * allocator: the stack selector is already valid for the guest, and the
     * bytes die with the call, which is exactly their lifetime.
     * `CallbackBlobArgument` is the index in CallbackArguments[] of the HIGH word of the far pointer
     * that should be made to point at it -- filled in by WowCallEnter, which
     * is the first code that knows what SS:SP will be. -1 = no blob.
     */
    BYTE             CallbackBlob[WOW32_CALLBACK_BLOB_MAX];
    INT              CallbackBlobLength;              /* bytes of CallbackBlob to place; 0 = none */
    INT              CallbackBlobArgument;            /* CallbackArguments[] index to receive SEG:OFF, or -1 */
    /* "DO NOT RETURN TO THE CALLER AT ALL." (session 57) (Importance = 3):
     * Every other field here describes a call to make BEFORE resuming the
     * guest; this one says the guest must not be resumed past its BOP yet,
     * because the service it asked for -- DialogBox -- is defined as not
     * returning until a dialog is dismissed. The host runs the modal loop
     * instead and completes this call much later, out of the same BOP handler.
     *
     * [CAUTION]: THE SERVICE STILL WRITES A RETURN VALUE, and it is deliberately the one
     * session 56 wrote: if the host declines to run the loop (callbacks off,
     * stack full) the guest gets the old behaviour rather than a hole nobody
     * filled. See src/wow/wowdlg.h.
     */
    INT              IsModalDialog;                   /* 1 = a modal dialog was parked by this call */
    /* 1 = an ENUMERATION was armed (wowenum.h) and its first call is owed. Same
     * "do not simply resume the guest" meaning as `IsModalDialog`, different
     * continuation; a service sets exactly one of them.
     */
    INT              IsEnumerationRequested;
    /* [INFO]: THE GUEST'S OWN DS AT THE BOP. A service that has to call back into
     * application code with no class or window to take an instance from --
     * LineDDA is the first -- needs the data segment the guest is actually
     * running on. Filled by the handler because only it can see the TIB.
     */
    WORD             GuestDataSelector;
} WOW32_FRAME, *PWOW32_FRAME;
typedef const WOW32_FRAME *PCWOW32_FRAME;

/* Hex widths for WowNoteHex: a byte, a word, a dword. */
#define WOW_HEX_BYTE_DIGITS                         2
#define WOW_HEX_WORD_DIGITS                         4
#define WOW_HEX_DWORD_DIGITS                        8

/* Taking WORDs and DWORDs apart, and putting them back, as every Win16 structure needs. */
#define WOW_WORD_BYTES                              2

/* ---- frame accessors ---------------------------------------------------- */

/* ---- writing back through a far pointer the guest gave us --------------- */

/* WHAT AN UNIMPLEMENTED CALL ANSWERS. (GH #128, session 36) (Importance = 2):
 * Session 35 measured that a stepped-over call is not inert: krnl386 takes the
 * return slot as the answer and acts on it. Leaving the slot unwritten
 * therefore does not mean "no answer", it means "an answer drawn from whatever
 * the stack last held" -- which made two separate runs stop for reasons that
 * were OURS, and which no amount of re-running can reproduce or rule out.
 *
 * [INFO]: SO ANSWER THE SAME WAY EVERY TIME. This does not make the answer TRUE -- it is
 * still a call we have not implemented, and the log says so on every line. It
 * makes the run REPRODUCIBLE, which is the property every other conclusion in
 * this investigation rests on. A deterministic wrong answer can be traced from
 * the wall back to its cause; a random one cannot.
 *
 * [INFO]: AND ZERO IS THE BETTER CONSTANT, at both calls measured so far -- chosen from
 * what the guest visibly did with each answer, not from taste:
 *   0xc6  litter 0x01b7 sent the guest down a path we read as FAILURE. Zero
 *         does not.
 *         [CAUTION]: s92: non-zero actually makes the guest FREE the block, not fail --
 *         but zero is still right: answering 1 broke every launch (see 0xc6's
 *         own case).
 *   0x2d  litter 0x2714 was accepted as a MODULE HANDLE (Win16 treats < 0x21
 *         as an error code) and ran on into the terminal #GP with a NULL
 *         parameter block. Zero is below 0x21, so LoadModule takes its error
 *         path and REPORTS, instead of faulting somewhere else.
 * In both cases zero fails nearer the cause, which is the whole point.
 *
 * [CAUTION]: NOT 0xFFFFFFFF: that is WOW32_DECLINE, and a decline is a different statement
 * ("ask real DOS instead") that only holds at the sites in g_Wow32DeclineSites.
 * Reusing it here would make every unimplemented call claim to be one.
 */
#define WOW32_UNIMPL_RET                            0u

/* ---- the function IDs we can name -------------------------------------- */
/* Names for the 28 that krnl386's export table names outright, plus the ones
 * worked out from their call sites. An ID with no name here is not a gap in the
 * evidence -- it is a function reached only from internal code, not yet pinned
 * down by what it is called with and what the guest does with the answer.
 */
#define WOW32_FATALEXIT                             0x01
#define WOW32_EXITKERNELTHUNK                       0x02
#define WOW32_WRITEOUTPROFILES                      0x03
#define WOW32_GETVDMPOINTER32W                      0x1a
#define WOW32_CALLPROCEX32W                         0x1c
#define WOW32_YIELD                                 0x1d
#define WOW32_WAITEVENT                             0x1e
#define WOW32_POSTEVENT                             0x1f
#define WOW32_SETPRIORITY                           0x20
#define WOW32_LOCKCURRENTTASK                       0x21
#define WOW32_WOWLOADMODULE                         0x2d
#define WOW32_SETCURRENTDRIVE                       0xc8            /* Named from its use, see below */
#define WOW32_GETPROFILEINT                         0x39            /* Pinned from its strings, below */
#define WOW32_GETPROFILESTRING                      0x3a            /* IT DECIDES PAINT'S COLOUR MODE */
#define WOW32_WRITEPROFILESTRING                    0x3b            /* #293, the inventory's top gap */
#define WOW32_WRITEPRIVATEPROFILESTRING             0x81
#define WOW32_WOWGETNEXTVDMCOMMAND                  0x70
#define WOW32_OLDYIELD                              0x75
#define WOW32_REGISTERDOSDATA                       0x78            /* Named from its use, below */
#define WOW32_GETSHORTPATHNAME                      0x7b
#define WOW32_SETCURRENTDIR                         0x82            /* WHERE File > Save As PUT THE FILE */
#define WOW32_ACCEPTTASKSELECTOR                    0x7d            /* Pinned from its use, below */
#define WOW32_GETPRIVATEPROFILESTRING               0x80            /* Pinned from its strings, below */

/* 0x7f GetPrivateProfileInt -- NAMED BY MINESWEEPER'S FIRST RUN (Importance = 3):
 * 14 arg bytes = 4 + 4 + 2 + 4, and the fourth is a FILENAME, so it is the
 * private twin of 0x39 exactly as 0x80 is of the string form. WINMINE.EXE
 * asks it for "Height", "Width", "Mines", "Difficulty", "Xpos" and "Ypos" out
 * of `winmine.ini`, 37 times in one startup.
 *
 * [INFO]: AND UNIMPLEMENTED IT ANSWERED 0, WHICH THE CALL SITE READS AS A REAL
 * ANSWER -- the fifth time this exact shape has cost this project a session.
 * `nDefault` is right there in the arguments (8, for "Height") and we were
 * throwing it away, so Minesweeper laid itself out with every stored value 0
 * and created its window at (-2,-48): its caption and menu bar OFF THE TOP OF
 * THE SCREEN. Nothing looked broken in the log.
 */
#define WOW32_GETPRIVATEPROFILEINT                  0x7f
#define WOW32_GETPRIVATEPROFILEINT_ARG_FILE         0               /* Far */
#define WOW32_GETPRIVATEPROFILEINT_ARG_DEFAULT      4
#define WOW32_GETPRIVATEPROFILEINT_ARG_KEY          6               /* Far */
#define WOW32_GETPRIVATEPROFILEINT_ARG_APP          10              /* Far */
#define WOW32_WOWWAITFORMSGANDEVENT                 0x83
#define WOW32_WOWMSGBOX                             0x84
#define WOW32_GETDATETIME                           0x86            /* Answer is used as a packed date */
#define WOW32_GETDRIVETYPE                          0x88
#define WOW32_WOWREGISTERSHELLWINDOW                0x8b
#define WOW32_FREELIBRARY32W                        0x8c
#define WOW32_GETPROCADDRESS32W                     0x8d
#define WOW32_DIRECTEDYIELD                         0x96
#define WOW32_LOADLIBRARYEX32W                      0x9a
#define WOW32_WOWQUERYPERFCOUNTER                   0x9b
#define WOW32_WOWCURSORICONOP                       0x9c
#define WOW32_WOWFAILEDEXEC                         0x9d
#define WOW32_WOWCLOSECOMPORT                       0x9f
#define WOW32_VIRTUALALLOC                          0xb8
#define WOW32_VIRTUALFREE                           0xb9
#define WOW32_GLOBALMEMORYSTATUS                    0xbc
#define WOW32_WOWKILLREMOTETASK                     0xbf
#define WOW32_MESSAGEBOX                            0xc4            /* The fatal-error box; see wowmap */

/* 0xc5: RESOLVE A MODULE NAME TO A FULL PATH. (session 39) (Importance = 3):
 * Serviced in main.c, not here: the answer is a 16:16 far pointer, so it needs
 * guest-visible memory and a selector, and both live over there.
 *
 * [INFO]: NAMED BY HOW IT ARRIVES, in pairs, and the second call is what makes it
 * unambiguous:
 *   0xc5(dst, src)    -- resolve; a zero answer makes the guest fall back to
 *                        the name it started with
 *   0xc5(dst, NULL)   -- release, made only after a successful resolve, and
 *                        its result ignored
 * so the pair is resolve/release and the host owns the storage between them.
 *
 * [INFO]: AND `dst` RECEIVES A FAR POINTER, NOT A COPIED STRING: on success the guest
 * goes on to use the far pointer stored at `dst` exactly where, on failure, it
 * uses the far pointer to the name it was given. One is the resolved path, the
 * other is the original name; they must be the same kind of thing.
 *
 * Answering 0 is what made krnl386 compose module names against the CURRENT
 * DIRECTORY and fail to open `C:\Documents and Settings\<user>\SHELL.DLL`.
 */
#define WOW32_RESOLVEMODULEPATH                     0xc5
#define WOW32_RESOLVEMODULEPATH_ARG_DESTINATION     0               /* Far */
#define WOW32_RESOLVEMODULEPATH_ARG_SOURCE          4               /* Far */

/* 0xd0: GetWindowsDirectory(lpBuffer, uSize). (session 40) (Importance = 2):
 * Not named by krnl386's export table, so it comes from how it is called and
 * from what the answer is USED for -- and the two agree.
 *
 * [INFO]: krnl386's own call says what SHAPE it is: 6 argument bytes, a far buffer
 * and 0x80 (lpBuffer, uSize=128); on a non-zero answer it takes the length of
 * the NUL-terminated string now in the buffer and keeps both. So it fills the
 * caller's buffer with a path and returns non-zero on success. That is a
 * Get<something>Directory and nothing else.
 *
 * [INFO]: SYSEDIT says WHICH directory, and it is a count rather than a guess:
 * `sysedit` imports `KERNEL.134 GETWINDOWSDIRECTORY` and NOT
 * `KERNEL.135 GETSYSTEMDIRECTORY` (`neimports.py`: exactly two fixups), and in
 * a run SYSEDIT's task makes exactly TWO calls to this id. It then `lstrcat`s `\SYSTEM.INI` and
 * `\WIN.INI` onto the answer, which is where those files live.
 *
 * [CAUTION]: WHAT LEAVING IT UNIMPLEMENTED LOOKED LIKE: not an error, but a WRONG NAME.
 * The buffer kept whatever was in it, so SYSEDIT opened -- and titled a
 * window -- `"REGISTERPENAPP\SYSTEM.INI"`, a path built from another module's
 * leftover string. The "runs but lies" class, and it took the callback work to
 * get far enough to see it.
 */
#define WOW32_GETWINDOWSDIRECTORY                   0xd0
#define WOW32_WOWSHUTDOWNTIMER                      0xcd

/* Serviced in main.c, not here: it needs the DOS machine. Listed so the name table
 * below can print it, and so nobody adds a decline for it -- krnl386 reports a
 * DX=0xFFFF answer to the app as a hard error, not as "ask DOS instead".
 */
#define WOW32_GETCURDIR                             0xc9
#define WOW32_GETCURDIR_ARG_BUFFER_OFFSET           0
#define WOW32_GETCURDIR_ARG_BUFFER_SELECTOR         2
#define WOW32_GETCURDIR_ARG_DRIVE                   4
#define WOW32_GETCURDIR_BUFFER_SIZE                 68
#define WOW32_GETSYSTEMDEFAULTLANGID                0xcf

/* Each service's argument block, reversed as always (offset 0 = the LAST parameter). */
#define WOW32_LOADLIBRARYEX32W_ARG_FLAGS            0
#define WOW32_LOADLIBRARYEX32W_ARG_PATH             8
#define WOW32_FREELIBRARY32W_ARG_MODULE             0
#define WOW32_GETPROCADDRESS32W_ARG_MODULE          4
#define WOW32_GETPROCADDRESS32W_ARG_NAME            0
#define WOW32_GETVDMPOINTER32W_ARG_MODE             0
#define WOW32_GETVDMPOINTER32W_ARG_POINTER          2
#define WOW32_CALLPROCEX32W_ARG_COUNT               6
#define WOW32_CALLPROCEX32W_ARG_MASK                10
#define WOW32_CALLPROCEX32W_ARG_PROCEDURE           14
#define WOW32_CALLPROCEX32W_ARG_FIRST               18
#define WOW32_VIRTUALALLOC_ARG_ADDRESS              12
#define WOW32_VIRTUALALLOC_ARG_SIZE                 8
#define WOW32_VIRTUALALLOC_ARG_TYPE                 4
#define WOW32_VIRTUALALLOC_ARG_PROTECT              0
#define WOW32_VIRTUALFREE_ARG_ADDRESS               8
#define WOW32_VIRTUALFREE_ARG_SIZE                  4
#define WOW32_VIRTUALFREE_ARG_TYPE                  0
#define WOW32_GLOBALMEMORYSTATUS_ARG_BUFFER         0
#define WOW32_GETSHORTPATHNAME_ARG_BUFFER           2
#define WOW32_GETSHORTPATHNAME_ARG_CAPACITY         0
#define WOW32_GETSHORTPATHNAME_ARG_PATH             6
#define WOW32_GETWINDOWSDIRECTORY_ARG_BUFFER        2
#define WOW32_GETWINDOWSDIRECTORY_ARG_CAPACITY      0
#define WOW32_GETPRIVATEPROFILESTRING_ARG_BUFFER    6
#define WOW32_GETPRIVATEPROFILESTRING_ARG_SIZE      4
#define WOW32_GETPRIVATEPROFILESTRING_ARG_APP       18
#define WOW32_GETPRIVATEPROFILESTRING_ARG_KEY       14
#define WOW32_GETPRIVATEPROFILESTRING_ARG_DEFAULT   10
#define WOW32_GETPRIVATEPROFILESTRING_ARG_FILE      0
#define WOW32_WOWMSGBOX_ARG_TYPE                    0
#define WOW32_WOWMSGBOX_ARG_CAPTION                 4
#define WOW32_WOWMSGBOX_ARG_TEXT                    8
#define WOW32_GETPROFILEINT_ARG_APP                 6
#define WOW32_GETPROFILEINT_ARG_KEY                 2
#define WOW32_GETPROFILEINT_ARG_DEFAULT             0
#define WOW32_GETPROFILESTRING_ARG_BUFFER           2
#define WOW32_GETPROFILESTRING_ARG_SIZE             0
#define WOW32_GETPROFILESTRING_ARG_APP              14
#define WOW32_GETPROFILESTRING_ARG_KEY              10
#define WOW32_GETPROFILESTRING_ARG_DEFAULT          6
#define WOW32_WRITEPROFILESTRING_ARG_APP            8
#define WOW32_WRITEPROFILESTRING_ARG_KEY            4
#define WOW32_WRITEPROFILESTRING_ARG_VALUE          0
#define WOW32_WRITEPRIVATEPROFILESTRING_ARG_APP     12
#define WOW32_WRITEPRIVATEPROFILESTRING_ARG_KEY     8
#define WOW32_WRITEPRIVATEPROFILESTRING_ARG_VALUE   4
#define WOW32_WRITEPRIVATEPROFILESTRING_ARG_FILE    0
#define WOW32_SETCURRENTDIR_ARG_PATH                0
#define WOW32_GETDRIVETYPE_ARG_DRIVE                0
#define WOW32_ISDRIVEREMOVABLE_ARG_DRIVE            0
#define WOW32_ACCEPTTASKSELECTOR_ARG_SELECTOR       0
#define WOW32_WOWGETNEXTVDMCOMMAND_ARG_COMMANDINFO  0
#define WOW32_REGISTERDOSDATA_ARG_POINTER           0
#define WOW32_CALLPROCEX32W_ARG_SIZE                4               /* Each DWORD of the argument list */
#define WOW32_CALLPROCEX32W_EXTENDED                0x40000000u     /* krnl386's own mark: 518 */

/* The internal ids krnl386 sends from its own code (see the s92 #298 note in Wow32Call). */
#define WOW32_ISDRIVEREMOVABLE                      0x87            /* INT 21h AX=4408h in protected mode */
#define WOW32_GLOBALFREE_OWNED                      0xc6
#define WOW32_APPCOMPATFLAGS                        0x8a
#define WOW32_GETMODULEHANDLE_UNKNOWN               0x2f
#define WOW32_GETTABLEOFFSETS                       0xbe
#define WOW32_BOOTPOINTERS                          0xc0
#define WOW32_ISDRIVEREMOVABLE_INVALID              0xFFFF000Fu     /* DX = FFFFh: an error, AX = 0Fh invalid drive */

#define WOW32_RETURN_FAILED                         0xFFFFFFFFu
#define WOW32_PROC_NAME_MAX                         256
#define WOW32_PROFILE_NAME_MAX                      128
#define WOW32_PROFILE_VALUE_MAX                     512
#define WOW32_PROFILE_WRITE_NAME_MAX                256
#define WOW32_PROFILE_WRITE_VALUE_MAX               4096
#define WOW32_MSGBOX_TEXT_MAX                       512
#define WOW32_MSGBOX_LOG_MAX                        1200
#define WOW32_MSGBOX_LOG_LINE_END                   2
#define WOW32_MSGBOX_LOG_HEAD_ROOM                  40
#define WOW32_MSGBOX_LOG_TAIL_ROOM                  8
#define WOW32_MSGBOX_ICON_MASK                      0xF0u           /* MB_ICON* */
#define WOW32_DATE_YEAR_SHIFT                       16              /* GetDateTime: year, day, month, day of week */
#define WOW32_DATE_DAY_SHIFT                        8
#define WOW32_DATE_MONTH_SHIFT                      4
#define WOW32_DATE_NIBBLE_MASK                      0x0F
#define WOW32_ROOT_PATH_SIZE                        4               /* "A:\\" and its NUL */
#define WOW32_LAST_DRIVE                            25              /* Z:, as a 0-based drive */
#define WOW32_DRIVE_COUNT                           26
#define WOW32_DRIVE_C                               3               /* 1-based */
#define WOW32_LOWERCASE_BIT                         0x20
#define WOW32_LOWER_TO_UPPER                        32
#define WOW32_COMMAND_TAIL_ROOM                     3               /* The CR, the LF and the NUL */

/* What the host learned from a REGISTERDOSDATA call, for the log and for anyone
 * who later wants to reconcile krnl386's view of DOS with ours.
 */
typedef struct _WOW32_DOSDATA
{
    INT   IsSeen;
    DWORD FarPointer;                     /* the 16:16 the guest passed */
} WOW32_DOSDATA, *PWOW32_DOSDATA;

/* THE WIN16 PROGRAM THIS VDM EXISTS TO RUN:
 * Filled in by the host once it knows what it was launched for, and handed to
 * the guest by WOW32 0x70 (WowGetNextVDMCommand) -- see that case for the
 * structure and for why this is the call that launches an application.
 *
 * [CAUTION]: A WOW LAUNCH DOES NOT CARRY THE PROGRAM ON ITS COMMAND LINE. Windows starts
 * the VDM as `ntvdm -f -i1 -w -a <krnl386>` and the application is delivered
 * out of band; real ntvdm gets it from the Win32 GetNextVDMCommand, which
 * returns FALSE/0x57 for us (measured -- see docs/research/). On the rig it
 * comes from target.txt, which is the harness's channel for the same fact.
 *
 * [INFO]: EMPTY IS A LEGITIMATE STATE and it has a correct answer: "no command", which
 * is NOT the same as an error. See the 0x70 case.
 */
#define WOW32_COMMAND_PROGRAM_MAX               512
#define WOW32_COMMAND_ARGUMENTS_MAX             192
#define WOW32_SHORT_PATH_BUFFER                 (MAX_PATH + 16)

/* Field offsets of the command structure -- derived in the 0x70 case, which is
 * the only place they are used and the only place the derivation makes sense.
 */
#define WOWCMD_LPCMDLINE                        0x00
#define WOWCMD_LPAPPNAME                        0x04
#define WOWCMD_LPENV                            0x08
#define WOWCMD_CBCMDLINE                        0x10
#define WOWCMD_CBAPPNAME                        0x12
#define WOWCMD_CBENV                            0x14
#define WOWCMD_CURDRIVE                         0x16
#define WOWCMD_LPBUFC                           0x18
#define WOWCMD_CBBUFC                           0x1c
#define WOWCMD_NCMDSHOW                         0x1e

/* DECLINING IS A REAL ANSWER, AND krnl386 ALREADY HANDLES IT (Importance = 1):
 * krnl386 hooks INT 21h in protected mode and offers some functions to its
 * 32-bit companion first. When the companion answers 0xFFFF(FFFF), the
 * original INT 21h request then arrives at the PREVIOUS INT 21h handler --
 * observed in the run log as the matching `INT21h AH=..` line right after the
 * call. In this host that handler is our own DOS layer, the one COMMAND.COM and
 * Doom already use. So a sentinel return hands file I/O to working code instead
 * of to a parallel Win32 handle table that would then disagree with every call
 * that chains anyway.
 *
 * [CAUTION]: ONLY WHERE THE CALL SITE SAYS SO. There are sites (0x82, 0xc9, 0x71) where
 * 0xFFFF is a plain ERROR and krnl386 reports failure to the app rather than
 * chaining. Declining there would turn
 * "not implemented" into "the file does not exist" -- a wrong answer instead
 * of a missing one, which is the more expensive kind. They are NOT in the list
 * below, and the list is per call site, not a guess about the family.
 *
 * [CAUTION]: Some sites test AX and some test DX, so the sentinel has to be 0xFFFFFFFF
 * rather than either half.
 *
 * - THE HONEST TRADE. Real WOW routes these to Win32, so a Win16 app gets NT
 *   file semantics (sharing modes, long names). Declining gives it our DOS
 *   semantics instead. For loading and running a program that is the same thing,
 *   and it is one line to change later -- but it IS a difference, so it is
 *   written down rather than discovered.
 */
#define WOW32_DECLINE                           0xFFFFFFFFu

/* Verified declinable. All seven are the INT 21h file family; declining 0x97 and
 * 0x6f makes krnl386 re-issue a plain AH=3Fh / AH=40h to DOS, visible in the run
 * log as the INT 21h line that follows the call.
 */
#define WOW32_FILE_OPEN                         0xc1            /* AH=3Dh */
#define WOW32_FILE_READ                         0x97            /* -> AH=3Fh on decline */
#define WOW32_FILE_READ_ARG_COUNT               8               /* DWORD */
#define WOW32_FILE_READ_ARG_BUFFER_OFFSET       12
#define WOW32_FILE_READ_ARG_BUFFER_SELECTOR     14
#define WOW32_FILE_READ_ARG_HANDLE              16
#define WOW32_FILE_READ_FAILED_U                0xFFFFFFFFu     /* DX:AX: a real failure, never a short read */
#define WOW32_FILE_CLOSE                        0xc2            /* AH=3Eh */
#define WOW32_FILE_GETATTR                      0xc7            /* AH=43h AL=0 */
#define WOW32_FILE_7E                           0x7e            /*  */
#define WOW32_FILE_GETDATE                      0x89            /* AH=57h AL=0 */
#define WOW32_FILE_WRITE                        0x6f            /* -> AH=40h on decline */

/* THE SEEK, AND WHY IT WAS MISSED. (GH #128, session 34) (Importance = 1):
 * The first list of declinable sites was incomplete: 0x98 was not on it, and
 * sat in the "unimplemented, stepped over" list looking like work rather than
 * like a one-line answer.
 *
 * [INFO]: IT IS THE FILE SEEK, and leaving it unanswered is what produced "NTVDM
 * KERNEL: Missing 16-bit system module". krnl386 reads SYSTEM.DRV's first 0x40
 * bytes, calls 0x98 with offset 0x0400 -- which is that file's e_lfanew, checked
 * against the bytes on disk, not assumed -- and reads the NE header. Stepping the
 * seek over left the file position where the MZ read had left it, so every
 * subsequent read returned the wrong part of the file and the NE header it
 * parsed was whatever followed the MZ stub.
 * Declining restores the original AX (AH=42h) and chains to real DOS, which our
 * PM thunk already serves.
 */
#define WOW32_FILE_SEEK                         0x98            /* -> AH=42h on decline */

/* DECLINING IS A PROPERTY OF THE CALL SITE, NOT OF THE ID (Importance = 2):
 * This was keyed by ID, and that is measurably wrong: krnl386 calls 0x97 (read)
 * from TWO places with OPPOSITE meanings --
 *   one where 0xFFFF means "ask real DOS": an `INT21h AH=3F` follows the
 *     decline in the log. A decline is a true statement.
 *   one where 0xFFFF is RETURNED TO THE CALLER as a failure: nothing follows.
 *     A decline here turns "we did not implement this" into "the read failed",
 *     which is a WRONG ANSWER rather than a missing one.
 *
 * Session 34's failing run declined at `from=0x8a51` -- the second kind -- and the
 * log shows what that looks like: no `INT21h AH=3F` follows it, unlike every other
 * read in the run. 0x6f (write) has the same split.
 *
 * [CAUTION]: THE VALUES BELOW ARE krnl386 CALL-SITE RETURN OFFSETS -- what the frame carries
 * at WOW32_OFF_FROM, as seen in the run log's `from=` -- each one checked to be
 * followed by the chained INT 21h request when declined. They are specific to the
 * XP krnl386.exe build. Anything not listed is not declined -- an unknown site
 * gets the honest "unimplemented" rather than a guess.
 */
typedef struct _WOW32_DECLINE_SITE
{
    WORD Id;
    WORD CallSite;
} WOW32_DECLINE_SITE;

/* ---- the services ------------------------------------------------------- */
/* Returns 1 if this ID was serviced (the caller then advances EIP past the BOP),
 * 0 if it is still unimplemented (the caller logs and steps over).
 *
 * [CAUTION]: EVERY SERVICE MUST CALL Wow32SetReturn(), even a void one. The guest receives
 * the return slot unconditionally, so "no return value" still means "write zero"
 * -- otherwise the guest gets whatever was on the stack. GlobalMemoryStatus is
 * the void case and it still writes 0.
 */
/* GENERIC THUNKS (#5, s90): 16-bit code calling 32-bit DLLs directly (Importance = 2):
 * LoadLibraryEx32W / GetProcAddress32W / FreeLibrary32W / GetVDMPointer32W, and
 * CallProc32W / _CallProcEx32W -- the documented Win16 route to Win32 (the WOW
 * generic-thunk API), and the one XP's own 16-bit MMSYSTEM uses to reach WINMM.
 *
 * [INFO]: THE 32-BIT SIDE IS THIS PROCESS. Under NT the DLL is loaded into the NTVDM
 * process and called there; this host IS that process, so LoadLibraryExA and a
 * direct call are the faithful answer, not a shortcut.
 *
 * THE CallProc FRAME, AS IT ARRIVES (KERNEL ords 517/518 -> thunk id 0x1c):
 * Both arrive as id 0x1c with an argument byte count of 0 -- so the arguments lie
 * past the declared block and are read RAW (offsets from the argument block, as
 * measured with w_gthunk):
 *   +0 a saved bp   +2/+4 the app's far return   +6 cParams (DWORD)
 *   +10 fAddressConvert (DWORD)   +14 lpProcAddress (DWORD)   +18 the params
 * CallProc32W is PASCAL: p1 was pushed first, so +18 holds pN and p1 is highest.
 * _CallProcEx32W is CDECL: +18 holds p1. They arrive told apart for us -- 518
 * with 0x4000 in cParams' high word, 517 without -- and krnl386 removes the
 * arguments itself (517 per the PASCAL convention; 518 is cdecl, the caller pops).
 *
 * [CAUTION]: The mask's bit order is taken from the probe run against stock
 * (tests/probes/win16/w_gthunk, 16/16), not from the documentation.
 */
#define WOW_GT_MAX_PARAMETERS       32
#define WOW32_RAW_ARGUMENTS_MAX     0x200   /* The furthest a raw argument read may reach */

/* Defined in wow32.c (#335). */
VOID WowNotePut(PSTR buffer, INT capacity, PINT length, PCSTR text);
VOID WowNoteHex(PSTR buffer, INT capacity, PINT length, DWORD value, INT digits);
DWORD Wow32ArgDword(PCWOW32_FRAME frame, INT offset);
VOID Wow32SetReturn(PWOW32_FRAME frame, DWORD value);
DWORD WowGenericThunkInvoke(DWORD procedure, PCDWORD arguments, INT count);
VOID WowNoteQuoted(PSTR buffer, INT capacity, PINT length, PCSTR text);
WORD Wow32ArgWord(PCWOW32_FRAME frame, INT offset);
volatile BYTE *Wow32ArgPointer(PCWOW32_FRAME frame, INT offset);
WORD Wow32PeekWord(volatile BYTE *bytes);
VOID Wow32PokeWord(volatile BYTE *bytes, WORD value);
volatile BYTE *Wow32FarAt(PCWOW32_FRAME frame, volatile BYTE *base, INT offset);
INT Wow32ArgString(PCWOW32_FRAME frame, INT offset, PSTR output, INT capacity);
extern CHAR g_WowCommandProgram[WOW32_COMMAND_PROGRAM_MAX];
DWORD Wow32Flat(PCWOW32_FRAME frame, DWORD farPointer);
DWORD Wow32PeekReturn(PCWOW32_FRAME frame);
PCSTR Wow32Name(WORD id);
extern CHAR g_WowCommandArguments[WOW32_COMMAND_ARGUMENTS_MAX];
extern CHAR g_WowCommandDirectory[MAX_PATH];
extern INT g_WowCommandIsTaken;
VOID WowShorten(PSTR path, UINT capacity);
INT Wow32MayDecline(WORD id, WORD callSite);
INT Wow32Call(PWOW32_FRAME frame, PWOW32_DOSDATA dosData);
#endif /* NTVDMEX_WOW32_H */
