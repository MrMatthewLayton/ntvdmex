/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * CALLING 16-BIT CODE FROM THE HOST. GH #128, session 40.
 *
 * WHY THIS EXISTS:
 * Everything this host has ever done to a Win16 guest has been in one
 * direction: the guest calls out through a BOP, we answer, it carries on.
 * Session 39 ended on the other direction. SYSEDIT gives up in WinMain because
 * it has no MDI client window, and that window is created INSIDE the frame's
 * own window procedure while it handles WM_CREATE. Our CreateWindow returned a
 * handle without ever calling that procedure, so there was no client and
 * WinMain correctly gave up.
 *
 * The missing thing is not a service. It is a DIRECTION.
 *
 * THE MECHANISM, AND WHY IT IS THIS SMALL:
 * A Win16 window procedure is an ordinary FAR PASCAL function. To call one we
 * need four things, and this host already has three of them:
 *
 * 1. A CONTEXT WE CAN PUT BACK. wowsched.h established that the whole guest
 *    register file is one 0x40-byte block in the VDM TIB, and that saving and
 *    restoring it is sound as long as the frame we park stays where it is.
 *    Here it does, trivially: the parked frame is on the SAME stack, ABOVE
 *    the callback's own frames, and nothing else runs in between.
 * 2. A STACK. The one we are standing on. At a USER BOP the chain is
 *    app -> USER stub -> krnl386's thunk -> BOP, all on the application's own
 *    task stack -- which is exactly the stack real Windows dispatches a window
 *    procedure on.
 * 3. AN ENTRY CONVENTION. The documented Win16 one: a WNDPROC is FAR PASCAL
 *    (hwnd, msg, wParam, lParam), returns with a far RET that cleans its 10
 *    argument bytes, and -- with the standard exported-function prologue --
 *    takes its DS from AX on entry. With the usual `inc bp / push bp` frame:
 *        [bp+0x0e] hwnd    [bp+0x0c] msg    [bp+0x0a] wParam
 *        [bp+0x06] lParam  (low word first, so the DWORD reads normally)
 *    [CAUTION]: sysedit.exe is MULTIPLEDATA (`nedump`), so the loader does NOT fix
 *    that prologue up to load its own DGROUP. The procedure therefore takes
 *    DS from WHOEVER CALLED IT. Enter it with the wrong DS and it runs its
 *    whole body against another module's data. So DS is not a detail, it is
 *    the contract -- we enter with DS = AX = the class's own hInstance,
 *    which in Win16 IS the instance's DGROUP selector.
 * 4. A WAY BACK. The only genuinely new thing: three bytes of guest-visible
 *    memory holding `C4 C4 57` and a 16-bit CODE selector over them. We push
 *    that as the far return address; the procedure's own `retf` lands on it;
 *    the BOP arrives at the host like any other, and we put the context back.
 *    [INFO]: IT IS DISPATCHED BY LINEAR ADDRESS, NOT BY THE CODE BYTE. The code
 *    byte would be a guess about a namespace we do not own (our own INT-site
 *    patcher writes `C4 C4` over `CD nn`, so a third byte can be anything);
 *    the address is exact, is ours by construction, and cannot collide.
 *
 * WHAT THE 16-BIT SIDE SEES:
 * Exactly a call. It is entered with the arguments pushed Pascal order (first
 * declared argument at the highest address), a far return address below them,
 * and its own SS unchanged. It returns with `retf 0x0a`, which pops our return
 * address and discards the ten argument bytes -- so it balances its own stack
 * and we do not have to. We restore SS:SP from the saved context regardless,
 * because trusting a guest's arithmetic about our frame is how a host loses one.
 *
 * RE-ENTRANCY IS THE NORMAL CASE, NOT THE EDGE:
 * The very first thing SYSEDIT's WM_CREATE handler does is call CreateWindow
 * again (the MDI client), which is another USER BOP, which may want another
 * callback. So the saved contexts are a STACK, not a slot. Depth is bounded and
 * exceeding it REFUSES rather than truncating -- a refused callback leaves the
 * guest with a correct "no client window", which it handles; a truncated one
 * leaves it running on somebody else's stack.
 *
 * [CAUTION]: WHAT THIS DOES NOT DO YET, said plainly rather than discovered later:
 * - lParam for WM_CREATE should be a far pointer to a CREATESTRUCT. This host
 *   has never built one and does not know its Win16 layout from measurement,
 *   so it passes 0 and says so on the log line. SYSEDIT's frame procedure
 *   runs correctly without it; an MDI child would not.
 * - There is no message QUEUE, so this is a SendMessage, never a PostMessage.
 * - A callback that faults, or one whose procedure never returns, ends the run
 *   wherever it ends. The parked context is in host memory and is logged, so
 *   the log says which call was in flight.
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#ifndef NTVDMEX_WOWCALL_H
#define NTVDMEX_WOWCALL_H

/* WowCallEnter: whether the 16-bit procedure is known to be absent. */
#define WOWCALL_PROCEDURE_PRESENT   0
#define WOWCALL_PROCEDURE_ABSENT    1

/* The three bytes we plant, and the code that identifies them in a log. The
 * dispatch is on the ADDRESS -- this byte is for the reader.
 */
#define WOWCALL_BOP_CODE            0x57

/* ONE MORE BYTE: A `RETF`, SO THE CPU CAN FAULT FOR US. (session 56) (Importance = 5):
 * A Win16 code segment is LOADED ON DEMAND, and the demand is a fault: krnl386
 * marks the selector not-present (access 0x7b), and the first far transfer to
 * it raises #NP, which its handler services by reading the segment in and
 * committing the descriptor present (access 0xfb). This host reflects that
 * exception correctly and has done for sessions -- eight of them succeeded in
 * the very run that found this.
 *
 * [CAUTION]: BUT WE NEVER LET IT HAPPEN ON OUR OWN CALLS. WowCallEnter reaches a 16-bit
 * procedure by WRITING CS:EIP into the VDM TIB, and a TIB whose CS names a
 * not-present selector is not a fault, it is a VDM that does not come back:
 * no exception, no log line, the process simply gone. Measured on CARDFILE,
 * which registers its class, creates its window, and dies on the first
 * instruction of its own WM_CREATE:
 *
 *     INT31h AX=0x000c BX=0x0b7f sel 0x0b7f <- desc ... acc 0x7b     (NOT present)
 *     WOWCALL: -> 0x0b7f:0x0000 (hwnd=0x0140 msg=0x0001) -- ENTERED, depth 1
 *     <log ends>
 *
 * The contrast is in the same log: 0x04a7 gets the SAME 0x7b descriptor and is
 * later COMMITTED present (`acc=0xfb`), and its WOWCALL returns normally.
 * 0x0b7f never is, because nothing ever faulted on it.
 *
 * So enter on a `RETF` instead, with the target pushed as a far address. The
 * RETF is a normal instruction on a segment that IS present, so the #NP it
 * raises is an ordinary restartable fault: krnl386 loads the segment, IRETs,
 * the RETF re-executes and this time it transfers. The guest's own mechanism,
 * used the way it was designed, instead of stepped around.
 * It lives at offset 4 of the callback paragraph, whose first three bytes are
 * the return BOP above; the selector already covers it.
 */
#define WOWCALL_RETF_OFF            4
#define WOWCALL_RETF_BYTE           0xCB

/* Win16 messages this host sends. Kept here rather than in wowuser.h because
 * the callback is what makes them meaningful.
 */
#define WM_CREATE16                 0x0001

/* [INFO]: Win16 and Win32 agree on the number, as they do for the keyboard messages.
 * Relayed from WowWinProc since session 45; see the long note there for why the
 * OS's update region has to be consumed before it is handed on.
 */
#define WM_PAINT16                  0x000f

/* Eight is not a guess about depth, it is a bound: SYSEDIT nests two
 * (frame WM_CREATE -> MDI client), and a host that recursed deeper than this
 * would be looping rather than working.
 */
#define WOWCALL_MAX_DEPTH           8

/* WHOSE ANSWER IS THE CALL'S ANSWER:
 * Two different things are called "the return value" here and conflating them
 * would be silent:
 * KEEP   the SERVICE already wrote the answer (CreateWindow wrote the handle it
 *      made) and the procedure's return only gets a veto -- WM_CREATE == -1
 *      means "abandon this window", so the hole is revised to 0 and otherwise
 *      left exactly as the service left it.
 * RESULT the PROCEDURE's return IS the answer. SendMessage is defined as
 *      "whatever the window procedure returned", so the host must not invent
 *      one, and the LONG in DX:AX goes into the hole verbatim.
 * RESULTW the same, but the callee returns a WORD in AX and DX IS LITTER.
 *      Measured the moment the first non-wndproc call was made: LocalAlloc
 *      returned `0x00422502`, whose low word is the handle and whose high word
 *      is the `0x42` we had pushed as its FLAGS. The guest read AX and was
 *      unharmed, but the log printed a 32-bit number that was not a value --
 *      an instrument lying about a call the host itself made. A Win16 function
 *      that returns a WORD does not set DX, so the width has to be declared.
 */
#define WOWCALL_RET_KEEP            0
#define WOWCALL_RET_RESULT          1
#define WOWCALL_RET_RESULTW         2

/* What a window procedure answers to WM_CREATE to abandon its window (-1, as a WORD). */
#define WOWCALL_CREATE_REFUSED      0xFFFF

/* WHAT THE HOST DOES ONCE THE ANSWER ARRIVES. (session 42) (Importance = 1):
 * A sink is enough when the result is a value to keep. It is not enough when
 * the result is a POINTER the host then has to follow -- `LocalLock` hands back
 * an offset into the application's own data segment, and reading the text there
 * is work that can only happen after the guest has returned. Naming the action
 * on the frame keeps that decision with the call that asked for it, instead of
 * leaving the BOP handler to guess from a sink's address what it was for.
 */
#define WOWCALL_ACT_NONE            0
#define WOWCALL_ACT_EDITTEXT        1   /* ActionArgument = the Win16 hwnd of an EDIT control */

/* -- THE SAVE DIRECTION, AS A CHAIN. (session 44) EM_GETHANDLE has to hand
 * back a block containing the control's CURRENT text, and only the guest's
 * KERNEL can touch the guest's heap -- so it takes three calls, each one
 * issued by the action of the one before:
 * EDITLOCK: the allocator has returned a handle; LocalLock it.
 * EDITFILL: the lock has returned a near offset; write the text there and
 *           LocalUnlock.
 * Both carry the EDIT control's Win16 hwnd in `ActionArgument`, as EDITTEXT does.
 */
#define WOWCALL_ACT_EDITLOCK        2
#define WOWCALL_ACT_EDITFILL        3

/* AND THE SAME SHAPE AGAIN, AS A LOOP. (session 57) (Importance = 5):
 * A modal dialog is this chain with a continuation rule instead of a fixed
 * next step: when the dialog procedure returns, ask whether EndDialog has been
 * called, and if not, wait for the next message and call it again. The parked
 * frame underneath the whole chain is the DialogBox call itself, which is why
 * `DialogBox` can finally do the one thing that defines it -- not return.
 * `ActionArgument` = the dialog's Win16 hwnd. See src/wow/wowdlg.h.
 */
#define WOWCALL_ACT_MODALPUMP       4

/* AND A FOURTH USE OF THE SAME SHAPE: ONE CALL PER ITEM. (session 57) (Importance = 1):
 * EnumWindows / EnumChildWindows / EnumTaskWindows / LineDDA all call the
 * guest's callback once per item and stop when it answers 0. The continuation
 * rule is "next item"; see src/wow/wowenum.h. `ActionArgument` = the item's window
 * handle where it has one, for the log.
 */
#define WOWCALL_ACT_ENUMNEXT        5

/* -- THE CLIPBOARD BRIDGE (#160). Text crosses in guest GLOBAL memory, which only
 * krnl386 can hand out, so each direction is a short chain of its calls:
 * GetClipboardData: GlobalAlloc -> CLIPLOCK: GlobalLock -> CLIPFILL: copy the
 *                   host text in, GlobalUnlock.        ActionArgument = the handle
 * SetClipboardData: GlobalLock -> CLIPPUT: copy the guest text out to the host
 *                   clipboard, GlobalUnlock.           ActionArgument = the handle
 */
#define WOWCALL_ACT_CLIPLOCK        6
#define WOWCALL_ACT_CLIPFILL        7
#define WOWCALL_ACT_CLIPPUT         8

/* s88: the guest's DLGPROC answered for DefDlgProc; if it said FALSE (0), the
 * dialog manager's DEFAULT runs and its value replaces the answer. The message's
 * parameters wait in g_WowUserDlgDefaults[] at the frame's depth -- ActionArgument is the hdlg.
 */
#define WOWCALL_ACT_DLGDEFAULT      9

/* THE ENUMERATION SOURCES, DECLARED HERE FOR THE INCLUDE ORDER:
 * The walk itself is src/wow/wowenum.h, which is compiled AFTER the two
 * dispatchers that arm one (GDI's LineDDA, USER's EnumWindows family). One
 * enum rather than four flags: a cursor only ever walks ONE of these, and a
 * bitfield would allow a state that means nothing.
 */
#define WOWENUM_NONE                0
#define WOWENUM_WINDOWS             1   /* Every top-level window of this guest */
#define WOWENUM_CHILDREN            2   /* Every child of `parent` */
#define WOWENUM_TASK                3   /* Every window of a task */
#define WOWENUM_LINE                4   /* Every point on a line (LineDDA) */
#define WOWENUM_FONTS               5   /* Every font (family) -- EnumFontFamilies (s89) */
#define WOWENUM_OBJECTS   6     /* every pen or brush -- EnumObjects (s90); the
                                   LOGPEN16/LOGBRUSH16 blobs reuse g_WowEnumFonts[]   */
#define WOWENUM_PROPS     7     /* every property of a window -- EnumProps (s90):
                                   Blob[] = the name (Blob[0]==0: an atom in Blob[1..2]),
                                   .FontType = the data handle                     */
#define WOWENUM_METAFILE  8     /* every record of a metafile -- EnumMetaFile
                                   (#295): the snapshot and the handle table live
                                   in wowgdi.h (g_WowGdiMetafile), not in g_WowEnumFonts[]        */

/* The largest blob WowCallEnter will place on the guest stack. 256 covered every
 * structure before #295 (the font pair is 187 bytes); a METARECORD plus its handle
 * table is the first variable-sized one, and wowgdi.h bounds it by this.
 */
#define WOWCALL_MAX_BLOB    1024

/* -- s89: THE FONTS, COLLECTED UP FRONT AS WIN16 STRUCTURES. EnumFontFamilies
 * asks Win32 for the list in one synchronous call (wowgdi.h) and the walk hands
 * one entry per 16-bit callback. Each entry is the callback's blob verbatim:
 * ENUMLOGFONT16 (146 bytes) then NEWTEXTMETRIC16 (41), byte-packed as Win16's
 * GDI declares them, plus the FontType word.
 */
#define WOWENUM_ELF16       146
#define WOWENUM_NTM16       41
#define WOWENUM_MAXFONT     256
typedef struct _WOWENUM_FONT
{
    BYTE Blob[WOWENUM_ELF16 + WOWENUM_NTM16];
    WORD FontType;
} WOWENUM_FONT, *PWOWENUM_FONT;
typedef const WOWENUM_FONT *PCWOWENUM_FONT;

/* Six words is not a guess about Win16 -- it is what the two things this host
 * calls actually push: a window procedure's 5 (hwnd, msg, wParam, lParam hi+lo)
 * and LocalAlloc's 2. Anything wider gets caught here rather than overrunning.
 */
#define WOWCALL_MAX_ARGW  32  /* s89: EnumFontFamilies' callback takes 7 words; s91: WOWCallback16Ex
                                 allows 64 argument bytes (WCB16_MAX_CBARGS) */

typedef struct _WOWCALL_FRAME
{
    WOWSCHED_SLOT Saved;   /* the interrupted context, verbatim */
    DWORD ReturnLinear;      /* the originating WOW32 frame's return hole, or 0 */
    DWORD Procedure;         /* what we called -- for the log and the failure */
    WORD  Window, Message;   /* for the log; 0/0 when the call is not a message */
    INT   ReturnMode;        /* WOWCALL_RET_* -- see above */
    /* [INFO]: WHERE THE ANSWER ALSO GOES. A call the host makes for its OWN reasons
     * (LocalAlloc, to get an edit control a text handle) has a result the host
     * must keep, and the result only exists when the guest returns -- long
     * after the service that asked for it has finished. One pointer into the
     * static object it belongs to closes that gap without a completion queue.
     *
     * [CAUTION]: It must point into storage that outlives the call. Everything it is used
     * for is a static array, and it must stay that way.
     */
    PWORD Sink;
    DWORD Written;           /* what actually went into the hole, for the log */
    INT   Action;            /* WOWCALL_ACT_* -- see above */
    WORD  ActionArgument;    /* what the action is about */
    WORD  EnteredTask;       /* s92: krnl386's current task when it was entered */
    WORD  PreviousTask;      /* s92: non-zero = an INTER-TASK call; put back on leave */
} WOWCALL_FRAME, *PWOWCALL_FRAME;

/* Defined in wowcall.c (#335). */
extern INT g_WowCallBlob2Argument;
extern INT g_WowCallBlob2Offset;
extern DWORD g_WowCallBlobLinear;
extern WOWENUM_FONT g_WowEnumFonts[WOWENUM_MAXFONT];
extern INT g_WowEnumFontCount;
extern WOWCALL_FRAME g_WowCallFrames[WOWCALL_MAX_DEPTH];
extern INT g_WowCallDepth;
INT WowCallEnter(volatile BYTE *tib, DWORD stackBase, WORD returnSelector, DWORD procedure, WORD dataSelector, PCWORD argumentWords, INT argumentWordCount, DWORD returnLinear, INT returnMode, PWORD sink, WORD window, WORD message, PCBYTE blob, INT blobLength, INT blobArgument, INT isAbsent);
extern WORD (*g_WowCallCurrentTask)(VOID);
extern INT (*g_WowCallRetarget)(WORD window, PWORD stackSelector, PWORD stackPointer, PDWORD stackBase, PWORD previousTask);
extern VOID (*g_WowCallUntarget)(WORD previousTask);
extern DWORD g_WowCallLastResult;
PWOWCALL_FRAME WowCallLeave(volatile BYTE *tib, DWORD result);
#endif /* NTVDMEX_WOWCALL_H */
