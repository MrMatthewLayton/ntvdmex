#ifndef WOWCALL_H
#define WOWCALL_H
/*
 * wowcall.h -- ★ CALLING 16-BIT CODE FROM THE HOST. GH #128, session 40.
 *
 * ── WHY THIS EXISTS ─────────────────────────────────────────────────────────
 * Everything this host has ever done to a Win16 guest has been in one
 * direction: the guest calls out through a BOP, we answer, it carries on.
 * Session 39 ended on the other direction, named to the instruction.
 * `sysedit seg2:0x0114` gives up because `[0x22]` -- its MDI client window --
 * is zero, and `[0x22]` has exactly one writer, at `sysedit seg1:0x01cf`,
 * INSIDE the frame's own window procedure while it handles WM_CREATE. Our
 * CreateWindow returns a handle without ever calling that procedure, so the
 * field stays zero and WinMain correctly gives up.
 *
 * ⇒ The missing thing is not a service. It is a DIRECTION.
 *
 * ── THE MECHANISM, AND WHY IT IS THIS SMALL ─────────────────────────────────
 * A Win16 window procedure is an ordinary FAR PASCAL function. To call one we
 * need four things, and this host already has three of them:
 *
 *   1. A CONTEXT WE CAN PUT BACK. wowsched.h established that the whole guest
 *      register file is one 0x40-byte block in the VDM TIB, and that saving and
 *      restoring it is sound as long as the frame we park stays where it is.
 *      Here it does, trivially: the parked frame is on the SAME stack, ABOVE
 *      the callback's own frames, and nothing else runs in between.
 *   2. A STACK. The one we are standing on. At a USER BOP the chain is
 *      app -> USER stub -> krnl386's thunk -> BOP, all on the application's own
 *      task stack -- which is exactly the stack real Windows dispatches a window
 *      procedure on.
 *   3. AN ENTRY CONVENTION. Read off SYSEDIT's own prologue rather than a
 *      header (`guest/ne/sysedit.exe` seg1:0x0131):
 *          0131  push ds / pop ax / nop        ; ★ DS ON ENTRY MUST BE RIGHT
 *          0134  inc bp / push bp / mov bp,sp
 *          0138  push ds / mov ds,ax
 *          ...
 *          0222  retf 0x0a                    ; ★ FAR, and it cleans 10 bytes
 *      and the body pins the frame without inference:
 *          [bp+0x0e] hwnd    (`mov si,[bp+0xe]`, and si is later pushed as the
 *                             MDI client's PARENT -- it can only be the hwnd)
 *          [bp+0x0c] msg     (`dec ax / je` -> 1 == WM_CREATE)
 *          [bp+0x0a] wParam
 *          [bp+0x06] lParam  (low word first, so the DWORD reads normally)
 *      ⚠ `push ds / pop ax` is the UNPATCHED Win16 export prologue, and
 *        sysedit.exe is MULTIPLEDATA (`nedump`), so the loader does NOT rewrite
 *        it into `mov ax,<DGROUP>`. The procedure therefore takes DS from
 *        WHOEVER CALLED IT. Enter it with the wrong DS and it runs its whole
 *        body against another module's data. So DS is not a detail here, it is
 *        the contract -- we enter with DS = AX = the class's own hInstance,
 *        which in Win16 IS the instance's DGROUP selector.
 *   4. A WAY BACK. The only genuinely new thing: three bytes of guest-visible
 *      memory holding `C4 C4 57` and a 16-bit CODE selector over them. We push
 *      that as the far return address; the procedure's own `retf` lands on it;
 *      the BOP arrives at the host like any other, and we put the context back.
 *      ★ IT IS DISPATCHED BY LINEAR ADDRESS, NOT BY THE CODE BYTE. The code
 *        byte would be a guess about a namespace we do not own (our own INT-site
 *        patcher writes `C4 C4` over `CD nn`, so a third byte can be anything);
 *        the address is exact, is ours by construction, and cannot collide.
 *
 * ── WHAT THE 16-BIT SIDE SEES ───────────────────────────────────────────────
 * Exactly a call. It is entered with the arguments pushed Pascal order (first
 * declared argument at the highest address), a far return address below them,
 * and its own SS unchanged. It returns with `retf 0x0a`, which pops our return
 * address and discards the ten argument bytes -- so it balances its own stack
 * and we do not have to. We restore SS:SP from the saved context regardless,
 * because trusting a guest's arithmetic about our frame is how a host loses one.
 *
 * ── RE-ENTRANCY IS THE NORMAL CASE, NOT THE EDGE ────────────────────────────
 * The very first thing SYSEDIT's WM_CREATE handler does is call CreateWindow
 * again (the MDI client), which is another USER BOP, which may want another
 * callback. So the saved contexts are a STACK, not a slot. Depth is bounded and
 * exceeding it REFUSES rather than truncating -- a refused callback leaves the
 * guest with a correct "no client window", which it handles; a truncated one
 * leaves it running on somebody else's stack.
 *
 * ⚠ WHAT THIS DOES NOT DO YET, said plainly rather than discovered later:
 *   - lParam for WM_CREATE should be a far pointer to a CREATESTRUCT. This host
 *     has never built one and does not know its Win16 layout from measurement,
 *     so it passes 0 and says so on the log line. SYSEDIT's frame procedure
 *     never reads it (seg1:0x018e onward); an MDI child would.
 *   - There is no message QUEUE, so this is a SendMessage, never a PostMessage.
 *   - A callback that faults, or one whose procedure never returns, ends the run
 *     wherever it ends. The parked context is in host memory and is logged, so
 *     the log says which call was in flight.
 */

/* The three bytes we plant, and the code that identifies them in a log. The
   dispatch is on the ADDRESS -- this byte is for the reader. */
#define WOWCALL_BOP_CODE  0x57
#define WOWCALL_BOP_LEN   3

/* Win16 messages this host sends. Kept here rather than in wowuser.h because
   the callback is what makes them meaningful. */
#define WM_CREATE16       0x0001

/* Eight is not a guess about depth, it is a bound: SYSEDIT nests two
   (frame WM_CREATE -> MDI client), and a host that recursed deeper than this
   would be looping rather than working. */
#define WOWCALL_MAX_DEPTH 8

/* ── WHOSE ANSWER IS THE CALL'S ANSWER ────────────────────────────────────────
     Two different things are called "the return value" here and conflating them
     would be silent:
   KEEP   the SERVICE already wrote the answer (CreateWindow wrote the handle it
          made) and the procedure's return only gets a veto -- WM_CREATE == -1
          means "abandon this window", so the hole is revised to 0 and otherwise
          left exactly as the service left it.
   RESULT the PROCEDURE's return IS the answer. SendMessage is defined as
          "whatever the window procedure returned", so the host must not invent
          one, and the LONG in DX:AX goes into the hole verbatim.
   RESULTW the same, but the callee returns a WORD in AX and ★ DX IS LITTER.
          Measured the moment the first non-wndproc call was made: LocalAlloc
          returned `0x00422502`, whose low word is the handle and whose high word
          is the `0x42` we had pushed as its FLAGS. The guest read AX and was
          unharmed, but the log printed a 32-bit number that was not a value --
          an instrument lying about a call the host itself made. A Win16 function
          that returns a WORD does not set DX, so the width has to be declared. */
#define WOWCALL_RET_KEEP    0
#define WOWCALL_RET_RESULT  1
#define WOWCALL_RET_RESULTW 2

/* ── ★ WHAT THE HOST DOES ONCE THE ANSWER ARRIVES. (session 42) ───────────────
     A sink is enough when the result is a value to keep. It is not enough when
     the result is a POINTER the host then has to follow -- `LocalLock` hands back
     an offset into the application's own data segment, and reading the text there
     is work that can only happen after the guest has returned. Naming the action
     on the frame keeps that decision with the call that asked for it, instead of
     leaving the BOP handler to guess from a sink's address what it was for. */
#define WOWCALL_ACT_NONE     0
#define WOWCALL_ACT_EDITTEXT 1   /* actarg = the Win16 hwnd of an EDIT control */
/* ── ★★ THE SAVE DIRECTION, AS A CHAIN. (session 44) EM_GETHANDLE has to hand
     back a block containing the control's CURRENT text, and only the guest's
     KERNEL can touch the guest's heap -- so it takes three calls, each one
     issued by the action of the one before:
       EDITLOCK: the allocator has returned a handle; LocalLock it.
       EDITFILL: the lock has returned a near offset; write the text there and
                 LocalUnlock.
     Both carry the EDIT control's Win16 hwnd in `actarg`, as EDITTEXT does. */
#define WOWCALL_ACT_EDITLOCK 2
#define WOWCALL_ACT_EDITFILL 3

/* Six words is not a guess about Win16 -- it is what the two things this host
   calls actually push: a window procedure's 5 (hwnd, msg, wParam, lParam hi+lo)
   and LocalAlloc's 2. Anything wider gets caught here rather than overrunning. */
#define WOWCALL_MAX_ARGW  6

typedef struct {
    wowsched_slot_t saved;   /* the interrupted context, verbatim               */
    DWORD retlin;            /* the originating WOW32 frame's return hole, or 0 */
    DWORD proc;              /* what we called -- for the log and the failure    */
    WORD  hwnd, msg;         /* for the log; 0/0 when the call is not a message  */
    int   retmode;           /* WOWCALL_RET_* -- see above                       */
    /* ★ WHERE THE ANSWER ALSO GOES. A call the host makes for its OWN reasons
         (LocalAlloc, to get an edit control a text handle) has a result the host
         must keep, and the result only exists when the guest returns -- long
         after the service that asked for it has finished. One pointer into the
         static object it belongs to closes that gap without a completion queue.
       ⚠ It must point into storage that outlives the call. Everything it is used
         for is a static array, and it must stay that way. */
    WORD *sink;
    DWORD written;           /* what actually went into the hole, for the log   */
    int   action;            /* WOWCALL_ACT_* -- see above                      */
    WORD  actarg;            /* what the action is about                        */
} wowcall_frame_t;

static wowcall_frame_t g_wc[WOWCALL_MAX_DEPTH];
static int             g_wc_depth  = 0;
static DWORD           g_wc_calls  = 0;   /* how many 16-bit calls this run made */

/* Push one word onto the guest stack at ssbase:*sp, growing down. */
static void wowcall_push(DWORD ssbase, WORD *sp, WORD v)
{
    volatile BYTE *s;
    *sp = (WORD)(*sp - 2);
    s = (volatile BYTE *)(ULONG_PTR)(ssbase + *sp);
    s[0] = (BYTE)(v & 0xFF);
    s[1] = (BYTE)(v >> 8);
}

/*
 * Enter a 16-bit FAR PASCAL window procedure. The caller must ALREADY have
 * advanced EIP past its own BOP -- what we save here is where the guest goes
 * when the callback returns, and a context saved on the BOP would execute it
 * a second time.
 *
 * `retlin` is the linear address of the originating call's return-value hole,
 * so that a WM_CREATE answering -1 can still fail the CreateWindow that sent
 * it. Pass 0 when there is nothing to revise.
 *
 * Returns 1 if the guest is now standing at the procedure's first instruction.
 */
static int wowcall_enter(volatile BYTE *tib, DWORD ssbase, WORD retsel,
                         DWORD proc, WORD ds, const WORD *argw, int nargw,
                         DWORD retlin, int retmode, WORD *sink,
                         WORD hwnd, WORD msg)
{
    wowcall_frame_t *fr;
    WORD sp;
    int i;
    if (g_wc_depth >= WOWCALL_MAX_DEPTH) return 0;
    if (!ssbase || !retsel || !(proc >> 16)) return 0;
    if (nargw < 0 || nargw > WOWCALL_MAX_ARGW) return 0;

    fr = &g_wc[g_wc_depth++];
    wowsched_save(&fr->saved, tib, 0, 0, 0);
    fr->retlin  = retlin;
    fr->proc    = proc;
    fr->hwnd    = hwnd;
    fr->msg     = msg;
    fr->retmode = retmode;
    fr->sink    = sink;
    fr->action  = WOWCALL_ACT_NONE;   /* the caller sets it after we succeed */
    fr->actarg  = 0;

    /* Pascal order: the FIRST declared argument is pushed FIRST, so it ends up
       at the highest address -- which is what `[bp+0x0e] == hwnd` in a window
       procedure and `[bp+8] == uFlags` in krnl386's own LocalAlloc both say it
       must be. A DWORD is two words, high first, for the same reason. The caller
       hands them in declared order and this pushes them in that order. */
    sp = (WORD)(VDM_REG(tib, VTIB_ESP) & 0xFFFF);
    for (i = 0; i < nargw; ++i) wowcall_push(ssbase, &sp, argw[i]);
    wowcall_push(ssbase, &sp, retsel);       /* the far return address: CS ... */
    wowcall_push(ssbase, &sp, 0);            /* ... then IP, at offset 0       */
    VDM_SET16(tib, VTIB_ESP, sp);

    /* DS is the contract (see the header note); AX carries the same value so
       that a MakeProcInstance-style `mov ds,ax` prologue is satisfied too. One
       assignment cannot be right for one form and wrong for the other, because
       both forms read the same register. */
    VDM_SET16(tib, VTIB_EAX, ds);
    VDM_SET16(tib, VTIB_DS,  ds);
    VDM_SET16(tib, VTIB_CS,  (WORD)(proc >> 16));
    VDM_REG(tib, VTIB_EIP) = (DWORD)(proc & 0xFFFF);
    ++g_wc_calls;
    return 1;
}

/*
 * The procedure returned. `result` is DX:AX, read by the caller before this.
 * Puts the interrupted context back and hands the result to whoever asked.
 * Returns the frame that was in flight, or NULL if there was none -- and "none"
 * is not a curiosity, it means something executed our return stub that we did
 * not send there, which is a fact worth printing rather than swallowing.
 */
static wowcall_frame_t *wowcall_leave(volatile BYTE *tib, DWORD result)
{
    wowcall_frame_t *fr;
    if (g_wc_depth <= 0) return NULL;
    fr = &g_wc[--g_wc_depth];
    wowsched_restore(&fr->saved, tib);
    if (fr->sink) *fr->sink = (WORD)result;
    fr->written = 0;
    if (fr->retlin) {
        volatile BYTE *h = (volatile BYTE *)(ULONG_PTR)fr->retlin;
        DWORD v = 0;
        int write = 0;
        /* ★ WM_CREATE MAY REFUSE. Returning -1 from WM_CREATE is the documented
             way for a window procedure to abort its own creation, and the host
             must honour it: the call that made the window comes back 0. The
             return hole is guest memory and outlives the context switch, so
             revising it is a four-byte write, not a special case. */
        if (fr->retmode == WOWCALL_RET_KEEP) {
            if (fr->msg == WM_CREATE16 && (WORD)result == 0xFFFF) write = 1;
        } else {
            /* ⚠ MASK A WORD RETURN. DX is not the high half of a WORD result --
                 see WOWCALL_RET_RESULTW above, and the LocalAlloc call that
                 proved it. */
            v = (fr->retmode == WOWCALL_RET_RESULTW) ? (result & 0xFFFF) : result;
            write = 1;                  /* SendMessage: the procedure's answer */
        }
        if (write) {
            fr->written = v;
            h[0] = (BYTE)(v & 0xFF);        h[1] = (BYTE)((v >> 8)  & 0xFF);
            h[2] = (BYTE)((v >> 16) & 0xFF); h[3] = (BYTE)((v >> 24) & 0xFF);
        }
    }
    return fr;
}

#endif /* WOWCALL_H */
